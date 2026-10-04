// Determinism micro-test: does the game's RNG produce identical sequences across
// standard library implementations / compilers / architectures?
// The RandomGenerator below is a verbatim copy of RTE::RandomGenerator (Source/System/RTETools.h).
// std::mt19937 itself is fully specified by the standard; the *distributions* are NOT.
#include <random>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <cmath>
#include <type_traits>

class RandomGenerator {
	std::mt19937 m_RNG;
public:
	void Seed(uint64_t seed) { m_RNG.seed(seed); };
	template <typename floatType = float>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNormalNum() {
		return std::uniform_real_distribution<floatType>(floatType(-1.0), std::nextafter(floatType(1.0), std::numeric_limits<floatType>::max()))(m_RNG);
	}
	template <typename floatType = float>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum() {
		return std::uniform_real_distribution<floatType>(floatType(0.0), std::nextafter(floatType(1.0), std::numeric_limits<floatType>::max()))(m_RNG);
	}
	template <typename floatType>
	typename std::enable_if<std::is_floating_point<floatType>::value, floatType>::type RandomNum(floatType min, floatType max) {
		if (max < min) { std::swap(min, max); }
		return (std::uniform_real_distribution<floatType>(floatType(0.0), std::nextafter(max - min, std::numeric_limits<floatType>::max()))(m_RNG) + min);
	}
	template <typename intType>
	typename std::enable_if<std::is_integral<intType>::value, intType>::type RandomNum(intType min, intType max) {
		if (max < min) { std::swap(min, max); }
		return (std::uniform_int_distribution<intType>(intType(0), max - min)(m_RNG) + min);
	}
	uint32_t Raw() { return m_RNG(); }
};

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
	const unsigned char* b = static_cast<const unsigned char*>(p);
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

int main() {
	const uint32_t seed = 0x5EED;
	const int N = 1000000;
	struct { const char* name; uint64_t h; } r[8];
	int k = 0;
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { uint32_t v = g.Raw(); h = fnv(h, &v, 4); } r[k++] = {"mt19937 raw", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { float v = g.RandomNum<float>(); h = fnv(h, &v, 4); } r[k++] = {"RandomNum<float>()", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { double v = g.RandomNum<double>(); h = fnv(h, &v, 8); } r[k++] = {"RandomNum<double>() (Lua PosRand)", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { float v = g.RandomNum(-180.0F, 180.0F); h = fnv(h, &v, 4); } r[k++] = {"RandomNum(-180f,180f)", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { float v = g.RandomNormalNum<float>(); h = fnv(h, &v, 4); } r[k++] = {"RandomNormalNum<float>()", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { int v = g.RandomNum<int>(0, 99); h = fnv(h, &v, 4); } r[k++] = {"RandomNum<int>(0,99) (Lua SelectRand)", h}; }
	{ RandomGenerator g; g.Seed(seed); uint64_t h = 14695981039346656037ULL; for (int i = 0; i < N; ++i) { uint64_t v = g.RandomNum<uint64_t>(0, std::numeric_limits<uint64_t>::max()); h = fnv(h, &v, 8); } r[k++] = {"RandomNum<uint64_t>(full) (Lua seed)", h}; }
	for (int i = 0; i < k; ++i) { std::printf("%-40s %016llx\n", r[i].name, (unsigned long long)r[i].h); }
	RandomGenerator g; g.Seed(seed);
	std::printf("first 3 RandomNum<int>(0,99): %d %d %d\n", g.RandomNum<int>(0, 99), g.RandomNum<int>(0, 99), g.RandomNum<int>(0, 99));
	return 0;
}
