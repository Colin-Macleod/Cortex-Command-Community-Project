// RakNet includes <windows.h> on Windows (see below). Both build systems define these already; this makes sure they're in effect even if this file is built some other way.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "LockstepMan.h"
#include "MathConsistency.h"
#include "lua.hpp"

#include "ActivityMan.h"
#include "AudioMan.h"
#include "ConsoleMan.h"
#include "DataModule.h"
#include "DeterminismHarness.h"
#include "FrameMan.h"
#include "GameActivity.h"
#include "GameVersion.h"
#include "LuaMan.h"
#include "MovableMan.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "WindowMan.h"
#include "System.h"
#include "AllegroBitmap.h"
#include "GUIFont.h"

#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"

// RakNet includes <WinSock2.h> and <windows.h> on Windows, after the engine headers above. Undefine the Win32 A/W macros that have the same names as engine
// methods, or calls to those methods below (e.g. Entity::GetClassName) would be renamed (GetClassNameA) and fail to compile. See also NetworkServer.h.
#ifdef GetClassName
#undef GetClassName
#endif
#ifdef GetObject
#undef GetObject
#endif
#ifdef SendMessage
#undef SendMessage
#endif
#ifdef GetMessage
#undef GetMessage
#endif
#ifdef LoadString
#undef LoadString
#endif
#ifdef PlaySound
#undef PlaySound
#endif
#ifdef DrawText
#undef DrawText
#endif
#ifdef CreateFont
#undef CreateFont
#endif
#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <list>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace RTE;

namespace {
	constexpr uint32_t c_ProtocolVersion = 3; //!< Bump whenever the message format changes.
	constexpr unsigned short c_DefaultPort = 7777; //!< Default UDP port.
	constexpr int c_MaxClients = Players::MaxPlayerCount - 1; //!< At most one player per peer.
	constexpr int c_InputTimeoutMS = 3000; //!< How long the host waits for a player's late input before repeating their previous input.
	constexpr int c_WaitingOverlayDelayMS = 250; //!< How long to wait for input before showing "waiting for players".
	constexpr int c_LeaveConfirmMS = 3000; //!< How long a first Esc press stays armed.
	constexpr int c_ConnectionTimeoutMS = 30000; //!< How long a connection may go silent before it's considered lost. Generous, as loading a big scene on a slow machine can starve the network thread.
	constexpr int c_ReconnectIntervalMS = 2000; //!< How often a client retries connecting to a host that isn't up yet.
	constexpr int c_LagIdleMS = 10000; //!< After lagging this long, a player's held input is no longer repeated, so their actor stops instead of e.g. firing forever.
	constexpr long long c_MaxInputLead = 600; //!< Input or checksums for sim updates further ahead than this are ignored (a buggy or malicious peer could otherwise grow the queues without bound).
	constexpr int c_StalledEscapeDelayMS = 3000; //!< How long the sim must have been stalled before Esc is read outside the sim update.

	constexpr int c_MaxResolutionRequests = 2; //!< How many times a client tries switching to the host's resolution before giving up.
	constexpr int c_MinResolution = 200; //!< Smallest resolution (each way) a client accepts switching to.
	constexpr int c_MaxResolution = 16384; //!< Largest resolution (each way) a client accepts switching to.

	static_assert(InputElements::INPUT_COUNT <= 64, "VirtualInputFrame keeps input elements in 64-bit masks.");

	/// Hashes the contents of every file in a module that can affect the simulation, i.e. everything but sounds and music.
	/// Line endings in text files are ignored, so a checkout with Windows line endings matches one with Unix line endings.
	/// @param modulePath Path to the module's directory.
	/// @return The hash, or 0 if the directory couldn't be read.
	uint64_t HashModuleContents(const std::string& modulePath) {
		auto lowercase = [](std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return text;
		};
		std::vector<std::string> files;
		std::error_code error;
		for (auto itr = std::filesystem::recursive_directory_iterator(modulePath, error); !error && itr != std::filesystem::recursive_directory_iterator(); itr.increment(error)) {
			std::error_code typeError;
			if (!itr->is_regular_file(typeError)) {
				continue;
			}
			const std::string extension = lowercase(itr->path().extension().string());
			if (extension == ".flac" || extension == ".ogg" || extension == ".wav" || extension == ".mp3" || extension == ".reapeaks") {
				continue;
			}
			files.push_back(std::filesystem::relative(itr->path(), modulePath, typeError).generic_string());
		}
		if (error) {
			return 0;
		}
		// Directory iteration order differs between file systems.
		std::sort(files.begin(), files.end());

		uint64_t hash = 1469598103934665603ULL;
		auto mixBytes = [&hash](const char* bytes, size_t size) {
			for (size_t i = 0; i < size; ++i) {
				hash = (hash ^ static_cast<unsigned char>(bytes[i])) * 1099511628211ULL;
			}
		};
		std::vector<char> contents;
		for (const std::string& file: files) {
			mixBytes(file.data(), file.size() + 1);
			std::ifstream stream(modulePath + "/" + file, std::ios::binary);
			contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			const std::string extension = lowercase(std::filesystem::path(file).extension().string());
			if (extension == ".ini" || extension == ".lua" || extension == ".txt" || extension == ".frag" || extension == ".vert" || extension == ".json") {
				contents.erase(std::remove(contents.begin(), contents.end(), '\r'), contents.end());
			}
			const uint64_t size = contents.size();
			mixBytes(reinterpret_cast<const char*>(&size), sizeof(size));
			// Eight bytes at a time; byte by byte would take seconds for the official content.
			size_t offset = 0;
			for (; offset + 8 <= contents.size(); offset += 8) {
				uint64_t word;
				std::memcpy(&word, contents.data() + offset, 8);
				hash = (hash ^ word) * 1099511628211ULL;
				hash ^= hash >> 29;
			}
			mixBytes(contents.data() + offset, contents.size() - offset);
		}
		return hash;
	}
	constexpr int c_MinAutoInputDelay = 3; //!< Smallest input delay (in sim updates) the host picks automatically.
	constexpr int c_MaxAutoInputDelay = 20; //!< Largest input delay (in sim updates) the host picks automatically.

	/// Builds a network message: a message type byte followed by plain data.
	class MessageWriter {
	public:
		explicit MessageWriter(uint8_t messageType) { m_Data.push_back(static_cast<uint8_t>(ID_USER_PACKET_ENUM + messageType)); }

		template <typename T> MessageWriter& Write(const T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
			m_Data.insert(m_Data.end(), bytes, bytes + sizeof(T));
			return *this;
		}

		MessageWriter& WriteString(const std::string& value) {
			Write(static_cast<uint32_t>(value.size()));
			m_Data.insert(m_Data.end(), value.begin(), value.end());
			return *this;
		}

		const std::vector<uint8_t>& Data() const { return m_Data; }

	private:
		std::vector<uint8_t> m_Data;
	};

	/// Reads plain data written by MessageWriter. Any read past the end fails and leaves the reader failed.
	class MessageReader {
	public:
		MessageReader(const uint8_t* data, size_t size) :
		    m_Data(data), m_Remaining(size) {}

		template <typename T> bool Read(T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			if (!m_Ok || m_Remaining < sizeof(T)) {
				m_Ok = false;
				return false;
			}
			std::memcpy(&value, m_Data, sizeof(T));
			m_Data += sizeof(T);
			m_Remaining -= sizeof(T);
			return true;
		}

		bool ReadString(std::string& value) {
			uint32_t length = 0;
			if (!Read(length) || m_Remaining < length) {
				m_Ok = false;
				return false;
			}
			value.assign(reinterpret_cast<const char*>(m_Data), length);
			m_Data += length;
			m_Remaining -= length;
			return true;
		}

		bool Ok() const { return m_Ok; }

	private:
		const uint8_t* m_Data;
		size_t m_Remaining;
		bool m_Ok = true;
	};

	/// Parses "key=value" lines.
	std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
		std::map<std::string, std::string> values;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line)) {
			if (size_t separator = line.find('='); separator != std::string::npos) {
				values[line.substr(0, separator)] = line.substr(separator + 1);
			}
		}
		return values;
	}

	int ToInt(const std::map<std::string, std::string>& values, const std::string& key, int fallback = 0) {
		auto itr = values.find(key);
		return itr != values.end() ? std::atoi(itr->second.c_str()) : fallback;
	}

	std::string ToString(const std::map<std::string, std::string>& values, const std::string& key) {
		auto itr = values.find(key);
		return itr != values.end() ? itr->second : std::string();
	}

	long long ElapsedMS(std::chrono::steady_clock::time_point since) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
	}

	/// Names the first module that differs between two compatibility strings, e.g. "Ronin.rte is different" or "Host has MyMod.rte, you don't".
	std::string FirstModuleMismatch(const std::string& hostCompatibility, const std::string& clientCompatibility) {
		auto parseModules = [](const std::string& compatibility) {
			std::map<std::string, std::string> modules;
			const size_t start = compatibility.find("|modules=");
			if (start == std::string::npos) {
				return modules;
			}
			std::istringstream stream(compatibility.substr(start + 9));
			std::string entry;
			while (std::getline(stream, entry, ';')) {
				if (!entry.empty()) {
					const size_t colon = entry.find(':');
					modules[entry.substr(0, colon)] = colon == std::string::npos ? "" : entry.substr(colon + 1);
				}
			}
			return modules;
		};
		const std::map<std::string, std::string> hostModules = parseModules(hostCompatibility);
		const std::map<std::string, std::string> clientModules = parseModules(clientCompatibility);
		for (const auto& [name, hash]: hostModules) {
			auto itr = clientModules.find(name);
			if (itr == clientModules.end()) {
				return "the host has " + name + " enabled, you don't";
			} else if (itr->second != hash) {
				return name + " is different";
			}
		}
		for (const auto& [name, hash]: clientModules) {
			if (hostModules.find(name) == hostModules.end()) {
				return "you have " + name + " enabled, the host doesn't";
			}
		}
		return "load order differs";
	}

	/// Makes a frame received from the network safe to use: no NaNs or infinities, and values in range. Done by the host before bundling, so every peer gets the same values.
	void SanitizeFrame(VirtualInputFrame& frame) {
		auto clean = [](float& value, float limit) {
			value = std::isfinite(value) ? std::clamp(value, -limit, limit) : 0.0F;
		};
		for (int axis = 0; axis < 2; ++axis) {
			clean(frame.AnalogMove[axis], 1.0F);
			clean(frame.AnalogAim[axis], 1.0F);
			clean(frame.MouseMovement[axis], 10000.0F);
		}
		clean(frame.MousePosition[0], 100000.0F);
		clean(frame.MousePosition[1], 100000.0F);
		frame.MousePosition[0] = std::clamp(frame.MousePosition[0], 0.0F, static_cast<float>(g_WindowMan.GetResX() - 1));
		frame.MousePosition[1] = std::clamp(frame.MousePosition[1], 0.0F, static_cast<float>(g_WindowMan.GetResY() - 1));
	}

	/// Folds a later frame into an earlier one queued for the same sim update, keeping the later held state and movement but every press and release.
	void MergeFrameInto(VirtualInputFrame& queued, const VirtualInputFrame& later) {
		const uint64_t pressed = queued.ElementPressed | later.ElementPressed;
		const uint64_t released = queued.ElementReleased | later.ElementReleased;
		const uint8_t mousePressed = queued.MousePressed | later.MousePressed;
		const uint8_t mouseReleased = queued.MouseReleased | later.MouseReleased;
		const float movementX = queued.MouseMovement[0] + later.MouseMovement[0];
		const float movementY = queued.MouseMovement[1] + later.MouseMovement[1];
		queued = later;
		queued.ElementPressed = pressed;
		queued.ElementReleased = released;
		queued.MousePressed = mousePressed;
		queued.MouseReleased = mouseReleased;
		queued.MouseMovement[0] = movementX;
		queued.MouseMovement[1] = movementY;
	}

	/// An input frame that keeps held inputs from the previous one but has no new presses, releases or movement. Used when a player's input is late.
	VirtualInputFrame RepeatFrame(const VirtualInputFrame& previous) {
		VirtualInputFrame frame = previous;
		frame.ElementPressed = 0;
		frame.ElementReleased = 0;
		frame.MousePressed = 0;
		frame.MouseReleased = 0;
		frame.MouseMovement[0] = frame.MouseMovement[1] = 0;
		frame.MouseWheel = 0;
		return frame;
	}
} // namespace

