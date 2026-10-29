/* os-portal: the Operating System's portal backend.
 *
 * Apps (Flatpak or not) ask xdg-desktop-portal for things that touch the
 * system: choosing files, the colour scheme, notifications. The portal
 * front end hands those to this backend (org.freedesktop.impl.portal.*).
 * The parts people see are drawn by the desktop shell: this program talks
 * to it over a local socket, one JSON object per line:
 *
 *   to the shell:   {"id":7,"op":"file","mode":"open","title":"Open",...}
 *                   {"op":"notify","app":"org.x","id":"n1","title":"..","body":".."}
 *   from the shell: {"id":7,"response":0,"uris":["file:///root/a.txt"]}
 *                   {"op":"appearance","scheme":1,"accent":[0.2,0.4,0.9],"contrast":0}
 *
 * Built against the Freedesktop SDK (scripts/build-base.sh os-portal). */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.os"
#define OBJ_PATH "/org/freedesktop/portal/desktop"

static const char introspection[] =
    "<node>"
    " <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "  <method name='OpenFile'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "   <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "   <arg type='a{sv}' name='results' direction='out'/></method>"
    "  <method name='SaveFile'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "   <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "   <arg type='a{sv}' name='results' direction='out'/></method>"
    "  <method name='SaveFiles'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "   <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "   <arg type='a{sv}' name='results' direction='out'/></method>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.impl.portal.Settings'>"
    "  <method name='ReadAll'><arg type='as' name='namespaces' direction='in'/>"
    "   <arg type='a{sa{sv}}' name='value' direction='out'/></method>"
    "  <method name='Read'><arg type='s' name='namespace' direction='in'/><arg type='s' name='key' direction='in'/>"
    "   <arg type='v' name='value' direction='out'/></method>"
    "  <signal name='SettingChanged'><arg type='s' name='namespace'/><arg type='s' name='key'/><arg type='v' name='value'/></signal>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.impl.portal.Notification'>"
    "  <method name='AddNotification'><arg type='s' name='app_id' direction='in'/><arg type='s' name='id' direction='in'/>"
    "   <arg type='a{sv}' name='notification' direction='in'/></method>"
    "  <method name='RemoveNotification'><arg type='s' name='app_id' direction='in'/><arg type='s' name='id' direction='in'/></method>"
    "  <signal name='ActionInvoked'><arg type='s' name='app_id'/><arg type='s' name='id'/><arg type='s' name='action'/>"
    "   <arg type='av' name='parameter'/></signal>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.impl.portal.AppChooser'>"
    "  <method name='ChooseApplication'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "   <arg type='s' name='parent_window' direction='in'/><arg type='as' name='choices' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "   <arg type='a{sv}' name='results' direction='out'/></method>"
    "  <method name='UpdateChoices'><arg type='o' name='handle' direction='in'/><arg type='as' name='choices' direction='in'/></method>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.impl.portal.Inhibit'>"
    "  <method name='Inhibit'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "   <arg type='s' name='window' direction='in'/><arg type='u' name='flags' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/></method>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    "</node>";

static GDBusConnection *bus;
static GDBusNodeInfo *node;

/* ---- the shell ---- */

static GSocketConnection *shell;
static GDataInputStream *shell_in;
static GHashTable *pending; /* id -> GDBusMethodInvocation */
static guint next_id = 1;

/* What the shell last said about its look. */
static guint scheme = 0;      /* 0 no preference, 1 dark, 2 light */
static double accent[3] = {-1, -1, -1};
static guint contrast = 0;

static void shell_connect(void);

static gboolean shell_retry(gpointer data) {
    (void)data;
    shell_connect();
    return G_SOURCE_REMOVE;
}

static void shell_lost(void) {
    g_clear_object(&shell_in);
    g_clear_object(&shell);
    /* every open request gets a "cancelled" answer */
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, pending);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_dbus_method_invocation_return_value(value, g_variant_new("(u@a{sv})", 2, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)));
        g_hash_table_iter_remove(&it);
    }
    g_timeout_add_seconds(2, shell_retry, NULL);
}

static void shell_send(JsonBuilder *b) {
    if (!shell) {
        g_object_unref(b);
        return;
    }
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    gchar *text = json_generator_to_data(g, NULL);
    GString *line = g_string_new(text);
    g_string_append_c(line, '\n');
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(shell));
    g_output_stream_write_all(out, line->str, line->len, NULL, NULL, NULL);
    g_string_free(line, TRUE);
    g_free(text);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
}

static void emit_setting(const char *ns, const char *key, GVariant *value) {
    g_dbus_connection_emit_signal(bus, NULL, OBJ_PATH, "org.freedesktop.impl.portal.Settings", "SettingChanged",
                                  g_variant_new("(ssv)", ns, key, value), NULL);
}

