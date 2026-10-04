# Lockstep multiplayer: feasibility study

**Bottom line:** feasible. On one platform and one build, the simulation can be made deterministic with modest, contained changes. A small prototype set of fixes took identical runs from **diverging at sim tick 2–4** to **bit-identical for 6,000 ticks** (100 s of a full AI battle). The real cost of lockstep multiplayer is elsewhere: turning menus and UI into network commands, removing client-local inputs from the sim (screen size, settings, real time), and, if wanted, cross-platform play. Cross-platform needs a deterministic math library and RNG.

Everything below was measured on Linux x86-64 with GCC 13 and the meson release build, headless under Xvfb, using the harness and tools added in this branch (`Source/System/DeterminismHarness.*`, `Tools/Determinism/`).

---

## 1. Background: why lockstep fits this game

Cortex Command simulates thousands of pixel particles, destructible terrain and Lua-scripted AI. Syncing that state over the network is impractical. The current multiplayer avoids the problem by streaming rendered frames from the server: `NetworkServer` sends per-player frame-buffer deltas and clients send raw mouse and key input.

Deterministic lockstep sends only inputs: about 40 control bits plus a few analog values per player per tick. Every peer runs the full simulation. This scales to the current game's complexity at almost no bandwidth cost, and gives replays for free.

What it requires: every peer must compute **bit-identical** results, every tick, forever. Even one different float bit or one different RNG draw eventually changes the whole battle, as the measurements below show.

Useful foundations already exist:
- **Fixed timestep:** 60 Hz in `TimerMan`.
- **Deterministic seeding:** a constant-seed global RNG, reseeded in `Activity::Start`, with an explicit "for determinism" comment.
- **Per-Lua-state RNGs:** each Lua state has its own RNG, with a "for determinism" comment.
- **Transport:** RakNet, with NAT punch-through and a lobby.

## 2. Baseline measurement: is the game deterministic today?

**No, not even on the same machine with the same binary.**

| Test (Bunker Breach, Zekarra Mining Outpost) | Result |
|---|---|
| Run A vs run B, same binary, same machine | **Diverges at tick 2–4** (global RNG; varies run to run). Particle counts differ by tick 17. By tick 3000 the battles are unrecognisable (84 vs 164 particles). |
| Same, with fog of war off (removes the threaded see-ray race) | Still diverges at tick 2 |

The RNG tracer (`-DRTE_RNG_TRACE`) identified the cause:

> During `ThreadedUpdateAI`, Lua scripts on worker threads call into C++ that draws from the **single global `std::mt19937`**. For example, `AHuman::LookForMOs` made 18,300 draws from worker threads in 1,000 ticks, and `AudioMan::PlaySoundContainer` also draws when equipping devices. Several threads race on one generator, so the order of draws depends on OS scheduling. Every later random event then differs, starting with bullet penetration in `SceneMan::TryPenetrate` and particle physics.

## 3. Prototype fixes and measurement after them

These fixes are in this branch as an experiment. Each is small:

| # | Fix | Where |
|---|---|---|
| 1 | `RandomNum()` called from a thread that is running a threaded Lua state now draws from **that state's own RNG**, not the shared global one. Each state is updated by one thread at a time in a fixed order, so this is deterministic. | `RTETools.h` (`g_ThreadRandomGeneratorOverride`), `LuaMan::SetThreadLuaStateOverride` |
| 2 | Actor see rays, which run in parallel and use the RNG, use a generator seeded from (actor ID, sim tick), so the result doesn't depend on which worker runs them. | `MovableMan::Update` |
| 3 | Purely cosmetic randomness now uses a separate per-thread `CosmeticRandomGenerator()`, so render and audio frequency can't perturb the sim. This covers sound selection and pitch, music, screen shake, glow and HUD effects, and particle trail length. | `AudioMan`, `SoundSet`, `DynamicSong`, `CameraMan`, `PostProcessMan`, `HDFirearm`, `AHuman`, `Atom` |
| 4 | Threaded-script update order follows registration order. It used to iterate an `unordered_set<MovableObject*>`, i.e. heap-address order. | `LuaStateWrapper::m_RegisteredMOs` |
| 5 | Lua-state RNGs are reseeded and round-robin state assignment is reset at every `Activity::Start`. They were seeded once at boot, so the second match of a session played differently from the first. | `LuaMan::ResetRandomGeneratorsAndStateAssignment` |
| 6 | LuaJIT built with `LUAJIT_SECURITY_PRNG=0` and `LUAJIT_SECURITY_STRHASH=0` (see §4.4). | `external/sources/LuaJIT-2.1` (meson and vcxproj) |
| 7 | The pathfinding refresh timer runs on sim time instead of real time. | `Scene::Update` |