LockstepMan::LockstepMan() {
	m_PlayerOwnerPeer.fill(-2);
	m_PlayerDevices.fill(InputDevice::DEVICE_KEYB_ONLY);
	m_PlayerDigitalAimSpeeds.fill(1.0F);
}

LockstepMan::~LockstepMan() {
	Destroy();
}

void LockstepMan::Destroy() {
	EndMatch();
	if (m_Peer) {
		m_Peer->Shutdown(300);
		RakNet::RakPeerInterface::DestroyInstance(m_Peer);
		m_Peer = nullptr;
	}
	ResetSessionState();
}

void LockstepMan::ResetSessionState() {
	// The host's settings are applied as soon as its match configuration arrives, before the match begins, so leaving in between must restore ours too.
	RestoreLocalSettings();
	if (m_Role == Role::Client && (m_MatchStartPending || m_DeferredMatchStart)) {
		// Don't start the host's Activity on our own after leaving.
		g_ActivityMan.SetRestartActivity(false);
	}
	m_Role = Role::None;
	m_HostGuid = 0;
	m_HostAddress.clear();
	m_ConnectedToHost = false;
	m_HelloSent = false;
	m_StatusMessage.clear();
	m_RejectReason.clear();
	m_Peers.clear();
	m_LobbyPeerCount = 0;
	m_LobbyNames.clear();
	m_ResolutionRequests = 0;
	m_AutoStartDone = false;
	m_MatchStartPending = false;
	m_DeferredMatchStart = false;
	m_DelayedMessages.clear();
}

#pragma region Session Setup

void LockstepMan::HandleCommandLine(int argCount, char** argValue) {
	std::string hostPort;
	std::string joinAddress;
	for (int i = 1; i < argCount; ++i) {
		std::string arg = argValue[i];
		bool hasNext = i + 1 < argCount;
		std::string next = hasNext ? argValue[i + 1] : "";
		if (arg == "-coop-host") {
			hostPort = std::to_string(c_DefaultPort);
			if (hasNext && !next.empty() && std::isdigit(static_cast<unsigned char>(next[0]))) {
				hostPort = next;
				++i;
			}
		} else if (arg == "-coop-join" && hasNext) {
			joinAddress = next;
			++i;
		} else if (arg == "-coop-players" && hasNext) {
			m_Options.ExpectedPlayers = std::max(1, std::atoi(next.c_str()));
			++i;
		} else if (arg == "-coop-activity" && hasNext) {
			m_Options.AutoActivity = next;
			++i;
		} else if (arg == "-coop-scene" && hasNext) {
			m_Options.AutoScene = next;
			++i;
		} else if (arg == "-coop-difficulty" && hasNext) {
			m_Options.AutoDifficulty = std::atoi(next.c_str());
			++i;
		} else if (arg == "-coop-gold" && hasNext) {
			m_Options.AutoGold = std::atoi(next.c_str());
			++i;
		} else if (arg == "-coop-fog" && hasNext) {
			m_Options.AutoFog = std::atoi(next.c_str()) != 0;
			++i;
		} else if (arg == "-coop-delay" && hasNext) {
			m_InputDelay = std::clamp(std::atoi(next.c_str()), 1, 60);
			m_Options.InputDelayFixed = true;
			++i;
		} else if (arg == "-coop-bot" && hasNext) {
			m_Options.BotSeed = std::atoi(next.c_str());
			++i;
		} else if (arg == "-coop-inject-desync" && hasNext) {
			m_Options.DesyncInjectTick = std::atoi(next.c_str());
			++i;
		} else if (arg == "-coop-sim-latency" && hasNext) {
			m_Options.SimulatedLatencyMS = std::max(0, std::atoi(next.c_str()));
			++i;
		} else if (arg == "-coop-sim-jitter" && hasNext) {
			m_Options.SimulatedJitterMS = std::max(0, std::atoi(next.c_str()));
			++i;
		}
	}

	if (!hostPort.empty()) {
		StartHosting(static_cast<unsigned short>(std::atoi(hostPort.c_str())));
	} else if (!joinAddress.empty()) {
		unsigned short port = c_DefaultPort;
		if (size_t colon = joinAddress.rfind(':'); colon != std::string::npos) {
			port = static_cast<unsigned short>(std::atoi(joinAddress.substr(colon + 1).c_str()));
			joinAddress = joinAddress.substr(0, colon);
		}
		StartJoining(joinAddress, port);
	}
	if (IsInSession()) {
		g_SettingsMan.SetSkipIntro(true);
	}
}

bool LockstepMan::StartHosting(unsigned short port) {
	m_Peer = RakNet::RakPeerInterface::GetInstance();
	RakNet::SocketDescriptor socketDescriptor(port, nullptr);
	if (m_Peer->Startup(c_MaxClients, &socketDescriptor, 1) != RakNet::RAKNET_STARTED) {
		g_ConsoleMan.PrintString("ERROR: CO-OP: Could not start hosting on port " + std::to_string(port) + "!");
		RakNet::RakPeerInterface::DestroyInstance(m_Peer);
		m_Peer = nullptr;
		return false;
	}
	m_Peer->SetMaximumIncomingConnections(c_MaxClients);
	m_Peer->SetTimeoutTime(c_ConnectionTimeoutMS, RakNet::UNASSIGNED_SYSTEM_ADDRESS);
	m_Role = Role::Host;
	m_StatusMessage = "Hosting co-op on port " + std::to_string(port);
	g_ConsoleMan.PrintString("CO-OP: " + m_StatusMessage);
	return true;
}

bool LockstepMan::StartJoining(const std::string& address, unsigned short port) {
	m_Peer = RakNet::RakPeerInterface::GetInstance();
	RakNet::SocketDescriptor socketDescriptor;
	if (m_Peer->Startup(1, &socketDescriptor, 1) != RakNet::RAKNET_STARTED) {
		g_ConsoleMan.PrintString("ERROR: CO-OP: Could not start networking!");
		RakNet::RakPeerInterface::DestroyInstance(m_Peer);
		m_Peer = nullptr;
		return false;
	}
	m_Peer->SetTimeoutTime(c_ConnectionTimeoutMS, RakNet::UNASSIGNED_SYSTEM_ADDRESS);
	m_Role = Role::Client;
	m_HostAddress = address + ":" + std::to_string(port);
	m_WaitingSince = std::chrono::steady_clock::now() - std::chrono::milliseconds(c_ReconnectIntervalMS);
	m_StatusMessage = "Connecting to " + m_HostAddress + "...";
	g_ConsoleMan.PrintString("CO-OP: " + m_StatusMessage);
	return true;
}

bool LockstepMan::HostSession(unsigned short port) {
	LeaveSession();
	if (!StartHosting(port)) {
		m_StatusMessage = "Could not host on port " + std::to_string(port) + ". Is another program using it?";
		return false;
	}
	// Computed now rather than when the first player says hello, as hashing the modules takes a moment.
	GetCompatibilityString();
	BroadcastLobbyStatus();
	return true;
}

bool LockstepMan::JoinSession(const std::string& address) {
	LeaveSession();
	std::string host = address;
	unsigned short port = c_DefaultPort;
	// An IPv6 address has colons of its own, so only take a port from [address]:port or address:port with a single colon.
	if (size_t colon = host.rfind(':'); colon != std::string::npos && (host.find(':') == colon || (host.front() == '[' && colon > 0 && host[colon - 1] == ']'))) {
		port = static_cast<unsigned short>(std::atoi(host.substr(colon + 1).c_str()));
		host = host.substr(0, colon);
	}
	if (host.size() > 1 && host.front() == '[' && host.back() == ']') {
		host = host.substr(1, host.size() - 2);
	}
	if (host.empty() || port == 0) {
		m_StatusMessage = "Enter the host's address, e.g. 192.168.1.20 or 192.168.1.20:" + std::to_string(c_DefaultPort) + ".";
		return false;
	}
	GetCompatibilityString();
	if (!StartJoining(host, port)) {
		m_StatusMessage = "Could not start networking.";
		return false;
	}
	return true;
}

void LockstepMan::LeaveSession() {
	if (m_MatchRunning) {
		// Destroy ends the match, which tells the host we left.
		g_ActivityMan.EndActivity();
		g_ActivityMan.SetInActivity(false);
	}
	const bool wasInSession = IsInSession();
	Destroy();
	if (m_LocalResX > 0) {
		if (!g_ActivityMan.IsInActivity()) {
			g_WindowMan.ChangeResolution(m_LocalResX, m_LocalResY, m_LocalResMultiplier, g_WindowMan.IsFullscreen());
			m_ChangedResolution = g_WindowMan.ResolutionChanged();
		}
		m_LocalResX = 0;
		m_LocalResY = 0;
	}
	if (wasInSession) {
		g_ConsoleMan.PrintString("CO-OP: Left the session.");
	}
}

std::vector<std::string> LockstepMan::GetLobbyPlayerNames() const {
	if (m_Role != Role::Host) {
		return m_LobbyNames;
	}
	std::vector<std::string> names{g_SettingsMan.GetCoopPlayerName()};
	for (const Peer& peer: m_Peers) {
		if (peer.Accepted && peer.Connected) {
			names.push_back(peer.Name);
		}
	}
	return names;
}

#pragma endregion

#pragma region Networking