static GVariant *accent_value(void) { return g_variant_new("(ddd)", accent[0], accent[1], accent[2]); }

static void on_shell_line(GObject *src, GAsyncResult *res, gpointer data);

static void read_next(void) {
    if (shell_in)
        g_data_input_stream_read_line_async(shell_in, G_PRIORITY_DEFAULT, NULL, on_shell_line, NULL);
}

static void on_shell_line(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    gsize len;
    gchar *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) {
        shell_lost();
        return;
    }
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, line, (gssize)len, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        const char *op = json_object_has_member(o, "op") ? json_object_get_string_member(o, "op") : NULL;
        if (op && !strcmp(op, "appearance")) {
            guint s = (guint)json_object_get_int_member_with_default(o, "scheme", 0);
            guint c = (guint)json_object_get_int_member_with_default(o, "contrast", 0);
            double a[3] = {-1, -1, -1};
            if (json_object_has_member(o, "accent")) {
                JsonArray *arr = json_object_get_array_member(o, "accent");
                for (guint i = 0; i < 3 && i < json_array_get_length(arr); i++)
                    a[i] = json_array_get_double_element(arr, i);
            }
            if (s != scheme) {
                scheme = s;
                emit_setting("org.freedesktop.appearance", "color-scheme", g_variant_new_uint32(scheme));
            }
            if (memcmp(a, accent, sizeof a)) {
                memcpy(accent, a, sizeof a);
                emit_setting("org.freedesktop.appearance", "accent-color", accent_value());
            }
            if (c != contrast) {
                contrast = c;
                emit_setting("org.freedesktop.appearance", "contrast", g_variant_new_uint32(contrast));
            }
        } else if (json_object_has_member(o, "id")) {
            guint id = (guint)json_object_get_int_member(o, "id");
            GDBusMethodInvocation *inv = g_hash_table_lookup(pending, GUINT_TO_POINTER(id));
            if (inv) {
                guint response = (guint)json_object_get_int_member_with_default(o, "response", 2);
                GVariantBuilder r;
                g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
                if (json_object_has_member(o, "uris")) {
                    JsonArray *uris = json_object_get_array_member(o, "uris");
                    GVariantBuilder u;
                    g_variant_builder_init(&u, G_VARIANT_TYPE("as"));
                    for (guint i = 0; i < json_array_get_length(uris); i++)
                        g_variant_builder_add(&u, "s", json_array_get_string_element(uris, i));
                    g_variant_builder_add(&r, "{sv}", "uris", g_variant_builder_end(&u));
                }
                if (json_object_has_member(o, "choice"))
                    g_variant_builder_add(&r, "{sv}", "choice",
                                          g_variant_new_string(json_object_get_string_member(o, "choice")));
                g_variant_builder_add(&r, "{sv}", "writable", g_variant_new_boolean(TRUE));
                g_dbus_method_invocation_return_value(inv, g_variant_new("(ua{sv})", response, &r));
                g_hash_table_remove(pending, GUINT_TO_POINTER(id));
            }
        }
    }
    g_object_unref(p);
    g_free(line);
    read_next();
}

static void shell_connect(void) {
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    gchar *path = g_build_filename(dir ? dir : "/run/user/1000", "os-shell-portal.sock", NULL);
    GSocketClient *c = g_socket_client_new();
    GSocketAddress *a = g_unix_socket_address_new(path);
    shell = g_socket_client_connect(c, G_SOCKET_CONNECTABLE(a), NULL, NULL);
    g_object_unref(a);
    g_object_unref(c);
    g_free(path);
    if (!shell) {
        g_timeout_add_seconds(2, shell_retry, NULL);
        return;
    }
    shell_in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(shell)));
    read_next();
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "op");
    json_builder_add_string_value(b, "hello");
    json_builder_end_object(b);
    shell_send(b);
}

/* ---- FileChooser ---- */

static gchar *bytes_path(GVariant *v) { /* ay, NUL-terminated */
    gsize n;
    const gchar *s = g_variant_get_fixed_array(v, &n, 1);
    return n ? g_strndup(s, n) : NULL;
}

static void add_filters(JsonBuilder *b, GVariant *filters) {
    /* a(sa(us)): a name, then patterns (0) or MIME types (1) */
    json_builder_begin_array(b);
    GVariantIter it;
    const gchar *name;
    GVariant *rules;
    g_variant_iter_init(&it, filters);
    while (g_variant_iter_loop(&it, "(&s@a(us))", &name, &rules)) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, name);
        json_builder_set_member_name(b, "patterns");
        json_builder_begin_array(b);
        GVariantIter ri;
        guint kind;
        const gchar *rule;
        g_variant_iter_init(&ri, rules);
        while (g_variant_iter_loop(&ri, "(u&s)", &kind, &rule)) {
            if (kind == 0) {
                json_builder_add_string_value(b, rule);
            } else { /* a MIME type: its usual extensions */
                GList *exts = NULL;
                gchar *ct = g_content_type_from_mime_type(rule);
                (void)ct;
                g_free(ct);
                g_list_free(exts);
                json_builder_add_string_value(b, rule);
            }
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
}

