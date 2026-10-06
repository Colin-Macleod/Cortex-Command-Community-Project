#include "DeterminismHarness.h"

#include "ActivityMan.h"
#include "ConsoleMan.h"
#include "GameActivity.h"
#include "LuaMan.h"
#include "MovableMan.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "ThreadMan.h"
#include "TimerMan.h"
#include "SLTerrain.h"
#include "Actor.h"
#include "AHuman.h"
#include "Attachable.h"
#include "HeldDevice.h"
#include "MOSprite.h"
#include "Scene.h"
#include "System.h"
#include "RTETools.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <thread>

#ifdef __linux__
#include <execinfo.h>
#endif

#ifdef RTE_RNG_TRACE
#include <dlfcn.h>
#include <mutex>
#endif

using namespace RTE;

bool DeterminismHarness::s_Enabled = false;
std::ofstream DeterminismHarness::s_Log;
std::string DeterminismHarness::s_LogPath;
long long DeterminismHarness::s_TicksToRun = 3000;
long long DeterminismHarness::s_Tick = 0;
int DeterminismHarness::s_TerrainEvery = 10;
bool DeterminismHarness::s_Fog = true;
bool DeterminismHarness::s_SyncBeforeTerrainHash = false;
std::string DeterminismHarness::s_ActivityName = "Bunker Breach";
std::string DeterminismHarness::s_SceneName = "Zekarra Mining Outpost";
std::set<long long> DeterminismHarness::s_DumpTicks;
size_t DeterminismHarness::s_DumpRingSize = 0;
std::deque<std::pair<long long, std::string>> DeterminismHarness::s_DumpRing;
uint64_t DeterminismHarness::s_LastTerrainHash = 0;
bool DeterminismHarness::s_ObserveOnly = false;
bool DeterminismHarness::s_RandomTicksPerFrame = false;
std::minstd_rand DeterminismHarness::s_FrameRNG;
int DeterminismHarness::s_FrameJitterMS = 0;
long long DeterminismHarness::s_SaveSettingsAtTick = -1;

namespace {
	/// FNV-1a, 64 bit. Hashes the exact bytes so any float bit difference shows up.
	struct Hasher {
		uint64_t m_Hash = 14695981039346656037ULL;

