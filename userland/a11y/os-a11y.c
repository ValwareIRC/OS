/* os-a11y: what has focus in other apps (Firefox, Prism, any GTK, Qt or Chromium app), for the screen reader in the
 * desktop (userland/shell/reader.c). Apps tell the accessibility bus (AT-SPI, at-spi2-core in the base system)
 * what they are made of and what changes in them; this listens there and writes one JSON line per event to its
 * standard output; the desktop writes commands to its standard input:
 *   where        what has focus now, again, with where it is (window, app)
 *   describe     the focused window's contents, for the companion to describe in its own words
 *   quit
 * Events: {"ev":"focus","app","window","role","name","desc","states":[...],"value","text","index","of"},
 *         {"ev":"caret","line"}, {"ev":"window","app","name"}, {"ev":"tree","lines":[...]}, {"ev":"ready"}.
 * It also says, on the bus, that a screen reader is running (org.a11y.Status): apps only expose themselves then.
 * Built with the base system (scripts/build-base.sh), against the Freedesktop SDK. */
#include <atspi/atspi.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static AtspiAccessible *focused; /* (a reference held) */

static void put_json_string(GString *o, const char *s) {
    g_string_append_c(o, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(o, '\\'), g_string_append_c(o, (char)*p);
        else if (*p == '\n')
            g_string_append(o, "\\n");
        else if (*p < 0x20)
            g_string_append_printf(o, "\\u%04x", *p);
        else
            g_string_append_c(o, (char)*p);
    }
    g_string_append_c(o, '"');
}

static void field(GString *o, const char *key, const char *val) {
    g_string_append_printf(o, ",\"%s\":", key);
    put_json_string(o, val);
}

static void emit(GString *o) {
    g_string_append(o, "}\n");
    fputs(o->str, stdout);
    fflush(stdout);
    g_string_free(o, TRUE);
}

static char *name_of(AtspiAccessible *a) {
    char *n = a ? atspi_accessible_get_name(a, NULL) : NULL;
    return n ? n : g_strdup("");
}

/* The window it is in: the nearest frame, dialog or window above it. */
static char *window_of(AtspiAccessible *a) {
    AtspiAccessible *p = a ? g_object_ref(a) : NULL;
    for (int depth = 0; p && depth < 40; depth++) {
        AtspiRole r = atspi_accessible_get_role(p, NULL);
        if (r == ATSPI_ROLE_FRAME || r == ATSPI_ROLE_DIALOG || r == ATSPI_ROLE_WINDOW || r == ATSPI_ROLE_ALERT) {
            char *n = name_of(p);
            g_object_unref(p);
            return n;
        }
        AtspiAccessible *up = atspi_accessible_get_parent(p, NULL);
        g_object_unref(p);
        p = up;
    }
    if (p)
        g_object_unref(p);
    return g_strdup("");
}

