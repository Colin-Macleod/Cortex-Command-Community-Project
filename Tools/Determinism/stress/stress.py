#!/usr/bin/env python3
"""Lockstep determinism stress tests.

Runs co-op sessions (a host and 1-3 clients, all on this machine, each under its own Xvfb display) with input bots driving every
player, under deliberately hostile conditions, and checks that every peer's per-sim-update state hashes stay identical.

Usage:
    Tools/Determinism/stress/stress.py --list
    Tools/Determinism/stress/stress.py                     # run every scenario
    Tools/Determinism/stress/stress.py chaos cpu-features  # run the named scenarios (prefix match)
    Tools/Determinism/stress/stress.py --quick ...         # a quarter of the sim updates
    Tools/Determinism/stress/stress.py --jobs 2 ...        # run scenarios in parallel (each uses 2-4 game instances)

Needs Xvfb and a built game (./CortexCommand, or --binary). Linux only. Logs go to --out (default /tmp/cccp-stress).
The DeterminismStress.rte test module is linked into Mods/ for the duration of the run.
"""
import argparse
import json
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.dirname(HERE))
sys.dont_write_bytecode = True  # Don't leave __pycache__ in the repository.
import compare_logs  # noqa: E402

STRESS_MODULE = "DeterminismStress.rte"
COMPONENTS = compare_logs.COMPONENTS

# Masks the CPU features glibc's libm picks its fast paths with, so this instance computes double-precision sin/cos/exp/log/pow/atan2
# like a CPU without FMA/AVX2 would.
NO_FMA = "glibc.cpu.hwcaps=-AVX2,-FMA,-FMA4,-AVX512F,-AVX512VL,-AVX512DQ,-AVX512BW"


def peer(env=None, args=None, wrap=None, settings=None):
    """One game instance. env: extra environment. args: extra command line. wrap: command prefix (e.g. taskset).
    settings: None to start from a copy of Userdata/Settings.ini (or from no settings file, if there's none), "fresh" to start from no settings file,
    or a dict of SettingsMan properties to start from a settings file with only those."""
    return {"env": env or {}, "args": args or [], "wrap": wrap or [], "settings": settings}


def prepare_settings(path, settings):
    """Sets up a game instance's own settings file. Instances never share one: on start-up a game writes its settings file when there's none (or
    it's incomplete), with the values it has in memory, and another instance reading it meanwhile, or afterwards, gets values that differ
    from those (e.g. DeltaTime is written rounded)."""
    if os.path.exists(path):
        os.remove(path)
    if isinstance(settings, dict):
        with open(path, "w") as f:
            f.write("SettingsMan\n" + "".join(f"\t{key} = {value}\n" for key, value in settings.items()))
    elif settings is None and os.path.exists(os.path.join(REPO, "Userdata", "Settings.ini")):
        shutil.copyfile(os.path.join(REPO, "Userdata", "Settings.ini"), path)


