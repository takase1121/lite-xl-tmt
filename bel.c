// cross-platform(?) bell library, because nothing ever goes your way
//
#include <lua.h>
#include <lauxlib.h>

#ifdef __WIN32
#include <Windows.h>
#else
#include <dlfcn.h>

#define FUNC(name, restype, ...) typedef restype (*name##_func)(__VA_ARGS__); static name##_func name = NULL

#define LOAD_FUNC(handle, name) { \
    *(void **) (&name) = dlsym(handle, #name); \
    if (!name) { \
        const char *error = dlerror(); \
        if (error != NULL) { \
            lua_pushfstring(L, "failed to load %s: %s", #name, error); \
            dlclose(handle); \
            handle = NULL; \
            return lua_error(L); \
        } \
    } \
}

static void *libcanberra_handle = NULL;
static void *canberra_ctx = NULL;

FUNC(ca_context_create, int, void **);
FUNC(ca_context_destroy, int, void *);
typedef int (*ca_context_play_func)(void *, uint32_t, ...); static ca_context_play_func ca_context_play = NULL;

int load_libcanberra(lua_State *L) {
    static int done = 0;
    if (done) return 1;
    done = 1;

    const char *libnames[] = {
        "libcanberra.so",
        "libcanberra.so.0",
        "libcanberra.so.0.2.5",
        NULL
    };

    for (int i = 0; libnames[i]; i++) {
        libcanberra_handle = dlopen(libnames[i], RTLD_LAZY);
        if (libcanberra_handle) break;
    }

    if (libcanberra_handle == NULL)
        return luaL_error(L, "cannot load libcanberra: %s", dlerror());

    LOAD_FUNC(libcanberra_handle, ca_context_create);
    LOAD_FUNC(libcanberra_handle, ca_context_play);
    LOAD_FUNC(libcanberra_handle, ca_context_destroy);

    if (ca_context_create(&canberra_ctx) != 0) {
        ca_context_destroy(canberra_ctx); canberra_ctx = NULL;
        dlclose(libcanberra_handle); libcanberra_handle = NULL;
        return luaL_error(L, "cannot create libcanberra context");
    }
    return 1;
}
#endif

static int f_bel(lua_State *L) {
    // since this is hooked to __call, 1st arg is self
    const char *path = luaL_optstring(L, 2, NULL);
    const char *source = luaL_optstring(L, 3, "lite-xl-tmt bell");
#ifdef __WIN32
    PlaySoundA(
        path ? path : (LPSTR) SND_ALIAS_SYSTEMDEFAULT,
        NULL,
        (path ? SND_FILENAME : SND_ALIAS_ID)
        | SND_ASYNC
        | SND_NODEFAULT
    );
#else
    load_libcanberra(L);
    if (libcanberra_handle == NULL || canberra_ctx == NULL) return 0;
    ca_context_play(
        canberra_ctx, 0,
        path ? "media.filename" : "event.id", path ? path : "bell",
        "event.description", source,
        "media.role", "event",
        "canberra.cache-control", "permanent",
        NULL
    );
#endif
    return 0;
}

static int f_gc(lua_State *L) {
#ifndef __WIN32
    if (canberra_ctx) ca_context_destroy(canberra_ctx);
    if (libcanberra_handle) dlclose(libcanberra_handle);
    canberra_ctx = libcanberra_handle = NULL;
#endif
    return 0;
}

static const luaL_Reg lib[] = {
    { "__call", f_bel },
    { "__gc", f_gc },
    { NULL, NULL }
};

int luaopen_bel(lua_State *L) {
    luaL_newmetatable(L, "bel");
    luaL_setmetatable(L, "bel");
    luaL_setfuncs(L, lib, 0);
    return 1;
}
