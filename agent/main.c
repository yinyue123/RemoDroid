/* remodroid-agent entry point and shared agent state.
 *
 *   remodroid-agent [-c /etc/remodroid/agent.ini] [-v]     run the agent
 *   remodroid-agent devices|status|shell|push|pull|logcat|key|restart-server|reconnect|reload
 *                                                         talk to the running agent */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "msg.h"

agent_t A;

#define LOG_RING 500
static char *log_ring[LOG_RING];
static int log_head, log_n;

static void log_hook(const char *line)
{
    if (log_n == LOG_RING) {
        free(log_ring[log_head]);
        log_ring[log_head] = rd_strdup(line);
        log_head = (log_head + 1) % LOG_RING;
    } else {
        log_ring[(log_head + log_n++) % LOG_RING] = rd_strdup(line);
    }
}

cJSON *agent_log_tail(int lines)
{
    cJSON *arr = cJSON_CreateArray();
    int n = RD_MIN(lines, log_n);
    for (int i = log_n - n; i < log_n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(log_ring[(log_head + i) % LOG_RING]));
    return arr;
}

/* ---------------- configuration ---------------- */

const char *agent_conf(const char *sec, const char *key, const char *def) { return rd_conf_get(A.conf, sec, key, def); }
long agent_conf_int(const char *sec, const char *key, long def) { return rd_conf_int(A.conf, sec, key, def); }
int agent_conf_bool(const char *sec, const char *key, int def) { return rd_conf_bool(A.conf, sec, key, def); }

int agent_reload(void)
{
    int line = 0;
    rd_conf_t *c = rd_conf_load(A.conf_path, &line);
    if (!c) {
        if (line > 0)
            LOGE("%s: syntax error on line %d", A.conf_path, line);
        else
            LOGE("%s: cannot read the file", A.conf_path);
        return -1;
    }
    rd_conf_free(A.conf);
    A.conf = c;
    rd_log_level = rd_log_parse_level(agent_conf("agent", "log_level", NULL), rd_log_level);
    snprintf(A.url, sizeof A.url, "%s", agent_conf("server", "url", ""));
    snprintf(A.device_id, sizeof A.device_id, "%s", agent_conf("server", "device_id", ""));
    snprintf(A.device_secret, sizeof A.device_secret, "%s", agent_conf("server", "device_secret", ""));
    snprintf(A.ca_file, sizeof A.ca_file, "%s", agent_conf("server", "ca_file", ""));
    A.ping_s = (int)agent_conf_int("keepalive", "ping_seconds", 20);
    A.timeout_s = (int)agent_conf_int("keepalive", "timeout_seconds", 60);
    A.heartbeat_s = (int)agent_conf_int("keepalive", "heartbeat_seconds", 30);
    A.reconnect_max_s = (int)agent_conf_int("keepalive", "reconnect_max_seconds", 30);
    if (A.ping_s < 1) A.ping_s = 20;
    if (A.heartbeat_s < 1) A.heartbeat_s = 30;
    if (A.reconnect_max_s < 1) A.reconnect_max_s = 30;
    snprintf(A.files_dir, sizeof A.files_dir, "%s", agent_conf("agent", "files_dir", "/usr/share/remodroid"));
    snprintf(A.phone_dir, sizeof A.phone_dir, "%s", agent_conf("agent", "phone_dir", "/data/local/tmp/remodroid"));
    snprintf(A.sock_path, sizeof A.sock_path, "%s", agent_conf("agent", "socket", "/var/run/remodroid.sock"));
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* Line-based edit that keeps comments, order and inline comments. */
int agent_conf_set(const char *sec, const char *key, const char *val)
{
    FILE *f = fopen(A.conf_path, "r");
    if (!f)
        return -1;
    rd_buf_t out;
    rd_buf_init(&out);
    char line[1024], cur[64] = "";
    int done = 0, in_sec = 0, sec_seen = 0;
    long insert_at = -1; /* offset in out after the last line of the section */
    while (fgets(line, sizeof line, f)) {
        char copy[1024];
        snprintf(copy, sizeof copy, "%s", line);
        char *t = trim(copy);
        if (*t == '[') {
            char *e = strchr(t, ']');
            if (e) {
                *e = 0;
                snprintf(cur, sizeof cur, "%s", t + 1);
                in_sec = !strcmp(cur, sec);
                sec_seen |= in_sec;
            }
        } else if (in_sec && !done && *t && *t != ';' && *t != '#') {
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = 0;
                if (!strcmp(trim(t), key)) {
                    /* keep an inline comment (" ; ..."), if any */
                    const char *cm = strstr(line, " ;");
                    if (cm) {
                        char c2[1024];
                        snprintf(c2, sizeof c2, "%s", cm);
                        rd_buf_printf(&out, "%s = %s   %s\n", key, val, trim(c2));
                    } else {
                        rd_buf_printf(&out, "%s = %s\n", key, val);
                    }
                    done = 1;
                    continue;
                }
            }
        }
        rd_buf_printf(&out, "%s", line);
        if (in_sec && *t)
            insert_at = (long)out.len;
    }
    fclose(f);
    if (!done) {
        char add[512];
        int n = snprintf(add, sizeof add, "%s = %s\n", key, val);
        if (sec_seen && insert_at >= 0) {
            rd_buf_reserve(&out, (size_t)n);
            memmove(out.p + insert_at + n, out.p + insert_at, out.len - (size_t)insert_at);
            memcpy(out.p + insert_at, add, (size_t)n);
            out.len += (size_t)n;
        } else {
            if (out.len && out.p[out.len - 1] != '\n')
                rd_buf_printf(&out, "\n");
            rd_buf_printf(&out, "\n[%s]\n%s", sec, add);
        }
    }
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", A.conf_path);
    FILE *w = fopen(tmp, "w");
    int ok = w && fwrite(out.p, 1, out.len, w) == out.len;
    if (w && fclose(w) != 0)
        ok = 0;
    rd_buf_free(&out);
    if (!ok || rename(tmp, A.conf_path) < 0) {
        unlink(tmp);
        return -1;
    }
    LOGI("agent.ini: [%s] %s = %s", sec, key, val);
    return 0;
}

