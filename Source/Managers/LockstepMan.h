#pragma once

#include "Singleton.h"
#include "Constants.h"
#include "UInputMan.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#define g_LockstepMan LockstepMan::Instance()

struct BITMAP;

namespace RakNet {
	class RakPeerInterface;
	struct Packet;
} // namespace RakNet

namespace RTE {

	class GameActivity;

	/// Online co-op multiplayer using deterministic lockstep.
	///
	/// Every peer runs the whole simulation. The only thing sent over the network is each player's input: every sim update, each peer captures its
	/// local player's input and sends it to the host tagged for a sim update a few updates in the future (the input delay). The host bundles the
	/// input of all players for each sim update and broadcasts the bundle. A peer only runs sim update N once it has bundle N, so every peer runs
	/// every update with exactly the same input, and (because the simulation is deterministic, see TimerMan::SetDeterministicMode) ends up in exactly
	/// the same state. Peers periodically exchange hashes of their simulation state so any desync is detected and reported.
	///
	/// Session flow: the host clicks Host in the main menu's Multiplayer screen (or starts the game with -coop-host) and then picks an Activity in the menus as
	/// usual, or passes -coop-activity to start one automatically once enough players have joined. Clients click Join in the Multiplayer screen (or start with
	/// -coop-join <address>). A client whose resolution differs from the host's switches to the host's, as the screen size affects the simulation. When the host starts an Activity, every connected client
	/// is added to it as an extra human player on the host's team (or watches, if the Activity has no free player slot left), and the Activity configuration is sent
	/// to the clients so they start the same one.
	class LockstepMan : public Singleton<LockstepMan> {

	public:
		/// What this machine is doing in a co-op session.
		enum class Role {
			None,
			Host,
			Client
		};

#pragma region Creation
		/// Constructor method used to instantiate a LockstepMan object in system memory.
		LockstepMan();

		/// Destructor method used to clean up a LockstepMan object before deletion from system memory.
		~LockstepMan();

		/// Shuts down any session and networking.
		void Destroy();
#pragma endregion

#pragma region Session Setup
		/// Parses the co-op command line arguments (-coop-host, -coop-join etc.) and starts hosting or joining if requested.
		/// @param argCount Argument count.
		/// @param argValue Argument values.
		void HandleCommandLine(int argCount, char** argValue);

		/// Starts hosting a co-op session.
		/// @param port The UDP port to listen on.
		/// @return Whether hosting started successfully.
		bool StartHosting(unsigned short port);

		/// Starts joining a co-op session.
		/// @param address The host's address.
		/// @param port The host's UDP port.
		/// @return Whether the connection attempt started.
		bool StartJoining(const std::string& address, unsigned short port);

		/// Starts hosting a co-op session, leaving any current session first. For the menus.
		/// @param port The UDP port to listen on.
		/// @return Whether hosting started successfully.
		bool HostSession(unsigned short port);

		/// Starts joining a co-op session, leaving any current session first. For the menus.
		/// @param address The host's address, optionally followed by :port.
		/// @return Whether the connection attempt started.
		bool JoinSession(const std::string& address);

		/// Leaves the current session, ending any running match (for everyone, when hosting), and restores the resolution if joining changed it.
		void LeaveSession();

		/// Restores the resolution from before joining, once no Activity is running (the resolution can't change during one).
		void RestoreLocalResolutionIfPossible();

		/// Gets a description of the session's state, for the menus and overlay.
		/// @return The status text.
		const std::string& GetStatusMessage() const { return m_StatusMessage; }

		/// Gets the names of the players in the session, the host's first. Empty on a client that hasn't been accepted yet.
		/// @return The player names.
		std::vector<std::string> GetLobbyPlayerNames() const;

		/// Gets whether the last resolution change was made by joining or leaving a session, and forgets it. Lets the menus go back to the co-op screen.
		/// @return Whether this changed the resolution since the last call.
		bool TakeResolutionChangedFlag() { return std::exchange(m_ChangedResolution, false); }

		/// Gets whether this client has been accepted into the host's session.
		/// @return Whether this machine is an accepted client.
		bool IsClientAccepted() const { return m_Role == Role::Client && m_ConnectedToHost && !m_LobbyNames.empty(); }

		/// Gets what this machine is doing in a co-op session.
		/// @return The role of this machine.
		Role GetRole() const { return m_Role; }

