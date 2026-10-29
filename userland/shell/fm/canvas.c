#define _GNU_SOURCE
#include "fmi.h"

#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#include "../sys.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* <canvas>: the escape hatch for things markup can't say. The AI writes a
 * small Lua script defining draw(w, h, t); it runs every frame inside a
 * sandbox (no files, no OS, a strict instruction budget) and paints with a
 * handful of primitives. Optional click(x, y) receives clicks. */

#define BUDGET 400000 /* Lua instructions per call */

struct cv {
    fm_node *n;
    lua_State *L;
    bool broken;
    char error[160];
};

static struct cv cvs[16];
static canvas *cur_c;
static rect cur_r;
static float cur_s, cur_op;
static fm_doc *cur_d;

static struct cv *find_cv(fm_node *n, bool create) {
    for (int i = 0; i < 16; i++)
        if (cvs[i].n == n)
            return &cvs[i];
    if (!create)
        return NULL;
    for (int i = 0; i < 16; i++)
        if (!cvs[i].n) {
            memset(&cvs[i], 0, sizeof cvs[i]);
            cvs[i].n = n;
            return &cvs[i];
        }
    return NULL;
}

void fmi_canvas_forget(fm_node *n) {
    struct cv *c = find_cv(n, false);
    if (c) {
        if (c->L)
            lua_close(c->L);
        memset(c, 0, sizeof *c);
    }
}

static void budget_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "the script took too long");
}

static color arg_color(lua_State *L, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) {
        lua_Integer v = lua_tointeger(L, i);
        color c = (color)v;
        if (v <= 0xffffff)
            c |= 0xff000000u;
        return color_alpha(c, cur_op);
    }
    const char *s = luaL_optstring(L, i, "#ffffff");
    color c = 0xffffffffu;
    if (!strncmp(s, "--", 2) && cur_d) {
        const char *v = fm_var(cur_d, s);
        if (v)
            s = v;
    }
    fm_parse_color(s, &c);
    return color_alpha(c, cur_op);
}

static float X(lua_State *L, int i) { return cur_r.x + (float)luaL_checknumber(L, i) * cur_s; }
static float Y(lua_State *L, int i) { return cur_r.y + (float)luaL_checknumber(L, i) * cur_s; }
static float S(lua_State *L, int i) { return (float)luaL_checknumber(L, i) * cur_s; }

static int l_rect(lua_State *L) {
    rect r = {(int)X(L, 1), (int)Y(L, 2), (int)S(L, 3), (int)S(L, 4)};
    fill_round(cur_c, r, (float)luaL_optnumber(L, 6, 0) * cur_s, arg_color(L, 5));
    return 0;
}

static int l_gradient(lua_State *L) {
    rect r = {(int)X(L, 1), (int)Y(L, 2), (int)S(L, 3), (int)S(L, 4)};
    fill_round_vgradient(cur_c, r, (float)luaL_optnumber(L, 7, 0) * cur_s, arg_color(L, 5), arg_color(L, 6));
    return 0;
}

static int l_circle(lua_State *L) {
    fill_circle(cur_c, X(L, 1), Y(L, 2), S(L, 3), arg_color(L, 4));
    return 0;
}

static int l_glow(lua_State *L) {
    glow_circle(cur_c, X(L, 1), Y(L, 2), S(L, 3), arg_color(L, 4));
    return 0;
}

static int l_line(lua_State *L) {
    draw_line(cur_c, X(L, 1), Y(L, 2), X(L, 3), Y(L, 4), (float)luaL_optnumber(L, 6, 1.5) * cur_s, arg_color(L, 5));
    return 0;
}

static int l_arc(lua_State *L) {
    draw_arc(cur_c, X(L, 1), Y(L, 2), S(L, 3), (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5),
             (float)luaL_optnumber(L, 7, 2) * cur_s, arg_color(L, 6));
    return 0;
}

static int l_text(lua_State *L) {
    const char *s = luaL_checkstring(L, 3);
    float size = (float)luaL_optnumber(L, 5, 16) * cur_s;
    const char *fam = luaL_optstring(L, 6, "Inter");
    draw_text(cur_c, fm_host_font(fam, size), (int)X(L, 1), (int)Y(L, 2), s, arg_color(L, 4));
    return 0;
}

