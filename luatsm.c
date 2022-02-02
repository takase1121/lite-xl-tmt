#include <string.h>
#include <errno.h>

#include <lua.h>
#include <lauxlib.h>

#include <libtsm.h>

#define LUA_ASSERT(x, ...) if (!(x)) return luaL_error(L, __VA_ARGS__)
#define LUA_THROW(r, fmt) return luaL_error(L, fmt, strerror(r))
#define LUA_TSETI(K, V) (lua_pushinteger(L, V), lua_setfield(L, -2, K))
#define LUA_TPUSH(I) (lua_rawseti(L, I, lua_rawlen(L, I) + 1))

#define API_TYPE_VTS "VTState"

typedef struct {
    struct tsm_screen *screen;
    struct tsm_vte *vte;
    lua_State *L;
    int data, draw_cb;
    uint8_t tr, tg, tb, rr, rg, rb;
    unsigned int tx, ty, ti, rx, ry, rw;
    int rect, textrun;
    char *buf;
} vts_t;

static const char *sev2str_table[] = {
    "FATAL",
    "ALERT",
    "CRITICAL",
    "ERROR",
    "WARNING",
    "NOTICE",
    "INFO",
    "DEBUG"
};

static const char *sev2str(unsigned int sev) {
    if (sev > 7)
        return "UNKNOWN";
    return sev2str_table[sev];
}

static void log_cb(void *data,
                    const char *file,
                    int line,
                    const char *func,
                    const char *subs,
                    unsigned int sev,
                    const char *format,
                    va_list args) {
    fprintf(stderr, "%s: %s", sev2str(sev), subs ? subs : "unknown");
    vfprintf(stderr, format, args);
    fputs("", stderr);
}

static void write_cb(struct tsm_vte *vte,
                        const char *u8,
                        size_t len,
                        void *data) {
    vts_t *vt = (vts_t *) data;
    lua_State *L = vt->L;
    if (L == NULL) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, vt->data);
    if (!lua_istable(L, -1)) return;

    lua_pushlstring(L, u8, len);
    LUA_TPUSH(-2);
}

static int f_new(lua_State *L) {
    int rows = luaL_optinteger(L, 1, 24);
    int cols = luaL_optinteger(L, 2, 80);
    int sb = luaL_optinteger(L, 3, 0);

    LUA_ASSERT(rows > 0 && cols > 0, "rows and columns must be positive");
    LUA_ASSERT(sb >= 0, "scrollback size must be positive");

    vts_t *vt = lua_newuserdata(L, sizeof(vts_t));
    luaL_setmetatable(L, API_TYPE_VTS);

    memset(vt, 0, sizeof(vts_t));
    vt->L = L;
    vt->data = vt->draw_cb = LUA_NOREF;
    vt->buf = malloc(rows * cols * 4 * sizeof(char));
    if (vt->buf == NULL)
        LUA_THROW(ENOMEM, "cannot allocate memory for line buffer: %s");

    int r;
    r = tsm_screen_new(&vt->screen, log_cb, NULL);
    if (r < 0) goto ERR_SCREEN;

    r = tsm_screen_resize(vt->screen, cols, rows);
    if (r < 0) goto FREE_SCREEN;

    vt->buf = malloc(rows * cols * 4 * sizeof(char));
    if (vt->buf == NULL) goto FREE_BUF;

    tsm_screen_set_max_sb(vt->screen, sb);

    r = tsm_vte_new(&vt->vte, vt->screen, write_cb, vt, log_cb, NULL);
    if (r < 0) goto FREE_BUF;

    return 1;

FREE_BUF:
    free(vt->buf);
FREE_SCREEN:
    tsm_screen_unref(vt->screen);
ERR_SCREEN:
    LUA_THROW(-r, "cannot create virtual terminal: %s");
}