		void Bytes(const void* data, size_t size) {
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i) {
				m_Hash ^= bytes[i];
				m_Hash *= 1099511628211ULL;
			}
		}
		template <typename T> void Add(const T& value) { Bytes(&value, sizeof(T)); }
		void Add(const std::string& value) { Bytes(value.data(), value.size()); }
	};

	/// Hashes the identity and physical state of a single MovableObject.
	void HashMO(Hasher& hasher, const MovableObject* mo) {
		hasher.Add(mo->GetUniqueID());
		hasher.Add(mo->GetPresetName());
		hasher.Add(mo->GetPos().m_X);
		hasher.Add(mo->GetPos().m_Y);
		hasher.Add(mo->GetVel().m_X);
		hasher.Add(mo->GetVel().m_Y);
		hasher.Add(mo->GetRotAngle());
		hasher.Add(mo->GetAngularVel());
		hasher.Add(mo->GetMass());
		hasher.Add(mo->GetTeam());
		hasher.Add(mo->IsSetToDelete());
		if (const Actor* actor = dynamic_cast<const Actor*>(mo)) {
			hasher.Add(actor->GetHealth());
			hasher.Add(actor->GetStatus());
			hasher.Add(actor->GetAIMode());
			hasher.Add(actor->GetAimAngle(false));
			hasher.Add(actor->GetMovePathSize());
			hasher.Add(const_cast<Actor*>(actor)->GetWaypointsSize());
			// What it carries, so a desync in pickups, purchases or dropped items is caught before it shows up in the world.
			for (const MovableObject* item: *actor->GetInventory()) {
				hasher.Add(item->GetUniqueID());
				hasher.Add(item->GetPresetName());
			}
			if (const AHuman* human = dynamic_cast<const AHuman*>(actor); human && human->GetEquippedItem()) {
				hasher.Add(human->GetEquippedItem()->GetUniqueID());
			}
		}
		if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
			hasher.Add(rotating->GetAttachableList().size());
			for (const Attachable* attachable: rotating->GetAttachableList()) {
				hasher.Add(attachable->GetUniqueID());
				hasher.Add(attachable->GetMass());
			}
		}
	}

	/// Hashes the Activity's game state that isn't in movable objects.
	uint64_t HashActivity() {
		Hasher hasher;
		Activity* activity = g_ActivityMan.GetActivity();
		if (!activity) {
			return hasher.m_Hash;
		}
		hasher.Add(activity->GetActivityState());
		for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
			hasher.Add(activity->GetTeamFunds(team));
			hasher.Add(activity->GetTeamDeathCount(team));
		}
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			// These pointers can outlive their actor, so only read actors that still exist.
			const Actor* brain = activity->GetPlayerBrain(player);
			hasher.Add(g_MovableMan.IsActor(brain) ? brain->GetUniqueID() : 0L);
			const Actor* controlled = activity->GetControlledActor(player);
			hasher.Add(g_MovableMan.IsActor(controlled) ? controlled->GetUniqueID() : 0L);
		}
		return hasher.m_Hash;
	}

	/// Hashes the state of a RandomGenerator without disturbing it, by sampling from a copy.
	uint64_t HashRNG(const RandomGenerator& rng) {
		RandomGenerator copy = rng;
		Hasher hasher;
		for (int i = 0; i < 4; ++i) {
			hasher.Add(copy.RandomNum<int>(0, std::numeric_limits<int>::max()));
		}
		return hasher.m_Hash;
	}

	std::string FormatMO(const MovableObject* mo) {
		char buffer[512];
		const Actor* actor = dynamic_cast<const Actor*>(mo);
		std::snprintf(buffer, sizeof(buffer), "uid=%ld cls=%s preset=\"%s\" team=%d pos=(%.9g,%.9g) vel=(%.9g,%.9g) rot=%.9g angvel=%.9g mass=%.9g health=%.9g",
		              mo->GetUniqueID(), mo->GetClassName().c_str(), mo->GetPresetName().c_str(), mo->GetTeam(),
		              mo->GetPos().m_X, mo->GetPos().m_Y, mo->GetVel().m_X, mo->GetVel().m_Y,
		              mo->GetRotAngle(), mo->GetAngularVel(), mo->GetMass(), actor ? actor->GetHealth() : 0.0F);
		return buffer;
	}
} // namespace

#ifdef RTE_RNG_TRACE
namespace {
	std::mutex s_TraceMutex;
	std::ofstream s_Trace;
	long long s_TraceTicks = 0;
	long long s_TraceFromTick = 0;
	std::thread::id s_MainThread;
} // namespace