/* ---------------- status and events ---------------- */

cJSON *agent_adb_json(void)
{
    cJSON *j = cJSON_CreateObject();
    const adb_transport_t *tr = adb_transport(A.adb);
    cJSON_AddStringToObject(j, "usb", tr ? "connected" : "none");
    cJSON_AddStringToObject(j, "auth", adb_state_name(adb_state(A.adb)));
    if (tr) {
        uint64_t tx, rx;
        int streams;
        adb_stats(A.adb, &tx, &rx, &streams);
        char feat[512];
        cJSON_AddStringToObject(j, "transport", tr->kind);
        cJSON_AddStringToObject(j, "serial", adb_serial(A.adb));
        cJSON_AddStringToObject(j, "usb_speed", tr->speed);
        cJSON_AddStringToObject(j, "protocol_version", "0x01000001");
        cJSON_AddNumberToObject(j, "max_payload", adb_max_payload(A.adb));
        cJSON *fa = cJSON_AddArrayToObject(j, "features");
        if (adb_banner_prop(A.adb, "features", feat, sizeof feat)) {
            char *save = NULL;
            for (char *f = strtok_r(feat, ",", &save); f; f = strtok_r(NULL, ",", &save))
                cJSON_AddItemToArray(fa, cJSON_CreateString(f));
        }
        cJSON_AddNumberToObject(j, "streams_open", streams);
        cJSON_AddNumberToObject(j, "tx_bytes", (double)tx);
        cJSON_AddNumberToObject(j, "rx_bytes", (double)rx);
    }
    return j;
}

cJSON *agent_status_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "phone", phone_status_json());
    cJSON_AddItemToObject(j, "adb", agent_adb_json());
    return j;
}

