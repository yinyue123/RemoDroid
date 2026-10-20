/* `remodroid enroll --panel https://your.panel/ --code X5SX-K3M7`
 *
 * Redeems a one-time code from the panel's Agents tab (server/enroll.c, or
 * the Cloudflare Worker): the panel answers with this router's device id and
 * secret, which go into agent.ini together with the WebSocket URL. A running
 * agent is told to reload and we wait until it is online. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "enroll.h"
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
    if (strncmp(panel, "https://", 8) && strncmp(panel, "http://", 7)) {
        fprintf(stderr, "--panel must start with https:// or http://\n");
        return 2;
    }
    char host[64];
    rd_hostname(host, sizeof host);
    printf("▸ contacting the panel %s…\n", panel);
    fflush(stdout);
    rd_enroll_t en;
    char err[300];
    if (rd_enroll(panel, code, name ? name : host, "android", ca, "remodroid-agent/" RD_VERSION, &en, err,
                  sizeof err) < 0) {
        fprintf(stderr, "✗ %s\n", err);
        if (strstr(err, "trusted") || strstr(err, "CA bundle"))
            fprintf(stderr, "  (check the router's clock and that ca-bundle / ca-certificates is installed)\n");
        return 1;
    }
    if (rd_conf_write(A.conf_path, "server", "url", en.ws_url) < 0 ||
        rd_conf_write(A.conf_path, "server", "device_id", en.device_id) < 0 ||
        rd_conf_write(A.conf_path, "server", "device_secret", en.device_secret) < 0 ||
        (ca && rd_conf_write(A.conf_path, "server", "ca_file", ca) < 0)) {
        fprintf(stderr, "✗ cannot write %s (run as root?)\n", A.conf_path);
        return 1;
    }
    printf("✓ enrolled as \"%s\" (%s); saved in %s\n", en.name, en.device_id, A.conf_path);

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
