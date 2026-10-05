# Online co-op (lockstep)

Online co-op lets 2–4 players play an Activity together, each on their own computer. It uses **deterministic lockstep**: every computer runs the whole simulation, and only the players' inputs are sent over the network. A match uses a few kilobytes per second per player, however big the battle gets.

> **Status: experimental.** All players need the **same build** of the game and the **same mods**. Only Scenario-style Activities (started from the Scenario menu) are supported.

## Playing

Everything is in the main menu's **Multiplayer** screen. Enter your name there; it's shown in the lobby.

### Host

1. Click **Host Game** (UDP port 7777 by default; change it in the box next to the button).
2. Wait for the other players to show up under *Players in session*.
3. Click **Choose Activity** (or go to *Scenario Battle* from the main menu), pick an Activity and scene, put yourself on a team as usual and start it. Every connected player joins your team as an extra human player, as long as the Activity has free player slots (four in all); anyone left over watches the match instead, and is told so. Players who connect after the match has started join the next one.

Players outside your local network need to reach your UDP port: forward it on your router.

### Clients

Enter the host's address (e.g. `192.168.1.20`, or `192.168.1.20:7778` for another port) and click **Join Game**. The game keeps retrying until the host is up, and starts the match by itself when the host starts one, whichever menu you're in.

If your game resolution differs from the host's, the game switches to the host's resolution when you join (the screen size affects gameplay, e.g. how far actors can see), checks it again whenever the host starts a match (in case either of you changed it in the video settings meanwhile), and switches back when you leave. If the host's resolution is bigger than your screen, the window is scaled down to fit. If switching fails, the game says so; set a matching resolution in the video settings and join again.

**Leave Session** disconnects. Leaving the main menu screen doesn't.

### Command line

Sessions can also be started from the command line, which skips the intro:

```
CortexCommand -coop-host [port]                     # host, default port 7777 (UDP)
CortexCommand -coop-join <host address>[:port]      # join
```

To start a specific Activity automatically once everyone has joined:

```
CortexCommand -coop-host 7777 -coop-players 2 -coop-activity "Bunker Breach" -coop-scene "Zekarra Mining Outpost"
```

Without `-coop-scene`, the Activity's default scene is used, or the first compatible scene by name if it has none. Optional host settings:

- `-coop-difficulty <0-100>`: difficulty of the automatic Activity.
- `-coop-gold <amount>`: starting gold of the automatic Activity.
- `-coop-fog <0|1>`: fog of war for the automatic Activity.
- `-coop-delay <sim updates>`: fixed input delay; see below. By default the host picks it when the match starts, from the slowest player's measured round trip time (between 3 and 20 sim updates, i.e. 50–333 ms at the default sim speed).

### During a match

- Your screen shows only your own player, filling the window.
- **Esc twice** leaves the match, also while the game is waiting for other players. If the host leaves, the match ends for everyone.
- Pausing, restarting, quick save/load and script reloading are disabled, because doing them on one computer only would break the sync. For the same reason the console doesn't run commands during a match.
- The buy menu offers the built-in loadouts only, not the ones you saved yourself (each computer would otherwise use its own file for every player). Your saved loadouts are kept for single player.
- On Linux the game restarts itself once at start-up, to use the same math library code on every CPU (see below). `-no-math-restart` skips that, e.g. when debugging, at the risk of being refused when joining.

### What's checked when you join

The host rejects a player whose game version, mods, audio availability, math library results or Lua setup (LuaJIT on or off, string hashing, case-sensitive paths) don't match its own, and says why. Mods (and the official content) are compared by the contents of their files, not by name or version, ignoring line endings and the files operating systems and editors leave around (sounds and music only by their size and the parts their length is read from, as only their lengths affect gameplay); the message names the first module that differs. A player whose resolution differs is switched to the host's (see above). Each player's input device and digital aim speed setting are recorded when they join, and every computer uses those for that player. For the duration of a match, clients use the host's values for settings that affect gameplay:

- AI update interval
- automatic gold deposit
- path finder grid size
- crab bombs
- max unheld items
- scrap compacting
- particle settling and MO subtraction
- sim delta time
- enabled global scripts
- buy menu options
- which groups the editors' object pickers always show

Local settings are restored afterwards.

## How it works

- Every sim update, each computer captures its local player's input (input elements, analog sticks, mouse movement and buttons, cursor position, and which keyboard keys are held). It sends that to the host, tagged for a sim update a few updates in the future (the *input delay*). The host measures each player's round trip time while in the menus and sets the input delay so input normally arrives before it's needed. The top line of the screen shows the current value.
- The host bundles everyone's input for each sim update and sends the bundle to all players. A computer only runs sim update N once it has bundle N.
- In-game menus (pie menu, buy menu, inventory, object pickers) read their input through the same per-player input path (`UInputMan` virtual input). They therefore work, and stay in sync, without any special handling.
- Code that asks about keys, mice or gamepads without naming a player (scripts checking `UInputMan:KeyPressed(Key.SPACE)`, "any key" checks, Shift in the buy menu) gets answers from the players' synced input too: "a key on this computer" becomes "a key of any player". Raw gamepad queries read as idle. Player slots that nobody uses get no input at all.
- If a player's input is more than 3 seconds late, the host marks them as lagging and repeats their previous held input, so one stalled or disconnected computer doesn't freeze everyone. While a player is lagging, the host doesn't wait for them at all; input that arrives too late for its own sim update is used for the next one, so they keep playing while their computer catches up. After 10 seconds of lag their held input is no longer repeated, so their actor stops. Players still loading are waited for. A player who disconnects or leaves stands idle for the rest of the match.
- Every 60 sim updates, each client sends a hash of its simulation state to the host. The hash covers every movable object (position, velocity, rotation, mass, team, health, AI mode, aim, inventory, equipped item, AI path and waypoints, attachables), the random generators, the terrain (every 600 updates) and the activity (team funds and deaths, each player's brain and controlled actor). On a mismatch, everyone sees a **DESYNC** message saying which part of the state differs (RNG, Lua RNG, actors, items, particles, terrain, activity). Each computer also writes a `CoopDesync_*.txt` state dump in the `Userdata` folder.

### What keeps the simulation identical

Determinism changes in the engine (see also `LockstepMultiplayerFeasibility.md`). All of these are active only in `TimerMan` deterministic mode, which a co-op match switches on:

- Real-time timers count sim time, so C++ and Lua gameplay code using real-time timers behaves identically everywhere.
- Async pathing results are published at a fixed point, in a fixed order.
- Threaded Lua scripts run one Lua state after another rather than in parallel. Some scripts read objects owned by other states, which would otherwise race.
- See rays and MOID drawing finish within the sim update.
- Sound playback state, which gameplay code checks (e.g. weapon pre-fire sounds), is tracked in sim time instead of asked of the audio system. It's worked out from the sounds' lengths, so those are part of the content check.
- Camera shake is applied only when drawing, because scripts read camera offsets. Every player's camera is simulated on every computer, although only one is drawn.
- Every player gets one full-size screen at the shared resolution, so screen-size-dependent gameplay values match.
- Nothing worked out while drawing this computer's screen feeds back into the simulation (e.g. `AboveHUDPos`, which scripts spawn effects at, and the funds-changed flag).
- Lua garbage collection runs on the main thread at a fixed point, one Lua state after another. On worker threads, the order objects were freed back to the engine's memory pools depended on thread timing.
- Path cost updates wait for background path requests to finish instead of being skipped if any are running.

Always on:

- A fixed number of Lua states.
- Per-state RNGs and unique ID ranges.
- Each scripted object runs in the Lua state picked by its unique ID. It used to be whichever state's turn it was, and every object loading scripts on the main thread took a turn, including ones outside the simulation, so the same object could run in a different state (and draw from a different random generator) on another computer.
- Ordered script registration.
- Terrain cleaning and fog-of-war reveal processing done in the sim update instead of when drawing.
- LuaJIT built without randomised string hashing, and with string IDs (which Lua tables hash string keys by) computed from the string's content instead of the order strings were created in. Otherwise `pairs()` over string keys would depend on everything a Lua state had ever done, which differs between computers.
- Folder scans (module `.ini` files, Lua's `GetDirectoryList`/`GetFileList`) sorted by name, instead of file system order.
- Uninitialised fields that could carry leftover memory from a previous object (path nodes, atoms, actors' movement state) are initialised.
- Shipped scripts that looped over tables keyed by objects (`pairs()` order follows memory addresses, which differ between computers) now use `SortedPairs` from `Base.rte/Utilities.lua`, ordered arrays, or tie-breaks on `UniqueID`.
- **The same math library code on every CPU.** The x86-64 math libraries choose faster FMA-based versions of double-precision `sin`, `cos`, `exp`, `log`, `pow`, `atan2` and others on CPUs that support FMA. These give a slightly different result for a fraction of a percent of inputs, enough to desync a Haswell-or-newer PC from an older one (or a low-end Pentium/Celeron) over time; Lua's `math` functions and `^` use them. On Windows the game switches the FMA versions off at start-up. On Linux it restarts itself once at start-up with FMA hidden from glibc (`GLIBC_TUNABLES`), since a session can be hosted or joined at any time. A fingerprint of the math library's results is part of the join check, so any remaining difference is refused instead of desyncing.

## Testing

`Tools/Determinism/` has the tools used to test this. The in-game harness logs per-sim-update state hashes on both computers. Set `CCCP_DT_OBSERVE=1` when running a co-op session.

`Tools/Determinism/stress/stress.py` runs a suite of stress tests: co-op sessions with every player driven by an input bot, under hostile conditions (constant war in a test activity, other CPUs, starved and jittery frame rates, scrambled memory, a bad network, a frozen client, and more). See `Tools/Determinism/README.md`.

Test-only options:

- `-coop-bot <seed>`: drives the local player with a pseudo-random input bot.
- `-coop-inject-desync <update>`: deliberately perturbs the simulation on one computer, to check the desync detector.
- `-coop-sim-latency <ms>` and `-coop-sim-jitter <ms>`: hold back every message this computer sends by the given latency plus a random 0 to jitter ms, keeping message order (like a reliable ordered connection over a slow link). Pass them to every game instance to test a slow network with all instances on one machine.

Example, two computers on one machine:

```
CCCP_DT_LOG=host.log CCCP_DT_OBSERVE=1 CCCP_DT_TICKS=3000 ./CortexCommand -coop-host 7777 -coop-players 2 -coop-activity "Bunker Breach" -coop-scene "Zekarra Mining Outpost" -coop-bot 1 &
CCCP_DT_LOG=client.log CCCP_DT_OBSERVE=1 CCCP_DT_TICKS=3000 ./CortexCommand -coop-join 127.0.0.1:7777 -coop-bot 2
Tools/Determinism/compare_logs.py host.log client.log
```

### Results so far

All on one Linux machine, two to three game instances, bots driving every player:

| Test | Result |
|---|---|
| Bunker Breach, 2 players, 3000 sim updates, several bot seeds | Identical on all computers |
| Bunker Breach, 3 players | Identical |
| Bunker Breach, 60–80 ms simulated latency (60 ms plus up to 20 ms jitter) on every message | Identical; the auto input delay keeps the normal speed |
| Wave Defense, started from the Scenario menu by hand; the client joined as player 2 automatically | Identical |
| Wave Defense, 4000 sim updates, bots placing objects in the build phase | Identical |
| `-coop-inject-desync` | Desync detected at the next check, with the differing part named |
| Client killed during a match | Host carries on after 3 s; that player stands idle |
| Host killed during a match | Clients end the match, return to the main menu and keep trying to reconnect |

The stress test suite (`Tools/Determinism/stress/stress.py`, see `Tools/Determinism/README.md`) then passed every scenario: identical on every peer for 2000-4000 sim updates of constant war in the *Determinism Chaos* test activity (2 and 4 players), with the client's math library on its non-FMA code paths, with one peer starved onto a single CPU core, with random frame pacing, with scrambled memory and no ASLR, under a different locale, over a bad network with a client frozen for 5 seconds, in eight stock activities, and for a 20000-update (about 5.5 minutes) chaos match. The negative tests were caught: a deliberately perturbed peer (desync reported in game) and a mismatched math library (refused at join).

Getting there, the suite found and the fixes cover: LuaJIT ordering string-keyed tables by creation history, a heap-corrupting use-after-free in the audio code when objects were deleted mid-sound (found with an AddressSanitizer build), a crash on a missing sound channel entry, actors reading items and move targets that had been deleted, and flames reading the object they stuck to after it was deleted.

A later AddressSanitizer sweep over every stock activity found more objects being read after deletion: a brain deleted from the item or particle list (and, in co-op, a brain shared by two players), a drop ship in Decision Day, and about 40 shipped scripts that kept objects across updates. Under heavy CPU load it also showed rare desyncs. Per-tick state dumps of both peers (`CCCP_DT_DUMP_RING`) pinned one down: a weapon's trail-particle script kept moving its trail particle after it had been deleted, and the memory had gone to an unrelated new particle, a different one on each peer. Scripts checked their stored objects with `MovableMan:IsParticle`/`IsActor`/`ValidMO`, which only compare addresses, and the entity pools handed out the most recently freed memory first. Now every shipped script that keeps an object between updates stores its unique ID and looks it up with `MovableMan:FindObjectByUniqueID` (exact), actors do the same for their move target, and the pools reuse the memory that was freed longest ago, which makes such mix-ups very unlikely for mods' scripts too. Scripted objects are also assigned to Lua states by unique ID now (see above).

## Known limitations

- **Mods' scripts that keep objects between updates** and check them with `MovableMan:IsParticle`/`IsActor`/`ValidMO` can still, rarely, act on the wrong object after theirs was deleted, which desyncs. Mods should keep `UniqueID`s and use `MovableMan:FindObjectByUniqueID`. The desync detector reports such cases; `CCCP_DT_DUMP_RING` and `CCCP_DT_LUA_RNG_LOG` (see `Tools/Determinism/README.md`) help pin them down.
- **AddressSanitizer builds** still show an occasional desync in the *Determinism Chaos* test activity (one of three 1500-update runs in the last check), starting in a single flame particle's velocity with every other object identical. ASan gives every object its own allocation at an address that differs between runs, which the normal build's pools don't; it hasn't been seen in normal builds since the fixes above (the full stress suite, including a 20000-update match, and six further 4000-update chaos matches were identical).
- **Same build only.** Windows and Linux builds can't play together; neither can different compilers or compiler settings. See the feasibility report for what cross-platform play needs: own RNG distributions and a deterministic math library.
- **Same game resolution on every computer.** Joining switches to the host's resolution automatically (and back when leaving); the window can still be scaled.
- **One player per computer.** No local split-screen in a co-op match.
- **Joining:** no joining or rejoining mid-match. A player who dropped out can reconnect and join the next match.
- **Content check:** sound and music files are compared by their size and the parts their length is read from, not entirely (their lengths affect gameplay, see above, but nothing else about them does). User-made scenes and saved games (`Userdata`) aren't compared. A user-made scene with the same name as someone else's will desync.
- **Scripts:**
  - Mods whose scripts read state outside the engine's control (`os.clock`, `io`, `TimerMan:TimeForSimUpdate()`, the mouse position without a player), or keep tables keyed by objects and act on `pairs()` order, can still desync. The desync detector will report it. Mods can use `SortedPairs` from `Base.rte/Utilities.lua` for object-keyed tables.
  - Scripts that read `FrameMan.PlayerScreenWidth` behave as if every player had a full screen at the shared resolution.
- **Activities:** Conquest (MetaGame) battles, the tutorial, editor Activities and saved games aren't supported. Started while hosting, they're played on the host's computer only, and the other players keep waiting in the lobby.
- **Raw keyboard input in GUIs** (e.g. typing into a text box in the buy menu) isn't sent, so it doesn't work for any player in a match.