/// Records the call stack of every draw from the global RNG (and which thread made it) during the first CCCP_DT_TRACE_TICKS ticks.
/// Addresses are written as offsets into the module so they can be resolved with addr2line.
void RTE::RNGTraceHook(const RandomGenerator* generator) {
	if (!DeterminismHarness::IsEnabled() || !s_Trace.is_open()) {
		return;
	}
	if (const long long currentTick = DeterminismHarness::GetTick(); currentTick < s_TraceFromTick || currentTick >= s_TraceTicks) {
		return;
	}
	// Identify the generator: the global one, a Lua state's, or something else (cosmetic or temporary generators, which aren't traced).
	std::string generatorName;
	std::string scriptPath;
	if (generator == &g_RandomGenerator) {
		generatorName = "G";
	} else if (generator == &g_LuaMan.GetMasterScriptState().GetRandomGenerator()) {
		generatorName = "Lm";
		scriptPath = g_LuaMan.GetMasterScriptState().GetCurrentlyRunningScriptFilePath();
	} else {
		const LuaStatesArray& luaStates = g_LuaMan.GetThreadedScriptStates();
		for (size_t i = 0; i < luaStates.size(); ++i) {
			if (generator == &luaStates[i].GetRandomGenerator()) {
				generatorName = "L" + std::to_string(i);
				scriptPath = luaStates[i].GetCurrentlyRunningScriptFilePath();
				break;
			}
		}
	}
	// Keep the record a single whitespace-separated token for the script.
	std::replace(scriptPath.begin(), scriptPath.end(), ' ', '_');
	if (generatorName.empty()) {
		return;
	}
	long long tick = DeterminismHarness::GetTick();
	if (tick >= s_TraceTicks) {
		return;
	}
	void* frames[10];
	int frameCount = backtrace(frames, 10);
	std::scoped_lock lock(s_TraceMutex);
	s_Trace << tick << (std::this_thread::get_id() == s_MainThread ? " M" : " W") << ":" << generatorName;
	if (!scriptPath.empty()) {
		s_Trace << " lua:" << scriptPath;
	}
	for (int i = 2; i < frameCount; ++i) {
		Dl_info info;
		if (dladdr(frames[i], &info) && info.dli_fbase) {
			s_Trace << " 0x" << std::hex << (reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase)) << std::dec;
		}
	}
	s_Trace << "\n";
}
#endif

void DeterminismHarness::Initialize() {
	const char* logPath = std::getenv("CCCP_DT_LOG");
	if (!logPath || logPath[0] == '\0') {
		return;
	}
	s_Enabled = true;
	s_LogPath = logPath;
	s_Log.open(s_LogPath, std::ios::out | std::ios::trunc);

	if (const char* value = std::getenv("CCCP_DT_TICKS")) {
		s_TicksToRun = std::atoll(value);
	}
	if (const char* value = std::getenv("CCCP_DT_ACTIVITY")) {
		s_ActivityName = value;
	}
	if (const char* value = std::getenv("CCCP_DT_SCENE")) {
		s_SceneName = value;
	}
	if (const char* value = std::getenv("CCCP_DT_FOG")) {
		s_Fog = std::atoi(value) != 0;
	}
	if (const char* value = std::getenv("CCCP_DT_SYNC_TERRAIN_HASH")) {
		s_SyncBeforeTerrainHash = std::atoi(value) != 0;
	}
	if (const char* value = std::getenv("CCCP_DT_TERRAIN_EVERY")) {
		s_TerrainEvery = std::atoi(value);
	}
	if (const char* value = std::getenv("CCCP_DT_OBSERVE")) {
		s_ObserveOnly = std::atoi(value) != 0;
	}
	if (const char* value = std::getenv("CCCP_DT_TICKS_PER_FRAME")) {
		s_RandomTicksPerFrame = std::string(value) == "random";
	}
	if (const char* value = std::getenv("CCCP_DT_FRAME_JITTER_MS")) {
		s_FrameJitterMS = std::max(0, std::atoi(value));
	}
	if (const char* value = std::getenv("CCCP_DT_SAVE_SETTINGS_AT")) {
		s_SaveSettingsAtTick = std::atoll(value);
	}
	if (const char* value = std::getenv("CCCP_DT_FRAME_SEED")) {
		s_FrameRNG.seed(static_cast<unsigned int>(std::atoll(value)));
	}
	if (!s_ObserveOnly) {
		// Run the way a lockstep session does. In observe mode whatever is driving the game (e.g. a co-op session) is responsible for this.
		bool deterministic = true;
		if (const char* value = std::getenv("CCCP_DT_DETERMINISTIC")) {
			deterministic = std::atoi(value) != 0;
		}
		g_TimerMan.SetDeterministicMode(deterministic);
	}
	if (const char* value = std::getenv("CCCP_DT_DUMP_RING")) {
		s_DumpRingSize = static_cast<size_t>(std::max(0, std::atoi(value)));
	}
	if (const char* value = std::getenv("CCCP_DT_DUMP_TICKS")) {
		std::stringstream stream(value);
		std::string item;
		while (std::getline(stream, item, ',')) {
			if (!item.empty()) {
				s_DumpTicks.insert(std::atoll(item.c_str()));
			}
		}
	}
#ifdef RTE_RNG_TRACE
	if (const char* value = std::getenv("CCCP_DT_TRACE_TICKS")) {
		s_TraceTicks = std::atoll(value);
		if (const char* fromValue = std::getenv("CCCP_DT_TRACE_FROM")) {
			s_TraceFromTick = std::atoll(fromValue);
		}
		s_MainThread = std::this_thread::get_id();
		s_Trace.open(s_LogPath + ".rngtrace", std::ios::out | std::ios::trunc);
	}
#endif
	s_Log << "# CCCP determinism log. activity=\"" << s_ActivityName << "\" scene=\"" << s_SceneName << "\" fog=" << s_Fog << " luaStates=" << g_LuaMan.GetThreadedScriptStates().size()
	      << " observe=" << s_ObserveOnly << " deterministic=" << g_TimerMan.IsInDeterministicMode() << " ticksPerFrame=" << (s_RandomTicksPerFrame ? "random" : "1") << "\n";
	s_Log << "# tick actors items particles | rng luaRng actorsHash itemsHash particlesHash terrainHash | combined\n";
}