		/// Gets whether this machine is hosting or has joined a co-op session.
		/// @return Whether a session is active.
		bool IsInSession() const { return m_Role != Role::None; }

		/// Gets whether a lockstep match (a running Activity) is in progress.
		/// @return Whether a match is running.
		bool IsMatchRunning() const { return m_MatchRunning; }

		/// Gets whether the host has started a match that this client hasn't started yet. Used to leave the menus.
		/// @return Whether a match start is pending.
		bool IsMatchStartPending() const { return m_MatchStartPending; }

		/// Gets this computer's own values of the settings a co-op match uses the host's values of, while the host's are in effect. The settings file gets
		/// these instead of the host's (see SettingsMan::Save). Keyed by their Settings.ini property names, except "DeltaTimeTicks" (DeltaTime in ticks),
		/// "EnabledGlobalScripts" and "VisibleAssemblyGroups" (each a list of names, every one followed by ';').
		/// @return The local values, or nullptr if the local settings are in effect.
		const std::map<std::string, std::string>* GetLocalValuesOfSessionSettings() const { return m_SavedSettings.empty() ? nullptr : &m_SavedSettings; }
#pragma endregion

#pragma region Match Lifecycle
		/// Gets whether the Activity about to be started should be set up as a lockstep match: when hosting, if it's a kind of Activity co-op supports (otherwise it's
		/// played on the host only, and the clients keep waiting), and on clients only when the host started it.
		/// @param activity The Activity about to be started.
		/// @return Whether ActivityMan should call PrepareMatch and BeginMatch.
		bool WantsToPrepareMatch(const GameActivity* activity);

		/// Called by ActivityMan when an Activity is about to be (re)started. On the host this adds the connected clients to the Activity as players and
		/// sends its configuration to them; on every peer it then rebuilds the Activity from that configuration so all peers start exactly the same one.
		/// @param activity The Activity about to be started. Ownership IS transferred.
		/// @return The Activity to actually start. Ownership IS transferred.
		GameActivity* PrepareMatch(GameActivity* activity);

		/// Called by ActivityMan right before the Activity prepared by PrepareMatch is started. Switches the engine into lockstep operation.
		void BeginMatch();

		/// Stops lockstep operation and restores local settings and input.
		void EndMatch();

		/// Leaves the current match after the player confirms by pressing Esc twice. Hosts end the match for everyone.
		void RequestLeave();
#pragma endregion

#pragma region Per-Frame and Per-Update Hooks
		/// Pumps the network. Call once per frame, both in menus and in game.
		void Update();

		/// Gets whether the next sim update can run, i.e. the input for it has arrived. Always true when no match is running.
		/// @return Whether the next sim update can run.
		bool CanSimulateNextUpdate();

		/// Call at the start of every sim update, after UInputMan::Update. Captures and sends the local player's input and applies everyone's input for this update.
		void BeginSimUpdate();

		/// Call at the end of every sim update. Handles desync detection.
		void EndSimUpdate();

		/// Draws session status (waiting for players, desyncs etc.) over the frame.
		/// @param targetBitmap The bitmap to draw on.
		void DrawOverlay(BITMAP* targetBitmap);
#pragma endregion

	private:
		/// Network message identifiers, offset from RakNet's ID_USER_PACKET_ENUM.
		enum MessageType : uint8_t {
			MsgHello, //!< Client -> host: version and setup info.
			MsgReject, //!< Host -> client: the client can't join, and why.
			MsgLobby, //!< Host -> clients: how many peers are connected.
			MsgStart, //!< Host -> client: start a match with this configuration.
			MsgInput, //!< Client -> host: the client's input for a sim update.
			MsgTick, //!< Host -> clients: everyone's input for a sim update.
			MsgChecksum, //!< Client -> host: hash of the client's sim state at a sim update.
			MsgDesync, //!< Host -> clients: a desync was detected.
			MsgEndMatch, //!< Host -> clients: the host left the match.
			MsgPing, //!< Host -> client: round trip time measurement.
			MsgPong, //!< Client -> host: reply to MsgPing.
			MsgLeave, //!< Client -> host: this client left the match, or couldn't start it.
			MsgResolution //!< Host -> client: switch to this resolution and say hello again.
		};

