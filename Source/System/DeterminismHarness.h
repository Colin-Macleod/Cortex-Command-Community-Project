#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <utility>

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
	///   CCCP_DT_DUMP_RING   Keep a per-MO dump of each of the last N ticks in memory, and write them all ("<path>.ring<tick>") when a co-op desync is
	///                       reported, so both peers' dumps of the first diverging tick can be compared.
	///   CCCP_DT_TERRAIN_EVERY  Hash the terrain material layer every N ticks (default 10, 0 disables).
	///   CCCP_DT_OBSERVE     1 to only log hashes: don't launch an Activity or control sim stepping (e.g. to log a co-op session on each peer).
	///   CCCP_DT_DETERMINISTIC  0 to run without TimerMan deterministic mode (default 1, as in lockstep sessions). Ignored in observe mode.
	///   CCCP_DT_TICKS_PER_FRAME  "random" to run a random 0-3 sim updates per frame instead of exactly one, to check the sim doesn't depend on frame rate.
	///   CCCP_DT_FRAME_SEED  Seed for the random ticks-per-frame sequence.
	///   CCCP_DT_SYNC_TERRAIN_HASH  1 to wait for all thread pool tasks before hashing terrain (rules out read races in the harness itself).
	///   CCCP_DT_TRACE_TICKS Only in builds compiled with -DRTE_RNG_TRACE: log the call stack of every global RNG draw
	///                       during the first N ticks to "<log>.rngtrace" (resolve with Tools/Determinism/symbolize_trace.py).
	///   CCCP_DT_TRACE_FROM  With CCCP_DT_TRACE_TICKS, only trace from this tick on (tracing slows the game, which can hide timing-dependent bugs).
	class DeterminismHarness {

	public:
		/// Reads configuration from the environment. Must be called once at startup before any other harness function.
		static void Initialize();

		/// Whether the harness is active for this run.
		static bool IsEnabled() { return s_Enabled; }

		/// The number of sim ticks completed since the test Activity started.
		static long long GetTick() { return s_Tick; }

		/// Whether the harness only logs hashes, without launching an Activity or controlling sim stepping.
		static bool IsObserveOnly() { return s_ObserveOnly; }

		/// Configures and queues the test Activity to be started by the game loop.
		/// @return Whether the Activity was set up successfully.
		static bool SetupActivity();

		/// Called once at the end of every sim update. Hashes and logs sim state, and requests quit when done.
		static void EndOfSimUpdate();

		/// Gets how many sim updates the harness wants to run before the next frame is drawn.
		/// @return The number of sim updates to run this frame, or -1 if the harness isn't controlling sim stepping.
		static int GetSimUpdatesForThisFrame();

		/// Sleeps for a random 0 to CCCP_DT_FRAME_JITTER_MS milliseconds, if set, so that a machine draws an uneven number of frames between sim updates. Call once per frame.
		static void SleepFrameJitter();

		/// Hashes of the simulation state. Any difference in object state, RNG state or (optionally) terrain shows up as a different hash.
		struct SimStateHashes {
			uint64_t RNG = 0; //!< The global random generator.
			uint64_t LuaRNG = 0; //!< All the Lua states' random generators.
			uint64_t Actors = 0; //!< All actors.
			uint64_t Items = 0; //!< All items.
			uint64_t Particles = 0; //!< All particles.
			uint64_t Terrain = 0; //!< The terrain material layer, if requested.
			uint64_t Activity = 0; //!< Activity state: team funds and deaths, each player's brain and controlled actor.
			uint64_t Combined = 0; //!< All of the above combined.
			size_t ActorCount = 0; //!< Number of actors.
			size_t ItemCount = 0; //!< Number of items.
			size_t ParticleCount = 0; //!< Number of particles.
		};

		/// Hashes the current simulation state. Usable whether or not the harness is enabled (e.g. for lockstep desync detection).
		/// @param hashTerrain Whether to also hash the terrain material layer, which is comparatively expensive.
		/// @return The hashes.
		static SimStateHashes HashSimState(bool hashTerrain);

		/// Writes every MovableObject's state, and the raw terrain material layer to "<path>.terrain", for diffing between machines or runs.
		/// @param path The file to write the dump to.
		static void WriteStateDump(const std::string& path);

		/// Writes the dumps kept for CCCP_DT_DUMP_RING, if any, to "<pathPrefix>.ring<tick>".
		/// @param pathPrefix The start of the dump file paths.
		static void WriteDumpRing(const std::string& pathPrefix);

		/// Testing aid, active with CCCP_DT_LOCAL_AUDIT=1 during deterministic (co-op) play: checks that the code only this computer runs (drawing its
		/// player's screen, polling its devices, networking) leaves the simulation alone. Marks the start of such code, and takes the simulation state hashes.
		/// @param section What the code does, for the report.
		static void BeginLocalOnly(const char* section);

		/// Marks the end of code started with BeginLocalOnly. With CCCP_DT_LOCAL_AUDIT, reports any change to the simulation state hashes in between.
		static void EndLocalOnly();

		/// Called where the simulation is changed in ways the state hashes don't show (unique IDs handed out, scripts run, Lua random numbers drawn).
		/// With CCCP_DT_LOCAL_AUDIT, reports it, with a stack trace, if it happens on the main thread between BeginLocalOnly and EndLocalOnly.
		/// @param what What happened, for the report.
		static void CheckSimAccess(const char* what);

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
		static size_t s_DumpRingSize; //!< How many recent ticks' per-MO dumps to keep in memory. 0 for none.
		static std::deque<std::pair<long long, std::string>> s_DumpRing; //!< The recent ticks' per-MO dumps, oldest first.
		static uint64_t s_LastTerrainHash; //!< Most recently computed terrain hash.
		static bool s_ObserveOnly; //!< Whether the harness only logs hashes, without launching an Activity or controlling stepping.
		static bool s_RandomTicksPerFrame; //!< Whether to run a random number of sim updates per frame.
		static std::minstd_rand s_FrameRNG; //!< Generator for the random ticks-per-frame sequence. Separate from all sim generators.
		static int s_FrameJitterMS; //!< Maximum random sleep per frame, in milliseconds. 0 for none.
	};
} // namespace RTE
