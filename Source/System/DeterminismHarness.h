#pragma once

#include <cstdint>
#include <fstream>
#include <set>
#include <string>

namespace RTE {

	/// Test harness for measuring simulation determinism (groundwork for lockstep multiplayer).
	/// Entirely inert unless the CCCP_DT_LOG environment variable is set. When enabled it:
	///   - Skips the intro/menus and launches a configurable Activity with CPU opponents.
	///   - Forces exactly one fixed-length sim update per frame, so sim progression is independent of wall-clock time.
	///   - Writes one line per sim tick containing hashes of the simulation state (RNGs, actors, items, particles, terrain).
	///   - Optionally dumps every MovableObject's state at chosen ticks so diverging runs can be diffed.
	///   - Quits after a configurable number of sim ticks.
	///
	/// Environment variables:
	///   CCCP_DT_LOG         Path of the per-tick hash log. Enables the harness.
	///   CCCP_DT_TICKS       Number of sim ticks to run before quitting (default 3000).
	///   CCCP_DT_ACTIVITY    GAScripted preset name to launch (default "Bunker Breach"). Activities that start with a brain-placement
	///                       editing phase (e.g. "Skirmish Defense") will just sit in the editor, since no input arrives.
	///   CCCP_DT_SCENE       Scene preset name (default "Zekarra Mining Outpost").
	///   CCCP_DT_FOG         1 to enable fog of war, 0 to disable (default 1).
	///   CCCP_DT_DUMP_TICKS  Comma-separated list of ticks at which to write a full per-MO dump to "<log>.dump<tick>".
	///   CCCP_DT_TERRAIN_EVERY  Hash the terrain material layer every N ticks (default 10, 0 disables).
	///   CCCP_DT_SYNC_TERRAIN_HASH  1 to wait for all thread pool tasks before hashing terrain (rules out read races in the harness itself).
	///   CCCP_DT_TRACE_TICKS Only in builds compiled with -DRTE_RNG_TRACE: log the call stack of every global RNG draw
	///                       during the first N ticks to "<log>.rngtrace" (resolve with Tools/Determinism/symbolize_trace.py).
	class DeterminismHarness {

	public:
		/// Reads configuration from the environment. Must be called once at startup before any other harness function.
		static void Initialize();

		/// Whether the harness is active for this run.
		static bool IsEnabled() { return s_Enabled; }

		/// The number of sim ticks completed since the test Activity started.
		static long long GetTick() { return s_Tick; }

		/// Configures and queues the test Activity to be started by the game loop.
		/// @return Whether the Activity was set up successfully.
		static bool SetupActivity();

		/// Called once at the end of every sim update. Hashes and logs sim state, and requests quit when done.
		static void EndOfSimUpdate();

	private:
		static bool s_Enabled; //!< Whether the harness is active.
		static std::ofstream s_Log; //!< The per-tick hash log.
		static std::string s_LogPath; //!< Path of the per-tick hash log.
		static long long s_TicksToRun; //!< How many sim ticks to run before quitting.
		static long long s_Tick; //!< The current harness tick count.
		static int s_TerrainEvery; //!< Hash terrain every this many ticks.
		static bool s_Fog; //!< Whether fog of war is enabled in the test Activity.
		static bool s_SyncBeforeTerrainHash; //!< Whether to wait for all thread pool work to finish before hashing the terrain.
		static std::string s_ActivityName; //!< The GAScripted preset to launch.
		static std::string s_SceneName; //!< The Scene preset to launch.
		static std::set<long long> s_DumpTicks; //!< Ticks at which to write a full per-MO dump.
		static uint64_t s_LastTerrainHash; //!< Most recently computed terrain hash.

		/// Writes a full per-MO state dump for the current tick.
		static void WriteDump();
	};
} // namespace RTE