static void file_request(GDBusMethodInvocation *inv, const char *mode, GVariant *params) {
    const gchar *handle, *app_id, *parent, *title;
    GVariant *options;
    g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &options);
    if (!shell) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@a{sv})", 2, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)));
        g_variant_unref(options);
        return;
    }
    guint id = next_id++;
    g_hash_table_insert(pending, GUINT_TO_POINTER(id), inv);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, id);
    json_builder_set_member_name(b, "op");
    json_builder_add_string_value(b, "file");
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, mode);
    json_builder_set_member_name(b, "app");
    json_builder_add_string_value(b, app_id);
    json_builder_set_member_name(b, "parent");
    json_builder_add_string_value(b, parent);
    json_builder_set_member_name(b, "title");
    json_builder_add_string_value(b, title);
    gboolean flag;
    const gchar *str;
    GVariant *v;
    if (g_variant_lookup(options, "multiple", "b", &flag)) {
        json_builder_set_member_name(b, "multiple");
        json_builder_add_boolean_value(b, flag);
    }
    if (g_variant_lookup(options, "directory", "b", &flag)) {
        json_builder_set_member_name(b, "directory");
        json_builder_add_boolean_value(b, flag);
    }
    if (g_variant_lookup(options, "accept_label", "&s", &str)) {
        json_builder_set_member_name(b, "accept");
        json_builder_add_string_value(b, str);
    }
    if (g_variant_lookup(options, "current_name", "&s", &str)) {
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, str);
    }
    if ((v = g_variant_lookup_value(options, "current_folder", G_VARIANT_TYPE_BYTESTRING))) {
        gchar *p = bytes_path(v);
        if (p) {
            json_builder_set_member_name(b, "folder");
            json_builder_add_string_value(b, p);
        }
        g_free(p);
        g_variant_unref(v);
    }
    if ((v = g_variant_lookup_value(options, "current_file", G_VARIANT_TYPE_BYTESTRING))) {
        gchar *p = bytes_path(v);
        if (p) {
            json_builder_set_member_name(b, "file");
            json_builder_add_string_value(b, p);
        }
        g_free(p);
        g_variant_unref(v);
    }
    if ((v = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))")))) {
        json_builder_set_member_name(b, "filters");
        add_filters(b, v);
        g_variant_unref(v);
    }
    json_builder_end_object(b);
    shell_send(b);
    g_variant_unref(options);
}

/* ---- Settings ---- */

static GVariant *appearance(const char *key) {
    if (!strcmp(key, "color-scheme"))
        return g_variant_new_uint32(scheme);
    if (!strcmp(key, "accent-color"))
        return accent_value();
    if (!strcmp(key, "contrast"))
        return g_variant_new_uint32(contrast);
    return NULL;
}

static gboolean wanted(const gchar *const *namespaces, const char *ns) {
    if (!namespaces || !namespaces[0])
        return TRUE;
    for (int i = 0; namespaces[i]; i++) {
        size_t l = strlen(namespaces[i]);
        if (!strcmp(namespaces[i], ns) || (l && namespaces[i][l - 1] == '*' && !strncmp(namespaces[i], ns, l - 1)))
            return TRUE;
    }
    return FALSE;
}

/* ---- calls ---- */

