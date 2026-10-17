/* remodroid-server entry point.
 *
 *   remodroid-server [-c /etc/remodroid/server.ini]
 *   remodroid-server hashpw [password]     print a bcrypt hash for server.ini
 *   remodroid-server gensecret             print a random device secret */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "server.h"

server_t S;

static void count_key(const char *k, const char *v, void *ud) { ++*(int *)ud; }

static void set_str(char **dst, const char *v)
{
    free(*dst);
    *dst = rd_strdup(v);
}

static int load_config(void)
{
    int line = 0;
    rd_conf_t *c = rd_conf_load(S.conf_path, &line);
    if (!c) {
        if (line > 0)
            LOGE("%s: syntax error on line %d", S.conf_path, line);
        else
            LOGE("%s: cannot read the file", S.conf_path);
        return -1;
    }
    rd_conf_free(S.conf);
    S.conf = c;
    rd_log_level = rd_log_parse_level(rd_conf_get(c, "log", "level", NULL), rd_log_level);
    int users = 0;
    rd_conf_each(c, "users", count_key, &users);
    S.setup = users == 0; /* fresh install: only the setup wizard until an admin exists */
    snprintf(S.state_dir, sizeof S.state_dir, "%s", rd_conf_get(c, "server", "state_dir", "/var/lib/remodroid"));
    S.session_days = (int)rd_conf_int(c, "auth", "session_days", 30);
    S.max_failed = (int)rd_conf_int(c, "auth", "max_failed_logins", 5);
    /* lockout_seconds wins when set; otherwise fall back to lockout_minutes (default 15 min). */
    S.lockout_sec = (int)rd_conf_int(c, "auth", "lockout_seconds",
                                     (int)rd_conf_int(c, "auth", "lockout_minutes", 15) * 60);
    snprintf(S.cookie_name, sizeof S.cookie_name, "%s", rd_conf_get(c, "auth", "cookie_name", "rd_session"));
    S.ping_s = (int)rd_conf_int(c, "keepalive", "ping_seconds", 20);
    S.timeout_s = (int)rd_conf_int(c, "keepalive", "timeout_seconds", 60);
    S.heartbeat_s = (int)rd_conf_int(c, "keepalive", "heartbeat_seconds", 30);
    set_str(&S.web_root, rd_conf_get(c, "http", "web_root", "/usr/share/remodroid/web"));
    set_str(&S.origins, rd_conf_get(c, "http", "allowed_origins", ""));
    set_str(&S.turn_urls, rd_conf_get(c, "turn", "urls", ""));
    set_str(&S.turn_secret, rd_conf_get(c, "turn", "shared_secret", ""));
    S.turn_ttl_min = (int)rd_conf_int(c, "turn", "credential_ttl_minutes", 60);
    /* [tls]: HTTPS turns on with a listen address, or by default when [acme] domains is set */
    int acme = *rd_conf_get(c, "acme", "domains", "") != 0;
    set_str(&S.tls_listen, rd_conf_get(c, "tls", "listen", acme ? "0.0.0.0:443" : ""));
    set_str(&S.tls_cert, rd_conf_get(c, "tls", "cert", ""));
    set_str(&S.tls_key, rd_conf_get(c, "tls", "key", ""));
    const char *plain = rd_conf_get(c, "tls", "plain_http", "redirect");
    if (strcmp(plain, "redirect") && strcmp(plain, "allow")) {
        LOGE("[tls] plain_http must be redirect or allow (got %s)", plain);
        return -1;
    }
    S.tls_redirect = !strcmp(plain, "redirect");
    if (*S.tls_listen && !acme && (!*S.tls_cert || !*S.tls_key)) {
        LOGE("[tls] listen is set: also set [acme] domains, or [tls] cert and key");
        return -1;
    }
    if (acme && *S.tls_cert)
        LOGW("[tls] cert is ignored because [acme] domains is set");
    if (S.session_days < 1)
        S.session_days = 1;
    if (S.ping_s < 1)
        S.ping_s = 20;
    return 0;
}

int server_conf_set(const char *sec, const char *key, const char *val)
{
    if (rd_conf_write(S.conf_path, sec, key, val) < 0) {
        LOGE("cannot write %s", S.conf_path);
        return -1;
    }
    load_config(); /* S.conf follows the file; a half-written change may not validate yet */
    return 0;
}

/* After the wizard: start over with the new listen addresses. All sockets are
 * close-on-exec; the short delay lets the reply reach the browser. */
static ev_timer restart_timer;

static void on_restart(struct ev_loop *loop, ev_timer *w, int revents)
{
    LOGI("restarting with the new settings");
    char path[400];
    snprintf(path, sizeof path, "%s/sessions", S.state_dir);
    if (rd_mkdirs(S.state_dir, 0700) == 0)
        auth_save_sessions(path);
    execv("/proc/self/exe", S.argv);
    LOGE("re-exec failed (%s); exiting so the service manager restarts us", strerror(errno));
    exit(0);
}

void server_restart_soon(void)
{
    ev_timer_init(&restart_timer, on_restart, 0.8, 0.);
    ev_timer_start(S.loop, &restart_timer);
}