Results after the fixes. Same binary unless stated; each 6,000-tick run is about 100 s of game time.

| Test | Result |
|---|---|
| Bunker Breach, run A vs run B, 1,500 ticks | **IDENTICAL** |
| Bunker Breach, run A vs run B, 6,000 ticks | **IDENTICAL** |
| One-Man Army, run A vs run B, 3,000 ticks | **IDENTICAL** |
| Bunker Breach, normal vs pinned to **1 CPU core** (very different thread timing), 6,000 ticks | RNG, Lua RNGs, every actor, item and particle **identical**. Terrain hash differs transiently (see open item) |
| Release `-O2` build vs `-O0` build, 1,500 ticks | **Identical**, apart from the same transient terrain blip |
| `-O2` vs `-O0` libm and FP micro-test | Identical |
| 4 Lua states vs **2 Lua states** (simulates a 2-core machine) | **Diverges at tick 1.** The Lua-state count must be fixed and the same on every peer. Today it is `hardware_concurrency()`. |
| 960×540 vs **1280×720** resolution | **Diverges at tick 13.** Screen size feeds the sim: AI look distance, bullet lethal range, and 70 uses of `FrameMan.PlayerScreenWidth/Height` in Lua AI. |

**Open item:** in about 3 of 10 runs, the terrain material hash at a few sample ticks (e.g. 1120 and 2300) took one of two values, then reconverged within 10 ticks. All other state stayed identical through 6,000 ticks. It persists when every thread pool is idle before hashing, so it is not a harness read race. It vanished whenever dump instrumentation was added, so it looks like a timing-dependent terrain write whose end result is the same. It needs a follow-up with a per-tick terrain hash, ideally on a dedicated machine.

## 4. Primitive-level tests

### 4.1 RNG distributions (`micro/rng_dist.cpp`, `micro/rng_msvc_emulation.cpp`)

`std::mt19937` is fully specified by the standard and was identical everywhere. **The `std::uniform_*_distribution` classes are not specified**, and the game uses them for every random number.

| Engine call | libstdc++ vs libc++ | libstdc++ (Linux) vs MSVC STL (Windows)* | x86-64 vs 32-bit x87 |
|---|---|---|---|
| `RandomNum<float>()`, physics | same | **66% of draws differ** | same |
| `RandomNum<double>()`, Lua `PosRand`/`math.random()` | same | **67% differ** | **differs** |
| `RandomNum<int>(0,99)`, Lua `SelectRand`/`math.random(a,b)` | **99% differ** | same | same |
| `RandomNormalNum<float>()` | same | (not ported) | **differs** |

\*MSVC can't run here. The MSVC column uses a hand port of `generate_canonical` and `_Rng_from_urng_v2` from microsoft/STL `main`. The likely cause is that MSVC truncates to 24/53 bits while libstdc++ rounds a float conversion.

**Implication:** Windows and Linux builds cannot play together until the engine has its own distribution code. That is easy: about 50 lines of integer-only code in `RandomGenerator`.

### 4.2 libm and floating-point flags (`micro/fp_libm.c`)

Compared against `gcc -O2` with glibc:

| Variant | sinf/cosf | atan2f | powf/expf/logf | double sin/cos/pow | chaotic physics loop |
|---|---|---|---|---|---|
| clang -O2, same glibc | same | same | same | same | same |
| gcc -O0 | same | same | same | same | same |
| `-ffast-math` (≈ MSVC `/fp:fast`) | same | same | **DIFF** | same | **DIFF** |
| `-march=haswell -ffp-contract=fast` (FMA) | same | same | same | same | **DIFF** |
| **musl libm** (stand-in for the MSVC CRT or Apple libm) | **DIFF** | **DIFF** | same | **DIFF** | **DIFF** |
| 32-bit x87 | DIFF | DIFF | same | DIFF | DIFF |

**Implications:**
- Same compiler, same flags and same libm are bit-identical, even across optimisation levels. That is what made the same-build results in §3 possible.
- `RTEA.vcxproj` sets `<FloatingPointModel>Fast</FloatingPointModel>` in every configuration, which licenses the compiler to reassociate and contract FP. Two MSVC builds of the same source are not guaranteed to agree; in practice the same .exe will agree with itself. Use `/fp:precise` (or `/fp:strict`) for multiplayer builds.
- Cross-platform play needs sim code to stop using the platform `sin`/`cos`/`atan2`/`pow`/`exp`/`log`. `Vector::RadRotate`, `Vector::GetAbsRadAngle`, `Matrix` and `AHuman` all call them. Lua's `math` library calls them too. The options are a bundled, correctly-rounded or fixed implementation (e.g. CORE-MATH or a small polynomial set). FMA contraction must also be disabled explicitly (`-ffp-contract=off`), because clang on ARM, i.e. Apple Silicon, contracts by default.

**Same build, different CPU (found later, `micro/libm_cpu_paths.c`):** the table above compares builds. glibc also picks an FMA/AVX2 implementation of double `sin`, `cos`, `exp`, `log`, `pow`, `atan2`, `atan`, `asin` and `acos` at load time when the CPU has FMA, and Microsoft's x64 UCRT does the same. Masking FMA with `GLIBC_TUNABLES=glibc.cpu.hwcaps=-FMA,-FMA4` changes their results in the last bit for 0.03-0.07% of inputs (glibc 2.39), while the float versions don't change. So even the same binary can disagree between, say, a Haswell-or-newer PC and an older or low-end CPU, through Lua (whose numbers are doubles) and the few double calls in C++. None of the million samples still differed once converted to float, which is why short same-build tests didn't show it. The co-op implementation now selects the portable code paths at start-up (`Source/System/MathConsistency.*`) and puts a fingerprint of the math library's results in the join check.

### 4.3 LuaJIT JIT vs interpreter (`micro/lua_jit_vs_interp.c`)

2 M iterations of trig, sqrt, pow, exp and log vector math gave **identical** bits with the JIT on and off on x86-64. I did not test ARM64, where the JIT's FMA use should be checked.

### 4.4 LuaJIT table iteration order (`micro/lua_pairs_order.c`)

