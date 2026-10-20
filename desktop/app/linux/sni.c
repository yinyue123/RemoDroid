/* StatusNotifierItem (org.kde.StatusNotifierItem) with its menu over
 * com.canonical.dbusmenu, implemented with GDBus; GtkStatusIcon (XEmbed) when
 * no StatusNotifierWatcher runs. See sni.h. */
#include "sni.h"

#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

static const char ITEM_XML[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='WindowId' type='i' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionIconName' type='s' access='read'/>"
    "<property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewAttentionIcon'/><signal name='NewToolTip'/><signal name='NewTitle'/>"
    "<signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "</interface></node>";

static const char MENU_XML[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' name='parentId' direction='in'/><arg type='i' name='recursionDepth' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/><arg type='u' name='revision' direction='out'/>"
    "<arg type='(ia{sv}av)' name='layout' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' name='ids' direction='in'/><arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='a(ia{sv})' name='properties' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' name='id' direction='in'/><arg type='s' name='name' direction='in'/>"
    "<arg type='v' name='value' direction='out'/></method>"
    "<method name='Event'><arg type='i' name='id' direction='in'/><arg type='s' name='eventId' direction='in'/>"
    "<arg type='v' name='data' direction='in'/><arg type='u' name='timestamp' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' name='events' direction='in'/><arg type='ai' name='idErrors' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' name='id' direction='in'/><arg type='b' name='needUpdate' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' name='ids' direction='in'/><arg type='ai' name='updatesNeeded' direction='out'/>"
    "<arg type='ai' name='idErrors' direction='out'/></method>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ia{sv})'/></signal>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemActivationRequested'><arg type='i'/><arg type='u'/></signal>"
    "</interface></node>";

#define MAX_ITEMS 24

static struct {
    sni_cb_t cb;
    GDBusConnection *bus;
    char service[96];
    GBytes *pixmap; /* ARGB32, network byte order */
    int size, attention;
    char tooltip[256];
    sni_item_t items[MAX_ITEMS];
    char labels[MAX_ITEMS][128];
    int nitems;
    guint revision;
    GtkStatusIcon *legacy;
} T;

/* ---------------- StatusNotifierItem ---------------- */

static GVariant *pixmap_variant(void)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    if (T.pixmap)
        g_variant_builder_add(&b, "(ii@ay)", T.size, T.size,
                              g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, T.pixmap, TRUE));
    return g_variant_builder_end(&b);
}

static GVariant *item_get(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop,
                          GError **err, gpointer ud)
{
    if (!strcmp(prop, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!strcmp(prop, "Id")) return g_variant_new_string("remodroid");
    if (!strcmp(prop, "Title")) return g_variant_new_string("RemoDroid");
    if (!strcmp(prop, "Status")) return g_variant_new_string(T.attention ? "NeedsAttention" : "Active");
    if (!strcmp(prop, "WindowId")) return g_variant_new_int32(0);
    if (!strcmp(prop, "IconName") || !strcmp(prop, "AttentionIconName")) return g_variant_new_string("");
    if (!strcmp(prop, "IconPixmap") || !strcmp(prop, "AttentionIconPixmap")) return pixmap_variant();
    if (!strcmp(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "Menu")) return g_variant_new_object_path("/MenuBar");
    if (!strcmp(prop, "ToolTip")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(sa(iiay)ss)", "", &b, "RemoDroid", T.tooltip);
    }
    return NULL;
}

static void item_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
                      GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
    if (!strcmp(method, "Activate") && T.cb.activate)
        T.cb.activate();
    g_dbus_method_invocation_return_value(inv, NULL);
}

/* ---------------- dbusmenu ---------------- */

static GVariant *item_props(const sni_item_t *it)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (!it->label) {
        g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(it->label));
        g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(it->enabled));
        if (it->check) {
            g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(it->checked ? 1 : 0));
        }
    }
    g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(TRUE));
    return g_variant_builder_end(&b);
}

static int item_id(int i) { return T.items[i].label ? T.items[i].id : 1000 + i; }

