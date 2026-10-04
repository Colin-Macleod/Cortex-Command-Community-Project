/* Determinism micro-test: does glibc's libm give the same bits on CPUs with and without FMA?
 * glibc picks an FMA/AVX2 implementation of many functions at load time when the CPU supports it. Build once, then run it as is and with
 * the FMA code paths masked, and compare the two outputs:
 *   gcc -O2 -o libm_cpu_paths libm_cpu_paths.c -lm
 *   ./libm_cpu_paths > native.txt
 *   GLIBC_TUNABLES=glibc.cpu.hwcaps=-FMA,-FMA4 ./libm_cpu_paths > nofma.txt
 *   diff native.txt nofma.txt
 * With glibc 2.39 the float functions match, while double sin, cos, exp, log, pow, atan2, atan, asin and acos differ (in the last bit, for
 * roughly 0.03-0.07% of inputs). */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t h = 1469598103934665603ULL;
static void mixf(float f) { uint32_t u; memcpy(&u, &f, 4); h = (h ^ u) * 1099511628211ULL; }
static void mixd(double f) { uint64_t u; memcpy(&u, &f, 8); h = (h ^ u) * 1099511628211ULL; }

int main(void) {
	const char* names[] = {"sinf", "cosf", "tanf", "expf", "logf", "powf", "atan2f", "sqrtf", "sin", "cos", "exp", "log", "pow", "atan2", "atan", "asin", "acos", "fmodf", "exp2f", "log2f", "sincosf"};
	for (int f = 0; f < 21; f++) {
		h = 1469598103934665603ULL;
		uint32_t x = 12345;
		for (int i = 0; i < 2000000; i++) {
			x = x * 1664525u + 1013904223u;
			float a = ((int32_t)x) / 2147483648.0f * 1000.0f;
			x = x * 1664525u + 1013904223u;
			float b = ((int32_t)x) / 2147483648.0f * 10.0f;
			switch (f) {
				case 0: mixf(sinf(a)); break;
				case 1: mixf(cosf(a)); break;
				case 2: mixf(tanf(a)); break;
				case 3: mixf(expf(b)); break;
				case 4: mixf(logf(fabsf(a) + 1e-3f)); break;
				case 5: mixf(powf(fabsf(b), b)); break;
				case 6: mixf(atan2f(a, b)); break;
				case 7: mixf(sqrtf(fabsf(a))); break;
				case 8: mixd(sin(a)); break;
				case 9: mixd(cos(a)); break;
				case 10: mixd(exp(b)); break;
				case 11: mixd(log(fabs(a) + 1e-3)); break;
				case 12: mixd(pow(fabs(b), b)); break;
				case 13: mixd(atan2(a, b)); break;
				case 14: mixd(atan(a)); break;
				case 15: mixd(asin(b / 10)); break;
				case 16: mixd(acos(b / 10)); break;
				case 17: mixf(fmodf(a, b)); break;
				case 18: mixf(exp2f(b)); break;
				case 19: mixf(log2f(fabsf(a) + 1e-3f)); break;
				case 20: { float s, c; sincosf(a, &s, &c); mixf(s); mixf(c); } break;
			}
		}
		printf("%-8s %016llx\n", names[f], (unsigned long long)h);
	}
	return 0;
}