# Each scenario: peers[0] is the host. 'expect' is "identical" (all logs match, no desync reported) or "desync" (the detector must fire).
SCENARIOS = [
    {
        "name": "baseline",
        "doc": "Two peers, Bunker Breach, nothing special. If this fails, nothing else means anything.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 2000,
        "peers": [peer(), peer()],
    },
    {
        "name": "negative-control",
        "doc": "One peer deliberately perturbs its sim. The desync detector and the log comparison must both catch it.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 900,
        "peers": [peer(), peer(args=["-coop-inject-desync", "150"])],
        "expect": "desync",
    },
    {
        "name": "chaos",
        "doc": "Determinism Chaos: constant multi-faction war, craft deliveries, bombardment, gibbing, path finding, chaotic Lua double math.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 4000, "mod": True,
        "peers": [peer(), peer()],
    },
    {
        "name": "chaos-4p",
        "doc": "Determinism Chaos with four human players on four machines.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 2500, "mod": True,
        "peers": [peer(), peer(), peer(), peer()],
    },
    {
        "name": "cpu-features",
        "doc": "Client's libm uses the non-FMA code paths (like an older CPU). Chaos amplifies any Lua double-math difference.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 3000, "mod": True,
        "peers": [peer(), peer(env={"GLIBC_TUNABLES": NO_FMA})],
    },
    {
        "name": "math-mismatch",
        "doc": "Client skips the start-up switch to the portable math library code paths, so its libm differs from the host's. The host must refuse it.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 300, "timeout": 120,
        "peers": [peer(), peer(env={"CCCP_MATH_RESTARTED": "1"})],
        "expect": "reject",
    },
    {
        "name": "cpu-features-bb",
        "doc": "As cpu-features, in a stock Activity (Bunker Breach).",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 3000,
        "peers": [peer(), peer(env={"GLIBC_TUNABLES": NO_FMA})],
    },
    {
        "name": "contention",
        "doc": "Host squeezed onto one CPU core at low priority while the client has the rest: very different thread timing and frame rates.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 2000, "mod": True,
        "peers": [peer(wrap=["taskset", "-c", "0", "nice", "-n", "15"]), peer(wrap=["taskset", "-c", "1-3"])],
    },
    {
        "name": "frame-jitter",
        "doc": "Each peer sleeps a random 0-40 ms per frame, so draws and sim updates interleave differently everywhere.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 2000, "mod": True,
        "peers": [peer(env={"CCCP_DT_FRAME_JITTER_MS": "40", "CCCP_DT_FRAME_SEED": "1"}),
                  peer(env={"CCCP_DT_FRAME_JITTER_MS": "40", "CCCP_DT_FRAME_SEED": "2"})],
    },
    {
        "name": "memory",
        "doc": "Client fills every malloc/free with junk (MALLOC_PERTURB_) and runs without ASLR; host keeps defaults. Catches uninitialised reads and address dependence.",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 2000, "mod": True,
        "peers": [peer(env={"MALLOC_PERTURB_": "0"}), peer(env={"MALLOC_PERTURB_": "165"}, wrap=["setarch", "-R"])],
    },
    {
        "name": "environment",
        "doc": "Client runs under a different locale, time zone and working-set limits.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 1500,
        "peers": [peer(), peer(env={"LC_ALL": "C.UTF-8", "LANG": "C.UTF-8", "TZ": "Pacific/Chatham"})],
    },
    {
        "name": "settings-mismatch",
        "doc": "Host starts with no settings file (in-memory defaults); the client's own gameplay settings differ (its DeltaTime among them). Every computer must use the host's.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 1500,
        "peers": [peer(settings="fresh"),
                  peer(settings={"DeltaTime": "0.02", "AIUpdateInterval": "3", "MaxUnheldItems": "40", "CrabBombThreshold": "7", "ScrapCompactingHeight": "10",
                                 "SubPieMenuHoverOpenDelay": "500", "EnableParticleSettling": "0"})],
    },
    {
        "name": "bad-network",
        "doc": "120-270 ms per message on every peer, plus a 5 s freeze of one client mid-match (it's marked lagging, then must catch up in sync).",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 2000,
        "peers": [peer(args=["-coop-sim-latency", "120", "-coop-sim-jitter", "150"]),
                  peer(args=["-coop-sim-latency", "120", "-coop-sim-jitter", "150"]),
                  peer(args=["-coop-sim-latency", "120", "-coop-sim-jitter", "150"])],
        "freeze": {"peer": 2, "at_tick": 700, "seconds": 5},
    },
    {
        "name": "stall-recovery",
        "doc": "The client is frozen for 5 s, later the host. After each, the peers must be back in step within 15 s: after the client's freeze it must catch up (and the host say so), after the host's nobody may be left behind.",
        "activity": "Bunker Breach", "scene": "Zekarra Mining Outpost", "ticks": 2000,
        "peers": [peer(), peer()],
        "freeze": [{"peer": 1, "at_tick": 600, "seconds": 5}, {"peer": 0, "at_tick": 1300, "seconds": 5}],
        "recover_within": 15,
    },
    {
        "name": "activity-sweep",
        "doc": "Several stock Activities in turn, two peers each, different bot seeds.",
        "sweep": [("Wave Defense", "First Signs"), ("One-Man Army", ""), ("Massacre", ""), ("Survival", ""), ("Skirmish Defense", ""),
                  ("Harvester", ""), ("Keepie Uppie", ""), ("Brain vs Brain", "Highlands Bunkers")],
        "ticks": 1200,
        "peers": [peer(), peer()],
    },
    {
        "name": "long",
        "doc": "One long Chaos match (about 5.5 minutes of game time).",
        "activity": "Determinism Chaos", "scene": "Ketanot Hills", "ticks": 20000, "mod": True,
        "peers": [peer(), peer()],
    },
]


