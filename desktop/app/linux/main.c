/* RemoDroid for Linux: a tray icon (sni.c), its menu and the pairing / status
 * window in GTK 3, the desktop's own toolkit. The agent itself is
 * libremodroid.so (desktop/core/rd_agent.h); its callbacks come from the
 * agent's thread and are moved to the GTK main loop with g_idle_add.
 *
 *   remodroid-desktop [--background] [--conf ~/.config/remodroid/desktop.ini] */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "icon.h"
#include "rd_agent.h"
#include "sni.h"

enum { M_STATUS = 1, M_VIEWER, M_DISCONNECT, M_VIEWONLY, M_OPEN, M_WINDOW, M_LOG, M_LOGIN, M_QUIT };

static struct {
    GtkApplication *app;
    GtkWidget *win, *title, *state, *pair_grid, *line, *name, *pair, *paired_box, *info, *disc, *view_only, *login;
    rd_status_t st;
    int pairing, updating;
    char pair_msg[300];
    const char *conf;
    int background;
    GtkWidget *log_win, *log_view;
    unsigned log_seq;
    guint log_timer;
} U;

static void show_log(void);

static const char *state_text(void)
{
    switch (U.st.state) {
    case RD_UNPAIRED: return "Not paired";
    case RD_CONNECTING: return "Connecting…";
    case RD_ONLINE: return "Online";
    case RD_IN_SESSION: return "Someone is viewing";
    default: return "Stopped";
    }
}

/* ---------------- start at login (XDG autostart) ---------------- */

static char *autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", "remodroid-desktop.desktop", NULL);
}

static int login_enabled(void)
{
    char *p = autostart_path();
    int on = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return on;
}

static void set_login(int on)
{
    char *p = autostart_path();
    if (on) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        char *dir = g_path_get_dirname(p);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        char *body = g_strdup_printf("[Desktop Entry]\nType=Application\nName=RemoDroid\nComment=Remote control of this "
                                     "computer\nExec=\"%s\" --background\nIcon=remodroid\nX-GNOME-Autostart-enabled=true\n",
                                     n > 0 ? exe : "remodroid-desktop");
        g_file_set_contents(p, body, -1, NULL);
        g_free(body);
    } else {
        g_unlink(p);
    }
    g_free(p);
}

/* ---------------- notifications (org.freedesktop.Notifications) ---------------- */

static void notify(const char *text)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", "RemoDroid", 0, "remodroid", "RemoDroid", text, NULL, &hints, -1),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref(bus);
}

/* ---------------- tray ---------------- */

static void show_window(void);

static void open_panel(void)
{
    if (U.st.panel[0])
        g_app_info_launch_default_for_uri(U.st.panel, NULL, NULL);
}

static void tray_refresh(void)
{
    enum { SZ = 64 };
    static uint32_t px[SZ * SZ];
    rd_icon_render(px, SZ, U.st.state == RD_IN_SESSION ? RD_ICON_LIVE : U.st.state == RD_ONLINE ? RD_ICON_ON : RD_ICON_OFF);
    char tip[200];
    if (U.st.state == RD_IN_SESSION && U.st.viewer[0])
        snprintf(tip, sizeof tip, "%s is viewing", U.st.viewer);
    else
        snprintf(tip, sizeof tip, "%s", state_text());
    sni_set_icon(px, SZ, tip, U.st.state == RD_IN_SESSION);
    char status[160], viewer[120];
    snprintf(status, sizeof status, "RemoDroid — %s", state_text());
    snprintf(viewer, sizeof viewer, "Viewer: %s", U.st.viewer[0] ? U.st.viewer : "someone");
    sni_item_t items[12];
    int n = 0;
    items[n++] = (sni_item_t){ M_STATUS, status, 0, 0, 0 };
    if (U.st.state == RD_IN_SESSION)
        items[n++] = (sni_item_t){ M_VIEWER, viewer, 0, 0, 0 };
    items[n++] = (sni_item_t){ 0, NULL, 0, 0, 0 };
    if (U.st.state == RD_IN_SESSION)
        items[n++] = (sni_item_t){ M_DISCONNECT, "Disconnect viewer", 1, 0, 0 };
    items[n++] = (sni_item_t){ M_VIEWONLY, "View only (ignore mouse and keyboard)", 1, 1, U.st.view_only };
    if (U.st.panel[0])
        items[n++] = (sni_item_t){ M_OPEN, "Open panel", 1, 0, 0 };
    items[n++] = (sni_item_t){ M_WINDOW, U.st.state == RD_UNPAIRED ? "Pair with a panel…" : "Pairing & status…", 1, 0, 0 };
    items[n++] = (sni_item_t){ M_LOG, "Show log", 1, 0, 0 };
    items[n++] = (sni_item_t){ 0, NULL, 0, 0, 0 };
    items[n++] = (sni_item_t){ M_LOGIN, "Start at login", 1, 1, login_enabled() };
    items[n++] = (sni_item_t){ M_QUIT, "Quit RemoDroid", 1, 0, 0 };
    sni_set_menu(items, n);
}