void agent_event(const char *kind, cJSON *info)
{
    LOGI("event: %s", kind);
    cJSON *m = msg_new("event", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddStringToObject(d, "kind", kind);
    cJSON_AddNumberToObject(d, "ts", (double)rd_now_ms());
    cJSON_AddItemToObject(d, "info", info ? info : cJSON_CreateObject());
    wsc_send(m);
}

static void on_status_debounce(struct ev_loop *loop, ev_timer *w, int revents)
{
    cJSON *m = msg_new("status", 0, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", agent_status_json());
    wsc_send(m);
}

void agent_status_changed(void)
{
    if (!ev_is_active(&A.status_debounce)) {
        ev_timer_set(&A.status_debounce, 0.2, 0.);
        ev_timer_start(A.loop, &A.status_debounce);
    }
}

/* ---------------- main ---------------- */

static void on_adb_state(adb_t *a, int state, void *ud) { phone_adb_state(state); }

static void on_hup(struct ev_loop *loop, ev_signal *w, int revents)
{
    char old_url[256], old_id[64], old_secret[128];
    snprintf(old_url, sizeof old_url, "%s", A.url);
    snprintf(old_id, sizeof old_id, "%s", A.device_id);
    snprintf(old_secret, sizeof old_secret, "%s", A.device_secret);
    if (agent_reload() < 0)
        return;
    LOGI("reloaded %s", A.conf_path);
    if (strcmp(old_url, A.url) || strcmp(old_id, A.device_id) || strcmp(old_secret, A.device_secret))
        wsc_restart();
    phone_config_changed();
}

static void on_term(struct ev_loop *loop, ev_signal *w, int revents)
{
    LOGI("shutting down");
    unlink(A.sock_path);
    ev_break(loop, EVBREAK_ALL);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: remodroid-agent [-c agent.ini] [-v]\n"
            "       remodroid-agent [-c agent.ini] devices | status | shell \"cmd\" | push LOCAL REMOTE |\n"
            "                       pull REMOTE LOCAL | logcat [args] | key CODE | restart-server |\n"
            "                       reconnect | reload\n");
}

int main(int argc, char **argv)
{
    A.conf_path = "/etc/remodroid/agent.ini";
    rd_log_init("agent", RD_LOG_INFO);
    int opt;
    while ((opt = getopt(argc, argv, "+c:vh")) != -1) {
        switch (opt) {
        case 'c': A.conf_path = optarg; break;
        case 'v': rd_log_level++; break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }
    if (agent_reload() < 0)
        return 1;
    if (optind < argc)
        return cli_client_main(argc - optind, argv + optind);

    signal(SIGPIPE, SIG_IGN);
    rd_log_set_hook(log_hook);
    A.loop = EV_DEFAULT;
    ev_timer_init(&A.status_debounce, on_status_debounce, 0.2, 0.);
    LOGI("remodroid-agent %s starting (device %s)", RD_VERSION, A.device_id);
    A.adb = adb_new(A.loop, agent_conf("agent", "adbkey", "/etc/remodroid/adbkey"),
                    (adb_callbacks_t){ on_adb_state }, NULL);
    phone_init();
    const char *adbt = agent_conf("phone", "adb", "usb");
    const char *link = agent_conf("phone", "link", "adb");
    if (strncmp(link, "tcp:", 4)) /* ADB supervises the phone unless the tunnel is a plain TCP link */
        A.tm = transport_start(A.loop, A.adb, agent_conf("phone", "serial", ""),
                               !strncmp(adbt, "tcp:", 4) ? adbt + 4 : NULL);
    if (cli_server_start() < 0)
        LOGW("CLI socket %s unavailable: %s", A.sock_path, strerror(errno));
    if (A.url[0])
        wsc_start();
    else
        LOGW("[server] url is empty; running without a server connection");
    ev_signal_init(&A.sig_hup, on_hup, SIGHUP);
    ev_signal_start(A.loop, &A.sig_hup);
    ev_signal_init(&A.sig_term, on_term, SIGTERM);
    ev_signal_start(A.loop, &A.sig_term);
    ev_signal_init(&A.sig_int, on_term, SIGINT);
    ev_signal_start(A.loop, &A.sig_int);
    ev_run(A.loop, 0);
    return 0;
}