void LockstepMan::Send(const std::vector<uint8_t>& message, uint64_t guid) {
	if (m_Options.SimulatedLatencyMS <= 0 && m_Options.SimulatedJitterMS <= 0) {
		SendNow(message, guid);
		return;
	}
	// Testing aid: hold the message back. Keep send order, as a reliable ordered stream would deliver it.
	int delayMS = m_Options.SimulatedLatencyMS + (m_Options.SimulatedJitterMS > 0 ? static_cast<int>(m_NetworkSimulationRNG() % (m_Options.SimulatedJitterMS + 1)) : 0);
	std::chrono::steady_clock::time_point sendTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMS);
	if (!m_DelayedMessages.empty() && m_DelayedMessages.back().SendTime > sendTime) {
		sendTime = m_DelayedMessages.back().SendTime;
	}
	m_DelayedMessages.push_back({sendTime, guid, message});
}

void LockstepMan::SendNow(const std::vector<uint8_t>& message, uint64_t guid) {
	if (m_Peer) {
		m_Peer->Send(reinterpret_cast<const char*>(message.data()), static_cast<int>(message.size()), HIGH_PRIORITY, RELIABLE_ORDERED, 0, RakNet::AddressOrGUID(RakNet::RakNetGUID(guid)), false);
	}
}

void LockstepMan::FlushDelayedMessages() {
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	size_t sent = 0;
	while (sent < m_DelayedMessages.size() && m_DelayedMessages[sent].SendTime <= now) {
		SendNow(m_DelayedMessages[sent].Data, m_DelayedMessages[sent].Guid);
		++sent;
	}
	m_DelayedMessages.erase(m_DelayedMessages.begin(), m_DelayedMessages.begin() + sent);
}

void LockstepMan::Broadcast(const std::vector<uint8_t>& message) {
	for (const Peer& peer: m_Peers) {
		if (peer.Accepted && peer.Connected) {
			Send(message, peer.Guid);
		}
	}
}

LockstepMan::Peer* LockstepMan::FindPeer(uint64_t guid) {
	for (Peer& peer: m_Peers) {
		if (peer.Guid == guid) {
			return &peer;
		}
	}
	return nullptr;
}

void LockstepMan::BroadcastLobbyStatus() {
	int accepted = 1;
	for (const Peer& peer: m_Peers) {
		accepted += (peer.Accepted && peer.Connected) ? 1 : 0;
	}
	MessageWriter message(MsgLobby);
	message.Write(static_cast<uint8_t>(accepted));
	for (const std::string& name: GetLobbyPlayerNames()) {
		message.WriteString(name);
	}
	Broadcast(message.Data());
	m_StatusMessage = "Hosting co-op: " + std::to_string(accepted) + " player" + (accepted == 1 ? "" : "s") + " connected";
}

void LockstepMan::SendHello() {
	MessageWriter hello(MsgHello);
	hello.Write(c_ProtocolVersion);
	hello.WriteString(GetCompatibilityString());
	hello.Write(static_cast<uint8_t>(g_UInputMan.GetControlScheme(Players::PlayerOne)->GetDevice()));
	hello.Write(static_cast<uint16_t>(g_WindowMan.GetResX()));
	hello.Write(static_cast<uint16_t>(g_WindowMan.GetResY()));
	hello.Write(g_UInputMan.GetControlScheme(Players::PlayerOne)->GetDigitalAimSpeed());
	hello.WriteString(g_SettingsMan.GetCoopPlayerName());
	Send(hello.Data(), m_HostGuid);
	m_HelloSent = true;
}

void LockstepMan::HandlePacket(RakNet::Packet* packet) {
	const uint8_t packetId = packet->data[0];
	const uint64_t guid = packet->guid.g;

	if (m_Role == Role::Host) {
		switch (packetId) {
			case ID_NEW_INCOMING_CONNECTION: {
				// A client reconnecting keeps its GUID. Forget it on the old, disconnected entry (which stays, as players' owner indices point into the
				// list) so messages go to the new one.
				for (Peer& oldPeer: m_Peers) {
					if (oldPeer.Guid == guid) {
						oldPeer.Guid = 0;
						oldPeer.Connected = false;
					}
				}
				Peer peer;
				peer.Guid = guid;
				peer.Address = packet->systemAddress.ToString(true);
				m_Peers.push_back(peer);
				g_ConsoleMan.PrintString("CO-OP: Incoming connection from " + peer.Address);
				return;
			}
			case ID_DISCONNECTION_NOTIFICATION:
			case ID_CONNECTION_LOST:
				if (Peer* peer = FindPeer(guid)) {
					peer->Connected = false;
					g_ConsoleMan.PrintString("CO-OP: " + peer->Address + " disconnected." + (peer->Player != Players::NoPlayer ? " Player " + std::to_string(peer->Player + 1) + " will stand idle." : ""));
					BroadcastLobbyStatus();
				}
				return;
			default:
				break;
		}
		if (packetId >= ID_USER_PACKET_ENUM) {
			if (Peer* peer = FindPeer(guid); peer && peer->Connected) {
				HandleHostMessage(*peer, static_cast<MessageType>(packetId - ID_USER_PACKET_ENUM), packet->data + 1, packet->length - 1);
			}
		}
		return;
	}

	switch (packetId) {
		case ID_CONNECTION_REQUEST_ACCEPTED: {
			m_ConnectedToHost = true;
			m_HostGuid = guid;
			m_ResolutionRequests = 0;
			m_StatusMessage = "Connected to host, waiting to be let in...";
			g_ConsoleMan.PrintString("CO-OP: Connected to host.");
			SendHello();
			return;
		}
		case ID_CONNECTION_ATTEMPT_FAILED:
		case ID_NO_FREE_INCOMING_CONNECTIONS:
			m_StatusMessage = "Could not connect to " + m_HostAddress + ", retrying...";
			return;
		case ID_DISCONNECTION_NOTIFICATION:
		case ID_CONNECTION_LOST:
			m_ConnectedToHost = false;
			m_HelloSent = false;
			m_LobbyNames.clear();
			m_StatusMessage = m_RejectReason.empty() ? "Lost connection to the host, retrying..." : m_RejectReason;
			g_ConsoleMan.PrintString("CO-OP: " + m_StatusMessage);
			if (m_MatchRunning) {
				m_MatchEndedByHost = true;
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				EndMatch();
			}
			return;
		default:
			break;
	}
	if (packetId >= ID_USER_PACKET_ENUM && guid == m_HostGuid) {
		HandleClientMessage(static_cast<MessageType>(packetId - ID_USER_PACKET_ENUM), packet->data + 1, packet->length - 1);
	}
}

void LockstepMan::HandleHostMessage(Peer& peer, MessageType type, const uint8_t* data, size_t size) {
	MessageReader reader(data, size);
	switch (type) {
		case MsgHello: {
			uint32_t protocol = 0;
			std::string compatibility;
			uint8_t device = 0;
			uint16_t resX = 0;
			uint16_t resY = 0;
			float digitalAimSpeed = 1.0F;
			std::string name;
			reader.Read(protocol);
			reader.ReadString(compatibility);
			reader.Read(device);
			reader.Read(resX);
			reader.Read(resY);
			reader.Read(digitalAimSpeed);
			reader.ReadString(name);
			if (peer.Accepted) {
				return;
			}

			std::string rejectReason;
			if (!reader.Ok() || protocol != c_ProtocolVersion) {
				rejectReason = "Incompatible co-op protocol version.";
			} else if (compatibility != GetCompatibilityString()) {
				// Name the first part that differs, so players know what to fix.
				std::string mismatch = "?";
				std::istringstream ours(GetCompatibilityString());
				std::istringstream theirs(compatibility);
				std::string ourField;
				std::string theirField;
				while (true) {
					const bool haveOurs = static_cast<bool>(std::getline(ours, ourField, '|'));
					const bool haveTheirs = static_cast<bool>(std::getline(theirs, theirField, '|'));
					if (!haveOurs && !haveTheirs) {
						break;
					}
					if (ourField != theirField || haveOurs != haveTheirs) {
						mismatch = haveOurs ? ourField.substr(0, ourField.find('=')) : theirField.substr(0, theirField.find('='));
						break;
					}
				}
				if (mismatch == "modules") {
					rejectReason = "Installed or enabled mods don't match the host's (" + FirstModuleMismatch(GetCompatibilityString(), compatibility) + ").";
				} else {
					rejectReason = "Game version or settings don't match the host's (" + mismatch + ").";
				}
				g_ConsoleMan.PrintString("CO-OP: Host: " + GetCompatibilityString());
				g_ConsoleMan.PrintString("CO-OP: Client: " + compatibility);
			} else if (resX != g_WindowMan.GetResX() || resY != g_WindowMan.GetResY()) {
				// The screen size affects the simulation (e.g. how far actors see), so the client switches to ours and says hello again.
				MessageWriter resolution(MsgResolution);
				resolution.Write(static_cast<uint16_t>(g_WindowMan.GetResX()));
				resolution.Write(static_cast<uint16_t>(g_WindowMan.GetResY()));
				Send(resolution.Data(), peer.Guid);
				g_ConsoleMan.PrintString("CO-OP: Asked " + peer.Address + " to switch from " + std::to_string(resX) + "x" + std::to_string(resY) + " to our resolution.");
				return;
			}
			if (!rejectReason.empty()) {
				MessageWriter reject(MsgReject);
				reject.WriteString(rejectReason);
				Send(reject.Data(), peer.Guid);
				g_ConsoleMan.PrintString("CO-OP: Rejected " + peer.Address + ": " + rejectReason);
				m_Peer->CloseConnection(RakNet::AddressOrGUID(RakNet::RakNetGUID(peer.Guid)), true);
				peer.Connected = false;
				return;
			}
			peer.Accepted = true;
			peer.Device = static_cast<InputDevice>(std::clamp<int>(device, InputDevice::DEVICE_KEYB_ONLY, InputDevice::DEVICE_GAMEPAD_4));
			peer.DigitalAimSpeed = std::isfinite(digitalAimSpeed) ? std::clamp(digitalAimSpeed, 0.01F, 100.0F) : 1.0F;
			peer.Name.clear();
			for (char character: name.substr(0, 24)) {
				if (character >= 32 && character < 127) {
					peer.Name += character;
				}
			}
			if (peer.Name.empty()) {
				peer.Name = "Player";
			}
			g_ConsoleMan.PrintString("CO-OP: " + peer.Name + " (" + peer.Address + ") joined.");
			BroadcastLobbyStatus();
			return;
		}
		case MsgInput: {
			uint32_t matchId = 0;
			long long simUpdate = 0;
			VirtualInputFrame frame;
			reader.Read(matchId);
			reader.Read(simUpdate);
			reader.Read(frame);
			if (!reader.Ok() || !m_MatchRunning || matchId != m_MatchId || peer.Player == Players::NoPlayer || simUpdate > m_NextBundleUpdate + c_MaxInputLead) {
				return;
			}
			SanitizeFrame(frame);
			m_PlayerHasSentInput[peer.Player] = true;
			if (simUpdate >= m_NextBundleUpdate) {
				m_PendingPlayerInputs[peer.Player][simUpdate] = frame;
				if (m_PlayerLagging[peer.Player]) {
					m_PlayerLagging[peer.Player] = false;
					g_ConsoleMan.PrintString("CO-OP: Player " + std::to_string(peer.Player + 1) + " caught up.");
				}
			} else if (m_PlayerLagging[peer.Player]) {
				// Too late for its own sim update, which was bundled without it. Use it for the next bundle instead, so a player who fell behind
				// (and can only catch up by simulating faster than real time) still gets to play meanwhile, just with more delay.
				auto [itr, inserted] = m_PendingPlayerInputs[peer.Player].try_emplace(m_NextBundleUpdate, frame);
				if (!inserted) {
					MergeFrameInto(itr->second, frame);
				}
			}
			HostBuildBundles();
			return;
		}
		case MsgPong: {
			int64_t sentNanoseconds = 0;
			if (reader.Read(sentNanoseconds)) {
				const int64_t nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				const float roundTripMS = static_cast<float>(nowNanoseconds - sentNanoseconds) / 1.0e6F;
				peer.RoundTripMS = peer.RoundTripSamples == 0 ? roundTripMS : peer.RoundTripMS * 0.8F + roundTripMS * 0.2F;
				++peer.RoundTripSamples;
			}
			return;
		}
		case MsgChecksum: {
			uint32_t matchId = 0;
			long long simUpdate = 0;
			std::array<uint64_t, 8> hashes{};
			reader.Read(matchId);
			reader.Read(simUpdate);
			reader.Read(hashes);
			if (!reader.Ok() || matchId != m_MatchId || peer.Player == Players::NoPlayer || simUpdate > m_NextBundleUpdate + c_MaxInputLead) {
				return;
			}
			m_ClientChecksums[simUpdate].emplace_back(peer.Player, hashes);
			return;
		}
		case MsgLeave: {
			uint32_t matchId = 0;
			if (reader.Read(matchId) && m_MatchRunning && matchId == m_MatchId && peer.Player != Players::NoPlayer) {
				g_ConsoleMan.PrintString("CO-OP: Player " + std::to_string(peer.Player + 1) + " left the match and will stand idle.");
				m_PlayerOwnerPeer[peer.Player] = -2;
				m_PendingPlayerInputs[peer.Player].clear();
				peer.Player = Players::NoPlayer;
				HostBuildBundles();
			}
			if (matchId == m_MatchId) {
				peer.InMatch = false;
			}
			return;
		}
		default:
			return;
	}
}

