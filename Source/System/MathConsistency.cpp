#include "MathConsistency.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBC__)
#include <unistd.h>
#define RTE_MATH_REEXEC_WITH_TUNABLES
#endif

using namespace RTE;

void MathConsistency::PrepareForLockstep(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_M_X64)
	// Harmless outside co-op too, and must happen before any math library call whose results could be compared.
	_set_FMA3_enable(0);
#elif defined(RTE_MATH_REEXEC_WITH_TUNABLES)
	// Always, not only when the command line starts a co-op session, because a session can also be hosted or joined from the menus, long after
	// the math library was loaded. -no-math-restart opts out, e.g. for debugging, at the cost of possibly being refused when joining.
	bool optOut = false;
	for (int i = 1; i < argc; ++i) {
		optOut = optOut || std::strcmp(argv[i], "-no-math-restart") == 0;
	}
	const char* restartMarker = "CCCP_MATH_RESTARTED";
	if (optOut || std::getenv(restartMarker)) {
		return;
	}

	constexpr const char* hwcapsTunable = "glibc.cpu.hwcaps=";
	constexpr const char* maskedFeatures = "-FMA,-FMA4";
	std::string tunables = std::getenv("GLIBC_TUNABLES") ? std::getenv("GLIBC_TUNABLES") : "";
	if (size_t hwcaps = tunables.find(hwcapsTunable); hwcaps != std::string::npos) {
		tunables.insert(hwcaps + std::strlen(hwcapsTunable), std::string(maskedFeatures) + ",");
	} else {
		tunables += (tunables.empty() ? "" : ":") + std::string(hwcapsTunable) + maskedFeatures;
	}
	setenv("GLIBC_TUNABLES", tunables.c_str(), 1);
	setenv(restartMarker, "1", 1);
	// Through the real path, not /proc/self/exe, which would also become the process name.
	char executablePath[4096];
	if (ssize_t length = readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1); length > 0) {
		executablePath[length] = '\0';
		execv(executablePath, argv);
	}
	// Only returns if the restart failed. Carry on: the fingerprint check at join time will refuse a mismatched machine instead.
#else
	(void)argc;
	(void)argv;
#endif
}

uint64_t MathConsistency::GetFingerprint() {
	static const uint64_t fingerprint = []() {
		uint64_t hash = 1469598103934665603ULL;
		auto mix = [&hash](const void* data, size_t size) {
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i) {
				hash = (hash ^ bytes[i]) * 1099511628211ULL;
			}
		};
		auto mixDouble = [&mix](double value) { mix(&value, sizeof(value)); };
		auto mixFloat = [&mix](float value) { mix(&value, sizeof(value)); };

		// Inputs come from integer arithmetic only, so they're the same everywhere. The FMA and non-FMA implementations differ for roughly
		// 0.03-0.07% of inputs, so this many samples all but guarantees catching a difference.
		uint32_t state = 0x2545F491u;
		auto next = [&state]() {
			state = state * 1664525u + 1013904223u;
			return static_cast<double>(static_cast<int32_t>(state)) / 2147483648.0;
		};
		for (int i = 0; i < 10000; ++i) {
			const double a = next();
			const double b = next();
			mixDouble(std::sin(a * 1000.0));
			mixDouble(std::cos(a * 1000.0));
			mixDouble(std::tan(a * 3.0));
			mixDouble(std::exp(b * 20.0));
			mixDouble(std::log(std::abs(a) * 1000.0 + 1e-6));
			mixDouble(std::pow(std::abs(a) * 100.0, b * 10.0));
			mixDouble(std::atan2(a, b));
			mixDouble(std::atan(a * 100.0));
			mixDouble(std::asin(b));
			mixDouble(std::acos(b));
			mixDouble(std::sinh(b * 5.0));
			mixDouble(std::tanh(b * 5.0));
			const float fa = static_cast<float>(a);
			const float fb = static_cast<float>(b);
			mixFloat(std::sin(fa * 1000.0F));
			mixFloat(std::cos(fa * 1000.0F));
			mixFloat(std::exp(fb * 20.0F));
			mixFloat(std::log(std::abs(fa) * 1000.0F + 1e-6F));
			mixFloat(std::pow(std::abs(fa) * 100.0F, fb * 10.0F));
			mixFloat(std::atan2(fa, fb));
		}
		return hash;
	}();
	return fingerprint;
}