static GVariant *layout(void)
{
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    for (int i = 0; i < T.nitems; i++) {
        GVariantBuilder none;
        g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&kids, "v", g_variant_new("(i@a{sv}av)", item_id(i), item_props(&T.items[i]), &none));
    }
    GVariantBuilder root;
    g_variant_builder_init(&root, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&root, "{sv}", "children-display", g_variant_new_string("submenu"));
    return g_variant_new("(ia{sv}av)", 0, &root, &kids);
}

static void menu_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
                      GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
    if (!strcmp(method, "GetLayout")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", T.revision, layout()));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        for (int i = 0; i < T.nitems; i++)
            g_variant_builder_add(&b, "(i@a{sv})", item_id(i), item_props(&T.items[i]));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &b));
    } else if (!strcmp(method, "GetProperty")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", g_variant_new_string("")));
    } else if (!strcmp(method, "Event")) {
        gint32 id;
        const char *ev;
        g_variant_get(params, "(i&svu)", &id, &ev, NULL, NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
        if (!strcmp(ev, "clicked") && id > 0 && id < 1000 && T.cb.menu)
            T.cb.menu(id);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *it;
        g_variant_get(params, "(a(isvu))", &it);
        gint32 id;
        const char *ev;
        GVariant *data;
        guint32 ts;
        int clicked[8], n = 0;
        while (g_variant_iter_loop(it, "(i&svu)", &id, &ev, &data, &ts))
            if (!strcmp(ev, "clicked") && n < 8)
                clicked[n++] = id;
        g_variant_iter_free(it);
        GVariantBuilder e;
        g_variant_builder_init(&e, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", &e));
        for (int i = 0; i < n; i++)
            if (clicked[i] > 0 && clicked[i] < 1000 && T.cb.menu)
                T.cb.menu(clicked[i]);
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        GVariantBuilder a, b;
        g_variant_builder_init(&a, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(aiai)", &a, &b));
    } else {
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static GVariant *menu_get(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop,
                          GError **err, gpointer ud)
{
    if (!strcmp(prop, "Version")) return g_variant_new_uint32(3);
    if (!strcmp(prop, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(prop, "Status")) return g_variant_new_string("normal");
    if (!strcmp(prop, "IconThemePath")) return g_variant_new_strv(NULL, 0);
    return NULL;
}

/* ---------------- registration ---------------- */

static void register_with_watcher(GDBusConnection *c, const char *name, const char *owner, gpointer ud)
{
    g_dbus_connection_call(T.bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                           "RegisterStatusNotifierItem", g_variant_new("(s)", T.service), NULL, G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);
}

static void legacy_activate(GtkStatusIcon *i, gpointer ud)
{
    if (T.cb.activate)
        T.cb.activate();
}

static void legacy_item(GtkMenuItem *mi, gpointer id)
{
    if (T.cb.menu)
        T.cb.menu(GPOINTER_TO_INT(id));
}

static void legacy_menu(GtkStatusIcon *i, guint button, guint time, gpointer ud)
{
    GtkWidget *m = gtk_menu_new();
    for (int k = 0; k < T.nitems; k++) {
        const sni_item_t *it = &T.items[k];
        GtkWidget *mi;
        if (!it->label) {
            mi = gtk_separator_menu_item_new();
        } else if (it->check) {
            mi = gtk_check_menu_item_new_with_label(it->label);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), it->checked);
        } else {
            mi = gtk_menu_item_new_with_label(it->label);
        }
        gtk_widget_set_sensitive(mi, it->enabled || !it->label);
        if (it->label)
            g_signal_connect(mi, "activate", G_CALLBACK(legacy_item), GINT_TO_POINTER(it->id));
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    }
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), NULL);
}

static int has_watcher(void)
{
    GVariant *r = g_dbus_connection_call_sync(T.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "NameHasOwner", g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
                                              G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    gboolean on = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &on);
        g_variant_unref(r);
    }
    return on;
}