static void on_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method,
                    GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void)c, (void)sender, (void)path, (void)data;
    if (!strcmp(iface, "org.freedesktop.impl.portal.FileChooser")) {
        file_request(inv, !strcmp(method, "OpenFile") ? "open" : !strcmp(method, "SaveFile") ? "save" : "saves", params);
        return;
    }
    if (!strcmp(iface, "org.freedesktop.impl.portal.Settings")) {
        if (!strcmp(method, "ReadAll")) {
            const gchar **ns = NULL;
            g_variant_get(params, "(^a&s)", &ns);
            GVariantBuilder all;
            g_variant_builder_init(&all, G_VARIANT_TYPE("a{sa{sv}}"));
            if (wanted(ns, "org.freedesktop.appearance")) {
                GVariantBuilder app;
                g_variant_builder_init(&app, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&app, "{sv}", "color-scheme", appearance("color-scheme"));
                g_variant_builder_add(&app, "{sv}", "accent-color", appearance("accent-color"));
                g_variant_builder_add(&app, "{sv}", "contrast", appearance("contrast"));
                g_variant_builder_add(&all, "{s@a{sv}}", "org.freedesktop.appearance", g_variant_builder_end(&app));
            }
            g_free(ns);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sa{sv}})", &all));
        } else {
            const gchar *ns, *key;
            g_variant_get(params, "(&s&s)", &ns, &key);
            GVariant *v = !strcmp(ns, "org.freedesktop.appearance") ? appearance(key) : NULL;
            if (v)
                g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
            else
                g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                                                      "No such setting: %s %s", ns, key);
        }
        return;
    }
    if (!strcmp(iface, "org.freedesktop.impl.portal.Notification")) {
        const gchar *app, *id;
        GVariant *n = NULL;
        if (!strcmp(method, "AddNotification"))
            g_variant_get(params, "(&s&s@a{sv})", &app, &id, &n);
        else
            g_variant_get(params, "(&s&s)", &app, &id);
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "op");
        json_builder_add_string_value(b, n ? "notify" : "unnotify");
        json_builder_set_member_name(b, "app");
        json_builder_add_string_value(b, app);
        json_builder_set_member_name(b, "id");
        json_builder_add_string_value(b, id);
        const gchar *str;
        if (n && g_variant_lookup(n, "title", "&s", &str)) {
            json_builder_set_member_name(b, "title");
            json_builder_add_string_value(b, str);
        }
        if (n && g_variant_lookup(n, "body", "&s", &str)) {
            json_builder_set_member_name(b, "body");
            json_builder_add_string_value(b, str);
        }
        json_builder_end_object(b);
        shell_send(b);
        if (n)
            g_variant_unref(n);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (!strcmp(iface, "org.freedesktop.impl.portal.AppChooser")) {
        if (!strcmp(method, "UpdateChoices")) {
            g_dbus_method_invocation_return_value(inv, NULL);
            return;
        }
        /* the first (the default handler); a chooser may come later */
        const gchar *handle, *app_id, *parent;
        const gchar **choices = NULL;
        GVariant *opts;
        g_variant_get(params, "(&o&s&s^a&s@a{sv})", &handle, &app_id, &parent, &choices, &opts);
        GVariantBuilder r;
        g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
        guint response = 1;
        if (choices && choices[0]) {
            g_variant_builder_add(&r, "{sv}", "choice", g_variant_new_string(choices[0]));
            response = 0;
        } else {
            /* nothing installed opens it: the shell says so and offers what could */
            const gchar *uri = NULL, *type = NULL;
            g_variant_lookup(opts, "uri", "&s", &uri);
            g_variant_lookup(opts, "content_type", "&s", &type);
            JsonBuilder *b = json_builder_new();
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "op");
            json_builder_add_string_value(b, "nohandler");
            json_builder_set_member_name(b, "app");
            json_builder_add_string_value(b, app_id);
            json_builder_set_member_name(b, "uri");
            json_builder_add_string_value(b, uri ? uri : "");
            json_builder_set_member_name(b, "type");
            json_builder_add_string_value(b, type ? type : "");
            json_builder_end_object(b);
            shell_send(b);
        }
        g_free(choices);
        g_variant_unref(opts);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ua{sv})", response, &r));
        return;
    }
    if (!strcmp(iface, "org.freedesktop.impl.portal.Inhibit")) {
        g_dbus_method_invocation_return_value(inv, NULL); /* nothing sleeps here yet */
        return;
    }
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method);
}

static GVariant *on_get(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *prop, GError **err, gpointer data) {
    (void)c, (void)sender, (void)path, (void)err, (void)data, (void)prop;
    if (!strcmp(iface, "org.freedesktop.impl.portal.Settings"))
        return g_variant_new_uint32(2);
    if (!strcmp(iface, "org.freedesktop.impl.portal.FileChooser"))
        return g_variant_new_uint32(4);
    if (!strcmp(iface, "org.freedesktop.impl.portal.AppChooser"))
        return g_variant_new_uint32(2);
    return g_variant_new_uint32(1);
}

static const GDBusInterfaceVTable vtable = {on_call, on_get, NULL, {0}};

static void on_bus(GDBusConnection *c, const gchar *name, gpointer data) {
    (void)name, (void)data;
    bus = c;
    for (int i = 0; node->interfaces[i]; i++)
        g_dbus_connection_register_object(c, OBJ_PATH, node->interfaces[i], &vtable, NULL, NULL, NULL);
}

static void on_name(GDBusConnection *c, const gchar *name, gpointer data) {
    (void)c, (void)data;
    g_message("os-portal: %s is ours", name);
}

static void on_lost(GDBusConnection *c, const gchar *name, gpointer data) {
    (void)c, (void)data;
    g_warning("os-portal: lost %s", name);
}

int main(void) {
    node = g_dbus_node_info_new_for_xml(introspection, NULL);
    pending = g_hash_table_new(g_direct_hash, g_direct_equal);
    shell_connect();
    g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_REPLACE, on_bus, on_name, on_lost, NULL, NULL);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