/* g.poly({x0,y0, x1,y1, ...}, colour): a filled shape (3D faces, maps). */
static int l_poly(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)lua_rawlen(L, 1) / 2;
    if (n > 256)
        n = 256;
    float xy[512];
    for (int i = 0; i < n * 2; i++) {
        lua_rawgeti(L, 1, i + 1);
        float v = (float)lua_tonumber(L, -1) * cur_s;
        lua_pop(L, 1);
        xy[i] = v + (i % 2 ? cur_r.y : cur_r.x);
    }
    fill_polygon(cur_c, xy, n, arg_color(L, 2));
    return 0;
}

/* g.pointer(): where the pointer is, in the canvas's own units, and
 * whether it is over the canvas. */
static int l_pointer(lua_State *L) {
    int mx = cur_d && cur_d->in ? cur_d->in->mx : -1, my = cur_d && cur_d->in ? cur_d->in->my : -1;
    lua_pushnumber(L, (mx - cur_r.x) / cur_s);
    lua_pushnumber(L, (my - cur_r.y) / cur_s);
    lua_pushboolean(L, mx >= cur_r.x && my >= cur_r.y && mx < cur_r.x + cur_r.w && my < cur_r.y + cur_r.h);
    return 3;
}

/* g.apps(): { {id=, name=}, ... }, every app on this computer. */
static int l_apps(lua_State *L) {
    int n = fm_host_app_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, fm_host_app_id(i));
        lua_setfield(L, -2, "id");
        lua_pushstring(L, fm_host_app_label(i));
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* g.files(path): what a folder holds ("~" or nothing: home), folders first:
 * { {name=, path=, dir=true|false, size=bytes}, ... }. Read at most every
 * two seconds per folder, so drawing it every frame costs nothing. */
#define FILES_MAX 200
struct fentry {
    char name[96];
    bool dir;
    long long size;
};
static struct fcache {
    char path[256];
    uint64_t at;
    int n;
    struct fentry e[FILES_MAX];
} fcache[4];

