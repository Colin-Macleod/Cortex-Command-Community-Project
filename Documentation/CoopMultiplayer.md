# Online co-op (lockstep)

Online co-op lets 2–4 players play an Activity together, each on their own computer. It uses **deterministic lockstep**: every computer runs the whole simulation, and only the players' inputs are sent over the network. A match uses a few kilobytes per second per player, however big the battle gets.

> **Status: experimental.** All players need the **same build** of the game, the **same mods**, and the **same game resolution**. Only Scenario-style Activities (started from the Scenario menu) are supported.

## Playing

### Host

Start the game with:

```
CortexCommand -coop-host [port]          # default port 7777 (UDP)
```

Pick an Activity in the Scenario menu as usual, with yourself as the only human player. When you start it, every connected player joins your team as an extra human player. Players who connect after the match has started join the next one.

To start a specific Activity automatically once everyone has joined, use:

```
CortexCommand -coop-host 7777 -coop-players 2 -coop-activity "Bunker Breach" -coop-scene "Zekarra Mining Outpost"
```

Optional host settings:

- `-coop-difficulty <0-100>`: difficulty of the automatic Activity.
- `-coop-gold <amount>`: starting gold of the automatic Activity.
- `-coop-fog <0|1>`: fog of war for the automatic Activity.
- `-coop-delay <sim updates>`: input delay; see below. Default 4, which is about 67 ms.

The host needs to forward the UDP port, or be on the same LAN as the other players.

### Clients

```
CortexCommand -coop-join <host address>[:port]
```

The game goes straight to the main menu and waits for the host to start a match. A status line at the top of the screen shows the connection state. The client keeps retrying until the host is up.

### During a match

- Your screen shows only your own player, filling the window.
- **Esc twice** leaves the match. If the host leaves, the match ends for everyone.
- Pausing, restarting, quick save/load and script reloading are disabled, because doing them on one computer only would break the sync.
- Typing in the console still works, but anything you run there only affects your own computer and will cause a desync.

### What's checked when you join

The host rejects a player whose game version, loaded mods, audio availability or game resolution don't match its own, and says why. For the duration of a match, clients use the host's values for settings that affect gameplay:

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

Local settings are restored afterwards.

## How it works

- Every sim update, each computer captures its local player's input (input elements, analog sticks, mouse movement and buttons, cursor position). It sends that to the host, tagged for a sim update a few updates in the future (the *input delay*).
- The host bundles everyone's input for each sim update and sends the bundle to all players. A computer only runs sim update N once it has bundle N.
- In-game menus (pie menu, buy menu, inventory, object pickers) read their input through the same per-player input path (`UInputMan` virtual input). They therefore work, and stay in sync, without any special handling.
- If a player's input is more than 3 seconds late, the host repeats their previous held input, so one stalled computer doesn't freeze everyone. Players still loading are waited for.
- Every 60 sim updates, each client sends a hash of its simulation state to the host. On a mismatch, everyone sees a **DESYNC** message saying which part of the state differs (RNG, Lua RNG, actors, items, particles, terrain). Each computer also writes a `CoopDesync_*.txt` state dump in the `Userdata` folder.

### What keeps the simulation identical

Determinism changes in the engine (see also `LockstepMultiplayerFeasibility.md`). All of these are active only in `TimerMan` deterministic mode, which a co-op match switches on:

- Real-time timers count sim time, so C++ and Lua gameplay code using real-time timers behaves identically everywhere.
- Async pathing results are published at a fixed point, in a fixed order.
- Threaded Lua scripts run one Lua state after another rather than in parallel. Some scripts read objects owned by other states, which would otherwise race.
- See rays and MOID drawing finish within the sim update.
- Sound playback state, which gameplay code checks (e.g. weapon pre-fire sounds), is tracked in sim time instead of asked of the audio system.
- Camera shake is applied only when drawing, because scripts read camera offsets.
- Every player gets one full-size screen at the shared resolution, so screen-size-dependent gameplay values match.

Always on:

- A fixed number of Lua states.
- Per-state RNGs and unique ID ranges.
- Ordered script registration.
- Terrain cleaning and fog-of-war reveal processing done in the sim update instead of when drawing.
- LuaJIT built without randomised string hashing.

## Testing

`Tools/Determinism/` has the tools used to test this. The in-game harness logs per-sim-update state hashes on both computers. Set `CCCP_DT_OBSERVE=1` when running a co-op session.

Two test-only options:

- `-coop-bot <seed>`: drives the local player with a pseudo-random input bot.
- `-coop-inject-desync <update>`: deliberately perturbs the simulation on one computer, to check the desync detector.

Example, two computers on one machine:

```
CCCP_DT_LOG=host.log CCCP_DT_OBSERVE=1 CCCP_DT_TICKS=3000 ./CortexCommand -coop-host 7777 -coop-players 2 -coop-activity "Bunker Breach" -coop-scene "Zekarra Mining Outpost" -coop-bot 1 &
CCCP_DT_LOG=client.log CCCP_DT_OBSERVE=1 CCCP_DT_TICKS=3000 ./CortexCommand -coop-join 127.0.0.1:7777 -coop-bot 2
Tools/Determinism/compare_logs.py host.log client.log
```

## Known limitations

- **Same build only.** Windows and Linux builds can't play together; neither can different compilers or compiler settings. See the feasibility report for what cross-platform play needs: own RNG distributions and a deterministic math library.
- **Same game resolution on every computer.** The window can still be scaled with the resolution multiplier.
- **One player per computer.** No local split-screen in a co-op match.
- **Joining:** no joining mid-match, and no reconnecting.
- **Scripts:**
  - Mods whose scripts read real-time state outside the engine's control (e.g. `os.clock`), or keep tables keyed by objects and act on `pairs()` order, can still desync. The desync detector will report it.
  - Scripts that read `FrameMan.PlayerScreenWidth` behave as if every player had a full screen at the shared resolution.
- **Activities:** Conquest (MetaGame) and editor Activities aren't supported.
- **Raw keyboard input in GUIs** (e.g. typing into a text box in the buy menu) isn't sent, so it doesn't work for any player in a match.
