#include "MathConsistency.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBC__)
#include <cerrno>
#include <cstdio>
#include <vector>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
#define RTE_MATH_REEXEC_WITH_TUNABLES
#endif

using namespace RTE;

namespace {
	std::string s_StartupReport; //!< What PrepareForLockstep did, for the console.

#ifdef RTE_MATH_REEXEC_WITH_TUNABLES
	constexpr const char* c_TunablesVariable = "GLIBC_TUNABLES";
	/// Set in the restarted process. Set by hand, it skips the restart (the stress tests use that for a peer whose math differs).
	constexpr const char* c_RestartedVariable = "CCCP_MATH_RESTARTED";
	/// Set in the restarted process only: GLIBC_TUNABLES as it was before the restart ("1:<value>", or "0" if it wasn't set), to put back.
	constexpr const char* c_OriginalTunablesVariable = "CCCP_MATH_ORIGINAL_TUNABLES";
	/// Set in the probe process, which exits with c_ProbeSuccessExitCode as soon as it is loaded.
	constexpr const char* c_ProbeVariable = "CCCP_MATH_PROBE";
	/// Test aid: makes the probe fail like it would if the dynamic loader refused to load a library.
	constexpr const char* c_ProbeTestFailVariable = "CCCP_MATH_PROBE_TEST_FAIL";
	constexpr int c_ProbeSuccessExitCode = 73;
	constexpr int c_ProbeTimeoutMS = 10000;

	/// Copies this process's environment, leaving out the named variables, and adds the given "NAME=value" entries.
	std::vector<std::string> BuildEnvironment(const std::vector<const char*>& remove, const std::vector<std::string>& add) {
		std::vector<std::string> environment;
		for (char** variable = environ; variable && *variable; ++variable) {
			bool keep = true;
			for (const char* name: remove) {
				const size_t nameLength = std::strlen(name);
				keep = keep && !(std::strncmp(*variable, name, nameLength) == 0 && (*variable)[nameLength] == '=');
			}
			if (keep) {
				environment.emplace_back(*variable);
			}
		}
		environment.insert(environment.end(), add.begin(), add.end());
		return environment;
	}

	/// @return Null-terminated pointers to the strings, for exec.
	std::vector<char*> ToPointers(std::vector<std::string>& strings) {
		std::vector<char*> pointers;
		for (std::string& string: strings) {
			pointers.push_back(string.data());
		}
		pointers.push_back(nullptr);
		return pointers;
	}

	/// Starts this program with the given environment, in which it exits as soon as it is loaded, and waits for it. That fails if the dynamic
	/// loader refuses to start it, e.g. because a library needs CPU features (an x86-64 ISA level) that the tunables hide.
	/// @return An empty string on success, otherwise what went wrong.
	std::string RunProbe(const char* executablePath, std::vector<std::string> environment) {
		std::vector<char*> environmentPointers = ToPointers(environment);
		char probeName[] = "CortexCommand-math-probe";
		char* probeArgv[] = {probeName, nullptr};
		pid_t pid = 0;
		if (int error = posix_spawn(&pid, executablePath, nullptr, nullptr, probeArgv, environmentPointers.data()); error != 0) {
			return std::string("couldn't start it: ") + std::strerror(error);
		}
		int status = 0;
		for (int waitedMS = 0;; ++waitedMS) {
			pid_t result = waitpid(pid, &status, WNOHANG);
			if (result == pid) {
				break;
			} else if (result < 0 && errno != EINTR) {
				return std::string("couldn't wait for it: ") + std::strerror(errno);
			} else if (waitedMS >= c_ProbeTimeoutMS) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				return "it didn't exit within " + std::to_string(c_ProbeTimeoutMS / 1000) + " s";
			}
			timespec oneMS = {0, 1000000};
			nanosleep(&oneMS, nullptr);
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == c_ProbeSuccessExitCode) {
			return "";
		} else if (WIFSIGNALED(status)) {
			return "it was killed by signal " + std::to_string(WTERMSIG(status));
		}
		const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		return "it exited with code " + std::to_string(exitCode) + (exitCode == 127 ? ", the dynamic loader probably refused to load a library" : "");
	}

	/// Keeps the report for the console and prints it right away too, as the console doesn't exist yet.
	void Report(const std::string& report) {
		s_StartupReport = report;
		std::fprintf(stderr, "%s\n", report.c_str());
	}

	/// In the probe process, exits as soon as the program and its libraries are loaded, which is all the probe checks. Runs before the program's
	/// other static initialization (the priority makes it first), which takes far longer, e.g. the profiler calibrating its timer.
	__attribute__((constructor(101))) void ExitIfProbe() {
		if (std::getenv(c_ProbeVariable)) {
			_exit(std::getenv(c_ProbeTestFailVariable) ? 127 : c_ProbeSuccessExitCode);
		}
	}