bool DeterminismHarness::SetupActivity() {
	const Entity* activityPreset = g_PresetMan.GetEntityPreset("GAScripted", s_ActivityName);
	const Scene* scenePreset = dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", s_SceneName));
	if (!activityPreset || !scenePreset) {
		s_Log << "# ERROR: could not find activity or scene preset\n";
		s_Log.flush();
		return false;
	}
	GameActivity* gameActivity = dynamic_cast<GameActivity*>(activityPreset->Clone());
	gameActivity->SetDifficulty(Activity::DifficultySetting::MediumDifficulty);
	gameActivity->SetStartingGold(5000);
	gameActivity->SetRequireClearPathToOrbit(false);
	gameActivity->SetFogOfWarEnabled(s_Fog);
	g_SceneMan.SetSceneToLoad(scenePreset, true, true);

	// One idle "human" player (no input arrives, so its brain just sits there) versus a CPU team. All other units on both sides are AI-driven.
	gameActivity->ClearPlayers(false);
	gameActivity->AddPlayer(Players::PlayerOne, true, Activity::Teams::TeamOne, 0);
	gameActivity->SetCPUTeam(Activity::Teams::TeamTwo);
	gameActivity->SetTeamTech(Activity::Teams::TeamOne, "Coalition.rte");
	gameActivity->SetTeamTech(Activity::Teams::TeamTwo, "Ronin.rte");
	for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
		gameActivity->SetTeamAISkill(team, Activity::AISkillSetting::DefaultSkill);
	}
	g_ActivityMan.SetStartActivity(gameActivity);
	g_ActivityMan.SetRestartActivity();
	return true;
}

int DeterminismHarness::GetSimUpdatesForThisFrame() {
	if (!s_Enabled || s_ObserveOnly) {
		return -1;
	}
	// Either exactly one sim update per frame, or a random 0-3 to check that nothing in the sim depends on how many updates happen between draws.
	return s_RandomTicksPerFrame ? static_cast<int>(s_FrameRNG() % 4) : 1;
}

void DeterminismHarness::SleepFrameJitter() {
	if (s_Enabled && s_FrameJitterMS > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(s_FrameRNG() % (s_FrameJitterMS + 1)));
	}
}