void sni_start(sni_cb_t cb)
{
    T.cb = cb;
    T.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (T.bus && has_watcher()) {
        static const GDBusInterfaceVTable item_vt = { item_call, item_get, NULL, { 0 } };
        static const GDBusInterfaceVTable menu_vt = { menu_call, menu_get, NULL, { 0 } };
        GDBusNodeInfo *in = g_dbus_node_info_new_for_xml(ITEM_XML, NULL), *mn = g_dbus_node_info_new_for_xml(MENU_XML, NULL);
        g_dbus_connection_register_object(T.bus, "/StatusNotifierItem", in->interfaces[0], &item_vt, NULL, NULL, NULL);
        g_dbus_connection_register_object(T.bus, "/MenuBar", mn->interfaces[0], &menu_vt, NULL, NULL, NULL);
        snprintf(T.service, sizeof T.service, "org.kde.StatusNotifierItem-%d-1", (int)getpid());
        g_bus_own_name_on_connection(T.bus, T.service, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
        /* register now, and again whenever the tray host (re)starts */
        g_bus_watch_name_on_connection(T.bus, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                       register_with_watcher, NULL, NULL, NULL);
        return;
    }
    T.legacy = gtk_status_icon_new();
    gtk_status_icon_set_title(T.legacy, "RemoDroid");
    g_signal_connect(T.legacy, "activate", G_CALLBACK(legacy_activate), NULL);
    g_signal_connect(T.legacy, "popup-menu", G_CALLBACK(legacy_menu), NULL);
}

void sni_set_icon(const uint32_t *argb, int size, const char *tooltip, int attention)
{
    snprintf(T.tooltip, sizeof T.tooltip, "%s", tooltip);
    int changed_status = T.attention != attention;
    T.attention = attention;
    if (T.legacy) {
        GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
        guchar *px = gdk_pixbuf_get_pixels(pb);
        int stride = gdk_pixbuf_get_rowstride(pb);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                uint32_t c = argb[y * size + x];
                guchar *p = px + y * stride + x * 4;
                p[0] = (guchar)(c >> 16), p[1] = (guchar)(c >> 8), p[2] = (guchar)c, p[3] = (guchar)(c >> 24);
            }
        gtk_status_icon_set_from_pixbuf(T.legacy, pb);
        gtk_status_icon_set_tooltip_text(T.legacy, tooltip);
        g_object_unref(pb);
        return;
    }
    guchar *bytes = g_malloc((size_t)size * size * 4);
    for (int i = 0; i < size * size; i++) { /* ARGB32 in network byte order */
        bytes[i * 4] = (guchar)(argb[i] >> 24), bytes[i * 4 + 1] = (guchar)(argb[i] >> 16);
        bytes[i * 4 + 2] = (guchar)(argb[i] >> 8), bytes[i * 4 + 3] = (guchar)argb[i];
    }
    if (T.pixmap)
        g_bytes_unref(T.pixmap);
    T.pixmap = g_bytes_new_take(bytes, (size_t)size * size * 4);
    T.size = size;
    if (!T.bus)
        return;
    g_dbus_connection_emit_signal(T.bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL);
    g_dbus_connection_emit_signal(T.bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
    if (changed_status)
        g_dbus_connection_emit_signal(T.bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewStatus",
                                      g_variant_new("(s)", attention ? "NeedsAttention" : "Active"), NULL);
}

void sni_set_menu(const sni_item_t *items, int n)
{
    T.nitems = n > MAX_ITEMS ? MAX_ITEMS : n;
    for (int i = 0; i < T.nitems; i++) {
        T.items[i] = items[i];
        if (items[i].label) {
            snprintf(T.labels[i], sizeof T.labels[i], "%s", items[i].label);
            T.items[i].label = T.labels[i];
        }
    }
    T.revision++;
    if (T.bus && !T.legacy)
        g_dbus_connection_emit_signal(T.bus, NULL, "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated",
                                      g_variant_new("(ui)", T.revision, 0), NULL);
}