| Build | String-keyed `pairs()` | Table-keyed `pairs()` |
|---|---|---|
| As shipped (security defaults) | **different every process** | different every process |
| `LUAJIT_SECURITY_PRNG=0`, `STRHASH=0` (fix #6) | **stable** | different every process; stable only with ASLR off |

Content in `Data/` uses `pairs(` about 300 times (52 files). It also uses 17 `table.sort` calls with possible ties, and `_ScriptedObjects` is keyed by object ID. String-keyed iteration is fixed by #6. **Table- or userdata-keyed iteration can never be made deterministic across machines**, because it hashes on memory addresses. Scripts that iterate such tables and act on the order need auditing, or need to sort keys first.

## 5. Remaining hazards (static audit)

These are not covered by the prototype. File references are approximate.

**Must fix for any lockstep, same platform:**
1. **Fixed Lua-state count.** `LuaMan::Initialize` uses `hardware_concurrency()`, and `NumberOfLuaStatesOverride` already exists. Make it a constant in multiplayer.
2. **Screen size feeds the sim.**
   - C++: `MOPixel.cpp:22` (default lethal range), `HDFirearm.cpp:718`, `Actor.cpp:1314`, `AHuman.cpp:1394`, `ACrab.cpp:706`, `GameActivity.cpp` (landing zone, cursor).
   - Lua: AI behaviours use `FrameMan.PlayerScreenWidth` about 70 times.
   - Fix: a constant "sim view size", independent of resolution and split-screen count.
3. **Real time in sim logic.**
   - C++: `Controller.cpp` (analog acceleration and release timers), `PieMenu.cpp` (hover and sub-menu timers decide which slice is chosen), `GameActivity.cpp` (game-over timer), `GATutorial.cpp`.
   - Lua: about 52 uses of `ElapsedRealTime*` and `IsPastRealMS` in 19 files, including AI shot timing in `HumanBehaviors.lua` and BuyDoor cooldowns.
   - Fix: switch to sim time.
4. **Object creation from worker threads.**
   - `MovableObject::m_UniqueIDCounter` is a global atomic incremented from all threads, and is never reset per activity.
   - `m_AddedActors` / `m_AddedItems` / `m_AddedParticles` are filled under a mutex, in thread-completion order.
   - The prototype runs didn't hit this, but a script that spawns objects in `ThreadedUpdate` would. Fix: per-state ID and spawn queues, merged in state order.
5. **Async pathfinding.**
   - Results are picked up on whichever tick they complete (`Actor.cpp:1096`, Lua path callbacks).
   - Fix: use the existing `ForceImmediatePathingRequestCompletion` behaviour, or deliver results at a fixed tick offset.
   - It didn't diverge in the 1-core test, but it is a real race.
6. **Sim reads of local settings.**
   - `AIUpdateInterval`, `AutomaticGoldDeposit`, `EnableCrabBombs`/`CrabBombThreshold`, `PathFinderGridNodeSize`, enabled global scripts, `DisableLuaJIT`, `BlipOnRevealUnseen`.
   - Fix: lobby-synchronised match settings.
7. **Direct `UInputMan` reads in sim code**, bypassing `Controller`: `ACrab.cpp:914`, `GameActivity.cpp:1700`, and about 40 Lua uses (most of them debug).
8. **Audio state visible to Lua.** `SoundContainer:IsBeingPlayed()` depends on FMOD's real-time channel callbacks (19 uses in `Data/`).
9. **Unordered containers whose order matters:**
   - `AtomGroup` impulse accumulation over `unordered_map<MOID,…>`.
   - `SpatialPartitionGrid` returns `unordered_set<MOID>` results to Lua.
   - `PieMenu` listener map keyed by pointer.
   - Same-build stdlib makes these deterministic in practice. The `PieMenu` map is address-dependent.

**Cross-platform only:** RNG distributions (§4.1), libm (§4.2), `/fp:fast`, unstable `std::partition` on actor and particle lists (stdlib-specific order), LuaJIT on ARM64.

**Found by a second audit, after the co-op implementation (all fixed; see `CoopMultiplayer.md`):** raw keyboard queries without a player in scripts and the buy menu; unused player slots reading local gamepads; per-machine loadout files, input device and aim speed settings; a HUD value computed while drawing that scripts used to place effects; Lua garbage collection on worker threads returning objects to the memory pools in timing order; path cost updates racing background path requests; uninitialised path node, atom and actor fields; unsorted folder scans; and `pairs()` over object-keyed tables in Bunker Breach, Decision Day and the Automovers, whose order follows memory addresses (confirmed by `micro/lua_pairs_order.c`: table-keyed order changes from run to run with ASLR). The stress tests then showed that even string-keyed `pairs()` order differed between a host and a client: this LuaJIT hashes string keys by an ID handed out in the order strings are created, so the order depended on each Lua state's whole history. `micro/lua_pairs_order.c` didn't catch it because it starts from a fresh state each time. LuaJIT is now built to derive string IDs from the content (`LUAJIT_STRID_FROM_CONTENT`), and the order is part of the co-op join check.

## 6. What lockstep needs beyond determinism

1. **Inputs as commands.**
   - Per player per tick, send the post-`Controller` state: about 40 bits plus analog move, aim and cursor. That's about 20 bytes, or about 1.2 KB/s per player at 60 Hz before compression.
   - `MsgInput` in `NetworkMessages.h` is the starting point.
   - **The hard part is the UI.** Buy menu, pie menu, inventory, the brain-placement editor and object pickers are per-screen GUIs driven by cursor positions in screen pixels and by real-time timers. They need to run locally and emit discrete commands into the input stream instead: "buy this order", "activate slice X", "switch to actor N", "place brain at P". `GameActivity.cpp` alone references these GUIs about 136 times. **This is the single biggest work item.**
2. **Tick scheduler with input delay.**
   - The sim advances tick N only once all peers' inputs for N have arrived. The harness's fixed-step mode (`TimerMan::SetAccumulatorForSingleSimUpdate`) is the mechanism.
   - 3–6 ticks of delay (50–100 ms) hides typical latency. It is noticeable in a shooter, but fine for co-op. Rendering the cursor and aim reticle locally and immediately hides most of it.
   - Rollback is **not** realistic: the full state includes several Lua VMs.
3. **Desync detection.** Exchange a state hash every N ticks; the harness's hasher is a starting point. Show "desynced at tick N" and dump state for debugging. Without this, desync bugs are nearly impossible to track down.
4. **Content handshake.** Every peer must have the same build, the same mods (checksum `Data/` and `Mods/` modules) and the same match settings.
5. **Joining.**
   - Start-of-match only is easy.
   - Late join or reconnect means either replaying the input log from tick 0, or a full snapshot format. `SaveCurrentGame` is not one: it omits RNG state, Lua VM state and object IDs, and needs `OnSave` support.
6. **Keep the sim/presentation split.** Camera, HUD, split-screen layout, audio and post-effects are per-peer and must never write sim state. Fix #3 starts this.

## 7. Rough effort estimate

This is my judgment, for one developer who knows the codebase. Treat it as an order of magnitude.

| Phase | Scope | Estimate |
|---|---|---|
| 0. Determinism groundwork | Land or harden the fixes in §3. Hazards §5.1–5.9. CI job that runs the harness twice and diffs the logs. | 3–5 weeks |
| 1. Lockstep MVP | Same platform and build, 2–4 players, start-of-match join. Command stream, tick scheduler, lobby handshake, desync detection, UI-to-command conversion. | 6–10 weeks |
| 2. Content audit | Base and official `.rte` Lua: real-time timers, screen size, `pairs` over tables keyed by objects, direct input. Plus a modding guideline. | 2–4 weeks (ongoing for mods) |
| 3. Cross-platform | Own RNG distributions (days), deterministic math library in C++ and Lua `math`, `/fp:precise` and contraction flags, Windows/Linux/macOS test matrix. | 3–6 weeks |
| 4. Nice-to-haves | Replays (nearly free once lockstep works), spectators, reconnect via input-log replay. | varies |

**Recommendation:**
1. Do phase 0 first; it is valuable on its own. It fixes real data races, such as the global RNG drawn from worker threads, and makes bugs reproducible.
2. Then build a **Windows-only, same-build** lockstep MVP for co-op against AI. That gets most of the value while deferring the hardest parts: cross-platform math and versus play with tight input latency.
3. The existing frame-streaming multiplayer can stay as a fallback for mismatched builds.

## 8. Reproducing

See `Tools/Determinism/README.md`. In short:

```sh
meson setup build && ninja -C build && ln -s build/CortexCommand .
CCCP_DT_LOG=/tmp/a.log CCCP_DT_TICKS=3000 xvfb-run -a ./CortexCommand
CCCP_DT_LOG=/tmp/b.log CCCP_DT_TICKS=3000 xvfb-run -a ./CortexCommand
Tools/Determinism/compare_logs.py /tmp/a.log /tmp/b.log
```

To reproduce the baseline divergence, revert fixes #1–#7 (or check out the parent of the "prototype determinism fixes" commit) and rerun.