static void on_menu(int id)
{
    switch (id) {
    case M_DISCONNECT: rd_agent_end_session(); break;
    case M_VIEWONLY: rd_agent_set("desktop.view_only", U.st.view_only ? "false" : "true"); break;
    case M_OPEN: open_panel(); break;
    case M_WINDOW: show_window(); break;
    case M_LOG: show_log(); break;
    case M_LOGIN: set_login(!login_enabled()); tray_refresh(); break;
    case M_QUIT: g_application_quit(G_APPLICATION(U.app)); break;
    }
}

/* ---------------- window ---------------- */

static void refresh(void)
{
    tray_refresh();
    if (!U.win)
        return;
    U.updating = 1;
    int paired = U.st.state != RD_UNPAIRED && U.st.state != RD_STOPPED;
    gtk_label_set_text(GTK_LABEL(U.title), paired ? "This computer is paired" : "Pair this computer with your panel");
    char s[600];
    if (U.pairing)
        snprintf(s, sizeof s, "Pairing…");
    else if (U.pair_msg[0])
        snprintf(s, sizeof s, "%s", U.pair_msg);
    else if (U.st.problem[0])
        snprintf(s, sizeof s, "%s — %s", state_text(), U.st.problem);
    else
        snprintf(s, sizeof s, "%s", state_text());
    gtk_label_set_text(GTK_LABEL(U.state), s);
    gtk_widget_set_visible(U.pair_grid, !paired);
    gtk_widget_set_visible(U.paired_box, paired);
    gtk_widget_set_sensitive(U.pair, !U.pairing);
    snprintf(s, sizeof s, "Panel:  %s\nDevice: %s", U.st.panel, U.st.device_id);
    gtk_label_set_text(GTK_LABEL(U.info), s);
    gtk_widget_set_visible(U.disc, U.st.state == RD_IN_SESSION);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(U.view_only), U.st.view_only);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(U.login), login_enabled());
    U.updating = 0;
}

static void on_paired_main(int ok, char *msg)
{
    U.pairing = 0;
    snprintf(U.pair_msg, sizeof U.pair_msg, ok ? "Paired as “%s”." : "Pairing failed: %s", msg);
    if (ok)
        notify(U.pair_msg);
    refresh();
}

typedef struct {
    int ok;
    char *msg;
} paired_t;

static gboolean paired_idle(gpointer p)
{
    paired_t *x = p;
    on_paired_main(x->ok, x->msg);
    g_free(x->msg);
    g_free(x);
    return G_SOURCE_REMOVE;
}

static void on_paired(void *ud, int ok, const char *msg)
{
    paired_t *x = g_new0(paired_t, 1);
    x->ok = ok;
    x->msg = g_strdup(msg);
    g_idle_add(paired_idle, x);
}

