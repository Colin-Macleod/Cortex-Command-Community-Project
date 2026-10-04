# Determinism tools

Tools for measuring whether the simulation is deterministic, which is a prerequisite for lockstep multiplayer.
See `Documentation/LockstepMultiplayerFeasibility.md` for the findings.

## In-game harness (`Source/System/DeterminismHarness.*`)

The harness does nothing unless `CCCP_DT_LOG` is set. When it is set, the game:

- skips the menus and launches an Activity against CPU opponents;
- forces exactly one fixed-length sim update per frame;
- writes one line per tick with hashes of the global RNG, the Lua RNGs, actors, items, particles and the terrain;
- quits after `CCCP_DT_TICKS` ticks.

Run two runs and diff them:

```sh
CCCP_DT_LOG=/tmp/a.log CCCP_DT_TICKS=3000 ./CortexCommand
CCCP_DT_LOG=/tmp/b.log CCCP_DT_TICKS=3000 ./CortexCommand
Tools/Determinism/compare_logs.py /tmp/a.log /tmp/b.log
# -> "IDENTICAL for all 3000 ticks" or "DIVERGED at tick N: rng@N, particles@M, ..."
```

Headless on Linux: `xvfb-run -a -s "-screen 0 1280x720x24" ./CortexCommand`.

The other `CCCP_DT_*` variables are documented in `DeterminismHarness.h`. They select the activity, scene and fog of war, request per-object dumps at chosen ticks, and set how often the terrain is hashed.

### Finding the call site that diverged (RNG tracing)

Build with `-DRTE_RNG_TRACE` (Linux only; it uses `backtrace`). In this build every draw from the global RNG records its call stack, and whether it came from a worker thread, to `<log>.rngtrace` during the first `CCCP_DT_TRACE_TICKS` ticks.

```sh
meson setup build-trace -Dcpp_args="-DRTE_RNG_TRACE -g -fno-omit-frame-pointer" -Db_lto=false && ninja -C build-trace
CCCP_DT_LOG=/tmp/a.log CCCP_DT_TRACE_TICKS=500 CCCP_DT_TICKS=500 ./build-trace/CortexCommand   # twice, a and b
Tools/Determinism/symbolize_trace.py build-trace/CortexCommand /tmp/a.log.rngtrace /tmp/b.log.rngtrace
```

The script prints three things:

- the first draw that differs between the two runs;
- every call site that drew from a worker thread (each one is a data race);
- call sites whose draw counts differ between the runs.

## Stress tests (`stress/`)

`stress/stress.py` runs co-op sessions on one machine: a host and one to three clients, each under its own Xvfb display, with every player driven by the `-coop-bot` input bot. It compares every peer's per-sim-update state hashes and reports, per scenario, whether they stayed identical (or, for the negative tests, whether the expected failure was caught).

```sh
Tools/Determinism/stress/stress.py --list               # what each scenario does
Tools/Determinism/stress/stress.py                      # everything (about 1.5 hours on 4 cores)
Tools/Determinism/stress/stress.py chaos memory --quick # chosen scenarios, a quarter of the sim updates
Tools/Determinism/stress/stress.py --jobs 2             # two scenarios at a time
```

Needs Linux, Xvfb and a built `./CortexCommand`. Logs, each instance's console output and `results.json` go to `--out` (default `/tmp/cccp-stress`).

| Scenario | What's different between the peers |
|---|---|
| `baseline` | Nothing (Bunker Breach). |
| `negative-control` | One peer perturbs its sim on purpose. Passes only if both the logs and the in-game detector catch it. |
| `chaos`, `chaos-4p` | Nothing, but the sim is pushed hard: the test-only *Determinism Chaos* activity (`stress/DeterminismStress.rte`, linked into `Mods/` while the suite runs) keeps a multi-faction war going with craft deliveries, bombardment, gibbing and path finding, and feeds a chaotic Lua double-precision value into spawn positions. 2 and 4 players. |
| `cpu-features` | The client's glibc picks its non-FMA math code paths, like an older CPU (`GLIBC_TUNABLES`). |
| `math-mismatch` | The client skips the start-up switch to the portable math code. Passes only if the host refuses it at join. |
| `contention` | The host runs on one CPU core at low priority, the client on the other three: very different thread timing and frame rates. |
| `frame-jitter` | Each peer sleeps a random 0-40 ms every frame (`CCCP_DT_FRAME_JITTER_MS`). |
| `memory` | The client fills allocated and freed memory with junk (`MALLOC_PERTURB_`) and runs without ASLR. |
| `environment` | The client has a different locale and time zone. |
| `bad-network` | 120-270 ms on every message, three players, and one client frozen for 5 s mid-match (it must catch up in sync). |
| `activity-sweep` | Several stock activities in turn. |
| `long` | 20000 sim updates of Determinism Chaos. |

## Micro-tests (`micro/`)

Standalone programs. Build each one with different compilers, standard libraries or flags and compare the output.

| File | Question |
|---|---|
| `rng_dist.cpp` | Do the engine's `RandomNum` wrappers (a verbatim copy of `RTE::RandomGenerator`) produce the same stream from the same seed under libstdc++, libc++ and 32-bit x87? |
| `rng_msvc_emulation.cpp` | Does the Windows (MSVC STL) build produce the same stream as the Linux (libstdc++) build? Uses a hand port of MSVC's distribution algorithms. |
| `fp_libm.c` | Are `sinf`/`cosf`/`atan2f`/`powf`/... bit-identical across libm implementations and FP flags (`-ffast-math`, FMA contraction, x87)? Also runs a small chaotic physics loop. |
| `lua_pairs_order.c` | Is LuaJIT `pairs()` order the same from one process to the next? Link it against `build/external/sources/LuaJIT-2.1/libluajit.a`. |
| `lua_jit_vs_interp.c` | Does LuaJIT float math give the same bits with the JIT on and off? |
| `libm_cpu_paths.c` | Does glibc's libm give the same bits with its FMA code paths (newer CPUs) and without? Run it as is and with `GLIBC_TUNABLES=glibc.cpu.hwcaps=-FMA,-FMA4` and compare. |