DeterminismHarness::SimStateHashes DeterminismHarness::HashSimState(bool hashTerrain) {
	SimStateHashes hashes;
	hashes.ActorCount = g_MovableMan.m_Actors.size();
	hashes.ItemCount = g_MovableMan.m_Items.size();
	hashes.ParticleCount = g_MovableMan.m_Particles.size();

	Hasher actorsHash;
	for (const Actor* actor: g_MovableMan.m_Actors) {
		HashMO(actorsHash, actor);
	}
	hashes.Actors = actorsHash.m_Hash;
	Hasher itemsHash;
	for (const MovableObject* item: g_MovableMan.m_Items) {
		HashMO(itemsHash, item);
	}
	hashes.Items = itemsHash.m_Hash;
	Hasher particlesHash;
	for (const MovableObject* particle: g_MovableMan.m_Particles) {
		HashMO(particlesHash, particle);
	}
	hashes.Particles = particlesHash.m_Hash;

	hashes.RNG = HashRNG(g_RandomGenerator);
	Hasher luaRngHash;
	luaRngHash.Add(HashRNG(g_LuaMan.GetMasterScriptState().m_RandomGenerator));
	for (const LuaStateWrapper& luaState: g_LuaMan.GetThreadedScriptStates()) {
		luaRngHash.Add(HashRNG(luaState.m_RandomGenerator));
	}
	hashes.LuaRNG = luaRngHash.m_Hash;
	hashes.Activity = HashActivity();

	if (hashTerrain && g_SceneMan.GetTerrain()) {
		const BITMAP* materialBitmap = g_SceneMan.GetTerrain()->GetMaterialBitmap();
		Hasher terrainHash;
		for (int y = 0; y < materialBitmap->h; ++y) {
			terrainHash.Bytes(materialBitmap->line[y], materialBitmap->w);
		}
		hashes.Terrain = terrainHash.m_Hash;
	}

	Hasher combined;
	combined.Add(hashes.RNG);
	combined.Add(hashes.LuaRNG);
	combined.Add(hashes.Actors);
	combined.Add(hashes.Items);
	combined.Add(hashes.Particles);
	combined.Add(hashes.Terrain);
	combined.Add(hashes.Activity);
	hashes.Combined = combined.m_Hash;
	return hashes;
}

void DeterminismHarness::EndOfSimUpdate() {
	if (!s_Enabled || !g_ActivityMan.IsInActivity()) {
		return;
	}
	++s_Tick;

	bool hashTerrain = s_TerrainEvery > 0 && (s_Tick % s_TerrainEvery) == 0 && g_SceneMan.GetTerrain();
	if (hashTerrain && s_SyncBeforeTerrainHash) {
		// Make sure no in-flight background work can be touching the terrain while we read it.
		g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
		g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();
	}
	SimStateHashes hashes = HashSimState(hashTerrain);
	if (hashTerrain) {
		s_LastTerrainHash = hashes.Terrain;
	}

	Hasher combined;
	combined.Add(hashes.RNG);
	combined.Add(hashes.LuaRNG);
	combined.Add(hashes.Actors);
	combined.Add(hashes.Items);
	combined.Add(hashes.Particles);
	combined.Add(s_LastTerrainHash);
	combined.Add(hashes.Activity);

	char line[512];
	std::snprintf(line, sizeof(line), "%lld %zu %zu %zu | %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " | %016" PRIx64 "\n",
	              s_Tick, hashes.ActorCount, hashes.ItemCount, hashes.ParticleCount,
	              hashes.RNG, hashes.LuaRNG, hashes.Actors, hashes.Items, hashes.Particles, s_LastTerrainHash, hashes.Activity, combined.m_Hash);
	s_Log << line;

	if (s_Tick == s_SaveSettingsAtTick) {
		g_SettingsMan.UpdateSettingsFile();
		g_ConsoleMan.PrintString("DETERMINISM: Wrote the settings file at tick " + std::to_string(s_Tick) + ".");
	}
	if (s_DumpTicks.count(s_Tick)) {
		WriteStateDump(s_LogPath + ".dump" + std::to_string(s_Tick));
	}
	if (s_DumpRingSize > 0) {
		std::string dump = "# tick " + std::to_string(s_Tick) + "\n# actors\n";
		for (const Actor* actor: g_MovableMan.m_Actors) {
			dump += FormatMO(actor) + "\n";
		}
		dump += "# items\n";
		for (const MovableObject* item: g_MovableMan.m_Items) {
			dump += FormatMO(item) + "\n";
		}
		dump += "# particles\n";
		for (const MovableObject* particle: g_MovableMan.m_Particles) {
			dump += FormatMO(particle) + "\n";
		}
		s_DumpRing.emplace_back(s_Tick, std::move(dump));
		while (s_DumpRing.size() > s_DumpRingSize) {
			s_DumpRing.pop_front();
		}
	}
	if (s_Tick >= s_TicksToRun) {
		s_Log.flush();
		System::SetQuit(true);
	}
}