static int f_get_size(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    lua_pushinteger(L, tsm_screen_get_height(vt->screen));
    lua_pushinteger(L, tsm_screen_get_width(vt->screen));
    return 2;
}

static int f_set_size(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    int rows = luaL_checkinteger(L, 2);
    int cols = luaL_checkinteger(L, 3);

    int r = tsm_screen_resize(vt->screen, cols, rows);
    if (r < 0) LUA_THROW(-r, "cannot resize virtual terminal: %s");

    vt->ti = 0;
    vt->buf = realloc(vt->buf, rows * cols * 4 * sizeof(char));
    if (vt->buf == NULL) LUA_THROW(ENOMEM, "cannot resize line buffer: %s");

    return 0;
}

static int f_get_cursor(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    lua_pushinteger(L, tsm_screen_get_cursor_y(vt->screen) + 1);
    lua_pushinteger(L, tsm_screen_get_cursor_x(vt->screen) + 1);
    return 2;
}

static int f_write(lua_State *L) {
    size_t len;
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    const char *str = luaL_checklstring(L, 2, &len);

    // replace the current answer table with a blank one
    lua_createtable(L, 16, 0); // just a guess
    vt->data = luaL_ref(L, LUA_REGISTRYINDEX);

    tsm_vte_input(vt->vte, str, len);

    lua_rawgeti(L, LUA_REGISTRYINDEX, vt->data);
    luaL_unref(L, LUA_REGISTRYINDEX, vt->data); vt->data = LUA_NOREF;
    return 1;
}

static void add_rect(vts_t *vt) {
    if (vt->rw == 0) return;
    lua_State *L = vt->L;

    lua_newtable(L);
    LUA_TSETI("x", vt->rx); LUA_TSETI("y", vt->ry); LUA_TSETI("w", vt->rw);
    LUA_TSETI("r", vt->rr);
    LUA_TSETI("g", vt->rg);
    LUA_TSETI("b", vt->rb);
    LUA_TPUSH(vt->rect);

    vt->rw = 0;
}

static void add_textrun(vts_t *vt) {
    if (vt->ti == 0) return;
    lua_State *L = vt->L;

    lua_newtable(L);
    lua_pushlstring(L, vt->buf, vt->ti);
    lua_setfield(L, -2, "text");
    LUA_TSETI("x", vt->tx); LUA_TSETI("y", vt->ty);
    LUA_TSETI("r", vt->tr);
    LUA_TSETI("g", vt->tg);
    LUA_TSETI("b", vt->tb);
    LUA_TPUSH(vt->textrun);

    vt->ti = 0;
}

static int flush_line(vts_t *vt) {
    lua_State *L = vt->L;
    add_rect(vt);
    add_textrun(vt);
    lua_rawgeti(L, LUA_REGISTRYINDEX, vt->draw_cb);
    if (!lua_isfunction(L, -1))
        return lua_pop(L, 3), 1;

    lua_insert(L, -3); // move function to the top

    if (lua_pcall(L, 2, 0, 0) != LUA_OK)
        return lua_pop(L, 1), 1;

    return 0;
}

static int draw_cb(struct tsm_screen *screen,
                    uint64_t id,
                    const uint32_t *ch,
                    size_t len,
                    unsigned int width,
                    unsigned int posx,
                    unsigned int posy,
                    const struct tsm_screen_attr *attr,
                    tsm_age_t age,
                    void *data) {
    vts_t *vt = (vts_t *) data;
    lua_State *L = vt->L;

    if (L == NULL) return 1;

    if (vt->ry != posy) {
        flush_line(vt);
        vt->rx = vt->tx = posx;
        vt->ry = vt->ty = posy;
        lua_newtable(L); vt->rect = lua_gettop(L);
        lua_newtable(L); vt->textrun = lua_gettop(L);
    }
    
    if (vt->rr != attr->br && vt->rg != attr->bg && vt->rb != attr->bb) {
        add_rect(vt);
        vt->rx = posx; vt->ry = posy;
    }

    if (vt->tr != attr->fr && vt->tg != attr->fg && vt->tb != attr->fb) {
        add_textrun(vt);
        vt->tx = posx; vt->ty = posy;
    }

    vt->rr = attr->br; vt->rg = attr->bg; vt->rb = attr->bb;
    vt->tr = attr->fr; vt->tg = attr->fg; vt->tb = attr->fb;

    vt->ti += tsm_ucs4_to_utf8(len == 0 ? ' ' : *ch, vt->buf + vt->ti);
    vt->rw += width;

    return 0;
}

