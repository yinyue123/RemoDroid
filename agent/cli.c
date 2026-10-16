/* Local control socket (plan Appendix A.10): one JSON request per line in,
 * {"t":"out"} lines and a final ack/error out. Works without the server. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "agent.h"
#include "msg.h"

typedef struct client {
    int fd, closed, pending;
    ev_io rio, wio;
    rd_buf_t in, out;
    adb_stream_t *stream; /* shell / logcat in progress */
    struct client *next;
} client_t;

static int listen_fd = -1;
static ev_io listen_io;

/* ---------------- server side ---------------- */

static void client_free_if_done(client_t *c)
{
    if (c->closed && !c->pending) {
        rd_buf_free(&c->in);
        rd_buf_free(&c->out);
        free(c);
    }
}

static void client_close(client_t *c)
{
    if (c->closed)
        return;
    c->closed = 1;
    ev_io_stop(A.loop, &c->rio);
    ev_io_stop(A.loop, &c->wio);
    close(c->fd);
    if (c->stream) { /* cancel a running shell/logcat */
        adb_close(c->stream);
        c->stream = NULL;
        c->pending--;
    }
    client_free_if_done(c);
}

static void client_flush(client_t *c)
{
    if (c->closed)
        return;
    while (rd_buf_avail(&c->out)) {
        ssize_t n = write(c->fd, rd_buf_data(&c->out), rd_buf_avail(&c->out));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            client_close(c);
            return;
        }
        rd_buf_consume(&c->out, (size_t)n);
    }
    if (rd_buf_avail(&c->out))
        ev_io_start(A.loop, &c->wio);
    else
        ev_io_stop(A.loop, &c->wio);
}

static void reply(client_t *c, cJSON *m)
{
    char *s = msg_print(m);
    cJSON_Delete(m);
    if (!c->closed) {
        rd_buf_append(&c->out, s, strlen(s));
        rd_buf_append(&c->out, "\n", 1);
        client_flush(c);
    }
    free(s);
}

static void reply_error(client_t *c, const char *code, const char *text) { reply(c, msg_error(0, code, text)); }

static void shell_out(const uint8_t *d, size_t n, void *ud)
{
    client_t *c = ud;
    cJSON *m = msg_new("out", 0, NULL);
    char *s = malloc(n + 1);
    memcpy(s, d, n);
    s[n] = 0;
    cJSON_AddStringToObject(msg_d(m), "data", s);
    free(s);
    reply(c, m);
}

static void shell_done(int code, const char *out, size_t n, void *ud)
{
    client_t *c = ud;
    c->stream = NULL;
    c->pending--;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "exit_code", code);
    reply(c, msg_ack(0, d));
    client_free_if_done(c);
}

static void file_done(int code, const char *msg, void *ud)
{
    client_t *c = ud;
    c->pending--;
    if (code < 0) {
        reply_error(c, "io_error", msg);
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "result", msg);
        reply(c, msg_ack(0, d));
    }
    client_free_if_done(c);
}

static int need_phone(client_t *c)
{
    if (adb_state(A.adb) == ADB_ONLINE)
        return 1;
    reply_error(c, adb_state(A.adb) == ADB_UNAUTHORIZED ? "adb_unauthorized" : "offline",
                adb_state(A.adb) == ADB_UNAUTHORIZED ? "accept \"Allow USB debugging\" on the phone"
                                                     : "no phone connected over ADB");
    return 0;
}

static void start_shell(client_t *c, const char *cmd, int stream_output)
{
    c->stream = adb_shell(A.adb, cmd, stream_output ? shell_out : NULL, shell_done, c);
    if (!c->stream) {
        reply_error(c, "io_error", "cannot open a shell");
        return;
    }
    c->pending++;
}

