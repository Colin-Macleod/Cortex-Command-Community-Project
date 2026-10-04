/* Determinism micro-test: floating point and libm consistency.
 * Hashes the bit patterns of the transcendental functions the engine uses (float, as in Vector/Matrix/Atom,
 * and double, as used by Lua's math library), plus a small chaotic "physics" integration that mimics how
 * one-ulp differences grow in the game sim. Compile with different compilers / libcs / flags and compare. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t h = 14695981039346656037ULL;
static void add(const void* p, size_t n) { const unsigned char* b = p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; } }
static uint64_t take(void) { uint64_t r = h; h = 14695981039346656037ULL; return r; }

/* Prevent constant folding: inputs come from a volatile scale. */
static volatile float vscale = 1.0f;

int main(void) {
	const int N = 2000000;
	float f; double d;
	for (int i = 0; i < N; ++i) { float x = (i - N / 2) * 0.000731f * vscale; f = sinf(x); add(&f, 4); f = cosf(x); add(&f, 4); }
	printf("sinf/cosf             %016llx\n", (unsigned long long)take());
	for (int i = 0; i < N; ++i) { float x = (i % 2001 - 1000) * 0.37f * vscale, y = (i / 2001 - 500) * 0.53f * vscale; f = atan2f(y, x); add(&f, 4); }
	printf("atan2f                %016llx\n", (unsigned long long)take());
	for (int i = 1; i < N; ++i) { float x = i * 0.0137f * vscale; f = powf(x, 1.37f); add(&f, 4); f = expf(-x * 0.01f); add(&f, 4); f = logf(x); add(&f, 4); }
	printf("powf/expf/logf        %016llx\n", (unsigned long long)take());
	for (int i = 1; i < N; ++i) { float x = i * 0.0137f * vscale; f = sqrtf(x); add(&f, 4); f = fmodf(x, 6.2831853f); add(&f, 4); }
	printf("sqrtf/fmodf           %016llx\n", (unsigned long long)take());
	for (int i = 0; i < N; ++i) { double x = (i - N / 2) * 0.000731 * vscale; d = sin(x); add(&d, 8); d = cos(x); add(&d, 8); d = atan2(x, 1.7); add(&d, 8); }
	printf("sin/cos/atan2 double  %016llx\n", (unsigned long long)take());
	for (int i = 1; i < N; ++i) { double x = i * 0.0137 * vscale; d = pow(x, 1.37); add(&d, 8); d = exp(-x * 0.01); add(&d, 8); d = log(x); add(&d, 8); }
	printf("pow/exp/log double    %016llx\n", (unsigned long long)take());

	/* Chaotic mini-sim: particles under gravity bouncing in a box with rotation, float math like Atom/MOSRotating. */
	enum { P = 512 };
	float px[P], py[P], vx[P], vy[P], ang[P], av[P];
	for (int i = 0; i < P; ++i) { px[i] = 10.0f + i * 1.3f; py[i] = 50.0f + (i % 17) * 2.1f; vx[i] = sinf(i * 0.7f) * 9.0f; vy[i] = cosf(i * 1.3f) * 9.0f; ang[i] = 0; av[i] = (i % 7 - 3) * 0.1f; }
	const float dt = 1.0f / 60.0f, g = 9.8f * vscale;
	int firstDiffTick = -1;
	for (int t = 0; t < 6000; ++t) {
		for (int i = 0; i < P; ++i) {
			vy[i] += g * dt;
			float c = cosf(ang[i]), s = sinf(ang[i]);
			float ox = c * 0.5f - s * 0.25f, oy = s * 0.5f + c * 0.25f; /* rotated atom offset */
			px[i] += vx[i] * dt + ox * av[i] * dt;
			py[i] += vy[i] * dt + oy * av[i] * dt;
			ang[i] += av[i] * dt;
			if (py[i] > 400.0f) { py[i] = 400.0f; float n = atan2f(vy[i], vx[i]); vy[i] = -vy[i] * 0.7f; vx[i] = vx[i] * 0.9f + sinf(n) * 0.3f; av[i] = -av[i] + vx[i] * 0.01f; }
			if (px[i] < 0.0f || px[i] > 900.0f) { vx[i] = -vx[i] * 0.8f; px[i] = px[i] < 0.0f ? 0.0f : 900.0f; }
			/* pairwise interaction with neighbour */
			int j = (i + 1) % P; float dx = px[j] - px[i], dy = py[j] - py[i]; float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
			if (dist < 3.0f) { float imp = (3.0f - dist) * 0.5f; vx[i] -= dx / dist * imp; vy[i] -= dy / dist * imp; }
		}
		(void)firstDiffTick;
	}
	add(px, sizeof px); add(py, sizeof py); add(vx, sizeof vx); add(vy, sizeof vy); add(ang, sizeof ang);
	printf("chaotic mini-sim      %016llx\n", (unsigned long long)take());
	return 0;
}