static int f_draw(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    vt->draw_cb = luaL_ref(L, LUA_REGISTRYINDEX);
    vt->tr = vt->tg = vt->tb = vt->rr = vt->rg = vt->rb = 0;
    vt->tx = vt->ty = vt->ti = vt->rx = vt->ry = vt->rw = 0;

    lua_newtable(L); vt->rect = lua_gettop(L);
    lua_newtable(L); vt->textrun = lua_gettop(L);
    tsm_screen_draw(vt->screen, &draw_cb, vt);
    flush_line(vt);
    luaL_unref(L, LUA_REGISTRYINDEX, vt->draw_cb); vt->draw_cb = LUA_NOREF;

    return 0;
}

static int f_set_palette(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    if (!lua_isstring(L, 2) && !lua_istable(L, 2))
        return luaL_typeerror(L, 2, "string or table");

    int r;
    const char *palette = NULL;
    if (lua_istable(L, 2)) {
        uint8_t colors[TSM_COLOR_NUM][3];

        LUA_ASSERT(lua_rawlen(L, 2) >= TSM_COLOR_NUM, "palette should have %d entries", TSM_COLOR_NUM);
        for (int i = 1; i <= TSM_COLOR_NUM; i++) {
            lua_rawgeti(L, 2, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            LUA_ASSERT(lua_rawlen(L, -1) >= 3, "color table must contain 3 or more numbers");

            colors[i-1][0] = (lua_rawgeti(L, -1, 1), luaL_checkinteger(L, -1));
            colors[i-1][1] = (lua_rawgeti(L, -2, 2), luaL_checkinteger(L, -1));
            colors[i-1][2] = (lua_rawgeti(L, -3, 3), luaL_checkinteger(L, -1));

            lua_pop(L, 4);
        }
    
        r = tsm_vte_set_custom_palette(vt->vte, colors);
        if (r < 0) LUA_THROW(-r, "cannot change color palette: %s");

        palette = "custom";
    } else {
        palette = luaL_checkstring(L, 2);
    }

    r = tsm_vte_set_palette(vt->vte, palette);
    if (r < 0) LUA_THROW(-r, "cannot change color palette: %s");

    return 0;
}

static int f_gc(lua_State *L) {
    vts_t *vt = luaL_checkudata(L, 1, API_TYPE_VTS);
    if (vt->L) {
        luaL_unref(L, LUA_REGISTRYINDEX, vt->data);
        luaL_unref(L, LUA_REGISTRYINDEX, vt->draw_cb);

        free(vt->buf);

        tsm_vte_unref(vt->vte);
        tsm_screen_unref(vt->screen);
        vt->L = NULL;
        vt->vte = NULL;
        vt->screen = NULL;
        vt->data = vt->draw_cb = LUA_NOREF;
    }
    return 0;
}

static const luaL_Reg lib[] = {
    { "new",         f_new },
    { "get_size",    f_get_size },
    { "set_size",    f_set_size },
    { "get_cursor",  f_get_cursor },
    { "write",       f_write },
    { "draw",        f_draw },
    { "set_palette", f_set_palette },
    { "__gc",        f_gc },
    { NULL, NULL }
};

int luaopen_luatsm(lua_State *L) {
    luaL_newmetatable(L, API_TYPE_VTS);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, lib, 0);
    return 1;
}