def display_in_use(number):
    return os.path.exists(f"/tmp/.X{number}-lock") or os.path.exists(f"/tmp/.X11-unix/X{number}")


class Display:
    """An Xvfb server on a display number nothing else uses (other runs of this script, or anyone's own X servers)."""

    lock = threading.Lock()
    used = set()

    def __init__(self):
        for attempt in range(20):
            with Display.lock:
                number = next(n for n in iter(lambda: random.randint(200, 900), None) if n not in Display.used and not display_in_use(n))
                Display.used.add(number)
            self.number = number
            # No TCP listener: it's not needed, and its port (6000 + display) could be taken.
            self.proc = subprocess.Popen(["Xvfb", f":{number}", "-screen", "0", "1280x720x24", "-nolisten", "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            # Ready once its socket exists. If it exits instead, another server took the number between the check and the start: try another.
            deadline = time.time() + 15
            while self.proc.poll() is None and not os.path.exists(f"/tmp/.X11-unix/X{number}") and time.time() < deadline:
                time.sleep(0.1)
            if self.proc.poll() is None and os.path.exists(f"/tmp/.X11-unix/X{number}"):
                return
            self.close()
        raise RuntimeError("couldn't start an Xvfb server")

    def close(self):
        # SIGTERM, so Xvfb removes its lock file and socket; a killed one leaves them behind.
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        with Display.lock:
            Display.used.discard(self.number)


class Port:
    """A UDP port that's free right now and not handed to another session of this run."""

    lock = threading.Lock()
    used = set()

    def __init__(self):
        with Port.lock:
            while True:
                number = random.randint(20000, 40000)
                if number in Port.used:
                    continue
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    try:
                        probe.bind(("", number))
                    except OSError:
                        continue
                Port.used.add(number)
                break
        self.number = number

    def close(self):
        with Port.lock:
            Port.used.discard(self.number)


def count_ticks(path):
    try:
        with open(path) as f:
            # Complete lines only: a game killed mid-write leaves part of one.
            return sum(1 for line in f if line.endswith("\n") and not line.startswith("#"))
    except OSError:
        return 0


def read_text(path):
    try:
        with open(path, errors="replace") as f:
            return re.sub(r"\x1b\[[0-9;]*m", "", f.read())
    except OSError:
        return ""


def match_ended(text):
    """Whether a peer's console output says its match ended because the Activity ended (e.g. the players lost)."""
    return "CO-OP: Match ended" in text and re.search(r"Activity .* was ended", text) is not None


def input_delay(host_text):
    """The input delay the host's match uses, from its console output, or None if it hasn't said yet."""
    found = re.findall(r"input delay: (\d+) sim updates", host_text)
    return int(found[-1]) if found else None


def lag_outstanding(host_text, player):
    """Whether the host said the player's input is late and hasn't said since that they caught up."""
    late = host_text.rfind(f"Input from player {player} is late")
    return late >= 0 and host_text.rfind(f"Player {player} caught up") < late


def run_session(binary, out, name, activity, scene, ticks, peers, bot_base, timeout, freeze=None):
    """Runs one co-op session. Returns a result dict.
    freeze: a freeze or a list of them, each {"peer": index, "at_tick": tick, "seconds": duration}: the peer is stopped (SIGSTOP) once its log reaches
    the tick. After each, how far apart the peers' sim updates are is sampled until they're back in step (see sample_stall)."""
    port_holder = Port()
    port = port_holder.number
    displays, procs, logs, outs = [], [], [], []
    started = time.time()
    ended_early = False
    killed_by_us = set()
    try:
        for index, p in enumerate(peers):
            display = Display()
            displays.append(display)
            log = os.path.join(out, f"{name}_p{index}.log")
            stdout = os.path.join(out, f"{name}_p{index}.out")
            for stale in (log, stdout):
                if os.path.exists(stale):
                    os.remove(stale)
            logs.append(log)
            outs.append(stdout)
            env = dict(os.environ)
            env.update({"DISPLAY": f":{display.number}", "CCCP_DT_LOG": log, "CCCP_DT_OBSERVE": "1",
                        # The host runs a little longer so every client can reach its last tick.
                        "CCCP_DT_TICKS": str(ticks + (120 if index == 0 else 0))})
            settings_path = os.path.join(out, f"{name}_p{index}.Settings.ini")
            prepare_settings(settings_path, p.get("settings"))
            env["CCCP_SETTINGSPATH"] = settings_path
            env.update(p["env"])
            command = p["wrap"] + [binary, "-cout"]
            if index == 0:
                command += ["-coop-host", str(port), "-coop-players", str(len(peers)), "-coop-activity", activity]
                if scene:
                    command += ["-coop-scene", scene]
            else:
                command += ["-coop-join", f"127.0.0.1:{port}"]
            command += ["-coop-bot", str(bot_base + index)] + p["args"]
            with open(stdout, "w") as f:
                procs.append(subprocess.Popen(command, cwd=REPO, env=env, stdout=f, stderr=subprocess.STDOUT))
            if index == 0:
                time.sleep(2)

        freezes = [] if not freeze else ([freeze] if isinstance(freeze, dict) else list(freeze))
        stalls = []  # One per freeze done: when it ended, and how far apart the peers' sim updates were afterwards.
        tracking = None
        last_progress, last_ticks = time.time(), -1
        host_exited_at = None
        while True:
            if all(proc.poll() is not None for proc in procs[1:]):
                break
            if time.time() - started > timeout:
                break
            # Watchdog: give up if no peer has made progress for a long time (stuck loading, an assertion dialog, a stalled session).
            ticks_now = sum(count_ticks(log) for log in logs)
            if ticks_now != last_ticks:
                last_progress, last_ticks = time.time(), ticks_now
            elif time.time() - last_progress > (300 if ticks_now == 0 else 180):
                break
            # The Activity ended before the tick target (e.g. Keepie Uppie when the bots lose the rocket): the peers go back to the menus and
            # never reach it. Stop once every peer says so and no log has grown for a while; judge() checks the logs match up to there.
            if ticks_now > 0 and time.time() - last_progress > 15 and all(match_ended(read_text(o)) for o in outs):
                ended_early = True
                break
            # A host that crashed or quit leaves the clients waiting for it until the watchdog fires.
            if procs[0].poll() is not None:
                host_exited_at = host_exited_at or time.time()
                if time.time() - max(host_exited_at, last_progress) > 30:
                    break
            # Rejected clients stay in the menus, so there's nothing more to wait for.
            if all("rejected the connection" in read_text(o) for o in outs[1:]):
                break
            if tracking:
                sample_stall(tracking, logs, procs, outs)
                if tracking["recovered_after"] is not None or time.time() - tracking["resumed"] > 120:
                    tracking = None
            if freezes and not tracking and count_ticks(logs[freezes[0]["peer"]]) >= freezes[0]["at_tick"]:
                f = freezes.pop(0)
                procs[f["peer"]].send_signal(signal.SIGSTOP)
                stopped_at = [count_ticks(log) for log in logs]
                time.sleep(f["seconds"])
                procs[f["peer"]].send_signal(signal.SIGCONT)
                tracking = {"peer": f["peer"], "seconds": f["seconds"], "ticks_at_stop": stopped_at, "resumed": time.time(), "samples": [], "recovered_after": None, "limit": None}
                stalls.append(tracking)
            time.sleep(0.25 if tracking else 1)
        # Give the host a moment to log its last ticks and notice the clients leaving.
        deadline = time.time() + 30
        while procs[0].poll() is None and time.time() < deadline:
            time.sleep(1)
        # The host's match can end when the last client leaves, before it reaches its own (later) target; it then waits in the menus,
        # which is fine once every client finished.
        host_done = procs[0].poll() is not None or (all(proc.poll() == 0 for proc in procs[1:]) and "CO-OP: Match ended" in read_text(outs[0]))
        timed_out = not ended_early and (not host_done or any(proc.poll() is None for proc in procs[1:]))
    finally:
        for index, proc in enumerate(procs):
            if proc.poll() is None:
                proc.send_signal(signal.SIGCONT)
                proc.kill()
                killed_by_us.add(index)
            proc.wait()
        for display in displays:
            display.close()
        port_holder.close()

    result = {"name": name, "seconds": round(time.time() - started), "exit_codes": [proc.returncode for proc in procs],
              "ticks": [count_ticks(log) for log in logs], "timed_out": timed_out, "ended_early": ended_early, "comparisons": []}
    if stalls:
        result["stalls"] = [{"peer": st["peer"], "seconds": st["seconds"], "ticks_at_stop": st["ticks_at_stop"], "recovered_after": st["recovered_after"],
                             "max_apart": max((apart for _, _, apart in st["samples"]), default=None),
                             "last_apart": st["samples"][-1][2] if st["samples"] else None, "limit": st["limit"]} for st in stalls]
        # The samples: seconds since the frozen peer was resumed, each peer's sim updates, how far apart they are.
        with open(os.path.join(out, f"{name}_stalls.json"), "w") as f:
            json.dump([{k: v for k, v in st.items() if k != "resumed"} for st in stalls], f, indent=1)
    host = compare_logs.load(logs[0]) if os.path.exists(logs[0]) else {}
    for index in range(1, len(peers)):
        client = compare_logs.load(logs[index]) if os.path.exists(logs[index]) else {}
        common = sorted(set(host) & set(client))
        first = None
        for t in common:
            a, b = host[t], client[t]
            if a["hashes"] != b["hashes"] or a["counts"] != b["counts"]:
                parts = [c for i, c in enumerate(COMPONENTS[:min(len(a["hashes"]), len(b["hashes"]))]) if a["hashes"][i] != b["hashes"][i]] or ["counts"]
                first = {"tick": t, "components": parts}
                break
        result["comparisons"].append({"peer": index, "common_ticks": len(common), "diverged": first})
    texts = [read_text(o) for o in outs]
    result["desync_reported"] = any("DESYNC" in text for text in texts)
    result["rejected"] = any("rejected the connection" in text for text in texts[1:])
    # Killed by something else, e.g. the kernel's out-of-memory killer when the machine is short of memory.
    result["killed"] = [index for index, code in enumerate(result["exit_codes"]) if code == -signal.SIGKILL and index not in killed_by_us]
    result["crashed"] = any(code not in (0, None) and code != -signal.SIGKILL for code in result["exit_codes"]) or any(
        "Stack trace" in text or "Segmentation fault" in text or "ERROR: Assertion" in text or "Abort in file" in text for text in texts)
    # Script and engine errors, minus the audio system's complaints about having no sound device.
    result["lua_errors"] = sum(1 for text in texts for line in text.splitlines() if "ERROR:" in line and "sound" not in line.lower())
    return result


def sample_stall(tracking, logs, procs, outs):
    """After a freeze, records how far apart the running peers' sim updates are, and when they're back in step: no further apart than the input
    delay (plus a little, as the logs are read one after another), with no player still marked late by the host."""
    now = time.time() - tracking["resumed"]
    counts = [count_ticks(log) for log, proc in zip(logs, procs) if proc.poll() is None]
    if len(counts) < 2:
        return
    apart = max(counts) - min(counts)
    tracking["samples"].append((round(now, 2), counts, apart))
    host_text = read_text(outs[0])
    delay = input_delay(host_text)
    if delay is None:
        return
    tracking["limit"] = delay + 5
    late = any(lag_outstanding(host_text, player) for player in range(2, len(logs) + 1))
    if tracking["recovered_after"] is None and apart <= tracking["limit"] and not late:
        tracking["recovered_after"] = round(now, 2)


def judge(scenario, result, ticks):
    """Returns (passed, reason)."""
    expect = scenario.get("expect", "identical")
    diverged = [c for c in result["comparisons"] if c["diverged"]]
    if expect == "reject":
        if result["rejected"] and not any(result["ticks"]):
            return True, "client refused at join"
        return False, f"client wasn't refused (ticks {result['ticks']})"
    if expect == "desync":
        if not diverged:
            return False, "expected a divergence, logs are identical"
        if not result["desync_reported"]:
            return False, "logs diverged but the in-game desync detector didn't report it"
        return True, f"divergence caught at tick {diverged[0]['diverged']['tick']} and reported in game"
    if result["crashed"]:
        return False, f"a peer crashed (exit codes {result['exit_codes']})"
    if result["killed"]:
        return False, f"peer {result['killed'][0]} was killed from outside (out of memory?), exit codes {result['exit_codes']}"
    if diverged:
        d = diverged[0]
        return False, f"peer {d['peer']} diverged at tick {d['diverged']['tick']} ({', '.join(d['diverged']['components'])})"
    if result["desync_reported"]:
        return False, "desync reported in game"
    if scenario.get("recover_within"):
        limit = scenario["recover_within"]
        for st in result.get("stalls", []):
            who = "the host" if st["peer"] == 0 else f"peer {st['peer']}"
            if st["recovered_after"] is None or st["recovered_after"] > limit:
                return False, (f"not back in step within {limit} s after freezing {who} for {st['seconds']} s: up to {st['max_apart']} sim updates apart, "
                               f"{st['last_apart']} when last sampled (limit {st['limit']})")
        if len(result.get("stalls", [])) < len(scenario["freeze"]):
            return False, "the session didn't get far enough for every freeze"
    if result.get("ended_early"):
        # Every peer's log has to stop at the same tick: a peer that fell behind or dropped out isn't a clean end.
        if len(set(result["ticks"])) == 1 and all(c["common_ticks"] == result["ticks"][0] for c in result["comparisons"]):
            return True, f"identical for {result['ticks'][0]} ticks on {len(result['ticks'])} peers (the activity ended before {ticks})"
        return False, f"the activity ended early, but the peers stopped at different ticks {result['ticks']}"
    short = [c for c in result["comparisons"] if c["common_ticks"] < ticks]
    if short or result["timed_out"]:
        return False, f"incomplete: compared {[c['common_ticks'] for c in result['comparisons']]} of {ticks} ticks" + (" (timed out)" if result["timed_out"] else "")
    stalls = "".join(f"; {'host' if st['peer'] == 0 else 'peer ' + str(st['peer'])} frozen {st['seconds']} s: back in step after {st['recovered_after']} s"
                     f" (up to {st['max_apart']} apart)" for st in result.get("stalls", []) if st["recovered_after"] is not None)
    return True, f"identical for {ticks} ticks on {len(result['ticks'])} peers" + stalls


def run_scenario(scenario, args):
    ticks = max(200, scenario["ticks"] // 4) if args.quick else scenario["ticks"]
    timeout = scenario.get("timeout", max(300, ticks * len(scenario["peers"]) // 4))
    sessions = scenario.get("sweep") or [(scenario["activity"], scenario.get("scene", ""))]
    outcomes = []
    for index, (activity, scene) in enumerate(sessions):
        name = scenario["name"] if len(sessions) == 1 else f"{scenario['name']}-{activity.replace(' ', '')}"
        result = run_session(args.binary, args.out, name, activity, scene, ticks, scenario["peers"], args.seed + 10 * index, timeout, scenario.get("freeze"))
        passed, reason = judge(scenario, result, ticks)
        result.update({"activity": activity, "passed": passed, "reason": reason})
        outcomes.append(result)
        print(f"  {'PASS' if passed else 'FAIL'}  {name:34s} {reason}  [{result['seconds']} s, {result['lua_errors']} errors in console]", flush=True)
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenarios", nargs="*", help="scenario names or prefixes (default: all)")
    parser.add_argument("--list", action="store_true", help="list scenarios and exit")
    parser.add_argument("--quick", action="store_true", help="run a quarter of the sim updates")
    parser.add_argument("--jobs", type=int, default=1, help="scenarios to run at once")
    parser.add_argument("--seed", type=int, default=1000, help="base bot seed")
    parser.add_argument("--binary", default=os.path.join(REPO, "CortexCommand"))
    parser.add_argument("--out", default="/tmp/cccp-stress")
    args = parser.parse_args()

    if args.list:
        for s in SCENARIOS:
            print(f"{s['name']:18s} {s['doc']}")
        return 0
    chosen = [s for s in SCENARIOS if not args.scenarios or any(s["name"].startswith(p) for p in args.scenarios)]
    if not chosen:
        print("no matching scenarios")
        return 2
    os.makedirs(args.out, exist_ok=True)
    args.binary = os.path.abspath(args.binary)

    link = os.path.join(REPO, "Mods", STRESS_MODULE)
    made_link = False
    if any(s.get("mod") for s in chosen) and not os.path.exists(link):
        os.makedirs(os.path.dirname(link), exist_ok=True)
        os.symlink(os.path.join(HERE, STRESS_MODULE), link)
        made_link = True
    try:
        results = []
        lock = threading.Lock()
        queue = list(chosen)

        def worker():
            while True:
                with lock:
                    if not queue:
                        return
                    scenario = queue.pop(0)
                outcome = run_scenario(scenario, args)
                with lock:
                    results.append((scenario["name"], outcome))

        threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        if made_link:
            os.remove(link)

    flat = [r for _, outcome in results for r in outcome]
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(flat, f, indent=1)
    failed = [r for r in flat if not r["passed"]]
    print(f"\n{len(flat) - len(failed)}/{len(flat)} passed. Logs and results.json in {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