static void describe_focus(AtspiAccessible *a, const char *ev) {
    if (!a)
        return;
    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "{\"ev\":\"%s\"", ev);
    AtspiAccessible *app = atspi_accessible_get_application(a, NULL);
    char *s = name_of(app);
    field(o, "app", s);
    g_free(s);
    if (app)
        g_object_unref(app);
    s = window_of(a);
    field(o, "window", s);
    g_free(s);
    s = atspi_accessible_get_role_name(a, NULL);
    field(o, "role", s ? s : "");
    g_free(s);
    s = name_of(a);
    field(o, "name", s);
    g_free(s);
    s = atspi_accessible_get_description(a, NULL);
    field(o, "desc", s ? s : "");
    g_free(s);
    /* what it is now: on or off, open or shut, can't be used, can be typed in... */
    AtspiStateSet *ss = atspi_accessible_get_state_set(a);
    static const struct {
        AtspiStateType t;
        const char *w;
    } st[] = {{ATSPI_STATE_CHECKED, "checked"},     {ATSPI_STATE_PRESSED, "pressed"},
              {ATSPI_STATE_SELECTED, "selected"},   {ATSPI_STATE_EXPANDED, "expanded"},
              {ATSPI_STATE_COLLAPSED, "collapsed"}, {ATSPI_STATE_EDITABLE, "editable"},
              {ATSPI_STATE_REQUIRED, "required"},   {ATSPI_STATE_INVALID_ENTRY, "invalid"},
              {ATSPI_STATE_READ_ONLY, "read-only"}, {ATSPI_STATE_MULTI_LINE, "multi-line"},
              {ATSPI_STATE_VISITED, "visited"},     {ATSPI_STATE_BUSY, "busy"}};
    g_string_append(o, ",\"states\":[");
    bool first = true;
    for (size_t i = 0; ss && i < G_N_ELEMENTS(st); i++)
        if (atspi_state_set_contains(ss, st[i].t)) {
            g_string_append_printf(o, "%s\"%s\"", first ? "" : ",", st[i].w);
            first = false;
        }
    if (ss && !atspi_state_set_contains(ss, ATSPI_STATE_SENSITIVE) && !atspi_state_set_contains(ss, ATSPI_STATE_EDITABLE))
        g_string_append_printf(o, "%s\"unavailable\"", first ? "" : ",");
    g_string_append(o, "]");
    if (ss)
        g_object_unref(ss);
    AtspiValue *v = atspi_accessible_get_value_iface(a);
    if (v) {
        char num[64];
        g_snprintf(num, sizeof num, "%g of %g to %g", atspi_value_get_current_value(v, NULL),
                   atspi_value_get_minimum_value(v, NULL), atspi_value_get_maximum_value(v, NULL));
        field(o, "value", num);
        g_object_unref(v);
    }
    AtspiText *t = atspi_accessible_get_text_iface(a);
    if (t) {
        gint n = atspi_text_get_character_count(t, NULL);
        char *txt = n > 0 ? atspi_text_get_text(t, 0, n < 300 ? n : 300, NULL) : NULL;
        field(o, "text", txt ? txt : "");
        g_free(txt);
        g_object_unref(t);
    }
    gint idx = atspi_accessible_get_index_in_parent(a, NULL);
    AtspiAccessible *parent = atspi_accessible_get_parent(a, NULL);
    gint of = parent ? atspi_accessible_get_child_count(parent, NULL) : 0;
    if (parent)
        g_object_unref(parent);
    g_string_append_printf(o, ",\"index\":%d,\"of\":%d", idx + 1, of);
    emit(o);
}

static void on_focus(AtspiEvent *e, void *data) {
    (void)data;
    if (!e->detail1 || !e->source)
        return;
    if (focused)
        g_object_unref(focused);
    focused = g_object_ref(e->source);
    describe_focus(focused, "focus");
}

static void on_state(AtspiEvent *e, void *data) {
    (void)data; /* checked, selected, expanded on what has focus: said again */
    if (focused && e->source == focused)
        describe_focus(focused, "changed");
}

static void on_caret(AtspiEvent *e, void *data) {
    (void)data;
    if (!focused || e->source != focused)
        return;
    AtspiText *t = atspi_accessible_get_text_iface(e->source);
    if (!t)
        return;
    gint at = atspi_text_get_caret_offset(t, NULL);
    AtspiTextRange *r = atspi_text_get_string_at_offset(t, at, ATSPI_TEXT_GRANULARITY_LINE, NULL);
    GString *o = g_string_new("{\"ev\":\"caret\"");
    field(o, "line", r && r->content ? r->content : "");
    g_string_append_printf(o, ",\"at\":%d", r ? at - r->start_offset : 0);
    emit(o);
    if (r)
        g_free(r->content), g_free(r);
    g_object_unref(t);
}

static void on_window(AtspiEvent *e, void *data) {
    (void)data;
    if (!e->source)
        return;
    GString *o = g_string_new("{\"ev\":\"window\"");
    AtspiAccessible *app = atspi_accessible_get_application(e->source, NULL);
    char *s = name_of(app);
    field(o, "app", s);
    g_free(s);
    if (app)
        g_object_unref(app);
    s = name_of(e->source);
    field(o, "name", s);
    g_free(s);
    emit(o);
}

static void on_announce(AtspiEvent *e, void *data) {
    (void)data; /* an app saying something to whoever reads for them ("Download finished") */
    const char *s = G_VALUE_HOLDS_STRING(&e->any_data) ? g_value_get_string(&e->any_data) : NULL;
    if (!s || !*s)
        return;
    GString *o = g_string_new("{\"ev\":\"announce\"");
    field(o, "text", s);
    emit(o);
}