		/// A connected client, as seen by the host.
		struct Peer {
			uint64_t Guid = 0; //!< RakNet GUID of the connection.
			std::string Address; //!< Network address, for messages.
			std::string Name; //!< The player's name, from their settings.
			bool Accepted = false; //!< Whether the client passed the version checks.
			InputDevice Device = InputDevice::DEVICE_KEYB_ONLY; //!< The input device the client plays with.
			float DigitalAimSpeed = 1.0F; //!< The client's digital aim speed setting.
			int Player = Players::NoPlayer; //!< The player the client controls in the current match.
			bool InMatch = false; //!< Whether the client was sent the current match's start (it may have no player and be watching), and hasn't left it.
			bool Connected = true; //!< Whether the client is still connected.
			std::chrono::steady_clock::time_point ConnectedSince = std::chrono::steady_clock::now(); //!< When the client connected, to drop connections that never say hello.
			float RoundTripMS = 0; //!< Smoothed round trip time to this client, as seen by the game loop (includes waiting for the next frame to process messages).
			int RoundTripSamples = 0; //!< How many round trip measurements have been made.
		};

		/// Configuration from the command line.
		struct LaunchOptions {
			int ExpectedPlayers = 2; //!< With AutoActivity, how many players (including the host) to wait for before starting.
			std::string AutoActivity; //!< Activity to start automatically on the host once enough players have joined.
			std::string AutoScene; //!< Scene for AutoActivity.
			int AutoDifficulty = 50; //!< Difficulty for AutoActivity.
			int AutoGold = 5000; //!< Starting gold for AutoActivity.
			bool AutoFog = true; //!< Fog of war for AutoActivity.
			bool AutoDeployUnits = true; //!< Whether to deploy the scene's units for AutoActivity.
			bool AutoClearPathToOrbit = false; //!< Whether AutoActivity requires a clear path to orbit from the brains placed in a build phase.
			int BotSeed = -1; //!< If not negative, the local player is driven by a pseudo-random input bot (for automated testing).
			int DesyncInjectTick = -1; //!< If not negative, deliberately perturb this machine's simulation at this sim update (for testing desync detection).
			bool InputDelayFixed = false; //!< Whether the input delay was set on the command line. Otherwise the host picks it from the measured round trip times.
			int SimulatedLatencyMS = 0; //!< Testing: artificial delay added to every outgoing message.
			int SimulatedJitterMS = 0; //!< Testing: additional random delay (0 to this) added to every outgoing message. Message order is preserved.
			std::vector<std::pair<std::string, std::string>> AutoChain; //!< Testing: Activities (and scenes) to start automatically one after another, in the same processes.
			long long MatchUpdates = 0; //!< Testing: if positive, every peer ends each match after this many sim updates (at the same update everywhere).
		};

		Role m_Role = Role::None; //!< What this machine is doing in a session.
		RakNet::RakPeerInterface* m_Peer = nullptr; //!< The RakNet peer. Owned.
		uint64_t m_HostGuid = 0; //!< Client: the host's GUID once connected.
		std::string m_HostAddress; //!< Client: the host's address.
		bool m_ConnectedToHost = false; //!< Client: whether the connection to the host is up.
		bool m_HelloSent = false; //!< Client: whether the hello message has been sent.
		std::string m_StatusMessage; //!< Status text shown in the overlay.
		std::string m_RejectReason; //!< Client: why the host rejected us, if it did.
		std::vector<Peer> m_Peers; //!< Host: connected clients.
		int m_LobbyPeerCount = 0; //!< Client: number of peers in the session, as last reported by the host.
		std::vector<std::string> m_LobbyNames; //!< Client: the names of the players in the session, as last reported by the host.
		int m_ResolutionRequests = 0; //!< Client: how many times the host asked us to switch resolution since connecting.
		int m_LocalResX = 0; //!< Client: the resolution before switching to the host's, to restore when leaving. 0 if not switched.
		int m_LocalResY = 0; //!< Client: see m_LocalResX.
		float m_LocalResMultiplier = 1.0F; //!< Client: see m_LocalResX.
		bool m_ChangedResolution = false; //!< Whether joining or leaving changed the resolution and the menus haven't been rebuilt for it yet.
		LaunchOptions m_Options; //!< Configuration from the command line.
		bool m_AutoStartDone = false; //!< Host: whether the automatic Activity start has happened.
		size_t m_AutoChainIndex = 0; //!< Host: which entry of the automatic Activity chain is being played.
		std::vector<std::pair<MessageType, std::vector<uint8_t>>> m_HeldBackMessages; //!< Client, testing with MatchUpdates: messages for the next match that arrived before this peer finished the current one.
		std::chrono::steady_clock::time_point m_LastPingTime; //!< Host: when round trip measurements were last sent.

