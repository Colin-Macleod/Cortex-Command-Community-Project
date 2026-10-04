/* Determinism micro-test: is LuaJIT pairs() iteration order stable between processes?
 * Builds tables the way game scripts do (string keys such as preset names, and table/userdata-like keys),
 * then prints a hash of the visiting order. Run it several times and compare the output. */
#include <stdio.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

static const char* script =
	"local names = {'Soldier Light','Soldier Heavy','Browncoat Heavy','Dummy','Crab','Dropship MK1','Rocket MK2',"
	"  'Medic Drone','Brain Robot','Ronin Soldier','Culled Clone','Gatling Gun','Pistol','SMG','Shotgun','Sniper Rifle'}\n"
	"local function order(t) local h = 0 for k in pairs(t) do local s = tostring(type(k)=='table' and k.id or k)"
	"  for i = 1, #s do h = (h * 31 + s:byte(i)) % 4294967296 end end return h end\n"
	"local strTable = {} for i, n in ipairs(names) do strTable[n] = i end\n"
	"local objTable = {} for i = 1, 64 do objTable[{id = i}] = i end\n"
	"local mixed = {} for i = 1, 64 do mixed['actor' .. i] = i end\n"
	"return string.format('string-keyed=%08x  table-keyed=%08x  generated-keys=%08x', order(strTable), order(objTable), order(mixed))\n";

int main(void) {
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	if (luaL_dostring(L, script) != 0) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
	printf("%s\n", lua_tostring(L, -1));
	lua_close(L);
	return 0;
}
