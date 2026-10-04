// Determinism micro-test: libstdc++ (Linux/macOS gcc builds) vs MSVC STL (Windows build) distributions.
// MSVC can't be run here, so this file contains a hand port of the MSVC STL algorithms
// (microsoft/STL stl/inc/random: generate_canonical + uniform_real_distribution::_Eval, and
//  stl/inc/algorithm: _Rng_from_urng_v2 used by uniform_int_distribution::_Eval), specialised for std::mt19937,
// and compares them draw-for-draw against the local standard library on the same engine stream.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>

// --- MSVC STL port (mt19937: 32-bit output, range is a power of two) ---
static float msvc_canonical_float(std::mt19937& g) {
	// _Minbits = 24, _Rx_is_pow2, _Kx = 1, _Smax_bits = 32 -> discard low 8 bits, scale by 2^-24.
	uint32_t sx = static_cast<uint32_t>(g()) >> 8;
	return static_cast<float>(sx) * (1.0f / 16777216.0f);
}
static double msvc_canonical_double(std::mt19937& g) {
	// _Minbits = 53, _Kx = 2, _Smax_bits = 64 -> discard 11 bits, second draw shifted by 32 - 11.
	uint64_t sx = static_cast<uint64_t>(g()) >> 11;
	sx += static_cast<uint64_t>(g()) << 21;
	return static_cast<double>(sx) * static_cast<double>(1.0f / static_cast<float>(1ULL << 53));
}
template <typename T> static T msvc_uniform_real(std::mt19937& g, T a, T b) {
	T u = sizeof(T) == 4 ? static_cast<T>(msvc_canonical_float(g)) : static_cast<T>(msvc_canonical_double(g));
	return u * (b - a) + a;
}
static int msvc_uniform_int(std::mt19937& g, int lo, int hi) {
	// _Rng_from_urng_v2<uint32_t, mt19937>: Lemire's nearly divisionless method with 32-bit words.
	uint32_t index = static_cast<uint32_t>(hi - lo) + 1u;
	uint64_t product = static_cast<uint64_t>(static_cast<uint32_t>(g())) * index;
	uint32_t rem = static_cast<uint32_t>(product);
	if (rem < index) {
		uint32_t threshold = (0xFFFFFFFFu - index + 1u) % index;
		while (rem < threshold) {
			product = static_cast<uint64_t>(static_cast<uint32_t>(g())) * index;
			rem = static_cast<uint32_t>(product);
		}
	}
	return lo + static_cast<int>(product >> 32);
}

int main() {
	const int N = 1000000;
	const float fhi = std::nextafter(1.0f, std::numeric_limits<float>::max());
	const double dhi = std::nextafter(1.0, std::numeric_limits<double>::max());
	struct Test { const char* name; int mismatches; } tests[3] = {{"RandomNum<float>()  [C++ physics]", 0}, {"RandomNum<double>() [Lua PosRand]", 0}, {"RandomNum<int>(0,99) [Lua SelectRand]", 0}};
	{
		std::mt19937 a(1234), b(1234);
		for (int i = 0; i < N; ++i) { float x = std::uniform_real_distribution<float>(0.0f, fhi)(a); float y = msvc_uniform_real<float>(b, 0.0f, fhi); tests[0].mismatches += (x != y); }
	}
	{
		std::mt19937 a(1234), b(1234);
		for (int i = 0; i < N; ++i) { double x = std::uniform_real_distribution<double>(0.0, dhi)(a); double y = msvc_uniform_real<double>(b, 0.0, dhi); tests[1].mismatches += (x != y); }
	}
	{
		std::mt19937 a(1234), b(1234);
		for (int i = 0; i < N; ++i) { int x = std::uniform_int_distribution<int>(0, 99)(a); int y = msvc_uniform_int(b, 0, 99); tests[2].mismatches += (x != y); }
	}
	for (const Test& t: tests) {
		std::printf("%-40s local-stdlib vs MSVC-STL: %7d / %d draws differ (%.2f%%)\n", t.name, t.mismatches, N, 100.0 * t.mismatches / N);
	}
	return 0;
}