static void handle_request(client_t *c, cJSON *m)
{
    const char *t = msg_type(m);
    cJSON *d = msg_d(m);
    char err[200] = "";
    if (!strcmp(t, "devices")) {
        cJSON *r = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(r, "devices");
        if (adb_transport(A.adb))
            cJSON_AddItemToArray(arr, agent_adb_json());
        reply(c, msg_ack(0, r));
    } else if (!strcmp(t, "status")) {
        cJSON *r = agent_status_json();
        cJSON_AddBoolToObject(r, "server_connected", wsc_online());
        cJSON_AddStringToObject(r, "server_url", A.url);
        cJSON_AddStringToObject(r, "version", RD_VERSION);
        reply(c, msg_ack(0, r));
    } else if (!strcmp(t, "shell")) {
        if (need_phone(c))
            start_shell(c, msg_str(d, "cmd", "true"), 1);
    } else if (!strcmp(t, "logcat")) {
        if (need_phone(c)) {
            char cmd[600];
            snprintf(cmd, sizeof cmd, "logcat -v threadtime %s", msg_str(d, "args", ""));
            start_shell(c, cmd, 1);
        }
    } else if (!strcmp(t, "key")) {
        if (need_phone(c)) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "input keyevent %d", (int)msg_num(d, "code", 0));
            start_shell(c, cmd, 1);
        }
    } else if (!strcmp(t, "push") || !strcmp(t, "pull")) {
        if (!need_phone(c))
            return;
        int push = !strcmp(t, "push");
        const char *l = msg_str(d, "local", ""), *r = msg_str(d, "remote", "");
        int rc = push ? adb_push(A.adb, l, r, 0100644, file_done, c) : adb_pull(A.adb, r, l, file_done, c);
        if (rc < 0)
            reply_error(c, "io_error", push ? "cannot read the local file" : "cannot write the local file");
        else
            c->pending++;
    } else if (!strcmp(t, "restart-server") || !strcmp(t, "reconnect") || !strcmp(t, "reload")) {
        const char *cmd = !strcmp(t, "restart-server") ? "server.restart" : !strcmp(t, "reconnect") ? "adb.reconnect"
                                                                                                    : "agent.reload";
        if (phone_run_cmd(cmd, NULL, err, sizeof err) < 0)
            reply_error(c, "io_error", err[0] ? err : cmd);
        else
            reply(c, msg_ack(0, NULL));
    } else {
        reply_error(c, "unsupported", t);
    }
}

static void client_read(struct ev_loop *loop, ev_io *w, int revents)
{
    client_t *c = w->data;
    rd_buf_reserve(&c->in, 4096);
    ssize_t n = read(c->fd, c->in.p + c->in.len, c->in.cap - c->in.len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        client_close(c);
        return;
    }
    c->in.len += (size_t)n;
    for (;;) {
        uint8_t *p = rd_buf_data(&c->in);
        uint8_t *nl = memchr(p, '\n', rd_buf_avail(&c->in));
        if (!nl) {
            if (rd_buf_avail(&c->in) > 65536)
                client_close(c);
            return;
        }
        cJSON *m = cJSON_ParseWithLength((const char *)p, (size_t)(nl - p));
        rd_buf_consume(&c->in, (size_t)(nl - p) + 1);
        if (cJSON_IsObject(m))
            handle_request(c, m);
        else
            reply_error(c, "bad_request", "expected one JSON object per line");
        cJSON_Delete(m);
        if (c->closed)
            return;
    }
}

static void client_write(struct ev_loop *loop, ev_io *w, int revents) { client_flush(w->data); }

static void on_accept(struct ev_loop *loop, ev_io *w, int revents)
{
    int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    client_t *c = calloc(1, sizeof *c);
    c->fd = fd;
    ev_io_init(&c->rio, client_read, fd, EV_READ);
    ev_io_init(&c->wio, client_write, fd, EV_WRITE);
    c->rio.data = c->wio.data = c;
    ev_io_start(loop, &c->rio);
}

int cli_server_start(void)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(A.sock_path) >= sizeof sa.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(sa.sun_path, A.sock_path);
    unlink(A.sock_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(listen_fd, 8) < 0)
        return -1;
    ev_io_init(&listen_io, on_accept, listen_fd, EV_READ);
    ev_io_start(A.loop, &listen_io);
    return 0;
}

/* ---------------- client side (the CLI) ---------------- */

static int cli_fd = -1;

static void on_sigint(int sig)
{
    /* Closing the socket makes the agent stop a running logcat/shell. */
    if (cli_fd >= 0)
        shutdown(cli_fd, SHUT_RDWR);
}