void LockstepMan::HandleClientMessage(MessageType type, const uint8_t* data, size_t size) {
	MessageReader reader(data, size);
	switch (type) {
		case MsgReject:
			reader.ReadString(m_RejectReason);
			m_RejectReason = "The host rejected the connection: " + m_RejectReason;
			m_StatusMessage = m_RejectReason;
			g_ConsoleMan.PrintString("CO-OP: " + m_RejectReason);
			return;
		case MsgLobby: {
			uint8_t count = 0;
			if (reader.Read(count)) {
				m_LobbyPeerCount = count;
				std::vector<std::string> names;
				std::string name;
				for (int i = 0; i < count && reader.ReadString(name); ++i) {
					names.push_back(name);
				}
				m_LobbyNames = names.empty() ? std::vector<std::string>{"?"} : names;
				if (!m_MatchRunning) {
					m_StatusMessage = "In the host's lobby (" + std::to_string(count) + " player" + (count == 1 ? "" : "s") + "), waiting for the host to start an activity...";
				}
			}
			return;
		}
		case MsgResolution: {
			uint16_t resX = 0;
			uint16_t resY = 0;
			reader.Read(resX);
			reader.Read(resY);
			if (!reader.Ok() || m_MatchRunning || g_ActivityMan.IsInActivity()) {
				return;
			}
			const std::string resolutionText = std::to_string(resX) + "x" + std::to_string(resY);
			if (++m_ResolutionRequests > c_MaxResolutionRequests || !SwitchToHostResolution(resX, resY)) {
				m_RejectReason = "Could not switch to the host's resolution (" + resolutionText + "). Set it in the video settings and join again.";
				m_StatusMessage = m_RejectReason;
				g_ConsoleMan.PrintString("CO-OP: " + m_RejectReason);
				m_Peer->CloseConnection(RakNet::AddressOrGUID(RakNet::RakNetGUID(m_HostGuid)), true);
				m_ConnectedToHost = false;
				return;
			}
			SendHello();
			return;
		}
		case MsgStart: {
			uint32_t matchId = 0;
			int8_t yourPlayer = Players::NoPlayer;
			std::string config;
			reader.Read(matchId);
			reader.Read(yourPlayer);
			reader.ReadString(config);
			if (!reader.Ok()) {
				return;
			}
			if (m_MatchRunning) {
				// The host restarted the match.
				m_MatchEndedByHost = true;
				EndMatch();
			}
			m_MatchId = matchId;
			m_MatchConfig = config;
			m_UpdateInputs.clear();
			m_MatchStartPending = false;
			m_DeferredMatchStart = false;
			m_LocalPlayer = (yourPlayer >= Players::PlayerOne && yourPlayer < Players::MaxPlayerCount) ? yourPlayer : Players::NoPlayer;

			// The screen size affects the simulation, and the host's may have changed since we joined (or ours may have), so it's checked for every match.
			const std::map<std::string, std::string> values = ParseKeyValues(config);
			const int resX = ToInt(values, "resX");
			const int resY = ToInt(values, "resY");
			if (resX > 0 && resY > 0 && (resX != g_WindowMan.GetResX() || resY != g_WindowMan.GetResY())) {
				if (!SwitchToHostResolution(resX, resY)) {
					m_StatusMessage = "Could not switch to the host's resolution (" + std::to_string(resX) + "x" + std::to_string(resY) + ") for the match. Set it in the video settings.";
					g_ConsoleMan.PrintString("ERROR: CO-OP: " + m_StatusMessage);
					// Otherwise the host would wait for this player to finish loading forever.
					MessageWriter leave(MsgLeave);
					leave.Write(m_MatchId);
					Send(leave.Data(), m_HostGuid);
					return;
				}
				// The menus finish the resolution change at the start of the next frame; the match starts after that (see Update).
				m_DeferredMatchStart = true;
				return;
			}
			FinishMatchStartFromHost();
			return;
		}
		case MsgTick: {
			uint32_t matchId = 0;
			long long simUpdate = 0;
			uint8_t playerMask = 0;
			reader.Read(matchId);
			reader.Read(simUpdate);
			reader.Read(playerMask);
			std::array<VirtualInputFrame, Players::MaxPlayerCount> frames{};
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
				if (playerMask & (1 << player)) {
					reader.Read(frames[player]);
				}
			}
			// Only while in (or about to start) the match: a client that reconnected during one isn't in it, and would otherwise collect its ticks until it ends.
			if (reader.Ok() && matchId == m_MatchId && (m_MatchRunning || m_MatchStartPending || m_DeferredMatchStart)) {
				m_UpdateInputs[simUpdate] = frames;
			}
			return;
		}
		case MsgDesync: {
			uint32_t matchId = 0;
			long long simUpdate = 0;
			std::string description;
			reader.Read(matchId);
			reader.Read(simUpdate);
			reader.ReadString(description);
			if (reader.Ok() && m_MatchRunning && matchId == m_MatchId) {
				ReportDesync(simUpdate, description);
			}
			return;
		}
		case MsgPing: {
			int64_t sentNanoseconds = 0;
			if (reader.Read(sentNanoseconds)) {
				MessageWriter pong(MsgPong);
				pong.Write(sentNanoseconds);
				Send(pong.Data(), m_HostGuid);
			}
			return;
		}
		case MsgEndMatch: {
			uint32_t matchId = 0;
			if (!reader.Read(matchId) || matchId != m_MatchId) {
				return;
			}
			if (m_MatchRunning) {
				g_ConsoleMan.PrintString("CO-OP: The host ended the match.");
				m_MatchEndedByHost = true;
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				EndMatch();
			}
			m_StatusMessage = "The host ended the match, waiting for the host to start an activity...";
			return;
		}
		default:
			return;
	}
}

bool LockstepMan::SwitchToHostResolution(int resX, int resY) {
	if (resX < c_MinResolution || resY < c_MinResolution || resX > c_MaxResolution || resY > c_MaxResolution || g_ActivityMan.IsInActivity()) {
		return false;
	}
	if (m_LocalResX == 0) {
		m_LocalResX = g_WindowMan.GetResX();
		m_LocalResY = g_WindowMan.GetResY();
		m_LocalResMultiplier = g_WindowMan.GetResMultiplier();
	}
	float multiplier = g_WindowMan.GetResMultiplier();
	if (!g_WindowMan.IsFullscreen()) {
		// Keep the window on the screen.
		const float fitMultiplier = std::min(static_cast<float>(g_WindowMan.GetMaxResX()) / static_cast<float>(resX), static_cast<float>(g_WindowMan.GetMaxResY()) / static_cast<float>(resY));
		multiplier = std::max(0.25F, std::min(multiplier, fitMultiplier));
	}
	g_ConsoleMan.PrintString("CO-OP: Switching to the host's resolution, " + std::to_string(resX) + "x" + std::to_string(resY) + ".");
	g_WindowMan.ChangeResolution(resX, resY, multiplier, g_WindowMan.IsFullscreen());
	m_ChangedResolution = g_WindowMan.ResolutionChanged();
	return g_WindowMan.GetResX() == resX && g_WindowMan.GetResY() == resY;
}

void LockstepMan::FinishMatchStartFromHost() {
	m_DeferredMatchStart = false;
	int unusedLocalPlayer = Players::NoPlayer;
	GameActivity* activity = BuildActivityFromConfig(m_MatchConfig, unusedLocalPlayer);
	if (!activity) {
		m_StatusMessage = "Could not create the host's activity. Are the same mods installed?";
		g_ConsoleMan.PrintString("ERROR: CO-OP: " + m_StatusMessage);
		// Otherwise the host would wait for this player to finish loading forever.
		MessageWriter leave(MsgLeave);
		leave.Write(m_MatchId);
		Send(leave.Data(), m_HostGuid);
		return;
	}
	for (long long simUpdate = 0; simUpdate < m_InputDelay; ++simUpdate) {
		m_UpdateInputs[simUpdate] = {};
	}
	g_ActivityMan.SetStartActivity(activity);
	g_ActivityMan.SetRestartActivity();
	m_MatchStartPending = true;
	if (m_LocalPlayer == Players::NoPlayer) {
		g_ConsoleMan.PrintString("CO-OP: The host started \"" + activity->GetPresetName() + "\", which has no free player slot for you. You're watching this match.");
	} else {
		g_ConsoleMan.PrintString("CO-OP: The host started \"" + activity->GetPresetName() + "\". You are player " + std::to_string(m_LocalPlayer + 1) + ".");
	}
}