static void do_pair(GtkWidget *w, gpointer ud)
{
    const char *bad = rd_agent_pair_line(gtk_entry_get_text(GTK_ENTRY(U.line)), gtk_entry_get_text(GTK_ENTRY(U.name)),
                                         on_paired, NULL);
    U.pairing = !bad;
    U.pair_msg[0] = 0;
    if (bad)
        snprintf(U.pair_msg, sizeof U.pair_msg, "Pairing failed: %s", bad);
    refresh();
}

static void do_unpair(GtkWidget *w, gpointer ud)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(U.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
                                          "Unpair this computer?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Viewers can't reach it until you pair it again.");
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
        rd_agent_unpair();
    gtk_widget_destroy(d);
}

static void do_disconnect(GtkWidget *w, gpointer ud) { rd_agent_end_session(); }
static void do_open(GtkWidget *w, gpointer ud) { open_panel(); }

static void view_only_toggled(GtkToggleButton *b, gpointer ud)
{
    if (!U.updating)
        rd_agent_set("desktop.view_only", gtk_toggle_button_get_active(b) ? "true" : "false");
}

static void login_toggled(GtkToggleButton *b, gpointer ud)
{
    if (!U.updating) {
        set_login(gtk_toggle_button_get_active(b));
        tray_refresh();
    }
}

/* ---------------- log window ---------------- */

static void copy_log(GtkWidget *w, gpointer ud)
{
    char *log = rd_agent_log(1000);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), log, -1);
    rd_agent_free(log);
    notify("The log is on the clipboard.");
}

static void open_log_folder(GtkWidget *w, gpointer ud)
{
    char *dir = g_path_get_dirname(rd_agent_log_path());
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri)
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_free(dir);
}

/* New lines appear as they are logged; the view follows the end unless scrolled up. */
static gboolean log_tick(gpointer ud)
{
    unsigned seq = rd_agent_log_seq();
    if (seq == U.log_seq)
        return G_SOURCE_CONTINUE;
    U.log_seq = seq;
    GtkAdjustment *a = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(U.log_view));
    int at_end = gtk_adjustment_get_value(a) + gtk_adjustment_get_page_size(a) >= gtk_adjustment_get_upper(a) - 4;
    char *log = rd_agent_log(1000);
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(U.log_view));
    gtk_text_buffer_set_text(b, log, -1);
    rd_agent_free(log);
    if (at_end) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(b, &end);
        gtk_text_buffer_place_cursor(b, &end);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(U.log_view), gtk_text_buffer_get_insert(b), 0, FALSE, 0, 1);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean log_hidden(GtkWidget *w, GdkEvent *e, gpointer ud)
{
    if (U.log_timer)
        g_source_remove(U.log_timer);
    U.log_timer = 0;
    return gtk_widget_hide_on_delete(w);
}

static void show_log(void)
{
    if (!U.log_win) {
        U.log_win = gtk_application_window_new(U.app);
        gtk_window_set_title(GTK_WINDOW(U.log_win), "RemoDroid log");
        gtk_window_set_default_size(GTK_WINDOW(U.log_win), 820, 460);
        gtk_window_set_icon_name(GTK_WINDOW(U.log_win), "remodroid");
        g_signal_connect(U.log_win, "delete-event", G_CALLBACK(log_hidden), NULL);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 10);
        gtk_container_add(GTK_CONTAINER(U.log_win), box);
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
        U.log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(U.log_view), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(U.log_view), TRUE);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(U.log_view), 6);
        gtk_container_add(GTK_CONTAINER(sw), U.log_view);
        gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *path = gtk_label_new(rd_agent_log_path());
        gtk_label_set_selectable(GTK_LABEL(path), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
        gtk_style_context_add_class(gtk_widget_get_style_context(path), "dim-label");
        gtk_box_pack_start(GTK_BOX(row), path, TRUE, TRUE, 0);
        GtkWidget *folder = gtk_button_new_with_label("Open folder");
        g_signal_connect(folder, "clicked", G_CALLBACK(open_log_folder), NULL);
        gtk_box_pack_end(GTK_BOX(row), folder, FALSE, FALSE, 0);
        GtkWidget *copy = gtk_button_new_with_label("Copy");
        g_signal_connect(copy, "clicked", G_CALLBACK(copy_log), NULL);
        gtk_box_pack_end(GTK_BOX(row), copy, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
        gtk_widget_show_all(box);
    }
    U.log_seq = rd_agent_log_seq() - 1; /* fill it now */
    log_tick(NULL);
    if (!U.log_timer)
        U.log_timer = g_timeout_add(500, log_tick, NULL);
    gtk_window_present(GTK_WINDOW(U.log_win));
}