int cli_client_main(int argc, char **argv)
{
    const char *cmd = argv[0];
    cJSON *m = msg_new(cmd, 0, NULL);
    cJSON *d = msg_d(m);
    char path[PATH_MAX];
    if (!strcmp(cmd, "shell")) {
        if (argc < 2) goto usage;
        rd_buf_t b;
        rd_buf_init(&b);
        for (int i = 1; i < argc; i++)
            rd_buf_printf(&b, "%s%s", i > 1 ? " " : "", argv[i]);
        rd_buf_append(&b, "", 1);
        cJSON_AddStringToObject(d, "cmd", (char *)b.p);
        rd_buf_free(&b);
    } else if (!strcmp(cmd, "logcat")) {
        rd_buf_t b;
        rd_buf_init(&b);
        for (int i = 1; i < argc; i++)
            rd_buf_printf(&b, "%s%s", i > 1 ? " " : "", argv[i]);
        rd_buf_append(&b, "", 1);
        cJSON_AddStringToObject(d, "args", (char *)b.p);
        rd_buf_free(&b);
    } else if (!strcmp(cmd, "push") || !strcmp(cmd, "pull")) {
        if (argc < 3) goto usage;
        int push = !strcmp(cmd, "push");
        const char *local = push ? argv[1] : argv[2], *remote = push ? argv[2] : argv[1];
        if (push ? !realpath(local, path) : (snprintf(path, sizeof path, "%s", local), 0)) {
            fprintf(stderr, "%s: %s\n", local, strerror(errno));
            return 1;
        }
        if (!push && path[0] != '/') {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof cwd))
                snprintf(path, sizeof path, "%s/%s", cwd, local);
        }
        char rem[PATH_MAX];
        snprintf(rem, sizeof rem, "%s", remote);
        if (push && rem[strlen(rem) - 1] == '/') { /* push into a directory */
            const char *base = strrchr(local, '/');
            snprintf(rem + strlen(rem), sizeof rem - strlen(rem), "%s", base ? base + 1 : local);
        }
        cJSON_AddStringToObject(d, "local", path);
        cJSON_AddStringToObject(d, "remote", rem);
    } else if (!strcmp(cmd, "key")) {
        if (argc < 2) goto usage;
        cJSON_AddNumberToObject(d, "code", atoi(argv[1]));
    } else if (strcmp(cmd, "devices") && strcmp(cmd, "status") && strcmp(cmd, "restart-server") &&
               strcmp(cmd, "reconnect") && strcmp(cmd, "reload")) {
        goto usage;
    }

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", A.sock_path);
    cli_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (cli_fd < 0 || connect(cli_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "cannot reach the agent at %s: %s (is remodroid-agent running?)\n", A.sock_path,
                strerror(errno));
        return 1;
    }
    signal(SIGINT, on_sigint);
    char *s = msg_print(m);
    cJSON_Delete(m);
    dprintf(cli_fd, "%s\n", s);
    free(s);

    rd_buf_t in;
    rd_buf_init(&in);
    int rc = 1;
    for (;;) {
        rd_buf_reserve(&in, 65536);
        ssize_t n = read(cli_fd, in.p + in.len, in.cap - in.len);
        if (n <= 0)
            break;
        in.len += (size_t)n;
        uint8_t *nl;
        while ((nl = memchr(rd_buf_data(&in), '\n', rd_buf_avail(&in)))) {
            cJSON *r = cJSON_ParseWithLength((const char *)rd_buf_data(&in), (size_t)(nl - rd_buf_data(&in)));
            rd_buf_consume(&in, (size_t)(nl - rd_buf_data(&in)) + 1);
            const char *t = msg_type(r);
            if (!strcmp(t, "out")) {
                fputs(msg_str(msg_d(r), "data", ""), stdout);
                fflush(stdout);
            } else if (!strcmp(t, "error")) {
                fprintf(stderr, "error: %s\n", msg_str(msg_d(r), "msg", "?"));
                cJSON_Delete(r);
                goto done;
            } else if (!strcmp(t, "ack")) {
                cJSON *rd = msg_d(r);
                double code = msg_num(rd, "exit_code", -2);
                if (!strcmp(cmd, "shell") || !strcmp(cmd, "logcat") || !strcmp(cmd, "key")) {
                    rc = code >= 0 ? (int)code : 0;
                } else if (!strcmp(cmd, "push") || !strcmp(cmd, "pull")) {
                    printf("%s\n", msg_str(rd, "result", "ok"));
                    rc = 0;
                } else if (rd->child) {
                    char *p = cJSON_Print(rd);
                    printf("%s\n", p);
                    free(p);
                    rc = 0;
                } else {
                    printf("ok\n");
                    rc = 0;
                }
                cJSON_Delete(r);
                goto done;
            }
            cJSON_Delete(r);
        }
    }
done:
    rd_buf_free(&in);
    close(cli_fd);
    return rc;
usage:
    cJSON_Delete(m);
    fprintf(stderr, "usage: remodroid-agent devices | status | shell CMD... | push LOCAL REMOTE | pull REMOTE LOCAL |\n"
                    "                       logcat [ARGS...] | key CODE | restart-server | reconnect | reload\n");
    return 2;
}