#pragma endregion

#pragma region Match Helpers

long long LockstepMan::GetLuaStringOrderFingerprint() {
	// pairs() over string keys is only the same from one process to the next with LuaJIT's string hash randomization off (as our build of it
	// does). A LuaJIT built otherwise (e.g. a system library picked up by a cross build) gives a different order in every process, so this
	// also differs and the host refuses the connection instead of desyncing.
	static const long long fingerprint = []() {
		LuaStateWrapper& luaState = g_LuaMan.GetMasterScriptState();
		std::lock_guard<std::recursive_mutex> lock(luaState.GetMutex());
		lua_State* state = luaState.GetLuaState();
		const int stackTop = lua_gettop(state);
		long long result = -1;
		if (luaL_dostring(state, "local t = {} for i = 1, 64 do t['k' .. i] = i end local h = 0 for _, v in pairs(t) do h = (h * 31 + v) % 2147483647 end return h") == 0) {
			result = static_cast<long long>(lua_tonumber(state, -1));
		}
		lua_settop(state, stackTop);
		return result;
	}();
	return fingerprint;
}

std::string LockstepMan::GetCompatibilityString() const {
	// Everything that has to be identical for two machines to simulate identically, apart from the per-match settings the host sends.
	std::ostringstream stream;
	stream << c_VersionString << "|luaStates=" << g_LuaMan.GetThreadedScriptStates().size() << "|audio=" << g_AudioMan.IsAudioEnabled();
	stream << "|math=" << std::hex << MathConsistency::GetFingerprint() << std::dec;
	// These change which scripts load or how: a script path with the wrong case loads on one machine and not the other, and the JIT changes what
	// scripts can see through the jit library.
	stream << "|caseSensitivePaths=" << System::FilePathsCaseSensitive() << "|luaJIT=" << !g_SettingsMan.DisableLuaJIT();
	stream << "|luaStringOrder=" << GetLuaStringOrderFingerprint();
	// By contents rather than name and version, as a mod can be changed without its version changing (and the official content often is).
	// Userdata (saved games and editor scenes) differs between machines and doesn't matter unless used, so only names are compared for it.
	static std::map<std::string, uint64_t> s_ModuleHashes;
	const auto hashingStartTime = std::chrono::steady_clock::now();
	const size_t modulesHashedBefore = s_ModuleHashes.size();
	stream << "|modules=";
	for (int module = 0; module < g_PresetMan.GetTotalModuleCount(); ++module) {
		if (const DataModule* dataModule = g_PresetMan.GetDataModule(module)) {
			stream << dataModule->GetFileName();
			if (!dataModule->IsUserdata()) {
				auto [hash, inserted] = s_ModuleHashes.try_emplace(dataModule->GetFileName(), 0);
				if (inserted) {
					hash->second = HashModuleContents(g_PresetMan.GetFullModulePath(dataModule->GetFileName()));
				}
				stream << ":" << std::hex << hash->second << std::dec;
			}
			stream << ";";
		}
	}
	if (s_ModuleHashes.size() != modulesHashedBefore) {
		g_ConsoleMan.PrintString("CO-OP: Hashed the contents of " + std::to_string(s_ModuleHashes.size() - modulesHashedBefore) + " modules in " + std::to_string(ElapsedMS(hashingStartTime)) + " ms.");
	}
	return stream.str();
}

std::string LockstepMan::SerializeMatchConfig(const GameActivity* activity) const {
	GameActivity* mutableActivity = const_cast<GameActivity*>(activity);
	std::ostringstream config;
	config << "activityClass=" << activity->GetClassName() << "\n";
	config << "activityPreset=" << activity->GetPresetName() << "\n";
	config << "difficulty=" << activity->GetDifficulty() << "\n";
	config << "gold=" << mutableActivity->GetStartingGold() << "\n";
	config << "fog=" << mutableActivity->GetFogOfWarEnabled() << "\n";
	config << "clearPath=" << activity->GetRequireClearPathToOrbit() << "\n";
	config << "cpuTeam=" << activity->GetCPUTeam() << "\n";
	if (const Scene* scene = g_SceneMan.GetSceneToLoad()) {
		config << "scene=" << scene->GetPresetName() << "\n";
	}
	config << "placeObjects=" << g_SceneMan.GetPlaceObjectsOnLoad() << "\n";
	config << "placeUnits=" << g_SceneMan.GetPlaceUnitsOnLoad() << "\n";
	for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
		config << "tech" << team << "=" << activity->GetTeamTech(team) << "\n";
		config << "aiSkill" << team << "=" << activity->GetTeamAISkill(team) << "\n";
	}
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		config << "playerActive" << player << "=" << activity->PlayerActive(player) << "\n";
		config << "playerHuman" << player << "=" << activity->PlayerHuman(player) << "\n";
		config << "playerTeam" << player << "=" << activity->GetTeamOfPlayer(player) << "\n";
		config << "playerDevice" << player << "=" << static_cast<int>(m_PlayerDevices[player]) << "\n";
		// Exact bits, so every peer gets the same float whatever the locale.
		config << "playerAimSpeedBits" << player << "=" << std::bit_cast<uint32_t>(m_PlayerDigitalAimSpeeds[player]) << "\n";
	}
	config << "inputDelay=" << m_InputDelay << "\n";
	// The screen size affects the simulation. Clients switch to it before starting the match.
	config << "resX=" << g_WindowMan.GetResX() << "\n";
	config << "resY=" << g_WindowMan.GetResY() << "\n";

	// Settings that affect the simulation. Clients use the host's values for the duration of the match.
	config << "set.AIUpdateInterval=" << g_SettingsMan.GetAIUpdateInterval() << "\n";
	config << "set.AutomaticGoldDeposit=" << g_SettingsMan.GetAutomaticGoldDeposit() << "\n";
	config << "set.PathFinderGridNodeSize=" << g_SettingsMan.GetPathFinderGridNodeSize() << "\n";
	config << "set.BlipOnRevealUnseen=" << g_SettingsMan.BlipOnRevealUnseen() << "\n";
	config << "set.SubPieMenuHoverOpenDelay=" << g_SettingsMan.GetSubPieMenuHoverOpenDelay() << "\n";
	config << "set.ShowForeignItems=" << g_SettingsMan.ShowForeignItems() << "\n";
	config << "set.EnableCrabBombs=" << g_SettingsMan.CrabBombsEnabled() << "\n";
	config << "set.CrabBombThreshold=" << g_SettingsMan.GetCrabBombThreshold() << "\n";
	config << "set.SmartBuyMenuNavigation=" << g_SettingsMan.SmartBuyMenuNavigationEnabled() << "\n";
	config << "set.MaxUnheldItems=" << g_MovableMan.GetMaxDroppedItems() << "\n";
	config << "set.ScrapCompactingHeight=" << g_SceneMan.GetScrapCompactingHeight() << "\n";
	config << "set.EnableParticleSettling=" << g_MovableMan.IsParticleSettlingEnabled() << "\n";
	config << "set.EnableMOSubtraction=" << g_MovableMan.IsMOSubtractionEnabled() << "\n";
	config << "set.DeltaTimeTicks=" << g_TimerMan.GetDeltaTimeTicks() << "\n";
	std::string enabledGlobalScripts;
	for (const auto& [scriptName, enabled]: g_SettingsMan.GetEnabledGlobalScriptMap()) {
		if (enabled) {
			enabledGlobalScripts += scriptName + ";";
		}
	}
	config << "set.EnabledGlobalScripts=" << enabledGlobalScripts << "\n";
	// Which groups the editors' object pickers show, so the same clicks pick the same objects (e.g. in Wave Defense's build phase).
	std::string visibleAssemblyGroups;
	for (const std::string& group: g_SettingsMan.GetVisibleAssemblyGroupsList()) {
		visibleAssemblyGroups += group + ";";
	}
	config << "set.VisibleAssemblyGroups=" << visibleAssemblyGroups << "\n";
	return config.str();
}

GameActivity* LockstepMan::BuildActivityFromConfig(const std::string& configText, int& localPlayerOut) {
	std::map<std::string, std::string> config = ParseKeyValues(configText);

	m_InputDelay = std::clamp(ToInt(config, "inputDelay", m_InputDelay), 1, 60);
	std::map<std::string, std::string> settings;
	for (const auto& [key, value]: config) {
		if (key.rfind("set.", 0) == 0) {
			settings[key.substr(4)] = value;
		}
	}
	ApplySessionSettings(settings);

	const Entity* activityPreset = g_PresetMan.GetEntityPreset(ToString(config, "activityClass"), ToString(config, "activityPreset"));
	GameActivity* activity = activityPreset ? dynamic_cast<GameActivity*>(activityPreset->Clone()) : nullptr;
	if (!activity) {
		RestoreLocalSettings();
		return nullptr;
	}

	// Mirrors ScenarioActivityConfigGUI::StartGame.
	activity->SetDifficulty(ToInt(config, "difficulty", Activity::DifficultySetting::MediumDifficulty));
	activity->SetStartingGold(ToInt(config, "gold", 2000));
	activity->SetRequireClearPathToOrbit(ToInt(config, "clearPath") != 0);
	activity->SetFogOfWarEnabled(ToInt(config, "fog") != 0);
	if (std::string sceneName = ToString(config, "scene"); !sceneName.empty()) {
		if (const Scene* scene = dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", sceneName))) {
			g_SceneMan.SetSceneToLoad(scene, ToInt(config, "placeObjects", 1) != 0, ToInt(config, "placeUnits", 1) != 0);
		} else {
			delete activity;
			RestoreLocalSettings();
			return nullptr;
		}
	}

	localPlayerOut = Players::NoPlayer;
	activity->ClearPlayers(false);
	m_MatchPlayers.fill(false);
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_PlayerDevices[player] = static_cast<InputDevice>(ToInt(config, "playerDevice" + std::to_string(player), InputDevice::DEVICE_KEYB_ONLY));
		m_PlayerDigitalAimSpeeds[player] = 1.0F;
		if (std::string bits = ToString(config, "playerAimSpeedBits" + std::to_string(player)); !bits.empty()) {
			m_PlayerDigitalAimSpeeds[player] = std::bit_cast<float>(static_cast<uint32_t>(std::strtoul(bits.c_str(), nullptr, 10)));
		}
		if (ToInt(config, "playerActive" + std::to_string(player)) != 0) {
			bool human = ToInt(config, "playerHuman" + std::to_string(player)) != 0;
			activity->AddPlayer(player, human, ToInt(config, "playerTeam" + std::to_string(player)), 0);
			m_MatchPlayers[player] = human;
		}
	}
	activity->SetCPUTeam(ToInt(config, "cpuTeam", Activity::Teams::NoTeam));
	for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
		if (std::string tech = ToString(config, "tech" + std::to_string(team)); !tech.empty()) {
			activity->SetTeamTech(team, tech);
		}
		activity->SetTeamAISkill(team, ToInt(config, "aiSkill" + std::to_string(team), Activity::AISkillSetting::DefaultSkill));
	}
	return activity;
}