		// Match state
		bool m_MatchRunning = false; //!< Whether a lockstep match is running.
		bool m_MatchStartPending = false; //!< Client: whether a match start was received but not yet acted on.
		bool m_DeferredMatchStart = false; //!< Client: whether a match start was received that waits for the switch to the host's resolution to complete.
		bool m_MatchEndedByHost = false; //!< Client: whether the host ended the current match (or the connection to it was lost), so leaving it needn't be reported.
		GameActivity* m_PendingMatchActivity = nullptr; //!< Client: the Activity built from the host's match configuration. Not owned once handed to ActivityMan.
		uint32_t m_MatchId = 0; //!< Identifies the current match, so stale messages from a previous one are ignored.
		std::string m_MatchConfig; //!< The current match's configuration, as sent by the host.
		int m_InputDelay = 4; //!< How many sim updates in the future local input is scheduled for.
		int m_LocalPlayer = Players::NoPlayer; //!< The player controlled from this machine.
		std::array<bool, Players::MaxPlayerCount> m_MatchPlayers{}; //!< Which players' input goes through lockstep (all human players in the match).
		std::array<InputDevice, Players::MaxPlayerCount> m_PlayerDevices{}; //!< The input device of each match player.
		std::array<float, Players::MaxPlayerCount> m_PlayerDigitalAimSpeeds{}; //!< The digital aim speed setting of each match player.
		std::array<int, Players::MaxPlayerCount> m_PlayerOwnerPeer{}; //!< Host: which peer index controls each player (-1 host, -2 nobody).
		long long m_NextSimUpdate = 0; //!< Index of the next sim update to run in this match.
		std::map<long long, std::array<VirtualInputFrame, Players::MaxPlayerCount>> m_UpdateInputs; //!< Input bundles for upcoming sim updates.
		std::chrono::steady_clock::time_point m_WaitingSince; //!< When we started waiting for the next bundle.
		bool m_Waiting = false; //!< Whether we're currently waiting for the next bundle.

		// Host bundle building
		std::array<std::map<long long, VirtualInputFrame>, Players::MaxPlayerCount> m_PendingPlayerInputs; //!< Host: inputs received but not yet bundled, per player.
		std::array<VirtualInputFrame, Players::MaxPlayerCount> m_LastPlayerInputs; //!< Host: the last input bundled for each player, repeated if a player's input is late.
		std::array<bool, Players::MaxPlayerCount> m_PlayerHasSentInput{}; //!< Host: whether each player has sent any input this match (i.e. has finished loading).
		std::array<bool, Players::MaxPlayerCount> m_PlayerLagging{}; //!< Host: whether each player's input timed out and hasn't caught up since. Their missing input is repeated without waiting.
		std::array<std::chrono::steady_clock::time_point, Players::MaxPlayerCount> m_PlayerLaggingSince{}; //!< Host: when each lagging player started lagging.
		bool m_StalledEscapeHeld = false; //!< Whether Esc was held on the previous frame while the sim was stalled.
		long long m_NextBundleUpdate = 0; //!< Host: the next sim update to build a bundle for.

		// Desync detection
		static constexpr int c_ChecksumInterval = 60; //!< How often (in sim updates) peers compare state hashes.
		static constexpr int c_TerrainChecksumInterval = 600; //!< How often the (expensive) terrain hash is included.
		static constexpr size_t c_HostChecksumsKept = 256; //!< Host: how many of its own state hashes it keeps for clients that are behind (at the default sim speed, about 4 minutes' worth).
		std::map<long long, std::array<uint64_t, 8>> m_HostChecksums; //!< Host: own state hashes by sim update.
		std::map<long long, std::vector<std::pair<int, std::array<uint64_t, 8>>>> m_ClientChecksums; //!< Host: client state hashes by sim update, waiting for the host's own.
		bool m_Desynced = false; //!< Whether a desync has been detected in this match.
		std::string m_DesyncMessage; //!< Description of the detected desync.
		long long m_ChecksumsCompared = 0; //!< Host: how many checksum comparisons succeeded, for the overlay and logs.