static int by_kind_name(const void *a, const void *b) {
    const struct fentry *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int l_files(lua_State *L) {
    const char *want = luaL_optstring(L, 1, "~");
    /* the person's home (the desktop itself may run with HOME=/) */
    const char *home = getenv("HOME");
    if (!home || !*home || !strcmp(home, "/"))
        home = "/root";
    char path[256];
    if (!strcmp(want, "~") || !*want)
        snprintf(path, sizeof path, "%s", home);
    else if (!strncmp(want, "~/", 2))
        snprintf(path, sizeof path, "%s/%s", home, want + 2);
    else
        snprintf(path, sizeof path, "%s", want);
    uint64_t now = fm_host_time_ms();
    struct fcache *fc = NULL, *oldest = &fcache[0];
    for (int i = 0; i < 4; i++) {
        if (!strcmp(fcache[i].path, path))
            fc = &fcache[i];
        if (fcache[i].at < oldest->at)
            oldest = &fcache[i];
    }
    if (!fc || now - fc->at > 2000) {
        if (!fc) {
            fc = oldest;
            snprintf(fc->path, sizeof fc->path, "%s", path);
        }
        fc->at = now ? now : 1;
        fc->n = 0;
        DIR *dir = opendir(path);
        struct dirent *de;
        while (dir && (de = readdir(dir)) && fc->n < FILES_MAX) {
            if (de->d_name[0] == '.')
                continue; /* hidden, and . and .. */
            struct fentry *e = &fc->e[fc->n];
            char full[512];
            snprintf(full, sizeof full, "%s/%s", path, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0)
                continue;
            snprintf(e->name, sizeof e->name, "%s", de->d_name);
            e->dir = S_ISDIR(st.st_mode);
            e->size = (long long)st.st_size;
            fc->n++;
        }
        if (dir)
            closedir(dir);
        qsort(fc->e, (size_t)fc->n, sizeof fc->e[0], by_kind_name);
    }
    lua_createtable(L, fc->n, 0);
    for (int i = 0; i < fc->n; i++) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", fc->path, fc->e[i].name);
        lua_createtable(L, 0, 4);
        lua_pushstring(L, fc->e[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, full);
        lua_setfield(L, -2, "path");
        lua_pushboolean(L, fc->e[i].dir);
        lua_setfield(L, -2, "dir");
        lua_pushnumber(L, (lua_Number)fc->e[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* g.clock(): the local time of day as hours (14.5 is half past two), then
 * the weekday (0 Sunday), the day of the month and the month (1-12). */
static int l_clock(lua_State *L) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lua_pushnumber(L, tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0);
    lua_pushinteger(L, tm.tm_wday);
    lua_pushinteger(L, tm.tm_mday);
    lua_pushinteger(L, tm.tm_mon + 1);
    return 4;
}

static int l_var(lua_State *L) {
    const char *v = cur_d ? fm_var(cur_d, luaL_checkstring(L, 1)) : NULL;
    lua_pushstring(L, v ? v : "");
    return 1;
}

static const luaL_Reg api[] = {{"rect", l_rect}, {"gradient", l_gradient}, {"circle", l_circle}, {"glow", l_glow},
                               {"line", l_line}, {"arc", l_arc},           {"text", l_text},     {"var", l_var},
                               {"poly", l_poly}, {"pointer", l_pointer},   {"apps", l_apps},     {"files", l_files},
                               {"clock", l_clock},
                               {NULL, NULL}};

static bool start(fm_doc *d, fm_node *n, struct cv *c) {
    char *src = fm_text_of(n);
    const char *attr = fm_attr_get(n, "script");
    c->L = luaL_newstate();
    lua_State *L = c->L;
    /* only safe libraries: no io, os, package, debug */
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 4);
    static const char *const unsafe[] = {"dofile", "loadfile", "load", "require", "collectgarbage", "rawset"};
    for (size_t i = 0; i < sizeof unsafe / sizeof unsafe[0]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe[i]);
    }
    luaL_newlib(L, api);
    lua_setglobal(L, "g");
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, BUDGET);
    int r = luaL_loadbuffer(L, attr ? attr : src, strlen(attr ? attr : src), "canvas") || lua_pcall(L, 0, 0, 0);
    free(src);
    if (r) {
        snprintf(c->error, sizeof c->error, "%s", lua_tostring(L, -1));
        fm_report(d, n->line, false, "canvas script: %s", c->error);
        fprintf(stderr, "fm: canvas (line %d) won't start: %s\n", n->line, c->error);
        c->broken = true;
        return false;
    }
    return true;
}

void fmi_canvas_draw(fm_doc *d, canvas *cv, fm_node *n, rect r, float op) {
    struct cv *c = find_cv(n, true);
    if (!c || c->broken)
        return;
    if (!c->L && !start(d, n, c))
        return;
    lua_State *L = c->L;
    lua_getglobal(L, "draw");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    cur_c = cv, cur_r = r, cur_s = n->vscale > 0 ? n->vscale : 1, cur_op = op, cur_d = d;
    lua_pushnumber(L, r.w / cur_s);
    lua_pushnumber(L, r.h / cur_s);
    lua_pushnumber(L, (double)fm_host_time_ms() / 1000.0);
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, BUDGET); /* the budget is per frame, not per lifetime */
    if (lua_pcall(L, 3, 0, 0)) {
        snprintf(c->error, sizeof c->error, "%s", lua_tostring(L, -1));
        fm_report(d, n->line, false, "canvas draw: %s", c->error);
        fprintf(stderr, "fm: canvas (line %d) stopped: %s\n", n->line, c->error);
        c->broken = true;
    }
}

bool fmi_canvas_click(fm_doc *d, fm_node *n, int x, int y) {
    struct cv *c = find_cv(n, false);
    if (!c || !c->L || c->broken)
        return false;
    lua_getglobal(c->L, "click");
    if (!lua_isfunction(c->L, -1)) {
        lua_pop(c->L, 1);
        return false;
    }
    float s = n->vscale > 0 ? n->vscale : 1;
    lua_pushnumber(c->L, (x - n->vis.x) / s);
    lua_pushnumber(c->L, (y - n->vis.y) / s);
    lua_sethook(c->L, budget_hook, LUA_MASKCOUNT, BUDGET);
    if (lua_pcall(c->L, 2, 1, 0)) {
        fm_report(d, n->line, false, "canvas click: %s", lua_tostring(c->L, -1));
        c->broken = true;
        return false;
    }
    /* click may return an action: "open:files" */
    if (lua_isstring(c->L, -1))
        fm_host_action(d, n, lua_tostring(c->L, -1));
    lua_pop(c->L, 1);
    return true;
}