GameActivity* LockstepMan::BuildAutoActivity() const {
	const Entity* activityPreset = g_PresetMan.GetEntityPreset("GAScripted", m_Options.AutoActivity);
	GameActivity* activity = activityPreset ? dynamic_cast<GameActivity*>(activityPreset->Clone()) : nullptr;
	if (!activity) {
		g_ConsoleMan.PrintString("ERROR: CO-OP: Unknown activity \"" + m_Options.AutoActivity + "\"!");
		return nullptr;
	}
	activity->SetDifficulty(m_Options.AutoDifficulty);
	activity->SetStartingGold(m_Options.AutoGold);
	activity->SetRequireClearPathToOrbit(false);
	activity->SetFogOfWarEnabled(m_Options.AutoFog);
	// Without -coop-scene, the Activity's own default scene, or failing that the first compatible scene the scenario menu would offer, by name.
	const std::string sceneName = m_Options.AutoScene.empty() ? activity->GetSceneName() : m_Options.AutoScene;
	const Scene* scene = sceneName.empty() ? nullptr : dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", sceneName));
	if (!scene && m_Options.AutoScene.empty()) {
		std::list<Entity*> scenePresets;
		g_PresetMan.GetAllOfType(scenePresets, "Scene");
		for (Entity* preset: scenePresets) {
			Scene* candidate = dynamic_cast<Scene*>(preset);
			if (candidate && !candidate->GetLocation().IsZero() && !candidate->IsMetagameInternal() && !candidate->IsSavedGameInternal() && candidate->GetMetasceneParent().empty() && activity->SceneIsCompatible(candidate) &&
			    (!scene || candidate->GetPresetName() < scene->GetPresetName())) {
				scene = candidate;
			}
		}
	}
	if (scene) {
		g_SceneMan.SetSceneToLoad(scene, true, m_Options.AutoDeployUnits);
		g_ConsoleMan.PrintString("CO-OP: Automatic start on scene \"" + scene->GetPresetName() + "\".");
	}
	// The host is player one on team one against a CPU team. Clients are added to the host's team when the match is prepared.
	activity->ClearPlayers(false);
	activity->AddPlayer(Players::PlayerOne, true, Activity::Teams::TeamOne, 0);
	activity->SetCPUTeam(Activity::Teams::TeamTwo);
	return activity;
}

void LockstepMan::ApplySessionSettings(const std::map<std::string, std::string>& values) {
	if (m_SavedSettings.empty()) {
		m_SavedSettings["AIUpdateInterval"] = std::to_string(g_SettingsMan.GetAIUpdateInterval());
		m_SavedSettings["AutomaticGoldDeposit"] = std::to_string(g_SettingsMan.GetAutomaticGoldDeposit());
		m_SavedSettings["PathFinderGridNodeSize"] = std::to_string(g_SettingsMan.GetPathFinderGridNodeSize());
		m_SavedSettings["BlipOnRevealUnseen"] = std::to_string(g_SettingsMan.BlipOnRevealUnseen());
		m_SavedSettings["SubPieMenuHoverOpenDelay"] = std::to_string(g_SettingsMan.GetSubPieMenuHoverOpenDelay());
		m_SavedSettings["ShowForeignItems"] = std::to_string(g_SettingsMan.ShowForeignItems());
		m_SavedSettings["EnableCrabBombs"] = std::to_string(g_SettingsMan.CrabBombsEnabled());
		m_SavedSettings["CrabBombThreshold"] = std::to_string(g_SettingsMan.GetCrabBombThreshold());
		m_SavedSettings["SmartBuyMenuNavigation"] = std::to_string(g_SettingsMan.SmartBuyMenuNavigationEnabled());
		m_SavedSettings["MaxUnheldItems"] = std::to_string(g_MovableMan.GetMaxDroppedItems());
		m_SavedSettings["ScrapCompactingHeight"] = std::to_string(g_SceneMan.GetScrapCompactingHeight());
		m_SavedSettings["EnableParticleSettling"] = std::to_string(g_MovableMan.IsParticleSettlingEnabled());
		m_SavedSettings["EnableMOSubtraction"] = std::to_string(g_MovableMan.IsMOSubtractionEnabled());
		m_SavedSettings["DeltaTimeTicks"] = std::to_string(g_TimerMan.GetDeltaTimeTicks());
		std::string enabledGlobalScripts;
		for (const auto& [scriptName, enabled]: g_SettingsMan.GetEnabledGlobalScriptMap()) {
			if (enabled) {
				enabledGlobalScripts += scriptName + ";";
			}
		}
		m_SavedSettings["EnabledGlobalScripts"] = enabledGlobalScripts;
		std::string visibleAssemblyGroups;
		for (const std::string& group: g_SettingsMan.GetVisibleAssemblyGroupsList()) {
			visibleAssemblyGroups += group + ";";
		}
		m_SavedSettings["VisibleAssemblyGroups"] = visibleAssemblyGroups;
	}

	auto intValue = [&values](const std::string& key, int& out) {
		if (auto itr = values.find(key); itr != values.end()) {
			out = std::atoi(itr->second.c_str());
			return true;
		}
		return false;
	};
	int value = 0;
	if (intValue("AIUpdateInterval", value)) { g_SettingsMan.SetAIUpdateInterval(value); }
	if (intValue("AutomaticGoldDeposit", value)) { g_SettingsMan.m_AutomaticGoldDeposit = value != 0; }
	if (intValue("PathFinderGridNodeSize", value)) { g_SettingsMan.m_PathFinderGridNodeSize = value; }
	if (intValue("BlipOnRevealUnseen", value)) { g_SettingsMan.SetBlipOnRevealUnseen(value != 0); }
	if (intValue("SubPieMenuHoverOpenDelay", value)) { g_SettingsMan.SetSubPieMenuHoverOpenDelay(value); }
	if (intValue("ShowForeignItems", value)) { g_SettingsMan.SetShowForeignItems(value != 0); }
	if (intValue("EnableCrabBombs", value)) { g_SettingsMan.SetCrabBombsEnabled(value != 0); }
	if (intValue("CrabBombThreshold", value)) { g_SettingsMan.SetCrabBombThreshold(value); }
	if (intValue("SmartBuyMenuNavigation", value)) { g_SettingsMan.SetSmartBuyMenuNavigation(value != 0); }
	if (intValue("MaxUnheldItems", value)) { g_MovableMan.SetMaxDroppedItems(value); }
	if (intValue("ScrapCompactingHeight", value)) { g_SceneMan.SetScrapCompactingHeight(value); }
	if (intValue("EnableParticleSettling", value)) { g_MovableMan.EnableParticleSettling(value != 0); }
	if (intValue("EnableMOSubtraction", value)) { g_MovableMan.m_MOSubtractionEnabled = value != 0; }
	if (intValue("DeltaTimeTicks", value) && value > 0) { g_TimerMan.SetDeltaTimeTicks(value); }
	if (auto itr = values.find("EnabledGlobalScripts"); itr != values.end()) {
		std::unordered_map<std::string, bool>& globalScripts = g_SettingsMan.GetEnabledGlobalScriptMap();
		for (auto& [scriptName, enabled]: globalScripts) {
			enabled = false;
		}
		std::istringstream stream(itr->second);
		std::string scriptName;
		while (std::getline(stream, scriptName, ';')) {
			if (!scriptName.empty()) {
				globalScripts[scriptName] = true;
			}
		}
	}
	if (auto itr = values.find("VisibleAssemblyGroups"); itr != values.end()) {
		g_SettingsMan.m_VisibleAssemblyGroupsList.clear();
		std::istringstream stream(itr->second);
		std::string group;
		while (std::getline(stream, group, ';')) {
			if (!group.empty()) {
				g_SettingsMan.m_VisibleAssemblyGroupsList.push_back(group);
			}
		}
	}
}

void LockstepMan::RestoreLocalSettings() {
	if (!m_SavedSettings.empty()) {
		std::map<std::string, std::string> saved;
		saved.swap(m_SavedSettings);
		ApplySessionSettings(saved);
		m_SavedSettings.clear();
	}
}

VirtualInputFrame LockstepMan::CaptureLocalInput() {
	VirtualInputFrame frame = g_UInputMan.CaptureLocalInputFrame();
	if (m_Options.BotSeed < 0) {
		return frame;
	}

	// Automated testing: a bot that keeps picking a new random combination of held inputs, so actors walk, jump, crouch, aim, fire and switch.
	const uint64_t previousHeld = m_BotHeld.ElementHeld;
	const uint8_t previousMouseHeld = m_BotHeld.MouseHeld;
	if (m_BotHoldUpdates <= 0) {
		m_BotHeld = VirtualInputFrame();
		auto chance = [this](int percent) { return static_cast<int>(m_BotRNG() % 100) < percent; };
		int move = static_cast<int>(m_BotRNG() % 3);
		if (move == 1) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_L_LEFT;
		} else if (move == 2) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_L_RIGHT;
		}
		if (chance(30)) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_JUMP;
		}
		if (chance(15)) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_CROUCH;
		}
		if (chance(40)) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_FIRE;
			m_BotHeld.MouseHeld |= 1 << MouseButtons::MOUSE_LEFT;
		}
		if (chance(25)) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_AIM;
		}
		if (chance(5)) {
			m_BotHeld.ElementHeld |= 1ULL << InputElements::INPUT_NEXT;
		}
		if (chance(3)) {
			// Raw keys reach scripts too (e.g. Space in Wave Defense).
			m_BotHeld.KeysHeld[SDL_SCANCODE_SPACE / 64] |= 1ULL << (SDL_SCANCODE_SPACE % 64);
		}
		m_BotHeld.MouseMovement[0] = static_cast<float>(static_cast<int>(m_BotRNG() % 21) - 10);
		m_BotHeld.MouseMovement[1] = static_cast<float>(static_cast<int>(m_BotRNG() % 21) - 10);
		m_BotHoldUpdates = 10 + static_cast<int>(m_BotRNG() % 80);
	}
	--m_BotHoldUpdates;

	frame = m_BotHeld;
	frame.ElementPressed = frame.ElementHeld & ~previousHeld;
	frame.ElementReleased = previousHeld & ~frame.ElementHeld;
	frame.MousePressed = frame.MouseHeld & ~previousMouseHeld;
	frame.MouseReleased = previousMouseHeld & ~frame.MouseHeld;
	frame.MousePosition[0] = static_cast<float>(g_WindowMan.GetResX() / 2);
	frame.MousePosition[1] = static_cast<float>(g_WindowMan.GetResY() / 2);
	// Only move the mouse on the first update of each hold, otherwise the aim just pins to the edge.
	if (m_BotHoldUpdates < 5) {
		frame.MouseMovement[0] = frame.MouseMovement[1] = 0;
	}
	return frame;
}