static void on_ping(struct ev_loop *loop, ev_timer *w, int revents) { server_tick_ping(); }
static void on_sweep(struct ev_loop *loop, ev_timer *w, int revents) { auth_sweep(); }

static void on_hup(struct ev_loop *loop, ev_signal *w, int revents)
{
    LOGI("reloading %s", S.conf_path);
    if (load_config() == 0) {
        ev_timer_set(&S.ping_timer, S.ping_s, S.ping_s);
        ev_timer_again(loop, &S.ping_timer);
        if (*rd_conf_get(S.conf, "acme", "domains", ""))
            acme_reload();
        else if (*S.tls_cert && S.nlisteners > 1)
            tls_load(S.tls_cert, S.tls_key); /* e.g. renewed by certbot */
        LOGI("reloaded (session_days=%d; device list and listen address changes need a restart)", S.session_days);
    }
}

static void on_term(struct ev_loop *loop, ev_signal *w, int revents)
{
    LOGI("shutting down");
    ev_break(loop, EVBREAK_ALL);
}

static int read_password(char *buf, size_t cap)
{
    struct termios t, old;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    if (tty) {
        fprintf(stderr, "Password: ");
        t = old;
        t.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(0, TCSANOW, &t);
    }
    char *r = fgets(buf, (int)cap, stdin);
    if (tty) {
        tcsetattr(0, TCSANOW, &old);
        fprintf(stderr, "\n");
    }
    if (!r)
        return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: remodroid-server [-c server.ini] [-v] [-t]     -t: check the configuration and exit\n"
            "       remodroid-server hashpw [password]   bcrypt hash for [users]/[devices]\n"
            "       remodroid-server gensecret           random device secret\n");
}

int main(int argc, char **argv)
{
    S.conf_path = "/etc/remodroid/server.ini";
    S.argv = argv;
    rd_log_init("server", RD_LOG_INFO);
    if (argc > 1 && !strcmp(argv[1], "hashpw")) {
        char pw[256];
        if (argc > 2)
            snprintf(pw, sizeof pw, "%s", argv[2]);
        else if (read_password(pw, sizeof pw) < 0)
            return 1;
        char *h = auth_hash_password(pw);
        if (!h)
            return 1;
        printf("%s\n", h);
        free(h);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "gensecret")) {
        uint8_t r[18];
        char out[32];
        rd_random(r, sizeof r);
        rd_b64_encode(r, sizeof r, out, sizeof out);
        for (char *p = out; *p; p++)
            if (*p == '+') *p = '-'; else if (*p == '/') *p = '_';
        printf("%s\n", out);
        return 0;
    }
    int opt, test_only = 0;
    while ((opt = getopt(argc, argv, "c:tvh")) != -1) {
        switch (opt) {
        case 'c': S.conf_path = optarg; break;
        case 't': test_only = 1; break;
        case 'v': rd_log_level++; break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (load_config() < 0)
        return 1;
    if (test_only) { /* like nginx -t: no sockets, no ACME */
        if (*S.tls_cert && !*rd_conf_get(S.conf, "acme", "domains", "") && tls_load(S.tls_cert, S.tls_key) < 0)
            return 1;
        printf("%s: configuration OK\n", S.conf_path);
        return 0;
    }
    hub_init_devices();
    {
        char path[400];
        snprintf(path, sizeof path, "%s/sessions", S.state_dir);
        auth_load_sessions(path);
    }
    if (S.ndevices == 0 && !S.setup)
        LOGW("no routers yet: add one on the panel's Agents tab (/panel.html#agents)");
    S.loop = EV_DEFAULT;
    if (server_listen(rd_conf_get(S.conf, "http", "listen", "0.0.0.0:80"), 0) < 0)
        return 1;
    if (*S.tls_listen) {
        if (server_listen(S.tls_listen, 1) < 0)
            return 1;
        if (*rd_conf_get(S.conf, "acme", "domains", "")) {
            if (acme_start() < 0)
                return 1;
        } else if (tls_load(S.tls_cert, S.tls_key) < 0) {
            return 1;
        }
    }
    if (turn_start() < 0)
        return 1;
    if (S.setup)
        setup_init();
    ev_timer_init(&S.ping_timer, on_ping, S.ping_s, S.ping_s);
    ev_timer_start(S.loop, &S.ping_timer);
    ev_timer_init(&S.sweep_timer, on_sweep, 600., 600.);
    ev_timer_start(S.loop, &S.sweep_timer);
    ev_signal_init(&S.sig_hup, on_hup, SIGHUP);
    ev_signal_start(S.loop, &S.sig_hup);
    ev_signal_init(&S.sig_term, on_term, SIGTERM);
    ev_signal_start(S.loop, &S.sig_term);
    ev_signal_init(&S.sig_int, on_term, SIGINT);
    ev_signal_start(S.loop, &S.sig_int);
    LOGI("remodroid-server %s: %d device(s), web root %s, sessions last %d days", RD_VERSION, S.ndevices, S.web_root,
         S.session_days);
    ev_run(S.loop, 0);
    return 0;
}