#endif
} // namespace

void MathConsistency::PrepareForLockstep(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_M_X64)
	// Harmless outside co-op too, and must happen before any math library call whose results could be compared.
	_set_FMA3_enable(0);
#elif defined(RTE_MATH_REEXEC_WITH_TUNABLES)
	if (std::getenv(c_ProbeVariable)) {
		// We're the probe started below, and ExitIfProbe didn't run (it should have).
		_exit(std::getenv(c_ProbeTestFailVariable) ? 127 : c_ProbeSuccessExitCode);
	}
	if (const char* originalTunables = std::getenv(c_OriginalTunablesVariable); originalTunables && std::getenv(c_RestartedVariable)) {
		// We're the restarted process. glibc has read the tunables already; put the environment back the way it was, so programs started from
		// here don't inherit our changes.
		if (originalTunables[0] == '1' && originalTunables[1] == ':') {
			setenv(c_TunablesVariable, originalTunables + 2, 1);
		} else {
			unsetenv(c_TunablesVariable);
		}
		unsetenv(c_OriginalTunablesVariable);
		unsetenv(c_RestartedVariable);
		s_StartupReport = "Math library: restarted with glibc's FMA code paths masked.";
		return;
	}

	// Always, not only when the command line starts a co-op session, because a session can also be hosted or joined from the menus, long after
	// the math library was loaded. -no-math-restart opts out, e.g. for debugging, at the cost of possibly being refused when joining.
	const std::string mayBeRefused = " Co-op hosts whose math library gives different results will refuse this computer.";
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-no-math-restart") == 0) {
			s_StartupReport = "Math library: not restarted (-no-math-restart)." + mayBeRefused;
			return;
		}
	}
	if (std::getenv(c_RestartedVariable)) {
		s_StartupReport = std::string("Math library: not restarted (") + c_RestartedVariable + " is set)." + mayBeRefused;
		return;
	}
	__builtin_cpu_init();
	if (!__builtin_cpu_supports("fma") && !__builtin_cpu_supports("fma4")) {
		// glibc picks the same code paths as it would with FMA masked.
		s_StartupReport = "Math library: no restart needed, this CPU has no FMA.";
		return;
	}

	constexpr const char* hwcapsTunable = "glibc.cpu.hwcaps=";
	constexpr const char* maskedFeatures = "-FMA,-FMA4";
	const char* originalTunables = std::getenv(c_TunablesVariable);
	std::string tunables = originalTunables ? originalTunables : "";
	if (size_t hwcaps = tunables.find(hwcapsTunable); hwcaps != std::string::npos) {
		tunables.insert(hwcaps + std::strlen(hwcapsTunable), std::string(maskedFeatures) + ",");
	} else {
		tunables += (tunables.empty() ? "" : ":") + std::string(hwcapsTunable) + maskedFeatures;
	}
	const std::string tunablesEntry = std::string(c_TunablesVariable) + "=" + tunables;

	// Through the real path, not /proc/self/exe, which would also become the process name.
	char executablePath[4096];
	const ssize_t pathLength = readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
	if (pathLength <= 0) {
		Report("Math library: not restarted, as the program's path is unknown." + mayBeRefused);
		return;
	}
	executablePath[pathLength] = '\0';

	// On a system whose libraries are built for newer CPUs only (x86-64-v3, which includes FMA), the dynamic loader refuses to load them with FMA
	// masked, so the restarted program would die before getting to main. Try it in a throwaway process first; that takes a few milliseconds.
	const std::vector<const char*> ourVariables = {c_TunablesVariable, c_RestartedVariable, c_OriginalTunablesVariable, c_ProbeVariable};
	if (std::string probeFailure = RunProbe(executablePath, BuildEnvironment(ourVariables, {tunablesEntry, std::string(c_ProbeVariable) + "=1"})); !probeFailure.empty()) {
		Report("Math library: not restarted with glibc's FMA code paths masked, because a test start with them masked failed (" + probeFailure + "). This system's libraries probably need a newer CPU." + mayBeRefused);
		return;
	}

	std::vector<std::string> environment = BuildEnvironment(ourVariables, {tunablesEntry, std::string(c_RestartedVariable) + "=1", std::string(c_OriginalTunablesVariable) + "=" + (originalTunables ? "1:" + std::string(originalTunables) : "0")});
	execve(executablePath, argv, ToPointers(environment).data());
	// Only returns if the restart failed. Carry on: the fingerprint check at join time will refuse a mismatched machine instead.
	Report(std::string("Math library: restart failed (") + std::strerror(errno) + ")." + mayBeRefused);
#else
	(void)argc;
	(void)argv;
#endif
}

const std::string& MathConsistency::GetStartupReport() {
	return s_StartupReport;
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