void LockstepMan::ReportDesync(long long simUpdate, const std::string& description) {
	if (m_Desynced) {
		return;
	}
	m_Desynced = true;
	m_DesyncMessage = "DESYNC at sim update " + std::to_string(simUpdate) + ": " + description;
	g_ConsoleMan.PrintString("ERROR: CO-OP: " + m_DesyncMessage);
	std::string dumpPath = System::GetWorkingDirectory() + System::GetUserdataDirectory() + "CoopDesync_" + (m_Role == Role::Host ? std::string("Host") : std::string("Client")) + "_" + std::to_string(simUpdate) + ".txt";
	DeterminismHarness::WriteStateDump(dumpPath);
	DeterminismHarness::WriteDumpRing(dumpPath);
	g_ConsoleMan.PrintString("CO-OP: Wrote state dump to " + dumpPath);
}

#pragma endregion

#pragma region Match Lifecycle

GameActivity* LockstepMan::PrepareMatch(GameActivity* activity) {
	m_MatchStartPending = false;
	if (m_Role == Role::None || !activity) {
		return activity;
	}

	int localPlayer = Players::NoPlayer;
	GameActivity* matchActivity = nullptr;

	if (m_Role == Role::Host) {
		// The host plays the first human player of the Activity it configured. Each connected client gets a free player slot on the host's team.
		m_PlayerOwnerPeer.fill(-2);
		m_PlayerDevices.fill(InputDevice::DEVICE_KEYB_ONLY);
		m_PlayerDigitalAimSpeeds.fill(1.0F);
		int hostTeam = Activity::Teams::TeamOne;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (activity->PlayerActive(player) && activity->PlayerHuman(player)) {
				localPlayer = player;
				hostTeam = activity->GetTeamOfPlayer(player);
				m_PlayerOwnerPeer[player] = -1;
				m_PlayerDevices[player] = g_UInputMan.GetControlScheme(Players::PlayerOne)->GetDevice();
				m_PlayerDigitalAimSpeeds[player] = g_UInputMan.GetControlScheme(Players::PlayerOne)->GetDigitalAimSpeed();
				break;
			}
		}
		for (size_t peerIndex = 0; peerIndex < m_Peers.size(); ++peerIndex) {
			Peer& peer = m_Peers[peerIndex];
			peer.Player = Players::NoPlayer;
			peer.InMatch = peer.Accepted && peer.Connected;
			if (!peer.Accepted || !peer.Connected) {
				continue;
			}
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
				if (!activity->PlayerActive(player)) {
					activity->AddPlayer(player, true, hostTeam, 0);
					peer.Player = player;
					m_PlayerOwnerPeer[player] = static_cast<int>(peerIndex);
					m_PlayerDevices[player] = peer.Device;
					m_PlayerDigitalAimSpeeds[player] = peer.DigitalAimSpeed;
					break;
				}
			}
		}

		if (!m_Options.InputDelayFixed) {
			// Input for a sim update has to make the round trip to the host and back before that update runs, so the input delay needs to cover the
			// slowest player's round trip time, plus some margin for jitter.
			float slowestRoundTripMS = 0;
			for (const Peer& peer: m_Peers) {
				if (peer.Accepted && peer.Connected && peer.Player != Players::NoPlayer) {
					slowestRoundTripMS = std::max(slowestRoundTripMS, peer.RoundTripMS);
				}
			}
			const float simUpdateMS = g_TimerMan.GetDeltaTimeMS();
			m_InputDelay = std::clamp(static_cast<int>(std::ceil((slowestRoundTripMS * 1.25F + simUpdateMS) / simUpdateMS)), c_MinAutoInputDelay, c_MaxAutoInputDelay);
			g_ConsoleMan.PrintString("CO-OP: Slowest round trip " + std::to_string(static_cast<int>(slowestRoundTripMS)) + " ms, using an input delay of " + std::to_string(m_InputDelay) + " sim updates.");
		}

		m_MatchId = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		m_MatchConfig = SerializeMatchConfig(activity);
		for (const Peer& peer: m_Peers) {
			if (peer.Accepted && peer.Connected) {
				MessageWriter start(MsgStart);
				start.Write(m_MatchId);
				start.Write(static_cast<int8_t>(peer.Player));
				start.WriteString(m_MatchConfig);
				Send(start.Data(), peer.Guid);
			}
		}

		for (auto& pending: m_PendingPlayerInputs) {
			pending.clear();
		}
		m_LastPlayerInputs.fill(VirtualInputFrame());
		m_PlayerHasSentInput.fill(false);
		m_PlayerLagging.fill(false);
		m_NextBundleUpdate = m_InputDelay;
		m_HostChecksums.clear();
		m_ClientChecksums.clear();
		m_ChecksumsCompared = 0;
		m_UpdateInputs.clear();
		for (long long simUpdate = 0; simUpdate < m_InputDelay; ++simUpdate) {
			m_UpdateInputs[simUpdate] = {};
		}
	} else {
		localPlayer = m_LocalPlayer;
	}

	// Every peer, the host included, constructs the Activity the same way from the same configuration.
	delete activity;
	int unusedLocalPlayer = Players::NoPlayer;
	matchActivity = BuildActivityFromConfig(m_MatchConfig, unusedLocalPlayer);
	m_LocalPlayer = localPlayer;
	if (!matchActivity && m_Role == Role::Client && m_ConnectedToHost) {
		// Otherwise the host would wait for this player to finish loading forever.
		MessageWriter leave(MsgLeave);
		leave.Write(m_MatchId);
		Send(leave.Data(), m_HostGuid);
	}
	return matchActivity;
}

void LockstepMan::BeginMatch() {
	if (m_Role == Role::None) {
		return;
	}
	g_TimerMan.SetDeterministicMode(true);
	if (m_LocalPlayer >= Players::PlayerOne && m_LocalPlayer < Players::MaxPlayerCount) {
		m_MatchPlayers[m_LocalPlayer] = true;
	}
	g_UInputMan.BeginVirtualInput(m_LocalPlayer, m_PlayerDevices, m_PlayerDigitalAimSpeeds);

	m_MatchRunning = true;
	m_MatchEndedByHost = false;
	m_NextSimUpdate = 0;
	m_Waiting = false;
	m_Desynced = false;
	m_DesyncMessage.clear();
	m_LeaveRequested = false;
	if (m_Options.BotSeed >= 0) {
		m_BotRNG.seed(static_cast<unsigned int>(m_Options.BotSeed));
		m_BotHeld = VirtualInputFrame();
		m_BotHoldUpdates = 0;
	}
	m_StatusMessage.clear();
	g_ConsoleMan.PrintString("CO-OP: Match started. Local player: " + std::to_string(m_LocalPlayer + 1) + ", input delay: " + std::to_string(m_InputDelay) + " sim updates.");
}

void LockstepMan::EndMatch() {
	if (!m_MatchRunning) {
		return;
	}
	m_MatchRunning = false;
	if (m_Role == Role::Host) {
		// However the host's match ended, the clients' must end too. They'd otherwise wait for bundles forever.
		MessageWriter endMatch(MsgEndMatch);
		endMatch.Write(m_MatchId);
		Broadcast(endMatch.Data());
	} else if (m_Role == Role::Client && m_ConnectedToHost && !m_MatchEndedByHost) {
		// However a client's match ended (leaving, the Activity failing to start, the game over screen), tell the host, which would otherwise wait for
		// this player's input (or, while it's still loading, forever).
		MessageWriter leave(MsgLeave);
		leave.Write(m_MatchId);
		SendNow(leave.Data(), m_HostGuid);
	}
	g_TimerMan.SetDeterministicMode(false);
	g_UInputMan.EndVirtualInput();
	RestoreLocalSettings();
	m_UpdateInputs.clear();
	for (auto& pending: m_PendingPlayerInputs) {
		pending.clear();
	}
	m_Waiting = false;
	m_LeaveRequested = false;
	if (m_Role == Role::Host) {
		m_StatusMessage.clear();
		BroadcastLobbyStatus();
	} else if (m_ConnectedToHost) {
		m_StatusMessage = "Waiting for the host to start an activity...";
	}
	g_ConsoleMan.PrintString("CO-OP: Match ended.");
}

void LockstepMan::RequestLeave() {
	if (!m_MatchRunning) {
		return;
	}
	if (!m_LeaveRequested || ElapsedMS(m_LeaveRequestTime) > c_LeaveConfirmMS) {
		m_LeaveRequested = true;
		m_LeaveRequestTime = std::chrono::steady_clock::now();
		return;
	}
	// EndMatch tells the host, if we're a client.
	g_ActivityMan.EndActivity();
	g_ActivityMan.SetInActivity(false);
	EndMatch();
}

#pragma endregion

#pragma region Per-Frame and Per-Update Hooks

