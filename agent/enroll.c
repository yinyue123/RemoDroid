/* `remodroid enroll --panel https://your.panel/ --code X5SX-K3M7`
 *
 * Redeems a one-time code from the panel's Agents tab (server/enroll.c, or
 * the Cloudflare Worker): the panel answers with this router's device id and
 * secret, which go into agent.ini together with the WebSocket URL. A running
 * agent is told to reload and we wait until it is online. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "agent.h"
#include "httpc.h"
#include "msg.h"

static void usage(void)
{
    fprintf(stderr, "usage: remodroid-agent [-c agent.ini] enroll --panel https://your.panel/ --code XXXX-XXXX\n"
                    "                       [--name NAME] [--ca-file FILE]\n"
                    "Get the command with its code on the panel: Agents → Add.\n");
}

int enroll_main(int argc, char **argv)
{
    const char *panel = NULL, *code = NULL, *name = NULL, *ca = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if ((!strcmp(a, "--panel") || !strcmp(a, "-p")) && v) panel = v, i++;
        else if ((!strcmp(a, "--code") || !strcmp(a, "-k")) && v) code = v, i++;
        else if (!strcmp(a, "--name") && v) name = v, i++;
        else if (!strcmp(a, "--ca-file") && v) ca = v, i++;
        else { usage(); return 2; }
    }
    if (!panel || !code) {
        usage();
        return 2;
    }
    int tls = !strncmp(panel, "https://", 8);
    if (!tls && strncmp(panel, "http://", 7)) {
        fprintf(stderr, "--panel must start with https:// or http://\n");
        return 2;
    }
    char base[300];
    snprintf(base, sizeof base, "%s", panel);
    size_t bl = strlen(base);
    while (bl > 8 && base[bl - 1] == '/')
        base[--bl] = 0;
    const char *hostport = base + (tls ? 8 : 7);
    if (strchr(hostport, '/')) {
        fprintf(stderr, "--panel is the panel's address only, e.g. https://remodroid.example.com\n");
        return 2;
    }

    struct utsname u;
    uname(&u);
    char host[64];
    if (gethostname(host, sizeof host) < 0)
        snprintf(host, sizeof host, "router");
    host[sizeof host - 1] = 0;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "code", code);
    cJSON_AddStringToObject(req, "name", name ? name : host);
    cJSON_AddStringToObject(req, "arch", u.machine);
    cJSON_AddStringToObject(req, "version", RD_VERSION);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    printf("▸ contacting the panel %s…\n", base);
    fflush(stdout);
    char url[400], err[300];
    snprintf(url, sizeof url, "%s/api/v1/enroll", base);
    httpc_t *h = httpc_new(ca ? ca : "", "remodroid-agent/" RD_VERSION);
    httpc_resp_t r;
    int e = h ? httpc_request(h, "POST", url, "application/json", body, &r, err, sizeof err) : -1;
    free(body);
    httpc_free(h);
    if (e < 0) {
        fprintf(stderr, "✗ %s\n", h ? err : "out of memory");
        if (strstr(err, "trusted") || strstr(err, "CA bundle"))
            fprintf(stderr, "  (check the router's clock and that ca-bundle / ca-certificates is installed)\n");
        return 1;
    }
    cJSON *res = cJSON_Parse(r.body);
    if (r.status != 200) {
        fprintf(stderr, "✗ %s\n", msg_str(res, "msg", r.status == 404 ? "this panel has no enrollment (wrong address?)"
                                                                        : "the panel refused the code"));
        cJSON_Delete(res);
        httpc_resp_free(&r);
        return 1;
    }
    httpc_resp_free(&r);
    const char *id = msg_str(res, "device_id", ""), *secret = msg_str(res, "device_secret", "");
    if (!*id || !*secret) {
        fprintf(stderr, "✗ unexpected answer from the panel\n");
        cJSON_Delete(res);
        return 1;
    }
    char ws[400];
    snprintf(ws, sizeof ws, "%s://%s%s", tls ? "wss" : "ws", hostport, msg_str(res, "ws_path", "/ws/agent"));
    if (rd_conf_write(A.conf_path, "server", "url", ws) < 0 || rd_conf_write(A.conf_path, "server", "device_id", id) < 0 ||
        rd_conf_write(A.conf_path, "server", "device_secret", secret) < 0 ||
        (ca && rd_conf_write(A.conf_path, "server", "ca_file", ca) < 0)) {
        fprintf(stderr, "✗ cannot write %s (run as root?)\n", A.conf_path);
        cJSON_Delete(res);
        return 1;
    }
    printf("✓ enrolled as \"%s\" (%s); saved in %s\n", msg_str(res, "name", id), id, A.conf_path);
    cJSON_Delete(res);

    /* a running agent picks it up now; otherwise it's used on the next start */
    if (agent_reload() < 0)
        return 1;
    cJSON *ack = cli_call("reload");
    if (!ack) {
        printf("▸ the agent isn't running; start it to connect:\n"
               "    /etc/init.d/remodroid-agent restart   (OpenWrt, Alpine)\n"
               "    systemctl restart remodroid-agent     (Debian, Ubuntu, CentOS)\n");
        return 0;
    }
    cJSON_Delete(ack);
    printf("▸ connecting…\n");
    fflush(stdout);
    for (int i = 0; i < 30; i++) {
        sleep(1);
        cJSON *st = cli_call("status");
        int on = st && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "server_connected"));
        cJSON_Delete(st);
        if (on) {
            printf("✓ bound — this router is online\n");
            return 0;
        }
    }
    printf("▸ saved, but not connected yet; see the agent's log (logread -e remodroid / journalctl -u remodroid-agent)\n");
    return 0;
}