/* The focused window's contents, line by line ("push button: Reload"), for a fuller description. */
static void tree(AtspiAccessible *a, int depth, GString *o, int *n) {
    if (!a || depth > 8 || *n > 250)
        return;
    char *name = name_of(a), *role = atspi_accessible_get_role_name(a, NULL);
    if (name[0] || depth < 2) {
        g_string_append(o, *n ? "," : "");
        GString *l = g_string_new(NULL);
        g_string_append_printf(l, "%*s%s: %s", depth * 2, "", role ? role : "", name);
        put_json_string(o, l->str);
        g_string_free(l, TRUE);
        (*n)++;
    }
    g_free(name);
    g_free(role);
    gint k = atspi_accessible_get_child_count(a, NULL);
    for (gint i = 0; i < k && i < 80; i++) {
        AtspiAccessible *c = atspi_accessible_get_child_at_index(a, i, NULL);
        if (c) {
            AtspiStateSet *ss = atspi_accessible_get_state_set(c);
            bool shown = !ss || atspi_state_set_contains(ss, ATSPI_STATE_SHOWING);
            if (ss)
                g_object_unref(ss);
            if (shown)
                tree(c, depth + 1, o, n);
            g_object_unref(c);
        }
    }
}

static void describe_window(void) {
    AtspiAccessible *w = focused ? g_object_ref(focused) : NULL;
    for (int depth = 0; w && depth < 40; depth++) {
        AtspiRole r = atspi_accessible_get_role(w, NULL);
        if (r == ATSPI_ROLE_FRAME || r == ATSPI_ROLE_DIALOG || r == ATSPI_ROLE_WINDOW)
            break;
        AtspiAccessible *up = atspi_accessible_get_parent(w, NULL);
        g_object_unref(w);
        w = up;
    }
    GString *o = g_string_new("{\"ev\":\"tree\",\"lines\":[");
    int n = 0;
    tree(w, 0, o, &n);
    g_string_append(o, "]");
    if (w)
        g_object_unref(w);
    emit(o);
}

static gboolean on_command(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)data;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        atspi_event_quit();
        return FALSE;
    }
    char *line = NULL;
    gsize len = 0;
    if (g_io_channel_read_line(ch, &line, &len, NULL, NULL) != G_IO_STATUS_NORMAL) {
        g_free(line);
        atspi_event_quit();
        return FALSE;
    }
    g_strstrip(line);
    if (!strcmp(line, "where") && focused)
        describe_focus(focused, "where");
    else if (!strcmp(line, "describe"))
        describe_window();
    else if (!strcmp(line, "quit"))
        atspi_event_quit();
    g_free(line);
    return TRUE;
}

/* org.a11y.Status: a screen reader runs (apps turn their accessibility on); off again when this ends. */
static void say_reader_runs(gboolean on) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    static const char *const props[] = {"IsEnabled", "ScreenReaderEnabled"};
    for (size_t i = 0; i < G_N_ELEMENTS(props); i++) {
        GVariant *r = g_dbus_connection_call_sync(
            bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.a11y.Status", props[i], g_variant_new_boolean(on)), NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
        if (r)
            g_variant_unref(r);
    }
    g_object_unref(bus);
}

int main(void) {
    say_reader_runs(TRUE); /* (first: the bus starts, apps begin to expose themselves) */
    if (atspi_init() < 0) {
        puts("{\"ev\":\"error\",\"text\":\"the accessibility bus couldn't be reached\"}");
        return 1;
    }
    static const struct {
        const char *ev;
        AtspiEventListenerCB cb;
    } on[] = {{"object:state-changed:focused", on_focus}, {"object:state-changed:checked", on_state},
              {"object:state-changed:selected", on_state}, {"object:state-changed:expanded", on_state},
              {"object:text-caret-moved", on_caret},      {"window:activate", on_window},
              {"object:announcement", on_announce}};
    for (size_t i = 0; i < G_N_ELEMENTS(on); i++) {
        AtspiEventListener *l = atspi_event_listener_new(on[i].cb, NULL, NULL);
        atspi_event_listener_register(l, on[i].ev, NULL);
    }
    GIOChannel *in = g_io_channel_unix_new(0);
    g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, on_command, NULL);
    puts("{\"ev\":\"ready\"}");
    fflush(stdout);
    atspi_event_main();
    say_reader_runs(FALSE);
    return 0;
}