static void do_show_log(GtkWidget *w, gpointer ud) { show_log(); }

static GtkWidget *add_label(GtkWidget *grid, const char *text, int row)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    return l;
}

static GtkWidget *add_entry(GtkWidget *grid, int row, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
    return e;
}

static void build_window(void)
{
    U.win = gtk_application_window_new(U.app);
    gtk_window_set_title(GTK_WINDOW(U.win), "RemoDroid");
    gtk_window_set_default_size(GTK_WINDOW(U.win), 460, -1);
    gtk_window_set_resizable(GTK_WINDOW(U.win), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(U.win), "remodroid");
    g_signal_connect(U.win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_container_add(GTK_CONTAINER(U.win), box);

    U.title = gtk_label_new("");
    gtk_widget_set_halign(U.title, GTK_ALIGN_START);
    PangoAttrList *a = pango_attr_list_new();
    pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(a, pango_attr_scale_new(1.3));
    gtk_label_set_attributes(GTK_LABEL(U.title), a);
    pango_attr_list_unref(a);
    gtk_box_pack_start(GTK_BOX(box), U.title, FALSE, FALSE, 0);
    U.state = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(U.state), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(U.state), 52);
    gtk_label_set_xalign(GTK_LABEL(U.state), 0);
    gtk_box_pack_start(GTK_BOX(box), U.state, FALSE, FALSE, 0);

    U.pair_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(U.pair_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(U.pair_grid), 12);
    GtkWidget *ll = add_label(U.pair_grid, "Pairing command", 0);
    gtk_container_child_set(GTK_CONTAINER(U.pair_grid), ll, "width", 2, NULL);
    U.line = add_entry(U.pair_grid, 1, "remodroid enroll --panel https://… --code XXXX-XXXX");
    gtk_container_child_set(GTK_CONTAINER(U.pair_grid), U.line, "left-attach", 0, "width", 2, NULL);
    gtk_entry_set_width_chars(GTK_ENTRY(U.line), 52);
    GtkWidget *hint = gtk_label_new("Paste the whole line from the panel: Agents → Add → Computer.");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_grid_attach(GTK_GRID(U.pair_grid), hint, 0, 2, 2, 1);
    add_label(U.pair_grid, "Name", 3);
    U.name = add_entry(U.pair_grid, 3, "");
    char host[64] = "";
    gethostname(host, sizeof host - 1);
    gtk_entry_set_text(GTK_ENTRY(U.name), host);
    U.pair = gtk_button_new_with_label("Pair");
    gtk_widget_set_can_default(U.pair, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(U.pair), "suggested-action");
    gtk_widget_set_halign(U.pair, GTK_ALIGN_END);
    g_signal_connect(U.pair, "clicked", G_CALLBACK(do_pair), NULL);
    gtk_grid_attach(GTK_GRID(U.pair_grid), U.pair, 1, 4, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), U.pair_grid, FALSE, FALSE, 0);

    U.paired_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    U.info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(U.info), 0);
    gtk_label_set_selectable(GTK_LABEL(U.info), TRUE);
    gtk_box_pack_start(GTK_BOX(U.paired_box), U.info, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *open = gtk_button_new_with_label("Open panel");
    g_signal_connect(open, "clicked", G_CALLBACK(do_open), NULL);
    gtk_box_pack_start(GTK_BOX(row), open, FALSE, FALSE, 0);
    U.disc = gtk_button_new_with_label("Disconnect viewer");
    g_signal_connect(U.disc, "clicked", G_CALLBACK(do_disconnect), NULL);
    gtk_box_pack_start(GTK_BOX(row), U.disc, FALSE, FALSE, 0);
    GtkWidget *unpair = gtk_button_new_with_label("Unpair");
    gtk_style_context_add_class(gtk_widget_get_style_context(unpair), "destructive-action");
    g_signal_connect(unpair, "clicked", G_CALLBACK(do_unpair), NULL);
    gtk_box_pack_end(GTK_BOX(row), unpair, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(U.paired_box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), U.paired_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    U.view_only = gtk_check_button_new_with_label("View only: ignore the viewer's mouse and keyboard");
    g_signal_connect(U.view_only, "toggled", G_CALLBACK(view_only_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(box), U.view_only, FALSE, FALSE, 0);
    U.login = gtk_check_button_new_with_label("Start RemoDroid when I log in");
    g_signal_connect(U.login, "toggled", G_CALLBACK(login_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(box), U.login, FALSE, FALSE, 0);
    GtkWidget *log = gtk_button_new_with_label("Show log");
    gtk_widget_set_halign(log, GTK_ALIGN_START);
    g_signal_connect(log, "clicked", G_CALLBACK(do_show_log), NULL);
    gtk_box_pack_start(GTK_BOX(box), log, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
}

static void show_window(void)
{
    if (!U.win)
        build_window();
    refresh();
    gtk_widget_grab_default(U.pair);
    gtk_window_present(GTK_WINDOW(U.win));
}

/* ---------------- agent callbacks ---------------- */

static gboolean status_idle(gpointer p)
{
    rd_status_t *s = p;
    int was = U.st.state;
    U.st = *s;
    g_free(s);
    if (was == RD_UNPAIRED && U.st.state != RD_UNPAIRED)
        U.pair_msg[0] = 0;
    refresh();
    return G_SOURCE_REMOVE;
}

static void on_status(void *ud, const rd_status_t *s) { g_idle_add(status_idle, g_memdup2(s, sizeof *s)); }

static gboolean notify_idle(gpointer p)
{
    notify(p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void on_notify(void *ud, const char *text) { g_idle_add(notify_idle, g_strdup(text)); }

static gboolean confirm_idle(gpointer p)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "%s wants to see and control this computer.", (char *)p);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Allow the connection?");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Deny", GTK_RESPONSE_NO, "Allow", GTK_RESPONSE_YES, NULL);
    gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
    rd_agent_confirm(gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES);
    gtk_widget_destroy(d);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void on_confirm(void *ud, const char *viewer) { g_idle_add(confirm_idle, g_strdup(viewer)); }

/* ---------------- application ---------------- */

static void on_startup(GApplication *a, gpointer ud)
{
    g_application_hold(a); /* runs in the tray without a window */
    sni_start((sni_cb_t){ show_window, on_menu });
    U.st.state = RD_STOPPED;
    tray_refresh();
    rd_agent_cb_t cb = { on_status, on_confirm, on_notify };
    if (rd_agent_start(U.conf, cb, NULL) < 0) {
        GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "RemoDroid could not start.");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "See ~/.config/remodroid/desktop.log.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_application_release(a);
        return;
    }
    rd_agent_status(&U.st);
    refresh();
}

static int first_activation = 1;

static void on_activate(GApplication *a, gpointer ud)
{
    /* the first start honours --background; starting it again shows the window */
    if (!first_activation || !U.background || U.st.state == RD_UNPAIRED || U.st.problem[0])
        show_window();
    first_activation = 0;
}

static void on_shutdown(GApplication *a, gpointer ud) { rd_agent_stop(); }

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--background"))
            U.background = 1;
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc)
            U.conf = argv[++i];
    }
    gtk_init(&argc, &argv);
    U.app = gtk_application_new("com.remodroid.desktop", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(U.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(U.app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(U.app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int r = g_application_run(G_APPLICATION(U.app), 1, argv); /* our own options are handled above */
    g_object_unref(U.app);
    return r;
}
