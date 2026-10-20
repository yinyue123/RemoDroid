/* remodroid-desktopd: the desktop agent without a UI (servers, systemd user
 * services, tests). Same library and settings as the app.
 *
 *   remodroid-desktopd [-c desktop.ini] [-v]                    run
 *   remodroid-desktopd [-c desktop.ini] pair --panel URL --code CODE [--name NAME]
 *   remodroid-desktopd [-c desktop.ini] unpair
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "rd_agent.h"

static volatile int quit, done, ok;
static char msg[300];

static void on_signal(int s) { quit = 1; }

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static const char *state_name(rd_state_t s)
{
    switch (s) {
    case RD_UNPAIRED: return "not paired";
    case RD_CONNECTING: return "connecting";
    case RD_ONLINE: return "online";
    case RD_IN_SESSION: return "viewer connected";
    default: return "stopped";
    }
}

static void on_status(void *ud, const rd_status_t *s)
{
    fprintf(stderr, "▸ %s%s%s%s%s\n", state_name(s->state), s->viewer[0] ? " (" : "", s->viewer,
            s->viewer[0] ? ")" : "", s->problem[0] ? " — " : "");
    if (s->problem[0])
        fprintf(stderr, "  %s\n", s->problem);
}

static void on_paired(void *ud, int good, const char *m)
{
    ok = good;
    snprintf(msg, sizeof msg, "%s", m);
    done = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: remodroid-desktopd [-c desktop.ini] [-v]\n"
                    "       remodroid-desktopd [-c desktop.ini] pair --panel https://your.panel --code XXXX-XXXX [--name NAME]\n"
                    "       remodroid-desktopd [-c desktop.ini] unpair\n"
                    "Get a pairing code on the panel: Agents → Add → Computer.\n");
}

int main(int argc, char **argv)
{
    const char *conf = NULL, *panel = NULL, *code = NULL, *name = NULL, *cmd = NULL;
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-c") && v) conf = v, i++;
        else if (!strcmp(a, "-v")) verbose++;
        else if (!strcmp(a, "--panel") && v) panel = v, i++;
        else if (!strcmp(a, "--code") && v) code = v, i++;
        else if (!strcmp(a, "--name") && v) name = v, i++;
        else if (!cmd && (!strcmp(a, "pair") || !strcmp(a, "unpair") || !strcmp(a, "run"))) cmd = a;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (cmd && !strcmp(cmd, "pair") && (!panel || !code)) {
        usage();
        return 2;
    }
    if (verbose) { /* this run only; [desktop] log_level is the lasting setting */
#ifdef _WIN32
        _putenv(verbose > 1 ? "RD_LOG_LEVEL=trace" : "RD_LOG_LEVEL=debug");
#else
        setenv("RD_LOG_LEVEL", verbose > 1 ? "trace" : "debug", 1);
#endif
    }
    rd_agent_cb_t cb = { on_status, NULL, NULL };
    if (rd_agent_start(conf, cb, NULL) < 0) {
        fprintf(stderr, "✗ cannot start (see the log)\n");
        return 1;
    }
    if (cmd && !strcmp(cmd, "pair")) {
        fprintf(stderr, "▸ contacting the panel %s…\n", panel);
        rd_agent_pair(panel, code, name, on_paired, NULL);
        while (!done)
            sleep_ms(50);
        if (!ok) {
            fprintf(stderr, "✗ %s\n", msg);
            rd_agent_stop();
            return 1;
        }
        fprintf(stderr, "✓ paired as \"%s\"; saved in %s\n", msg, conf ? conf : rd_agent_default_conf());
        rd_agent_stop();
        return 0;
    }
    if (cmd && !strcmp(cmd, "unpair")) {
        rd_agent_unpair();
        sleep_ms(300);
        rd_agent_stop();
        fprintf(stderr, "✓ unpaired\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!quit)
        sleep_ms(200);
    rd_agent_stop();
    return 0;
}