void DeterminismHarness::WriteDumpRing(const std::string& pathPrefix) {
	for (const auto& [tick, dump]: s_DumpRing) {
		std::ofstream file(pathPrefix + ".ring" + std::to_string(tick), std::ios::out | std::ios::trunc);
		file << dump;
	}
}

void DeterminismHarness::WriteStateDump(const std::string& path) {
	std::ofstream dump(path, std::ios::out | std::ios::trunc);
	dump << "# sim update " << g_TimerMan.GetSimUpdateCount() << "\n# actors\n";
	for (const Actor* actor: g_MovableMan.m_Actors) {
		dump << FormatMO(actor) << "\n";
	}
	dump << "# items\n";
	for (const MovableObject* item: g_MovableMan.m_Items) {
		dump << FormatMO(item) << "\n";
	}
	dump << "# particles\n";
	for (const MovableObject* particle: g_MovableMan.m_Particles) {
		dump << FormatMO(particle) << "\n";
	}

	if (g_SceneMan.GetTerrain()) {
		// Raw 8-bit material layer, row by row, preceded by width and height as 32-bit ints.
		const BITMAP* materialBitmap = g_SceneMan.GetTerrain()->GetMaterialBitmap();
		std::ofstream terrainDump(path + ".terrain", std::ios::out | std::ios::trunc | std::ios::binary);
		int32_t dimensions[2] = {materialBitmap->w, materialBitmap->h};
		terrainDump.write(reinterpret_cast<const char*>(dimensions), sizeof(dimensions));
		for (int y = 0; y < materialBitmap->h; ++y) {
			terrainDump.write(reinterpret_cast<const char*>(materialBitmap->line[y]), materialBitmap->w);
		}
	}
}

namespace {
	/// State of the CCCP_DT_LOCAL_AUDIT testing aid.
	struct LocalAudit {
		bool Enabled = false; //!< Whether CCCP_DT_LOCAL_AUDIT is set.
		std::thread::id MainThread; //!< The thread that runs the game loop.
		const char* Section = nullptr; //!< The code only this computer runs that's running now, if any.
		const Activity* SectionActivity = nullptr; //!< The Activity when the section started. If it changed, the section started or ended a match.
		long long SectionSimUpdate = 0; //!< The sim update count when the section started.
		DeterminismHarness::SimStateHashes SectionHashes; //!< The simulation state hashes when the section started.
		std::set<std::string> Reported; //!< What was already reported, so each problem is reported once.
		int ReportCount = 0; //!< How many reports were made, to stop at some point.

		LocalAudit() {
			const char* value = std::getenv("CCCP_DT_LOCAL_AUDIT");
			Enabled = value && std::atoi(value) != 0;
			MainThread = std::this_thread::get_id();
		}

		/// Whether the audit applies right now: deterministic play in an Activity.
		static bool Applies() { return g_TimerMan.IsInDeterministicMode() && g_ActivityMan.IsInActivity() && g_ActivityMan.GetActivity(); }

