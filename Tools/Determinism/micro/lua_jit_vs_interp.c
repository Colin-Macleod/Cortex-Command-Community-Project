/* Determinism micro-test: does LuaJIT produce bit-identical float results with the JIT on vs off (interpreter)?
 * Game AI scripts do a lot of float math (math.sin/cos/atan2/sqrt/pow, vector maths); a peer with the JIT disabled
 * (SettingsMan DisableLuaJIT) or where a trace aborts differently must still compute the same bits. */
#include <stdio.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

static const char* script =
	"local mode = ...\n"
	"if mode == 'off' then jit.off() else jit.on() end\n"
	"local sin, cos, atan2, sqrt, pow, floor, exp, log = math.sin, math.cos, math.atan2, math.sqrt, math.pow, math.floor, math.exp, math.log\n"
	"local x, y, vx, vy, acc = 1.0, 2.0, 0.3, -0.7, 0\n"
	"for i = 1, 2000000 do\n"
	"  local a = atan2(vy, vx) + sin(i * 0.001) * 0.1\n"
	"  local m = sqrt(vx * vx + vy * vy)\n"
	"  vx = cos(a) * m * 0.999 + 0.0001; vy = sin(a) * m * 0.999 - 0.00098\n"
	"  x = x + vx; y = y + vy\n"
	"  if y < -100 then y = -100; vy = -vy * 0.7 end\n"
	"  acc = acc + pow(m + 1, 1.37) + exp(-m) + log(m + 1) + (x * y) % 7.3\n"
	"end\n"
	"return string.format('%a %a %a', x, y, acc)\n";

int main(int argc, char** argv) {
	const char* modes[2] = {"on", "off"};
	for (int i = 0; i < 2; ++i) {
		lua_State* L = luaL_newstate();
		luaL_openlibs(L);
		if (luaL_loadstring(L, script) != 0) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
		lua_pushstring(L, modes[i]);
		if (lua_pcall(L, 1, 1, 0) != 0) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
		printf("jit %-3s: %s\n", modes[i], lua_tostring(L, -1));
		lua_close(L);
	}
	return 0;
}
