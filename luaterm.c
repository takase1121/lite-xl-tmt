// aggregrate module for luatsm, bel and other modules
// that lite-xl-tmt uses

#include <lua.h>
#include <lauxlib.h>

int luaopen_luatsm(lua_State *L);
int luaopen_bel(lua_State *L);

int luaopen_luaterm(lua_State *L) {
	lua_newtable(L);
	luaopen_luatsm(L);
	lua_setfield(L, -2, "tsm");
	luaopen_bel(L);
	lua_setfield(L, -2, "bel");
	return 1;
}