void LockstepMan::Update() {
	if (!m_Peer) {
		return;
	}
	FlushDelayedMessages();
	for (RakNet::Packet* packet = m_Peer->Receive(); packet; m_Peer->DeallocatePacket(packet), packet = m_Peer->Receive()) {
		if (packet->length > 0) {
			HandlePacket(packet);
		}
	}

	if (m_DeferredMatchStart && !g_WindowMan.ResolutionChanged()) {
		FinishMatchStartFromHost();
	}

	if (m_Role == Role::Client && !m_ConnectedToHost && m_RejectReason.empty() && ElapsedMS(m_WaitingSince) > c_ReconnectIntervalMS) {
		m_WaitingSince = std::chrono::steady_clock::now();
		std::string address = m_HostAddress.substr(0, m_HostAddress.rfind(':'));
		unsigned short port = static_cast<unsigned short>(std::atoi(m_HostAddress.substr(m_HostAddress.rfind(':') + 1).c_str()));
		m_Peer->Connect(address.c_str(), port, nullptr, 0);
	}

	if (m_Role == Role::Host) {
		if (ElapsedMS(m_LastPingTime) > 500) {
			m_LastPingTime = std::chrono::steady_clock::now();
			MessageWriter ping(MsgPing);
			ping.Write(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
			Broadcast(ping.Data());
		}

		if (!m_AutoStartDone && !m_Options.AutoActivity.empty() && !m_MatchRunning && !g_ActivityMan.ActivitySetToRestart()) {
			int players = 1;
			bool roundTripsMeasured = true;
			for (const Peer& peer: m_Peers) {
				if (peer.Accepted && peer.Connected) {
					++players;
					roundTripsMeasured = roundTripsMeasured && peer.RoundTripSamples >= 3;
				}
			}
			if (players >= m_Options.ExpectedPlayers && roundTripsMeasured) {
				m_AutoStartDone = true;
				if (GameActivity* activity = BuildAutoActivity()) {
					g_ActivityMan.SetStartActivity(activity);
					g_ActivityMan.SetRestartActivity();
					m_MatchStartPending = true;
				}
			}
		}
		HostBuildBundles();

		// Compare state hashes from clients against our own.
		for (auto itr = m_ClientChecksums.begin(); itr != m_ClientChecksums.end();) {
			auto own = m_HostChecksums.find(itr->first);
			if (own == m_HostChecksums.end()) {
				++itr;
				continue;
			}
			for (const auto& [player, hashes]: itr->second) {
				if (hashes == own->second) {
					++m_ChecksumsCompared;
					continue;
				}
				static const char* componentNames[8] = {"combined", "RNG", "Lua RNG", "actors", "items", "particles", "terrain", "activity"};
				std::string description = "player " + std::to_string(player + 1) + " differs in";
				for (int component = 1; component < 8; ++component) {
					if (hashes[component] != own->second[component]) {
						description += std::string(" ") + componentNames[component];
					}
				}
				MessageWriter desync(MsgDesync);
				desync.Write(m_MatchId);
				desync.Write(itr->first);
				desync.WriteString(description);
				Broadcast(desync.Data());
				ReportDesync(itr->first, description);
			}
			itr = m_ClientChecksums.erase(itr);
		}
		while (m_HostChecksums.size() > 64) {
			m_HostChecksums.erase(m_HostChecksums.begin());
		}
	}

	if (m_MatchRunning && m_Waiting && ElapsedMS(m_WaitingSince) > c_StalledEscapeDelayMS) {
		// While the sim is stalled waiting for other players, input isn't updated (that happens per sim update), so look at the keyboard directly to
		// let the player leave.
		const bool* keyStates = SDL_GetKeyboardState(nullptr);
		const bool escapeHeld = keyStates && keyStates[SDL_SCANCODE_ESCAPE];
		if (escapeHeld && !m_StalledEscapeHeld) {
			RequestLeave();
		}
		m_StalledEscapeHeld = escapeHeld;
	} else {
		m_StalledEscapeHeld = false;
	}

	if (m_MatchRunning && !g_ActivityMan.IsInActivity() && !g_ActivityMan.ActivitySetToRestart()) {
		// The Activity ended on its own (e.g. the game was won or lost and the players went back to the menus).
		EndMatch();
	}
}

void LockstepMan::HostBuildBundles() {
	if (m_Role != Role::Host || !m_MatchRunning) {
		return;
	}
	while (true) {
		const long long simUpdate = m_NextBundleUpdate;
		std::array<VirtualInputFrame, Players::MaxPlayerCount> frames{};
		bool missing = false;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!m_MatchPlayers[player]) {
				continue;
			}
			const int owner = m_PlayerOwnerPeer[player];
			const bool ownerGone = owner == -2 || (owner >= 0 && (owner >= static_cast<int>(m_Peers.size()) || !m_Peers[owner].Connected));
			if (ownerGone) {
				continue; // Nobody controls this player, so it gets no input.
			}
			if (m_PendingPlayerInputs[player].find(simUpdate) == m_PendingPlayerInputs[player].end()) {
				if (!m_PlayerHasSentInput[player]) {
					return; // Still loading the match. Wait for them however long it takes (a dropped connection ends the wait).
				}
				if (!m_PlayerLagging[player]) {
					missing = true;
				}
			}
		}

		if (missing) {
			// Only give up on late input when we ourselves are stuck waiting for this bundle.
			const bool timedOut = m_Waiting && m_NextSimUpdate >= simUpdate && ElapsedMS(m_WaitingSince) > c_InputTimeoutMS;
			if (!timedOut) {
				return;
			}
		}

		uint8_t playerMask = 0;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!m_MatchPlayers[player]) {
				continue;
			}
			playerMask |= static_cast<uint8_t>(1 << player);
			auto& pending = m_PendingPlayerInputs[player];
			if (auto itr = pending.find(simUpdate); itr != pending.end()) {
				frames[player] = itr->second;
			} else {
				const int owner = m_PlayerOwnerPeer[player];
				const bool ownerGone = owner == -2 || (owner >= 0 && (owner >= static_cast<int>(m_Peers.size()) || !m_Peers[owner].Connected));
				const bool laggedTooLong = m_PlayerLagging[player] && ElapsedMS(m_PlayerLaggingSince[player]) > c_LagIdleMS;
				frames[player] = (ownerGone || laggedTooLong) ? VirtualInputFrame() : RepeatFrame(m_LastPlayerInputs[player]);
				if (!ownerGone && player != m_LocalPlayer && !m_PlayerLagging[player]) {
					m_PlayerLagging[player] = true;
					m_PlayerLaggingSince[player] = std::chrono::steady_clock::now();
					g_ConsoleMan.PrintString("CO-OP: Input from player " + std::to_string(player + 1) + " is late, repeating their previous input until they catch up.");
				}
			}
			m_LastPlayerInputs[player] = frames[player];
			pending.erase(pending.begin(), pending.upper_bound(simUpdate));
		}

		m_UpdateInputs[simUpdate] = frames;
		MessageWriter tick(MsgTick);
		tick.Write(m_MatchId);
		tick.Write(simUpdate);
		tick.Write(playerMask);
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (playerMask & (1 << player)) {
				tick.Write(frames[player]);
			}
		}
		for (const Peer& peer: m_Peers) {
			if (peer.InMatch && peer.Accepted && peer.Connected) {
				Send(tick.Data(), peer.Guid);
			}
		}
		++m_NextBundleUpdate;
		if (missing) {
			// After a timeout, restart the wait so the next late bundle gets the full timeout too.
			m_WaitingSince = std::chrono::steady_clock::now();
		}
	}
}

bool LockstepMan::CanSimulateNextUpdate() {
	if (!m_MatchRunning) {
		return true;
	}
	if (m_UpdateInputs.find(m_NextSimUpdate) == m_UpdateInputs.end()) {
		if (!m_Waiting) {
			m_Waiting = true;
			m_WaitingSince = std::chrono::steady_clock::now();
		}
		HostBuildBundles();
		FlushDelayedMessages();
		if (m_UpdateInputs.find(m_NextSimUpdate) == m_UpdateInputs.end()) {
			return false;
		}
	}
	m_Waiting = false;
	return true;
}

void LockstepMan::BeginSimUpdate() {
	if (!m_MatchRunning) {
		return;
	}

	if (m_LocalPlayer >= Players::PlayerOne && m_LocalPlayer < Players::MaxPlayerCount) {
		VirtualInputFrame frame = CaptureLocalInput();
		const long long targetUpdate = m_NextSimUpdate + m_InputDelay;
		if (m_Role == Role::Host) {
			m_PendingPlayerInputs[m_LocalPlayer][targetUpdate] = frame;
			m_PlayerHasSentInput[m_LocalPlayer] = true;
		} else {
			MessageWriter input(MsgInput);
			input.Write(m_MatchId);
			input.Write(targetUpdate);
			input.Write(frame);
			Send(input.Data(), m_HostGuid);
		}
	}

	auto bundle = m_UpdateInputs.find(m_NextSimUpdate);
	if (bundle != m_UpdateInputs.end()) {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (m_MatchPlayers[player]) {
				g_UInputMan.ApplyVirtualInputFrame(player, bundle->second[player]);
			}
		}
		m_UpdateInputs.erase(bundle);
	}

	if (m_NextSimUpdate == m_Options.DesyncInjectTick) {
		// Testing aid: deliberately make this machine's simulation diverge.
		g_ConsoleMan.PrintString("CO-OP: Injecting a deliberate desync at sim update " + std::to_string(m_NextSimUpdate));
		RandomNum<int>(0, 100);
	}
}

void LockstepMan::EndSimUpdate() {
	if (!m_MatchRunning) {
		return;
	}
	const long long simUpdate = m_NextSimUpdate;
	if ((simUpdate + 1) % c_ChecksumInterval == 0) {
		DeterminismHarness::SimStateHashes hashes = DeterminismHarness::HashSimState((simUpdate + 1) % c_TerrainChecksumInterval == 0);
		std::array<uint64_t, 8> hashArray = {hashes.Combined, hashes.RNG, hashes.LuaRNG, hashes.Actors, hashes.Items, hashes.Particles, hashes.Terrain, hashes.Activity};
		if (m_Role == Role::Host) {
			m_HostChecksums[simUpdate] = hashArray;
		} else {
			MessageWriter checksum(MsgChecksum);
			checksum.Write(m_MatchId);
			checksum.Write(simUpdate);
			checksum.Write(hashArray);
			Send(checksum.Data(), m_HostGuid);
		}
	}
	++m_NextSimUpdate;
	HostBuildBundles();
}

void LockstepMan::DrawOverlay(BITMAP* targetBitmap) {
	if (m_Role == Role::None || !targetBitmap) {
		return;
	}
	AllegroBitmap bitmap(targetBitmap);
	GUIFont* smallFont = g_FrameMan.GetSmallFont(true);
	GUIFont* largeFont = g_FrameMan.GetLargeFont(true);
	const int centerX = targetBitmap->w / 2;

	std::string topLine;
	if (m_MatchRunning) {
		topLine = std::string("CO-OP ") + (m_Role == Role::Host ? "HOST" : "CLIENT") + " | Player " + std::to_string(m_LocalPlayer + 1) + " | Sim update " + std::to_string(m_NextSimUpdate) + " | Input delay " + std::to_string(m_InputDelay);
		if (m_Role == Role::Host && m_ChecksumsCompared > 0) {
			topLine += " | In sync (" + std::to_string(m_ChecksumsCompared) + " checks)";
		}
	} else {
		topLine = "CO-OP: " + m_StatusMessage;
	}
	smallFont->DrawAligned(&bitmap, centerX, 2, topLine, GUIFont::Centre);

	int lineY = targetBitmap->h / 3;
	if (m_MatchRunning && m_Waiting && ElapsedMS(m_WaitingSince) > c_WaitingOverlayDelayMS) {
		largeFont->DrawAligned(&bitmap, centerX, lineY, "Waiting for other players...", GUIFont::Centre);
		lineY += 16;
		if (ElapsedMS(m_WaitingSince) > c_StalledEscapeDelayMS) {
			smallFont->DrawAligned(&bitmap, centerX, lineY, "Press Esc twice to leave", GUIFont::Centre);
			lineY += 12;
		}
	}
	if (m_Desynced) {
		largeFont->DrawAligned(&bitmap, centerX, lineY, m_DesyncMessage, GUIFont::Centre);
		lineY += 16;
	}
	if (m_LeaveRequested && ElapsedMS(m_LeaveRequestTime) <= c_LeaveConfirmMS) {
		largeFont->DrawAligned(&bitmap, centerX, lineY, m_Role == Role::Host ? "Press Esc again to end the co-op match for everyone" : "Press Esc again to leave the co-op match", GUIFont::Centre);
	}
}

#pragma endregion
