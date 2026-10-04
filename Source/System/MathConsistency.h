#pragma once

#include <cstdint>

namespace RTE {

	/// Keeps the C math library giving bit-identical results on every machine in a lockstep session.
	/// The x86-64 math libraries (glibc's libm and Microsoft's UCRT) pick a faster implementation of functions like sin, cos, exp, log, pow and atan2
	/// at runtime when the CPU supports FMA instructions. Those give slightly different results (in the last bit, for a fraction of a percent of
	/// inputs) from the implementation used on CPUs without FMA, which is enough to eventually desync two machines running the same build. Lua's
	/// math library and the ^ operator use the same functions.
	class MathConsistency {

	public:
		/// Makes this process use the math library implementation that works the same on every x86-64 CPU. Call first thing in main.
		/// On Windows this switches off the FMA code paths. On Linux with glibc the choice is made when the program loads, so if the command line
		/// starts a co-op session, the program restarts itself once with FMA masked from glibc's CPU feature detection (GLIBC_TUNABLES).
		/// @param argc Command line argument count, as passed to main.
		/// @param argv Command line arguments, as passed to main.
		static void PrepareForLockstep(int argc, char** argv);

		/// Gets a hash of the math library's results for a fixed set of inputs to the functions the simulation and scripts use.
		/// Two machines whose fingerprints differ would desync. Computed once, on first use.
		/// @return The fingerprint.
		static uint64_t GetFingerprint();
	};
} // namespace RTE