		/// Prints a report to standard error and the console, once per distinct key, with a stack trace if asked.
		void Report(const std::string& key, const std::string& message, bool withStackTrace) {
			if (ReportCount >= 50 || !Reported.insert(key).second) {
				return;
			}
			++ReportCount;
			std::fprintf(stderr, "LOCALAUDIT: %s\n", message.c_str());
#ifdef __linux__
			if (withStackTrace) {
				// Resolve the "(+0x...)" offsets with addr2line -f -C -e <binary>.
				void* frames[48];
				int frameCount = backtrace(frames, 48);
				if (char** symbols = backtrace_symbols(frames, frameCount)) {
					for (int i = 1; i < frameCount; ++i) {
						std::fprintf(stderr, "LOCALAUDIT stack: %s\n", symbols[i]);
					}
					std::free(symbols);
				}
			}
#endif
			std::fflush(stderr);
			g_ConsoleMan.PrintString("ERROR: " + message);
		}
	};

	LocalAudit& GetLocalAudit() {
		static LocalAudit localAudit;
		return localAudit;
	}
} // namespace

void DeterminismHarness::BeginLocalOnly(const char* section) {
	LocalAudit& audit = GetLocalAudit();
	if (!audit.Enabled || std::this_thread::get_id() != audit.MainThread) {
		return;
	}
	audit.Section = nullptr;
	if (!LocalAudit::Applies()) {
		return;
	}
	audit.SectionActivity = g_ActivityMan.GetActivity();
	audit.SectionSimUpdate = g_TimerMan.GetSimUpdateCount();
	audit.SectionHashes = HashSimState(false);
	audit.Section = section;
}

void DeterminismHarness::EndLocalOnly() {
	LocalAudit& audit = GetLocalAudit();
	if (!audit.Enabled || !audit.Section || std::this_thread::get_id() != audit.MainThread) {
		return;
	}
	const char* section = audit.Section;
	audit.Section = nullptr;
	if (!LocalAudit::Applies() || g_ActivityMan.GetActivity() != audit.SectionActivity || g_TimerMan.GetSimUpdateCount() != audit.SectionSimUpdate) {
		return;
	}
	SimStateHashes hashes = HashSimState(false);
	const std::pair<const char*, bool> parts[] = {{"RNG", hashes.RNG != audit.SectionHashes.RNG}, {"Lua RNG", hashes.LuaRNG != audit.SectionHashes.LuaRNG},
	                                              {"actors", hashes.Actors != audit.SectionHashes.Actors || hashes.ActorCount != audit.SectionHashes.ActorCount},
	                                              {"items", hashes.Items != audit.SectionHashes.Items || hashes.ItemCount != audit.SectionHashes.ItemCount},
	                                              {"particles", hashes.Particles != audit.SectionHashes.Particles || hashes.ParticleCount != audit.SectionHashes.ParticleCount},
	                                              {"activity", hashes.Activity != audit.SectionHashes.Activity}};
	for (const auto& [part, changed]: parts) {
		if (changed) {
			audit.Report(std::string(section) + "/" + part, std::string(section) + " changed the simulation state (" + part + ") at sim update " + std::to_string(g_TimerMan.GetSimUpdateCount()), false);
		}
	}
}

void DeterminismHarness::CheckSimAccess(const char* what) {
	LocalAudit& audit = GetLocalAudit();
	if (!audit.Enabled || !audit.Section || std::this_thread::get_id() != audit.MainThread) {
		return;
	}
	if (const Activity* activity = g_ActivityMan.GetActivity(); !activity || activity->GetActivityState() == Activity::ActivityState::Over) {
		// Ending a match (e.g. when the connection to the host is lost) runs the Activity's end scripts outside a sim update, on purpose.
		return;
	}
	std::string key = std::string(audit.Section) + "/" + what;
#ifdef __linux__
	// Report each call site once: key on the return addresses of the few innermost frames.
	void* frames[6];
	int frameCount = backtrace(frames, 6);
	for (int i = 1; i < frameCount; ++i) {
		char address[32];
		std::snprintf(address, sizeof(address), " %p", frames[i]);
		key += address;
	}
#endif
	audit.Report(key, std::string(what) + " during " + audit.Section + " at sim update " + std::to_string(g_TimerMan.GetSimUpdateCount()), true);
}