		// Leaving
		std::chrono::steady_clock::time_point m_LeaveRequestTime; //!< When Esc was first pressed to leave.
		bool m_LeaveRequested = false; //!< Whether Esc was pressed once and a second press would leave.

		// Saved local settings, restored after a match
		std::map<std::string, std::string> m_SavedSettings; //!< Local values of the settings the host dictates during a match.

		// Testing aids
		struct DelayedMessage {
			std::chrono::steady_clock::time_point SendTime; //!< When to actually send the message.
			uint64_t Guid; //!< Who to send it to.
			std::vector<uint8_t> Data; //!< The message.
		};
		std::vector<DelayedMessage> m_DelayedMessages; //!< Outgoing messages held back to simulate latency, in send order.
		std::minstd_rand m_NetworkSimulationRNG; //!< Generator for simulated jitter.
		std::minstd_rand m_BotRNG; //!< Generator for the input bot.
		VirtualInputFrame m_BotHeld; //!< The bot's currently held input.
		int m_BotHoldUpdates = 0; //!< How many more sim updates the bot holds its current input.
		float m_BotMousePosition[2] = {0, 0}; //!< The bot's absolute mouse position, for clicking around in menus.
		bool m_BotDragging = false; //!< Whether the bot's current input is a drag, which moves the mouse until the button is released.

#pragma region Networking
		/// Sends a message to one connection.
		void Send(const std::vector<uint8_t>& message, uint64_t guid);

		/// Actually hands a message to RakNet.
		void SendNow(const std::vector<uint8_t>& message, uint64_t guid);

		/// Sends any messages held back for simulated latency whose time has come.
		void FlushDelayedMessages();

		/// Sends a message to every accepted, connected client.
		void Broadcast(const std::vector<uint8_t>& message);

		/// Handles one received packet.
		void HandlePacket(RakNet::Packet* packet);

		/// Host: handles a message from a client.
		void HandleHostMessage(Peer& peer, MessageType type, const uint8_t* data, size_t size);

		/// Client: handles a message from the host.
		void HandleClientMessage(MessageType type, const uint8_t* data, size_t size);

		/// Host: sends the lobby status to all clients.
		void BroadcastLobbyStatus();

		/// Client: sends the hello message, which the host checks before accepting us.
		void SendHello();

		/// Client: switches to the host's resolution, remembering the local one to restore when leaving. Not possible while in an Activity.
		/// @param resX The host's horizontal resolution.
		/// @param resY The host's vertical resolution.
		/// @return Whether the resolution was changed to the host's.
		bool SwitchToHostResolution(int resX, int resY);

		/// Client: builds the Activity from the match configuration the host sent (m_MatchConfig) and sets it to start.
		void FinishMatchStartFromHost();

		/// Forgets all session and connection state, after the network peer has been shut down.
		void ResetSessionState();

		/// Host: finds a peer by GUID.
		Peer* FindPeer(uint64_t guid);
#pragma endregion

#pragma region Match Helpers
		/// Serializes everything needed to construct an identical Activity on another machine.
		std::string SerializeMatchConfig(const GameActivity* activity) const;

		/// Constructs an Activity from a serialized configuration, and applies the session settings in it.
		GameActivity* BuildActivityFromConfig(const std::string& config, int& localPlayerOut);

		/// Host: builds the Activity for the automatic start option.
		GameActivity* BuildAutoActivity() const;

		/// Host: builds and broadcasts bundles for which everyone's input has arrived (or has timed out).
		void HostBuildBundles();

		/// Gets a string describing this build and its data, which must match between peers.
		std::string GetCompatibilityString() const;

		/// Gets a hash of the order pairs() visits string keys in, in Lua. See the definition.
		static long long GetLuaStringOrderFingerprint();

		/// Gets the local player's input for a sim update, from devices or from the input bot.
		VirtualInputFrame CaptureLocalInput();

		/// Saves the local values of the settings the host dictates, and applies the given values.
		void ApplySessionSettings(const std::map<std::string, std::string>& values);

		/// Restores the local values of the settings the host dictated.
		void RestoreLocalSettings();

		/// Handles a detected desync.
		void ReportDesync(long long simUpdate, const std::string& description);
#pragma endregion

		// Disallow the use of some implicit methods.
		LockstepMan(const LockstepMan& reference) = delete;
		LockstepMan& operator=(const LockstepMan& rhs) = delete;
	};
} // namespace RTE
