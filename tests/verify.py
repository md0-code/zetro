"""ZetRo's headless checks: the program itself, driven by the harness.

    python tests/verify.py                 the quick tier: every check but the ones that need the archive
    python tests/verify.py NAME...         only the named checks
    python tests/verify.py --list          their names

The check release (only by name: it needs this host's archive in dist/, which tools/build_dist.ps1 or
tools/build_linux.sh makes) extracts it into a fresh folder and starts the default machine from there, with the archive's own
ROMs and configuration and nothing of the repository.

Each check is a function in this file, named check_NAME, that runs zetro
with --hidden (no window ever opens) and looks at what comes back. A check
covers one thing a change can break; a package adds a check for what it
adds and runs only the checks it touches. Everything is written under the
git-ignored work/verify. The program is build/x64/zetro(.exe); set
ZETRO_EXE for another build.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work" / "verify"
EXE = Path(os.environ.get("ZETRO_EXE") or ROOT / "build" / "x64" / ("zetro.exe" if os.name == "nt" else "zetro"))


class Failed(Exception):
    pass


class Skipped(Exception):
    pass


def main_checkout() -> Path:
    """The main checkout's root (a worktree lives under <main>/work/wt/<name>)."""
    if ROOT.parent.name == "wt" and ROOT.parent.parent.name == "work":
        return ROOT.parent.parent.parent
    return ROOT


def need(condition: bool, message: str) -> None:
    if not condition:
        raise Failed(message)


def folder(name: str) -> Path:
    """An empty work folder for a check; zetro writes its default configuration there."""
    path = WORK / name
    path.mkdir(parents=True, exist_ok=True)
    for old in path.iterdir():
        if old.is_file():
            old.unlink()
    return path


def zetro(cwd: Path, *args: str, stdin: str | None = None, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run([str(EXE), "--hidden", *args], cwd=cwd, input=stdin, capture_output=True, text=True, timeout=timeout)


def script(cwd: Path, lines: list[str], *args: str) -> subprocess.CompletedProcess:
    """Runs a harness script; fails with the program's message when the script does."""
    (cwd / "check.script").write_text("\n".join(lines) + "\n", encoding="utf-8")
    run = zetro(cwd, "--script", "check.script", *args)
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip() or run.stdout.strip()}")
    return run


def control(cwd: Path, commands: list[str], *args: str) -> list[dict]:
    """Sends commands on the control channel; the answers, one for each. cwd is the program's working directory."""
    run = zetro(cwd, "--control", "stdin", *args, stdin="\n".join(commands) + "\n")
    answers = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    need(len(answers) == len(commands), f"{len(answers)} answers to {len(commands)} commands: {run.stderr.strip()}")
    return answers


# --- the checks ---------------------------------------------------------------------------


def check_script() -> None:
    """A script types on the test machine, saves a shot and a state, and loads the state back."""
    cwd = folder("script")
    script(cwd, ["frames 5", "type hello 42", "expect Keys: HELLO 42", "shot shot.png", "state save test.zrs",
                 "type \\bx", "expect HELLO 4X", "state load test.zrs", "frames 2", "expect Keys: HELLO 42"],
           "--model", "test", "--screenshot", "final.png")
    need((cwd / "shot.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "shot.png is not a PNG")
    need((cwd / "final.png").stat().st_size > 1000, "final.png is empty")
    need((cwd / "test.zrs").read_bytes()[:8] == b"ZETROST1", "test.zrs is not a state")
    (cwd / "bad.script").write_text("frames 1\nexpect nothing like this\n", encoding="utf-8")
    failing = zetro(cwd, "--script", "bad.script", "--model", "test")
    need(failing.returncode == 1 and "script line 2" in failing.stderr, "a failing script must end the run with its line")


def check_control() -> None:
    """The control channel answers each command with a line of JSON."""
    cwd = folder("control")
    answers = control(cwd, ["type ab", "expect Keys: AB", "expect nothing here", "text", "quit"], "--model", "test")
    need(answers[0] == {"ok": True, "frame": 8}, f"type: {answers[0]}")
    need(answers[1]["ok"] and not answers[2]["ok"], "expect")
    need("Keys: AB" in answers[3]["text"], "text")


def check_media() -> None:
    """A file goes into a slot and is refused by a slot that does not take its kind."""
    cwd = folder("media")
    (cwd / "demo.tap").write_bytes(bytes(63))
    (cwd / "demo.txt").write_bytes(b"x")
    answers = control(cwd, ["insert tape demo.tap", "expect Tape: demo.tap, 63", "insert tape demo.txt", "eject tape", "expect Tape: empty", "quit"],
                      "--model", "test")
    need(answers[0]["ok"] and answers[1]["ok"], f"insert: {answers[0]} {answers[1]}")
    need(not answers[2]["ok"] and answers[3]["ok"] and answers[4]["ok"], "a .txt is no tape; eject")


def tap_file() -> bytes:
    """A TAP file of two blocks: a header and 100 bytes of data."""
    header = bytearray([0x00, 0x03]) + b"ZETRO     " + bytes([98, 0, 0, 0, 0, 0])
    header.append(0)
    for b in header[:18]:
        header[18] ^= b
    data = bytearray([0xFF]) + bytes((i * 7) & 0xFF for i in range(98))
    data.append(0)
    for b in data[:99]:
        data[99] ^= b
    return len(header).to_bytes(2, "little") + bytes(header) + len(data).to_bytes(2, "little") + bytes(data)


def check_autoload() -> None:
    """A tape dropped on the window restarts the machine, types the model's LOAD and presses PLAY; a tape the harness
    inserts, or any tape with ui.autoload off, only goes in."""
    cwd = folder("autoload")
    (cwd / "demo.tap").write_bytes(tap_file())
    (cwd / "off.yaml").write_text("ui:\n  autoload: false\n", encoding="utf-8")
    script(cwd, ["until 20 Keys: LOA", "until 20 blocks >", "expect 2 blocks >"], "--model", "test", "--drop", "demo.tap")
    answers = control(cwd, ["insert tape demo.tap", "frames 200", "text", "quit"], "--model", "test")
    need(answers[0]["ok"] and "2 blocks" in answers[2]["text"] and "blocks >" not in answers[2]["text"], "an insert by the harness must not auto-load")
    answers = control(cwd, ["frames 200", "text", "quit"], "--config", "off.yaml", "--drop", "demo.tap")
    need("2 blocks" in answers[1]["text"] and "blocks >" not in answers[1]["text"], "ui.autoload false must not auto-load")


def jet_case() -> tuple[Path, Path, dict, list[str], float]:
    """The JET's runtime case from its core (the demo tape loaded through port FE): the main checkout, the core, the case,
    its commands as harness commands and the seconds the case runs after its last command. Skipped without the ROM or the core."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-jet"
    cases = main.parent / "MiSTerRO" / "docs" / "jet" / "runtime-cases.json"
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    if not cases.is_file() or not (core / "sim").is_dir() or not (core / "assets" / "tapes" / "demo.tap").is_file():
        raise Skipped(f"no JET core with its demo tape beside the repository ({core})")
    case = json.loads(cases.read_text(encoding="utf-8"))["cassette"]
    args = case["arguments"]
    commands = []
    for line in (core / args[args.index("--script") + 1]).read_text(encoding="utf-8").splitlines():
        word, _, rest = line.strip().partition(" ")
        if word == "type":
            # The 48K ROM takes a key again only after it was up for five frame interrupts: a doubled
            # character is typed in two goes.
            parts = [""]
            for ch in rest:
                if parts[-1].endswith(ch):
                    parts.append("")
                parts[-1] += ch
            for i, part in enumerate(parts):
                commands += (["frames 10"] if i else []) + [f"type {part}"]
        elif word == "play":
            commands.append(f"play {(core / rest).resolve()}")
        elif word:
            commands.append(line.strip())
    seconds = float(args[args.index("--seconds") + 1]) if "--seconds" in args else 0.0
    return main, core, case, commands, seconds


def check_jet() -> None:
    """The JET boots to its copyright screen, then replays its core's runtime case (the demo tape loaded
    through port FE, read back as text) and continues the same from a state saved in the middle of the
    load, in the same run and in a new one. Uses the main checkout's roms/ and the MiSTerRO checkout's tape."""
    main, core, case, commands, seconds = jet_case()
    cwd = folder("jet")
    # In real time: the tape traps (on in a new configuration) would load the demo in no time.
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\n  options:\n    tape_traps: off\n    tape_save_traps: off\n", encoding="utf-8")
    state = cwd / "mid.zrs"

    def run(commands: list[str]) -> list[dict]:
        # In the main checkout, so that its roms/ is found; every file named is in cwd.
        return control(main, commands + ["quit"], "--config", str(cwd / "zetro.yaml"))[:-1]

    def screen(answer: dict) -> None:
        need(answer.get("ok"), f"the case: {answer}")
        lines = answer["text"].splitlines()
        for pattern in case["screen"]:
            need(any(re.search(pattern, l) for l in lines), f"{pattern!r} is not on the screen: {answer['text']!r}")

    boot = run(["until 5 JET", f"shot {cwd / 'boot.png'}", "text"])
    need(boot[0]["ok"] and "JET" in boot[2]["text"], f"the copyright screen: {boot}")
    need((cwd / "boot.png").stat().st_size > 500, "boot.png")
    # The case, with a state saved while the program's data block loads (8.12 s after PLAY, in its bytes).
    waits = [i for i, c in enumerate(commands) if c.startswith("wait")]
    need(waits and commands[waits[-1] - 1].startswith("play"), f"the case does not end with play and wait: {commands}")
    tail = float(commands[waits[-1]].split()[1]) + seconds
    head = commands[:waits[-1]] + ["wait 8.12", f"state save {state}", f"wait {tail - 8.12:.2f}", "text"]
    first = run(["until 5 JET"] + head + [f"state load {state}", f"wait {tail - 8.12:.2f}", "text"])
    screen(first[-4])
    screen(first[-1])
    need(first[-1]["text"] == first[-4]["text"], "the state loaded back ends elsewhere")
    again = run([f"insert tape {(core / 'assets' / 'tapes' / 'demo.tap').resolve()}", f"state load {state}",
                 f"wait {tail - 8.12:.2f}", "text"])
    need(again[-1].get("text") == first[-1]["text"], f"the state in a new run: {again}")


def tap_blocks(data: bytes) -> list[bytes]:
    """The blocks of a TAP file."""
    blocks, at = [], 0
    while at < len(data):
        size = int.from_bytes(data[at:at + 2], "little")
        blocks.append(data[at + 2:at + 2 + size])
        at += 2 + size
    return blocks


def check_jet_traps() -> None:
    """The JET's ROM tape traps. (a) LOAD "" of the demo tape with the load trap on ends on the screen the real-time load ends
    on, a few frames after the command, and a state saved right after it loads again; (b) with the traps off, the load runs in
    real time under the turbo (the host does not wait for the tape) to the same screen; (c) SAVE with the save trap appends its
    two blocks to the writable tape in the slot, and after NEW, a rewind and LOAD "" the program lists. The tape is the JET
    core's demo; the ROM is the main checkout's."""
    main = main_checkout()
    tape = main.parent / "MiSTerRO" / "src" / "core-jet" / "assets" / "tapes" / "demo.tap"
    if not (main / "roms" / "jet.rom").is_file() or not tape.is_file():
        raise Skipped("no JET ROM or no JET core beside the repository")
    cwd = folder("jet_traps")
    for name, switch in (("on.yaml", "on"), ("off.yaml", "off")):
        (cwd / name).write_text(f"machine:\n  model: jet\n  options:\n    tape_traps: {switch}\n    tape_save_traps: {switch}\n", encoding="utf-8")
    load = ["until 5 JET", f"insert tape {tape.resolve()}", "type j\"\"", "enter"]
    state = cwd / "after.zrs"
    patterns = ["JET", "DEMO", r"^\s*42\s*$"]

    def screen(text: str, what: str) -> None:
        for pattern in patterns:
            need(any(re.search(pattern, line) for line in text.splitlines()), f"{what}: {pattern!r} is not on the screen: {text!r}")

    # (a) the load trap: the command, the program's output and the settled screen, then the state saved at once.
    answers = control(main, load + ["until 10 42", f"state save {state}", "frames 100", "text", f"state load {state}", "frames 100", "text", "quit"],
                      "--config", str(cwd / "on.yaml"))
    need(answers[4]["ok"], f"the demo did not run: {answers[4]}")
    trapped, again = answers[7]["text"], answers[10]["text"]
    seconds = (answers[4]["frame"] - answers[3]["frame"]) / 50.0
    need(seconds <= 3.0, f"the load took {seconds:.2f} s of guest time after the command")
    screen(trapped, "load trap")
    need(trapped == again, f"the state saved after a trapped load ends elsewhere: {again!r} for {trapped!r}")
    # (b) traps off, the hidden window paced to the clock: the tape plays in real time, the host runs ahead of it.
    began = time.time()
    answers = control(main, load + ["tape play", "until 60 42", "frames 100", "text", "quit"], "--config", str(cwd / "off.yaml"), "--realtime")
    host = time.time() - began
    need(answers[5]["ok"], f"the demo did not run in real time: {answers[5]}")
    screen(answers[-2]["text"], "real time")
    need(answers[-2]["text"] == trapped, f"the real-time load ends on another screen: {answers[-2]['text']!r} for {trapped!r}")
    need(host < 9.0, f"the load ran at the clock's speed ({host:.1f} s of the host for 8.3 s of tape)")
    # (c) SAVE and LOAD again through a writable tape that already holds two blocks.
    save = cwd / "save.tap"
    before = tap_file()
    save.write_bytes(before)
    answers = control(main, ["until 5 JET", f"insert tape {save.resolve()}", "type 10p1", "enter", "frames 5", "type s\"x\"", "enter",
                             "until 5 Start tape", "type z", "frames 20", "type a", "enter", "frames 200", "tape rewind",
                             "type j\"\"", "enter", "frames 40", "type k", "enter", "frames 10", "text", "quit"],
                      "--config", str(cwd / "on.yaml"))
    need(all(a["ok"] for a in answers), f"the save and load round trip: {[a for a in answers if not a['ok']]}")
    need("10 PRINT 1" in answers[-2]["text"], f"the saved program is not listed: {answers[-2]['text']!r}")
    blocks = tap_blocks(save.read_bytes())
    need(save.read_bytes().startswith(before) and len(blocks) == 4, f"the tape file has {len(blocks)} blocks, or its own are changed")
    header, body = blocks[2], blocks[3]
    need(len(header) == 19 and header[:2] == bytes([0, 0]) and header[2:12] == b"x         ", f"the header block: {header!r}")
    need(body[0] == 0xFF and len(body) == int.from_bytes(header[12:14], "little") + 2, f"the data block: {body!r}")
    for block in blocks[2:]:
        parity = 0
        for byte in block:
            parity ^= byte
        need(parity == 0, f"the parity byte of a saved block: {block!r}")


def check_jet_autoload() -> None:
    """A tape dropped on the JET restarts it, types LOAD "" and plays the tape to the program's output."""
    main = main_checkout()
    tape = main.parent / "MiSTerRO" / "src" / "core-jet" / "assets" / "tapes" / "demo.tap"
    if not (main / "roms" / "jet.rom").is_file() or not tape.is_file():
        raise Skipped("no JET ROM or no JET core beside the repository")
    cwd = folder("jet_autoload")
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\n", encoding="utf-8")
    (cwd / "check.script").write_text("until 60 DEMO\nuntil 60 42\n", encoding="utf-8")
    run = zetro(main, "--config", str(cwd / "zetro.yaml"), "--script", str(cwd / "check.script"), "--drop", str(tape))
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")


def state_chunks(path: Path) -> dict[str, list[bytes]]:
    """The chunks of a state file by tag (src/core/state.h)."""
    data = path.read_bytes()
    need(data[:8] == b"ZETROST1", f"{path.name} is not a state")
    at = 12
    for _ in range(2):
        at += 2 + struct.unpack_from("<H", data, at)[0]
    at += 4 + 32 * struct.unpack_from("<I", data, at)[0]
    chunks: dict[str, list[bytes]] = {}
    while True:
        tag, _, size = struct.unpack_from("<4sII", data, at)
        if tag == b"END ":
            return chunks
        chunks.setdefault(tag.decode("ascii"), []).append(data[at + 12:at + 12 + size])
        at += 12 + size


def jet_states_folder(name: str, slot: int = 1) -> Path:
    """A work folder with a JET configuration whose current slot is `slot`, its states/ folder emptied."""
    cwd = folder(name)
    shutil.rmtree(cwd / "states", ignore_errors=True)
    (cwd / "zetro.yaml").write_text(f"machine:\n  model: jet\n  options:\n    tape_traps: off\nvideo:\n  look: plain\nui:\n  state_slot: {slot}\n", encoding="utf-8")
    return cwd


def check_jet_slots() -> None:
    """F2 in the middle of the demo tape's load saves the current slot (states/jet/slot-1.zrs beside the configuration, with
    its media's path and hash, a thumbnail, the time and the version); the run goes on to the end of the load; F3 puts the
    state back (the load is under way again) and the load ends on the same screen; F3 alone in a fresh run (no tape
    inserted: the state puts it back) does the same."""
    main, core, case, commands, seconds = jet_case()
    cwd = jet_states_folder("jet_slots")
    config = str(cwd / "zetro.yaml")
    waits = [i for i, c in enumerate(commands) if c.startswith("wait")]
    need(waits and commands[waits[-1] - 1].startswith("play"), f"the case does not end with play and wait: {commands}")
    tail = f"wait {float(commands[waits[-1]].split()[1]) + seconds - 8.12:.2f}"
    head = ["until 5 JET"] + commands[:waits[-1]] + ["wait 8.12", tail, "text"]
    # The frames the save and the end of the load fall on (a headless run is the same every time).
    first = control(main, head + ["quit"], "--config", config)
    saved_at, ended_at, final = first[-4]["frame"], first[-3]["frame"], first[-2]
    for pattern in case["screen"]:
        need(any(re.search(pattern, l) for l in final["text"].splitlines()), f"{pattern!r} is not on the screen: {final}")
    # F2 where the first run was 8.12 s into the tape; F3 on the first frame after the load ended.
    second = control(main, head + ["frames 2", "text", tail, "text", "quit"], "--config", config,
                     "--hostkey", f"F2@{saved_at}", "--hostkey", f"F3@{ended_at + 1}")
    need(second[-6]["text"] == final["text"], f"the run after F2 ends elsewhere: {second[-6]}")
    need(second[-4]["text"] != final["text"], "F3 did not put the state of the middle of the load back")
    need(second[-2]["text"] == final["text"], f"the load after F3 ends elsewhere: {second[-2]}")
    slot = cwd / "states" / "jet" / "slot-1.zrs"
    need(slot.is_file(), f"no {slot}")
    chunks = state_chunks(slot)
    need({"MDIA", "THMB", "TIME", "VERS"} <= set(chunks), f"the slot's chunks: {sorted(chunks)}")
    width, height, colors = struct.unpack_from("<HHH", chunks["THMB"][0])
    need((width, height) == (88, 72) and 2 <= colors <= 256, f"the thumbnail is {width}x{height}, {colors} colours")
    media = chunks["MDIA"][0]
    length = struct.unpack_from("<H", media)[0]
    tape = (core / "assets" / "tapes" / "demo.tap").resolve()
    need(media[2:2 + length].decode() == str(tape) and media[2 + length] & 1, f"the slot's tape: {media[:2 + length + 1]!r}")
    need(media[3 + length:35 + length] == hashlib.sha256(tape.read_bytes()).digest(), "the slot's tape hash")
    fresh = control(main, ["frames 1", tail, "text", "quit"], "--config", config, "--hostkey", "F3@1")
    need(fresh[-2]["text"] == final["text"], f"the slot in a fresh run ends elsewhere: {fresh[-2]}")


def check_jet_rewind() -> None:
    """The harness's rewind on the JET: nothing to rewind before `rewind on`; then a line is typed, and going back past the
    typing shows the screen of before it, and the machine goes on from there; the speed commands answer."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = jet_states_folder("jet_rewind")
    answers = control(main, ["until 5 JET", "rewind 1", "rewind on", "wait 2", "text", "type 123", "wait 1", "text",
                             "rewind 2.5", "frames 1", "text", "speed full", "frames 10", "speed normal", "type 45", "wait 1",
                             "text", "quit"], "--config", str(cwd / "zetro.yaml"))
    need(not answers[1]["ok"], f"rewind before rewind on: {answers[1]}")
    before, typed, back, after = answers[4]["text"], answers[7]["text"], answers[10]["text"], answers[16]["text"]
    need("123" in typed and typed != before, f"the typing is not on the screen: {typed!r}")
    need(answers[8]["ok"] and back == before, f"rewind did not go back past the typing: {answers[8]} {back!r}")
    need(answers[11]["ok"] and answers[13]["ok"], f"speed: {answers[11]} {answers[13]}")
    need("45" in after and "123" not in after, f"the machine did not go on from the rewound state: {after!r}")
    far = control(main, ["rewind on", "wait 1", "rewind 5", "quit"], "--config", str(cwd / "zetro.yaml"))
    need(not far[2]["ok"] and "only" in far[2]["error"], f"rewind further than the ring: {far[2]}")


def check_setup_states() -> None:
    """The States page lists the running model's ten slots: one saved with F2 (the configuration's current slot, 3) shows its
    date and time and its thumbnail in the box under the list; Enter and Load put it back and leave the screen."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = jet_states_folder("setup_states", 3)
    config = str(cwd / "zetro.yaml")
    run = zetro(main, "--config", config, "--frames", "260", "--hostkey", "F2@250")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    need((cwd / "states" / "jet" / "slot-3.zrs").is_file(), "F2 did not save slot 3")
    shot = cwd / "states.png"
    answers = control(main, ["frames 3", f"shot {shot}", "text", "frames 6", "text", "quit"], "--config", config,
                      "--setup-page", "states", *keys("Return", "Return", first=4))
    page = answers[2]["text"]
    need(re.search(r"Slot 3 +\d{4}-\d\d-\d\d \d\d:\d\d", page), f"slot 3 is not listed with its time: {page!r}")
    need(re.search(r"Slot 1 +\(empty\)", page) and re.search(r"Slot 10 +\(empty\)", page), f"the empty slots: {page!r}")
    rows = png_rows(shot)
    box = [px for row in rows[24 + 96:24 + 168] for px in row[32 + 80:32 + 176]]
    need(sum(px == (205, 205, 205) for px in box) > 2000, "no thumbnail of the JET's screen in the box")
    need(any(BRIGHT_CYAN in row for row in rows[24 + 16:24 + 32]), "the current slot is not the selected row")
    need("JET" in answers[4]["text"] and "Slot" not in answers[4]["text"], f"Load did not put the slot back: {answers[4]}")
# --- the setup screen ----------------------------------------------------------------------

CONFIG = """\
# a comment of the file's own
machine:
  model: test                 # the computer
  options:
    beep: on                  # a switch of the test machine

media:
  tape:
    image: ""                 # a tape file
    write_protect: false
    fast: true

video:
  look: plain                 # the screenshots are the frame itself
  aspect: original
  scale: fit
  filter: sharp
  fullscreen: false
  vsync: true

sound:
  enabled: true
  volume: 100                 # 0 to 100

ui:
  language: en
"""

# the Spectrum's colours as the setup screen draws them
BLUE, BRIGHT_BLUE, RED, YELLOW, BRIGHT_CYAN = (0, 0, 205), (0, 0, 255), (255, 0, 0), (255, 255, 0), (0, 255, 255)


def png_rows(path: Path) -> list[list[tuple[int, int, int]]]:
    """The pixels of a PNG written by zetro (8-bit RGB), by rows."""
    data = path.read_bytes()
    need(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path.name} is not a PNG")
    width, height = struct.unpack(">II", data[16:24])
    chunks, at = [], 8
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind == b"IDAT":
            chunks.append(data[at + 8:at + 8 + size])
        at += 12 + size
    raw = zlib.decompress(b"".join(chunks))
    stride, rows, previous = width * 3, [], bytearray(width * 3)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - 3] if x >= 3 else 0
            up = previous[x]
            upleft = previous[x - 3] if x >= 3 else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else upleft)) & 255
        rows.append([(line[i], line[i + 1], line[i + 2]) for i in range(0, stride, 3)])
        previous = line
    return rows


def setup_folder(name: str) -> Path:
    cwd = folder(name)
    (cwd / "zetro.yaml").write_text(CONFIG, encoding="utf-8")
    return cwd


def keys(*pressed: str, first: int = 2) -> list[str]:
    """--hostkey arguments: one key a frame, from frame `first` on."""
    return [a for n, key in enumerate(pressed) for a in ("--hostkey", f"{key}@{n + first}")]


def tap_file_at(path: Path) -> None:
    """A TAP of four blocks: two headers and their data."""
    def block(flag: int, data: bytes) -> bytes:
        body = bytes([flag]) + data
        parity = 0
        for b in body:
            parity ^= b
        return struct.pack("<H", len(body) + 1) + body + bytes([parity])
    header = bytes([0]) + b"DEMO      " + struct.pack("<HHH", 20, 10, 20)
    screen = bytes([3]) + b"SCREEN    " + struct.pack("<HHH", 6912, 16384, 32768)
    path.write_bytes(block(0, header) + block(255, bytes(20)) + block(0, screen) + block(255, bytes(60)))


def check_setup_pages() -> None:
    """Every page of the setup screen is drawn as a Spectrum screen: 320 x 240, a blue border, the title bar's stripes, the selection bar."""
    cwd = setup_folder("setup_pages")
    seen = set()
    # The test machine has no byte ports and takes no .pok files: its screen has seven pages, without Ports and Cheats.
    for page in ("machine", "media", "display", "sound", "controllers", "states", "about"):
        shot = cwd / f"{page}.png"
        run = script(cwd, ["frames 3", "text"], "--setup-page", page, "--screenshot", shot.name)
        rows = png_rows(shot)
        need((len(rows[0]), len(rows)) == (320, 240), f"{page}: {len(rows[0])}x{len(rows)}, not 320x240")
        need(rows[0][0] == BLUE and rows[239][319] == BLUE, f"{page}: no blue border")
        need(re.search(r"^ ZetRo +\S+ +\d/7$", run.stdout, re.M), f"{page}: the title bar does not show the name ZetRo and the page: {run.stdout!r}")
        # the bar's stripes are the Romanian flag's: bright blue, yellow and red, in that order from the left (read along the bar's bottom row)
        bar = rows[24 + 7][32:288]
        need(all(c in bar for c in (BRIGHT_BLUE, YELLOW, RED)), f"{page}: the title bar has no blue, yellow and red stripes")
        need(bar.index(BRIGHT_BLUE) < bar.index(YELLOW) < bar.index(RED), f"{page}: the stripes are not blue, yellow, red from the left")
        need(sum(px == (0, 255, 0) for row in rows[24:32] for px in row[32:288]) == 0, f"{page}: a green stripe is left in the title bar")
        need(any(BRIGHT_CYAN in row for row in rows[32:216]), f"{page}: no selection bar")
        seen.add(hashlib.sha256(shot.read_bytes()).hexdigest())
    need(len(seen) == 7, "the pages are not seven different pictures")
    # The About page: the version, what is new in it, and the logo's panel (white, with the emblem's yellow) under the title bar.
    need(f"Version {(ROOT / 'src' / 'VERSION').read_text(encoding='ascii').strip()}" in run.stdout and "New in this version" in run.stdout, f"the About page: {run.stdout!r}")
    panel = [px for row in png_rows(cwd / "about.png")[24 + 16:24 + 80] for px in row[32:288]]
    need((255, 255, 255) in panel and (0, 0, 0) in panel and any(r > 230 and 180 < g < 230 and b < 60 for r, g, b in panel),
         "the About page has no logo (white ground, black letters, the emblem's yellow)")
    # With no page named the screen opens on Machine, the first.
    run = script(setup_folder("setup_first"), ["frames 3", "text"], *keys("F12"))
    need(re.search(r"^ ZetRo +Machine +1/7$", run.stdout, re.M), f"the screen does not open on the Machine page: {run.stdout!r}")
    # A page the machine has nothing for is not opened: the next one is.
    for page in ("ports", "cheats"):
        run = script(cwd, ["frames 3", "text"], "--setup-page", page)
        need(not re.search(r"^ ZetRo +(Ports|Cheats) ", run.stdout, re.M), f"{page}: the test machine has the page: {run.stdout!r}")


def check_setup_change() -> None:
    """A setting changed on the setup screen is written to zetro.yaml at once, the file's comments kept."""
    cwd = setup_folder("setup_change")
    # Sound: Down to Volume, Page Up turns it down a step. Display: Down x3 to Filter, Page Down, Down, Enter flips Full screen... on a
    # separate run so each reads back alone.
    run = zetro(cwd, "--setup-page", "sound", *keys("Down", "PageUp"), "--frames", "5")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("volume: 90" in text and "enabled: true" in text, f"the volume was not written:\n{text}")
    # Machine: Down to the switch, Enter flips it; Display: Language, through its menu, to Romanian.
    run = zetro(cwd, "--setup-page", "machine", *keys("Down", "Return"), "--frames", "5")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    run = zetro(cwd, "--setup-page", "display", *keys("End", "Return", "Down", "Return"), "--frames", "7")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("beep: off" in text or 'beep: "off"' in text, f"the option was not written:\n{text}")
    need("language: ro" in text, f"the language was not written:\n{text}")
    need("# a comment of the file's own" in text and "# a switch of the test machine" in text and "# 0 to 100" in text, f"a comment was lost:\n{text}")
    # The new settings are the ones the next run reads.
    run = zetro(cwd, "--setup-page", "sound", "--frames", "2", "--screenshot", "ro.png")
    need(run.returncode == 0, f"the file no longer loads: {run.stderr.strip()}")


def check_setup_tape() -> None:
    """A tape goes in through the file picker (opened in media/ beside the configuration), is browsed block by block and played."""
    cwd = setup_folder("setup_tape")
    (cwd / "media").mkdir(exist_ok=True)
    tap_file_at(cwd / "media" / "demo.tap")
    (cwd / "media" / "note.txt").write_text("not a tape", encoding="utf-8")
    # Enter opens the picker (media/ lists demo.tap alone for this slot), Enter inserts it; Down x5 to the tape blocks row (Eject,
    # Write protect, Fast loading, Auto-load, Tape blocks), Enter opens the browser, Down and Enter seek to the second block, p plays,
    # Escape and Escape leave the browser and the screen.
    pressed = ["Return", "Return", "Down", "Down", "Down", "Down", "Down", "Return", "Down", "Return", "p", "Escape", "Escape"]
    script(cwd, ["frames 20", "expect Tape: demo.tap, 130 bytes", "expect 4 blocks >", "shot browsed.png", "frames 3", "expect 4 blocks >"],
           "--setup-page", "media", *keys(*pressed))
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("media/demo.tap" in text.replace("\\", "/"), f"the tape was not written:\n{text}")
    # The picture after the screen was left is the machine's again (the test machine's white border), not the setup screen's.
    need(png_rows(cwd / "browsed.png")[0][0] == (205, 205, 205), "the setup screen is still showing")
    # The pop-ups as they look: the picker, and the browser with the tape at its second block, running.
    script(cwd, ["frames 5", "shot picker.png"], "--setup-page", "media", *keys("Return"))
    script(cwd, ["frames 16", "shot browser.png"], "--setup-page", "media", *keys(*pressed[:11]))
    picker, browser = png_rows(cwd / "picker.png"), png_rows(cwd / "browser.png")
    need(picker[0][0] == BLUE and browser[0][0] == BLUE, "the pop-ups are not on the setup screen")
    need(picker != browser, "the picker and the tape browser are the same picture")


def check_tape_key() -> None:
    """F1 is the tape's file picker alone: Enter takes the file it lists, the screen is left with the tape in the recorder, and
    the page the setup screen opens on stays the one it was."""
    cwd = setup_folder("tape_key")
    (cwd / "media").mkdir(exist_ok=True)
    tap_file_at(cwd / "media" / "demo.tap")
    script(cwd, ["frames 12", "expect Tape: demo.tap, 130 bytes", "shot after.png"], *keys("F1", "Return"))
    need(png_rows(cwd / "after.png")[0][0] == (205, 205, 205), "the setup screen is still showing")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("media/demo.tap" in text.replace("\\", "/") and "setup_page" not in text, f"zetro.yaml after F1:\n{text}")


def check_setup_controllers() -> None:
    """The Controllers page: a socket's device and the keys of the keyboard joystick (one taken, one refused) are changed on the
    screen and read back from zetro.yaml; the Media page: the tape's Auto-load."""
    cwd = setup_folder("setup_controllers")
    # The device menu opens at Keyboard: Down to Controller 1, Enter. The Media page: Down x3 to the tape's Auto-load (Write protect,
    # Fast loading, Auto-load: the test screen's tape has no sound, and Eject is dim on an empty slot), Enter flips it.
    run = zetro(cwd, "--setup-page", "controllers", *keys("Return", "Down", "Return"), "--frames", "6")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    run = zetro(cwd, "--setup-page", "media", *keys("Down", "Down", "Down", "Return"), "--frames", "7")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("device: controller 1" in text, f"the device was not written:\n{text}")
    need("autoload: false" in text, f"auto-load was not written:\n{text}")
    need("# a comment of the file's own" in text, f"a comment was lost:\n{text}")
    # The keyboard joystick's keys, on a keyboard socket: Down to the Up row, Enter and the key Q; Down to the Down row, Enter and Q
    # again is refused (a note), Enter closes the note, Down to Left, Enter, F5 is the window's, refused too.
    cwd = setup_folder("setup_controllers_keys")
    shot = cwd / "refused.png"
    run = script(cwd, ["frames 14", f"shot {shot.name}"], "--setup-page", "controllers",
                 *keys("Down", "Return", "Q", "Down", "Return", "Q", "Return", "Down", "Return", "F5", "Return"))
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("up: Q" in text, f"the key was not written:\n{text}")
    need("down: Q" not in text and "left: F5" not in text, f"a refused key was written:\n{text}")
    # The note that refused the last key is on the screen only until its Enter; the page is showing again.
    rows = png_rows(shot)
    need(any(BRIGHT_CYAN in row for row in rows[32:216]), "no selection bar after the refusals")
    # The file still loads, and the keys it names are the machine's joystick (one run of the program reads it).
    run = zetro(cwd, "--frames", "2")
    need(run.returncode == 0, f"the file no longer loads: {run.stderr.strip()}")


def jet_cwd(name: str) -> tuple[Path, Path]:
    """A work folder with a configuration for the JET, and the main checkout (its roms/ is found from there)."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = folder(name)
    (cwd / "zetro.yaml").write_text(
        "# a comment of the file's own\nmachine:\n  model: jet           # the computer\n  options:\n    tape_sound: on     # the tape is heard\n"
        "    joystick: kempston\nvideo:\n  look: plain\n", encoding="utf-8")
    return cwd, main


def check_setup_model() -> None:
    """The model picked on the Machine page builds the machine again when the screen is left: from the JET to the test machine and
    back, then an option of the JET (it holds no live call, so it too is a machine built again); zetro.yaml follows each step."""
    cwd, main = jet_cwd("setup_model")
    config = str(cwd / "zetro.yaml")
    # The JET, the Machine page: Enter opens the model list, Home to the test machine, Enter picks it; F12 asks to restart, Enter agrees.
    # The screen opens again (F12) on Machine: the list, Down x11 to the JET (after the six HCs and the four CIPs), F12 and Enter.
    there = keys("Return", "Home", "Return", "F12", "Return")
    back = keys("F12", "Return", *["Down"] * 11, "Return", "F12", "Return", first=14)
    # The screen is read only after it is left (while it is open, `until` reads the setup screen, which names the JET too).
    answers = control(main, ["frames 10", "expect test screen", "frames 25", "until 20 JET", "quit"], "--config", config,
                      "--setup-page", "machine", *there, *back)
    need(answers[1]["ok"], f"the machine was not built again as the test machine: {answers[1]}")
    need(answers[3]["ok"], f"the machine was not built again as the JET: {answers[3]}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("model: jet" in text and "# a comment of the file's own" in text, f"zetro.yaml after the round trip:\n{text}")
    # An option of the JET: its joystick, Down to the row (the one under Model), Page Down to None; the RAM poked while the screen was open is gone.
    answers = control(main, ["poke 8000 AB", "peek 8000", "frames 10", "peek 8000", "quit"], "--config", config, "--setup-page", "machine",
                      *keys("Down", "PageDown", "F12", "Return", first=3))
    need(answers[1]["data"] == "AB" and answers[3]["data"] == "00", f"the machine was not built again for its option: {answers}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("joystick: none" in text or 'joystick: "none"' in text, f"the option was not written:\n{text}")

def check_setup_model_clock() -> None:
    """A model with another master clock picked on the Machine page runs at its own speed: from the HC-91 (14 MHz) to the TIM-S
    (168 MHz) in real time, 150 pictures take 3 s, not twelve times that."""
    main = main_checkout()
    if not (main / "roms" / "tims-standard.rom").is_file() or not (main / "roms" / "hc91.rom").is_file():
        raise Skipped("no HC-91 and TIM-S ROMs (python tools/fetch_roms.py)")
    cwd = folder("setup_model_clock")
    config = cwd / "zetro.yaml"
    config.write_text("machine:\n  model: hc91\nvideo:\n  look: plain\n", encoding="utf-8")
    started = time.monotonic()
    answers = control(main, ["frames 150", "quit"], "--config", str(config), "--realtime", "--setup-page", "machine",
                      *keys("Return", *["Down"] * 9, "Return", "F12", "Return"))
    took = time.monotonic() - started
    need(answers[0]["ok"] and "model: tims" in config.read_text(encoding="utf-8"), f"the TIM-S was not picked: {answers}")
    need(took < 10.0, f"150 pictures of the TIM-S after the HC-91 took {took:.1f} s")


def check_power_cycle_time() -> None:
    """Ctrl+F12 in real time: the machine goes on at once (its time is not set back, which left it standing for as long as it had
    run): six seconds on, the power cycle, and four seconds later the JET takes what is typed."""
    cwd, main = jet_cwd("power_cycle_time")
    answers = control(main, ["frames 500", "type 10", "frames 60", "text", "quit"], "--config", str(cwd / "zetro.yaml"),
                      "--realtime", *keys("ctrl+F12", first=300))
    need("10" in answers[3]["text"], f"the machine stood still after the power cycle: {answers[3]}")


def check_reset_key() -> None:
    """Alt+F12 is the reset button: what was typed is gone and the machine takes keys again."""
    cwd, main = jet_cwd("reset_key")
    answers = control(main, ["frames 300", "type 1234", "frames 60", "text", "frames 300", "type 56", "frames 60", "text", "quit"],
                      "--config", str(cwd / "zetro.yaml"), *keys("alt+F12", first=400))
    need("1234" in answers[3]["text"], f"nothing typed before the reset: {answers[3]}")
    need("1234" not in answers[7]["text"] and "56" in answers[7]["text"], f"Alt+F12 did not reset the machine: {answers[7]}")


def check_tape_settings() -> None:
    """A tape's settings are the Media page's: a file of an earlier version (machine.options.tape_*, ui.autoload) is read, and once
    the page changes a setting (Tape sound, off) the file has them all under media.tape and no longer where they were. With Fast
    loading on the ROM's loader gets the demo's blocks at once; with it off it waits for the tape."""
    cwd, main = jet_cwd("tape_settings")
    config = cwd / "zetro.yaml"
    config.write_text("machine:\n  model: jet\n  options:\n    tape_traps: off\n    joystick: none\nui:\n  autoload: false\n", encoding="utf-8")
    # Media: Down x3 to Tape sound (Write protect, Fast loading, Tape sound), Enter flips it.
    run = zetro(main, "--config", str(config), "--setup-page", "media", *keys("Down", "Down", "Down", "Return"), "--frames", "7")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    text = config.read_text(encoding="utf-8")
    need("sound: false" in text and "traps: false" in text and "autoload: false" in text and "joystick: none" in text,
         f"the tape's settings were not written under media.tape:\n{text}")
    need("tape_traps" not in text and "ui:\n  autoload" not in text, f"the earlier keys are still in the file:\n{text}")
    main, core, case, _commands, _seconds = jet_case()
    tape = (core / "assets" / "tapes" / "demo.tap").resolve()
    for fast, loaded in (("true", True), ("false", False)):
        config.write_text(f"machine:\n  model: jet\nmedia:\n  tape:\n    fast: {fast}\n", encoding="utf-8")
        # The tape is in the recorder, not playing: with Fast loading the ROM's loader is given its blocks at once, without it it waits.
        answers = control(main, ["until 5 JET", f"insert tape {tape}", 'type j""', "enter", "wait 5", "text", "quit"], "--config", str(config))
        lines = answers[-2]["text"].splitlines()
        shown = all(any(re.search(pattern, l) for l in lines) for pattern in case["screen"])
        need(shown == loaded, f"fast {fast}: the screen five seconds after LOAD: {answers[-2]['text']!r}")


def check_jet_rewind_key() -> None:
    """F4 held goes back through the ring: what was typed is gone again when the key is released."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped("no JET ROM")
    cwd = folder("jet_rewind_key")
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\n", encoding="utf-8")
    (cwd / "check.script").write_text("until 5 JET\nrewind on\nframes 100\ntype 1234\nframes 100\nexpect 1234\nframes 60\ntext\n", encoding="utf-8")
    run = zetro(main, "--config", str(cwd / "zetro.yaml"), "--script", str(cwd / "check.script"), "--hostkey", "+F4@310", "--hostkey", "-F4@330")
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    need("JET" in run.stdout and "1234" not in run.stdout.split("Presenter", 1)[-1], f"the typing is still there: {run.stdout!r}")


# --- Spectrum timing (the JET's option "timing: spectrum": a Sinclair 48K's ULA) ---------------
#
# Each check pokes a Z80 routine of its own at 9000h and starts it with RANDOMIZE USR 36864. The routine sets IM 2 with
# I = 80h (the JET's acknowledge byte is FFh: the vector is at 80FFh), so nothing of it is contended but what the check
# puts below 8000h; the handler starts 19 T-states (the IM 2 response) after the interrupt is taken, which is j = 0-3
# T-states after the interrupt's start (the HALT's fetches are 4 T-states long, and the phase is not known): every
# expected value is worked out for the four and the measured one must be one of them. The expected numbers come from the
# documents, not from the emulator: 69888 T-states a frame, 224 a line, contention
# 6,5,4,3,2,1,0,0 from 14335 for 128 T-states of each of the 192 lines, the ULA's fetches 14338 (bitmap), 14339
# (attribute), 14340, 14341 (the next cell's) then nothing for four, the data sampled in the last T-state of an I/O
# cycle, an OUT to the border that ends 14339-14342 T-states after the interrupt changes it at the picture's first pixel.

SINCLAIR_FRAME, SINCLAIR_LINE, SINCLAIR_CONTENDED = 69888, 224, 14335
SINCLAIR_PATTERN = (6, 5, 4, 3, 2, 1, 0, 0)
USR = 0x9000


def sinclair_delay(t: int) -> int:
    """The 48K's delay for a contended access starting at T-state t of the frame."""
    t %= SINCLAIR_FRAME
    line, x = divmod(t - SINCLAIR_CONTENDED, SINCLAIR_LINE)
    return SINCLAIR_PATTERN[x % 8] if t >= SINCLAIR_CONTENDED and line < 192 and x < 128 else 0


def sinclair_bus(t: int, screen: bytes) -> int:
    """The byte the 48K's ULA has on its bus at T-state t of the frame (the floating bus), from the screen's 6912 bytes."""
    line, x = divmod(t - (SINCLAIR_CONTENDED + 3), SINCLAIR_LINE)
    if t < SINCLAIR_CONTENDED + 3 or line >= 192 or x >= 128 or x % 8 >= 4:
        return 0xFF
    cell = (x // 8) * 2 + (x % 8) // 2
    if x % 2:
        return screen[0x1800 + (line // 8) * 32 + cell]
    return screen[((line & 0xC0) << 5) | ((line & 7) << 8) | ((line & 0x38) << 2) | cell]


def usr_start(handler: int) -> bytes:
    """DI, SP kept at 81F0h, IM 2 through the vector at 80FFh to `handler`, EI, HALT (19 bytes: the handler can follow)."""
    return bytes([0xF3, 0xED, 0x73, 0xF0, 0x81, 0x3E, 0x80, 0xED, 0x47, 0xED, 0x5E, 0x21, handler & 0xFF, handler >> 8,
                  0x22, 0xFF, 0x80, 0xFB, 0x76])


# Back to BASIC: SP from 81F0h, I = 3Fh, IM 1, EI, RET.
USR_END = bytes([0xED, 0x7B, 0xF0, 0x81, 0x3E, 0x3F, 0xED, 0x47, 0xED, 0x56, 0xFB, 0xC9])


def djnz_delay(counts: list[int]) -> tuple[bytes, int]:
    """LD B,n then DJNZ to itself for each count (0: 256): the code and its T-states (7 + 13 n - 5 each)."""
    code, tstates = b"", 0
    for n in counts:
        code += bytes([0x06, n & 0xFF, 0x10, 0xFE])
        tstates += 7 + 13 * (n or 256) - 5
    return code, tstates


def jet_usr(name: str, timing: str, program: dict[int, bytes], after: list[str], before: list[str] | None = None) -> list[dict]:
    """The JET with the timing given, `before` run, the program poked, RANDOMIZE USR 36864 typed (USR in extended mode:
    CAPS SHIFT with SYMBOL SHIFT, then L), then `after`: the answers to `after`."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = WORK / name
    cwd.mkdir(parents=True, exist_ok=True)
    config = cwd / f"{timing}.yaml"
    config.write_text(f"machine:\n  model: jet\n  options:\n    timing: {timing}\nvideo:\n  look: plain\n", encoding="utf-8")
    pokes = [f"poke {address + i:04X} " + " ".join(f"{b:02X}" for b in code[i:i + 256])
             for address, code in program.items() for i in range(0, len(code), 256)]
    usr = ["until 5 JET", "frames 50", *(before or []), *pokes, "type t", "key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3",
           "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3", "type l36864", "enter"]
    answers = control(main, usr + after + ["quit"], "--config", str(config))
    need(all(a["ok"] for a in answers[:-1]), f"{name}, {timing}: {[a for a in answers if not a['ok']]}")
    return answers[len(usr):-1]


def check_jet_contention() -> None:
    """(a) Contended memory: a loop (INC HL; JR back) counts its turns from one frame interrupt to the next, once at 6400h
    and once at A400h. Timing original: the two counts are equal and are what 69888 T-states give; Spectrum timing: at
    A400h the same, at 6400h fewer, as many as the documented pattern gives when every cycle of the loop is delayed by it."""
    def program(loop: int) -> dict[int, bytes]:
        h1 = USR + 19
        h2 = h1 + 13
        handler = bytes([0x21, h2 & 0xFF, h2 >> 8, 0x22, 0xFF, 0x80, 0x21, 0, 0, 0xFB, 0xC3, loop & 0xFF, loop >> 8])
        return {USR: usr_start(h1) + handler + bytes([0x22, 0xF2, 0x81]) + USR_END, loop: bytes([0x23, 0x18, 0xFD])}

    def expected(loop: int, contended: bool) -> set[int]:
        """The turns for each j: the loop starts 69 T-states after the interrupt was taken (19 for the response, then
        LD HL 10, LD (nn),HL 16, LD HL 10, EI 4, JP 10); INC HL is pc:4 ir:1 ir:1, JR pc:4 pc+1:3 pc+1:1 x5 (I = 80h
        is not contended); the interrupt is taken after the first instruction that ends at or past 69888."""
        counts = set()
        for j in range(4):
            t, turns = j + 69, 0

            def cycle(t: int, address: int, length: int) -> int:
                return t + (sinclair_delay(t) if contended and 0x4000 <= address < 0x8000 else 0) + length
            while True:
                for address, length in ((loop, 4), (0x8000, 1), (0x8000, 1)):
                    t = cycle(t, address, length)
                turns += 1
                if t >= SINCLAIR_FRAME:
                    break
                for address, length in ((loop + 1, 4), (loop + 2, 3), *((loop + 2, 1),) * 5):
                    t = cycle(t, address, length)
                if t >= SINCLAIR_FRAME:
                    break
            counts.add(turns)
        return counts

    measured = {}
    for timing in ("original", "spectrum"):
        for loop in (0x6400, 0xA400):
            answers = jet_usr("jet_contention", timing, program(loop), ["frames 10", "peek 81F2 2"])
            low, high = (int(b, 16) for b in answers[-1]["data"].split())
            measured[timing, loop] = low | high << 8
    plain = expected(0x6400, False)
    need(len(plain) == 1 and expected(0xA400, False) == plain, f"the routine's count depends on the HALT's phase: {plain}")
    slow = expected(0x6400, True)
    for key in (("original", 0x6400), ("original", 0xA400), ("spectrum", 0xA400)):
        need(measured[key] in plain, f"{key[0]} timing, loop at {key[1]:04X}h: {measured[key]} turns, the documents give {plain}")
    need(measured["spectrum", 0x6400] in slow,
         f"Spectrum timing, loop at 6400h: {measured['spectrum', 0x6400]} turns, the documents give {slow} ({plain} uncontended)")


def check_jet_floating_bus() -> None:
    """(b) The floating bus: the screen filled with known bytes (bitmap bytes below 80h, attributes above, none FFh), a
    routine reads port 00FFh (which nothing on the JET answers) 64 times, one IN A,(C) every 23 T-states, from about 14321
    T-states after the interrupt (over the fetches of the picture's first lines). Spectrum timing: the bytes read are the
    ones the documented fetch times give, bitmap and attribute bytes among them; original timing: FFh only."""
    screen = bytes(1 + (o * 37) % 126 for o in range(6144)) + bytes(0x80 | (1 + (a * 11) % 126) for a in range(768))
    delay, tstates = djnz_delay([0, 0, 0, 0, 73])
    reads = bytes([0xED, 0x78, 0x77, 0x2C]) * 64                    # IN A,(C) 12 (sampled at its 12th T-state), LD (HL),A 7, INC L 4
    handler = delay + bytes([0x21, 0x00, 0xA0, 0x01, 0xFF, 0x00]) + reads + USR_END     # LD HL,A000h 10; LD BC,00FFh 10
    program = {0x4000: screen, USR: usr_start(USR + 19) + handler}
    first = 19 + tstates + 20 + 11
    candidates = {j: bytes(sinclair_bus(j + first + 23 * k, screen) for k in range(64)) for j in range(4)}
    for timing in ("original", "spectrum"):
        answers = jet_usr("jet_floating_bus", timing, program, ["frames 5", "peek A000 40", "peek 4000 20"])
        seen = bytes(int(b, 16) for b in answers[-2]["data"].split())
        need(bytes(int(b, 16) for b in answers[-1]["data"].split()) == screen[:32], "the screen's first bytes were changed")
        if timing == "original":
            need(seen == b"\xFF" * 64, f"original timing: an unattached port read {seen.hex(' ')}")
            continue
        need(any(b < 0x80 for b in seen) and any(0x80 <= b < 0xFF for b in seen), f"no bitmap and attribute bytes: {seen.hex(' ')}")
        match = [j for j, bytes_ in candidates.items() if bytes_ == seen]
        need(match, f"Spectrum timing: read {seen.hex(' ')}; the documents give, for j = 0: {candidates[0].hex(' ')}")


def raster_position(e: int) -> tuple[int, int]:
    """Where the 48K's documents put a border change by an OUT ending e T-states after the interrupt, in the frame
    (352 x 288, the picture's first pixel at (48, 48)): (x, y)."""
    y, x = divmod(48 + 8 * ((e - 14339) // 4), 448)
    return x, y + 48


def jet_raster() -> tuple[dict[int, bytes], int]:
    """The raster routine and the T-state its second OUT ends at for j = 0: every frame the IM 2 handler sets the border
    blue (OUT (FEh) ending 37 + j T-states after the interrupt, in lines nobody sees), waits, sets it red with an OUT
    that ends e = e0 + j, counts the frame in 81F4h and returns to HALT; e0 is chosen with e0 mod 4 = 3, so that the four
    phases j end in the same 4 T-state window of the documents, and so that the change is in the border."""
    for n in range(170, 250):
        delay, tstates = djnz_delay([0, 0, n])
        e0 = 19 + 7 + 11 + tstates + 7 + 11
        if e0 % 4 == 3 and 16 <= raster_position(e0)[0] < 336:
            break
    handler = 0x9011
    start = bytes([0xF3, 0x3E, 0x80, 0xED, 0x47, 0xED, 0x5E, 0x21, handler & 0xFF, handler >> 8, 0x22, 0xFF, 0x80, 0xFB,
                   0x76, 0x18, 0xFD])                                                       # ...EI; HALT; JR back to HALT
    body = bytes([0x3E, 0x01, 0xD3, 0xFE]) + delay + bytes([0x3E, 0x02, 0xD3, 0xFE, 0x2A, 0xF4, 0x81, 0x23, 0x22, 0xF4, 0x81,
                                                         0xFB, 0xC9])
    return {USR: start + body, 0x81F4: bytes(2)}, e0


def check_jet_raster() -> None:
    """(c) A raster effect: the border turns from blue to red at a fixed T-state of every frame (jet_raster). With Spectrum
    timing the change is where the 48K's documents put it: an OUT ending in [14339 + 4m, 14342 + 4m] changes the border
    8m pixels (along the raster, 448 pixels a line) after the picture's first pixel, which the frame shows at (48, 48);
    the PNG's row has blue before that pixel and red from it, the row above all blue and the row below all red."""
    program, e0 = jet_raster()
    cwd = WORK / "jet_raster"
    answers = jet_usr("jet_raster", "spectrum", program, ["frames 20", f"shot {cwd / 'raster.png'}"])
    rows = png_rows(cwd / "raster.png")
    x, y = raster_position(e0)
    need(0 < x < 352 and 0 < y < 287, f"the change at ({x}, {y}) is outside the frame")
    blue, red = (0, 0, 205), (205, 0, 0)
    row = rows[y]
    need(all(p == blue for p in row[:x]) and all(p == red for p in row[x:]),
         f"row {y} (the OUT ends {e0}-{e0 + 3} T-states after the interrupt): expected blue to x = {x - 1}, red from {x}; "
         f"the colours change at {[i for i in range(1, 352) if row[i] != row[i - 1]]}: {row[x - 2:x + 2]}")
    need(all(p == blue for p in rows[y - 1]) and all(p == red for p in rows[y + 1]), f"the rows around row {y}")


def check_jet_timing_state() -> None:
    """(d) A state saved with Spectrum timing on while the raster routine runs (mid-frame: the shell's frames end 68096
    T-states after the interrupt) loads back to the same pictures and the same frame count, in the same run and in a new
    one; a machine with the original timing refuses it."""
    program, _ = jet_raster()
    cwd = WORK / "jet_timing_state"
    state = cwd / "spectrum.zrs"
    after = ["frames 20", f"state save {state}", "frames 7", f"shot {cwd / 'a.png'}", "peek 81F4 2", f"state load {state}",
             "frames 7", f"shot {cwd / 'b.png'}", "peek 81F4 2"]
    answers = jet_usr("jet_timing_state", "spectrum", program, after)
    need(answers[4]["data"] == answers[8]["data"], f"the frame count after the state: {answers[4]['data']} then {answers[8]['data']}")
    need((cwd / "a.png").read_bytes() == (cwd / "b.png").read_bytes(), "the picture after loading the state differs")
    main = main_checkout()
    fresh = control(main, [f"state load {state}", "frames 7", f"shot {cwd / 'c.png'}", "peek 81F4 2", "quit"],
                    "--config", str(cwd / "spectrum.yaml"))
    need(fresh[0]["ok"] and fresh[3]["data"] == answers[4]["data"], f"the state in a new run: {fresh}")
    need((cwd / "c.png").read_bytes() == (cwd / "a.png").read_bytes(), "the picture after loading the state in a new run differs")
    (cwd / "original.yaml").write_text("machine:\n  model: jet\nvideo:\n  look: plain\n", encoding="utf-8")
    refused = control(main, ["frames 5", f"state load {state}", "quit"], "--config", str(cwd / "original.yaml"))
    need(not refused[1]["ok"], "a machine with the original timing loaded a state of Spectrum timing")


CHECKS ={name[6:]: fn for name, fn in sorted(globals().items()) if name.startswith("check_")}


# --- snapshots and cheats ----------------------------------------------------------------------


def szx_blocks(data: bytes) -> dict[str, list[bytes]]:
    """The blocks of an SZX file by their tags (the zero bytes of a tag taken off)."""
    need(data[:4] == b"ZXST", "not an SZX file")
    blocks: dict[str, list[bytes]] = {}
    at = 8
    while at < len(data):
        tag, size = struct.unpack_from("<4sI", data, at)
        blocks.setdefault(tag.rstrip(b"\0").decode("latin1"), []).append(data[at + 8:at + 8 + size])
        at += 8 + size
    need(at == len(data), "the blocks of the SZX file do not end where the file does")
    return blocks


def check_jet_snapshots() -> None:
    """The JET's snapshots. A BASIC line is typed; the machine is saved as SZX, Z80 and SNA, power cycled (the copyright screen is
    back), and each file loaded: the screen is as it was and the machine goes on (a second line is typed). Each file's header is what
    its format says; the SZX's T-states into the frame are those of a frame's end; a 128K SNA and a damaged file are refused with a
    message; a file dropped on the window with its ending is loaded as a snapshot."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = folder("jet_snapshots")
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\nvideo:\n  look: plain\n", encoding="utf-8")
    config = str(cwd / "zetro.yaml")
    files = {ending: cwd / f"line.{ending}" for ending in ("szx", "z80", "sna")}
    saved = control(main, ["until 5 JET", "type 10p1", "enter", "frames 20", "text", f"snapshot save {files['szx']}",
                           f"snapshot save {files['z80']}", f"snapshot save {files['sna']}", "frames 1", f"snapshot save {cwd / 'later.szx'}",
                           f"snapshot save {cwd / 'bad.txt'}", "quit"], "--config", config)
    need(all(a["ok"] for a in saved[:-3]), f"the saves: {[a for a in saved if not a['ok']]}")
    need(not saved[-2]["ok"] and "sna" in saved[-2]["error"], f"a snapshot saved with another ending: {saved[-2]}")
    before = saved[4]["text"]
    need(re.search(r"10.PRINT 1", before), f"the line is not on the screen: {before!r}")
    # The headers.
    szx = files["szx"].read_bytes()
    need(szx[4:7] == bytes([1, 5, 1]), f"the SZX header: {szx[:8].hex()}")
    blocks = szx_blocks(szx)
    need(len(blocks["Z80R"][0]) == 37 and len(blocks["SPCR"][0]) == 8 and "AY" not in blocks, f"the SZX blocks: {sorted(blocks)}")
    pages = {block[2]: block for block in blocks["RAMP"]}
    need(sorted(pages) == [0, 2, 5] and all(len(b) == 3 + 16384 and b[0:2] == b"\0\0" for b in pages.values()), "the SZX RAM pages")
    tstates = struct.unpack_from("<I", blocks["Z80R"][0], 29)[0]
    later = struct.unpack_from("<I", szx_blocks((cwd / "later.szx").read_bytes())["Z80R"][0], 29)[0]
    # A frame is 69888 T-states, the harness runs whole frames, and the last instruction ends a few T-states past the frame's end:
    # the interrupt of this machine begins at raster line 248 of 312, so the frame's end is 14335 T-states after it.
    need(14335 <= tstates < 14335 + 30 and (later - tstates) % 69888 < 30, f"the SZX T-states into the frame: {tstates}, {later}")
    z80 = files["z80"].read_bytes()
    need(z80[6:8] == b"\0\0" and struct.unpack_from("<H", z80, 30)[0] == 54 and z80[34] == 0, f"the Z80 header: {z80[:36].hex()}")
    at, found = 86, []
    while at < len(z80):
        length, page = struct.unpack_from("<HB", z80, at)
        found.append(page)
        at += 3 + (16384 if length == 0xFFFF else length)
    need(at == len(z80) and sorted(found) == [4, 5, 8], f"the Z80 pages {found}, end {at} of {len(z80)}")
    sna = files["sna"].read_bytes()
    need(len(sna) == 49179 and sna[25] in (0, 1, 2) and sna[26] <= 7, f"the SNA: {len(sna)} bytes, header {sna[:27].hex()}")
    sp = struct.unpack_from("<H", sna, 23)[0]
    need(sp >= 0x4000 and struct.unpack_from("<H", sna, 27 + sp - 0x4000)[0] == struct.unpack_from("<H", blocks["Z80R"][0], 22)[0],
         "the SNA's PC is not on its stack")
    # Each file loaded after a power cycle.
    run = []
    for ending in ("szx", "z80", "sna"):
        run += ["reset power", "until 5 JET", f"snapshot load {files[ending]}", "frames 3", "text", "type 20p2", "enter", "frames 20", "text"]
    answers = control(main, run + ["quit"], "--config", config)
    need(all(a["ok"] for a in answers), f"the loads: {[a for a in answers if not a['ok']]}")
    for n, ending in enumerate(("szx", "z80", "sna")):
        same, more = answers[n * 9 + 4], answers[n * 9 + 8]
        need(same["text"] == before, f"{ending}: the screen after the load is not as it was: {same['text']!r}")
        need(re.search(r"10.PRINT 1", more["text"]) and re.search(r"20.PRINT 2", more["text"]), f"{ending}: the machine did not go on: {more['text']!r}")
    # A 128K snapshot is refused with its reason, a cut one with a message, and nothing is changed by either.
    header = bytes([0x3F]) + bytes(18) + bytes([4, 0x5A]) + struct.pack("<HH", 0x1234, 0xFF00) + bytes([1, 2])
    (cwd / "big.sna").write_bytes(header + bytes(131103 - 27))
    (cwd / "cut.z80").write_bytes(files["z80"].read_bytes()[:700])
    (cwd / "cut.szx").write_bytes(szx[:20000])
    answers = control(main, ["until 5 JET", f"snapshot load {cwd / 'big.sna'}", f"snapshot load {cwd / 'cut.z80'}", f"snapshot load {cwd / 'cut.szx'}",
                             f"snapshot load {cwd / 'missing.szx'}", "frames 5", "text", "quit"], "--config", config)
    need(not answers[1]["ok"] and "128K" in answers[1]["error"] and "48K" in answers[1]["error"], f"the 128K SNA: {answers[1]}")
    need(not answers[2]["ok"] and "Z80" in answers[2]["error"] and not answers[3]["ok"] and "SZX" in answers[3]["error"], f"cut files: {answers[2:4]}")
    need(not answers[4]["ok"] and "JET" in answers[-2]["text"], f"a refused snapshot changed the machine: {answers[-2]}")
    # A dropped file with the ending is a snapshot.
    (cwd / "check.script").write_text("frames 3\nexpect PRINT 1\n", encoding="utf-8")
    run = zetro(main, "--config", config, "--drop", str(files["szx"]), "--script", str(cwd / "check.script"))
    need(run.returncode == 0, f"a dropped snapshot: exit {run.returncode}: {run.stderr.strip()}")


POK = """\
NInfinite attributes
M  8 22528   7   0
Z  8 22529   5   0
NAsk for a value
Z  8 22530 256   0
NNo such RAM page
Z  1 100 1 0
NBroken
M  8 22531 300 0
Z  8 22532 1 0
Y
"""


def check_jet_cheats() -> None:
    """The JET's cheats. A .pok file with two trainers that poke the screen's attributes (one asks for its value), one for a RAM page a 48K
    machine lacks and one damaged: `cheat on` changes the bytes, a BASIC POKE does not undo it (it is written again each frame), `cheat
    off` puts the old bytes back; the file is found beside a tape or in its POKES folder when the tape goes in, and dropped when another
    tape comes."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = folder("jet_cheats")
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\nvideo:\n  look: plain\n", encoding="utf-8")
    (cwd / "game.pok").write_text(POK, encoding="utf-8")
    config = str(cwd / "zetro.yaml")
    answers = control(main, ["until 5 JET", "peek 5800 4", f"cheat load {cwd / 'game.pok'}", "cheat on 1", "peek 5800 4", "cheat on 2", "cheat on 2 300",
                             "cheat on 2 200", "peek 5800 4", "cheat on 3", "cheat on 4", "type o22528,2", "enter", "frames 5", "peek 5800 4",
                             "cheat off 1", "peek 5800 4", "type o22528,2", "enter", "frames 5", "peek 5800 4", "cheat off 2", "peek 5800 4", "quit"],
                      "--config", config)
    was = answers[1]["data"].split()
    need(answers[2]["ok"] and answers[3]["ok"], f"loading and switching on: {answers[2:4]}")
    need(answers[4]["data"].split() == ["07", "05", was[2], was[3]], f"trainer 1 on: {answers[4]['data']} for {was}")
    need(not answers[5]["ok"] and "value" in answers[5]["error"] and not answers[6]["ok"], f"a trainer that asks, without its value: {answers[5:7]}")
    need(answers[7]["ok"] and answers[8]["data"].split() == ["07", "05", "C8", was[3]], f"trainer 2 on with 200: {answers[8]['data']}")
    need(not answers[9]["ok"] and "RAM page 1" in answers[9]["error"], f"a RAM page the machine lacks: {answers[9]}")
    need(not answers[10]["ok"] and "line 9" in answers[10]["error"], f"a damaged trainer: {answers[10]}")
    need(answers[13]["ok"] and answers[14]["data"].split() == ["07", "05", "C8", was[3]],
         f"the program's POKE undid the cheat: {answers[14]['data']}")
    need(answers[16]["data"].split() == [was[0], "05", "C8", was[3]] or answers[16]["data"].split()[0] == was[0], f"trainer 1 off: {answers[16]['data']}")
    need(answers[20]["data"].split()[0] == "02", f"the program's POKE did not write with the cheat off: {answers[20]['data']}")
    need(answers[22]["data"].split()[2:] == was[2:], f"trainer 2 off: {answers[22]['data']} for {was}")
    # The file beside a tape, and in its POKES folder; another tape drops it.
    (cwd / "POKES").mkdir(exist_ok=True)
    (cwd / "demo.tap").write_bytes(tap_file())
    (cwd / "demo.pok").write_text(POK, encoding="utf-8")
    (cwd / "two.tap").write_bytes(tap_file())
    (cwd / "POKES" / "two.pok").write_text(POK.replace("22528", "22600"), encoding="utf-8")
    (cwd / "plain.tap").write_bytes(tap_file())
    answers = control(main, ["until 5 JET", f"insert tape {cwd / 'plain.tap'}", "cheat on 1", f"insert tape {cwd / 'demo.tap'}", "cheat on 1", "peek 5800 2",
                             "eject tape", f"insert tape {cwd / 'two.tap'}", "cheat on 1", "peek 5848 1", f"insert tape {cwd / 'plain.tap'}", "cheat on 1",
                             "quit"], "--config", config)
    need(not answers[2]["ok"], f"cheats without a .pok file: {answers[2]}")
    need(answers[4]["ok"] and answers[5]["data"].split()[:2] == ["07", "05"], f"the .pok beside the tape: {answers[4:6]}")
    need(answers[8]["ok"] and answers[9]["data"] == "07", f"the .pok in POKES: {answers[8:10]}")
    need(not answers[11]["ok"], f"another tape did not drop the cheats: {answers[11]}")
    # A file dropped on the window, and a damaged one.
    (cwd / "empty.pok").write_text("Y\n", encoding="utf-8")
    answers = control(main, ["until 5 JET", f"cheat load {cwd / 'empty.pok'}", f"cheat load {cwd / 'missing.pok'}", "quit"], "--config", config)
    need(not answers[1]["ok"] and "no trainers" in answers[1]["error"] and not answers[2]["ok"], f"bad files: {answers[1:3]}")
    (cwd / "dropped.script").write_text("until 5 JET\ncheat on 1\nframes 3\nexpect JET\n", encoding="utf-8")
    run = zetro(main, "--config", config, "--drop", str(cwd / "game.pok"), "--script", str(cwd / "dropped.script"))
    need(run.returncode == 0, f"a dropped .pok file: exit {run.returncode}: {run.stderr.strip()}")


def check_setup_snapshots_cheats() -> None:
    """The Cheats page lists the trainers of a dropped .pok file and switches them (one asks for its value in a number entry); the States page
    has the snapshot rows, saves one under its default name in snapshots/ and loads it back through the file picker."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = folder("setup_cheats")
    shutil.rmtree(cwd / "snapshots", ignore_errors=True)
    (cwd / "zetro.yaml").write_text("machine:\n  model: jet\nvideo:\n  look: plain\n", encoding="utf-8")
    (cwd / "game.pok").write_text(POK, encoding="utf-8")
    config = str(cwd / "zetro.yaml")
    empty = control(main, ["frames 3", "text", f"shot {cwd / 'cheats_none.png'}", "quit"], "--config", config, "--setup-page", "cheats")
    need("Load cheat file" in empty[1]["text"] and "no cheats loaded" in empty[1]["text"], f"the empty Cheats page: {empty[1]['text']!r}")
    # The number entry of the trainer that asks: digits replace the value, Page Up adds one.
    entry = control(main, ["frames 12", "text", f"shot {cwd / 'value.png'}", "quit"], "--config", config, "--setup-page", "cheats",
                    "--drop", str(cwd / "game.pok"), *keys("Down", "Down", "Return", "2", "5", "PageUp", first=4))
    need("Ask for a value" in entry[1]["text"] and "26_" in entry[1]["text"] and "0 to 255" in entry[1]["text"], f"the number entry: {entry[1]['text']!r}")
    # Down to the first trainer, Enter; Down, Enter, the value 2 0 0 typed, Enter; the screen is left with Escape.
    pressed = keys("Down", "Return", "Down", "Return", "2", "0", "0", "Return", first=4)
    page = control(main, ["frames 14", "text", f"shot {cwd / 'cheats.png'}", "frames 3", "peek 5800 3", "quit"], "--config", config, "--setup-page", "cheats",
                   "--drop", str(cwd / "game.pok"), *pressed)
    text = page[1]["text"]
    for wanted in ("game.pok", "Infinite attributes", "Ask for a value", "No such RAM page", "Broken"):
        need(wanted in text, f"{wanted!r} is not on the Cheats page: {text!r}")
    need(len(re.findall(r"\[X\]", text)) == 2 and "200" in text, f"the two trainers are not on: {text!r}")
    need(re.search(r"No such RAM page +line 7", text) and re.search(r"Broken +line 9", text), f"the damaged trainers' lines: {text!r}")
    need(page[4]["data"].split()[:3] == ["07", "05", "C8"], f"the cheats are not in the machine: {page[4]}")
    rows = png_rows(cwd / "cheats.png")
    need((len(rows[0]), len(rows)) == (320, 240) and any(BRIGHT_CYAN in row for row in rows[32:216]), "the Cheats page picture")
    # The States page: End goes to the last row, the second of the snapshot rows; Enter asks for a name (the default is offered) and
    # Enter again saves it under that name in snapshots/ beside the configuration, where a note says so.
    states = control(main, ["frames 3", "text", f"shot {cwd / 'states.png'}", "frames 4", "text", f"shot {cwd / 'name.png'}", "quit"],
                     "--config", config, "--setup-page", "states", *keys("End", "Return", first=3))
    need("Save snapshot file" in states[1]["text"] and "Load snapshot file" in states[1]["text"], f"the snapshot rows: {states[1]['text']!r}")
    need("jet-0001.szx" in states[4]["text"] and "Snapshot file" in states[4]["text"], f"the name entry: {states[4]['text']!r}")
    saved = cwd / "snapshots" / "jet-0001.szx"
    need(not saved.exists(), "the snapshot was saved before its name was accepted")
    states = control(main, ["frames 12", "text", f"shot {cwd / 'saved.png'}", "quit"], "--config", config, "--setup-page", "states",
                     *keys("End", "Return", "Return", first=3))
    need(saved.is_file() and saved.read_bytes()[:4] == b"ZXST", f"the snapshot is not saved under its default name: {states[1]['text']!r}")
    need("jet-0001.szx" in states[1]["text"] and "Saved as" in states[1]["text"], f"no note of the save: {states[1]['text']!r}")
    # The file picker lists a snapshot of a machine with a line typed; Enter picks it, the screen is left with the machine as it was saved.
    saved.unlink()
    answers = control(main, ["until 5 JET", "type 10p1", "enter", "frames 20", f"snapshot save {cwd / 'snapshots' / 'line.szx'}", "quit"], "--config", config)
    need(all(a["ok"] for a in answers), f"preparing the snapshot: {answers}")
    answers = control(main, ["frames 5", "text", f"shot {cwd / 'picker.png'}", "frames 30", "text", "quit"], "--config", config, "--setup-page", "states",
                      *keys("End", "Up", "Return", "Return", first=3))
    need("line.szx" in answers[1]["text"], f"the picker does not list the snapshot: {answers[1]['text']!r}")
    need("PRINT 1" in answers[4]["text"] and "Slot" not in answers[4]["text"], f"the snapshot picked did not load: {answers[4]['text']!r}")


# --- loader acceleration (the option tape_accelerate, src/hw/loadaccel.h) -------------------------------
#
# Each check loads a tape twice, with acceleration off and on (the ROM tape traps off), saves states at the same guest
# times in both runs and compares them byte for byte (all their chunks but TIME, the host's clock), then the final
# screens. ZETRO_ACCEL_STATS makes the machine say on stderr, when it is destroyed, how much it jumped.


def accel_run(cwd: Path, name: str, accel: str, timing: str, commands: list[str]) -> tuple[list[dict], tuple[int, int, int]]:
    """The JET with the traps off, acceleration `accel` and `timing`: the answers to `commands` and (jumps, T-states jumped,
    T-states run) from the machine's report."""
    main = main_checkout()
    config = cwd / f"{name}-{accel}.yaml"
    config.write_text(f"machine:\n  model: jet\n  options:\n    tape_traps: off\n    tape_save_traps: off\n    tape_accelerate: {accel}\n"
                      f"    timing: {timing}\nvideo:\n  look: plain\n", encoding="utf-8")
    run = subprocess.run([str(EXE), "--hidden", "--control", "stdin", "--config", str(config)], cwd=main, capture_output=True, text=True,
                         timeout=300, input="\n".join(commands + ["quit"]) + "\n", env=dict(os.environ, ZETRO_ACCEL_STATS="1"))
    answers = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    need(len(answers) == len(commands) + 1 and all(a["ok"] for a in answers), f"{name}, {accel}: {[a for a in answers if not a['ok']]} {run.stderr.strip()}")
    stats = re.search(r"loadaccel: (\d+) jumps, (\d+) of (\d+) T-states jumped", run.stderr)
    return answers[:-1], tuple(int(g) for g in stats.groups()) if stats else (0, 0, 0)


def accel_compare(name: str, timing: str, before: list[str], seconds: list[float], after: list[str]) -> tuple[list[dict], tuple[int, int, int]]:
    """`before`, then a state saved at each of `seconds` (guest time from the end of `before`), a shot, then `after`; run
    with acceleration off and on: the states and the shots must be the same. The answers to `after` (on) and the stats."""
    cwd = WORK / name
    cwd.mkdir(parents=True, exist_ok=True)
    results = {}
    for accel in ("off", "on"):
        commands, last = list(before), 0.0
        for i, s in enumerate(seconds):
            commands += [f"wait {s - last:.2f}", f"state save {cwd / f'{timing}-{i}-{accel}.zrs'}"]
            last = s
        commands += [f"shot {cwd / f'{timing}-{accel}.png'}", *after]
        results[accel] = accel_run(cwd, name, accel, timing, commands)
    for i, s in enumerate(seconds):
        off, on = (state_chunks(cwd / f"{timing}-{i}-{a}.zrs") for a in ("off", "on"))
        off.pop("TIME", None)
        on.pop("TIME", None)
        differ = sorted(tag for tag in set(off) | set(on) if off.get(tag) != on.get(tag))
        need(not differ, f"{timing} timing, {s} s: the states saved with acceleration off and on differ in {differ}")
    need((cwd / f"{timing}-off.png").read_bytes() == (cwd / f"{timing}-on.png").read_bytes(), f"{timing} timing: the final screens differ")
    need(not results["off"][1][0], f"acceleration off jumped: {results['off'][1]}")
    return results["on"][0][-len(after):] if after else [], results["on"][1]


def tape_position(path: Path) -> tuple[int, int]:
    """The recorder's place in a state: the block and the T-states into it (media/tape_deck.c's chunk)."""
    return struct.unpack_from("<IQ", state_chunks(path)["TAPE"][0], 155)


def check_jet_accel_rom() -> None:
    """The demo tape loaded by the ROM's own loader (LOAD "", the traps off), acceleration off and on, the JET's own timing and
    Spectrum timing: identical states 2 s after PLAY (the first block's leader tone), 8.12 s (the program's bytes) and 12 s (after
    the end), the same final screen with the program's output, and the loader's waits jumped through."""
    main = main_checkout()
    tape = main.parent / "MiSTerRO" / "src" / "core-jet" / "assets" / "tapes" / "demo.tap"
    if not (main / "roms" / "jet.rom").is_file() or not tape.is_file():
        raise Skipped("no JET ROM or no JET core beside the repository")
    for timing in ("original", "spectrum"):
        after, (jumps, jumped, total) = accel_compare("jet_accel_rom", timing, ["until 5 JET", "frames 50", "type j\"\"", "enter", f"play {tape.resolve()}"],
                                                    [2.0, 8.12, 12.0], ["text"])
        need("DEMO" in after[0]["text"] and re.search(r"^\s*42\s*$", after[0]["text"], re.M), f"{timing} timing: the demo did not run: {after[0]['text']!r}")
        need(jumps > 1000 and jumped > total // 10, f"{timing} timing: the ROM's loader was hardly jumped through: {jumps} jumps, {jumped} of {total} T-states")
        places = [tape_position(WORK / "jet_accel_rom" / f"{timing}-{i}-on.zrs") for i in range(3)]
        need(places[0][0] == 0 and places[0][1] < 8063 * 2168 and places[1][0] == 1 and places[1][1] > 3223 * 2168 + 1402 and places[2][0] == 2,
             f"{timing} timing: the states are not in the leader, the bytes and after the end: {places}")


# A turbo loader of the check's own (at 8000h, started with RANDOMIZE USR 32768): a pilot, two sync pulses and the bytes, two
# pulses a bit (TZX block 14h, one pulse 2500 T-states, zero 1000), the first pulse measured by edge_a, the second by edge_b.
# edge_a samples at its loop's top and counts B down with DEC B, JR NZ (an 8-bit timeout); edge_b samples through E (IN E,(C))
# and counts HL up, its timeout tested by LD A,H; CP 8 (a 16-bit counter tested through another register). The guard's
# variants: edge_w also writes memory in its loop, edge_p also reads port 1Fh; neither may be jumped through.
ACCEL_BYTES = bytes((i * 37 + 11) & 0xFF for i in range(0xC0))
ACCEL_EDGES = {
    "a": [0x06, 0x00, "wait:", 0xDB, 0xFE, 0xE6, 0x40, 0xBA, ("jr", 0x20, "got"), 0x05, ("jr", 0x20, "wait"), 0xC9, "got:", 0x57, 0xC9],
    "w": [0x06, 0x00, "wait:", 0xDB, 0xFE, 0xE6, 0x40, 0x32, 0x00, 0x83, 0xBA, ("jr", 0x20, "got"), 0x05, ("jr", 0x20, "wait"), 0xC9,
          "got:", 0x57, 0xC9],
    "b": [0x21, 0x00, 0x00, "wait:", 0x23, 0x7C, 0xFE, 0x08, ("jr", 0x28, "done"), 0xED, 0x58, 0x7B, 0xE6, 0x40, 0xAA, ("jr", 0x28, "wait"),
          0x7A, 0xEE, 0x40, 0x57, "done:", 0xC9],
    "p": [0x21, 0x00, 0x00, "wait:", 0x23, 0xDB, 0x1F, 0x7C, 0xFE, 0x08, ("jr", 0x28, "done"), 0xED, 0x58, 0x7B, 0xE6, 0x40, 0xAA,
          ("jr", 0x28, "wait"), 0x7A, 0xEE, 0x40, 0x57, "done:", 0xC9],
}


def assemble(origin: int, items: list) -> bytes:
    """Bytes, "label:" and ("jr", opcode, label); labels are local to the list."""
    labels, pc = {}, origin
    for item in items:
        if isinstance(item, str):
            labels[item[:-1]] = pc
        else:
            pc += 1 if isinstance(item, int) else 2
    code = bytearray()
    for item in items:
        if isinstance(item, int):
            code.append(item)
        elif isinstance(item, tuple):
            d = labels[item[2]] - (origin + len(code) + 2)
            need(-128 <= d < 128, f"a jump too far: {item}")
            code += bytes([item[1], d & 0xFF])
    return bytes(code)


def accel_loader(first: str, second: str) -> bytes:
    """The loader at 8000h with its two edge routines."""
    edge_a = assemble(0x8100, ACCEL_EDGES[first])
    edge_b = assemble(0x8180, ACCEL_EDGES[second])
    main = [0xF3, 0xDD, 0x21, 0x00, 0xA0, 0x01, 0xFE, 0x00, 0xDB, 0xFE, 0xE6, 0x40, 0x57, 0x1E, 0x20,      # DI; IX, BC, D: the level; E 32
            "pilot:", 0xCD, 0x00, 0x81, 0x1D, ("jr", 0x20, "pilot"),                                     # 32 edges of the pilot
            "sync:", 0xCD, 0x00, 0x81, 0x78, 0xFE, 0xF2, ("jr", 0x38, "sync"),                           # until a pulse of 14 turns at most
            0xCD, 0x00, 0x81,                                                                            # the second sync pulse
            "byte:", 0xDD, 0x36, 0x00, 0x01,                                                             # LD (IX+0),1: the marker
            "bit:", 0xCD, 0x00, 0x81, 0x78, 0xFE, 0xE0, 0xDD, 0xCB, 0x00, 0x16, 0xF5,                    # edge_a; one if over 32 turns; RL (IX+0)
            0xCD, 0x80, 0x81, 0xF1, ("jr", 0x30, "bit"),                                                 # edge_b; until the marker comes out
            0xDD, 0x23, 0xDD, 0x7D, 0xFE, len(ACCEL_BYTES), ("jr", 0x20, "byte"), 0xFB, 0xC9]            # INC IX; LD A,IXL; CP; EI; RET
    code = assemble(0x8000, main)
    need(len(code) <= 0x100, "the loader is too long")
    return code + bytes(0x100 - len(code)) + edge_a + bytes(0x80 - len(edge_a)) + edge_b


def accel_tape() -> bytes:
    """The loader's tape (TZX): a pilot of 1000 pulses of 3500 T-states, two sync pulses of 500, the bytes (one 2500, zero 1000)."""
    data = ACCEL_BYTES
    return (b"ZXTape!\x1A\x01\x14" + bytes([0x12]) + struct.pack("<HH", 3500, 1000) + bytes([0x13, 2]) + struct.pack("<HH", 500, 500)
            + bytes([0x14]) + struct.pack("<HHBH", 1000, 2500, 8, 500) + len(data).to_bytes(3, "little") + data)


def accel_usr(loader: bytes, tape: Path) -> list[str]:
    """The loader poked at 8000h, the tape inserted, RANDOMIZE USR 32768 typed and PLAY pressed with its Enter."""
    pokes = [f"poke {0x8000 + i:04X} " + " ".join(f"{b:02X}" for b in loader[i:i + 128]) for i in range(0, len(loader), 128)]
    return ["until 5 JET", "frames 50", *pokes, f"insert tape {tape}", "type t", "key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3",
            "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3", "type l32768", "enter", "tape play"]


def check_jet_accel_turbo() -> None:
    """The check's own turbo loader (two loops of other shapes than the ROM's) and its TZX tape, acceleration off and on, the JET's
    own timing and Spectrum timing: identical states in the pilot (0.5 s after PLAY), in the bytes (1.8 s) and after the end (3.5 s),
    the same final screen, the bytes loaded right, and both loops jumped through."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = WORK / "jet_accel_turbo"
    cwd.mkdir(parents=True, exist_ok=True)
    tape = cwd / "turbo.tzx"
    tape.write_bytes(accel_tape())
    for timing in ("original", "spectrum"):
        after, (jumps, jumped, total) = accel_compare("jet_accel_turbo", timing, accel_usr(accel_loader("a", "b"), tape), [0.5, 1.8, 3.5],
                                                    [f"peek A000 {len(ACCEL_BYTES):X}", "text"])
        need(bytes(int(b, 16) for b in after[0]["data"].split()) == ACCEL_BYTES, f"{timing} timing: the bytes loaded: {after[0]['data']}")
        need("0 OK" in after[1]["text"], f"{timing} timing: the loader did not return: {after[1]['text']!r}")
        need(jumps > 1000 and jumped > total // 10, f"{timing} timing: the loader was hardly jumped through: {jumps} jumps, {jumped} of {total} T-states")
        places = [tape_position(cwd / f"{timing}-{i}-on.zrs") for i in range(3)]
        need(places[0][0] == 0 and places[1][0] == 2 and 0 < places[1][1] < len(ACCEL_BYTES) * 8 * 7000 and places[2][0] == 3,
             f"{timing} timing: the states are not in the pilot, the bytes and after the end: {places}")
    # Each loop alone: with the other one replaced by its guard variant, the loader is still jumped through.
    for loops in (("a", "p"), ("w", "b")):
        _, (jumps, _, _) = accel_run(cwd, "".join(loops), "on", "original", accel_usr(accel_loader(*loops), tape) + ["wait 3.5", "peek A000 1"])
        need(jumps > 100, f"edge_{loops[0] if loops[0] == 'a' else loops[1]} alone was not jumped through: {jumps} jumps")


def check_jet_accel_guard() -> None:
    """The guard: the turbo loader with loops that look like sampling loops but also write memory (edge_w) or read another port
    (edge_p) loads the same bytes to the same states and screen with acceleration off and on, and nothing is jumped through."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'jet.rom'} (python tools/fetch_roms.py)")
    cwd = WORK / "jet_accel_guard"
    cwd.mkdir(parents=True, exist_ok=True)
    tape = cwd / "turbo.tzx"
    tape.write_bytes(accel_tape())
    after, (jumps, jumped, total) = accel_compare("jet_accel_guard", "original", accel_usr(accel_loader("w", "p"), tape), [1.8, 3.5],
                                                [f"peek A000 {len(ACCEL_BYTES):X}"])
    need(bytes(int(b, 16) for b in after[0]["data"].split()) == ACCEL_BYTES, f"the bytes loaded: {after[0]['data']}")
    need(jumps == 0, f"a loop that writes memory or reads another port was jumped through: {jumps} jumps, {jumped} of {total} T-states")


# --- the HC-91 Plus and the HC-2000 ---------------------------------------------------------------------------
#
# Each check replays what the HC core's own validation cases do (MiSTerRO/src/core-hc/sim/scripts and its test programs,
# MiSTerRO/docs/hc/validation.md) through the harness, headless. They are skipped without the ROMs (python
# tools/fetch_roms.py) or the MiSTerRO checkout beside the repository.

SPECTRUM_KEYS = ["CAPS SHIFT", "Z", "X", "C", "V", "A", "S", "D", "F", "G", "Q", "W", "E", "R", "T", "1", "2", "3", "4", "5",
                 "0", "9", "8", "7", "6", "P", "O", "I", "U", "Y", "ENTER", "L", "K", "J", "H", "SPACE", "SYMBOL SHIFT", "M", "N", "B"]
# The PS/2 scan codes (set 2) the core's scripts press, as the keys of the Spectrum's matrix.
PS2_KEYS = {0x1C: "A", 0x32: "B", 0x21: "C", 0x23: "D", 0x24: "E", 0x2B: "F", 0x34: "G", 0x33: "H", 0x43: "I", 0x3B: "J", 0x42: "K",
            0x4B: "L", 0x3A: "M", 0x31: "N", 0x44: "O", 0x4D: "P", 0x15: "Q", 0x2D: "R", 0x1B: "S", 0x2C: "T", 0x3C: "U", 0x2A: "V",
            0x1D: "W", 0x22: "X", 0x35: "Y", 0x1A: "Z", 0x45: "0", 0x16: "1", 0x1E: "2", 0x26: "3", 0x25: "4", 0x2E: "5", 0x36: "6",
            0x3D: "7", 0x3E: "8", 0x46: "9", 0x29: "SPACE", 0x5A: "ENTER"}


def hcp_core() -> tuple[Path, Path]:
    """The main checkout and the HC core beside it; skipped when the core is not there."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-hc"
    if not (core / "sim" / "scripts").is_dir() or not (core / "assets" / "disks" / "hc2000").is_dir():
        raise Skipped(f"no HC core beside the repository ({core})")
    return main, core


def hc_rom(name: str) -> bytes:
    """A ROM of the main checkout's roms/; skipped when it is not there."""
    path = main_checkout() / "roms" / name
    if not path.is_file():
        raise Skipped(f"no {path} (python tools/fetch_roms.py)")
    return path.read_bytes()


def hcp_config(cwd: Path, name: str, model: str, **options: str) -> Path:
    path = cwd / f"{name}.yaml"
    lines = ["machine:", f"  model: {model}"] + (["  options:"] + [f"    {k}: {v}" for k, v in options.items()] if options else [])
    path.write_text("\n".join(lines + ["video:", "  look: plain", ""]), encoding="utf-8")
    return path


def hc_run(config: Path, commands: list[str], *args: str) -> list[dict]:
    """The commands on the control channel (in the main checkout, for its roms/); every answer but the last is ok."""
    answers = control(main_checkout(), commands + ["quit"], "--config", str(config), *args)
    failed = [(c, a) for c, a in zip(commands, answers) if not a.get("ok")]
    need(not failed, f"{failed[:2]}")
    return answers[:-1]


def peeked(answer: dict) -> bytes:
    return bytes(int(b, 16) for b in answer["data"].split())


def poke_commands(address: int, code: bytes) -> list[str]:
    return [f"poke {address + i:04X} " + " ".join(f"{b:02X}" for b in code[i:i + 64]) for i in range(0, len(code), 64)]


def key_commands(names: list[str], hold: int = 4) -> list[str]:
    """The keys down together for `hold` frames, then up for a while (the ROMs take a key again only after it was up)."""
    return [f"key {n}" for n in names] + [f"frames {hold}"] + [f"release {n}" for n in reversed(names)] + ["frames 8"]


def hcp_script(core: Path, name: str, waits: dict[str, list[str]]) -> list[str]:
    """A script of the core's simulator (press PS/2 code, matrix HEX SECONDS, run SECONDS, reset, and its waits) as harness
    commands: `waits` says what its basic_ready and cpm_ready are."""
    commands: list[str] = []
    for line in (core / "sim" / "scripts" / f"{name}.txt").read_text(encoding="utf-8").splitlines():
        word, _, rest = line.strip().partition(" ")
        if word == "press":
            code = int(rest, 16)
            if code == 0x172:                                           # the cursor down key: CAPS SHIFT and 6
                commands += key_commands(["CAPS SHIFT", "6"], 6)
            else:
                commands += key_commands(["SYMBOL SHIFT", "M"] if code == 0x49 else [PS2_KEYS[code]])
        elif word == "matrix":
            mask, seconds = rest.split()
            commands += key_commands([SPECTRUM_KEYS[b] for b in range(40) if int(mask, 16) >> b & 1], round(float(seconds) * 50))
        elif word == "run":
            commands.append(f"wait {rest}")
        elif word == "reset":
            commands.append("reset")
        elif word in waits:
            commands += waits[word]
        else:
            raise Failed(f"the core's script {name} has a command this check does not know: {line!r}")
    return commands


def screen_has(text: str, pattern: str) -> bool:
    return any(re.search(pattern, line) for line in text.splitlines())


def hc128_down(n: int) -> list[str]:
    return [c for _ in range(n) for c in key_commands(["CAPS SHIFT", "6"], 6)]


def hc128_start(entry: int) -> list[str]:
    """The 128 menu's entry `entry` (1: 128 BASIC, 3: 48 BASIC) chosen, and its BASIC up (the 48 BASIC tests its RAM first)."""
    return (["until 10 Tape Loader", "frames 30"] + hc128_down(entry) + ["enter"] +
            (["until 60 HC - 91", "frames 30"] if entry == 3 else ["frames 40"]))


def usr_commands(address: int) -> list[str]:
    """RANDOMIZE USR address typed in 48 BASIC's keyword mode (USR is extended mode, L), and entered."""
    return ["type t", "key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3",
            f"type l{address}", "enter"]


# The HC-91 Plus's paging and AY (the core's hc_expansion_bus.asm, with the AY read back): a distinct byte written to each of the
# eight pages through port 7FFD at C000h and read back, page 0 written last; then 3Fh to 7FFD (page 7, screen 7, ROM 1, the
# lock) and a 00h that the lock ignores, the byte at C000h kept; an AY tone (R0 16, R7 3Eh, R8 15) and the registers read back;
# 5Ah at 91FFh, and a loop. At 9000h (page 2, which 7FFD never moves); the results at 9100h.
HC128_PAGES = bytes([
    0xF3, 0x01, 0xFD, 0x7F, 0x1E, 0x00,                                         # DI; LD BC,7FFDh; LD E,0
    0x7B, 0xED, 0x79, 0xC6, 0x40, 0x32, 0x00, 0xC0, 0x1C, 0x7B, 0xFE, 0x08, 0x20, 0xF2,     # page E at C000h := 40h + E
    0xAF, 0xED, 0x79, 0x3E, 0x6E, 0x32, 0x00, 0xC0, 0x1E, 0x00, 0x21, 0x00, 0x91,           # page 0 := 6Eh; HL := 9100h
    0x7B, 0xED, 0x79, 0x3A, 0x00, 0xC0, 0x77, 0x23, 0x1C, 0x7B, 0xFE, 0x08, 0x20, 0xF2,     # each page read back to (HL)
    0x3E, 0x3F, 0xED, 0x79, 0xAF, 0xED, 0x79, 0x3A, 0x00, 0xC0, 0x32, 0x08, 0x91,           # the lock: (C000h) to 9108h
    0x01, 0xFD, 0xFF, 0xAF, 0xED, 0x79, 0x06, 0xBF, 0x3E, 0x10, 0xED, 0x79,                 # R0 := 16
    0x06, 0xFF, 0x3E, 0x07, 0xED, 0x79, 0x06, 0xBF, 0x3E, 0x3E, 0xED, 0x79,                 # R7 := 3Eh
    0x06, 0xFF, 0x3E, 0x08, 0xED, 0x79, 0x06, 0xBF, 0x3E, 0x0F, 0xED, 0x79,                 # R8 := 15
    0x06, 0xFF, 0xED, 0x78, 0x32, 0x0A, 0x91,                                               # R8 read to 910Ah
    0x3E, 0x07, 0xED, 0x79, 0xED, 0x78, 0x32, 0x0B, 0x91,                                   # R7 to 910Bh
    0xAF, 0xED, 0x79, 0xED, 0x78, 0x32, 0x0C, 0x91,                                         # R0 to 910Ch
    0x3E, 0x5A, 0x32, 0xFF, 0x91, 0x18, 0xFE])                                              # 5Ah to 91FFh; JR $


def hc128_run_pages(config: Path, after: list[str], *args: str) -> list[dict]:
    """The 128 BASIC up, the program above poked and run (RANDOMIZE USR 36864 typed in full), then `after`."""
    return hc_run(config, hc128_start(1) + poke_commands(0x9000, HC128_PAGES) + ["type randomize usr 36864", "enter", "frames 60"] + after, *args)


def check_hc128_basic() -> None:
    """The HC-91 Plus starts to its 128 menu (the five entries, the copyright line), runs the core's hc128-basic case (Down and Enter
    in the menu, PRINT 2+2 typed in full in the 128 editor: 4 and the report 0 OK on the screen) and goes into the 48 BASIC from the
    menu (the 48K machine's banner), where PRINT 3+3 gives 6."""
    main, core = hcp_core()
    hc_rom("hc128.rom")
    cwd = folder("hc128_basic")
    config = hcp_config(cwd, "hc128", "hc128")
    answers = hc_run(config, ["until 10 Tape Loader", "frames 30", "text", f"shot {cwd / 'menu.png'}"])
    for entry in ("Tape Loader", "128 BASIC", "Calculator", "48 BASIC", "Tape Tester", "FELIX COMPUTER"):
        need(entry in answers[-2]["text"], f"{entry!r} is not in the 128 menu: {answers[-2]['text']!r}")
    need((cwd / "menu.png").stat().st_size > 500, "menu.png")
    waits = {"basic_ready": ["frames 20"]}
    answers = hc_run(config, ["until 10 Tape Loader", "frames 30"] + hcp_script(core, "hc128-basic", waits) + ["text"])
    text = answers[-1]["text"]
    need(screen_has(text, r"^4$") and screen_has(text, r"0 OK"), f"PRINT 2+2 in 128 BASIC: {text!r}")
    answers = hc_run(config, hc128_start(3) + ["type p3+3", "enter", "frames 20", "text"])
    need(screen_has(answers[-1]["text"], r"^6$") and screen_has(answers[-1]["text"], r"0 OK"), f"PRINT 3+3 in 48 BASIC: {answers[-1]['text']!r}")


def check_hc128_paging() -> None:
    """The core's HC-91 Plus bus test through the harness: a different byte in each of the eight RAM pages through port 7FFD (also
    read through ZR_PAGE_ADDRESS), the ROM page paged (the editor's first bytes at first, the 48 BASIC's after the lock) and the
    lock holding against a later write; an AY tone whose registers read back (R0, R7, R8) and which a recording (--wav) hears."""
    rom = hc_rom("hc128.rom")
    cwd = folder("hc128_paging")
    config = hcp_config(cwd, "hc128", "hc128")
    wav = cwd / "tone.wav"
    answers = hc128_run_pages(config, ["peek 9100 10", "peek 91FF 1", "peek 0 8"] +
                              [f"peek {0x100000 + p * 0x4000:X} 1" for p in range(8)] + ["wait 0.5"], "--wav", str(wav))
    results = peeked(answers[-12])
    need(results[:8] == bytes([0x6E, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47]), f"the eight pages read back: {results[:8].hex(' ')}")
    need(results[8] == 0x47, f"the page 7 locked in at C000h, a later write to 7FFD ignored: {results[8]:02X}")
    need(results[10:13] == bytes([0x0F, 0x3E, 0x10]) and peeked(answers[-11]) == b"\x5A", f"the AY's registers read back: {results[10:13].hex(' ')}")
    need(peeked(answers[-10]) == rom[0x4000:0x4008], f"after the lock the ROM at 0000h is the 48 BASIC's: {peeked(answers[-10]).hex(' ')}")
    need(bytes(peeked(a)[0] for a in answers[-9:-1]) == results[:8], "the pages through ZR_PAGE_ADDRESS differ from the pages through 7FFD")
    # The editor ROM is the one at 0000h before the program.
    before = hc_run(config, hc128_start(1) + ["peek 0 8"])
    need(peeked(before[-1]) == rom[:8], f"the 128 editor's first bytes: {peeked(before[-1]).hex(' ')}")
    # The screen shown is page 7 (bit 3 of the last byte written to 7FFD): a letter A poked into that page, beside the program's own byte
    # at its first cell, is on the screen whose page 5 holds the editor.
    glyph = rom[0x7D00 + 33 * 8:0x7D00 + 34 * 8]                      # the 48 BASIC ROM's A
    answers = hc128_run_pages(config, [f"poke {0x11C001 + 256 * y:X} {b:02X}" for y, b in enumerate(glyph)] + ["frames 3", "text"])
    need(answers[-1]["text"].strip() == "?A",
         f"the screen of page 7: {answers[-1]['text']!r}")
    # The recording hears the tone (a 6.8 kHz square wave from R0 = 16, volume 15).
    with wave.open(str(wav)) as w:
        samples = struct.unpack(f"<{w.getnframes() * w.getnchannels()}h", w.readframes(w.getnframes()))
        rate = w.getframerate()
    need(rate == 48000 and max(samples) > 2000 and min(samples) < -2000, f"the recording is silent: {max(samples)}, {min(samples)}")


def basic_tap() -> bytes:
    """A TAP file of a BASIC program `10 PRINT 42` that starts itself at its line 10: its header and its data."""
    program = bytes([0x00, 0x0A, 0x0A, 0x00, 0xF5, 0x34, 0x32, 0x0E, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x0D])
    header = bytearray([0x00, 0x00]) + b"HCDEMO    " + struct.pack("<HHH", len(program), 10, len(program))
    data = bytearray([0xFF]) + program
    blocks = b""
    for block in (header, data):
        parity = 0
        for b in block:
            parity ^= b
        blocks += struct.pack("<H", len(block) + 1) + bytes(block) + bytes([parity])
    return blocks


def check_hc128_tape() -> None:
    """A BASIC program on a tape, loaded in the 48 BASIC (chosen from the 128 menu) with the ROM tape traps on (the block taken at
    the loader's entry) and off (the tape played in real guest time, loader acceleration on): the same screen, the program's output
    on it; and a tape dropped on the machine is auto-loaded through the menu's Tape Loader entry (Enter), to the same output."""
    main, core = hcp_core()
    hc_rom("hc128.rom")
    cwd = folder("hc128_tape")
    tape = cwd / "demo.tap"
    tape.write_bytes(basic_tap())
    screens = {}
    for switch in ("on", "off"):
        config = hcp_config(cwd, f"traps-{switch}", "hc128", tape_traps=switch, tape_save_traps=switch)
        play = ["tape play"] if switch == "off" else []
        answers = hc_run(config, hc128_start(3) + [f"insert tape {tape}", "type j\"\"", "enter", *play, "until 90 42", "frames 100", "text"])
        screens[switch] = answers[-1]["text"]
    need(screen_has(screens["on"], r"Program: HCDEMO") and screen_has(screens["on"], r"^42$"), f"the trapped load: {screens['on']!r}")
    need(screens["on"] == screens["off"], f"the load in real time ends on another screen: {screens['off']!r} for {screens['on']!r}")
    (cwd / "autoload.script").write_text("until 120 42\nframes 100\nexpect Program: HCDEMO\n", encoding="utf-8")
    run = zetro(main, "--config", str(cwd / "traps-on.yaml"), "--script", str(cwd / "autoload.script"), "--drop", str(tape))
    need(run.returncode == 0, f"the dropped tape: exit {run.returncode}: {run.stderr.strip()}")


def sna48() -> bytes:
    """A 48K SNA: a program at 9000h that tries to page ROM 0 in through 7FFD and stores what it then reads at C000h and 0001h
    at A000h and A001h, PC on its stack, a 5Ah at C000h, interrupts off, IM 1, the border red."""
    program = bytes([0xF3, 0x01, 0xFD, 0x7F, 0xAF, 0xED, 0x79, 0x3A, 0x00, 0xC0, 0x32, 0x00, 0xA0, 0x3A, 0x01, 0x00, 0x32, 0x01, 0xA0, 0x18, 0xFE])
    ram = bytearray(49152)
    ram[0x5000:0x5000 + len(program)] = program
    ram[0xC000 - 0x4000] = 0x5A
    sp = 0x7F00
    ram[sp - 0x4000:sp - 0x4000 + 2] = struct.pack("<H", 0x9000)
    header = bytearray(27)
    header[23:25] = struct.pack("<H", sp)
    header[25] = 1
    header[26] = 2
    return bytes(header + ram)


def szx_summary(path: Path) -> tuple[bytes, int, bytes, dict[int, bytes]]:
    """The AY registers, the last 7FFD, the AY's selected register and the RAM pages of an SZX file."""
    blocks = szx_blocks(path.read_bytes())
    ay = blocks["AY"][0]
    pages = {b[2]: b[3:] for b in blocks["RAMP"]}
    return ay[2:18], blocks["SPCR"][0][1], ay[1:2], pages


def check_hc128_snapshots() -> None:
    """The HC-91 Plus's snapshots and states. The paging program is running (pages with their bytes, the lock, an AY tone); the machine
    is saved as SZX, Z80 and SNA, whose headers say a 128K machine with 7FFD 3Fh, the AY's registers (SZX and Z80) and eight pages;
    after a power cycle each file loads back to the same pages, paging and (SZX, Z80) AY registers, read from a snapshot taken again;
    a state saved and loaded back, in the same run and in a new one, gives the same. A 48K SNA loads into the 48 BASIC with the
    paging locked (its program's write to 7FFD does not move the ROM)."""
    main, core = hcp_core()
    rom = hc_rom("hc128.rom")
    cwd = folder("hc128_snapshots")
    config = hcp_config(cwd, "hc128", "hc128")
    files = {e: cwd / f"pages.{e}" for e in ("szx", "z80", "sna")}
    state = cwd / "pages.zrs"
    hc128_run_pages(config, [f"snapshot save {f}" for f in files.values()] + [f"state save {state}"])
    regs, last, current, pages = szx_summary(files["szx"])
    szx = files["szx"].read_bytes()
    need(szx[6] == 2 and last == 0x3F and regs[0] == 16 and regs[7] == 0x3E and regs[8] == 15 and sorted(pages) == list(range(8)),
         f"the SZX: machine {szx[6]}, 7FFD {last:02X}, AY {regs.hex(' ')}, pages {sorted(pages)}")
    need(pages[0][:1] == b"\x6E" and pages[3][:1] == b"\x43" and pages[7][:1] == b"\x47", "the SZX's pages are not the machine's")
    z80 = files["z80"].read_bytes()
    found, at = [], 86
    while at < len(z80):
        length, page = struct.unpack_from("<HB", z80, at)
        found.append(page)
        at += 3 + (16384 if length == 0xFFFF else length)
    need(z80[34] == 4 and z80[35] == 0x3F and z80[39 + 7] == 0x3E and sorted(found) == list(range(3, 11)),
         f"the Z80: hardware {z80[34]}, 7FFD {z80[35]:02X}, AY {z80[39:55].hex(' ')}, pages {found}")
    sna = files["sna"].read_bytes()
    need(len(sna) == 131103 and sna[27 + 49152 + 2] == 0x3F, f"the SNA: {len(sna)} bytes, 7FFD {sna[27 + 49152 + 2:27 + 49152 + 3].hex()}")
    # Each file after a power cycle: a snapshot taken again says the same.
    for ending, path in files.items():
        again = cwd / f"again-{ending}.szx"
        answers = hc_run(config, ["until 10 Tape Loader", "frames 30", f"snapshot load {path}", "frames 5", "peek 9100 10", "peek 0 8",
                                  f"snapshot save {again}", "frames 5", "peek 91FF 1"])
        need(peeked(answers[-5])[:9] == bytes([0x6E, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x47]), f"{ending}: the pages read back: {answers[-5]}")
        need(peeked(answers[-4]) == rom[0x4000:0x4008], f"{ending}: the ROM paged in is not the 48 BASIC's")
        regs2, last2, _, pages2 = szx_summary(again)
        need(last2 == 0x3F and pages2 == pages, f"{ending}: the paging or the pages after the load differ (7FFD {last2:02X})")
        if ending != "sna":
            need(regs2 == regs, f"{ending}: the AY's registers after the load: {regs2.hex(' ')} for {regs.hex(' ')}")
    # A state: in a new run, and in the same run after a power cycle.
    after = cwd / "after-state.szx"
    answers = hc_run(config, ["until 10 Tape Loader", "frames 30", f"state load {state}", "frames 5", "peek 9100 10", f"snapshot save {after}"])
    need(peeked(answers[-2])[:9] == bytes([0x6E, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x47]), f"the state's pages: {answers[-2]}")
    regs3, last3, _, pages3 = szx_summary(after)
    need((regs3, last3, pages3) == (regs, 0x3F, pages), "the state loaded in a new run: the paging, the AY or the pages differ")
    hc128_run_pages(config, [f"state save {cwd / 'second.zrs'}", "reset power", "frames 5", f"state load {cwd / 'second.zrs'}", "frames 5",
                             f"snapshot save {cwd / 'second.szx'}"])
    regs4, last4, _, pages4 = szx_summary(cwd / "second.szx")
    need((regs4, last4, pages4) == (regs, 0x3F, pages), "the state loaded in the same run: the paging, the AY or the pages differ")
    # A 48K snapshot: the 48 BASIC with the paging locked.
    (cwd / "small.sna").write_bytes(sna48())
    answers = hc_run(config, ["until 10 Tape Loader", "frames 30", f"snapshot load {cwd / 'small.sna'}", "frames 5", "peek A000 2", "peek 0 8"])
    need(peeked(answers[-2]) == bytes([0x5A, rom[0x4001]]), f"the 48K snapshot's program saw {answers[-2]['data']}: the paging was not locked in the 48 BASIC")
    need(peeked(answers[-1]) == rom[0x4000:0x4008], "the 48K snapshot did not leave the 48 BASIC at 0000h")
    # A cut file is refused with its reason.
    (cwd / "cut.szx").write_bytes(szx[:20000])
    answers = control(main, ["until 10 Tape Loader", f"snapshot load {cwd / 'cut.szx'}", "quit"], "--config", str(config))
    need(not answers[1]["ok"] and "SZX" in answers[1]["error"], f"a cut SZX: {answers[1]}")


# The Sinclair 128K's contention, as the JET's check has the 48K's: 228 T-states a line, 70908 a frame, from T-state 14361 on each of
# the 192 lines the CPU is held 6, 5, 4, 3, 2, 1, 0, 0 T-states for 128 T-states for an access to the odd RAM pages (and 4000h).
def sinclair128_delay(t: int) -> int:
    t %= 70908
    line, x = divmod(t - 14361, 228)
    return SINCLAIR_PATTERN[x % 8] if t >= 14361 and line < 192 and x < 128 else 0


def loop_turns(frame: int, loop: int, contended: bool) -> set[int]:
    """The turns of a loop (INC HL; JR back: 6 and 12 T-states, the refresh at 80xxh) between two frame interrupts, as the JET check
    works them out: the loop starts 69 T-states (+ the phase of the HALT, 0 to 3) after the interrupt, and the interrupt is taken after
    the first instruction that ends at or past the frame; `contended` has the loop's addresses held by the Sinclair 128K's ULA."""
    counts = set()
    for j in range(4):
        t, turns = j + 69, 0

        def cycle(t: int, address: int, length: int) -> int:
            return t + (sinclair128_delay(t) if contended and address >= 0xC000 else 0) + length
        while True:
            for address, length in ((loop, 4), (0x8000, 1), (0x8000, 1)):
                t = cycle(t, address, length)
            turns += 1
            if t >= frame:
                break
            for address, length in ((loop + 1, 4), (loop + 2, 3), *((loop + 2, 1),) * 5):
                t = cycle(t, address, length)
            if t >= frame:
                break
        counts.add(turns)
    return counts


def hc128_timing_program(page: int) -> bytes:
    """At 9000h: page `page` at C000h through 7FFD, IM 2 through 80FFh, HALT; the first interrupt zeroes HL and jumps to the loop at C000h
    (INC HL; JR back, poked into the page), the second stores HL at 81F2h, puts page 0 back and returns to BASIC."""
    h1, h2 = 0x9000 + 26, 0x9000 + 39
    return bytes([0xF3, 0xED, 0x73, 0xF0, 0x81, 0x01, 0xFD, 0x7F, 0x3E, page, 0xED, 0x79, 0x3E, 0x80, 0xED, 0x47, 0xED, 0x5E,
                  0x21, h1 & 0xFF, h1 >> 8, 0x22, 0xFF, 0x80, 0xFB, 0x76,
                  0x21, h2 & 0xFF, h2 >> 8, 0x22, 0xFF, 0x80, 0x21, 0x00, 0x00, 0xFB, 0xC3, 0x00, 0xC0,
                  0x22, 0xF2, 0x81, 0x01, 0xFD, 0x7F, 0xAF, 0xED, 0x79]) + USR_END


def check_hc128_timing() -> None:
    """The option "Spectrum timing" on the HC-91 Plus stands for a Sinclair 128K: a loop of INC HL; JR back at C000h counts its turns
    from one frame interrupt to the next (a frame of 70908 T-states with the option, the clone's 69888 without), in page 0 (not
    contended) and in page 1 (contended: held by the 128K's ULA as the documented pattern says, with the option only; the contended
    pages follow port 7FFD)."""
    hcp_core()
    hc_rom("hc128.rom")
    cwd = folder("hc128_timing")
    measured = {}
    for timing in ("original", "spectrum"):
        config = hcp_config(cwd, timing, "hc128", timing=timing)
        for page in (0, 1):
            answers = hc_run(config, hc128_start(1) + poke_commands(0x9000, hc128_timing_program(page)) +
                             [f"poke {0x100000 + page * 0x4000:X} 23 18 FD", "type randomize usr 36864", "enter", "frames 20", "peek 81F2 2"])
            low, high = peeked(answers[-1])
            measured[timing, page] = low | high << 8
    plain, plain128 = loop_turns(69888, 0xC000, False), loop_turns(70908, 0xC000, False)
    slow = loop_turns(70908, 0xC000, True)
    need(len(plain) == 1 and len(plain128) == 1 and plain != plain128, f"the loop's turns do not tell the frames apart: {plain}, {plain128}")
    for page in (0, 1):
        need(measured["original", page] in plain, f"original timing, page {page}: {measured['original', page]} turns, the clone's frame gives {plain}")
    need(measured["spectrum", 0] in plain128, f"Spectrum timing, page 0: {measured['spectrum', 0]} turns, a 70908 T-state frame gives {plain128}")
    need(measured["spectrum", 1] in slow, f"Spectrum timing, page 1: {measured['spectrum', 1]} turns, the contention gives {slow} ({plain128} without)")


# The HC-2000's configuration port and aliases (the core's hc2000_bus.asm, run from BASIC with the interrupts off): port 7E to 01h (the
# CP/M ROM at 0000h), the byte there to 9000h; 11h at C000h and 22h at E000h (RAM); 7E to 03h (RAM at 0000h, the ROM's upper half at E000h):
# 66h at 0000h and back to 9001h, E000h to 9002h (the ROM); C7h (the alias: C000h reaches the RAM behind E000h) and C000h to 9003h; C5h (the
# alias off) and C000h to 9004h; 7E to 07h (the lock), a 00h ignored, 7E read back to 9005h; 5Ah at 90FFh, HALT. At 8000h; results at 9000h.
HC2000_CONFIG = bytes([
    0xF3, 0x3E, 0x01, 0xD3, 0x7E, 0x3A, 0x00, 0x00, 0x32, 0x00, 0x90,
    0x3E, 0x11, 0x32, 0x00, 0xC0, 0x3E, 0x22, 0x32, 0x00, 0xE0,
    0x3E, 0x03, 0xD3, 0x7E, 0x3E, 0x66, 0x32, 0x00, 0x00, 0x3A, 0x00, 0x00, 0x32, 0x01, 0x90, 0x3A, 0x00, 0xE0, 0x32, 0x02, 0x90,
    0xAF, 0xD3, 0xC7, 0x3A, 0x00, 0xC0, 0x32, 0x03, 0x90, 0xD3, 0xC5, 0x3A, 0x00, 0xC0, 0x32, 0x04, 0x90,
    0x3E, 0x07, 0xD3, 0x7E, 0xAF, 0xD3, 0x7E, 0xDB, 0x7E, 0x32, 0x05, 0x90,
    0x3E, 0x5A, 0x32, 0xFF, 0x90, 0x76])


def hc2000_start() -> list[str]:
    """The HC-2000's BASIC up (its banner on the last line)."""
    return ["until 10 ICE FELIX", "frames 40"]


def check_hc2000_basic() -> None:
    """The HC-2000 starts its BASIC (its banner, the ROM's own: 16 KiB at 0000h) and runs the core's hc2000-basic case (PRINT 2+2, a reset
    from the keyboard's reset path, PRINT 3+3: 4 then 6 and the report 0 OK each time); a BASIC program on tape loads (the ROM tape traps
    find the routines in the BASIC ROM; the typed LOAD is the model's auto-load text); and the configuration port, the C5 and C7
    aliases and the lock do what the core's bus test expects (both ROMs, RAM at 0000h under the upper ROM, the alias, the lock); a 48K
    snapshot round trip."""
    main, core = hcp_core()
    basic, cpm = hc_rom("hc2000-0.rom"), hc_rom("hc2000-1.rom")
    cwd = folder("hc2000_basic")
    config = hcp_config(cwd, "hc2000", "hc2000")
    waits = {"basic_ready": ["until 20 0 OK", "frames 10", "text"]}
    answers = hc_run(config, hc2000_start() + ["text", "peek 0 8"] + hcp_script(core, "hc2000-basic", waits))
    need(screen_has(answers[2]["text"], "HC2000 ICE FELIX") and peeked(answers[3]) == basic[:8], f"the HC-2000's banner: {answers[2]['text']!r}")
    texts = [a["text"] for a in answers[4:] if "text" in a]
    need(len(texts) == 2 and screen_has(texts[0], r"^4$") and screen_has(texts[1], r"^6$") and screen_has(texts[1], "0 OK"), f"the case: {texts}")
    # A tape; the routines are found in the BASIC ROM.
    tape = cwd / "demo.tap"
    tape.write_bytes(basic_tap())
    answers = hc_run(config, hc2000_start() + [f"insert tape {tape}", "type j\"\"", "enter", "until 30 42", "frames 100", "text"])
    need(screen_has(answers[-1]["text"], r"Program: HCDEMO") and screen_has(answers[-1]["text"], r"^42$"), f"the trapped load: {answers[-1]['text']!r}")
    # The configuration port.
    answers = hc_run(config, hc2000_start() + poke_commands(0x8000, HC2000_CONFIG) + usr_commands(32768) + ["frames 30", "peek 9000 6", "peek 90FF 1"])
    results = peeked(answers[-2])
    need(results == bytes([cpm[0], 0x66, cpm[0x2000], 0x22, 0x11, 0x07]) and peeked(answers[-1]) == b"\x5A", f"the configuration port: {results.hex(' ')}")
    # A snapshot of the 48K machine: SZX of the BASIC screen, a power cycle, loaded back to the same screen.
    szx = cwd / "line.szx"
    answers = hc_run(config, hc2000_start() + ["type 10p1", "enter", "frames 20", "text", f"snapshot save {szx}", "reset power", "until 10 ICE FELIX",
                                              f"snapshot load {szx}", "frames 3", "text"])
    need(screen_has(answers[-1]["text"], r"10.PRINT 1") and answers[-1]["text"] == answers[-7]["text"], f"the snapshot: {answers[-1]['text']!r}")
    need(szx_blocks(szx.read_bytes())["Z80R"] and sorted(b[2] for b in szx_blocks(szx.read_bytes())["RAMP"]) == [0, 2, 5], "the SZX's pages")


def hc2000_disks(cwd: Path, core: Path) -> tuple[Path, Path]:
    """Copies of the core's HC-2000 system disk (A, 720K) and the HC-91 disk its v1 BIOS lists in B."""
    a, b = cwd / "a.dsk", cwd / "b.dsk"
    shutil.copyfile(core / "assets" / "disks" / "hc2000" / "system.dsk", a)
    shutil.copyfile(core / "assets" / "disks" / "hc91" / "system.dsk", b)
    return a, b


def cpm_boot(a: Path, b: Path) -> list[str]:
    """Both disks in, a power cycle (the configuration says CP/M at start), the prompt up."""
    return [f"insert a {a}", f"insert b {b}", "reset power", "until 30 A>", "frames 20"]


def check_hc2000_cpm() -> None:
    """The HC-2000 starts CP/M from the core's system.dsk (Startup: CP/M; the unchanged CP/M ROM boots from drive A through the 8272 in
    its polled use) and runs the core's cpm-dir case (the banner, A> and the directory), then its hc2000-files case: SAVE 1 CHECK.COM
    creates a file, DIR lists it, B: and DIR list the HC-91 disk in the external drive. The image of drive A changes on the host (it holds
    the file, the rest as it was) and opens again in a new run whose DIR lists the file; drive B is read and its image is not changed.
    The drive sounds are in the recording with their option on and not with it off."""
    main, core = hcp_core()
    hc_rom("hc2000-0.rom")
    hc_rom("hc2000-1.rom")
    cwd = folder("hc2000_cpm")
    a, b = hc2000_disks(cwd, core)
    original_a, original_b = a.read_bytes(), b.read_bytes()
    config = hcp_config(cwd, "cpm", "hc2000", startup="cpm")
    waits = {"cpm_ready": ["frames 300"]}
    answers = hc_run(config, cpm_boot(a, b) + hcp_script(core, "cpm-dir", waits) + ["text", f"shot {cwd / 'dir.png'}"])
    listing = answers[-2]["text"]
    need("CP/M" in listing and screen_has(listing, r"^A>dir") and screen_has(listing, r"MBASIC\s+COM") and screen_has(listing, r"^A>$"), f"DIR: {listing!r}")
    need(a.read_bytes() == original_a, "DIR changed the image of drive A")
    answers = hc_run(config, cpm_boot(a, b) + hcp_script(core, "hc2000-files", waits) + ["text"])
    text = answers[-1]["text"]
    need(screen_has(text, r"^B>") and screen_has(text, r"CHECK\s+COM") and screen_has(text, r"CPM91\s+TXT") and "NO FILE" not in text,
         f"the files case: {text!r}")
    image = a.read_bytes()
    need(image != original_a and b"CHECK   COM" in image and image[:8] == original_a[:8] and len(image) == len(original_a),
         "the image of drive A does not hold the file CHECK.COM, or lost its form")
    need(b.read_bytes() == original_b, "drive B was read: its image must not change")
    # The changed image opens again, and the file is there to read.
    answers = hc_run(config, cpm_boot(a, b) + ["type dir", "enter", "frames 300", "text"])
    need(screen_has(answers[-1]["text"], r"CHECK\s+COM"), f"the disk written by the machine in a new run: {answers[-1]['text']!r}")
    # The drive sounds (not original, on in a new configuration): a recording of a DIR has the motor's hum throughout, the same run with
    # the option off only the beeper's clicks.
    heard = {}
    for switch in ("on", "off"):
        wav = cwd / f"drive-{switch}.wav"
        hc_run(hcp_config(cwd, f"sound-{switch}", "hc2000", startup="cpm", drive_sound=switch), cpm_boot(a, b) + ["type dir", "enter", "frames 200"],
               "--wav", str(wav))
        with wave.open(str(wav)) as w:
            samples = struct.unpack(f"<{w.getnframes() * w.getnchannels()}h", w.readframes(w.getnframes()))
        heard[switch] = sum(1 for x in samples if x), len(samples)
    need(heard["on"][0] > 0.9 * heard["on"][1] and heard["off"][0] < 0.5 * heard["off"][1], f"the drive sounds: samples heard {heard}")


def check_hc2000_state() -> None:
    """A state saved in the middle of a disk operation (the 8272 in its execution phase, the drive's motor on, found by saving every few
    frames of a DIR) loads back to the same end, in the same run and in a new one that has the disk in the drive: the controller, the
    drives, the latch and the motor delay are all in the state."""
    main, core = hcp_core()
    hc_rom("hc2000-0.rom")
    hc_rom("hc2000-1.rom")
    cwd = folder("hc2000_state")
    a, b = hc2000_disks(cwd, core)
    config = hcp_config(cwd, "cpm", "hc2000", startup="cpm")
    states = [cwd / f"s{i}.zrs" for i in range(40)]
    saves = [c for s in states for c in ("frames 3", f"state save {s}")]
    answers = hc_run(config, cpm_boot(a, b) + ["type dir", "enter"] + saves + ["frames 300", "text"])
    end = answers[-1]["text"]
    need(screen_has(end, r"CHECK|MBASIC"), f"DIR: {end!r}")
    # The 8272's chunk: version, rate, clock (9 bytes), then its flags: ... in_reset (6), phase (7); 2 is the execution phase.
    busy = [s for s in states if state_chunks(s)["8272"][0][16] == 2 and state_chunks(s)["FDD "][0][1] == 1]
    need(len(busy) >= 3, f"no state of the 40 was saved in the 8272's execution phase with the motor on ({len(busy)})")
    middle = busy[len(busy) // 2]
    again = hc_run(config, cpm_boot(a, b) + ["type dir", "enter", "frames 30", f"state load {middle}", "frames 300", "text"])
    # (The CP/M cursor blinks: the screen is the same but for that cell, which the read-back shows as '?' while it is lit.)
    need(again[-1]["text"].replace("?", "") == end.replace("?", ""), f"the state loaded in the same run ends on another screen: {again[-1]['text']!r} for {end!r}")
    fresh = hc_run(config, [f"insert a {a}", f"insert b {b}", f"state load {middle}", "frames 300", "text"])
    need(fresh[-1]["text"].replace("?", "") == end.replace("?", ""), f"the state loaded in a new run ends on another screen: {fresh[-1]['text']!r} for {end!r}")


def check_hc2000_screen() -> None:
    """The HC-2000's screen select (bit 3 of port 7E: the RAM at C000h instead of 4000h): a letter A poked at C000h is the screen's only
    character after the write to 7E."""
    hcp_core()
    glyph = hc_rom("hc2000-0.rom")[0x3D00 + 33 * 8:0x3D00 + 34 * 8]     # the BASIC ROM's A
    cwd = folder("hc2000_screen")
    config = hcp_config(cwd, "screen", "hc2000")
    answers = hc_run(config, hc2000_start() + [f"poke {0xC000 + 256 * y:X} {b:02X}" for y, b in enumerate(glyph)] +
                     poke_commands(0x8000, bytes([0xF3, 0x3E, 0x08, 0xD3, 0x7E, 0x18, 0xFE])) + usr_commands(32768) + ["frames 5", "text"])
    need(answers[-1]["text"].strip() == "A", f"the screen at C000h: {answers[-1]['text']!r}")


# A program that loops reading ports 1Fh (Kempston), 7Fh (Fuller), EFFEh (the keys 0 9 8 7 6) and F7FEh (1 2 3 4 5) into 9000h to 9003h.
HC_STICK_PORTS = bytes([0x01, 0x1F, 0x00, 0xED, 0x78, 0x32, 0x00, 0x90, 0x01, 0x7F, 0x00, 0xED, 0x78, 0x32, 0x01, 0x90,
                        0x01, 0xFE, 0xEF, 0xED, 0x78, 0x32, 0x02, 0x90, 0x01, 0xFE, 0xF7, 0xED, 0x78, 0x32, 0x03, 0x90, 0x18, 0xDE])


def hcp_run_usr(config: Path, model: str, program: bytes, after: list[str], *args: str) -> list[dict]:
    """The machine's BASIC up (the 128 menu's 128 BASIC), `program` poked at 8000h and run with RANDOMIZE USR 32768 typed the way the
    editor takes it, ten frames, then `after`."""
    if model == "hc128":
        start, typed = hc128_start(1), ["type randomize usr 32768", "enter"]
    else:
        start, typed = hc2000_start(), usr_commands(32768)
    return hc_run(config, start + poke_commands(0x8000, program) + typed + ["frames 10"] + after, *args)


def check_hc_plus_sticks() -> None:
    """The shared core's joystick kinds on the HC-91 Plus and the HC-2000: a program loops reading ports 1Fh, 7Fh, EFFEh and F7FEh while the
    harness holds joystick 0 left and fire (from the program's start: the Sinclair and Cursor kinds press the matrix's keys). Kempston
    gives 12h at 1Fh, Fuller 7Bh at 7Fh, Sinclair I the keys 6 and 0 (EFFEh: 0Eh), Sinclair II the keys 1 and 5 (F7FEh: 0Eh), Cursor the
    keys 5 and 0 (F7FEh 0Fh, EFFEh 1Eh); a port no socket answers reads FFh and a matrix row untouched 1Fh. With joystick 1 on
    Sinclair I and held up, the keys 9 (EFFEh: 1Dh) is pressed too."""
    hcp_core()
    cwd = folder("hc_plus_sticks")
    cases = [("none", "none", [0xFF, 0xFF, 0x1F, 0x1F]), ("kempston", "none", [0x12, 0xFF, 0x1F, 0x1F]), ("fuller", "none", [0xFF, 0x7B, 0x1F, 0x1F]),
             ("sinclair1", "none", [0xFF, 0xFF, 0x0E, 0x1F]), ("sinclair2", "none", [0xFF, 0xFF, 0x1F, 0x0E]), ("cursor", "none", [0xFF, 0xFF, 0x1E, 0x0F]),
             ("kempston", "sinclair1", [0x12, 0xFF, 0x1D, 0x1F])]
    for model, rom in (("hc128", "hc128.rom"), ("hc2000", "hc2000-0.rom")):
        hc_rom(rom)
        for first, second, want in cases:
            config = hcp_config(cwd, f"{model}-{first}-{second}", model, joystick=first, joystick2=second)
            held = ["joy 0 left fire"] + (["joy 1 up"] if second != "none" else [])
            answers = hcp_run_usr(config, model, HC_STICK_PORTS, held + ["frames 5", "peek 9000 4"])
            seen = list(peeked(answers[-1]))
            seen[2] &= 0x1F
            seen[3] &= 0x1F
            need(seen == want, f"{model}, joysticks {first} and {second}: ports 1F, 7F, EFFE, F7FE read {bytes(seen).hex(' ')}, expected {bytes(want).hex(' ')}")


def check_hc2000_ay_timing() -> None:
    """The HC-2000's AY sound board option (the expansion connector's: FFFDh and BFFDh, the low byte FDh decoded) and Spectrum timing.
    (a) With the AY mono a program writes R0, R7 and R8 and reads them back through FFFDh (0Fh, 3Eh, 10h at 9100h) and a recording hears
    the tone; with the option off the ports read FFh and the recording is silent. (b) A loop of INC HL; JR back counts its turns between
    two frame interrupts, at 6400h (contended: the Sinclair 48K's ULA holds it as the documented pattern says, with Spectrum timing
    only) and at A400h (not): the frame is the 48K's 69888 T-states whatever the option."""
    hcp_core()
    hc_rom("hc2000-0.rom")
    cwd = folder("hc2000_ay_timing")
    ay = bytes([0xF3]) + HC128_PAGES[HC128_PAGES.index(bytes([0x01, 0xFD, 0xFF])):]
    for switch, want in (("mono", [0x0F, 0x3E, 0x10]), ("off", [0xFF, 0xFF, 0xFF])):
        wav = cwd / f"ay-{switch}.wav"
        answers = hcp_run_usr(hcp_config(cwd, f"ay-{switch}", "hc2000", ay=switch), "hc2000", ay, ["frames 30", "peek 910A 3", "wait 0.3"], "--wav", str(wav))
        seen = list(peeked(answers[-2]))
        need(seen == want, f"AY {switch}: R8, R7 and R0 read back {bytes(seen).hex(' ')}, expected {bytes(want).hex(' ')}")
        with wave.open(str(wav)) as w:
            samples = struct.unpack(f"<{w.getnframes() * w.getnchannels()}h", w.readframes(w.getnframes()))
        loud = max(samples) > 2000 and min(samples) < -2000
        need(loud == (switch == "mono"), f"AY {switch}: the recording's extremes are {max(samples)}, {min(samples)}")
    measured = {}
    for timing in ("original", "spectrum"):
        for loop in (0x6400, 0xA400):
            h1 = USR + 19
            h2 = h1 + 13
            handler = bytes([0x21, h2 & 0xFF, h2 >> 8, 0x22, 0xFF, 0x80, 0x21, 0, 0, 0xFB, 0xC3, loop & 0xFF, loop >> 8])
            program = {USR: usr_start(h1) + handler + bytes([0x22, 0xF2, 0x81]) + USR_END, loop: bytes([0x23, 0x18, 0xFD])}
            pokes = [c for address, code in program.items() for c in poke_commands(address, code)]
            answers = hc_run(hcp_config(cwd, f"timing-{timing}", "hc2000", timing=timing), hc2000_start() + pokes + usr_commands(USR) + ["frames 10", "peek 81F2 2"])
            low, high = peeked(answers[-1])
            measured[timing, loop] = low | high << 8

    def turns(loop: int, contended: bool) -> set[int]:
        counts = set()
        for j in range(4):
            t, n = j + 69, 0

            def cycle(t: int, address: int, length: int) -> int:
                return t + (sinclair_delay(t) if contended and 0x4000 <= address < 0x8000 else 0) + length
            while True:
                for address, length in ((loop, 4), (0x8000, 1), (0x8000, 1)):
                    t = cycle(t, address, length)
                n += 1
                if t >= SINCLAIR_FRAME:
                    break
                for address, length in ((loop + 1, 4), (loop + 2, 3), *((loop + 2, 1),) * 5):
                    t = cycle(t, address, length)
                if t >= SINCLAIR_FRAME:
                    break
            counts.add(n)
        return counts
    plain, slow = turns(0x6400, False), turns(0x6400, True)
    need(len(plain) == 1 and plain != slow, f"the loop's turns do not tell contention apart: {plain}, {slow}")
    for key in (("original", 0x6400), ("original", 0xA400), ("spectrum", 0xA400)):
        need(measured[key] in plain, f"{key[0]} timing, loop at {key[1]:04X}h: {measured[key]} turns, the documents give {plain}")
    need(measured["spectrum", 0x6400] in slow, f"Spectrum timing, loop at 6400h: {measured['spectrum', 0x6400]} turns, the documents give {slow}")


def check_hc128_interface1() -> None:
    """The HC-91 Plus with Interface 1 (the core's HC-91 CP/M disk). (a) The 128 editor ROM does not page the interface in at its startup
    instruction at 0009h: the hc128-basic case runs as without it (PRINT 2+2: 4). (b) From the 48 BASIC, a DOS command (CAT 1) pages the
    interface's EPROM in at 0000h (its first bytes D7 18 00 C9). (c) Startup: CP/M (and, with Interface 1 off, not
    BREAK) starts CP/M: its banner, A> and DIR listing the system disk's files, as hc_cpm does for the HC-91; BREAK (CAPS SHIFT and SPACE)
    held across a reset starts it too, and across the window's reset (Ctrl+F12). (d) A state saved in the middle of DIR (the 8272 executing, the interface's latches) continues to the
    same screen in the same run and in a new one."""
    main, core = hcp_core()
    hc_rom("hc128.rom")
    cwd = folder("hc128_interface1")
    disk = cwd / "system.dsk"
    shutil.copyfile(core / "assets" / "disks" / "hc91" / "system.dsk", disk)
    # (a)
    config = hcp_config(cwd, "basic", "hc128", interface1="on")
    answers = hc_run(config, ["until 10 Tape Loader", "frames 30"] + hcp_script(core, "hc128-basic", {"basic_ready": ["frames 20"]}) + ["text"])
    need(screen_has(answers[-1]["text"], r"^4$") and screen_has(answers[-1]["text"], r"0 OK"), f"PRINT 2+2 in 128 BASIC with Interface 1: {answers[-1]['text']!r}")
    # (b) the interface pages in at the 48 BASIC's RST 8 (the firmware writes the control latch at 07h, which 7FFD's decode does not answer)
    cat = key_commands(["CAPS SHIFT", "SYMBOL SHIFT"]) + key_commands(["SYMBOL SHIFT", "9"]) + ["type 1", "enter"]
    session = Session(main, "--config", str(config))
    try:
        session(f"insert a {disk}")
        session.run(hc128_start(3) + cat)
        for _ in range(400):
            if session("peek 0 4")["data"] == "D7 18 00 C9":          # Interface 1's EPROM at 0000h
                break
            session("wait 0.01")
        else:
            raise Failed("Interface 1 was not paged in by CAT 1 in the 48 BASIC")
    finally:
        session.close()
    # (c)
    waits = {"cpm_ready": ["frames 300"]}
    for name, options, how in (("startup", {"interface1": "on", "startup": "cpm"}, ["reset power"]),
                               ("break", {"interface1": "on"}, ["frames 100", "key CAPS SHIFT", "key SPACE", "reset keys", "frames 5", "release SPACE", "release CAPS SHIFT"])):
        config = hcp_config(cwd, name, "hc128", **options)
        steps = hcp_script(core, "cpm-dir", waits)
        answers = hc_run(config, [f"insert a {disk}"] + how + ["frames 300", "text"] + steps + ["text"])
        banner, text = answers[len(how) + 2]["text"], answers[-1]["text"]
        need("CP/M" in banner and screen_has(banner, r"^A>"), f"{name}: no CP/M banner and prompt: {banner!r}")
        need("COM" in text and screen_has(text, r"^A>"), f"{name}: DIR does not list the system disk: {text!r}")
    # The window's reset (Ctrl+F12) keeps the keys the host holds (not Ctrl, which is the reset's own): Shift and Space held through it, by
    # host key events, start CP/M too; Shift alone does not.
    for name, keys, cpm in (("window", ["+Space@100"], True), ("window_shift", [], False)):
        config = hcp_config(cwd, name, "hc128", interface1="on")
        held = ["--hostkey", "+Left Shift@100", "--hostkey", "+Left Ctrl@101", "--hostkey", "ctrl+F12@102", "--hostkey", "-Left Ctrl@103",
                "--hostkey", "-Left Shift@130"] + [a for k in keys for a in ("--hostkey", k, "--hostkey", "-Space@130")]
        answers = hc_run(config, [f"insert a {disk}", "frames 150", "frames 300", "text"], *held)
        text = answers[-1]["text"]
        need(("CP/M" in text and screen_has(text, r"^A>")) == cpm, f"{name}: BREAK held through Ctrl+F12 (CP/M expected: {cpm}): {text!r}")
    config = hcp_config(cwd, "nointerface", "hc128")
    answers = hc_run(config, ["frames 100", "key CAPS SHIFT", "key SPACE", "reset keys", "frames 5", "release SPACE", "release CAPS SHIFT", "frames 100", "text"])
    need("Tape Loader" in answers[-1]["text"], f"BREAK at reset without Interface 1 must leave the menu: {answers[-1]['text']!r}")
    # (d)
    config = hcp_config(cwd, "state", "hc128", interface1="on", startup="cpm")
    states = [cwd / f"s{i}.zrs" for i in range(40)]
    boot = [f"insert a {disk}", "reset power", "until 30 A>", "frames 20"]
    answers = hc_run(config, boot + ["type dir", "enter"] + [c for s in states for c in ("frames 3", f"state save {s}")] + ["frames 300", "text"])
    end = answers[-1]["text"]
    busy = [s for s in states if state_chunks(s)["8272"][0][16] == 2 and state_chunks(s)["FDD "][0][1] == 1]
    need(len(busy) >= 3, f"no state of the 40 was saved with the 8272 executing and the motor on ({len(busy)})")
    middle = busy[len(busy) // 2]
    again = hc_run(config, boot + ["type dir", "enter", "frames 30", f"state load {middle}", "frames 300", "text"])
    need(again[-1]["text"].replace("?", "") == end.replace("?", ""), f"the state loaded in the same run ends on another screen: {again[-1]['text']!r}")
    fresh = hc_run(config, [f"insert a {disk}", f"state load {middle}", "frames 300", "text"])
    need(fresh[-1]["text"].replace("?", "") == end.replace("?", ""), f"the state loaded in a new run ends on another screen: {fresh[-1]['text']!r}")


def check_hc_model_groups() -> None:
    """Families of one name are one group: `zetro --list-models` gives the HC's boards (hc, hc128, hc2000) on one line, their models in the
    registry's order, and the model list of the setup screen lists them together, without headings."""
    main = main_checkout()
    run = zetro(main, "--list-models")
    need(run.returncode == 0, f"--list-models: exit {run.returncode}: {run.stderr.strip()}")
    lines = [l for l in run.stdout.splitlines() if l.startswith("HC:")]
    ids = re.findall(r" (hc[0-9a-z]*) [(]", lines[0]) if len(lines) == 1 else []
    need(ids[:5] == ["hc85", "hc90", "hc91", "hc128", "hc2000"], f"the HC models of --list-models: {run.stdout!r}")
    cwd = folder("hc_model_groups")
    config = cwd / "zetro.yaml"
    config.write_text("\n".join(["machine:", "  model: hc2000", "video:", "  look: plain", ""]), encoding="utf-8")
    answers = control(main, ["frames 4", "frames 6", "text", "quit"], "--config", str(config), "--setup-page", "machine", "--hostkey", "Return@2")
    rows = answers[2]["text"].splitlines()
    shown = [m.group(1) for r in rows[2:]
             for m in [re.search(r"  (HC|HC-85|HC-90|HC-91|HC-91 Plus [(]HC-128[)]|HC-2000)(?: +\S*)?$", r)] if m]
    need(shown == ["HC-85", "HC-90", "HC-91", "HC-91 Plus (HC-128)", "HC-2000"], f"the model list: {rows!r}")


# --- the HC family (src/core/machines/hc.c) and its Interface 1 (src/hw/if1.h) ---------------------------
#
# The HC core's own validation cases (MiSTerRO/src/core-hc/tools/test_family.py, test_expansion_runtime.py and the simulator
# scripts of core-hc/sim/scripts) replayed through the harness. A script's `basic_ready` and `cpm_ready` wait, as the
# simulator does, until the machine waits for a key again: here, until the screen shows BASIC's report or an empty
# edit line, or CP/M's prompt alone on its line.


def hc_core() -> tuple[Path, Path]:
    """The main checkout (its roms/) and the HC core; skipped without either."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-hc"
    if not all((main / "roms" / name).is_file() for name in ("hc85.rom", "hc90.rom", "hc91.rom", "if85.rom", "if90.rom", "hc-family-cpm.rom")):
        raise Skipped(f"no HC ROMs in {main / 'roms'} (python tools/fetch_roms.py)")
    if not (core / "sim" / "scripts").is_dir() or not (core / "assets" / "disks").is_dir():
        raise Skipped(f"no HC core with its scripts and media beside the repository ({core})")
    return main, core


class Session:
    """zetro on its control channel, a command at a time, for the cases that wait on what the screen shows."""

    def __init__(self, cwd: Path, *args: str) -> None:
        self.process = subprocess.Popen([str(EXE), "--hidden", "--control", "stdin", *args], cwd=cwd, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def __call__(self, command: str) -> dict:
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise Failed(f"zetro ended at {command!r}: {self.process.stderr.read() if self.process.stderr else ''}")
            if line.startswith("{"):
                answer = json.loads(line)
                need(answer.get("ok"), f"{command!r}: {answer}")
                return answer

    def run(self, commands: list) -> None:
        for command in commands:
            if isinstance(command, tuple):
                self.ready(command[1])
            else:
                self(command)

    def text(self) -> str:
        return self("text")["text"]

    def ready(self, kind: str, seconds: float = 180.0) -> str:
        """Runs until BASIC (its report, or an empty edit line with its cursor) or CP/M (the prompt alone) waits for a key."""
        waited = 0.0
        while waited < seconds:
            self("wait 0.25")
            waited += 0.25
            text = self.text()
            lines = [line for line in text.splitlines() if line.strip()]
            last = lines[-1].strip() if lines else ""
            if kind == "cpm" and re.fullmatch(r"[A-P]>.?", last):
                return text
            if kind == "basic" and (re.search(r", \d+:\d+$", last) or last in ("K", "L", "C") or "I.C.E. FELIX" in last):
                return text
        raise Failed(f"the machine did not wait for a key ({kind}) in {seconds:.0f} s: {self.text()!r}")

    def close(self) -> None:
        try:
            self("quit")
        except Failed:
            pass
        self.process.wait(timeout=120)


# PS/2 set 2 codes of the simulator's `press` and the bits of its `matrix` (keys_n: row * 5 + column, as the key table).
HC_PS2 = {0x16: "1", 0x1E: "2", 0x26: "3", 0x25: "4", 0x2E: "5", 0x36: "6", 0x3D: "7", 0x3E: "8", 0x46: "9", 0x45: "0",
          0x15: "Q", 0x1D: "W", 0x24: "E", 0x2D: "R", 0x2C: "T", 0x35: "Y", 0x3C: "U", 0x43: "I", 0x44: "O", 0x4D: "P",
          0x1C: "A", 0x1B: "S", 0x23: "D", 0x2B: "F", 0x34: "G", 0x33: "H", 0x3B: "J", 0x42: "K", 0x4B: "L", 0x5A: "ENTER",
          0x1A: "Z", 0x22: "X", 0x21: "C", 0x2A: "V", 0x32: "B", 0x31: "N", 0x3A: "M", 0x29: "SPACE"}
HC_MATRIX = ["CAPS SHIFT", "Z", "X", "C", "V", "A", "S", "D", "F", "G", "Q", "W", "E", "R", "T", "1", "2", "3", "4", "5",
             "0", "9", "8", "7", "6", "P", "O", "I", "U", "Y", "ENTER", "L", "K", "J", "H", "SPACE", "SYMBOL SHIFT", "M", "N", "B"]


def hc_press(names: list[str], seconds: float) -> list[str]:
    return [f"key {n}" for n in names] + [f"wait {seconds}"] + [f"release {n}" for n in names] + ["wait 0.1"]


def hc_script(path: Path) -> list:
    """A simulator script as harness commands; ("ready", "basic" or "cpm") where it waits for the machine."""
    steps: list = []
    words = path.read_text(encoding="utf-8").split()
    i = 0
    while i < len(words):
        op = words[i]
        if op in ("basic_ready", "cpm_ready"):
            steps.append(("ready", op[:-6]))
            i += 1
        elif op == "run":
            steps.append(f"wait {words[i + 1]}")
            i += 2
        elif op == "press":
            steps += hc_press([HC_PS2[int(words[i + 1], 16)]], 0.08)
            i += 2
        elif op == "matrix":
            mask = int(words[i + 1], 16)
            steps += hc_press([HC_MATRIX[b] for b in range(40) if mask >> b & 1], float(words[i + 2]))
            i += 3
        else:
            raise Failed(f"{path.name}: unknown operation {op}")
    return steps


def hc_config(cwd: Path, name: str, model: str, **options: str) -> str:
    lines = "".join(f"    {key}: {value}\n" for key, value in options.items())
    (cwd / name).write_text(f"machine:\n  model: {model}\n" + (f"  options:\n{lines}" if lines else ""), encoding="utf-8")
    return str(cwd / name)


HC_BANNERS = {"hc85": "HC - 85", "hc90": "HC", "hc91": "HC"}


def check_hc_basic() -> None:
    """Each of the HC-85, HC-90 and HC-91 starts its own BASIC and shows 4 for PRINT 2+2 (the core's family case), with
    Interface 1 off and on."""
    main, _ = hc_core()
    cwd = folder("hc_basic")
    for model in ("hc85", "hc90", "hc91"):
        for if1 in ("off", "on"):
            config = hc_config(cwd, f"{model}-{if1}.yaml", model, interface1=if1)
            answers = control(main, ["until 5 I.C.E. FELIX", "type p2+2", "enter", "frames 25", "text", "quit"], "--config", config)
            need(answers[0]["ok"], f"{model}, Interface 1 {if1}: BASIC did not start: {answers[0]}")
            lines = [l.strip() for l in answers[4]["text"].splitlines()]
            need(lines and lines[0] == "4" and any(re.search(r"0 OK, 0:1", l) for l in lines),
                 f"{model}, Interface 1 {if1}: PRINT 2+2: {answers[4]['text']!r}")


def check_hc_tapes() -> None:
    """Each model loads its demonstration tape (the core's demo.tzx: a BASIC program that loads its picture) with the ROM
    tape traps on and off, to the same screen."""
    main, core = hc_core()
    cwd = folder("hc_tapes")
    for model in ("hc85", "hc90", "hc91"):
        tape = (core / "assets" / "tapes" / model / "demo.tzx").resolve()
        need(tape.is_file(), f"no {tape}")
        screens = {}
        for traps in ("on", "off"):
            config = hc_config(cwd, f"{model}-{traps}.yaml", model, tape_traps=traps)
            answers = control(main, ["until 5 I.C.E. FELIX", "type j\"\"", "enter", f"play {tape}", "until 300 CLAVIATURA",
                                     "wait 10", "text", "quit"], "--config", config)
            need(answers[4]["ok"], f"{model}, traps {traps}: the demonstration did not load: {answers[4]}")
            screens[traps] = answers[6]["text"]
        need("CONECTOR INTERFATA" in screens["on"], f"{model}: the demonstration's picture: {screens['on']!r}")
        need(screens["on"] == screens["off"], f"{model}: the loads with traps on and off end on other screens: "
                                              f"{screens['on']!r} and {screens['off']!r}")


def check_hc_dos() -> None:
    """The core's hc85-dos case: the HC-85 with Interface 1 formats a copy of blank.dsk (FORMAT "d";1), lists it, saves a
    program to it, clears it with NEW, loads it back and RUN shows 42. A state saved in the middle of the LOAD, with the
    disk in the drive and Interface 1 paged in, continues to the same screen. The image changes on the host and opens
    again in a new run, its catalogue naming the program."""
    main, core = hc_core()
    cwd = folder("hc_dos")
    blank = core / "assets" / "disks" / "hc85" / "blank.dsk"
    disk = cwd / "dos.dsk"
    shutil.copyfile(blank, disk)
    state = cwd / "load.zrs"
    config = hc_config(cwd, "hc85.yaml", "hc85", interface1="on")
    steps = hc_script(core / "sim" / "scripts" / "hc85-dos.txt")
    # The LOAD: the last `press 3b` (J, LOAD in keyword mode); the state is saved once its command runs in Interface 1.
    load = max(i for i, s in enumerate(steps) if s == "key J")
    enter = next(i for i in range(load, len(steps)) if steps[i] == "release ENTER")
    session = Session(main, "--config", config)
    try:
        session(f"insert a {disk}")
        session("wait 3")
        session.run(steps[:enter + 1])
        for _ in range(400):
            session("wait 0.01")
            if session("peek 0 4")["data"] == "D7 18 00 C9":          # Interface 1's EPROM at 0000h
                break
        else:
            raise Failed("Interface 1 was not paged in during the LOAD")
        session(f"state save {state}")
        rest = steps[enter + 1:]
        session.run(rest)
        first = session.text()
        session(f"state load {state}")
        session.run(rest)
        second = session.text()
    finally:
        session.close()
    need(any(line.strip() == "42" for line in first.splitlines()), f"RUN after the disk LOAD does not show 42: {first!r}")
    need(second == first, f"the state saved in the LOAD ends elsewhere: {second!r} for {first!r}")
    data = disk.read_bytes()
    need(data != blank.read_bytes() and data.startswith(b"EXTENDED CPC DSK File"), "the disk image did not change on the host")
    answers = control(main, [f"insert a {disk}", "wait 3", *hc_press(["CAPS SHIFT", "SYMBOL SHIFT"], 0.08), *hc_press(["SYMBOL SHIFT", "9"], 0.08),
                             "type 1", "enter", "wait 5", "text", "quit"], "--config", config)
    need(answers[-2]["ok"] and "check" in answers[-2]["text"], f"the image does not open again with the program: {answers[-2]}")


def check_hc_cpm() -> None:
    """The core's cpm-dir case: CP/M starts from a copy of the HC-91 system disk (the HC-90 with Startup at CP/M, the HC-91
    through its BASIC's PRINT USR 14446) and DIR lists its files. On each, a state saved in the middle of DIR continues to
    the same screen."""
    main, core = hc_core()
    cwd = folder("hc_cpm")
    steps = hc_script(core / "sim" / "scripts" / "cpm-dir.txt")
    usr = ["wait 3", *hc_press(["P"], 0.08), *hc_press(["CAPS SHIFT", "SYMBOL SHIFT"], 0.08), *hc_press(["L"], 0.08), "type 14446", "enter"]
    for model, start, how in (("hc90", "cpm", ["wait 0.01"]), ("hc91", "basic", usr)):
        disk = cwd / f"{model}.dsk"
        shutil.copyfile(core / "assets" / "disks" / "hc91" / "system.dsk", disk)
        state = cwd / f"{model}.zrs"
        config = hc_config(cwd, f"{model}.yaml", model, interface1="on", startup=start)
        session = Session(main, "--config", config)
        try:
            session(f"insert a {disk}")
            session.run(how)
            boot = session.ready("cpm", 40.0)
            enter = steps.index("release ENTER")
            session.run(steps[:enter + 1])
            session("wait 0.05")
            session(f"state save {state}")
            session.run(steps[enter + 1:])
            first = session.text()
            session(f"state load {state}")
            session.run(steps[enter + 1:])
            second = session.text()
        finally:
            session.close()
        need("CP/M" in boot or "CP/M" in first, f"{model}: no CP/M banner: {boot!r}")
        need(re.search(r"\bCOM\b", first) and "A>" in first, f"{model}: DIR does not list the system disk: {first!r}")
        need(second == first, f"{model}: the state saved in DIR ends elsewhere: {second!r} for {first!r}")

HC_EXTENDED = ["key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3"]


def hc_serial_open(rate: str | None = None) -> list[str]:
    """48 BASIC with Interface 1: FORMAT "t";RATE when a rate is given (extended mode, SYMBOL SHIFT and 0), then OPEN #4;"t" (SYMBOL SHIFT and 4)."""
    format_rate = HC_EXTENDED + key_commands(["SYMBOL SHIFT", "0"]) + [f'type "t";{rate}', "enter", "frames 100"] if rate else []
    return format_rate + HC_EXTENDED + key_commands(["SYMBOL SHIFT", "4"]) + ['type 4;"t"', "enter", "frames 100"]


def check_hc_serial() -> None:
    """The HC's Interface 1 serial port, as the firmware drives it (the board takes the TX line's bits into bytes at the Serial speed option's
    rate, and plays received bytes on the RX line while the firmware listens). On the HC-91: OPEN #4;"t" and PRINT #4;"H" put H, CR and LF in
    the file of the serial end at 9600 baud; the same at 1200 (FORMAT "t";1200, with the option) for "Hi"; with a loopback end PRINT #4;"Hi"
    and INPUT #4;a$ give the string back; and with a TCP end the host's "Yo" is read by INPUT #4;a$ while the guest's text reaches the host.
    The HC-91 Plus (the same board) sends its H from 48 BASIC."""
    main, core = hc_core()
    hc_rom("hc128.rom")
    cwd = folder("hc_serial")
    boot = ["until 5 I.C.E. FELIX", "frames 30"]

    def sent(name: str, model: str, rate: str | None, text: str, start: list[str] | None = None) -> bytes:
        options = {"interface1": "on"} | ({"serial_baud": rate} if rate else {})
        config = with_ports(hcp_config(cwd, name, model, **options), serial=f"file:{name}.out")
        hc_run(config, (start or boot) + hc_serial_open(rate) + [f'type p#4;"{text}"', "enter", "frames 300"])
        return (cwd / f"{name}.out").read_bytes()

    got = sent("hc91", "hc91", None, "H")
    need(got == b"H\r\n", f"PRINT #4;\"H\" on the HC-91 at 9600 baud: {got.hex(' ')}")
    got = sent("hc91_1200", "hc91", "1200", "Hi")
    need(got == b"Hi\r\n", f"PRINT #4;\"Hi\" on the HC-91 at 1200 baud: {got.hex(' ')}")
    got = sent("hc128", "hc128", None, "H", hc128_start(3))
    need(got == b"H\r\n", f"PRINT #4;\"H\" on the HC-91 Plus: {got.hex(' ')}")
    # A loopback plug: what PRINT # sends INPUT # reads.
    config = with_ports(hcp_config(cwd, "loop", "hc91", interface1="on"), serial="loopback")
    answers = hc_run(config, boot + hc_serial_open() + ['type p#4;"Hi"', "enter", "frames 200", "type i#4;a$", "enter", "frames 400", "type pa$", "enter", "frames 30", "text"])
    need(screen_has(answers[-1]["text"], r"^Hi$"), f"INPUT #4 through the loopback: {answers[-1]['text']!r}")
    # A TCP end: the host's bytes are read by INPUT #4, the guest's go to the socket.
    import socket
    port = free_port()
    config = with_ports(hcp_config(cwd, "tcp", "hc91", interface1="on"), serial=f"tcp-listen:{port}")
    session = PortSession(config, cwd / "stderr.txt")
    try:
        for command in boot + hc_serial_open() + ["type i#4;a$", "enter", "frames 50"]:
            session.send(command)
        with socket.create_connection(("127.0.0.1", port), timeout=10) as peer:
            peer.settimeout(10)
            session.send("frames 5")
            peer.sendall(b"Yo\r")
            for command in ["frames 400", 'type p#4;"Hi"', "enter", "frames 200"]:
                session.send(command)
            heard = b""
            while len(heard) < 4:
                heard += peer.recv(16)
            need(heard == b"Hi\r\n", f"what the HC-91 sent to the socket: {heard.hex(' ')}")
            for command in ["type pa$", "enter", "frames 30"]:
                session.send(command)
            need(screen_has(session.send("text")["text"], r"^Yo$"), "INPUT #4 did not read the host's Yo")
    finally:
        session.close()


def check_hc_states() -> None:
    """A state round trip on the HC-90 and the HC-91 with a disk in drive A and Interface 1 paged in (its DOS running a
    CAT 1 of the CP/M system disk): the state loaded back continues to the same screen. (The HC-85's is in hc_dos.)"""
    main, core = hc_core()
    cwd = folder("hc_states")
    cat = [*hc_press(["CAPS SHIFT", "SYMBOL SHIFT"], 0.08), *hc_press(["SYMBOL SHIFT", "9"], 0.08), "type 1", "enter"]
    for model in ("hc90", "hc91"):
        disk = cwd / f"{model}.dsk"
        shutil.copyfile(core / "assets" / "disks" / "hc91" / "system.dsk", disk)
        state = cwd / f"{model}.zrs"
        session = Session(main, "--config", hc_config(cwd, f"{model}.yaml", model, interface1="on"))
        try:
            session(f"insert a {disk}")
            session("wait 3")
            session.run(cat)
            for _ in range(400):
                if session("peek 0 4")["data"] == "D7 18 00 C9":          # Interface 1's EPROM at 0000h
                    break
                session("wait 0.01")
            else:
                raise Failed(f"{model}: Interface 1 was not paged in by CAT 1")
            session(f"state save {state}")
            first = session.ready("basic")
            session(f"state load {state}")
            second = session.ready("basic")
        finally:
            session.close()
        need(second == first, f"{model}: the state saved in CAT 1 ends elsewhere: {second!r} for {first!r}")


def check_hc_sticks_ay() -> None:
    """The shared core's joystick kinds and AY board, on the HC-85: a program reads the AY's register 7 back through
    FFFDh after writing it through BFFDh, then ports 1Fh (Kempston), 7Fh (Fuller) and the matrix rows of 1-5 and 6-0 while
    the harness holds both joysticks up and fire (from the second after the command, which it waits)."""
    main, _ = hc_core()
    cwd = folder("hc_sticks_ay")
    program = bytes([0x06, 0x32, 0xFB, 0x76, 0x10, 0xFC, 0x01, 0xFD, 0xFF, 0x3E, 0x07, 0xED, 0x79, 0x06, 0xBF, 0x3E, 0x38, 0xED, 0x79, 0x06, 0xFF, 0xED, 0x78,
                     0x32, 0x00, 0x91, 0xDB, 0x1F, 0x32, 0x01, 0x91, 0x3E, 0x00, 0xDB, 0x7F, 0x32, 0x02, 0x91,
                     0x3E, 0xF7, 0xDB, 0xFE, 0x32, 0x03, 0x91, 0x3E, 0xEF, 0xDB, 0xFE, 0x32, 0x04, 0x91, 0xC9])
    usr = ["until 5 I.C.E. FELIX", "poke 9000 " + " ".join(f"{b:02X}" for b in program), "type t",
           *hc_press(["CAPS SHIFT", "SYMBOL SHIFT"], 0.06), "type l36864", "enter", "joy 0 up fire", "joy 1 up fire", "frames 80", "peek 9100 5", "quit"]
    for sticks, ay, want in ((("kempston", "fuller"), "abc", ["38", "18", "7E"]), (("sinclair2", "sinclair1"), "off", ["FF", None, None])):
        config = hc_config(cwd, f"{sticks[0]}.yaml", "hc85", joystick=sticks[0], joystick2=sticks[1], ay=ay)
        answers = control(main, usr, "--config", config)
        need(all(a["ok"] for a in answers), f"{sticks}: {[a for a in answers if not a['ok']]}")
        got = answers[-2]["data"].split()
        for i, value in enumerate(want):
            need(value is None or got[i] == value, f"{sticks}, AY {ay}: {got} (AY register 7, 1Fh, 7Fh) for {want}")
        rows = [int(b, 16) & 0x1F for b in got[3:5]]
        if sticks[0] == "sinclair2":
            # 1-5: up is 4 (bit 3), fire 5 (bit 4); 6-0: up is 9 (bit 1), fire 0 (bit 0).
            need(rows == [0x07, 0x1C], f"the Sinclair joysticks on the matrix: {[f'{r:02X}' for r in rows]}")
        else:
            need(rows == [0x1F, 0x1F], f"the Kempston and Fuller joysticks touched the matrix: {[f'{r:02X}' for r in rows]}")



# --- the HC-88 (P6c) ----------------------------------------------------------------------------------


HC88_DIR = ["A: DRIVES   COM : TRACK    COM : WRITE    COM : BASIC    COM", "A: DEMO     COM : README   TXT : HELLO    TXT"]


def hc88_case() -> tuple[Path, Path]:
    """The HC-88's validation case from its core: the main checkout (roms/hc88.rom) and the core's replacement system disk
    (core-hc/assets/disks/hc88/system.dsk). Skipped without the ROM or the MiSTerRO checkout."""
    main = main_checkout()
    disk = main.parent / "MiSTerRO" / "src" / "core-hc" / "assets" / "disks" / "hc88" / "system.dsk"
    if not (main / "roms" / "hc88.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'hc88.rom'} (python tools/fetch_roms.py)")
    if not disk.is_file():
        raise Skipped(f"no HC-88 core with its system disk beside the repository ({disk})")
    return main, disk


def hc88_folder(name: str, disk: Path) -> Path:
    """A work folder with a configuration of the HC-88 and its own copy of the system disk in drive A."""
    cwd = folder(name)
    shutil.copyfile(disk, cwd / "system.dsk")
    (cwd / "zetro.yaml").write_text(f"machine:\n  model: hc88\nmedia:\n  a: {{ image: {(cwd / 'system.dsk').as_posix()} }}\n", encoding="utf-8")
    return cwd


def hc88_run(main: Path, cwd: Path, commands: list[str]) -> list[dict]:
    """The commands on the control channel, from the main checkout (its roms/ is found); every file named is in cwd."""
    answers = control(main, commands + ["quit"], "--config", str(cwd / "zetro.yaml"))[:-1]
    for command, answer in zip(commands, answers):
        need(answer.get("ok"), f"{command!r}: {answer}")
    return answers


def hc88_command(line: str, expect: str, seconds: int = 90) -> list[str]:
    """A CP/M command typed and waited for; CP/M then warm boots (the replacement BIOS reads the CCP back from the slow
    disk), which takes a few seconds before it takes a key again."""
    return [f"type {line}\\n", f"until {seconds} {expect}", "wait 8"]


def check_hc88() -> None:
    """The HC-88 boots CP/M through its bootstrap ROM and the floppy controller from the core's replacement system disk
    and replays the core's validation: DIR, DRIVES, TYPE, TRACK (cylinder 79), WRITE (a file created, closed, opened and read
    back through BDOS); the disk image on the host has changed, its reserved tracks have not, and a fresh run lists and types
    the file it wrote; the reset button (the core's F11) boots again from the disk."""
    main, disk = hc88_case()
    cwd = hc88_folder("hc88", disk)
    commands = ["until 60 A>"] + hc88_command("dir", "HELLO    TXT") + ["text"]
    commands += hc88_command("drives", "B: not ready") + hc88_command("type hello.txt", "native floppy read test OK")
    commands += hc88_command("track", "PASS: native cylinder 79 seek/read") + hc88_command("write", "PASS: native BDOS create/write/close/read/compare")
    commands += hc88_command("type check.txt", "HC-88 persistent floppy write verified.", 60)
    commands += ["reset", "until 60 A>", "wait 4", "type type check.txt\\n", "wait 12", "text"]
    answers = hc88_run(main, cwd, commands)
    lines = next(a for c, a in zip(commands, answers) if c == "text")["text"].splitlines()
    for expected in HC88_DIR:
        need(expected in lines, f"{expected!r} is not in the directory listing: {lines!r}")
    after = answers[-1]["text"]
    need("HC-88 persistent floppy write verified." in after, f"the file after the reset: {after!r}")
    original = disk.read_bytes()
    written = (cwd / "system.dsk").read_bytes()
    need(written != original and len(written) == len(original), "the written disk image has not changed on the host (or has changed size)")
    need(written[:39168] == original[:39168], "the system tracks of the disk changed")
    need(written[:34] == b"EXTENDED CPC DSK File\r\nDisk-Info\r\n", "the disk image is no longer an extended DSK")
    again = hc88_run(main, cwd, ["until 60 A>"] + hc88_command("dir", "CHECK    TXT") + hc88_command("type check.txt", "HC-88 persistent floppy write verified.", 60) + ["text"])
    need("HC-88 persistent floppy write verified." in again[-1]["text"], f"the written file reads back in a fresh run: {again[-1]['text']!r}")


def png_colours(path: Path) -> tuple[int, int, int]:
    """The width, the height and the number of different colours of a PNG written by zetro."""
    rows = png_rows(path)
    return len(rows[0]), len(rows), len({p for row in rows for p in row})


def hc88_tape() -> bytes:
    """The core's EAR test program (10 PRINT 7319, started at line 10, named HC88 TEST) as two standard-speed blocks: the core's own
    cassette sends the header with a 1024-pulse leader, which the ROM's loader (a 1 s wait, then 256 pulses) does not take."""
    body = b"\xf57319\x0e\x00\x00\x97\x1c\x00\x0d"
    program = b"\x00\x0a" + len(body).to_bytes(2, "little") + body
    header = b"\x00" + b"HC88 TEST " + struct.pack("<HHH", len(program), 10, len(program))

    def block(flag: int, data: bytes) -> bytes:
        raw = bytes([flag]) + data
        parity = 0
        for b in raw:
            parity ^= b
        raw += bytes([parity])
        return b"\x10" + struct.pack("<HH", 1000, len(raw)) + raw

    return b"ZXTape!\x1a\x01\x14" + block(0, header) + block(255, program)


def check_hc88_video() -> None:
    """Both video modes are seen: the demo's 512-pixel colour bars (BRIGHT, FLASH, the speaker) over CP/M's 80-column terminal, and
    the 256-pixel screen of the HC-85 BASIC that BASIC.COM loads, which prints 4 for PRINT 2+2 and loads a program from a tape
    through EAR; every frame is 704 x 288. The screenshots are work/verify/hc88_video/mode512.png (the demo), terminal.png (CP/M's 80 columns) and mode256.png (BASIC)."""
    main, disk = hc88_case()
    cwd = hc88_folder("hc88_video", disk)
    tape = cwd / "program.tzx"
    tape.write_bytes(hc88_tape())
    commands = ["until 60 A>", "type demo\\n", "until 90 Press a key to return to CP/M.", "wait 8", f"shot {cwd / 'mode512.png'}", "text"]
    commands += ["key SPACE", "wait 0.2", "release SPACE", "wait 12", "text", f"shot {cwd / 'terminal.png'}"]
    commands += ["type basic\\n", "until 120 FELIX", "wait 3", "type p2+2\\n", "wait 2", "text", f"shot {cwd / 'mode256.png'}"]
    commands += ['type j""\\n', "wait 1", f"play {tape}", "wait 20", "text"]
    answers = hc88_run(main, cwd, commands)
    width, height, colours = png_colours(cwd / "mode512.png")
    need((width, height) == (704, 288) and colours >= 8, f"the 512-pixel frame: {width} x {height}, {colours} colours")
    width, height, colours = png_colours(cwd / "mode256.png")
    need((width, height) == (704, 288) and 2 <= colours <= 4, f"the 256-pixel frame: {width} x {height}, {colours} colours")
    texts = [a["text"] for c, a in zip(commands, answers) if c == "text"]
    need("Press a key to return to CP/M." in texts[0] and "A>" in texts[1], f"the demo and the return to CP/M: {texts[:2]!r}")
    need(any(l.strip() == "4" for l in texts[2].splitlines()) and "0 OK, 0:1" in texts[2], f"PRINT 2+2: {texts[2]!r}")
    need("Program: HC88 TEST" in texts[3] and any(l.strip() == "7319" for l in texts[3].splitlines()), f"the program loaded from the tape: {texts[3]!r}")


def check_hc88_state() -> None:
    """A state saved in the middle of a disk operation (the controller is busy reading the directory, which the trace shows) puts
    the machine back: the same run goes on from it to the directory listing it reached without the interruption, and so does a
    fresh run that has only the disk in drive A."""
    main, disk = hc88_case()
    cwd = hc88_folder("hc88_state", disk)
    state = cwd / "mid.zrs"
    trace = cwd / "trace.txt"
    start = ["until 60 A>", "type dir\\n", "wait 0.4", f"state save {state}", f"trace {trace}", "wait 0.05", "trace off"]
    first = hc88_run(main, cwd, start + ["until 90 HELLO    TXT", "text", f"state load {state}", "until 90 HELLO    TXT", "text"])
    busy = [int(l.split()[-1], 16) for l in trace.read_text().splitlines() if l.startswith("IN ") and l.split()[1].endswith("DD")]
    need(busy and any(v & 0x10 for v in busy), f"the controller was not in a command when the state was saved: {busy[:8]}")
    need(first[-1]["text"] == first[-4]["text"], f"the state loaded back ends elsewhere: {first[-1]['text']!r} {first[-4]['text']!r}")
    for expected in HC88_DIR:
        need(expected in first[-1]["text"].splitlines(), f"{expected!r} is not in the listing after the state: {first[-1]['text']!r}")
    fresh = hc88_run(main, cwd, [f"state load {state}", "until 90 HELLO    TXT", "text"])
    need(fresh[-1]["text"] == first[-1]["text"], f"the state in a fresh run: {fresh[-1]['text']!r}")


# --- the CIP family (CIP-01, CIP-02, CIP-03: src/core/machines/cip.c) ------------------------------------------------------


def cip_core() -> tuple[Path, Path]:
    """The main checkout (its roms/) and the CIP core beside it, with the tapes the cases use; skipped without either."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-cip"
    for name in ("cip01.rom", "cip03.rom"):
        if not (main / "roms" / name).is_file():
            raise Skipped(f"no {main / 'roms' / name} (python tools/fetch_roms.py)")
    for name in ("basic.tzx", "demo-presentation.tzx"):
        if not (core / "assets" / "tapes" / name).is_file():
            raise Skipped(f"no CIP core with its tapes beside the repository ({core / 'assets' / 'tapes' / name})")
    return main, core


def cip_config(cwd: Path, name: str, model: str, **options: str) -> str:
    lines = "".join(f"    {key}: {value}\n" for key, value in options.items())
    (cwd / f"{name}.yaml").write_text(f"machine:\n  model: {model}\n" + (f"  options:\n{lines}" if lines else "") + "video:\n  look: plain\n",
                                      encoding="utf-8")
    return str(cwd / f"{name}.yaml")


def cip_run(main: Path, config: str, commands: list[str]) -> list[dict]:
    """The commands on the control channel (in the main checkout, for its roms/); every answer but the last is ok."""
    answers = control(main, commands + ["quit"], "--config", config)
    failed = [(c, a) for c, a in zip(commands, answers) if not a.get("ok")]
    need(not failed, f"{failed[:2]}")
    return answers[:-1]


def cip_lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines()]


def cip_basic_payload(core: Path) -> bytes:
    """The BASIC the CIP-01 loads from tape: the CODE block of basic.tzx (a header and the data, standard blocks), 16,383 bytes."""
    data = (core / "assets" / "tapes" / "basic.tzx").read_bytes()
    pos, blocks = 10, []
    while pos < len(data):
        need(data[pos] == 0x10, "basic.tzx has a block that is not a standard-speed one")
        size = int.from_bytes(data[pos + 3:pos + 5], "little")
        blocks.append(data[pos + 5:pos + 5 + size])
        pos += 5 + size
    need(len(blocks) == 2 and blocks[1][0] == 0xFF and len(blocks[1]) == 16385, f"basic.tzx: {len(blocks)} blocks")
    return blocks[1][1:-1]


def cip_peek_commands(first: int, count: int) -> list[str]:
    return [f"peek {first + i:X} {min(0x400, count - i):X}" for i in range(0, count, 0x400)]


def cip_bytes(answers: list[dict]) -> bytes:
    return b"".join(bytes(int(b, 16) for b in a["data"].split()) for a in answers)


# The copier's keys: L, ENTER, ENTER launch from cassette (the core's cassette case), then the tape plays.
CIP_LAUNCH = ["wait 2", "type l", "enter", "enter", "wait 0.5"]


def cip_load_basic(core: Path) -> list[str]:
    """The CIP-01's copier started on its cassette launch and basic.tzx playing, until BASIC's copyright line is up."""
    return CIP_LAUNCH + [f"play {(core / 'assets' / 'tapes' / 'basic.tzx').resolve()}", "until 300 ELECTRONICA", "frames 25"]


def check_cip_basic() -> None:
    """The three models start what they have: the CIP-03 its BASIC (Enter at its Basic-S prompt, as its manual says) and PRINT 2+2
    shows 4; the CIP-02 the same, and with the Startup option at Copier the CIP-01's copier; the CIP-01 the copier (its
    menu read back as text, its code at 0000h)."""
    main, _ = cip_core()
    cwd = folder("cip_basic")
    print22 = ["until 10 Basic-S", "frames 20", "enter", "frames 20", "type p2+2", "enter", "frames 25", "text"]
    for model in ("cip03", "cip02"):
        text = cip_run(main, cip_config(cwd, model, model), print22 + ["peek 0 8"])
        lines = cip_lines(text[-2]["text"])
        need(lines[0] == "4" and "0 OK, 0:1" in lines, f"{model}: PRINT 2+2: {text[-2]['text']!r}")
        need(text[-1]["data"] == "F3 AF 11 FF FF C3 CB 11", f"{model}: BASIC is not at 0000h: {text[-1]['data']}")
    for model, options in (("cip01", {}), ("cip02", {"startup": "copier"})):
        answers = cip_run(main, cip_config(cwd, model + "-copier", model, **options), ["wait 3", "text", "peek 0 8", f"shot {cwd / (model + '.png')}"])
        for word in ("LANSEZ", "INCARC", "SALVEZ", "VERIFIC"):
            need(word in answers[1]["text"], f"{model}: {word} is not on the copier's screen: {answers[1]['text']!r}")
        need(answers[2]["data"] == "F3 31 EE 5C 21 04 00 44", f"{model}: the copier is not at 0000h: {answers[2]['data']}")
        need((cwd / (model + ".png")).stat().st_size > 500, "the shot")


def check_cip_tape_load() -> None:
    """The core's family case for the CIP-01: the copier launches from cassette (L, Enter, Enter), basic.tzx plays through port FE
    (loader acceleration on: seconds of host time for the two minutes of tape), all 16,383 bytes of BASIC arrive at 0000h
    unchanged, BASIC starts and PRINT 2+2 shows 4; a reset with "Boot latch on reset" at cleared (as the core does) is back at
    the copier, and with it kept (the board's, the default) BASIC restarts from the RAM."""
    main, core = cip_core()
    cwd = folder("cip_tape_load")
    payload = cip_basic_payload(core)
    need(len(payload) == 16383, f"{len(payload)} bytes of BASIC")
    config = cip_config(cwd, "cip01", "cip01", reset_latch="cleared")
    load = cip_load_basic(core)
    peeks = len(cip_peek_commands(0, 16383))
    start = time.time()
    answers = cip_run(main, config, load + cip_peek_commands(0, 16383) + ["type p2+2", "enter", "frames 25", "text", "reset", "wait 3", "text", "peek 0 4"])
    need(time.time() - start < 100, "loading BASIC took too long: the loader acceleration is not at work")
    loaded = cip_bytes(answers[len(load):len(load) + peeks])
    need(loaded == payload, f"BASIC arrived with {sum(a != b for a, b in zip(loaded, payload))} wrong bytes of {len(payload)}")
    tail = answers[len(load) + peeks:]
    need("4" in cip_lines(tail[3]["text"]), f"PRINT 2+2 on the CIP-01: {tail[3]['text']!r}")
    need("LANSEZ" in tail[6]["text"] and tail[7]["data"] == "F3 31 EE 5C", f"a reset did not bring the copier back: {tail[6]['text']!r} {tail[7]['data']}")
    # The board's RESET button alone leaves the latch (a new configuration's): BASIC runs again from the RAM.
    config = cip_config(cwd, "kept", "cip01")
    answers = cip_run(main, config, load + ["type 1", "reset", "wait 3", "text", "peek 0 4"])
    need("ELECTRONICA" in answers[-2]["text"] and answers[-1]["data"] == "F3 AF 11 FF", f"reset with the latch kept: {answers[-2]['text']!r} {answers[-1]['data']}")


# What the program below leaves at 9100h on a CIP-03: the byte of the BASIC ROM at 0100h (read before and after a write to
# it that went to the RAM under it), then the RAM's 5Ah once port EEh selects it, still 5Ah after a write to a port with A6
# low (not decoded), the ROM's byte after a write to the alias CEh (A6 high, A4 low, A0 low), FFh from a read of EEh, 0BFh
# (EAR and the keys' bits) from the alias DEh of port FEh and FFh from a port with A6 low.
CIP_PORTS = bytes([
    0xF3, 0x3A, 0x00, 0x01, 0x32, 0x00, 0x91,               # DI; LD A,(0100h); LD (9100h),A
    0x3E, 0x5A, 0x32, 0x00, 0x01,                           # LD A,5Ah; LD (0100h),A: under the ROM
    0x3A, 0x00, 0x01, 0x32, 0x01, 0x91,                     # LD A,(0100h): still the ROM; to 9101h
    0x3E, 0x01, 0xD3, 0xEE,                                 # LD A,1; OUT (0EEh),A: the RAM
    0x3A, 0x00, 0x01, 0x32, 0x02, 0x91,                     # 5Ah; to 9102h
    0x01, 0xAE, 0x00, 0xAF, 0xED, 0x79,                     # LD BC,00AEh; XOR A; OUT (C),A: A6 low, nothing decodes it
    0x3A, 0x00, 0x01, 0x32, 0x03, 0x91,                     # still 5Ah; to 9103h
    0x01, 0xCE, 0x00, 0xAF, 0xED, 0x79,                     # LD BC,00CEh; XOR A; OUT (C),A: an alias of EEh, D0 = 0
    0x3A, 0x00, 0x01, 0x32, 0x04, 0x91,                     # the ROM again; to 9104h
    0x3E, 0xFF, 0xDB, 0xEE, 0x32, 0x05, 0x91,               # LD A,0FFh; IN A,(0EEh); to 9105h
    0x3E, 0xFF, 0xDB, 0xDE, 0x32, 0x06, 0x91,               # IN A,(0DEh): the keyboard and EAR; to 9106h
    0x3E, 0xFF, 0xDB, 0xBE, 0x32, 0x07, 0x91,               # IN A,(0BEh): A6 low, nothing; to 9107h
    0xFB, 0xC9])                                            # EI; RET


def check_cip_ports() -> None:
    """Ports and memory: a program on the CIP-03 (run from BASIC by USR) writes under the ROM and reads the ROM, switches the boot
    latch through port EEh and its partial-decode alias, and reads FEh, its alias and an undecoded port; the RAM under the
    ROM is the page 1 of the family's peek."""
    main, _ = cip_core()
    cwd = folder("cip_ports")
    rom = (main / "roms" / "cip03.rom").read_bytes()
    usr = ["type t", "key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3", "type l36864", "enter"]
    commands = ["until 10 Basic-S", "frames 20", "enter", "frames 20", "poke 9000 " + " ".join(f"{b:02X}" for b in CIP_PORTS)] + usr + [
        "frames 30", "peek 9100 8", "peek 104100 1", "peek 100 1"]
    answers = cip_run(main, cip_config(cwd, "cip03", "cip03"), commands)
    got = bytes(int(b, 16) for b in answers[-3]["data"].split())
    want = bytes([rom[0x100], rom[0x100], 0x5A, 0x5A, rom[0x100], 0xFF, 0xBF, 0xFF])
    need(got == want, f"the ports: {got.hex(' ')} for {want.hex(' ')}")
    need(answers[-2]["data"] == "5A" and answers[-1]["data"] == f"{rom[0x100]:02X}", f"the RAM under the ROM: {answers[-2]['data']} {answers[-1]['data']}")


def check_cip_tapes() -> None:
    """The demonstration tape (the core's demo-presentation.tzx, a BASIC program with its picture) loads with the ROM tape traps on
    and off to the same screen: on the CIP-03 from its ROM and on the CIP-01 with the BASIC the tape brought into the RAM (the load
    trap finds the routine there)."""
    main, core = cip_core()
    cwd = folder("cip_tapes")
    demo = (core / "assets" / "tapes" / "demo-presentation.tzx").resolve()
    for model, start in (("cip03", ["until 10 Basic-S", "frames 20"]), ("cip01", cip_load_basic(core))):
        screens, frames = {}, {}
        for traps in ("on", "off"):
            answers = cip_run(main, cip_config(cwd, f"{model}-{traps}", model, tape_traps=traps),
                              start + ["enter", "frames 20", 'type j""', "enter", f"play {demo}", "until 900 apasa ENTER", "frames 500", "text"])
            screens[traps] = answers[-1]["text"]
            frames[traps] = answers[-3]["frame"]
        need("Salut prietene" in screens["on"] and "apasa ENTER" in screens["on"], f"{model}: the demonstration's screen: {screens['on']!r}")
        need(screens["on"] == screens["off"], f"{model}: the loads with traps on and off end on other screens: {screens['on']!r} and {screens['off']!r}")
        need(frames["on"] * 2 < frames["off"], f"{model}: the load trap did not speed the load up ({frames})")


def check_cip_states() -> None:
    """A state round trip on each model: the CIP-03 and the CIP-02 at BASIC's prompt (PRINT 2+2 typed after the load gives the same
    screen as after the first save), and the CIP-01 saved in the middle of BASIC's load from tape (the load then goes on from the
    state to a BASIC whose bytes are the tape's, in the same run and in a new one) and again with BASIC running (the boot latch is
    in the state: after a reset brings the copier back the state puts BASIC back)."""
    main, core = cip_core()
    cwd = folder("cip_states")
    prompt = ["until 10 Basic-S", "frames 20", "enter", "frames 20"]
    sum22 = ["type p2+2", "enter", "frames 25", "text"]
    for model in ("cip03", "cip02"):
        state = cwd / f"{model}.zrs"
        answers = cip_run(main, cip_config(cwd, model, model), prompt + [f"state save {state}"] + sum22 + [f"state load {state}"] + sum22)
        first, second = answers[len(prompt) + 4]["text"], answers[-1]["text"]
        need("4" in cip_lines(first) and second == first, f"{model}: the state saved at the prompt ends elsewhere: {second!r} for {first!r}")
    payload = cip_basic_payload(core)
    tape = (core / "assets" / "tapes" / "basic.tzx").resolve()
    mid, running = cwd / "mid.zrs", cwd / "running.zrs"
    peeks = len(cip_peek_commands(0, 16383))
    finish = ["until 300 ELECTRONICA", "frames 25"] + cip_peek_commands(0, 16383) + sum22
    config = cip_config(cwd, "cip01", "cip01", reset_latch="cleared")      # the reset after PRINT must come back to the copier
    head = CIP_LAUNCH + [f"play {tape}", "wait 40", f"state save {mid}"]
    back = [f"state save {running}", "reset", "wait 3", "text", f"state load {running}", "frames 5", "peek 0 4", "text"]
    first = cip_run(main, config, head + finish + back + [f"state load {mid}"] + finish)
    a = first[len(head):]
    need(cip_bytes(a[2:2 + peeks]) == payload, "the BASIC loaded in the run that saved the state differs from the tape's")
    screen = a[2 + peeks + 3]["text"]
    need("4" in cip_lines(screen), f"PRINT 2+2 after the load: {screen!r}")
    t = a[len(finish):len(finish) + len(back)]
    need("LANSEZ" in t[3]["text"], f"a reset did not bring the copier back: {t[3]['text']!r}")
    need(t[6]["data"] != "F3 31 EE 5C" and t[7]["text"] == screen, f"the state of BASIC running did not put it back: {t[6]['data']} {t[7]['text']!r}")
    # The state from the middle of the load, loaded in the same run (after the load had finished) and in a new run (the tape inserted
    # first, as the shell does): the load goes on to BASIC and the same screen.
    again = a[len(finish) + len(back) + 1:]
    need(cip_bytes(again[2:2 + peeks]) == payload and again[-1]["text"] == screen, f"the state loaded in the same run: {again[-1]['text']!r}")
    new = cip_run(main, config, [f"insert tape {tape}", f"state load {mid}"] + finish)
    need(cip_bytes(new[4:4 + peeks]) == payload and new[-1]["text"] == screen, f"the state in a new run: {new[-1]['text']!r}")


def check_cip_autoload() -> None:
    """A tape dropped on the CIP-03 restarts it, types Enter and LOAD "" and plays it (the demonstration's prompt comes up); on the CIP-01
    the copier's own launch keys (L, Enter, Enter) are typed and basic.tzx plays: BASIC loads, and PRINT 2+2 shows 4; a tape dropped
    after that, with BASIC running from the RAM (the harness's `drop`), does not power cycle (BASIC is still at 0000h while the typist
    waits) and gets BASIC's LOAD "": the demonstration's prompt comes up; the CIP-02 started as the copier gets the copier's keys too."""
    main, core = cip_core()
    cwd = folder("cip_autoload")
    for model, tape, lines in (("cip03", "demo-presentation.tzx", ["until 600 apasa ENTER"]),
                               ("cip01", "basic.tzx", ["until 300 ELECTRONICA", "wait 1", "type p2+2", "enter", "wait 1", "expect 4"])):
        (cwd / f"{model}.script").write_text("\n".join(lines) + "\n", encoding="utf-8")
        run = zetro(main, "--config", cip_config(cwd, model, model), "--script", str(cwd / f"{model}.script"), "--drop", str(core / "assets" / "tapes" / tape))
        need(run.returncode == 0, f"{model}: exit {run.returncode}: {run.stderr.strip() or run.stdout.strip()}")
    tapes = core / "assets" / "tapes"
    answers = cip_run(main, cip_config(cwd, "running", "cip01"), ["wait 1", f"drop {(tapes / 'basic.tzx').resolve()}", "until 300 ELECTRONICA", "frames 25", "enter", "frames 20",
                                                              f"drop {(tapes / 'demo-presentation.tzx').resolve()}", "frames 5", "peek 0 4", "until 900 apasa ENTER", "text"])
    need(answers[-3]["data"] == "F3 AF 11 FF", f"a tape dropped with BASIC running restarted the machine: {answers[-3]['data']}")
    need("Salut prietene" in answers[-1]["text"], f"BASIC's LOAD \"\" did not load the demonstration: {answers[-1]['text']!r}")
    (cwd / "c02.script").write_text("until 300 ELECTRONICA\n", encoding="utf-8")
    run = zetro(main, "--config", cip_config(cwd, "c02", "cip02", startup="copier"), "--script", str(cwd / "c02.script"), "--drop", str(tapes / "basic.tzx"))
    need(run.returncode == 0, f"cip02 started as the copier: exit {run.returncode}: {run.stderr.strip() or run.stdout.strip()}")


def check_cip_copier_only() -> None:
    """The CIP-01 runs with its copier's ROM alone (cip01.rom in a roms folder beside the configuration, in a folder of its own, so no cip03.rom
    is anywhere): the copier starts, basic.tzx loads BASIC from tape (the read-back takes its character set from the BASIC in the RAM:
    ELECTRONICA is read, and PRINT 2+2 gives 4), and the tape traps find BASIC's loader in the RAM: the demonstration loads in a few seconds
    of guest time, as it does with the BASIC image. The CIP-02 is still refused without cip03.rom."""
    main, core = cip_core()
    cwd = folder("cip_copier_only")
    (cwd / "roms").mkdir(exist_ok=True)
    shutil.copyfile(main / "roms" / "cip01.rom", cwd / "roms" / "cip01.rom")
    tapes = core / "assets" / "tapes"
    config = cip_config(cwd, "cip01", "cip01")
    commands = ["until 5 LANSEZ", "text"] + cip_load_basic(core) + ["enter", "frames 20", "type p2+2", "enter", "frames 25", "text", 'type j""', "enter",
                                                                f"play {(tapes / 'demo-presentation.tzx').resolve()}", "until 900 apasa ENTER"]
    answers = control(cwd, commands + ["quit"], "--config", config)
    need(all(a["ok"] for a in answers), f"{[a for a in answers if not a['ok']]}")
    need("LANSEZ" in answers[1]["text"], f"the copier did not start on cip01.rom alone: {answers[1]['text']!r}")
    need("4" in cip_lines(answers[-6]["text"]), f"PRINT 2+2 in the BASIC the tape brought: {answers[-6]['text']!r}")
    seconds = (answers[-2]["frame"] - answers[-3]["frame"]) / 50.0
    need(seconds < 40.0, f"the demonstration took {seconds:.0f} s of guest time to load: the load trap did not find BASIC's loader in the RAM")
    (cwd / "cip02.yaml").write_text("machine:\n  model: cip02\n", encoding="utf-8")
    refused = control(cwd, ["text", "quit"], "--config", "cip02.yaml")
    need("CIP-02: ROMs are missing" in refused[0]["text"], f"the CIP-02 without cip03.rom: {refused[0]['text']!r}")


# --- the TIM-S (P6f) -----------------------------------------------------------------------------------


def tims_core() -> tuple[Path, Path]:
    """The main checkout (its roms/tims-standard.rom) and the TIM-S core beside it with its runtime case, script and demo tape;
    skipped without either."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-tims"
    if not (main / "roms" / "tims-standard.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'tims-standard.rom'} (python tools/fetch_roms.py)")
    if (not (main.parent / "MiSTerRO" / "docs" / "tims" / "runtime-cases.json").is_file() or not (core / "sim" / "load-demo.script").is_file()
            or not (core / "assets" / "tapes" / "demo.tap").is_file()):
        raise Skipped(f"no TIM-S core with its runtime case and demo tape beside the repository ({core})")
    return main, core


def tims_config(cwd: Path, name: str, **options: str) -> str:
    lines = "".join(f"    {key}: {value}\n" for key, value in options.items())
    (cwd / name).write_text("machine:\n  model: tims\n" + (f"  options:\n{lines}" if lines else ""), encoding="utf-8")
    return str(cwd / name)


def tims_case() -> tuple[Path, dict, list[str], float]:
    """The core's runtime case standard-cassette: the demo tape, the case, its script as harness commands (type, enter, play, wait)
    and the seconds it runs after the script."""
    main, core = tims_core()
    cases = json.loads((main.parent / "MiSTerRO" / "docs" / "tims" / "runtime-cases.json").read_text(encoding="utf-8"))
    case = cases["standard-cassette"]
    args = case["arguments"]
    commands = []
    for line in (core / args[args.index("--script") + 1]).read_text(encoding="utf-8").splitlines():
        word, _, rest = line.strip().partition(" ")
        if word == "play":
            commands.append(f"play {(core / rest).resolve()}")
        elif word:
            commands.append(line.strip())
    return (core / "assets" / "tapes" / "demo.tap").resolve(), case, commands, float(args[args.index("--seconds") + 1])


def tims_screen(case: dict, text: str, what: str) -> None:
    lines = text.splitlines()
    for pattern in case["screen"]:
        need(any(re.search(pattern, l) for l in lines), f"{what}: {pattern!r} is not on the screen: {text!r}")


def check_tims_cassette() -> None:
    """The core's runtime case standard-cassette (LOAD "", the demo tape, the screen shows TIM, DEMO and a line that is 42): in
    real time with the ROM tape traps off, and with them on (the tape is in when LOAD is typed: the trap takes the block), to
    the same screen. A state saved in the middle of the real-time load (the data block is coming in) goes on from it to the
    same screen in the same run and in a new one."""
    main, _ = tims_core()
    tape, case, commands, seconds = tims_case()
    cwd = folder("tims_cassette")
    off = tims_config(cwd, "off.yaml", tape_traps="off", tape_save_traps="off")
    on = tims_config(cwd, "on.yaml")
    state = cwd / "mid.zrs"
    waits = [i for i, c in enumerate(commands) if c.startswith("wait")]
    need(waits and commands[waits[-1] - 1].startswith("play"), f"the case does not end with play and wait: {commands}")
    tail = float(commands[waits[-1]].split()[1]) + seconds
    head = commands[:waits[-1]]
    # Real time (the host runs ahead of the tape's own clock): the state after 8 s of the tape (it loads in 8.3), in the data block.
    script = ["until 5 IPTV"] + head + ["wait 8.0", "text", f"state save {state}", f"wait {tail - 8.0:.2f}", "text", f"state load {state}",
                                        f"wait {tail - 8.0:.2f}", "text", "quit"]
    first = control(main, script, "--config", off)
    need(all(a["ok"] for a in first), f"the case in real time: {[a for a in first if not a['ok']]}")
    midway, real, again = first[-8]["text"], first[-5]["text"], first[-2]["text"]
    tims_screen(case, real, "real time")
    need("42" not in midway.split() and "TIM-S DEMO" not in midway, f"the state was not saved in the middle of the load: {midway!r}")
    need(again == real, f"the state loaded back ends elsewhere: {again!r} for {real!r}")
    new = control(main, [f"insert tape {tape}", f"state load {state}", f"wait {tail - 8.0:.2f}", "text", "quit"], "--config", off)
    need(new[-2].get("text") == real, f"the state in a new run: {new[-2]}")
    # The traps: the same commands with the tape in first; the load takes a few frames.
    inserted = [f"insert tape {tape}"] + [c for c in head if not c.startswith("play")]
    trapped = control(main, ["until 5 IPTV"] + inserted + ["until 10 42", "frames 100", "text", "quit"], "--config", on)
    need(all(a["ok"] for a in trapped), f"the case with the traps: {[a for a in trapped if not a['ok']]}")
    taken = (trapped[-4]["frame"] - trapped[len(inserted)]["frame"]) / 50.0
    need(taken <= 3.0, f"the load with the traps took {taken:.1f} s of guest time")
    tims_screen(case, trapped[-2]["text"], "traps on")
    need(trapped[-2]["text"] == real, f"the traps on and off end on other screens: {trapped[-2]['text']!r} and {real!r}")


# Machine code for the checks below: LD BC,nn / LD A,n / OUT (C),A / IN A,(C) / CP n / JP NZ, and the marker of the core's own
# diagnostics (RPASS! at 9100h; on a failure 0EEh there, the step in D at 9107h and the byte read in A at 9108h).
TIMS_ORG = 0x9000


def tims_out(port: int, value: int) -> bytes:
    return bytes([0x01, port & 0xFF, port >> 8, 0x3E, value, 0xED, 0x79])


def tims_cmp(step: int, port: int, value: int, fail: int) -> bytes:
    return bytes([0x16, step, 0x01, port & 0xFF, port >> 8, 0xED, 0x78, 0xFE, value, 0xC2, fail & 0xFF, fail >> 8])


def tims_diagnostic(body: bytes, steps: list[tuple[int, int, int]]) -> bytes:
    """`body` (code to run first), then each (port, value) comparison with its step number, the pass marker and the failure."""
    body = bytes([0xF3, 0x01, 0xFF, 0xFF, 0x0B, 0x78, 0xB1, 0x20, 0xFB]) + body[1:]    # DI; a delay of 0.4 s (the ENTER that started it is still down)
    end = TIMS_ORG + len(body) + len(steps) * 12              # the pass code starts here: LD HL,passed / LD DE,9100h / LD BC,6 / LDIR / JR $
    fail = end + 13                                           # LD (9108h),A / LD A,D / LD (9107h),A / LD A,0EEh / LD (9100h),A / JR $ ; "RPASS!"
    code = body
    for step, port, value in steps:
        code += tims_cmp(step, port, value, fail)
    passed = fail + 14
    code += bytes([0x21, passed & 0xFF, passed >> 8, 0x11, 0x00, 0x91, 0x01, 0x06, 0x00, 0xED, 0xB0, 0x18, 0xFE])
    code += bytes([0x32, 0x08, 0x91, 0x7A, 0x32, 0x07, 0x91, 0x3E, 0xEE, 0x32, 0x00, 0x91, 0x18, 0xFE]) + b"RPASS!"
    assert TIMS_ORG + len(code) == passed + 6
    return code


def tims_run(config: str, program: bytes, after: list[str], before: list[str] | None = None, address: int = TIMS_ORG) -> list[dict]:
    """The TIM-S up, `before` run, the program poked at `address`, RANDOMIZE USR address typed (USR is extended mode, L), then `after`;
    the answers to `after`."""
    main, _ = tims_core()
    usr = ["until 5 IPTV", "frames 40", *(before or []), *poke_commands(address, program), "type t", "key CAPS SHIFT", "key SYMBOL SHIFT",
           "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3", f"type l{address}", "enter"]
    answers = control(main, usr + after + ["quit"], "--config", config)
    need(all(a["ok"] for a in answers[:-1]), f"{[a for a in answers if not a['ok']]}")
    return answers[len(usr):-1]


def check_tims_ports() -> None:
    """The standard model's ports on the real firmware (what the core's native diagnostic ports-standard, diagnostics.py, checks, written
    for this ROM as RAM programs): (a) the 8255's output ports A and B read back, and port A as the keyboard port at FEh, F9h and E0h gives
    7Fh with no key down (EAR high, serial not READY, the printer not on line, the five keys up), at the aliases the schematic's decode
    (A0, A3 and A4 alone) gives as well; (b) a key held shows in bits 0-4 of those ports for the row the address selects; (c) the RAM under
    the ROM: protected by port C bit 1 low, written when it is set, hidden by the startup ROM when the flip-flop of port F0h is clocked (it
    toggles: clocked twice, the ROM is out again) while the writes go to the RAM underneath, and BASIC runs on afterwards; (d) port C bit 2
    keeps writes out of the video buffer; (e) port C bit 4 low takes the RAM away (it reads FFh) and a reset starts the firmware again."""
    main, _ = tims_core()
    cwd = folder("tims_ports")
    config = tims_config(cwd, "tims.yaml", joystick="none")     # no added Kempston board: 1Fh is the keyboard port's alias
    rom = (main / "roms" / "tims-standard.rom").read_bytes()
    # (a) control word 89h: A and B outputs, C inputs (the pins float high: the RAM stays on). The reads at E0h and its alias A0h give
    # port A's latch, E2h port B's.
    body = bytes([0xF3]) + tims_out(0xE6, 0x89) + tims_out(0xE0, 0xA5) + tims_out(0xE2, 0x5A)
    diag = tims_diagnostic(body, [(1, 0xE0, 0xA5), (2, 0x00A0, 0xA5), (3, 0xE2, 0x5A), (4, 0x00E2, 0x5A)])
    got = peeked(tims_run(config, diag, ["frames 40", "peek 9100 9"])[-1])
    need(got[:6] == b"RPASS!", f"(a) the 8255's output ports: {got.hex(' ')}")
    # control word 99h: A input, B output, C input: the keyboard port with no key down, FEh, F9h, E0h, a row, the aliases 1Fh and FFh
    diag = tims_diagnostic(bytes([0xF3]) + tims_out(0xE6, 0x99), [(1, 0x00FE, 0x7F), (2, 0x00F9, 0x7F), (3, 0x00E0, 0x7F), (4, 0xFDFE, 0x7F),
                                                                  (5, 0x001F, 0x7F), (6, 0x00FF, 0x7F)])
    got = peeked(tims_run(config, diag, ["frames 40", "peek 9100 9"])[-1])
    need(got[:6] == b"RPASS!", f"(a) port A as the keyboard port with no key down: {got.hex(' ')}")
    # (b) a loop that samples FEh, F9h and E0h on row A9 (the A key), FEh on row A8 (CAPS SHIFT Z X C V) and A15 (SPACE SYMBOL SHIFT M N B),
    # and on all rows (B = 0)
    loop = bytearray([0xF3])
    for i, port in enumerate((0xFDFE, 0xFDF9, 0xFDE0, 0xFEFE, 0x7FFE, 0x00FE)):
        loop += bytes([0x01, port & 0xFF, port >> 8, 0xED, 0x78, 0x32, 0x10 + i, 0x91])
    loop += bytes([0x18, (-(len(loop) + 2)) & 0xFF])
    answers = tims_run(config, bytes(loop), ["frames 4", "peek 9110 6", "key A", "frames 4", "peek 9110 6", "release A", "key SPACE", "key CAPS SHIFT",
                                             "frames 4", "peek 9110 6"])
    quiet, a_down, others = (peeked(a) for a in (answers[1], answers[4], answers[-1]))
    need(quiet == bytes([0x7F] * 6), f"(b) no key: {quiet.hex(' ')}")
    need(a_down == bytes([0x7E, 0x7E, 0x7E, 0x7F, 0x7F, 0x7E]), f"(b) the A key: {a_down.hex(' ')}")
    need(others == bytes([0x7F, 0x7F, 0x7F, 0x7E, 0x7E, 0x7E]), f"(b) SPACE and CAPS SHIFT: {others.hex(' ')}")
    # (c) the RAM under the ROM, in a program that goes back to BASIC; the results at 9120h
    at = 0x0100

    def out_n(port: int, value: int) -> bytes:
        return bytes([0x3E, value, 0xD3, port])

    def store(n: int) -> bytes:
        return bytes([0x3A, at & 0xFF, at >> 8, 0x32, 0x20 + n, 0x91])

    prog = bytearray([0xF3, 0x3A, at & 0xFF, at >> 8, 0x32, 0x20, 0x91, 0x4F, 0xEE, 0xFF, 0x47])      # DI; LD A,(at); LD (9120h),A; LD C,A; XOR FFh; LD B,A
    prog += bytes([0x78, 0x32, at & 0xFF, at >> 8]) + store(1)                                        # protected: write the complement, read back
    prog += out_n(0xE6, 0x03) + bytes([0x78, 0x32, at & 0xFF, at >> 8]) + store(2)                # port C bit 1 set: the write lands
    prog += bytes([0x3E, 0, 0xD3, 0xF0]) + store(3)                                                    # OUT (F0h): the startup ROM is in: the ROM's byte
    prog += bytes([0x79, 0xEE, 0xAA, 0x32, at & 0xFF, at >> 8]) + store(4)                            # a write with it in: not seen
    prog += bytes([0x3E, 0, 0xD3, 0xF0]) + store(5)                                                    # OUT (F0h) again: the RAM, with that write
    prog += bytes([0x79, 0x32, at & 0xFF, at >> 8]) + out_n(0xE6, 0x02) + bytes([0xFB, 0xC9])     # the byte put back, protect again, EI, RET
    answers = tims_run(config, bytes(prog), ["frames 5", "peek 9120 6", "type p2+2", "enter", "frames 25", "text"])
    r = peeked(answers[1])
    original = rom[at]
    need(r == bytes([original, original, original ^ 0xFF, original, original, original ^ 0xAA]),
         f"(c) the RAM under the ROM: {r.hex(' ')} (the ROM's byte is {original:02X})")
    lines = [l.strip() for l in answers[-1]["text"].splitlines()]
    need("4" in lines and any("0 OK, 0:1" in l for l in lines), f"(c) BASIC after it: {answers[-1]['text']!r}")
    # (d) 'A' drawn into two cells of text row 10 straight into the display file, the first with port C bit 2 set (the video buffer is not
    # written), the second with it reset
    glyph = [0x00, 0x3C, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x00]
    draw = bytearray(tims_out(0xE6, 0x05))
    for column in (5, 10):
        for y in range(8):
            draw += bytes([0x3E, glyph[y], 0x32, (0x4840 + 0x100 * y + column) & 0xFF, (0x4840 + 0x100 * y + column) >> 8])
        if column == 5:
            draw += tims_out(0xE6, 0x04)
    draw += bytes([0xC9])
    row = tims_run(config, bytes(draw), ["frames 5", "text"])[-1]["text"].splitlines()[10]
    need(row == " " * 10 + "A", f"(d) the video buffer with port C bit 2: row 10 is {row!r}")
    # (e) the RAM off: the program only switches it off (port C bit 4 low); nothing can run after that, and the harness reads FFh from the
    # RAM and from the copy of the ROM at 0000h; a reset starts the firmware again
    answers = tims_run(config, tims_out(0xE4, 0xE9) + bytes([0x18, 0xFE]), ["frames 3", "peek 9000 4", "peek 0000 2", "reset", "until 5 IPTV", "poke 9000 5A", "peek 9000 1"])
    need(peeked(answers[1]) == b"\xff" * 4 and peeked(answers[2]) == b"\xff\xff", f"(e) the RAM with port C bit 4 low: {answers[1]['data']} {answers[2]['data']}")
    need(peeked(answers[-1]) == b"\x5a", f"(e) after the reset: {answers[-1]}")


def tims_loop_program(prologue: bytes, body: bytes) -> bytes:
    """`prologue`, then IM 2 with the handler at 9040h, EI and HALT (the first frame interrupt), HL := 0 and `body` (it loops) until
    the second, whose handler stores HL at 9100h and halts."""
    code = bytearray([0xF3]) + prologue
    code += bytes([0x31, 0xF0, 0x81, 0x3E, 0x80, 0xED, 0x47, 0xED, 0x5E, 0x21, 0x40, 0x90, 0x22, 0xFF, 0x80, 0xFB, 0x76, 0x21, 0x00, 0x00])
    code += body
    assert len(code) <= 0x40, len(code)
    code += bytes(0x40 - len(code))
    # the handler: PUSH AF; LD A,(9102h); OR A; JR NZ,second; INC A; LD (9102h),A; POP AF; EI; RETI; second: LD (9100h),HL; JR $
    code += bytes([0xF5, 0x3A, 0x02, 0x91, 0xB7, 0x20, 0x08, 0x3C, 0x32, 0x02, 0x91, 0xF1, 0xFB, 0xED, 0x4D, 0x22, 0x00, 0x91, 0x18, 0xFE])
    return bytes(code)


def tims_loop_count(config: str, program: bytes) -> int:
    answers = tims_run(config, program, ["frames 6", "peek 9100 2"], before=["poke 9102 00"])
    return int.from_bytes(peeked(answers[-1]), "little")


def check_tims_clock() -> None:
    """The CPU switch. A loop counts its turns from one frame interrupt to the next (the interrupt keeps its 50 Hz whatever the CPU does: the
    video is on the master clock): the turns are what 69888 T-states of 3.5 MHz give at the 3.5 MHz setting, and in proportion to the
    setting (6, 7, 8 MHz) once the firmware has selected the auxiliary clock, which it does after its start (port C bit 3 high, the
    startup ROM out); with the bit reset by a program's OUT the CPU is on 3.5 MHz again. A loop that reads a port runs at the auxiliary
    clock but for the port's cycle, which lasts as long as at 3.5 MHz."""
    tims_core()
    cwd = folder("tims_clock")
    spin = bytes([0x23, 0x18, 0xFD])                                  # INC HL; JR back (18 T-states a turn)
    reading = bytes([0xDB, 0xFE, 0x23, 0x18, 0xFB])                   # IN A,(FEh); INC HL; JR back
    drop = tims_out(0xE6, 0x06)                                       # port C bit 3 reset: the CPU is held at 3.5 MHz
    counts = {}
    for setting in ("3.5", "6", "7", "8"):
        counts[setting] = tims_loop_count(tims_config(cwd, f"cpu{setting}.yaml", cpu_switch=setting), tims_loop_program(b"", spin))
    base = counts["3.5"]
    need(abs(base - 69888 // 18) <= 12, f"3.5 MHz: {base} turns between two interrupts (69888 T-states / 18 = {69888 // 18})")
    for setting, factor in (("6", 6 / 3.5), ("7", 2.0), ("8", 8 / 3.5)):
        need(abs(counts[setting] / base - factor) <= 0.005 * factor, f"{setting} MHz: {counts[setting]} turns in a frame, {base * factor:.0f} expected ({counts})")
    held = tims_loop_count(tims_config(cwd, "cpu7.yaml", cpu_switch="7"), tims_loop_program(drop, spin))
    need(abs(held - base) <= 6, f"7 MHz with port C bit 3 reset: {held} turns, {base} at 3.5 MHz")
    reads = {s: tims_loop_count(tims_config(cwd, f"cpu{s}.yaml", cpu_switch=s), tims_loop_program(b"", reading)) for s in ("3.5", "7")}
    # IN A,(n) is 11 T-states, so 11 + 6 + 12 = 29 a turn at 3.5 MHz; at 7 MHz its port cycle is 8 T-states instead of 4: 33
    need(abs(reads["3.5"] - 69888 // 29) <= 8 and abs(reads["7"] - 139776 // 33) <= 12, f"the loop with IN: {reads} (expected {69888 // 29} and {139776 // 33})")


def check_tims_io() -> None:
    """The byte ports and the states from the host's side (the harness has no command for the ports): tests/test_tims.c, built with
    the program (ctest runs it too): LPRINT "A" waits for the printer to be on line, then A arrives at the printer port; the firmware's
    serial routine sends its bytes (7 data bits, 2 stop bits, 1444 T-states a bit) to the serial port once the device is ready; states
    saved in the start, in BASIC at 3.5 MHz and at 7 MHz load into a new machine and run on to the same bytes; a state of another CPU
    switch is refused."""
    tims_core()
    exe = EXE.with_name("test_tims.exe" if os.name == "nt" else "test_tims")
    if not exe.is_file():
        raise Skipped(f"{exe} is not built")
    (ROOT / "work" / "tests").mkdir(parents=True, exist_ok=True)
    run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=300)
    if run.returncode == 77:
        raise Skipped(run.stdout.strip())
    need(run.returncode == 0 and "test_tims passed" in run.stdout, f"exit {run.returncode}: {run.stdout.strip()}")




# --- the CoBra (core/machines/cobra.c) ---------------------------------------------------------------------------
# Each check replays a runtime case of the CoBra core (MiSTerRO/docs/cobra/runtime-cases.json: its arguments and sim/ scripts, its
# expected screen lines and disk contents) through the harness, headless. Skipped without roms/cobra.rom or the MiSTerRO checkout.

def cobra_core() -> tuple[Path, Path, dict]:
    """The main checkout, the CoBra core beside it and its runtime cases; skipped when either is missing."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-cobra"
    cases = main.parent / "MiSTerRO" / "docs" / "cobra" / "runtime-cases.json"
    if not (core / "sim").is_dir() or not cases.is_file():
        raise Skipped(f"no CoBra core beside the repository ({core})")
    if not (main / "roms" / "cobra.rom").is_file():
        raise Skipped(f"no {main / 'roms' / 'cobra.rom'} (python tools/fetch_roms.py)")
    return main, core, json.loads(cases.read_text(encoding="utf-8"))


def cobra_script(core: Path, name: str) -> list[str]:
    """A script of the core's simulator (type, enter, wait, play) as harness commands. The wait after the boot key D becomes a wait
    for CP/M's prompt: the 8272 here takes its head-load time as the data sheet gives it (Specify HLT at the 4 MHz clock of a
    5.25-inch drive), so CP/M comes up in about 3.5 s where the core's controller needs under 3."""
    commands: list[str] = []
    booted = False
    for line in (core / "sim" / name).read_text(encoding="utf-8").splitlines():
        word, _, rest = line.strip().partition(" ")
        if not word:
            continue
        if word == "play":
            commands.append(f"play {core / rest}")
        elif word == "wait" and commands and commands[-1] == "type d" and not booted:
            commands.append("until 30 A>")
            booted = True
        elif word in ("type", "enter", "wait"):
            commands.append(line.strip())
        else:
            raise Failed(f"the core's script {name} has a command this check does not know: {line!r}")
    return commands


def cobra_case_run(cwd: Path, core: Path, case: dict, disk: Path | None = None, model: str = "cobra80") -> list[dict]:
    """A runtime case: its model (--status 64: the 64K), its disk in drive A, its script, then the screen as text and a shot."""
    args = case["arguments"]
    script_name = Path(args[args.index("--script") + 1]).name
    if "--status" in args and args[args.index("--status") + 1] == "64":
        model = "cobra64"
    commands = ([f"insert a {disk}"] if disk else []) + ["frames 25"] + cobra_script(core, script_name)
    answers = hc_run(hcp_config(cwd, model, model), commands + ["text", f"shot {cwd / 'screen.png'}"])
    text = answers[-2]["text"]
    for pattern in case["screen"]:
        need(screen_has(text, pattern), f"{script_name}: no line matches {pattern!r} on the screen {text!r}")
    return answers


def check_cobra_basic64() -> None:
    """The 64K CoBra's runtime case basic64: its bootstrap's B starts the 48K BASIC copied from its EPROM; PRINT 42 shows 42."""
    main, core, cases = cobra_core()
    cobra_case_run(folder("cobra_basic64"), core, cases["basic64"])


def check_cobra_basic80_cassette() -> None:
    """The 80K CoBra's runtime case basic80-cassette: B starts BASIC (the system ROM's second quarter), LOAD "" loads demo.tap
    from the tape in real time: COBRA, DEMO and 42 on the screen."""
    main, core, cases = cobra_core()
    cobra_case_run(folder("cobra_basic80_cassette"), core, cases["basic80-cassette"])


def cobra_files(cwd: Path, core: Path, cases: dict) -> Path:
    """The runtime case cpm-files on a copy of the core's system.dsk: CP/M boots through the CTC-driven 8272, SAVE 1 PROBE.COM and
    DIR list CBOT and PROBE; the image on the host holds the directory entry the case names. The written image."""
    case = cases["cpm-files"]
    disk = cwd / "system.dsk"
    shutil.copyfile(core / "assets" / "disks" / "system.dsk", disk)
    original = disk.read_bytes()
    cobra_case_run(cwd, core, case, disk)
    image = disk.read_bytes()
    for _, hexa in case["disk_contains_hex"].items():
        need(bytes.fromhex(hexa) in image, f"the image of drive A has no {hexa}")
    need(len(image) == len(original) and image[:8] == original[:8], "the image of drive A lost its form")
    return disk


def check_cobra_cpm_files() -> None:
    main, core, cases = cobra_core()
    cobra_files(folder("cobra_cpm_files"), core, cases)


def check_cobra_cpm_reboot() -> None:
    """The runtime case cpm-reboot: the disk the files case wrote, in a new run, boots and DIR still lists PROBE."""
    main, core, cases = cobra_core()
    cwd = folder("cobra_cpm_reboot")
    disk = cobra_files(cwd, core, cases)
    written = cwd / "written.dsk"
    shutil.copyfile(disk, written)
    cobra_case_run(cwd, core, cases["cpm-reboot"], written)


def z80_program(origin: int, build) -> bytes:
    """A small Z80 program: `build(emit, fail_jump)` emits its bytes; jumps to the label 'fail' and 'passed' are fixed at the end."""
    code = bytearray()
    fixups: list[tuple[int, str]] = []
    labels: dict[str, int] = {}

    def emit(*items) -> None:
        for item in items:
            if isinstance(item, str):
                if item.startswith(":"):
                    labels[item[1:]] = origin + len(code)
                else:
                    fixups.append((len(code), item))
                    code.extend(b"\0\0")
            elif isinstance(item, bytes):
                code.extend(item)
            else:
                code.append(item)
    build(emit)
    for at, name in fixups:
        code[at:at + 2] = labels[name].to_bytes(2, "little")
    return bytes(code)


def cobra_ports_80k(emit) -> None:
    """The core's CoBra diagnostic (shared/romanian/tools/diagnostics.py, ports80) run from RAM: R7 set brings the startup overlay
    back from CP/M (the 80K's own bootstrap does so after a failed disk boot); the 8255's mode, port C, port A (no key: FFh; the
    diagnostic's 7Fh is its simulator's serial input held low), port B (the joystick's fire and down: 14h), the overlay's RAM at
    8000h (physical 0000h), the video RAM at C000h with PC6 set and the main RAM there with PC6 clear, the 8272's main status (RQM),
    the CTC's channel 3 counting frame pulses; R7 clear leaves the overlay for CP/M again (PC6 set)."""
    def cmp_in(port: int, value: int) -> None:
        emit(0x01, port & 0xFF, port >> 8, 0xED, 0x78, 0xFE, value, 0xC2, "fail")

    def out(port: int, value: int) -> None:
        emit(0x3E, value, 0xD3, port)

    def mem(address: int, value: int) -> None:
        emit(0x3E, value, 0x32, address & 0xFF, address >> 8, 0x3A, address & 0xFF, address >> 8, 0xFE, value, 0xC2, "fail")
    emit(0xF3, 0x3E, 0x80, 0xED, 0x4F)
    out(0xDF, 0x92)
    out(0xFE, 0x40)
    cmp_in(0x00FE, 0xFF)
    cmp_in(0x001F, 0x14)
    mem(0x8100, 0x63)
    mem(0xC000, 0xA5)
    out(0xFE, 0x00)
    mem(0xC000, 0x5A)
    out(0xFE, 0x40)
    emit(0x3A, 0x00, 0xC0, 0xFE, 0xA5, 0xC2, "fail")
    cmp_in(0x00F5, 0x80)
    out(0xFB, 0x57)                                     # channel 3: counter, rising edge, a time constant follows
    out(0xFB, 0x05)
    emit(0x01, 0x00, 0x00, 0x0B, 0x78, 0xB1, 0x20, 0xFB)   # about 22 frames
    emit(0x01, 0xFB, 0x00, 0xED, 0x78, 0xFE, 0x05, 0xCA, "fail")
    emit(0x21, "passed", 0x11, 0x00, 0x82, 0x01, 0x06, 0x00, 0xED, 0xB0, 0xAF, 0xED, 0x4F, 0x18, 0xFE)
    emit(":fail", 0x32, 0x06, 0x82, 0x3E, 0xEE, 0x32, 0x00, 0x82, 0xAF, 0xED, 0x4F, 0x18, 0xFE)
    emit(":passed", b"RPASS!")


def cobra_ports_64k(emit) -> None:
    """The 64K's BASIC side, from RANDOMIZE USR: RAM bank 0 (the BASIC copied from the EPROM) is write-protected, port A reads FFh with
    no key, port B the joystick (14h); RPASS! at 9000h."""
    def cmp_in(port: int, value: int) -> None:
        emit(0x01, port & 0xFF, port >> 8, 0xED, 0x78, 0xFE, value, 0xC2, "fail")
    emit(0xF3, 0x3A, 0x00, 0x00, 0x2F, 0x32, 0x00, 0x00, 0x47, 0x3A, 0x00, 0x00, 0xB8, 0xCA, "fail")
    emit(0x01, 0xFE, 0x00, 0xED, 0x78, 0xE6, 0x1F, 0xFE, 0x1F, 0x20, 0xF5)    # ENTER let go
    cmp_in(0x00FE, 0xFF)
    cmp_in(0x001F, 0x14)
    emit(0x21, "passed", 0x11, 0x00, 0x90, 0x01, 0x06, 0x00, 0xED, 0xB0, 0xFB, 0xC9)
    emit(":fail", 0x32, 0x06, 0x90, 0x3E, 0xEE, 0x32, 0x00, 0x90, 0xFB, 0xC9)
    emit(":passed", b"RPASS!")


def check_cobra_ports() -> None:
    """The diagnostic's ports, as far as the harness can drive them (the diagnostic itself replaces the bootstrap ROM, which ZetRo
    finds by its hash): on the 80K the program above runs as a CP/M command (T.COM, a jump to it at 9000h, with a copy at 1000h,
    where 9000h is in the overlay) and leaves RPASS! at 0200h; on the 64K its BASIC-side half runs from USR. The startup overlay
    of both and the 64K's banking are what the basic and cassette cases run through (the bootstraps copy BASIC through them)."""
    main, core, cases = cobra_core()
    cwd = folder("cobra_ports")
    disk = cwd / "system.dsk"
    shutil.copyfile(core / "assets" / "disks" / "system.dsk", disk)
    test = z80_program(0x9000, cobra_ports_80k)
    commands = [f"insert a {disk}", "frames 25", "type d", "until 30 A>", "frames 10"]
    commands += poke_commands(0x9000, test) + poke_commands(0x1000, test) + ["poke 0100 C3 00 90", "type save 1 t.com", "enter",
                                                                             "wait 1", "joy 0 down fire", "type t", "enter", "frames 60", "peek 0200 7"]
    answers = hc_run(hcp_config(cwd, "cobra80", "cobra80"), commands)
    got = peeked(answers[-1])
    need(got[:6] == b"RPASS!", f"the 80K's ports: {got!r} (EE then the failing value: the step that failed)")
    test = z80_program(0x8000, cobra_ports_64k)
    commands = ["frames 25", "type b", "wait 2"] + poke_commands(0x8000, test) + ["joy 0 down fire"] + usr_commands(32768) + ["frames 10", "peek 9000 7"]
    answers = hc_run(hcp_config(cwd, "cobra64", "cobra64"), commands)
    got = peeked(answers[-1])
    need(got[:6] == b"RPASS!", f"the 64K's ports: {got!r}")


def check_cobra_state() -> None:
    """A state saved in the middle of the CP/M disk case (DIR, the 8272 in its execution phase, the CTC counting its byte requests)
    loads back to the same end, in the same run and in a new one with the disk in the drive: the latch, the 8255, the CTC, the 8272
    and the drives are in the state."""
    main, core, cases = cobra_core()
    cwd = folder("cobra_state")
    disk = cwd / "system.dsk"
    shutil.copyfile(core / "assets" / "disks" / "system.dsk", disk)
    config = hcp_config(cwd, "cobra80", "cobra80")
    states = [cwd / f"s{i}.zrs" for i in range(30)]
    boot = [f"insert a {disk}", "frames 25", "type d", "until 30 A>", "frames 10", "type dir", "enter"]
    answers = hc_run(config, boot + [c for s in states for c in ("frames 1", f"state save {s}")] + ["frames 200", "text"])
    end = answers[-1]["text"]
    need(screen_has(end, r"CBOT\s+SYS"), f"DIR: {end!r}")
    busy = [s for s in states if state_chunks(s)["8272"][0][16] == 2]
    need(len(busy) >= 2, f"no state of the {len(states)} was saved in the 8272's execution phase ({len(busy)})")
    middle = busy[len(busy) // 2]
    again = hc_run(config, boot + ["frames 5", f"state load {middle}", "frames 200", "text"])
    need(again[-1]["text"].replace("?", "") == end.replace("?", ""), f"the state in the same run: {again[-1]['text']!r} for {end!r}")
    fresh = hc_run(config, [f"insert a {disk}", f"state load {middle}", "frames 200", "text", f"shot {cwd / 'cpm80.png'}"])
    need(fresh[-2]["text"].replace("?", "") == end.replace("?", ""), f"the state in a new run: {fresh[-2]['text']!r} for {end!r}")


# --- the CIP-04, experimental (src/core/machines/cip04.c) ----------------------------------------------------
#
# The +3's own hardware as its published documentation gives it, exercised through the harness with the
# original ROM (roms/cip04.rom): the menu, +3 BASIC and 48 BASIC; the eight RAM pages, the four special all-RAM configurations, the
# four ROMs and the lock through ports 7FFD and 1FFD by a machine-code program; the AY; the tape in 48 BASIC; the disk (the +3's DOS
# formats a blank DSK, saves, loads and lists a program); LPRINT on the printer port; 128K-format snapshots with 1FFD and the state in
# the middle of a disk operation. What the core's validation covers of the CIP-04 is the ROM and its ports; none of it runs the machine.


def cip04_rom() -> bytes:
    """The CIP-04's ROM of the main checkout's roms/; skipped when it is not there."""
    path = main_checkout() / "roms" / "cip04.rom"
    if not path.is_file():
        raise Skipped(f"no {path} (python tools/fetch_roms.py)")
    return path.read_bytes()


def cip04_config(cwd: Path, name: str, **options: str) -> Path:
    return hcp_config(cwd, name, "cip04", **options)


def cip04_run(config: Path, commands: list[str], *args: str) -> list[dict]:
    return hc_run(config, commands, *args)


def cip04_texts(commands: list[str], answers: list[dict]) -> list[str]:
    """The screens the `text` commands among `commands` answered, in order."""
    return [a["text"] for c, a in zip(commands, answers) if c == "text"]


def cip04_start(entry: int) -> list[str]:
    """The menu's entry `entry` (1: +3 BASIC, 3: 48 BASIC) chosen (Down is CAPS SHIFT and 6), and the BASIC up."""
    down = [c for _ in range(entry) for c in key_commands(["CAPS SHIFT", "6"], 6)]
    return ["until 10 Loader", "frames 30"] + down + ["enter", "frames 100" if entry == 3 else "frames 60"]


def cip04_line(text: str, wait: int = 60) -> list[str]:
    return [f"type {text}", "enter", f"frames {wait}"]


def cip04_blank_dsk(tracks: int = 40) -> bytes:
    """A DSK (CPCEMU format) of 40 tracks on one side, every track empty: no sector until the machine formats it."""
    head = bytearray(256)
    head[:34] = b"MV - CPCEMU Disk-File\r\nDisk-Info\r\n"
    head[34:48] = b"ZetRo verify".ljust(14, b"\0")
    head[48], head[49] = tracks, 1
    struct.pack_into("<H", head, 50, 256)
    out = bytearray(head)
    for t in range(tracks):
        block = bytearray(256)
        block[:13] = b"Track-Info\r\n\0"
        block[16], block[20], block[22], block[23] = t, 2, 0x4E, 0xE5
        out += block
    return bytes(out)


def check_cip04_basic() -> None:
    """The CIP-04 starts to the +3 menu (its four entries and the DOS's line about the drives it found), +3 BASIC takes PRINT 2+2 typed in
    full (4 and the report 0 OK on the screen) and 48 BASIC, chosen from the menu, PRINT 3+3."""
    cip04_rom()
    cwd = folder("cip04_basic")
    config = cip04_config(cwd, "cip04")
    answers = cip04_run(config, ["until 10 Loader", "frames 30", "text", f"shot {cwd / 'menu.png'}"])
    for entry in ("128 +3", "Loader", "BASIC128", "Calculator", "BASIC 48", "Drives A: and M: available."):
        need(entry in answers[-2]["text"], f"{entry!r} is not in the menu: {answers[-2]['text']!r}")
    need((cwd / "menu.png").stat().st_size > 500, "menu.png")
    answers = cip04_run(config, cip04_start(1) + ["type print 2+2", "enter", "frames 40", "text"])
    need(screen_has(answers[-1]["text"], r"^4$") and screen_has(answers[-1]["text"], r"0 OK"), f"PRINT 2+2 in +3 BASIC: {answers[-1]['text']!r}")
    answers = cip04_run(config, cip04_start(3) + ["type p3+3", "enter", "frames 20", "text"])
    need(screen_has(answers[-1]["text"], r"^6$") and screen_has(answers[-1]["text"], r"0 OK"), f"PRINT 3+3 in 48 BASIC: {answers[-1]['text']!r}")


def cip04_ld_bc(n: int) -> bytes: return b"\x01" + struct.pack("<H", n)
def cip04_ld_a(n: int) -> bytes: return bytes([0x3E, n])
def cip04_ld_a_mem(a: int) -> bytes: return b"\x3A" + struct.pack("<H", a)
def cip04_ld_mem_a(a: int) -> bytes: return b"\x32" + struct.pack("<H", a)


CIP04_OUT = b"\xED\x79"                             # OUT (C),A
CIP04_RESULTS = 0x9400


def cip04_paging_program() -> bytes:
    """Port 7FFD, 1FFD and the ROMs by the CPU (the interrupts off), run from 9000h (RAM page 2, which 7FFD never moves; the special modes
    do: the program copies itself into page 6 at the same place first): a byte in each of the eight RAM pages through C000h and read
    back; for each of the four special configurations the first bytes at 0000h, 4000h, 8000h and C000h and a byte written at 0001h; the
    byte 0100h of each of the four ROMs; the lock (7FFD with bit 5, then a write that changes the page, and the special mode that the
    lock holds off). The results are at 9400h."""
    r, body = CIP04_RESULTS, bytearray()
    for n in range(8):
        body += (cip04_ld_bc(0x7FFD) + cip04_ld_a(n) + CIP04_OUT + cip04_ld_a(0x40 + n) + cip04_ld_mem_a(0xC000) +
                 cip04_ld_a(0x50 + n) + cip04_ld_mem_a(0xC001))
    body += cip04_ld_bc(0x7FFD) + b"\xAF" + CIP04_OUT
    for n in range(8):
        body += cip04_ld_bc(0x7FFD) + cip04_ld_a(n) + CIP04_OUT + cip04_ld_a_mem(0xC000) + cip04_ld_mem_a(r + n)
    body += cip04_ld_bc(0x7FFD) + b"\xAF" + CIP04_OUT
    for c in range(4):
        body += cip04_ld_bc(0x1FFD) + cip04_ld_a(1 | c << 1) + CIP04_OUT
        for reg, address in ((b"\x57", 0x0000), (b"\x5F", 0x4000), (b"\x67", 0x8000), (b"\x6F", 0xC000)):    # LD D,A / E / H / L
            body += cip04_ld_a_mem(address) + reg
        body += cip04_ld_a(0xB0 + c) + cip04_ld_mem_a(0x0001) + b"\xAF" + CIP04_OUT                       # back to normal (BC is 1FFDh)
        for k, reg in enumerate((b"\x7A", b"\x7B", b"\x7C", b"\x7D")):                                      # LD A,D / E / H / L
            body += reg + cip04_ld_mem_a(r + 0x10 + 4 * c + k)
    for i, n in enumerate((0, 4)):
        body += cip04_ld_bc(0x7FFD) + cip04_ld_a(n) + CIP04_OUT + cip04_ld_a_mem(0xC001) + cip04_ld_mem_a(r + 0x20 + i)
    body += cip04_ld_bc(0x7FFD) + b"\xAF" + CIP04_OUT
    for rom in range(4):
        body += (cip04_ld_bc(0x7FFD) + cip04_ld_a((rom & 1) << 4) + CIP04_OUT + cip04_ld_bc(0x1FFD) + cip04_ld_a((rom >> 1) << 2) + CIP04_OUT +
                 cip04_ld_a_mem(0x0100) + cip04_ld_mem_a(r + 0x30 + 2 * rom) + cip04_ld_a_mem(0x0101) + cip04_ld_mem_a(r + 0x31 + 2 * rom))
    body += (cip04_ld_bc(0x1FFD) + cip04_ld_a(0x04) + CIP04_OUT + cip04_ld_bc(0x7FFD) + cip04_ld_a(0x37) + CIP04_OUT +
             cip04_ld_a(0x00) + CIP04_OUT + cip04_ld_a_mem(0xC000) + cip04_ld_mem_a(r + 0x40) +
             cip04_ld_bc(0x1FFD) + cip04_ld_a(0x01) + CIP04_OUT + cip04_ld_a_mem(0x0100) + cip04_ld_mem_a(r + 0x41) +
             cip04_ld_a(0x5A) + cip04_ld_mem_a(r + 0xFF) + b"\x18\xFE")
    # The prologue copies the whole program (itself included) to the same place in page 6, where the special modes find it.
    total = 25 + len(body)
    prologue = (b"\xF3" + cip04_ld_bc(0x7FFD) + cip04_ld_a(6) + CIP04_OUT + b"\x21" + struct.pack("<H", 0x9000) +
                b"\x11" + struct.pack("<H", 0xD000) + cip04_ld_bc(total) + b"\xED\xB0" + cip04_ld_bc(0x7FFD) + b"\xAF" + CIP04_OUT)
    need(len(prologue) == 25, "the paging program's prologue")
    return prologue + bytes(body)


def cip04_ay_program() -> bytes:
    """An AY note (register 0 16, mixer 3Eh, amplitude 15), register 14 cleared, and the registers 8, 7, 0 and 14 read back to 9500h to 9503h;
    register 14 reads its two input lines (RXD and the peripheral's ready line) high above the written bits."""
    code = bytearray(b"\xF3")
    for register, value in ((0, 16), (7, 0x3E), (8, 15), (14, 0)):
        code += cip04_ld_bc(0xFFFD) + cip04_ld_a(register) + CIP04_OUT + cip04_ld_bc(0xBFFD) + cip04_ld_a(value) + CIP04_OUT
    code += cip04_ld_bc(0xFFFD)
    for k, register in enumerate((8, 7, 0, 14)):
        code += cip04_ld_a(register) + CIP04_OUT + b"\xED\x78" + cip04_ld_mem_a(0x9500 + k)
    return bytes(code) + b"\x18\xFE"


def cip04_usr(program: bytes, after: list[str]) -> list[str]:
    """+3 BASIC up, the program poked at 9000h and run (RANDOMIZE USR 36864 typed in full), then `after`."""
    return cip04_start(1) + poke_commands(0x9000, program) + ["type randomize usr 36864", "enter", "frames 120"] + after


def check_cip04_paging() -> None:
    """Ports 7FFD and 1FFD by a machine-code program: a different byte in each of the eight RAM pages (also read through ZR_PAGE_ADDRESS),
    the four special all-RAM configurations (pages 0 1 2 3, 4 5 6 7, 4 5 6 3 and 4 7 6 3 at 0000h to C000h, and a write at 0001h that lands
    in page 0 and in page 4), the four ROMs (7FFD bit 4 low, 1FFD bit 2 high: the bytes of roms/cip04.rom), the lock holding page 7, ROM 3
    and the all-RAM mode off; an AY note whose registers read back (register 14 with its input lines) and which a recording hears."""
    rom = cip04_rom()
    cwd = folder("cip04_paging")
    config = cip04_config(cwd, "cip04")
    answers = cip04_run(config, cip04_usr(cip04_paging_program(), [f"peek {CIP04_RESULTS:X} 80", f"peek {CIP04_RESULTS + 0xFF:X} 1"] +
                                          [f"peek {0x100000 + p * 0x4000:X} 2" for p in range(8)]))
    results = peeked(answers[-10])
    need(results[:8] == bytes(0x40 + n for n in range(8)), f"the eight pages read back: {results[:8].hex(' ')}")
    need(results[0x10:0x20] == bytes([0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x44, 0x45, 0x46, 0x43, 0x44, 0x47, 0x46, 0x43]),
         f"the special configurations (the first bytes at 0000h 4000h 8000h C000h of each): {results[0x10:0x20].hex(' ')}")
    need(results[0x20:0x22] == bytes([0xB0, 0xB3]), f"the writes at 0001h of the special modes: {results[0x20:0x22].hex(' ')}")
    need(results[0x30:0x38] == bytes(rom[k * 0x4000 + o] for k in range(4) for o in (0x100, 0x101)), f"the four ROMs: {results[0x30:0x38].hex(' ')}")
    need(results[0x40] == 0x47 and results[0x41] == rom[0xC000 + 0x100], f"the lock: page {results[0x40]:02X} at C000h, {results[0x41]:02X} at 0100h")
    need(peeked(answers[-9]) == b"\x5A", "the paging program did not finish")
    pages = [peeked(a) for a in answers[-8:]]
    want = [bytes([0x40 + n, 0xB0 if n == 0 else 0xB3 if n == 4 else 0x50 + n]) for n in range(8)]
    need(pages == want, f"the pages through ZR_PAGE_ADDRESS: {[p.hex() for p in pages]}")
    wav = cwd / "tone.wav"
    answers = cip04_run(config, cip04_usr(cip04_ay_program(), ["peek 9500 4", "wait 0.5"]), "--wav", str(wav))
    need(peeked(answers[-2]) == bytes([0x0F, 0x3E, 0x10, 0xC0]), f"the AY's registers read back: {peeked(answers[-2]).hex(' ')}")
    with wave.open(str(wav)) as w:
        samples = struct.unpack(f"<{w.getnframes() * w.getnchannels()}h", w.readframes(w.getnframes()))
        rate = w.getframerate()
    need(rate == 48000 and max(samples) > 2000 and min(samples) < -2000, f"the recording is silent: {max(samples)}, {min(samples)}")


def check_cip04_tape() -> None:
    """A BASIC program on a tape, loaded in 48 BASIC (chosen from the menu) with the ROM tape traps on (the block taken at the loader's entry
    in ROM 3) and off (the tape played in real guest time, loader acceleration on): the same screen, the program's output on it; and a tape
    dropped on the machine is auto-loaded through the menu's Loader entry (Enter), to the same output."""
    cip04_rom()
    cwd = folder("cip04_tape")
    tape = cwd / "demo.tap"
    tape.write_bytes(basic_tap())
    screens = {}
    for switch in ("on", "off"):
        config = cip04_config(cwd, f"traps-{switch}", tape_traps=switch, tape_save_traps=switch)
        play = ["tape play"] if switch == "off" else []
        answers = cip04_run(config, cip04_start(3) + [f"insert tape {tape}", "type j\"\"", "enter", *play, "until 90 42", "frames 100", "text"])
        screens[switch] = answers[-1]["text"]
    need(screen_has(screens["on"], r"Program: HCDEMO") and screen_has(screens["on"], r"^42$"), f"the trapped load: {screens['on']!r}")
    need(screens["on"] == screens["off"], f"the load in real time ends on another screen: {screens['off']!r} for {screens['on']!r}")
    (cwd / "autoload.script").write_text("until 120 42\nframes 100\nexpect Program: HCDEMO\n", encoding="utf-8")
    run = zetro(main_checkout(), "--config", str(cwd / "traps-on.yaml"), "--script", str(cwd / "autoload.script"), "--drop", str(tape))
    need(run.returncode == 0, f"the dropped tape: exit {run.returncode}: {run.stderr.strip()}")


def check_cip04_disk() -> None:
    """The +3's DOS on the machine's uPD765A: it formats a blank DSK made here (FORMAT "A:" on a disk whose tracks hold no sector; the image grows
    to 40 formatted tracks), a one-line program is saved (SAVE "A:TEST"), erased from memory (NEW), loaded (LOAD "A:TEST") and listed (LIST),
    and CAT lists TEST with the free space; the image changed on the host and opens again in a new run whose CAT lists the file. The drive's
    sounds are in the recording with their option on."""
    cip04_rom()
    cwd = folder("cip04_disk")
    disk = cwd / "a.dsk"
    disk.write_bytes(cip04_blank_dsk())
    config = cip04_config(cwd, "cip04")
    # FORMAT and everything after it in one run. The first key after a report makes the +3's editor restore its saved screen (731 characters
    # through its slow print routine: about 0.6 s of guest time in which it reads no key), so the keys typed meanwhile are
    # lost: 1234567890 typed at once does not arrive whole, and the rest of the run then types normally.
    session = Session(main_checkout(), "--config", str(config))
    try:
        session.run(cip04_start(1) + [f"insert a {disk}"] + cip04_line('format "a:"', 2600))
        session.run(["type 1234567890", "frames 45"])
        lost = session.text()
        session.run(["enter", "frames 60"])
        commands = (cip04_line('10 print "hi"') + cip04_line('save "a:test"', 700) + ["text"] +
                    cip04_line("new") + cip04_line('load "a:test"', 700) + cip04_line("list") + ["text"] + cip04_line("cat", 700) + ["text"])
        texts = []
        for command in commands:
            if command == "text":
                texts.append(session.text())
            else:
                session(command)
    finally:
        session.close()
    saved = disk.read_bytes()
    need(len(saved) == 256 + 40 * (256 + 9 * 512) and saved.count(b"\xE5" * 512) >= 9 * 40 - 4, f"FORMAT did not format 40 tracks: {len(saved)} bytes")
    need(not screen_has(lost, r"1234567890") and screen_has(lost, r"^[0-9]{1,9}0$"), f"the keys typed right after FORMAT's report: {lost!r}")
    saved_screen, listing, catalogue = texts
    need(screen_has(saved_screen, r"0 OK"), f"SAVE: {saved_screen!r}")
    need(screen_has(listing, r'10 PRINT "hi"'), f"LIST after NEW and LOAD: {listing!r}")
    need(screen_has(catalogue, r"^TEST\s+\.\s+1K") and screen_has(catalogue, r"\d+K free"), f"CAT: {catalogue!r}")
    need(b"TEST    " in saved, "the image of drive A does not hold the file TEST, or was not written back")
    commands = cip04_start(1) + [f"insert a {disk}"] + cip04_line("cat", 700) + ["text"]
    need(screen_has(cip04_texts(commands, cip04_run(config, commands))[0], r"^TEST\s+\.\s+1K"), "CAT of the image written by the machine, in a new run")
    heard = {}
    for switch in ("on", "off"):
        wav = cwd / f"drive-{switch}.wav"
        cip04_run(cip04_config(cwd, f"sound-{switch}", drive_sound=switch), cip04_start(1) + [f"insert a {disk}"] + cip04_line("cat", 400), "--wav", str(wav))
        with wave.open(str(wav)) as w:
            samples = struct.unpack(f"<{w.getnframes() * w.getnchannels()}h", w.readframes(w.getnframes()))
        heard[switch] = sum(1 for x in samples if x), len(samples)
    need(heard["on"][0] > heard["off"][0] + 0.1 * heard["on"][1], f"the drive sounds: samples heard {heard}")


def check_cip04_state() -> None:
    """A state saved in the middle of a disk operation (the uPD765A in its execution phase with the motor on, found by saving every few frames
    of a LOAD) loads back to the same end, in the same run and in a new one that has the disk in the drive: the controller, the drive, the
    paging ports and the motor are all in the state; the paging program's end state (page 7, ROM 3, the lock) comes back through a state."""
    cip04_rom()
    cwd = folder("cip04_state")
    disk = cwd / "a.dsk"
    disk.write_bytes(cip04_blank_dsk())
    config = cip04_config(cwd, "cip04")
    cip04_run(config, cip04_start(1) + [f"insert a {disk}"] + cip04_line('format "a:"', 2600))
    cip04_run(config, cip04_start(1) + [f"insert a {disk}"] + cip04_line('10 print "hi"') + cip04_line('save "a:test"', 700))
    states = [cwd / f"s{i}.zrs" for i in range(60)]
    saves = [c for s in states for c in ("frames 3", f"state save {s}")]
    answers = cip04_run(config, cip04_start(1) + [f"insert a {disk}", 'type load "a:test"', "enter"] + saves + ["frames 700", "text"])
    end = answers[-1]["text"]
    need(screen_has(end, r"0 OK"), f"LOAD: {end!r}")
    # The controller's chunk: version, rate, clock (9 bytes), then its flags; byte 16 is its phase, 2 the execution phase; the drive's byte 1 the motor.
    busy = [s for s in states if state_chunks(s)["765 "][0][16] == 2 and state_chunks(s)["FDD "][0][1] == 1]
    need(len(busy) >= 3, f"no state of the 60 was saved in the controller's execution phase with the motor on ({len(busy)})")
    middle = busy[len(busy) // 2]
    again = cip04_run(config, cip04_start(1) + [f"insert a {disk}", 'type load "a:test"', "enter", "frames 8", f"state load {middle}", "frames 700", "text"])
    need(again[-1]["text"] == end, f"the state loaded in the same run ends on another screen: {again[-1]['text']!r} for {end!r}")
    fresh = cip04_run(config, [f"insert a {disk}", f"state load {middle}", "frames 700", "text"])
    need(fresh[-1]["text"] == end, f"the state loaded in a new run ends on another screen: {fresh[-1]['text']!r} for {end!r}")
    cip04_run(config, cip04_usr(cip04_paging_program(), [f"state save {cwd / 'paging.zrs'}"]))
    answers = cip04_run(config, [f"state load {cwd / 'paging.zrs'}", "frames 5", f"snapshot save {cwd / 'paging.szx'}", "peek C000 1"])
    spcr = szx_blocks((cwd / "paging.szx").read_bytes())["SPCR"][0]
    need(spcr[1] == 0x37 and spcr[2] & 7 == 0x04 and peeked(answers[-1]) == b"\x47", f"the paging after the state: 7FFD {spcr[1]:02X}, 1FFD {spcr[2]:02X}, C000h {answers[-1]['data']}")


def check_cip04_printer() -> None:
    """LPRINT sends its bytes to the printer port: the trace of the machine's port cycles shows each byte of LPRINT "A" (A, CR, LF) written to
    0FFD with BUSY read clear before it; and the unit test on the machine's interface (tests/test_cip04.c: programs on the CPU send bytes
    through 0FFD and 1FFD, the AY's register 14 and port FE, read one in, and the port host collects them)."""
    cip04_rom()
    cwd = folder("cip04_printer")
    config = cip04_config(cwd, "cip04")
    trace = cwd / "lprint.txt"
    commands = cip04_start(1) + [f"trace {trace}"] + cip04_line('lprint "A"', 200) + ["text", "frames 5"]
    need(screen_has(cip04_texts(commands, cip04_run(config, commands))[0], r"0 OK"), "LPRINT did not end with the report 0 OK")
    sent, ready = [], True
    for kind, port, value in (l.split()[:3] for l in trace.read_text().splitlines() if l.startswith(("IN 0FFD", "OUT 0FFD"))):
        if kind == "IN":
            ready = int(value, 16) & 1 == 0
        else:
            need(ready, "a byte written to 0FFD while BUSY")
            sent.append(int(value, 16))
    need(bytes(sent) == b"A\r\n", f"the bytes written to 0FFD: {bytes(sent)!r}")
    test = EXE.with_name("test_cip04" + EXE.suffix)
    need(test.is_file(), f"{test} is not built")
    run = subprocess.run([str(test)], capture_output=True, text=True, timeout=120)
    need(run.returncode == 0, f"test_cip04: exit {run.returncode}: {run.stdout.strip()}")
    need("ok" in run.stdout, f"the CIP-04's printer and serial ports: {run.stdout.strip()}")


def cip04_z80_pages(data: bytes) -> list[int]:
    found, at = [], 32 + data[30]
    while at < len(data):
        length, page = struct.unpack_from("<HB", data, at)
        found.append(page)
        at += 3 + (16384 if length == 0xFFFF else length)
    return found


def check_cip04_snapshots() -> None:
    """The +3's snapshots: with the paging program's end state (page 7 at C000h, ROM 3, the lock: 7FFD 37h, 1FFD 04h) the machine is saved
    as SZX (machine 5, +3) and Z80 (hardware 7, +3) with both paging ports and the eight pages, and as SNA, which the 128K's format cannot hold
    with ROM 3 (refused, naming 1FFD); in +3 BASIC (1FFD's paging bits clear) as the 128K's SNA. After a power cycle each file loads back to the
    same pages and paging, read from a snapshot taken again; a 48K SNA loads with ROM 3 and the paging locked (7FFD 30h, 1FFD 04h)."""
    cip04_rom()
    cwd = folder("cip04_snapshots")
    config = cip04_config(cwd, "cip04")
    files = {e: cwd / f"pages.{e}" for e in ("szx", "z80")}
    answers = control(main_checkout(), cip04_usr(cip04_paging_program(), [f"snapshot save {f}" for f in files.values()] + [f"snapshot save {cwd / 'pages.sna'}"]) + ["quit"],
                      "--config", str(config))
    need(not answers[-2]["ok"] and "1FFD" in answers[-2]["error"], f"the SNA of ROM 3: {answers[-2]}")
    szx = files["szx"].read_bytes()
    blocks = szx_blocks(szx)
    spcr = blocks["SPCR"][0]
    pages = {b[2]: b[3:] for b in blocks["RAMP"]}
    need(szx[6] == 5 and spcr[1] == 0x37 and spcr[2] & 7 == 0x04 and sorted(pages) == list(range(8)),
         f"the SZX: machine {szx[6]}, 7FFD {spcr[1]:02X}, 1FFD {spcr[2]:02X}, pages {sorted(pages)}")
    need(pages[0][1:2] == b"\xB0" and pages[4][1:2] == b"\xB3" and pages[7][:1] == b"\x47", "the SZX's pages are not the machine's")
    z80 = files["z80"].read_bytes()
    need(z80[34] == 7 and z80[35] == 0x37 and z80[30] == 55 and z80[86] & 7 == 0x04 and sorted(cip04_z80_pages(z80)) == list(range(3, 11)),
         f"the Z80: hardware {z80[34]}, 7FFD {z80[35]:02X}, 1FFD {z80[86]:02X}, header {z80[30]}, pages {cip04_z80_pages(z80)}")
    cip04_run(config, cip04_start(1) + [f"snapshot save {cwd / 'basic.sna'}", f"snapshot save {cwd / 'basic.szx'}"])
    basic = szx_blocks((cwd / "basic.szx").read_bytes())["SPCR"][0]
    sna = (cwd / "basic.sna").read_bytes()
    need(len(sna) == 131103 and sna[27 + 49152 + 2] == basic[1] and basic[2] & 7 == 0, f"the SNA of +3 BASIC: {len(sna)} bytes, 7FFD {sna[27 + 49152 + 2]:02X}")
    for ending, path in (("szx", files["szx"]), ("z80", files["z80"]), ("sna", cwd / "basic.sna")):
        again = cwd / f"again-{ending}.szx"
        answers = cip04_run(config, ["until 10 Loader", "frames 30", f"snapshot load {path}", "frames 3", f"snapshot save {again}", "frames 3", "peek 9400 8"])
        blocks = szx_blocks(again.read_bytes())
        spcr_again = blocks["SPCR"][0]
        want = (0x37, 0x04) if ending != "sna" else (basic[1], 0)
        need((spcr_again[1], spcr_again[2] & 7) == want, f"{ending}: the paging after the load: 7FFD {spcr_again[1]:02X}, 1FFD {spcr_again[2]:02X}")
        if ending != "sna":
            need(peeked(answers[-1]) == bytes(0x40 + n for n in range(8)) and {b[2]: b[3:] for b in blocks["RAMP"]} == pages, f"{ending}: the pages after the load differ")
    (cwd / "small.sna").write_bytes(sna48())
    cip04_run(config, ["until 10 Loader", "frames 30", f"snapshot load {cwd / 'small.sna'}", "frames 5", f"snapshot save {cwd / 'small.szx'}"])
    small = szx_blocks((cwd / "small.szx").read_bytes())["SPCR"][0]
    need(small[1] == 0x30 and small[2] & 7 == 0x04, f"the 48K snapshot's paging: 7FFD {small[1]:02X}, 1FFD {small[2]:02X}")
    (cwd / "cut.szx").write_bytes(szx[:20000])
    answers = control(main_checkout(), ["until 10 Loader", f"snapshot load {cwd / 'cut.szx'}", "quit"], "--config", str(config))
    need(not answers[1]["ok"] and "SZX" in answers[1]["error"], f"a cut SZX: {answers[1]}")


CIP04_PATTERN = (1, 0, 7, 6, 5, 4, 3, 2)


def cip04_delay(t: int) -> int:
    """The +2A/+3 gate array's delay of a contended memory cycle starting at T-state t of the frame (the FAQ: from 14365, 1, 0, 7, 6, 5, 4, 3, 2
    for 128 T-states of each of the 192 picture lines; frames of 70908)."""
    t %= 70908
    line, x = divmod(t - 14365, 228)
    return CIP04_PATTERN[x % 8] if t >= 14365 and line < 192 and x < 128 else 0


def cip04_loop_turns(frame: int, loop: int, contended: bool) -> set[int]:
    """The turns of the INC HL; JR back loop between two frame interrupts as loop_turns works them out, with the gate array: only a cycle with
    MREQ active (the opcode fetches and the displacement's read, not the internal T-states of INC HL and JR) waits."""
    counts = set()
    for j in range(4):
        t, turns = j + 69, 0

        def cycle(t: int, address: int, length: int, mreq: bool) -> int:
            return t + (cip04_delay(t) if contended and mreq and address >= 0xC000 else 0) + length
        while True:
            for address, length, mreq in ((loop, 4, True), (0x8000, 1, False), (0x8000, 1, False)):
                t = cycle(t, address, length, mreq)
            turns += 1
            if t >= frame:
                break
            for address, length, mreq in ((loop + 1, 4, True), (loop + 2, 3, True), *((loop + 2, 1, False),) * 5):
                t = cycle(t, address, length, mreq)
            if t >= frame:
                break
        counts.add(turns)
    return counts


def check_cip04_timing() -> None:
    """The option "Spectrum timing" on the CIP-04 stands for a Sinclair +3: a loop of INC HL; JR back at C000h counts its turns from one frame
    interrupt to the next (a frame of 70908 T-states with the option, the clone's 69888 without), in RAM page 1 (not contended on the +3,
    contended on the 128K) and in page 4 (contended: held by the gate array as the FAQ's pattern says, with the option only, and for memory
    cycles only)."""
    cip04_rom()
    cwd = folder("cip04_timing")
    measured = {}
    for timing in ("original", "spectrum"):
        config = cip04_config(cwd, timing, timing=timing)
        for page in (1, 4):
            answers = cip04_run(config, cip04_start(1) + poke_commands(0x9000, hc128_timing_program(page)) +
                                [f"poke {0x100000 + page * 0x4000:X} 23 18 FD", "type randomize usr 36864", "enter", "frames 20", "peek 81F2 2"])
            low, high = peeked(answers[-1])
            measured[timing, page] = low | high << 8
    plain, plain_frame = loop_turns(69888, 0xC000, False), loop_turns(70908, 0xC000, False)
    held = cip04_loop_turns(70908, 0xC000, True)
    need(len(plain) == 1 and len(plain_frame) == 1 and plain != plain_frame and not held & plain_frame, f"the loop's turns do not tell the cases apart: {plain}, {plain_frame}, {held}")
    for page in (1, 4):
        need(measured["original", page] in plain, f"original timing, page {page}: {measured['original', page]} turns, the clone's frame gives {plain}")
    need(measured["spectrum", 1] in plain_frame, f"Spectrum timing, page 1: {measured['spectrum', 1]} turns, a 70908 T-state frame gives {plain_frame}")
    need(measured["spectrum", 4] in held, f"Spectrum timing, page 4: {measured['spectrum', 4]} turns, the gate array's contention gives {held} ({plain_frame} without)")


def check_cip04_model_groups() -> None:
    """The CIP-04 is one of the CIP group: `zetro --list-models` lists it on the CIP line."""
    run = zetro(main_checkout(), "--list-models")
    need(run.returncode == 0, f"--list-models: exit {run.returncode}: {run.stderr.strip()}")
    lines = [l for l in run.stdout.splitlines() if l.startswith("CIP:")]
    need(len(lines) == 1 and "cip04 (CIP-04)" in lines[0], f"the CIP group of --list-models: {run.stdout!r}")


# --- the TIM-S Plus (P6g) -----------------------------------------------------------------------------------


def timsp_core() -> tuple[Path, Path]:
    """The main checkout (its roms/plus-loader.rom and plus3-system.rom) and the TIM-S core beside it with its runtime case, blank
    disk and demo tape; skipped without either."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / "core-tims"
    for rom in ("plus-loader.rom", "plus3-system.rom"):
        if not (main / "roms" / rom).is_file():
            raise Skipped(f"no {main / 'roms' / rom} (python tools/fetch_roms.py)")
    if (not (main.parent / "MiSTerRO" / "docs" / "tims" / "runtime-cases.json").is_file() or not (core / "assets" / "disks" / "blank.dsk").is_file()
            or not (core / "assets" / "tapes" / "demo.tap").is_file()):
        raise Skipped(f"no TIM-S core with its runtime case, blank disk and demo tape beside the repository ({core})")
    return main, core


def timsp_config(cwd: Path, name: str, **options: str) -> str:
    lines = "".join(f"    {key}: {value}\n" for key, value in options.items())
    (cwd / name).write_text("machine:\n  model: timsplus\n" + (f"  options:\n{lines}" if lines else ""), encoding="utf-8")
    return str(cwd / name)


def timsp_run(config: str, commands: list[str]) -> list[dict]:
    """The commands on the control channel from the main checkout (its ROMs); every answer must be ok."""
    main, _ = timsp_core()
    answers = control(main, commands + ["quit"], "--config", config)
    need(all(a["ok"] for a in answers), f"{[a for a in answers if not a['ok']]}")
    return answers[:-1]


def timsp_menu(entry: int) -> list[str]:
    """From the start: the +3 menu, then its entry `entry` (0 Loader, 1 +3 BASIC, 2 Calculator, 3 48 BASIC) chosen with the cursor
    keys (CAPS SHIFT and 6) and ENTER."""
    down = ["key CAPS SHIFT", "key 6", "frames 3", "release 6", "release CAPS SHIFT", "frames 5"]
    return ["until 10 Calculator", "frames 10"] + down * entry + ["enter", "wait 1"]


TIMSP_ORG, TIMSP_MARK = 0x9000, 0xB000     # the programs run from page 2 (8000h-BFFFh, there in every paging they use)


class TimspCode:
    """A RAM program for +3 BASIC's RANDOMIZE USR: DI, the body, then RPASS! at B000h (on a failure 0EEh there, the step in D at
    B007h and the byte in A at B008h), the board put back as the +3 system has it (port C of the first 8255 11h, 7FFDh and 1FFDh
    from the system's copies BANKM and BANK678), EI, RET."""

    def __init__(self, org: int = TIMSP_ORG) -> None:
        self.org = org
        self.code = bytearray([0xF3])
        self.fixups: list[tuple[int, str]] = []
        self.labels: dict[str, int] = {}

    def here(self) -> int:
        return self.org + len(self.code)

    def b(self, *data: int) -> "TimspCode":
        self.code += bytes(data)
        return self

    def label(self, name: str) -> "TimspCode":
        self.labels[name] = self.here()
        return self

    def jp(self, opcode: int, name: str) -> "TimspCode":
        self.code += bytes([opcode, 0, 0])
        self.fixups.append((len(self.code) - 2, name))
        return self

    def out(self, port: int, value: int) -> "TimspCode":
        return self.b(0x01, port & 0xFF, port >> 8, 0x3E, value, 0xED, 0x79)

    def check_in(self, step: int, port: int, value: int, mask: int = 0xFF) -> "TimspCode":
        self.b(0x16, step, 0x01, port & 0xFF, port >> 8, 0xED, 0x78)
        if mask != 0xFF:
            self.b(0xE6, mask)
        return self.b(0xFE, value).jp(0xC2, "fail")

    def check_mem(self, step: int, address: int, value: int) -> "TimspCode":
        return self.b(0x16, step, 0x3A, address & 0xFF, address >> 8, 0xFE, value).jp(0xC2, "fail")

    def store(self, address: int, value: int) -> "TimspCode":
        return self.b(0x3E, value, 0x32, address & 0xFF, address >> 8)

    def build(self) -> bytes:
        restore = bytes([0x01, 0xFD, 0x0C, 0x3E, 0x11, 0xED, 0x79,              # port C 11h
                         0x3A, 0x5C, 0x5B, 0x01, 0xFD, 0x7F, 0xED, 0x79,        # 7FFDh := BANKM
                         0x3A, 0x67, 0x5B, 0x01, 0xFD, 0x1F, 0xED, 0x79,        # 1FFDh := BANK678
                         0xFB, 0xC9])
        self.b(0x21)
        self.fixups.append((len(self.code), "passed"))
        self.b(0, 0, 0x11, TIMSP_MARK & 0xFF, TIMSP_MARK >> 8, 0x01, 6, 0, 0xED, 0xB0)
        self.code += restore
        self.label("fail")
        self.b(0x32, (TIMSP_MARK + 8) & 0xFF, (TIMSP_MARK + 8) >> 8, 0x7A, 0x32, (TIMSP_MARK + 7) & 0xFF, (TIMSP_MARK + 7) >> 8)
        self.store(TIMSP_MARK, 0xEE)
        self.code += restore
        self.label("passed")
        self.code += b"RPASS!"
        for at, name in self.fixups:
            self.code[at:at + 2] = self.labels[name].to_bytes(2, "little")
        return bytes(self.code)


def timsp_usr(program: bytes, before: list[str] | None = None) -> list[str]:
    """+3 BASIC up, the program poked at 9000h, RANDOMIZE USR 36864 typed (the editor takes keywords spelled out)."""
    return timsp_menu(1) + (before or []) + poke_commands(TIMSP_ORG, program) + ["type randomize usr 36864", "enter"]


def timsp_result(answers: list[dict], what: str) -> None:
    got = peeked(answers[-1])
    need(got[:6] == b"RPASS!", f"{what}: {got.hex(' ')} (EE: failed at step {got[7]:02X}, read {got[8]:02X})")


def timsp_screen_has(text: str, *patterns: str) -> bool:
    return all(any(re.search(p, l) for l in text.splitlines()) for p in patterns)


def check_timsp_menu() -> None:
    """The core's runtime case plus-menu: the TIM-S Plus starts through the replacement bootstrap into the +3 system's menu (Loader and
    Calculator on the screen after the case's seconds; a screenshot of it); +3 BASIC from the menu, PRINT 2+2 gives 4."""
    main, _ = timsp_core()
    cases = json.loads((main.parent / "MiSTerRO" / "docs" / "tims" / "runtime-cases.json").read_text(encoding="utf-8"))
    case = cases["plus-menu"]
    seconds = float(case["arguments"][case["arguments"].index("--seconds") + 1])
    cwd = folder("timsp_menu")
    config = timsp_config(cwd, "plus.yaml")
    shot = cwd / "menu.png"
    answers = timsp_run(config, [f"wait {seconds}", "text", f"shot {shot}"])
    need(timsp_screen_has(answers[1]["text"], *case["screen"]), f"the menu: {answers[1]['text']!r}")
    need(png_rows(shot)[115][200] == (0, 0xFF, 0xFF), "the menu's highlighted entry is not cyan in the screenshot")
    answers = timsp_run(config, timsp_menu(1) + ["type print 2+2", "enter", "until 5 0 OK", "text"])
    lines = [l.strip() for l in answers[-1]["text"].splitlines()]
    need("4" in lines and any(l.startswith("0 OK") for l in lines), f"+3 BASIC, PRINT 2+2: {answers[-1]['text']!r}")


def check_timsp_tape() -> None:
    """The demo tape loaded with LOAD "" in the +3 menu's 48 BASIC, in real time with the ROM tape traps off and through them, to the
    same screen (TIM-S DEMO and 42)."""
    _, core = timsp_core()
    cwd = folder("timsp_tape")
    tape = (core / "assets" / "tapes" / "demo.tap").resolve()
    screens = {}
    for traps in ("off", "on"):
        config = timsp_config(cwd, f"traps_{traps}.yaml", tape_traps=traps, tape_save_traps=traps)
        play = ["tape play"] if traps == "off" else []
        answers = timsp_run(config, [f"insert tape {tape}"] + timsp_menu(3) + ["wait 1", 'type j""', "enter"] + play + ["until 40 STOP", "text"])
        screens[traps] = answers[-1]["text"]
        need(timsp_screen_has(screens[traps], "TIM-S DEMO", r"^\s*42\s*$"), f"traps {traps}: {screens[traps]!r}")
    need(screens["off"] == screens["on"], f"the traps on and off end on other screens: {screens}")


def timsp_disk(cwd: Path, name: str) -> Path:
    _, core = timsp_core()
    disk = cwd / name
    shutil.copyfile(core / "assets" / "disks" / "blank.dsk", disk)
    return disk


def check_timsp_disk() -> None:
    """A disk through the +3 system on a copy of blank.dsk (the Plus's 720K format, which the system reads): FORMAT "A:" is refused
    with "Unsuitable media" (the unmodified +3 system cannot put its 40-track format on the Plus's 80-track drive: docs/NOTES.md);
    SAVE "A:T" of a program, NEW, LOAD "A:T", CAT lists T, RUN prints it; the image changed on the host and opens again in a new run,
    where CAT still lists the file."""
    cwd = folder("timsp_disk")
    disk = timsp_disk(cwd, "a.dsk")
    original = disk.read_bytes()
    config = timsp_config(cwd, "plus.yaml")
    answers = timsp_run(config, [f"insert a {disk}"] + timsp_menu(1) + [
        'type format "a:"', "enter", "until 60 Unsuitable media", "text", "enter", "wait 1",
        'type 10 print "hello"', "enter", "wait 1", 'type save "a:t"', "enter", "until 60 0 OK", "text", "enter", "wait 1",
        "type new", "enter"] + timsp_menu(1) + ['type load "a:t"', "enter", "until 60 0 OK", "enter", "wait 1",
        "type cat", "enter", "until 60 free", "text", "enter", "wait 1", "type run", "enter", "until 5 0 OK", "text"])
    need("Unsuitable media" in answers[len(timsp_menu(1)) + 4]["text"], "FORMAT \"A:\"")
    catalogue, run = answers[-7]["text"], answers[-1]["text"]
    need(timsp_screen_has(catalogue, r"^T +. ", r"K free"), f"CAT after LOAD \"A:T\": {catalogue!r}")
    need(timsp_screen_has(run, r"^hello$"), f"RUN after LOAD: {run!r}")
    need(disk.read_bytes() != original, "the image on the host did not change")
    again = timsp_run(config, [f"insert a {disk}"] + timsp_menu(1) + ["type cat", "enter", "until 60 free", "text"])
    need(timsp_screen_has(again[-1]["text"], r"^T +. "), f"CAT in a new run: {again[-1]['text']!r}")


def check_timsp_ports() -> None:
    """The parts of the core's native diagnostic ports-plus (diagnostics.py), as RAM programs run from +3 BASIC: (a) the twelve RAM banks
    (BR0 and BR1 by 7FFDh with PC2 low, BR2 with it high) and the four 16 KiB blocks of the video RAM (7FFDh bit 2 with PC2), each with
    its own byte, the system's bytes put back; (b) MICRONEXT: a byte at the address of xx0Fh, xx1Fh, xx2Fh read back at xx3Fh, another
    address reads 00h; (c) both 8253s count (a counter latched and read twice); (d) the native 8251 and the four expansion channels the
    multiplexer at AFFDh selects take a mode and a command and report TxRDY and TxEMPTY (the loopback is test_timsplus's, timsp_io);
    (e) the AY's register read back at FFFDh; (f) the 8272 on a copy of blank.dsk: recalibrate, seek, FORMAT a track, WRITE a sector of
    B6h, READ it back; (g) the network: F7h reads FFh with the line idle, F3h arms the NMI (in the state), which never comes; (h) the
    keyboard: BACKSPACE held reads CAPS SHIFT and 0 on the Spectrum rows and its own line on D7 (row A11); in the CP/M mode (PC4 low)
    the Spectrum rows do not see it and D7 does."""
    _, core = timsp_core()
    cwd = folder("timsp_ports")
    config = timsp_config(cwd, "plus.yaml")
    # (a) banks and video blocks
    p = TimspCode()
    for bank in range(12):
        p.out(0x0CFD, 0x13 if bank < 8 else 0x17).out(0x7FFD, bank & 7 if bank < 8 else bank - 8)
        p.b(0x3A, 0x23, 0xC1, 0x32, (0xB100 + bank) & 0xFF, 0xB1)            # LD A,(C123h); LD (B100h+bank),A
        p.store(0xC123, 0x40 + bank)
    for page in range(4):
        p.out(0x0CFD, 0x17).out(0x7FFD, 4 + page).b(0x3A, 0x00, 0xC1, 0x32, 0x10 + page, 0xB1).store(0xC100, 0xA0 + page)
    for bank in range(12):
        p.out(0x0CFD, 0x13 if bank < 8 else 0x17).out(0x7FFD, bank & 7 if bank < 8 else bank - 8).check_mem(bank, 0xC123, 0x40 + bank)
        p.b(0x3A, (0xB100 + bank) & 0xFF, 0xB1, 0x32, 0x23, 0xC1)
    for page in range(4):
        p.out(0x0CFD, 0x17).out(0x7FFD, 4 + page).check_mem(0x10 + page, 0xC100, 0xA0 + page).b(0x3A, 0x10 + page, 0xB1, 0x32, 0x00, 0xC1)
    answers = timsp_run(config, timsp_usr(p.build()) + ["frames 20", f"peek {TIMSP_MARK:04X} 9", "peek 12C000 3", "peek 131800 1"])
    timsp_result(answers[:-2], "(a) banks")
    need(peeked(answers[-2]) == b"\xf3\xaf\x11" and peeked(answers[-1]) == b"\x38",
         f"(a) BR2 and the video RAM through ZR_PAGE_ADDRESS (pages 11 and 12): {answers[-2]} {answers[-1]}")
    # (b) MICRONEXT, (c) the 8253s, (d) the 8251s, (e) the AY
    p = TimspCode()
    for port, value in ((0x0F, 0x34), (0x1F, 0x12), (0x2F, 0x3F)):
        p.out(port, value)
    p.out(0x3F, 0x6D).check_in(1, 0x3F, 0x6D).out(0x2F, 0x00).check_in(2, 0x3F, 0x00).out(0x2F, 0x3F).check_in(3, 0x3F, 0x6D)
    for step, control, counter in ((4, 0x9FFD, 0x9EFD), (5, 0x8FFD, 0x8CFD), (6, 0x8FFD, 0x8EFD)):
        select = ((counter >> 8) & 3) << 6
        p.out(control, select | 0x34).out(counter, 0).out(counter, 0)              # mode 2, 65536
        p.b(0x06, 0x00, 0x10, 0xFE)                                               # a short delay (DJNZ)
        p.out(control, select).check_in(step, counter, 0x00, 0x00)                # latch; the low byte
        p.check_in(step + 0x10, counter, 0xF0, 0xF0)                              # the high byte: counting down from 65536, not far yet
    p.out(0x9FFD, 0x36).out(0x9CFD, 13).out(0x9CFD, 0)                            # the native channel's clock again, as the bootstrap sets it
    for i, (data, command) in enumerate([(0xDEFD, 0xDFFD)] + [(0xCFFD, 0xEFFD)] * 4):
        if i:
            p.out(0xAFFD, i - 1)
        p.out(command, 0).out(command, 0).out(command, 0).out(command, 0x40).out(command, 0x4E).out(command, 0x37)
        p.check_in(0x20 + i, command, 0x05, 0x05)
    p.out(0xFFFD, 0).out(0xBFFD, 0x55).check_in(0x30, 0xFFFD, 0x55).out(0xBFFD, 0)
    answers = timsp_run(config, timsp_usr(p.build()) + ["frames 20", f"peek {TIMSP_MARK:04X} 9"])
    timsp_result(answers, "(b)-(e) MICRONEXT, 8253s, 8251s, AY")
    # (f) the 8272: Specify (non-DMA), motor on, recalibrate, seek to cylinder 2, FORMAT it (9 sectors of 512), WRITE sector 1 of B6h,
    # READ it back. The bytes of the execution phase go in tight loops (a byte every 32 us at 250 kbit/s).
    p = TimspCode().jp(0xC3, "main")
    p.label("send").b(0xF5, 0x01, 0xFD, 0x2F).label("send_wait").b(0xED, 0x78, 0xE6, 0xC0, 0xFE, 0x80).jp(0xC2, "send_wait")
    p.b(0xF1, 0x01, 0xFD, 0x3F, 0xED, 0x79, 0xC9)                                  # PUSH AF; wait RQM, DIO 0; POP AF; OUT (3FFDh),A
    p.label("get").b(0x01, 0xFD, 0x2F).label("get_wait").b(0xED, 0x78, 0xE6, 0xC0, 0xFE, 0xC0).jp(0xC2, "get_wait")
    p.b(0x01, 0xFD, 0x3F, 0xED, 0x78, 0xC9)                                       # wait RQM, DIO 1; IN A,(3FFDh)
    p.label("sense").b(0x3E, 0x08).jp(0xCD, "send").jp(0xCD, "get").b(0x57, 0xFE, 0x80, 0xC8)              # ST0 to D (80h: no interrupt)
    p.jp(0xCD, "get").b(0x5F, 0xC9)                                               # PCN to E
    p.label("results").jp(0xCD, "get")                                            # ST0 (abnormal: EOT came without TC)
    p.jp(0xCD, "get").b(0xE6, 0x7F, 0x16, 0x50).jp(0xC2, "fail")                  # ST1 but EN
    p.jp(0xCD, "get").b(0xE6, 0xBF, 0x16, 0x51).jp(0xC2, "fail")                  # ST2 but CM
    p.jp(0xCD, "get").jp(0xCD, "get").jp(0xCD, "get").jp(0xCD, "get").b(0xC9)    # C H R N
    # out: DE bytes from HL to the data register as the execution phase asks for them; in: DE bytes from it to HL
    p.label("out").b(0x01, 0xFD, 0x2F).label("out_wait").b(0xED, 0x78).jp(0xF2, "out_wait")
    p.b(0x06, 0x3F, 0x7E, 0xED, 0x79, 0x06, 0x2F, 0x23, 0x1B, 0x7A, 0xB3).jp(0xC2, "out_wait").b(0xC9)
    p.label("in").b(0x01, 0xFD, 0x2F).label("in_wait").b(0xED, 0x78).jp(0xF2, "in_wait")
    p.b(0x06, 0x3F, 0xED, 0x78, 0x77, 0x06, 0x2F, 0x23, 0x1B, 0x7A, 0xB3).jp(0xC2, "in_wait").b(0xC9)
    p.label("main")
    ids = bytes(b for r in range(1, 10) for b in (2, 0, r, 2))
    p.b(0x21, 0x00, 0xB4, 0x36, 0xB6, 0x11, 0x01, 0xB4, 0x01, 0xFF, 0x01, 0xED, 0xB0)    # B400h: 512 bytes of B6h
    for i, v in enumerate(ids):
        p.store(0xB200 + i, v)                                                    # B200h: the IDs of the track
    p.b(0x3A, 0x67, 0x5B, 0xF6, 0x08, 0x01, 0xFD, 0x1F, 0xED, 0x79)              # 1FFDh := BANK678 | 08h: the motor on
    for v in (0x03, 0xDF, 0x03, 0x07, 0x00):                                      # Specify (non-DMA); Recalibrate drive 0
        p.b(0x3E, v).jp(0xCD, "send")
    p.label("recal").jp(0xCD, "sense").b(0x7A, 0xE6, 0x20).jp(0xCA, "recal")     # until its interrupt (SE)
    for v in (0x0F, 0x00, 0x02):                                                  # Seek to cylinder 2
        p.b(0x3E, v).jp(0xCD, "send")
    p.label("seek").jp(0xCD, "sense").b(0x7A, 0xE6, 0x20).jp(0xCA, "seek")
    p.b(0x7B, 0x16, 0x40, 0xFE, 0x02).jp(0xC2, "fail")                            # PCN 2
    for v in (0x4D, 0x00, 0x02, 0x09, 0x52, 0xE5):                                # FORMAT: MFM, N 2, 9 sectors, gap 52h, filler E5h
        p.b(0x3E, v).jp(0xCD, "send")
    p.b(0x21, 0x00, 0xB2, 0x11, len(ids), 0x00).jp(0xCD, "out").jp(0xCD, "results")
    for v in (0x45, 0x00, 0x02, 0x00, 0x01, 0x02, 0x01, 0x2A, 0xFF):              # WRITE DATA C2 H0 R1 N2, EOT 1
        p.b(0x3E, v).jp(0xCD, "send")
    p.b(0x21, 0x00, 0xB4, 0x11, 0x00, 0x02).jp(0xCD, "out").jp(0xCD, "results")
    p.b(0x21, 0x00, 0xB4, 0x36, 0x00, 0x11, 0x01, 0xB4, 0x01, 0xFF, 0x01, 0xED, 0xB0)    # the buffer cleared
    for v in (0x46, 0x00, 0x02, 0x00, 0x01, 0x02, 0x01, 0x2A, 0xFF):              # READ DATA
        p.b(0x3E, v).jp(0xCD, "send")
    p.b(0x21, 0x00, 0xB4, 0x11, 0x00, 0x02).jp(0xCD, "in").jp(0xCD, "results")
    p.b(0x21, 0x00, 0xB4, 0x01, 0x00, 0x02, 0x16, 0x60)                          # every byte read is B6h
    p.label("cmp").b(0x7E, 0xFE, 0xB6).jp(0xC2, "fail").b(0x23, 0x0B, 0x78, 0xB1).jp(0xC2, "cmp")
    disk = timsp_disk(cwd, "a.dsk")
    answers = timsp_run(config, [f"insert a {disk}"] + timsp_usr(p.build()) + ["until 20 0 OK", f"peek {TIMSP_MARK:04X} 9", "wait 6"])
    timsp_result(answers[:-1], "(f) the 8272")
    # (g) the network
    p = TimspCode().check_in(1, 0x00F7, 0xFF).out(0x00F3, 0)
    state = cwd / "armed.zrs"
    answers = timsp_run(config, timsp_usr(p.build()) + ["frames 50", f"peek {TIMSP_MARK:04X} 9", f"state save {state}", "text"])
    timsp_result(answers[:-2], "(g) the network")
    need(state_chunks(state)["TIMP"][0][9] == 1, "(g) F3h did not arm the network NMI")
    need(timsp_screen_has(answers[-1]["text"], "0 OK"), f"(g) an NMI came: {answers[-1]['text']!r}")
    # (h) the keyboard: wait for a key on row A8, then the rows in Spectrum mode, then in CP/M mode (7FFDh 02h keeps page 2 at 8000h
    # there; DI: the EPROM is at 0000h in it)
    p = TimspCode().label("wait").b(0x01, 0xFE, 0xFE, 0xED, 0x78, 0xE6, 0x1F, 0xFE, 0x1F).jp(0xCA, "wait")
    p.check_in(1, 0xFEFE, 0xFE).check_in(2, 0xEFFE, 0xFE).check_in(3, 0xF7FE, 0x00, 0x80).check_in(4, 0xFBFE, 0xA0, 0xA0)
    p.out(0x7FFD, 0x02).out(0x0CFD, 0x00)
    p.check_in(5, 0xFEFE, 0xFF).check_in(6, 0xEFFE, 0xFF).check_in(7, 0xF7FE, 0x7F).out(0x0CFD, 0x11)
    answers = timsp_run(config, timsp_usr(p.build()) + ["frames 20", "key BACKSPACE", "frames 10", "release BACKSPACE", "frames 10",
                                                       f"peek {TIMSP_MARK:04X} 9"])
    timsp_result(answers, "(h) the keyboard's added lines")


def check_timsp_video() -> None:
    """The video modes on a +3 BASIC program's screen (eight lines printed, then the modes set by RAM programs, each followed by PAUSE 0):
    the 512-pixel window (PA2 set) with the hardware scroll at 32 lines (5CFDh) and the video page 1 a copy of page 0, so each character
    shows twice with its bytes interleaved and the text has moved up four rows; then the 640-pixel window (PA1 clear) with the attribute
    inhibit (PA0). Screenshots of both (work/verify/timsp_video), the text read back in 64 and 80 columns."""
    cwd = folder("timsp_video")
    config = timsp_config(cwd, "plus.yaml")
    copy = TimspCode().out(0x0CFD, 0x15).out(0x7FFD, 0x04)                     # C000h: the video RAM's first 16 KiB
    copy.b(0x21, 0x00, 0xC0, 0x11, 0x00, 0xE0, 0x01, 0x00, 0x1B, 0xED, 0xB0)     # page 0 to page 1
    copy.out(0x0EFD, 0x06).out(0x5CFD, 0x20)                                     # 512 pixels, the scroll at 32
    wide = TimspCode(0xA000).out(0x0EFD, 0x05).out(0x5CFD, 0x00)                 # 640 pixels (PA1 clear, PA2 set), attributes inhibited
    line = ('type 10 for n=0 to 7: print n;" TIM-S PLUS VIDEO": next n: pause 50: randomize usr 36864: pause 0: '
            'randomize usr 40960: pause 0')
    commands = timsp_menu(1) + poke_commands(TIMSP_ORG, copy.build()) + poke_commands(0xA000, wide.build()) + [
        line, "enter", "wait 1", "type run", "enter", "wait 0.5", "text", "wait 1.5", "text", f"shot {cwd / 'hires.png'}",
        "enter", "wait 1", "text", f"shot {cwd / 'wide.png'}"]
    answers = timsp_run(config, commands)
    before, after, wide_text = (answers[i]["text"].splitlines() for i in (-8, -6, -2))
    need(len(before) > 4 and before[4].startswith("4 TIM-S PLUS VIDEO"), f"the screen before: {before!r}")
    need(after and after[0].startswith("44  TTIIMM--SS  PPLLUUSS"), f"512 pixels with the scroll at 4 rows: {after!r}")
    need(any(len(l) > 64 for l in wide_text), f"640 pixels: the text read back in 80 columns: {wide_text!r}")
    for name in ("hires.png", "wide.png"):
        rows = png_rows(cwd / name)
        need(len(rows) == 288 and len(rows[0]) == 704, f"{name}: {len(rows[0])} x {len(rows)}")


def check_timsp_states() -> None:
    """States: (a) in the middle of a disk operation: LOAD "A:T" of a program saved before, the state taken while the 8272 is in a
    command's execution phase (its chunk says so), goes on to the same screen (RUN prints the program's line) in the same run and in a
    new one; (b) with a timer running: a line in FLASH, whose phase the 8253's counter 1 gives (the bootstrap programs it), the state
    taken between two of its edges, gives the same picture half a second later in the same run and in a new one."""
    cwd = folder("timsp_states")
    config = timsp_config(cwd, "plus.yaml")
    disk = timsp_disk(cwd, "a.dsk")
    mid = cwd / "mid.zrs"
    load = ['type load "a:t"', "enter"]
    rest = ["until 60 0 OK", "enter", "wait 1", "type run", "enter", "until 5 0 OK", "text"]
    answers = timsp_run(config, [f"insert a {disk}"] + timsp_menu(1) + [
        'type 10 print "state"', "enter", "wait 1", 'type save "a:t"', "enter", "until 60 0 OK", "enter", "wait 1",
        "type new", "enter"] + timsp_menu(1) + load + ["frames 56", f"state save {mid}"] + rest + [f"state load {mid}"] + rest)
    first, again = answers[-9]["text"], answers[-1]["text"]
    fdc = state_chunks(mid)["FDC "][0]
    need(fdc[16] == 2, f"(a) the state was not taken in a command's execution phase (phase {fdc[16]})")
    need(timsp_screen_has(first, r"^state$") and again == first, f"(a) the state loaded in the same run: {again!r} for {first!r}")
    new = timsp_run(config, [f"insert a {disk}", f"state load {mid}"] + rest)
    need(new[-1]["text"] == first, f"(a) the state in a new run: {new[-1]['text']!r}")
    # (b) FLASH from the timer
    timer = cwd / "timer.zrs"
    shots = [cwd / f"flash{i}.png" for i in range(3)]
    timsp_run(config, timsp_menu(1) + ['type print flash 1;"FLASH FROM THE 8253"', "enter", "wait 1.1", f"state save {timer}",
                                       "frames 25", f"shot {shots[0]}", f"state load {timer}", "frames 25", f"shot {shots[1]}"])
    timsp_run(config, [f"state load {timer}", "frames 25", f"shot {shots[2]}"])
    need(state_chunks(timer)["TVID"][0][7] == 1, "(b) FLASH was not the timer's")
    pictures = [png_rows(s) for s in shots]
    need(pictures[0] == pictures[1] == pictures[2], "(b) the pictures after the state with the timer running differ")
    later = cwd / "later.png"
    timsp_run(config, [f"state load {timer}", "frames 8", f"shot {later}"])
    need(png_rows(later) != pictures[0], "(b) FLASH did not change in 16 frames: the check does not see the timer")


def check_timsp_io() -> None:
    """The TIM-S Plus's video unit check and its byte ports from the host's side: tests/test_timsplus.c (ctest runs it too): the
    video's resolutions, scroll, wide window, inhibit, interrupt and state against a fake host; each of the five 8251 channels (the
    "UART channel" option, the multiplexer, its 8253 counter as the clock) sends a byte through a loopback plug and receives it with DSR
    up; a byte of 0FFDh reaches the printer port at the strobe."""
    timsp_core()
    exe = EXE.with_name("test_timsplus.exe" if os.name == "nt" else "test_timsplus")
    if not exe.is_file():
        raise Skipped(f"{exe} is not built")
    (ROOT / "work" / "tests").mkdir(parents=True, exist_ok=True)
    run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=300)
    need(run.returncode == 0 and "test_timsplus passed" in run.stdout and "skipped" not in run.stdout, f"exit {run.returncode}: {run.stdout.strip()}")



# --- the CUB Z (core/machines/cubz.c) ----------------------------------------------------------------------------------


CUBZ_DISK_SHA256 = "7ea2239733f01cbe70c4341fe40fc2600ff99128c33f5d8000582cb32d475d8a"


def cubz_case() -> tuple[Path, Path]:
    """The CUB Z's validation case from its core: the main checkout (roms/cubz-v14.rom, cubz-v13.rom) and the core's original
    CP/M disk (core-cubz/assets/disks/cpm.dsk). Skipped without the ROMs or the MiSTerRO checkout."""
    main = main_checkout()
    disk = main.parent / "MiSTerRO" / "src" / "core-cubz" / "assets" / "disks" / "cpm.dsk"
    for rom in ("cubz-v14.rom", "cubz-v13.rom"):
        if not (main / "roms" / rom).is_file():
            raise Skipped(f"no {main / 'roms' / rom} (python tools/fetch_roms.py)")
    if not disk.is_file():
        raise Skipped(f"no CUB Z core with its CP/M disk beside the repository ({disk})")
    return main, disk


def cubz_folder(name: str, disk: Path | None, model: str = "cubz14") -> Path:
    """A work folder with a configuration of the CUB Z and, when given, its own copy of a disk in drive A."""
    cwd = folder(name)
    text = f"machine:\n  model: {model}\n"
    if disk is not None:
        shutil.copyfile(disk, cwd / "cpm.dsk")
        text += f"media:\n  a: {{ image: {(cwd / 'cpm.dsk').as_posix()} }}\n"
    (cwd / "zetro.yaml").write_text(text, encoding="utf-8")
    return cwd


def cubz_run(main: Path, cwd: Path, commands: list[str]) -> list[dict]:
    """The commands on the control channel, from the main checkout (its roms/ is found); every file named is in cwd."""
    answers = control(main, commands + ["quit"], "--config", str(cwd / "zetro.yaml"))[:-1]
    for command, answer in zip(commands, answers):
        need(answer.get("ok"), f"{command!r}: {answer}")
    return answers


def cubz_texts(commands: list[str], answers: list[dict]) -> list[str]:
    return [a["text"] for c, a in zip(commands, answers) if c == "text"]


def edsk_tracks(data: bytes) -> list[tuple[int, int, list[tuple[int, int, int, int, bytes]]]]:
    """The tracks of an Extended DSK: (track block size, its index, [(R, ST1, ST2, stored length, data)]); a missing track has
    size 0 and no sectors."""
    need(data[:34] == b"EXTENDED CPC DSK File\r\nDisk-Info\r\n", "not an Extended DSK")
    count = data[0x30] * data[0x31]
    tracks, offset = [], 0x100
    for i in range(count):
        size = data[0x34 + i] * 256
        sectors = []
        if size:
            block = data[offset:offset + size]
            at = 0x100
            for k in range(block[0x15]):
                e = block[0x18 + 8 * k:0x20 + 8 * k]
                length = e[6] | e[7] << 8
                sectors.append((e[2], e[4], e[5], length, block[at:at + length]))
                at += length
            offset += size
        tracks.append((size, i, sectors))
    return tracks


def cubz_odd_tracks(data: bytes) -> list[tuple]:
    """What the CUB Z's disk keeps that a plain image would not: its missing tracks, its oversized track and its weak sector
    (the sizes, the sector IDs and statuses, the weak sector's copies)."""
    odd = []
    for size, i, sectors in edsk_tracks(data):
        if size == 0:
            odd.append((i, "missing"))
        elif size != 0x1300 or any(length != 512 or st1 or st2 for _, st1, st2, length, _ in sectors):
            odd.append((i, size, [(r, st1, st2, length, hashlib.sha256(d).hexdigest()) for r, st1, st2, length, d in sectors if length != 512 or st1 or st2]))
    return odd


def cubz_boot_bytes(data: bytes) -> bytes:
    """The 2048 bytes the monitor's DMA read takes from the disk: sectors 1-4 of track 0, side 0."""
    _, _, sectors = edsk_tracks(data)[0]
    by_r = {r: d for r, _, _, _, d in sectors}
    return b"".join(by_r[r] for r in (1, 2, 3, 4))


def cubz_boot_loaded(loaded: bytes, disk: bytes) -> bool:
    """The 2048 bytes at 3000h after the boot are the disk's first four sectors, but for the result buffer (30C0h-30C6h) that
    the boot sector's own program fills with the 8272's results."""
    boot = cubz_boot_bytes(disk)
    return len(loaded) == len(boot) and all(a == b or 0xC0 <= i < 0xC7 for i, (a, b) in enumerate(zip(loaded, boot)))


def check_cubz_monitor() -> None:
    """Both monitor EPROM sets start with no disk (the boot alias, the PIO's set-up, the RAM test, the font copied) to their
    banner and prompt and echo a command typed on the serial keyboard link, as the core's runtime validation has it. The
    screenshots are work/verify/cubz_monitor/monitor14.png and monitor13.png."""
    main, _ = cubz_case()
    for model, banner in (("cubz14", "cubZ V1.4"), ("cubz13", "cubZ V1.3")):
        cwd = cubz_folder("cubz_monitor", None, model) if model == "cubz14" else cwd
        (cwd / "zetro.yaml").write_text(f"machine:\n  model: {model}\n", encoding="utf-8")
        shot = cwd / f"monitor{model[4:]}.png"
        commands = ["until 10 " + banner, "wait 0.5", "type D 0000", "wait 1", "text", f"shot {shot}"]
        answers = cubz_run(main, cwd, commands)
        lines = cubz_texts(commands, answers)[0].splitlines()
        need(lines[:2] == [banner, ".D 0000"], f"{model}: the monitor's screen: {lines[:4]!r}")
        width, height, colours = png_colours(shot)
        need((width, height) == (544, 288) and colours == 2, f"{model}: the frame is {width} x {height}, {colours} colours")


def cubz_cpm_commands(cwd: Path) -> list[str]:
    """The core's runtime validation of CP/M (test_runtime.py and its simulator's --exercise-turbo): DIR, Turbo Pascal (its
    sign-on, N to the error messages question, its menu, Q), SAVE 1 CUBZTEST.COM and DIR CUBZTEST.COM."""
    return ["until 30 A>", "peek 3000 400", "peek 3400 400",
            "type DIR\\n", "until 20 FISP     DAT", "wait 1", "text",
            "type TURBO\\n", "until 40 Include error messages", "type N", "until 30 Logged drive", "until 10 >", "wait 1", "text",
            f"shot {cwd / 'turbo.png'}", "type Q", "until 10 A>", "wait 2",
            "type SAVE 1 CUBZTEST.COM\\n", "wait 6", "type DIR CUBZTEST.COM\\n", "until 20 A: CUBZTEST COM", "wait 1", "text",
            f"shot {cwd / 'cpm.png'}"]


def check_cubz_cpm() -> None:
    """With the original CP/M disk in drive A the monitor reads the boot sectors through the 8272 and the 8257 (DMA with bus
    request and terminal count: the 2048 bytes at 3000h are the disk's first four sectors), 57K CP/M 2.2 starts, DIR lists the
    disk, Turbo Pascal 3.00A starts and leaves, SAVE writes a file that DIR finds. The image on the host has changed, opens
    again and keeps its missing tracks, its oversized track and its weak sector; a new run with it finds the file, erases it
    (ERA) and no longer finds it, and the image changes again. Screenshots: work/verify/cubz_cpm/turbo.png and cpm.png."""
    main, disk = cubz_case()
    original = disk.read_bytes()
    need(hashlib.sha256(original).hexdigest() == CUBZ_DISK_SHA256, "the core's CP/M disk is not the original")
    cwd = cubz_folder("cubz_cpm", disk)
    trace = cwd / "boot.trace"
    commands = [f"trace {trace}", "until 30 A>", "trace off"] + cubz_cpm_commands(cwd)[1:]
    answers = cubz_run(main, cwd, commands)
    loaded = peeked(answers[commands.index("peek 3000 400")]) + peeked(answers[commands.index("peek 3400 400")])
    need(cubz_boot_loaded(loaded, original), "the bytes at 3000h are not the disk's boot sectors")
    io = trace.read_text().splitlines()
    outs = [(int(p[1], 16) & 0xFF, int(p[2], 16)) for p in (l.split() for l in io) if p and p[0] == "OUT"]
    need((0x88, 0x64) in outs and (0x84, 0x30) in outs, "the monitor did not set the 8257's channel 2 up (mode 64h, address 3000h)")
    texts = cubz_texts(commands, answers)
    need("57K CP/M ver 2.2/4" in texts[0] and "TURBO    COM : TURBO    MSG : TURBO    OVR" in texts[0], f"DIR: {texts[0]!r}")
    for expected in ("TURBO Pascal system       Version 3.00A", "Include error messages (Y/N)? N", "Logged drive: A",
                     "eXecute  Dir      Quit  compiler Options"):
        need(expected in texts[1], f"{expected!r} is not on Turbo Pascal's screen: {texts[1]!r}")
    need("A>DIR CUBZTEST.COM\nA: CUBZTEST COM" in texts[2], f"the saved file: {texts[2]!r}")
    saved = (cwd / "cpm.dsk").read_bytes()
    need(saved != original, "the disk image has not changed on the host")
    need(cubz_odd_tracks(saved) == cubz_odd_tracks(original), "the image lost its missing tracks, its oversized track or its weak sector")
    commands = ["until 30 A>", "type DIR CUBZTEST.COM\\n", "until 20 A: CUBZTEST COM", "wait 2", "type ERA CUBZTEST.COM\\n", "wait 6",
                "type DIR CUBZTEST.COM\\n", "until 20 NO FILE", "wait 1", "text"]
    texts = cubz_texts(commands, cubz_run(main, cwd, commands))
    need("A>DIR CUBZTEST.COM\nA: CUBZTEST COM" in texts[0] and "A>ERA CUBZTEST.COM\nA>DIR CUBZTEST.COM\nNO FILE" in texts[0],
         f"the reboot, ERA: {texts[0]!r}")
    erased = (cwd / "cpm.dsk").read_bytes()
    need(erased != saved and cubz_odd_tracks(erased) == cubz_odd_tracks(original), "the erasure did not reach the image, or the image lost its odd tracks")
    need(hashlib.sha256(disk.read_bytes()).hexdigest() == CUBZ_DISK_SHA256, "the core's own disk changed")


def check_cubz_state() -> None:
    """A state saved in the middle of the boot's DMA transfer (the first sector is at 3000h, the last not yet) goes on, in the
    same run and in a fresh one, to the same CP/M prompt with the boot sectors whole; and a state saved with a CTC interrupt
    pending on the daisy chain (a timer channel asking while the program runs with interrupts disabled) takes the interrupt
    after the load as the run without the load does (the same count of interrupts served)."""
    main, disk = cubz_case()
    original = disk.read_bytes()
    cwd = cubz_folder("cubz_state", disk)
    probe = []
    for _ in range(120):
        probe += ["frames 1", "peek 3000 2", "peek 37FE 2"]
    answers = cubz_run(main, cwd, probe)
    middle = next((answers[i]["frame"] for i in range(0, len(probe), 3)
                   if peeked(answers[i + 1]) != b"\0\0" and peeked(answers[i + 2]) == b"\0\0"), None)
    need(middle is not None, "no frame finds the boot's DMA transfer half done")
    state = cwd / "dma.zrs"
    commands = [f"frames {middle}", "peek 3000 2", "peek 37FE 2", f"state save {state}", "until 30 A>", "wait 1", "text",
                "peek 3000 400", "peek 3400 400", f"state load {state}", "until 30 A>", "wait 1", "text", "peek 3000 400", "peek 3400 400"]
    answers = cubz_run(main, cwd, commands)
    need(peeked(answers[1]) != b"\0\0" and peeked(answers[2]) == b"\0\0", "the state was not saved in the middle of the transfer")
    texts = cubz_texts(commands, answers)
    need("57K CP/M ver 2.2/4" in texts[0] and texts[1] == texts[0], f"the state loaded back ends elsewhere: {texts!r}")
    first, second = peeked(answers[7]) + peeked(answers[8]), peeked(answers[13]) + peeked(answers[14])
    need(cubz_boot_loaded(first, original) and second == first, "the boot sectors after the state")
    fresh = cubz_run(main, cwd, [f"state load {state}", "until 30 A>", "wait 1", "text"])
    need(fresh[-1]["text"] == texts[0], f"the state in a fresh run: {fresh[-1]['text']!r}")
    # The interrupt: at 4000h, with interrupts off, CTC channel 2 as a timer with its interrupt (vector 10h, mode 2, I = 43h:
    # the vector of channel 2 is 14h, its routine at 4100h counts at 4200h), a delay of about 2.7 s, then EI and a loop.
    program = bytes([0xF3, 0xED, 0x5E, 0x3E, 0x43, 0xED, 0x47, 0x3E, 0x10, 0xD3, 0xD0, 0x3E, 0x87, 0xD3, 0xD2, 0x3E, 0x10, 0xD3,
                     0xD2, 0x16, 0x04, 0x01, 0x00, 0x00, 0x0B, 0x78, 0xB1, 0x20, 0xFB, 0x15, 0x20, 0xF5, 0xFB, 0x18, 0xFE])
    routine = bytes([0xF5, 0x3A, 0x00, 0x42, 0x3C, 0x32, 0x00, 0x42, 0xF1, 0xFB, 0xED, 0x4D])
    cwd = cubz_folder("cubz_state_int", None)
    state = cwd / "int.zrs"
    setup = (["until 10 cubZ V1.4", "wait 0.5"] + poke_commands(0x4000, program) + poke_commands(0x4100, routine) +
             ["poke 4314 00 41", "poke 4200 00", "type G4000\\n", "wait 0.2", "peek 4200 1", f"state save {state}", "wait 4", "peek 4200 1"])
    answers = cubz_run(main, cwd, setup)
    pending, after = peeked(answers[-4])[0], peeked(answers[-1])[0]
    need(pending == 0 and after > 0, f"the interrupt: {pending} served before the state, {after} after the run went on")
    again = cubz_run(main, cwd, [f"state load {state}", "wait 4", "peek 4200 1"])
    need(peeked(again[-1])[0] == after, f"after the load: {peeked(again[-1])[0]} interrupts served, {after} without the load")


# --- the aMIC and the PRAE-M (src/core/machines/amic.c, prae.c) --------------------------------------------------------------
#
# Each check replays a runtime case of the core (MiSTerRO/docs/amic and prae runtime-cases.json: its arguments, its sim/ script, its expected screen
# lines) through the harness, headless; the record cases replay the core's record scripts and then load what was recorded; the diagnostics' ports
# run from RAM (the diagnostics themselves replace the ROM, which ZetRo finds by its hash). Skipped without the ROMs (python tools/fetch_roms.py)
# or the MiSTerRO checkout beside the repository. The scripts' keys come at the keyboard's pace: a wait after the command
# that starts BASIC becomes a wait for READY, and the tape's PLAY waits for the keys typed.

NATIVE_ROMS = {"amic": ["amic.rom"], "prae": ["prae-3.6.rom", "prae-3.5.rom"]}


def native_core(name: str) -> tuple[Path, Path, dict]:
    """The main checkout, the core of the aMIC ("amic") or the PRAE ("prae") beside it and its runtime cases; skipped when either is missing."""
    main = main_checkout()
    core = main.parent / "MiSTerRO" / "src" / f"core-{name}"
    cases = main.parent / "MiSTerRO" / "docs" / name / "runtime-cases.json"
    if not (core / "sim").is_dir() or not cases.is_file():
        raise Skipped(f"no {name.upper()} core beside the repository ({core})")
    for rom in NATIVE_ROMS[name]:
        if not (main / "roms" / rom).is_file():
            raise Skipped(f"no {main / 'roms' / rom} (python tools/fetch_roms.py)")
    return main, core, json.loads(cases.read_text(encoding="utf-8"))


def native_commands(core: Path, name: str, tape: Path | None = None) -> list[str]:
    """A script of the core's simulator (type, enter, wait, play, record, stop-record, save) as harness commands. `record PATH` puts an empty
    tape file `tape` in the recorder and starts recording, `stop-record` ends it and takes the tape out (the shell writes it back); `save`
    (a dump of the simulator) is dropped. The wait after the aMIC's G0800 becomes a wait for READY (BASIC measures its RAM first)."""
    commands: list[str] = []
    for line in (core / "sim" / name).read_text(encoding="utf-8").splitlines():
        word, _, rest = line.strip().partition(" ")
        if not word or word == "save":
            continue
        if word == "play":
            commands.append(f"play {core / rest}")
        elif word == "record":
            need(tape is not None, f"the script {name} records and the check gives no tape")
            tape.write_bytes(b"")
            commands += [f"insert tape {tape}", "tape record"]
        elif word == "stop-record":
            commands += ["tape record", "eject tape"]
        elif word == "wait" and commands[-2:] == ["type g0800", "enter"]:
            commands += ["until 10 READY", "frames 5"]
        elif word in ("type", "enter", "wait"):
            commands.append(line.strip())
        else:
            raise Failed(f"the core's script {name} has a command this check does not know: {line!r}")
    return commands


def native_boot(model: str) -> list[str]:
    """Until the machine is ready for the script's first key: the aMIC's monitor has printed its banner, the PRAE has had half a second."""
    return ["until 10 MONITOR"] if model == "amic" else ["frames 25"]


def native_screen(text: str, patterns: list[str], what: str) -> None:
    for pattern in patterns:
        need(screen_has(text, pattern), f"{what}: no line matches {pattern!r} on the screen {text!r}")


def native_run(config: Path, commands: list[str], env: dict[str, str] | None = None) -> tuple[list[dict], str]:
    """The commands on the control channel from the main checkout (its roms/ is found), every answer but the last ok; with the answers, what
    the program wrote on stderr."""
    run = subprocess.run([str(EXE), "--hidden", "--config", str(config), "--control", "stdin"], cwd=main_checkout(),
                         input="\n".join(commands + ["quit"]) + "\n", capture_output=True, text=True, timeout=300, env={**os.environ, **(env or {})})
    answers = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    need(len(answers) == len(commands) + 1, f"{len(answers)} answers to {len(commands) + 1} commands: {run.stderr.strip()}")
    failed = [(c, a) for c, a in zip(commands, answers) if not a.get("ok")]
    need(not failed, f"{failed[:2]}")
    return answers[:-1], run.stderr


def amic_case_options(case: dict) -> dict[str, str]:
    """The memory the case's --status asks for (the OSD's bits 9-10: 0 32 KiB, 1 16, 2 48)."""
    args = case["arguments"]
    status = int(args[args.index("--status") + 1]) if "--status" in args else 0
    return {"memory": ["32", "16", "48"][status >> 9 & 3]}


def native_case(cwd: Path, core: Path, case: dict, model: str, **options: str) -> list[dict]:
    """A runtime case: its model and options, its script, then the screen as text and a shot."""
    args = case["arguments"]
    script_name = Path(args[args.index("--script") + 1]).name
    commands = native_boot(model) + native_commands(core, script_name)
    tag = "_".join([model, Path(script_name).stem, *options.values()])
    config = hcp_config(cwd, tag, model, **options)
    answers, _ = native_run(config, commands + ["frames 2", "text", f"shot {cwd / (tag + '.png')}"])
    native_screen(answers[-2]["text"], case["screen"], tag)
    return answers


def check_amic_basic() -> None:
    """The aMIC's runtime cases basic32, basic16 and basic48: the monitor's G0800 starts BASIC with 32, 16 and 48 KiB of RAM, a one-line program
    prints 42 and BASIC says READY; the RAM is as big as the option says (a write at 8000h and at C000h lands or is lost: reads give FFh where
    nothing is fitted)."""
    main, core, cases = native_core("amic")
    cwd = folder("amic_basic")
    for name in ("basic32", "basic16", "basic48"):
        options = amic_case_options(cases[name])
        answers = native_case(cwd, core, cases[name], "amic", **options)
        need("WELCOME TO BASIC" in answers[-2]["text"], f"{name}: the aMIC's BASIC banner: {answers[-2]['text']!r}")
        kib = int(options["memory"])
        probe = ["until 10 MONITOR", "poke 4000 63", "poke 8000 63", "poke C000 63", "peek 4000", "peek 8000", "peek C000"]
        answers, _ = native_run(hcp_config(cwd, f"amic_ram{kib}", "amic", **options), probe)
        got = [peeked(a)[0] for a in answers[-3:]]
        need(got == [0x63, 0x63 if kib >= 32 else 0xFF, 0x63 if kib == 48 else 0xFF], f"{kib} KiB of RAM: bytes at 4000, 8000 and C000 read {got}")


def check_amic_cassette() -> None:
    """The aMIC's runtime case cassette: BASIC's LOA loads the core's demo.pzx (recorded by the machine's own SAV) from the tape in real time, RUN
    prints 42."""
    main, core, cases = native_core("amic")
    native_case(folder("amic_cassette"), core, cases["cassette"], "amic")


def check_prae_cassette() -> None:
    """The PRAE-M's runtime cases basic36-cassette and basic35-cassette: Enter three times (console, welcome page, HIGHEST MEMORY?), the version on
    the screen, ALOAD "DEMO" loads the core's demo.pzx from the tape, RUN prints 42."""
    main, core, cases = native_core("prae")
    cwd = folder("prae_cassette")
    native_case(cwd, core, cases["basic36-cassette"], "prae36")
    native_case(cwd, core, cases["basic35-cassette"], "prae35")


def pzx_pulses(path: Path) -> list[int]:
    """The pulses (T-states of 3.5 MHz) of the PULS blocks of a PZX file: each is a duration of up to 16 or 31 bits with an optional repeat count."""
    data = path.read_bytes()
    pulses: list[int] = []
    at = 0
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        body = data[at + 8:at + 8 + size]
        if tag == b"PULS":
            i = 0
            while i < len(body):
                (word,) = struct.unpack_from("<H", body, i)
                i += 2
                count = 1
                if word > 0x8000:
                    count = word & 0x7FFF
                    (word,) = struct.unpack_from("<H", body, i)
                    i += 2
                if word >= 0x8000:
                    (low,) = struct.unpack_from("<H", body, i)
                    i += 2
                    word = (word & 0x7FFF) << 16 | low
                pulses += [word] * count
        at += 8 + size
    return pulses


def pulse_wav(pulses: list[int], path: Path, low: int, high: int, noise: int = 0, rate: int = 44100) -> None:
    """The pulses as a mono 8-bit WAV: the level alternates from low, `low` and `high` the sample values, `noise` the amplitude of the steps added
    to every sample (a fixed pseudo-random sequence), the pulses' lengths in samples exact to a fraction carried on."""
    samples = bytearray()
    carry, level, seed = 0, 0, 12345
    for pulse in pulses:
        carry += pulse * rate
        count, carry = divmod(carry, 3_500_000)
        for _ in range(count):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            jitter = (seed >> 16) % (2 * noise + 1) - noise if noise else 0
            samples.append(max(0, min(255, (high if level else low) + jitter)))
        level ^= 1
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(rate)
        w.writeframes(bytes(samples))


def check_prae_wav() -> None:
    """A sampled tape loads on the PRAE-M: the pulses of the core's demo.pzx written as a WAV (44.1 kHz, 8 bit), ALOAD "DEMO" and RUN print 42 as
    from the PZX; so does a copy at a tenth of the volume with a DC offset (both levels above the format's midpoint) and noise on every sample, which
    the reader takes the midpoint and the hysteresis of from the signal's own peaks. (The software archive's WAV tapes are memory images, not programs:
    ALOAD, which asks for programs, names them and goes on looking; LOAD "NAME" &6000 loads each. They are too long for a check.)"""
    main, core, cases = native_core("prae")
    cwd = folder("prae_wav")
    pulses = pzx_pulses(core / "assets" / "tapes" / "demo.pzx")
    need(len(pulses) > 6000, f"the pulses of demo.pzx: {len(pulses)}")
    for name, low, high, noise in (("clean", 0x30, 0xD0, 0), ("quiet", 140, 156, 3)):
        wav = cwd / f"{name}.wav"
        pulse_wav(pulses, wav, low, high, noise)
        commands = [f"play {wav}" if c.startswith("play ") else c for c in native_boot("prae36") + native_commands(core, "load-demo.script")]
        answers, _ = native_run(hcp_config(cwd, name, "prae36"), commands + ["frames 2", "text"])
        native_screen(answers[-1]["text"], ["^\\s*42\\s*$", "READY"], f"the {name} WAV copy of demo.pzx")


def check_prae_autoload() -> None:
    """Auto-load on the PRAE-M types the command for the tape's own first file, the name and type read from its header record in the pulses
    (src/core/machines/prae.c): the core's demo.pzx, a program, is loaded by ALOAD "DEMO" (RUN prints 42, as after the core's own script), and the
    software archive's wall.wav, a sampled memory image, by LOAD "WALL" &6000 (READY comes back under the command, and the first record's
    bytes, read from the WAV here, are at 6000h). A tape that is no PRAE tape gets the model's ALOAD "" (it lists, loads nothing)."""
    main, core, cases = native_core("prae")
    cwd = folder("prae_autoload")
    tapes = (core / "assets" / "tapes").resolve()
    config = hcp_config(cwd, "prae36", "prae36")
    answers, _ = native_run(config, [f"drop {tapes / 'demo.pzx'}", 'until 60 ALOAD "DEMO"', "wait 14", "type run", "enter", "wait 1", "text"])
    native_screen(answers[-1]["text"], ['^\\s*ALOAD "DEMO"\\s*$', "^\\s*42\\s*$", "READY"], "demo.pzx dropped")
    image = bytes.fromhex("ED 73 17 50 21 C0 FF 39 F9 CD 23 50 ED 73 3E 6A")
    answers, _ = native_run(config, [f"drop {tapes / 'wall.wav'}", 'until 60 LOAD "WALL" &6000', "wait 45", "text", "peek 6000 10"])
    lines = [line.strip() for line in answers[-2]["text"].splitlines() if line.strip()]
    need('LOAD "WALL" &6000' in lines and lines[lines.index('LOAD "WALL" &6000') + 1:][:1] == ["READY:"], f"wall.wav dropped: no READY under the command: {lines!r}")
    need(peeked(answers[-1]) == image, f"wall.wav dropped: the bytes at 6000h are {peeked(answers[-1]).hex(' ')}, the tape's first record starts {image.hex(' ')}")
    (cwd / "nothing.tap").write_bytes(tap_file())
    native_run(config, [f"drop {cwd / 'nothing.tap'}", 'until 60 ALOAD ""'])


def native_record(core: Path, name: str, load_script: list[str], model: str, cwd: Path) -> None:
    """The core's record script run, the tape file it wrote read back on the host (a PZX of the machine's own pulses), and a fresh run that loads
    it and runs the program."""
    tape = cwd / f"{name}.pzx"
    config = hcp_config(cwd, name, model)
    native_run(config, native_boot(model) + native_commands(core, "record-demo.script", tape) + ["text"])
    need(tape.is_file() and tape.stat().st_size > 1000, f"{name}: the tape file was not written ({tape.stat().st_size if tape.is_file() else 'missing'} bytes)")
    need(tape.read_bytes()[:4] == b"PZXT", f"{name}: the tape file is not a PZX")
    commands = native_boot(model) + [c.replace("PATH", str(tape)) for c in load_script] + ["text", f"shot {cwd / (name + '.png')}"]
    answers, _ = native_run(config, commands)
    native_screen(answers[-2]["text"], ["^\\s*42\\s*$", "READY"], f"{name}: the tape the machine recorded, loaded again")


def check_amic_record() -> None:
    """The aMIC's record script: BASIC's SAV records a program through the tape recorder to a new PZX file, written back on the host; a fresh run's LOA
    loads that file and RUN prints 42."""
    main, core, cases = native_core("amic")
    load = ["type g0800", "enter", "until 10 READY", "type loa", "enter", "play PATH", "wait 14", "type run", "enter", "wait 1"]
    native_record(core, "amic_record", load, "amic", folder("amic_record"))


def check_prae_record() -> None:
    """The PRAE-M's record script: ASAVE "DEMO" records the program to a new PZX file, written back on the host; a fresh run's ALOAD "DEMO" loads
    that file and RUN prints 42."""
    main, core, cases = native_core("prae")
    load = ["enter", "wait 1", "enter", "wait 1", "enter", "wait 1", 'type aload "DEMO"', "enter", "play PATH", "wait 14", "type run", "enter", "wait 1"]
    native_record(core, "prae_record", load, "prae36", folder("prae_record"))


def z80_ports(origin: int, marker: int, body) -> bytes:
    """A diagnostic as shared/romanian/tools/diagnostics.py builds it: DI, a stack, the body (given `emit`, `out`, `compare` and `mem`, the
    diagnostic's own names, jumping to `fail`), then RPASS! copied to `marker`; on a failure A and EEh are left at marker + 6 and marker."""
    def build(emit) -> None:
        def out(port: int, value: int) -> None:
            emit(0x01, port & 0xFF, port >> 8, 0x3E, value, 0xED, 0x79)

        def compare(port: int, value: int) -> None:
            emit(0x01, port & 0xFF, port >> 8, 0xED, 0x78, 0xFE, value, 0xC2, "fail")

        def mem(address: int, value: int) -> None:
            emit(0x3E, value, 0x32, address & 0xFF, address >> 8, 0x3A, address & 0xFF, address >> 8, 0xFE, value, 0xC2, "fail")
        emit(0xF3, 0x31, 0xFF, 0xBF)
        body(emit, out, compare, mem)
        emit(0x21, "passed", 0x11, marker & 0xFF, marker >> 8, 0x01, 0x06, 0x00, 0xED, 0xB0, 0x76, 0x18, 0xFD)
        emit(":fail", 0x32, (marker + 6) & 0xFF, (marker + 6) >> 8, 0x3E, 0xEE, 0x32, marker & 0xFF, marker >> 8, 0x76, 0x18, 0xFD)
        emit(":passed", b"RPASS!")
    return z80_program(origin, build)


def check_amic_ports() -> None:
    """The aMIC's diagnostic (shared/romanian/tools/diagnostics.py, AMIC), run from RAM at 9000h by the monitor's G: the keyboard 8255's control word
    and port C, ports A and B reading FFh with no key and nothing on the tape, the external 8255's three ports written and read back, the 8251's
    mode and command words, a byte sent and received in the loopback (TX to RX, RTS to CTS, DTR to DSR: `ports: serial: loopback`) with its
    status reading DSR, RAM at 8000h. Then what the diagnostic does not: the byte the 8251 sends reaches a `file:` end, a key pressed reads 0 on
    its line of port A and SHIFT on port B bit 5, a joystick pulls the external port A's low five lines down, and port C bit 5 inverts the
    picture."""
    main, core, cases = native_core("amic")
    cwd = folder("amic_ports")

    def body(emit, out, compare, mem) -> None:
        out(0x23, 0x92)
        out(0x22, 0)
        compare(0x20, 0xFF)
        compare(0x21, 0xFF)
        out(0x43, 0x80)
        for port, value in ((0x40, 0xA5), (0x41, 0x5A), (0x42, 0x3C)):
            out(port, value)
            compare(port, value)
        out(1, 0x4E)
        out(1, 0x37)
        out(0, 0xA5)
        emit(0x01, 0x01, 0x00, 0xED, 0x78, 0xE6, 0x02, 0x28, 0xFA)         # wait for RxRDY
        compare(0, 0xA5)
        emit(0x01, 0x01, 0x00, 0xED, 0x78, 0xE6, 0x80, 0xFE, 0x80, 0xC2, "fail")      # DSR
        mem(0x8000, 0x63)
    test = z80_ports(0x9000, 0xB100, body)
    config = with_ports(hcp_config(cwd, "amic", "amic", memory="32"), serial="loopback")
    start = ["until 10 MONITOR"] + poke_commands(0x9000, test) + ["type g9000", "enter", "frames 100", "peek B100 7"]
    answers, _ = native_run(config, start)
    got = peeked(answers[-1])
    need(got[:6] == b"RPASS!", f"the aMIC's ports: {got!r} (EE then the failing value: the step that failed)")
    # With a file for an end the byte the 8251 sends is handed to the host.
    sender = z80_ports(0x9000, 0xB100, lambda emit, out, compare, mem: (out(1, 0x4E), out(1, 0x37), out(0, 0xA5)))
    sent = with_ports(hcp_config(cwd, "amic_file", "amic", memory="32"), serial="file:serial.out")
    native_run(sent, ["until 10 MONITOR"] + poke_commands(0x9000, sender) + ["type g9000", "enter", "frames 100"])
    need((cwd / "serial.out").read_bytes() == b"\xA5", f"the 8251 handed {(cwd / 'serial.out').read_bytes()!r} to the port, not A5")
    config = hcp_config(cwd, "amic", "amic", memory="32")

    def reader(emit, out, compare, mem) -> None:
        out(0x23, 0x92)
        out(0x22, 0x04)                             # scan line 4: A S D F G H J K
        emit(0xDB, 0x20, 0xFE, 0xFF, 0x28, 0xFA)    # IN A,(20); CP FF; JR Z,back: until a key is down
        emit(0x32, 0x00, 0xB2, 0xDB, 0x21, 0x32, 0x01, 0xB2)          # LD (B200),A; IN A,(21); LD (B201),A
        out(0x43, 0x9B)                             # the external 8255 all inputs
        emit(0xDB, 0x40, 0x32, 0x02, 0xB2)          # IN A,(40); LD (B202),A
        out(0x22, 0x20)                             # port C bit 5: the picture inverted
    test = z80_ports(0x9000, 0xB100, reader)
    commands = (["until 10 MONITOR", f"shot {cwd / 'normal.png'}"] + poke_commands(0x9000, test) + ["joy 0 up fire", "type g9000", "enter", "frames 40", "key SHIFT",
                "key A", "frames 20", "peek B100 6", "peek B200 3", f"shot {cwd / 'inverted.png'}"])
    answers, _ = native_run(config, commands)
    need(peeked(answers[-3])[:6] == b"RPASS!", f"the aMIC's reader program: {peeked(answers[-3])!r}")
    row, port_b, joy = peeked(answers[-2])
    need(row == 0xFE, f"key A pressed, scan line 4: port A reads {row:02X}, not FE")
    need(port_b & 0xE1 == 0xC1, f"SHIFT held: port B reads {port_b:02X} (bit 5 low, bits 6 and 7 high, the idle tape high)")
    need(joy == 0xE7, f"joystick up and fire: the external port A reads {joy:02X}, not E7")
    normal, inverted = png_rows(cwd / "normal.png"), png_rows(cwd / "inverted.png")
    need(normal[150][160] == (0, 0, 0) and inverted[150][160] == (255, 255, 255) and inverted[2][2] == (0, 0, 0),
         f"port C bit 5 inverts the picture and not the blanking: {normal[150][160]} -> {inverted[150][160]}, border {inverted[2][2]}")


def check_prae_ports() -> None:
    """The PRAE-M's diagnostic (shared/romanian/tools/diagnostics.py, PRAE), run from RAM at 9000h by BASIC's CALL: the port reading FFh (idle line,
    idle tape, no key), written with 00h and C0h, RAM at 4000h and at E000h (the bitmap). Then what the diagnostic does not: the keyboard (a key
    pressed reads 0 on its return line when its line is selected by an address bit, SHIFT likewise, D5 high) and the frame interrupt (a handler
    through RST 38's vector in RAM counts 100 ticks in 100 frames)."""
    main, core, cases = native_core("prae")
    cwd = folder("prae_ports")
    boot = ["frames 25", "enter", "frames 60", "enter", "frames 60", "enter", "frames 60"]

    def body(emit, out, compare, mem) -> None:
        compare(0x0080, 0xFF)
        out(0x80, 0)
        out(0x80, 0xC0)
        mem(0x4000, 0x63)
        mem(0xE000, 0xA5)
    test = z80_ports(0x9000, 0xC100, body)
    config = hcp_config(cwd, "prae", "prae36")
    answers, _ = native_run(config, boot + poke_commands(0x9000, test) + ["type call&9000", "enter", "frames 100", "peek C100 7"])
    got = peeked(answers[-1])
    need(got[:6] == b"RPASS!", f"the PRAE's ports: {got!r} (EE then the failing value: the step that failed)")
    # The keys and the interrupt: wait for a key on line 5 (A D G J L), read lines 5, 7 and none, then count interrupts in a HALT loop.
    program = bytes([0x01, 0x80, 0xDF, 0xED, 0x78, 0x32, 0x00, 0xB2, 0xE6, 0x01, 0x20, 0xF4,     # BC=DF80; IN A,(C); LD (B200),A; AND 1; JR NZ,back
                     0x01, 0x80, 0x7F, 0xED, 0x78, 0x32, 0x01, 0xB2,                              # line 7: LD (B201),A
                     0x01, 0x80, 0xFF, 0xED, 0x78, 0x32, 0x02, 0xB2,                              # no line: LD (B202),A
                     0x3E, 0x00, 0x32, 0x00, 0xB3, 0xFB, 0x76, 0x18, 0xFD])                       # B300 = 0; EI; HALT loop
    isr = bytes([0xF5, 0x3A, 0x00, 0xB3, 0x3C, 0x32, 0x00, 0xB3, 0xF1, 0xFB, 0xED, 0x4D])      # PUSH AF; B300++; POP AF; EI; RETI
    commands = (boot + poke_commands(0x9000, program) + poke_commands(0x9100, isr) + ["poke 4038 C3 00 91", "type call&9000", "enter", "frames 40", "key SHIFT",
                "frames 5", "key A", "frames 20", "peek B200 3", "peek B300", "frames 100", "peek B300"])
    answers, _ = native_run(config, commands)
    row, shift, idle = peeked(answers[-4])
    first, second = peeked(answers[-3])[0], peeked(answers[-1])[0]
    need(row & 0x1F == 0x1E and row & 0xE0 == 0xE0, f"key A held, line 5 selected: the port reads {row:02X}, not FE")
    need(shift & 0x1F == 0x1E, f"SHIFT held, line 7 selected: the port reads {shift:02X}")
    need(idle == 0xFF, f"no line selected: the port reads {idle:02X}, not FF (D5 high, idle line and tape high)")
    need((second - first) % 256 in (100, 101), f"the frame interrupt: {(second - first) % 256} ticks in 100 frames")


def check_prae_serial() -> None:
    """The PRAE-M's bit-banged serial line is turned into bytes at the firmware's framing: its own output routine (CO with the serial console
    selected) sends 41h, 7Ah, 7Fh, 00h and 55h, and the port is handed exactly those bytes. The firmware's start-up values (4012h-4015h: 7 data
    bits, no parity, 2 stop bits, a bit loop of 8333 T-states) are the 300 baud 7N2 the board's options default to; the line is read at the
    middle of each bit, so a rate or a width that is not the firmware's gives other bytes."""
    main, core, cases = native_core("prae")
    cwd = folder("prae_serial")
    boot = ["frames 25", "enter", "frames 60", "enter", "frames 60", "enter", "frames 60"]
    program = bytes([0x3E, 0x01, 0x32, 0x0B, 0x40]) + b"".join(bytes([0x0E, c, 0xCD, 0x54, 0x31]) for c in (0x41, 0x7A, 0x7F, 0x00, 0x55)) + bytes([0x3E, 0x00, 0x32, 0x0B, 0x40, 0xC9])
    commands = boot + poke_commands(0x9000, program) + ["type call&9000", "enter", "wait 5", "text"]
    answers, _ = native_run(with_ports(hcp_config(cwd, "prae", "prae36"), serial="file:serial.out"), commands)
    sent = (cwd / "serial.out").read_bytes()
    need(sent == bytes([0x41, 0x7A, 0x7F, 0x00, 0x55]), f"the bytes the serial line was turned into: {sent.hex(' ')}")
    native_screen(answers[-1]["text"], ["READY"], "prae serial")


def native_tape_run(config: Path, model: str, tape: Path, save_at: float, state: Path, load: bool = False) -> tuple[str, str]:
    """Boot, start the program load and on the way (save_at seconds after PLAY) save the state, or load that state into a machine with the tape in
    it; then to the end: the tape runs out, RUN. The screen at the save and at the end."""
    start = (["until 10 MONITOR", "type g0800", "enter", "until 10 READY", "type loa", "enter"] if model == "amic" else
             ["frames 25", "enter", "wait 1", "enter", "wait 1", "enter", "wait 1", 'type aload "DEMO"', "enter"])
    commands = ["frames 25", f"insert tape {tape}", f"state load {state}"] if load else start + [f"play {tape}", f"wait {save_at}", "text", f"state save {state}"]
    commands += ["wait 14", "type run", "enter", "wait 1", "text"]
    answers, _ = native_run(config, commands)
    texts = [a["text"] for a in answers if "text" in a]
    return texts[0], texts[-1]


def native_state(name: str, model: str, tape: Path, save_at: float, unfinished: str) -> None:
    cwd = folder(name)
    config = hcp_config(cwd, name, model)
    state = cwd / "mid.zrs"
    mid, end = native_tape_run(config, model, tape, save_at, state)
    need(not re.search(unfinished, mid), f"{name}: the state was saved after the load had finished: {mid!r}")
    native_screen(end, [r"^\s*42\s*$", "READY"], f"{name}: the load")
    need(state_chunks(state).get("TAPE"), f"{name}: the state has no tape recorder in it")
    _, again = native_tape_run(config, model, tape, save_at, state, load=True)
    need(again == end, f"{name}: the state in a new run: {again!r} for {end!r}")
    # And in the same run: back into the middle of the load after it has finished.
    answers, _ = native_run(config, ["frames 25", f"insert tape {tape}", f"state load {state}", "wait 14", f"state load {state}", "wait 14", "type run", "enter", "wait 1", "text"])
    need(answers[-1]["text"] == end, f"{name}: the state loaded twice in a run: {answers[-1]['text']!r} for {end!r}")


def check_amic_state() -> None:
    """A state saved in the middle of the aMIC's tape load (LOA reading the cassette input in its timing loops) loads back, in a new run with the
    tape in the recorder, to the same end: the CPU, the RAM, both 8255s, the 8251, the video, the sound and the recorder's position are in it."""
    main, core, cases = native_core("amic")
    native_state("amic_state", "amic", core / "assets" / "tapes" / "demo.pzx", 5, r"16 HL")


def check_prae_state() -> None:
    """The same on the PRAE-M, in the middle of ALOAD's read of the tape through its one port."""
    main, core, cases = native_core("prae")
    native_state("prae_state", "prae36", core / "assets" / "tapes" / "demo.pzx", 1.2, r'ALOAD "DEMO"\s+READY')


def check_native_fast() -> None:
    """The tape loads to the same screen with loader acceleration on and off and with the slot's full speed on and off, on both machines; with it on the
    recogniser jumps over the loops that wait on the tape (ZETRO_ACCEL_STATS), with it off it does not run."""
    main_checkout()
    cwd = folder("native_fast")
    for name, model, case, script in (("amic", "amic", "cassette", "load-demo.script"), ("prae", "prae36", "basic36-cassette", "load-demo.script")):
        main, core, cases = native_core(name)
        tape = core / "assets" / "tapes" / "demo.pzx"
        screens = set()
        for accelerate in ("true", "false"):
            for fast in ("true", "false"):
                config = cwd / f"{name}_{accelerate}_{fast}.yaml"
                config.write_text(f"machine:\n  model: {model}\nmedia:\n  tape:\n    image: {tape}\n    full_speed: {fast}\n    accelerate: {accelerate}\nvideo:\n  look: plain\n",
                                  encoding="utf-8")
                commands = native_boot(model) + [c for c in native_commands(core, script) if not c.startswith("play ")]
                commands.insert(commands.index("wait 14"), "tape play")
                answers, stderr = native_run(config, commands + ["text"], {"ZETRO_ACCEL_STATS": "1"})
                native_screen(answers[-1]["text"], cases[case]["screen"], f"{name} accel {accelerate} fast {fast}")
                screens.add(answers[-1]["text"])
                jumped = re.search(r"loadaccel: (\d+) jumps, (\d+) of (\d+) T-states", stderr)
                if accelerate == "true":
                    need(jumped and int(jumped.group(1)) > 100 and int(jumped.group(2)) > 0, f"{name}: loader acceleration did not jump ({stderr.strip()!r})")
                else:
                    need(not jumped, f"{name}: loader acceleration ran while off ({stderr.strip()!r})")
        need(len(screens) == 1, f"{name}: the load ends on {len(screens)} different screens")


def check_native_screens() -> None:
    """Each machine's BASIC as a picture (the shots stay in work/verify/native_screens): 320 x 288, black and the lit colour, the screen's text read
    back from the bitmap through the firmware's font; the green monitor's lit colour."""
    main, core, cases = native_core("amic")
    cwd = folder("native_screens")
    boot = {"amic": ["until 10 MONITOR", "type g0800", "enter", "until 10 READY", "frames 2", "text"],
            "prae": ["frames 25", "enter", "wait 1", "enter", "wait 1", "enter", "wait 1", "frames 2", "text"]}
    runs = [("amic", {}), ("amic", {"memory": "48"}), ("amic", {"monitor": "green"}), ("prae36", {}), ("prae35", {}), ("prae36", {"monitor": "green"})]
    for model, options in runs:
        tag = "_".join([model, *options.values()])
        answers, _ = native_run(hcp_config(cwd, tag, model, **options), boot["amic" if model == "amic" else "prae"] + [f"shot {cwd / (tag + '.png')}"])
        text = answers[-2]["text"]
        native_screen(text, ["BASIC" if model == "amic" else "PRAE-BASIC", "READY"], tag)
        if model == "prae35":
            native_screen(text, ["V3\\.5"], tag)
        rows = png_rows(cwd / f"{tag}.png")
        colours = {p for row in rows for p in row}
        need(len(rows) == 288 and len(rows[0]) == 320, f"{tag}: the picture is {len(rows[0])} x {len(rows)}, not 320 x 288")
        lit = (0x33, 0xFF, 0x66) if options.get("monitor") == "green" else (255, 255, 255)
        need(colours == {(0, 0, 0), lit}, f"{tag}: the picture's colours are {colours}")


def check_native_model_groups() -> None:
    """`zetro --list-models` lists the aMIC on its own line and the PRAE-M's two models on one."""
    run = zetro(main_checkout(), "--list-models")
    need(run.returncode == 0, f"--list-models: exit {run.returncode}: {run.stderr.strip()}")
    amic = [l for l in run.stdout.splitlines() if l.startswith("aMIC:")]
    prae = [l for l in run.stdout.splitlines() if l.startswith("PRAE:")]
    need(len(amic) == 1 and "amic (aMIC)" in amic[0], f"the aMIC line of --list-models: {run.stdout!r}")
    need(len(prae) == 1 and "prae36 (PRAE-M BASIC 3.6)" in prae[0] and "prae35 (PRAE-M BASIC 3.5)" in prae[0], f"the PRAE line of --list-models: {run.stdout!r}")


# --- the Dandanator (src/hw/dandanator.h; the option "interface", the slot "romset") -------------------------------------
#
# The checks run the cartridge with a ROM set that is not in the repository: the test image of the fuse-sdl3 history
# (524,288 bytes, the "MAsoft ZX Spectrum Collection" menu v10.3), found through ZETRO_DANDANATOR_ROM or at
# work/dandanator/dandanator-msft.rom; without it, or without the ROMs, they are skipped. The menu draws with its own
# character set (decompressed to 6F76h), not the ROM's, so the screen is read back here through that set.

DAN_CHARSET = 0x6F76
DAN_SECTOR = 0x40               # the flash sector the write check programs (slot 16, offset 0)


def dan_image() -> bytes:
    path = Path(os.environ.get("ZETRO_DANDANATOR_ROM") or ROOT / "work" / "dandanator" / "dandanator-msft.rom")
    if not path.is_file():
        raise Skipped(f"no Dandanator test image at {path} (set ZETRO_DANDANATOR_ROM, or: git -C <fuse-sdl3> show "
                      f"33527fe6:dandanator/dandanator-msft.rom > work/dandanator/dandanator-msft.rom)")
    data = path.read_bytes()
    need(len(data) == 524288 and data[0x3FE0:0x3FE5] == b"v10.3", f"{path} is not the test image (512 KiB, menu v10.3)")
    return data


def dan_folder(name: str, model: str, set_bytes: bytes | None, **options: str) -> tuple[Path, Path]:
    """A work folder with a copy of the set (`set.rom`; None: no file is written) and a configuration for `model` with the
    Dandanator on; the main checkout (its roms/ is found from there). Skipped without the model's ROMs."""
    main = main_checkout()
    roms = {"jet": "jet.rom", "hc91": "hc91.rom", "hc128": "hc128.rom"}
    if not (main / "roms" / roms[model]).is_file():
        raise Skipped(f"no {main / 'roms' / roms[model]} (python tools/fetch_roms.py)")
    cwd = folder(name)
    if set_bytes is not None:
        (cwd / "set.rom").write_bytes(set_bytes)
    opts = {"interface": "dandanator", **options}
    (cwd / "zetro.yaml").write_text("machine:\n  model: " + model + "\n  options:\n" + "".join(f"    {k}: {v}\n" for k, v in opts.items())
                                    + f"media:\n  romset:\n    image: {(cwd / 'set.rom').as_posix()}\nvideo:\n  look: plain\n",
                                    encoding="utf-8")
    return cwd, main


def dan_run(cwd: Path, main: Path, commands: list[str]) -> list[dict]:
    answers = control(main, commands + ["quit"], "--config", str(cwd / "zetro.yaml"))
    failed = [(c, a) for c, a in zip(commands, answers) if not a.get("ok")]
    need(not failed, f"{failed[:2]}")
    return answers[:-1]


DAN_SCREEN = [f"peek {0x4000 + i:04X} 400" for i in range(0, 0x1B00, 0x400)] + [f"peek {DAN_CHARSET:04X} 300"]


def dan_text(answers: list[dict]) -> str:
    """The screen (the answers to DAN_SCREEN) as text through the menu's character set: a cell that is a glyph, as it is or
    inverted, is its character, any other cell '?', a blank one a space."""
    screen = b"".join(peeked(a) for a in answers[:-1])
    font = peeked(answers[-1])
    glyphs = {font[g * 8:g * 8 + 8]: chr(0x20 + g) for g in range(96)}
    lines = []
    for row in range(24):
        line = ""
        for col in range(32):
            cell = bytes(screen[((row * 8 + y) & 0xC0) << 5 | ((row * 8 + y) & 7) << 8 | ((row * 8 + y) & 0x38) << 2 | col] for y in range(8))
            line += " " if not any(cell) else glyphs.get(cell) or glyphs.get(bytes(255 - b for b in cell)) or "?"
        lines.append(line.rstrip())
    return "\n".join(lines)


def dan_menu(text: str, what: str) -> None:
    for expected in ("v10.3", "1??3D Combat Zone", "4??Bomb Jack", "P. Toggle pokes", "R. Test - ZX Diagnostics"):
        need(expected in text, f"{what}: {expected!r} is not on the screen: {text!r}")


def dan_attrs(answers: list[dict]) -> bytes:
    return b"".join(peeked(a) for a in answers[:-1])[0x1800:0x1B00]


def check_dandanator_jet() -> None:
    """The JET with the Dandanator and the test image: the cartridge's menu at power on (read back through its own
    characters); key 4 starts Bomb Jack from its slot (its loading picture: cyan paper over much of the screen, the ROM
    area the 48K ROM again); F10, the cartridge's button, brings the menu back; a state saved in the menu and one saved
    in the game come back to the same screens, in the run and in a new one."""
    image = dan_image()
    cwd, main = dan_folder("dandanator_jet", "jet", image)
    menu_state, game_state = cwd / "menu.zrs", cwd / "game.zrs"
    n = len(DAN_SCREEN)
    commands = (["wait 3", f"shot {cwd / 'menu.png'}"] + DAN_SCREEN + [f"state save {menu_state}", "type 4", "wait 6",
                f"shot {cwd / 'game.png'}"] + DAN_SCREEN + ["peek 0000 8", f"state save {game_state}", "wait 2"] + DAN_SCREEN +
                ["nmi", "wait 3"] + DAN_SCREEN + [f"state load {menu_state}", "wait 1"] + DAN_SCREEN +
                [f"state load {game_state}", "wait 2"] + DAN_SCREEN)
    answers = dan_run(cwd, main, commands)
    at = 2
    menu = answers[at:at + n]; at += n + 4
    game = answers[at:at + n]; at += n
    rom = peeked(answers[at]); at += 3
    game_later = answers[at:at + n]; at += n + 2
    back = answers[at:at + n]; at += n + 2
    menu_again = answers[at:at + n]; at += n + 2
    game_again = answers[at:at + n]
    dan_menu(dan_text(menu), "the menu at power on")
    cyan = sum(1 for a in dan_attrs(game) if (a >> 3) & 7 == 5)
    need(cyan > 150 and rom[:5] == bytes([0xF3, 0xAF, 0x11, 0xFF, 0xFF]),
         f"Bomb Jack's loading picture is not on the screen ({cyan} cells of cyan paper), or 0000h is not the 48K ROM ({rom.hex()})")
    dan_menu(dan_text(back), "after the button")
    need(dan_text(menu_again) == dan_text(menu), "the state saved in the menu comes back to another screen")
    screen = lambda a: b"".join(peeked(x) for x in a[:-1])
    need(screen(game_again) == screen(game_later), "the state saved in the game goes on to another screen")
    fresh = dan_run(cwd, main, [f"state load {game_state}", "wait 2"] + DAN_SCREEN)
    need(screen(fresh[2:]) == screen(game_later), "the game's state in a new run goes on to another screen")


def dan_asm(origin: int, items: list) -> bytes:
    """Bytes (lists), labels (str), ('jr', opcode, label) and ('call', label)."""
    labels, out, fix = {}, bytearray(), []
    for it in items:
        if isinstance(it, str):
            labels[it] = origin + len(out)
        elif it[0] == "jr":
            out += bytes([it[1], 0])
            fix.append((len(out) - 1, it[2], 1))
        elif it[0] == "call":
            out += bytes([0xCD, 0, 0])
            fix.append((len(out) - 2, it[1], 2))
        else:
            out += bytes(it)
    for at, name, size in fix:
        if size == 1:
            out[at] = (labels[name] - (origin + at + 1)) & 0xFF
        else:
            out[at], out[at + 1] = labels[name] & 0xFF, labels[name] >> 8
    return bytes(out)


def dan_writer(sector: int) -> bytes:
    """At 9000h, with the interrupts off (as a snapshot starts it): the commands 46,16,16 (unlock), 48,16,sector (erase mode) and the flash's sector
    erase (AA 5555h, 55 2AAAh, 80 5555h, AA 5555h, 55 2AAAh, 30 at the sector: the CPU writes 1555h and 2AAAh), a wait of
    25.6 ms; 48,32,sector (program mode) and 4096 byte programs (AA, 55, A0, the byte) from A000h; 5Ah to 8F00h, and a loop. A command
    is its numbers as trains of writes to 0001h, 0002h and 0003h (32 T-states apart, 520 between trains), then a write to
    0000h."""
    off = (sector & 3) << 4
    unlock = lambda a, v: [0x01, a & 0xFF, a >> 8, 0x3E, v, 0x02]          # LD BC,a; LD A,v; LD (BC),A
    return dan_asm(0x9000, [
        [0x3E, 46, 0x16, 16, 0x1E, 16], ("call", "send"),
        [0x3E, 48, 0x16, 16, 0x1E, sector], ("call", "send"),
        [0x21, 0x00, off],
        unlock(0x1555, 0xAA), unlock(0x2AAA, 0x55), unlock(0x1555, 0x80), unlock(0x1555, 0xAA), unlock(0x2AAA, 0x55),
        [0x3E, 0x30, 0x77],
        [0x01, 0x78, 0x05], "wait", [0xE3, 0xE3, 0x0B, 0x78, 0xB1], ("jr", 0x20, "wait"),
        [0x3E, 48, 0x16, 32, 0x1E, sector], ("call", "send"),
        [0x21, 0x00, 0xA0, 0x11, 0x00, off],
        "byte", unlock(0x1555, 0xAA), unlock(0x2AAA, 0x55), unlock(0x1555, 0xA0),
        [0x7E, 0x12, 0x23, 0x13, 0x7A, 0xE6, 0x0F, 0xB3], ("jr", 0x20, "byte"),
        [0x3E, 0x5A, 0x32, 0x00, 0x8F], "end", ("jr", 0x18, "end"),
        "send", [0x21, 0x01, 0x00], ("call", "train"), [0x23, 0x7A], ("call", "train"), [0x23, 0x7B], ("call", "train"),
        [0x32, 0x00, 0x00, 0x06, 0x28, 0x10, 0xFE, 0xC9],
        "train", [0x47], "pulse", [0x00, 0x00, 0x00, 0x77], ("jr", 0x10, "pulse"), [0x06, 0x28, 0x10, 0xFE, 0xC9],
    ])


def dan_sna(data: bytes) -> bytes:
    """A 48K SNA that starts dan_writer at 9000h (its PC on the stack at 8FF0h), the bytes to program at A000h, interrupts
    off, IM 1."""
    ram = bytearray(49152)
    code = dan_writer(DAN_SECTOR)
    ram[0x5000:0x5000 + len(code)] = code
    ram[0x6000:0x6000 + len(data)] = data
    ram[0x8FF0 - 0x4000:0x8FF2 - 0x4000] = struct.pack("<H", 0x9000)
    header = bytearray(27)
    header[23:25] = struct.pack("<H", 0x8FF0)
    header[25] = 1
    return bytes(header + ram)


def check_dandanator_flash() -> None:
    """A program of the check's own (dan_writer), started from a snapshot, erases and programs one 4 KiB sector of a copy of
    the test image with known bytes: with the programming switch on (the cartridge starts out of the way, locked) the file on
    the host then differs from the image in that sector alone, which holds the bytes; with the switch off (the cartridge
    starts in its menu) the same program leaves the file as it was. A blank set (an empty file) starts the JET's own BASIC
    and is written back as 512 KiB of FFh."""
    image = dan_image()
    data = bytes((i * 7 + 3) & 0xFF for i in range(4096))
    base = DAN_SECTOR * 4096
    for switch in ("on", "off"):
        cwd, main = dan_folder(f"dandanator_flash_{switch}", "jet", image, dandanator_program=switch)
        (cwd / "writer.sna").write_bytes(dan_sna(data))
        answers = dan_run(cwd, main, ["wait 2", f"snapshot load {cwd / 'writer.sna'}", "wait 2", "peek 8F00 1"])
        need(peeked(answers[-1]) == b"\x5a", f"switch {switch}: the program did not finish ({answers[-1]})")
        written = (cwd / "set.rom").read_bytes()
        if switch == "on":
            need(written[base:base + 4096] == data, "the sector does not hold the bytes programmed")
            need(written[:base] == image[:base] and written[base + 4096:] == image[base + 4096:], "the file changed outside the sector")
        else:
            need(written == image, "with the programming switch off the file changed")
    cwd, main = dan_folder("dandanator_blank", "jet", b"")
    answers = dan_run(cwd, main, ["until 5 JET", "text", "peek 0000 4"])
    need("JET" in answers[-2]["text"] and peeked(answers[-1]) == bytes([0xF3, 0xAF, 0x11, 0xFF]),
         f"a blank set: {answers[-2]['text']!r} {answers[-1]}")
    need((cwd / "set.rom").read_bytes() == b"\xff" * 524288, "the blank set was not written back as 512 KiB of FFh")


def check_dandanator_models() -> None:
    """The menu on the HC-91 (Interface 1 off) and on the HC-91 Plus (the cartridge over whichever ROM page 7FFD selects);
    the HC-91 with Interface 1 on has no added interface, whatever the file says (both take the ROM at 0000h)."""
    image = dan_image()
    for model in ("hc91", "hc128"):
        cwd, main = dan_folder(f"dandanator_{model}", model, image)
        answers = dan_run(cwd, main, ["wait 3", f"shot {cwd / 'menu.png'}"] + DAN_SCREEN)
        dan_menu(dan_text(answers[2:]), model)
    cwd, main = dan_folder("dandanator_conflict", "hc91", image, interface1="on")
    run = zetro(main, "--config", str(cwd / "zetro.yaml"), "--frames", "1")
    said = run.stdout + run.stderr
    need(run.returncode == 0 and "HC-91, Interface 1" in said and "Dandanator" not in said, f"both on: exit {run.returncode}, {said.strip()!r}")


# --- the host ends of the byte ports and the tape recorder's record key (P11) ----------------------------------------------
#
# The guest's serial lines and printers reach the host through the `ports` section of zetro.yaml (platform/serial.c); the checks
# give a machine an end (a file, a loopback plug, a TCP server) and look at what comes out of it or goes in.

def with_ports(config: Path, **ends: str) -> Path:
    """The configuration with a `ports` section: key -> the end as the file words it."""
    config.write_text(config.read_text(encoding="utf-8") + "ports:\n" + "".join(f"  {key}: {end}\n" for key, end in ends.items()), encoding="utf-8")
    return config


def free_port() -> int:
    import socket
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class PortSession:
    """A zetro run on the control channel that is driven a command at a time, for the checks that have a peer on the other side of a
    port: the answer to a command comes when it is done, and guest time stands still between commands."""

    def __init__(self, config: Path, log: Path, *args: str) -> None:
        self.log = log.open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(EXE), "--hidden", "--config", str(config), "--control", "stdin", *args], cwd=main_checkout(),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True)

    def send(self, command: str) -> dict:
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            need(line != "", f"zetro ended before it answered {command!r}")
            if line.startswith("{"):
                answer = json.loads(line)
                need(answer.get("ok"), f"{command!r}: {answer}")
                return answer

    def close(self) -> None:
        try:
            if self.process.poll() is None:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
            self.process.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            self.process.kill()
        self.log.close()


def check_ports_config() -> None:
    """The `ports` section: an end is none, loopback, a host serial port, pipe:NAME, tcp:HOST:PORT, tcp-listen:[HOST:]PORT or file:FILE, kept
    for any model (a key the running model has no port for is ignored); a printer has nothing to loop back, a TCP target must be what its kind
    needs, and a key no model has is refused, each with its line."""
    cwd = folder("ports_config")

    def run(text: str) -> subprocess.CompletedProcess:
        (cwd / "c.yaml").write_text(text, encoding="utf-8")
        return zetro(cwd, "--config", "c.yaml", "--frames", "1")

    ok = run("machine:\n  model: tims\nports:\n  serial: tcp-listen:127.0.0.1:2323\n  printer: file:printer.out\n  user: loopback\n")
    need(ok.returncode == 0, f"the ends of the TIM-S (and a key of the CUB Z, which it ignores): {ok.stderr.strip()}")
    for end in ("none", "loopback", "COM3", "serial:COM3", "/dev/ttyUSB0", "pipe:zetro-serial", "tcp:localhost:2323", "tcp-listen:2323", "file:a.out"):
        run_ok = run(f"machine:\n  model: tims\nports:\n  serial: {end}\n")
        need(run_ok.returncode == 0, f"{end}: {run_ok.stderr.strip()}")
    for text, line, word in (("machine:\n  model: tims\nports:\n  printer: loopback\n", 4, "loop back"),
                             ("machine:\n  model: tims\nports:\n  serial: tcp:localhost\n", 4, "HOST:PORT"),
                             ("machine:\n  model: tims\nports:\n  serial: tcp-listen:70000\n", 4, "1 to 65535"),
                             ("machine:\n  model: tims\nports:\n  serial: \"file:\"\n", 4, "empty"),
                             ("machine:\n  model: tims\nports:\n  serial: telnet:host:23\n", 4, "must be"),
                             ("machine:\n  model: tims\nports:\n  serial: none\n  sreial: none\n", 5, "Unknown port")):
        bad = run(text)
        need(bad.returncode != 0 and f"line {line}:" in bad.stderr and word in bad.stderr, f"{text!r}: exit {bad.returncode}: {bad.stderr.strip()}")


def check_ports_printer() -> None:
    """LPRINT reaches the host: a printer end that is a file receives the bytes, appended. The TIM-S's LPRINT "A" (the printer is on line while
    its end is open, so the firmware's wait for it ends) and the CIP-04's (A, CR, LF); a second run appends to the file."""
    hc_rom("tims-standard.rom")
    hc_rom("cip04.rom")
    cwd = folder("ports_printer")
    config = cwd / "tims.yaml"
    config.write_text("machine:\n  model: tims\nports:\n  printer: file:tims-lpt.out\n", encoding="utf-8")
    extended = ["key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3"]
    lprint = ["until 5 IPTV", "frames 40", *extended, 'type c"A"', "enter", "until 20 0 OK", "frames 5"]
    hc_run(config, lprint)
    first = (cwd / "tims-lpt.out").read_bytes()
    need(first.rstrip() == b"A" and first.endswith(b"\r\n"), f"the TIM-S's LPRINT \"A\": {first!r}")
    hc_run(config, lprint)
    need((cwd / "tims-lpt.out").read_bytes() == first + first, "the printer file is appended to, not rewritten")
    config = cip04_config(cwd, "cip04")
    with_ports(config, printer="file:cip04-lpt.out")
    commands = cip04_start(1) + cip04_line('lprint "A"', 200) + ["text", "frames 5"]
    need(screen_has(cip04_texts(commands, hc_run(config, commands))[0], r"0 OK"), "the CIP-04's LPRINT did not end with the report 0 OK")
    need((cwd / "cip04-lpt.out").read_bytes() == b"A\r\n", f"the CIP-04's LPRINT \"A\": {(cwd / 'cip04-lpt.out').read_bytes()!r}")


def amic_serial_program(body) -> list[str]:
    """The harness commands that run a diagnostic-style program on the aMIC's monitor: the 8251 set to mode 4Eh and command 37h
    (RTS and DTR on, transmit and receive enabled), then `body`; its result is at B100 (RPASS!, or EEh and the failing value)."""
    test = z80_ports(0x9000, 0xB100, body)
    return ["until 10 MONITOR"] + poke_commands(0x9000, test) + ["type g9000", "enter"]


def amic_serial_wait(emit, flag: int) -> None:
    """Emits: wait until the 8251's status says RxRDY (bit 1) or TxRDY (bit 0)."""
    emit(0x01, 0x01, 0x00, 0xED, 0x78, 0xE6, flag, 0x28, 0xFA)


def check_ports_loopback() -> None:
    """A serial port with a loopback end returns what the guest sends: the aMIC's 8251, through the monitor's port 0 (data) and 1 (status), sends
    55h, AAh, 00h and FFh and receives each back (TX to RX, RTS to CTS, DTR to DSR), with the status saying DSR."""
    main, core, cases = native_core("amic")
    cwd = folder("ports_loopback")

    def body(emit, out, compare, mem) -> None:
        out(1, 0x4E)
        out(1, 0x37)
        for value in (0x55, 0xAA, 0x00, 0xFF):
            out(0, value)
            amic_serial_wait(emit, 0x02)
            compare(0, value)
        emit(0x01, 0x01, 0x00, 0xED, 0x78, 0xE6, 0x80, 0xFE, 0x80, 0xC2, "fail")      # DSR
    config = with_ports(hcp_config(cwd, "amic", "amic", memory="32"), serial="loopback")
    answers, _ = native_run(config, amic_serial_program(body) + ["frames 100", "peek B100 7"])
    need(peeked(answers[-1])[:6] == b"RPASS!", f"the aMIC's bytes through the loopback: {peeked(answers[-1])!r} (EE then the failing value)")
    # No end: the same program has nothing to send to, the bytes are lost and the first read never comes back.
    config = hcp_config(cwd, "amic_none", "amic", memory="32")
    answers, _ = native_run(config, amic_serial_program(body) + ["frames 100", "peek B100 7"])
    need(peeked(answers[-1])[:6] != b"RPASS!", "a serial port with no end returned the guest's byte")


def check_ports_tcp() -> None:
    """A serial port with a TCP server end (ports.serial: tcp-listen:PORT) is a socket: the check connects with a Python socket, sends three
    bytes that the aMIC's program (the 8251 at the monitor's ports, polled) reads and sends back plus one, and receives them with the byte the
    program sends of its own at the end; the bytes the guest read are in its memory. The aMIC's 8251 is the machine with the simplest receive:
    a RAM program polls the status and reads the byte."""
    import socket
    main, core, cases = native_core("amic")
    cwd = folder("ports_tcp")
    port = free_port()

    def body(emit, out, compare, mem) -> None:
        out(1, 0x4E)
        out(1, 0x37)
        for i in range(3):
            amic_serial_wait(emit, 0x02)                                     # RxRDY
            emit(0xDB, 0x00, 0x32, 0x00 + i, 0xB2, 0x3C, 0x57)              # IN A,(0); LD (B200+i),A; INC A; LD D,A
            amic_serial_wait(emit, 0x01)                                     # TxRDY
            emit(0x7A, 0xD3, 0x00)                                           # LD A,D; OUT (0),A
        amic_serial_wait(emit, 0x01)
        emit(0x3E, 0x5A, 0xD3, 0x00)                                         # the program's own byte
    config = with_ports(hcp_config(cwd, "amic", "amic", memory="32"), serial=f"tcp-listen:{port}")
    session = PortSession(config, cwd / "stderr.txt")
    try:
        session.send("until 10 MONITOR")
        for command in poke_commands(0x9000, z80_ports(0x9000, 0xB100, body)) + ["type g9000", "enter", "frames 20"]:
            session.send(command)
        with socket.create_connection(("127.0.0.1", port), timeout=10) as peer:
            peer.settimeout(10)
            session.send("frames 5")                                         # the connection is taken
            peer.sendall(bytes([0x10, 0x20, 0x30]))
            session.send("frames 200")
            got = b""
            while len(got) < 4:
                got += peer.recv(16)
            need(got == bytes([0x11, 0x21, 0x31, 0x5A]), f"what the guest sent to the socket: {got.hex(' ')}")
            seen = peeked(session.send("peek B200 3"))
            need(seen == bytes([0x10, 0x20, 0x30]), f"what the guest read from the socket: {seen.hex(' ')}")
    finally:
        session.close()


def check_ports_cubz() -> None:
    """The CUB Z's printer line (its second SIO channel, the user port with the option's default) reaches a file: CP/M's Ctrl+P echoes what
    is typed and listed to the list device, so the typed DIR and the directory it lists arrive at the `user` port's file end. The serial
    line (SIO A, the keyboard's link) has no end here and nothing breaks."""
    main, disk = cubz_case()
    cwd = cubz_folder("ports_cubz", disk)
    with_ports(cwd / "zetro.yaml", user="file:lst.out")
    commands = ["until 30 A>", "type \x10", "frames 20", "type DIR\\n", "until 20 FISP     DAT", "wait 3"]
    cubz_run(main, cwd, commands)
    data = (cwd / "lst.out").read_bytes()
    need(b"DIR" in data and b"FISP" in data, f"what CP/M listed to the printer line: {data!r}")


def cobra_serial_program(values: list[int], receive: bool) -> bytes:
    """A program for the CoBra 64K under 48 BASIC (RANDOMIZE USR 32768) that times its own bits on the 8255's lines: each of `values` goes out
    on PC7 (port FEh, bit 7) as a start bit, eight data bits LSB first and a stop bit, 372 T-states a bit (9600 baud at 3.5 MHz); with
    `receive` each byte is then read back from PA7 (bit 7 of port FEh): the start bit found, the middle of the first data bit waited for,
    eight samples a bit apart, the byte into 9000h + n."""
    code = bytearray([0xF3, 0x0E, 0xFE, 0x3E, 0x80, 0xED, 0x79, 0x06, 0xFF, 0x10, 0xFE])       # DI; C := FEh; the line idle high, a pause
    delay = lambda n: bytes([0x06, n, 0x10, 0xFE])                                              # LD B,n; DJNZ $
    for n, value in enumerate(values):
        bits = [0] + [value >> k & 1 for k in range(8)] + [1]
        for i, bit in enumerate(bits):
            code += bytes([0x3E, 0x80 if bit else 0x00, 0xED, 0x79])                            # LD A,level; OUT (C),A
            if i != len(bits) - 1:
                code += delay(27)
        if receive:
            code += bytes([0xED, 0x78, 0x17, 0x38, 0xFB]) + delay(40)                          # IN A,(C); RLA; JR C,back: the start bit; 1.5 bits
            for k in range(8):
                code += bytes([0xED, 0x78, 0x17, 0xCB, 0x1B])                                   # IN A,(C); RLA; RR E
                if k != 7:
                    code += delay(26)
            code += bytes([0x7B, 0x32, (0x9000 + n) & 0xFF, (0x9000 + n) >> 8])                 # LD A,E; LD (9000h+n),A
        code += bytes([0x3E, 0x80, 0xED, 0x79]) + delay(0xFF)                                   # idle, a pause
    return bytes(code + bytes([0xFB, 0xC9]))                                                    # EI; RET


def check_ports_cobra() -> None:
    """The CoBra's serial line (PC7 out, PA7 in) through the host's ends: a program in RAM under 48 BASIC times its own bits at 9600 baud
    (the board measures the bit time the guest sends at). With `ports: serial: loopback` the four bytes it sends come back to its input and
    it reads them (55h, A3h, 00h, FFh); with a file end the same bytes are in the file (after the 00h BASIC's border writes made)."""
    main, core, cases = cobra_core()
    cwd = folder("ports_cobra")
    values = [0x55, 0xA3, 0x00, 0xFF]
    after = lambda: ["frames 25", "type b", "wait 2"] + poke_commands(0x8000, cobra_serial_program(values, True)) + usr_commands(32768) + ["frames 40", "peek 9000 4"]
    config = with_ports(hcp_config(cwd, "cobra64", "cobra64"), serial="loopback")
    got = peeked(hc_run(config, after())[-1])
    need(got == bytes(values), f"the bytes the CoBra read back through the loopback: {got.hex(' ')}")
    sender = lambda: ["frames 25", "type b", "wait 2"] + poke_commands(0x8000, cobra_serial_program(values, False)) + usr_commands(32768) + ["frames 40"]
    config = with_ports(hcp_config(cwd, "cobra64_file", "cobra64"), serial="file:serial.out")
    hc_run(config, sender())
    sent = (cwd / "serial.out").read_bytes()
    # BASIC's own writes of port FE (the border: bit 7 low, a start bit) have made a byte of 00h at the host before the program's.
    need(sent[-len(values):] == bytes(values) and len(sent) <= len(values) + 1, f"the bytes the CoBra's line handed to the file: {sent.hex(' ')}")


def check_ports_error() -> None:
    """An end that cannot be opened (a TCP server on a port another program holds) leaves its port unconnected, the run goes on and says why
    on the console (and on the message line); the other port's end is served all the same: the TIM-S's LPRINT reaches its file."""
    import socket
    hc_rom("tims-standard.rom")
    cwd = folder("ports_error")
    with socket.socket() as holder:
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        config = cwd / "tims.yaml"
        config.write_text(f"machine:\n  model: tims\nports:\n  serial: tcp-listen:{holder.getsockname()[1]}\n  printer: file:lpt.out\n", encoding="utf-8")
        extended = ["key CAPS SHIFT", "key SYMBOL SHIFT", "frames 3", "release SYMBOL SHIFT", "release CAPS SHIFT", "frames 3"]
        run = subprocess.run([str(EXE), "--hidden", "--config", str(config), "--control", "stdin"], cwd=main_checkout(), capture_output=True, text=True,
                             timeout=120, input="\n".join(["until 5 IPTV", "frames 40", *extended, 'type c"B"', "enter", "until 20 0 OK", "quit"]) + "\n")
    answers = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    need(run.returncode == 0 and answers and all(a["ok"] for a in answers), f"exit {run.returncode}: {run.stderr.strip()}")
    need("port serial" in run.stderr and "Cannot listen" in run.stderr, f"the console does not say why the serial end is not open: {run.stderr!r}")
    need((cwd / "lpt.out").read_bytes().rstrip() == b"B", f"the printer's file: {(cwd / 'lpt.out').read_bytes()!r}")


def check_ports_page() -> None:
    """The Ports page lists the ports of the running model and what each is connected to (the TIM-S: Serial and Printer); a machine without ports
    says so; an end chosen from the menus (the printer a file with a typed name, the serial line a TCP server) is written to zetro.yaml at
    once, with the file's comments kept, and the page shows it."""
    hc_rom("tims-standard.rom")
    cwd = folder("ports_page")
    (cwd / "zetro.yaml").write_text("# a comment of the file's own\nmachine:\n  model: tims\n", encoding="utf-8")
    main = main_checkout()
    # Down to Printer; Enter opens its kinds (None, host serial port, pipe, TCP connection, TCP server, File: End is File), Enter, Enter
    # on "Type a name" and Enter on the proposed file name. Then Up to Serial, Enter, End and Up (TCP server), Enter, Enter on its port.
    pressed = ["Down", "Return", "End", "Return", "Return", "Return", "Up", "Return", "End", "Up", "Return", "Return"]
    answers = control(main, ["frames 3", "text", "frames 20", "text", "quit"], "--config", str(cwd / "zetro.yaml"), "--setup-page", "ports",
                      *keys(*pressed, first=6))
    shown = answers[1]["text"]
    need(screen_has(shown, r"^\s*ZetRo\s+Ports") and screen_has(shown, r"Serial\s+none") and screen_has(shown, r"Printer\s+none"), f"the page: {shown!r}")
    after = answers[3]["text"]
    need(screen_has(after, r"Printer\s+file:printer\.out") and screen_has(after, r"Serial\s+tcp-listen:2323"), f"the page after the changes: {after!r}")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8")
    need("printer: file:printer.out" in text and "serial: tcp-listen:2323" in text and "# a comment of the file's own" in text,
         f"the ends were not written:\n{text}")
    # What was written is what the next run reads.
    run = zetro(cwd, "--config", "zetro.yaml", "--frames", "2")
    need(run.returncode == 0, f"the file no longer loads: {run.stderr.strip()}")
    # A machine with no byte ports has no Ports page: the next page opens.
    answers = control(cwd, ["frames 3", "text", "quit"], "--model", "test", "--setup-page", "ports")
    need(not screen_has(answers[1]["text"], r"^\s*ZetRo\s+Ports"), f"the test machine's page: {answers[1]['text']!r}")


def check_setup_rules() -> None:
    """What a machine does not have is not on its pages, and what a setting requires is set. The HC-91 without Interface 1 has no
    floppy slot, no card, no serial port (so no Ports page) and no serial speed; set to start CP/M it has Interface 1 on, the
    drives, the serial port with its speed on the Ports page and no added interface although the file names one; drive A is
    empty until the user names a disk for it."""
    hc_rom("hc91.rom")
    cwd = folder("setup_rules")
    main = main_checkout()
    config = cwd / "zetro.yaml"

    def page(name: str) -> str:
        return control(main, ["frames 3", "text", "quit"], "--config", str(config), "--setup-page", name)[1]["text"]

    config.write_text("machine:\n  model: hc91\n", encoding="utf-8")
    media, machine, ports = page("media"), page("machine"), page("ports")
    need("Floppy A" not in media and "DivMMC" not in media and "Dandanator" not in media, f"the media of an HC-91 without interfaces: {media!r}")
    need("Serial speed" not in machine and "Dandanator programming" not in machine and screen_has(machine, r"^\*Interface\s"),
         f"the options of an HC-91 without interfaces: {machine!r}")
    need(not screen_has(ports, r"^\s*ZetRo\s+Ports"), f"an HC-91 without Interface 1 has a Ports page: {ports!r}")
    config.write_text("machine:\n  model: hc91\n  options:\n    startup: cpm\n    interface: divmmc\n", encoding="utf-8")
    media, machine, ports = page("media"), page("machine"), page("ports")
    need("Floppy A" in media and "DivMMC" not in media, f"the media of an HC-91 set to start CP/M: {media!r}")
    need(".dsk" not in media, f"a disk came into drive A by itself: {media!r}")
    need(screen_has(machine, r"Interface 1\s+\[X\]") and not screen_has(machine, r"^\*Interface\s"),
         f"the options of an HC-91 set to start CP/M: {machine!r}")
    need(screen_has(ports, r"^\s*ZetRo\s+Ports") and screen_has(ports, r"Serial\s+none") and "Serial speed" in ports, f"its Ports page: {ports!r}")
    text = config.read_text(encoding="utf-8")
    need(".dsk" not in text and "interface1" not in text, f"the rules wrote to the configuration:\n{text}")


def jet_save_commands(first_save: bool = True) -> list[str]:
    """The JET at its copyright screen, `10 PRINT 1`, and SAVE "x" up to its "Start tape" prompt."""
    return ["until 5 JET", "frames 20", "type 10p1", "enter", "frames 5", 'type s"x"', "enter", "until 5 Start tape"]


def tape_record_run(config: Path, hostkeys: list[str], tail: list[str]) -> list[dict]:
    """The recording case on the JET with its save trap off: the program is SAVEd in real time while the tape recorder records (Shift+F5 at the
    frames in `hostkeys`); the answers to the commands after the prompt (`tail`)."""
    commands = jet_save_commands() + ["frames 10", "type z"] + tail + ["quit"]
    answers = control(main_checkout(), commands, "--config", str(config), *hostkeys)
    need(all(a["ok"] for a in answers[:-1]), f"{[a for a in answers if not a['ok']]}")
    return answers


def tape_record_case(name: str, config_text: str, tape: Path) -> None:
    """SAVE on the JET (real time, the save trap off) while recording: the recorder is started with Shift+F5 as the prompt waits for a key and
    stopped after the SAVE ended; the tape file written holds the program, and a new run loads it from the file and lists it."""
    cwd = tape.parent.parent if tape.parent.name == "media" else tape.parent
    config = cwd / "zetro.yaml"
    config.write_text(config_text, encoding="utf-8")
    # A run without the key finds the frames the SAVE takes (the machine is deterministic), then the run with it records.
    dry = tape_record_run(config, [], ["until 20 0 OK", "frames 2"])
    prompt = dry[jet_save_commands().index("until 5 Start tape")]["frame"]
    ended = dry[-3]["frame"]
    need(ended > prompt + 100, f"the SAVE took {ended - prompt} frames")
    tape_record_run(config, ["--hostkey", f"shift+F5@{prompt + 3}", "--hostkey", f"shift+F5@{ended + 10}"], ["until 20 0 OK", "frames 30"])
    need(tape.is_file() and tape.stat().st_size > 30, f"the tape file {tape} was not written")
    # A fresh run: the tape in, LOAD "", the program listed.
    (cwd / "load.yaml").write_text(f"machine:\n  model: jet\n  options:\n    tape_traps: on\nmedia:\n  tape:\n    image: '{tape}'\n", encoding="utf-8")
    loaded = control(main_checkout(), ["until 5 JET", "frames 20", 'type j""', "enter", "frames 120", "type k", "enter", "frames 10", "text", "quit"],
                     "--config", str(cwd / "load.yaml"))
    need("10 PRINT 1" in loaded[-2]["text"], f"the recorded tape does not load: {loaded[-2]['text']!r}")


def check_tape_record_setup() -> None:
    """The setup screen's Record row (Media page) starts recording on an empty slot, making media/tape-0001.tzx and keeping it as the slot's file in
    zetro.yaml; the machine SAVEs in real time (the save trap off); the tape browser's W key ends the recording, the browser lists the blocks
    that were added and the tape file holds them: a fresh run loads the program from it."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped("no JET ROM")
    cwd = folder("tape_record_setup")
    shutil.rmtree(cwd / "media", ignore_errors=True)
    config = cwd / "zetro.yaml"
    config.write_text("machine:\n  model: jet\n  options:\n    tape_traps: off\n    tape_save_traps: off\n", encoding="utf-8")
    dry = control(main, jet_save_commands() + ["frames 10", "type z", "until 20 0 OK", "frames 2", "quit"], "--config", str(config))
    need(all(a["ok"] for a in dry[:-1]), f"{[a for a in dry if not a['ok']]}")
    ended = dry[-3]["frame"]
    # The screen opens on Media with the slot empty: its rows File, Protect, Fast loading, Tape sound, Auto-load and Record can be chosen
    # (Down x5), Enter records, F12 closes. After the SAVE: F12, the Tape blocks row (Down x6), Enter opens the browser and W stops the
    # recording.
    pressed = keys(*["Down"] * 5, "Return", "F12", first=3) + keys("F12", *["Down"] * 6, "Return", "W", first=ended + 60)
    commands = jet_save_commands() + ["frames 10", "type z", "until 20 0 OK", "frames 200", "until 5 Program: x", "text", "quit"]
    answers = control(main, commands, "--config", str(config), "--setup-page", "media", *pressed)
    need(all(a["ok"] for a in answers[:-1]), f"{[a for a in answers if not a['ok']]}")
    browser = answers[-2]["text"]
    need(screen_has(browser, r"1 Program: x") and screen_has(browser, r"2 Data"), f"the browser after the recording: {browser!r}")
    tape = cwd / "media" / "tape-0001.tzx"
    need(tape.is_file() and tape.stat().st_size > 30, f"the tape file {tape} was not written")
    text = config.read_text(encoding="utf-8")
    need("tape-0001.tzx" in text.replace("\\", "/"), f"the new tape is not the slot's file in zetro.yaml:\n{text}")
    (cwd / "load.yaml").write_text("machine:\n  model: jet\n  options:\n    tape_traps: on\nmedia:\n  tape:\n    image: media/tape-0001.tzx\n", encoding="utf-8")
    loaded = control(main, ["until 5 JET", "frames 20", 'type j""', "enter", "frames 120", "type k", "enter", "frames 10", "text", "quit"],
                     "--config", str(cwd / "load.yaml"))
    need("10 PRINT 1" in loaded[-2]["text"], f"the recorded tape does not load: {loaded[-2]['text']!r}")


def check_tape_record_jet() -> None:
    """Shift+F5 records on the tape in the first slot and stops: SAVE on the JET with its save trap off (the ROM's real-time saver) between the
    two presses writes the tape file back, and a fresh run loads the program from it."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped("no JET ROM")
    cwd = folder("tape_record_jet")
    tape = cwd / "rec.tzx"
    tape.write_bytes(b"")
    tape_record_case("jet", f"machine:\n  model: jet\n  options:\n    tape_traps: off\n    tape_save_traps: off\nmedia:\n  tape:\n    image: '{tape}'\n", tape)


def check_tape_record_new() -> None:
    """With the tape slot empty Shift+F5 makes media/tape-0001.tzx beside the configuration, puts it in and records on it, so SAVE with no tape in
    works: the file is there afterwards and a fresh run loads the program from it."""
    main = main_checkout()
    if not (main / "roms" / "jet.rom").is_file():
        raise Skipped("no JET ROM")
    cwd = folder("tape_record_new")
    shutil.rmtree(cwd / "media", ignore_errors=True)
    tape_record_case("jet", "machine:\n  model: jet\n  options:\n    tape_traps: off\n    tape_save_traps: off\n", cwd / "media" / "tape-0001.tzx")


# --- the DivIDE and the DivMMC (src/hw/divmem.h; the option "interface", the slots "card" and "card2") --------------------------
#
# The checks run the real esxDOS 0.8.9 firmware (roms/esxmmc.bin, roms/esxide.bin) on a card tools/make_card.py makes from the
# distribution tools/fetch_roms.py unpacks into work/esxdos/0.8.9; without either they are skipped. Each runs in a folder of its own
# with a roms/ folder holding just the ROMs it needs, so the main checkout's does not matter. The firmware's text is read back
# through the ROM's character set: its logo is graphics ("?"), its lines are text.

DIV_ROMS = {"jet": ("jet.rom",), "hc91": ("hc91.rom", "if90.rom", "hc-family-cpm.rom"), "hc128": ("hc128.rom", "if90.rom", "hc-family-cpm.rom"),
            "hc2000": ("hc2000-0.rom", "hc2000-1.rom")}


def div_find(*relative: str) -> Path | None:
    """A file under this checkout or the main one."""
    for base in (ROOT, main_checkout()):
        if (base.joinpath(*relative)).exists():
            return base.joinpath(*relative)
    return None


def div_tools():
    """tools/make_card.py as a module, and the esxDOS distribution it builds a card from."""
    sys.path.insert(0, str(ROOT / "tools"))
    import make_card
    distribution = div_find("work", "esxdos", "0.8.9")
    if distribution is None or not (distribution / "SYS" / "ESXDOS.SYS").is_file():
        raise Skipped("no esxDOS distribution in work/esxdos/0.8.9 (python tools/fetch_roms.py)")
    return make_card, distribution


def div_folder(name: str, model: str, interface: str, card: bool = True, firmware: bool = True, protect: bool = False, size: int = 64,
               files: dict[str, bytes] | None = None, second: bool = False, **options: str) -> Path:
    """A work folder for `model` with the interface on, its roms/ (the model's ROMs and the firmware), and with `card` a card image
    of `size` MiB made from the distribution (and `files`) in its slot (write-protected with `protect`), with `second` a blank 32 MiB
    image in the second slot."""
    make_card, distribution = div_tools() if card else (None, None)
    names = DIV_ROMS[model]
    if firmware:
        names += ({"divmmc": "esxmmc.bin", "divide": "esxide.bin"}[interface],)
    sources = [div_find("roms", n) for n in names]
    if not all(sources):
        raise Skipped(f"no {', '.join(n for n, s in zip(names, sources) if not s)} in roms/ (python tools/fetch_roms.py)")
    cwd = folder(name)
    shutil.rmtree(cwd / "roms", ignore_errors=True)
    (cwd / "roms").mkdir()
    for n, source in zip(names, sources):
        shutil.copyfile(source, cwd / "roms" / n)
    opts = {"interface": interface, **options}
    text = "machine:\n  model: " + model + "\n  options:\n" + "".join(f"    {k}: {v}\n" for k, v in opts.items())
    if card:
        image = cwd / "card.img"
        make_card.build_card(image, size, {**make_card.distribution_files(distribution), **(files or {})})
        text += f"media:\n  card:\n    image: {image.as_posix()}\n" + ("    write_protect: true\n" if protect else "")
        if second:
            with open(cwd / "card2.img", "wb") as f:
                f.truncate(32 << 20)
            text += f"  card2:\n    image: {(cwd / 'card2.img').as_posix()}\n"
    (cwd / "zetro.yaml").write_text(text + "video:\n  look: plain\n", encoding="utf-8")
    return cwd


def div_run(cwd: Path, commands: list[str]) -> list[dict]:
    """The commands on the control channel in the folder; every answer but the last is ok."""
    answers = control(cwd, commands + ["quit"], "--config", str(cwd / "zetro.yaml"))
    failed = [(c, a) for c, a in zip(commands, answers) if not a.get("ok")]
    need(not failed, f"{failed[:2]}")
    return answers[:-1]


def div_screen(answers: list[dict], at: int = -1) -> list[str]:
    return answers[at]["text"].splitlines()


def div_startup(interface: str, what: str, lines: list[str]) -> None:
    text = "\n".join(lines)
    drive = "sda: ZR ZETRO" if interface == "divmmc" else "hda: ZetRo IDE disk"
    for expected in (f"v0.8.9-{'DivMMC' if interface == 'divmmc' else 'DivIDE'}", "Papaya Dezign", "Detecting Devices...", drive,
                     "hd0: ZETRO, FAT16, 63M", "Loading ESXDOS.SYS...      [OK]", "Loading NMI.SYS...         [OK]",
                     "Loading BETADISK.SYS...    [OK]"):
        need(expected in text, f"{what}: {expected!r} is not on the screen: {text!r}")


def div_listing(lines: list[str], what: str) -> None:
    text = "\n".join(lines)
    for expected in ("/:", "BIN             <DIR>", "SYS             <DIR>", "TMP             <DIR>"):
        need(expected in text, f"{what}: {expected!r} is not in the listing: {text!r}")


# The key presses of a BASIC session: 10 PRINT 42, SAVE *"test", NEW, LOAD *"test", LIST, RUN (the keywords are single keys).
DIV_SESSION = ["type 10 p42", "enter", "frames 25", "type s*\"test\"", "enter", "wait 3", "text", "type a", "enter", "wait 2", "text",
               "type j*\"test\"", "enter", "wait 3", "type k", "enter", "wait 2", "text", "type r", "enter", "wait 1", "text"]


def div_session(cwd: Path, interface: str, what: str, boot_shot: str) -> None:
    """From power on: esxDOS starts (the lines it prints), BASIC comes up, .ls lists the card's root, a program saved to the card
    appears in the image on the host and loads back after NEW."""
    answers = div_run(cwd, ["until 10 BETADISK", "until 10 Electromagnetica", "text", f"shot {cwd / boot_shot}", "type .ls", "enter", "wait 3",
                            "text", *DIV_SESSION])
    shots = [a for a in answers if "text" in a]
    basic, listing, saved, new, loaded, ran = shots
    need("Electromagnetica" in basic["text"], f"{what}: BASIC did not come up: {basic['text']!r}")
    div_listing(listing["text"].splitlines(), what)
    need("0 OK, 0:1" in saved["text"], f"{what}: SAVE did not finish: {saved['text']!r}")
    need("10 PRINT 42" not in new["text"], f"{what}: NEW left the program: {new['text']!r}")
    need("10 PRINT 42" in loaded["text"], f"{what}: the program did not load back: {loaded['text']!r}")
    need(any(l.strip() == "42" for l in ran["text"].splitlines()), f"{what}: RUN: {ran['text']!r}")
    card = div_tools()[0].read_card(cwd / "card.img")
    need("TEST" in card and card["TEST"][:8] == b"PLUS3DOS" and b"\xf5" in card["TEST"] and b"42" in card["TEST"],
         f"{what}: the saved program is not in the image's FAT directory: {sorted(k for k in card if not k.startswith(('BIN/', 'SYS/')))}")


def check_div_mmc_jet() -> None:
    """The JET with the DivMMC and a card made by tools/make_card.py: esxDOS starts (read back as text), BASIC comes up, a dot command
    lists the card's root, SAVE *"test" puts the program in the image (found in its FAT directory) and LOAD brings it back after NEW."""
    cwd = div_folder("div_mmc_jet", "jet", "divmmc")
    boot = div_run(cwd, ["until 10 BETADISK", "wait 0.4", "text", f"shot {cwd / 'startup.png'}"])
    div_startup("divmmc", "the JET's start-up", div_screen(boot, 2))
    div_session(cwd, "divmmc", "the JET with the DivMMC", "basic.png")
    need((cwd / "startup.png").stat().st_size > 500, "startup.png")


def check_div_ide_jet() -> None:
    """The same start-up and .ls and SAVE with the DivIDE and the card image as its IDE drive (the machine's IDENTIFY, then the sectors),
    and a second drive found by its IDENTIFY."""
    cwd = div_folder("div_ide_jet", "jet", "divide", second=True)
    boot = div_run(cwd, ["until 10 BETADISK", "wait 0.4", "text"])
    div_startup("divide", "the JET's start-up", div_screen(boot))
    need("hdb: ZetRo IDE disk" in "\n".join(div_screen(boot)), f"the second drive is not found: {div_screen(boot)!r}")
    div_session(cwd, "divide", "the JET with the DivIDE", "basic.png")


def check_div_models() -> None:
    """The DivMMC on the HC-91 (Interface 1 off): esxDOS starts and .ls lists the root, and the NMI key (F10) opens esxDOS's menu, its
    browser of the card (SPACE leaves it, BASIC runs on); on the HC-91 Plus, which pages ROMs, the interface acts only while 48 BASIC is
    the ROM at 0000h (it does not at reset, in the 128 editor's ROM; it does once the menu has chosen 48 BASIC); with Interface 1 on
    the HC-91 has no added interface, whatever the file says."""
    cwd = div_folder("div_hc91", "hc91", "divmmc")
    answers = div_run(cwd, ["until 10 BETADISK", "wait 0.4", "text", "until 10 FELIX", "type .ls", "enter", "wait 3", "text", "nmi", "wait 3", "text",
                            f"shot {cwd / 'nmi.png'}", "key SPACE", "frames 4", "release SPACE", "wait 3", "text", "type 10 p42", "enter", "type r",
                            "enter", "wait 1", "text"])
    texts = [a["text"] for a in answers if "text" in a]
    div_startup("divmmc", "the HC-91's start-up", texts[0].splitlines())
    div_listing(texts[1].splitlines(), "the HC-91")
    menu = texts[2]
    need(all(w in menu for w in ("BIN", "SYS", "TMP", "[1/3]")) and "FELIX" not in menu, f"esxDOS's menu is not open: {menu!r}")
    need("[1/3]" not in texts[3] and "0 OK, 0:1" in texts[3], f"the menu did not close: {texts[3]!r}")
    need(any(l.strip() == "42" for l in texts[4].splitlines()), f"BASIC does not run after the menu: {texts[4]!r}")
    # The HC-91 Plus: the 128 editor's ROM is at 0000h at reset; the interface waits for 48 BASIC.
    cwd = div_folder("div_hc128", "hc128", "divmmc")
    ram = bytearray(49152)                                       # a 48K snapshot that starts at 0000h: its SP at 8000h holds 0
    header = bytearray(27)
    header[23:25] = struct.pack("<H", 0x8000)
    header[25] = 1
    (cwd / "start.sna").write_bytes(bytes(header + ram))
    answers = div_run(cwd, ["wait 5", "text", f"snapshot load {cwd / 'start.sna'}", "until 10 BETADISK", "wait 0.4", "text", "until 10 FELIX", "type .ls",
                            "enter", "wait 3", "text"])
    texts = [a["text"] for a in answers if "text" in a]
    need("Tape Loader" in texts[0] and "Detecting" not in texts[0], f"esxDOS started in the 128 editor's ROM: {texts[0]!r}")
    div_startup("divmmc", "the HC-91 Plus's start-up (48 BASIC)", texts[1].splitlines())
    div_listing(texts[2].splitlines(), "the HC-91 Plus")
    cwd = div_folder("div_conflict", "hc91", "divmmc", interface1="on")
    run = zetro(cwd, "--config", str(cwd / "zetro.yaml"), "--frames", "1")
    said = run.stdout + run.stderr
    need(run.returncode == 0 and "HC-91, Interface 1" in said and "DivMMC" not in said, f"both on: exit {run.returncode}, {said.strip()!r}")


def check_div_states() -> None:
    """A state saved in esxDOS's menu comes back to the menu, in the run and in a new one; a state saved in the middle of a read of the
    card (found among the states of the start-up, by the card's protocol chunk) goes on to the same screens as the run it was saved in,
    in a new run."""
    cwd = div_folder("div_states", "hc91", "divmmc")
    menu_state = cwd / "menu.zrs"
    answers = div_run(cwd, ["until 10 FELIX", "frames 50", "nmi", "wait 3", "text", f"state save {menu_state}", "key SPACE", "frames 4", "release SPACE",
                            "wait 2", "text", f"state load {menu_state}", "frames 2", "text"])
    texts = [a["text"] for a in answers if "text" in a]
    need("[1/3]" in texts[0] and "FELIX" in texts[1] and texts[2] == texts[0], f"the menu's state: {texts!r}")
    fresh = [a["text"] for a in div_run(cwd, [f"state load {menu_state}", "frames 2", "text", "key SPACE", "frames 4", "release SPACE", "wait 2", "text"]) if "text" in a]
    need(fresh[0] == texts[0] and "FELIX" in fresh[1], f"the menu's state in a new run: {fresh!r}")
    # In the middle of a card read: states a frame apart while esxDOS loads ESXDOS.SYS, the card's protocol chunk says which.
    cwd = div_folder("div_states_read", "jet", "divmmc")
    count, total = 40, 340
    screen = [f"peek {0x4000 + i:04X} 400" for i in range(0, 0x1B00, 0x400)]
    commands = ["until 10 Loading"] + [c for i in range(count) for c in ("frames 1", f"state save {cwd / f's{i}.zrs'}")] + [f"frames {total - count}", "text", *screen, f"state save {cwd / 'end.zrs'}"]
    run = div_run(cwd, commands)
    final = run[-2 - len(screen)]["text"], [a["data"] for a in run[-1 - len(screen):-1]]
    mid = None
    for i in range(count):
        chunks = state_chunks(cwd / f"s{i}.zrs")
        mmc = chunks["MMC "][0]
        tx_len, tx_pos = struct.unpack_from("<II", mmc, 48)
        if 0 < tx_pos < tx_len:
            mid = i
            break
    need(mid is not None, "no state of the start-up was saved in the middle of a block of the card")
    resumed = div_run(cwd, [f"state load {cwd / f's{mid}.zrs'}", f"frames {total - (mid + 1)}", "text", *screen,
                               f"state save {cwd / 'end2.zrs'}"])
    need((resumed[-2 - len(screen)]["text"], [a["data"] for a in resumed[-1 - len(screen):-1]]) == final,
         f"a state saved in the middle of a read (frame {mid + 1}) goes on to another screen")
    # The whole machine, but the picture's serial number (the ULA chunk), the clock and the thumbnail, which are the shell's.
    same = lambda path: {k: v for k, v in state_chunks(path).items() if k not in ("ULA ", "TIME", "THMB")}
    need(same(cwd / "end2.zrs") == same(cwd / "end.zrs"), "the machine's state differs after the resumed run")


def check_div_missing() -> None:
    """Without the firmware the machine is not built: zetro stops with a message naming the file and the fetch tool."""
    for interface, name in (("divmmc", "esxmmc.bin"), ("divide", "esxide.bin")):
        cwd = div_folder(f"div_missing_{interface}", "jet", interface, card=False, firmware=False)
        run = zetro(cwd, "--config", str(cwd / "zetro.yaml"), "--frames", "1")
        need(run.returncode != 0 and name in run.stderr and "tools/fetch_roms.py" in run.stderr, f"{interface}: exit {run.returncode}, {run.stderr.strip()!r}")


def check_div_protect() -> None:
    """A write-protected card image is not changed: SAVE fails and the file is byte for byte what it was."""
    cwd = div_folder("div_protect", "jet", "divmmc", protect=True)
    before = hashlib.sha256((cwd / "card.img").read_bytes()).hexdigest()
    answers = div_run(cwd, ["until 10 Electromagnetica", "type 10 p42", "enter", "frames 25", "type s*\"test\"", "enter", "wait 3", "text", "type .ls", "enter", "wait 2", "text"])
    saved, listing = [a["text"] for a in answers if "text" in a]
    need("0 OK" not in saved, f"SAVE to a protected card went through: {saved!r}")
    need("TEST" not in listing, f"the file is listed: {listing!r}")
    need(hashlib.sha256((cwd / "card.img").read_bytes()).hexdigest() == before, "the write-protected image changed")


def check_div_new_card() -> None:
    """The setup screen's Media page: "New blank card" on the card slot makes card-0001.img (raw, 64 MiB) in media/ beside the
    configuration and puts it in the slot (zetro.yaml names it; the slot's row shows it)."""
    cwd = div_folder("div_new_card", "jet", "divmmc", card=False)
    shutil.rmtree(cwd / "media", ignore_errors=True)
    (cwd / "media").mkdir()
    # Down to the card slot's New blank card row (tape: File, Protect, Fast loading, Tape sound, Auto-load, Record; the card: File,
    # Protect, Fast, New; the Dandanator's set is not on the page of a machine with the DivMMC), Enter opens its sizes, Enter takes
    # the first.
    pressed = ["Down"] * 9 + ["Return", "Return"]
    script(cwd, ["frames 20", "text", "shot media.png"], "--setup-page", "media", *keys(*pressed))
    image = cwd / "media" / "card-0001.img"
    need(image.is_file() and image.stat().st_size == 64 << 20, f"no 64 MiB image: {sorted(p.name for p in (cwd / 'media').iterdir())}")
    need(image.read_bytes()[:4096] == bytes(4096), "the new card is not blank")
    text = (cwd / "zetro.yaml").read_text(encoding="utf-8").replace("\\", "/")
    need("card:" in text and "media/card-0001.img" in text, f"the card was not written to the configuration:\n{text}")


# --- the on-screen keyboard (F8) -------------------------------------------------------------------------------------------

OSK_KEY, OSK_HELD, OSK_LATCHED, OSK_EDGE = (0, 0, 205), (255, 255, 255), (0, 255, 0), (0, 255, 255)
OSK_KEYWORD, OSK_SYMBOL = (0, 255, 0), (255, 255, 0)


def osk_rects(text: str) -> dict[str, tuple[int, int, int, int]]:
    """`osk keys`: the keycaps as x, y, width, height in the frame's pixels, by the key's name."""
    rects = {}
    for line in text.splitlines():
        x, y, w, h, name = line.split(" ", 4)
        rects[name] = (int(x), int(y), int(w), int(h))
    return rects


def spectrum_keys() -> dict[str, tuple[int, int, int, int]]:
    """The Spectrum layout's 40 keys (x, y, w, h in quarters of a key) as src/core/machines/spectrum.c has them."""
    text = (ROOT / "src" / "core" / "machines" / "spectrum.c").read_text(encoding="utf-8")
    block = text[text.index("zr_spectrum_keys[ZR_SPECTRUM_KEYS] = {"):text.index("#undef K")]
    keys = {m[1]: (int(m[2]), int(m[3]), 4, 4) for m in re.finditer(r'\bK\("(\w)", (\d+), (\d+)\)', block)}
    keys.update({m[1]: tuple(int(m[i]) for i in (2, 3, 4, 5)) for m in re.finditer(r'\{"([A-Z ]+)", "[^"]*", "[^"]*", (\d+), (\d+), (\d+), (\d+)\}', block)})
    need(len(keys) == 40, f"{len(keys)} keys read from spectrum.c")
    return keys


def osk_painted(rows, rect, colour) -> int:
    x, y, w, h = rect
    return sum(px == colour for row in rows[y:y + h] for px in row[x:x + w])


def check_osk_jet() -> None:
    """On the JET F8 opens the on-screen keyboard: 40 keycaps with their legends at the places of the key table in the lower part of the picture
    (the symbol-shift character and the keyword on the keycaps that have room), the machine's screen above it unchanged; a held host key shows
    inverted; a second F8 closes it, leaving the machine's picture alone; the status line says how to close it."""
    cwd, main = jet_cwd("osk_jet")
    config = str(cwd / "zetro.yaml")
    shots = {name: cwd / f"{name}.png" for name in ("closed", "open", "f8", "held", "up")}
    answers = control(main, ["until 5 JET", "frames 10", f"shot {shots['closed']}", "frames 100", f"shot {shots['open']}", "osk keys", "frames 250",
                             f"shot {shots['f8']}", "quit"], "--config", config, "--hostkey", "F8@150", "--hostkey", "F8@300")
    need(all(a["ok"] for a in answers), f"{answers}")
    closed, opened, again = (png_rows(shots[n]) for n in ("closed", "open", "f8"))
    need((len(opened[0]), len(opened)) == (352, 288), f"the JET's frame is {len(opened[0])}x{len(opened)}")
    rects = osk_rects(answers[5]["text"])
    table = spectrum_keys()
    need(set(rects) == set(table), f"the keycaps are not the table's keys: {set(rects) ^ set(table)}")
    # the places follow the table: one scale from the keys at its ends, every keycap within two pixels of where the table puts it
    sx = (rects["ENTER"][0] - rects["1"][0]) / (table["ENTER"][0] - table["1"][0])
    sy = (rects["CAPS SHIFT"][1] - rects["1"][1]) / (table["CAPS SHIFT"][1] - table["1"][1])
    for name, (tx, ty, tw, th) in table.items():
        x, y, w, h = rects[name]
        need(abs(x - (rects["1"][0] + tx * sx)) <= 2 and abs(y - (rects["1"][1] + ty * sy)) <= 2 and abs(w + 1 - tw * sx) <= 2 and abs(h + 1 - th * sy) <= 2,
             f"{name}: the keycap {rects[name]} is not where the table puts it")
        need(y >= 144 and x >= 0 and x + w <= 352 and y + h <= 288, f"{name}: the keycap {rects[name]} is not in the lower part of the picture")
        need(opened[y + 2][x + 2] == OSK_KEY, f"{name}: no keycap at {rects[name]}")
        need(osk_painted(opened, rects[name], (255, 255, 255)) >= 3, f"{name}: no legend on the keycap")
    need(opened[rects["1"][1] + 8][rects["1"][0] + rects["1"][2]] == (0, 0, 0), "no gap between the keycaps")
    need(osk_painted(opened, rects["Q"], OSK_KEYWORD) >= 8 and osk_painted(opened, rects["P"], OSK_SYMBOL) >= 2, "the keyword of Q or the symbol of P is not drawn")
    # the machine's picture above the keyboard is its own
    top = min(r[1] for r in rects.values()) - 4
    need(top > 96 and opened[:top] == closed[:top] and again[:top] == closed[:top], "the machine's picture above the keyboard changed")
    need(not any(OSK_KEY in row for row in closed) and not any(OSK_KEY in row for row in again), "the keyboard is still drawn when closed")
    # a key held on the host's keyboard is inverted on the keyboard, and back when let go
    answers = control(main, ["until 5 JET", "osk on", "frames 100", f"shot {shots['held']}", "osk keys", "frames 120", f"shot {shots['up']}", "quit"],
                      "--config", config, "--hostkey", "+Q@120", "--hostkey", "-Q@200")
    q = osk_rects(answers[4]["text"])["Q"]
    need(png_rows(shots["held"])[q[1] + 2][q[0] + 2] == OSK_HELD and png_rows(shots["up"])[q[1] + 2][q[0] + 2] == OSK_KEY, "a held host key is not shown held")
    # the message over the picture
    run = zetro(main, "--config", config, "--hostkey", "F8@10", "--frames", "30", "--screenshot", str(cwd / "message.png"))
    need(run.returncode == 0, f"exit {run.returncode}: {run.stderr.strip()}")
    bottom = png_rows(cwd / "message.png")[280:288]
    need(any((255, 255, 255) in row for row in bottom) and not any(OSK_EDGE in row[:280] for row in bottom), "no message on the bottom line")


def check_osk_click() -> None:
    """A click on a keycap types on the machine: a BASIC line typed through the keyboard itself (1, 0, PRINT, a quote from a latched SYMBOL SHIFT,
    a letter, the quote again, ENTER) is listed, and RUN and ENTER run it; a latched shift key shows latched and goes after the next key."""
    cwd, main = jet_cwd("osk_click")
    shot = cwd / "latched.png"
    commands = ["until 5 JET", "osk on", "osk keys"] + [f"osk click {k}" for k in ("1", "0", "P", "SYMBOL SHIFT")] + [f"shot {shot}"] + \
               [f"osk click {k}" for k in ("P", "A", "SYMBOL SHIFT", "P", "ENTER")] + \
               ["text", f"shot {cwd / 'after.png'}", "osk click R", "osk click ENTER", "frames 5", "text", "quit"]
    answers = control(main, commands, "--config", str(cwd / "zetro.yaml"))
    need(all(a["ok"] for a in answers), f"{[a for a in answers if not a['ok']]}")
    ss = osk_rects(answers[2]["text"])["SYMBOL SHIFT"]
    need(png_rows(shot)[ss[1] + 2][ss[0] + 2] == OSK_LATCHED, "the latched SYMBOL SHIFT is not shown latched")
    need(png_rows(cwd / "after.png")[ss[1] + 2][ss[0] + 2] == OSK_KEY, "the shift key stays latched after the keys that used it")
    listing = [a["text"] for c, a in zip(commands, answers) if c == "text"]
    need(re.search(r'10.PRINT "a"', listing[0]), f"the line typed on the keyboard is not listed: {listing[0]!r}")
    need(re.search(r"^a$", listing[1], re.M) and "0 OK" in listing[1], f"the line did not run: {listing[1]!r}")
    # the keyboard takes no click once closed
    closed = control(main, ["until 5 JET", "osk on", "osk off", "osk click 1", "osk pad up", "quit"], "--config", str(cwd / "zetro.yaml"))
    need(closed[2]["ok"] and not closed[3]["ok"] and not closed[4]["ok"], f"a closed keyboard took a click: {closed}")
    # the setup screen (F12 opens it at frame 120 and closes it at 140) closes the keyboard, which stays closed
    shown = control(main, ["until 5 JET", "osk on", "frames 60", "osk keys", "quit"], "--config", str(cwd / "zetro.yaml"), "--hostkey", "F12@120", "--hostkey", "F12@140")
    need(shown[1]["ok"] and not shown[3]["ok"] and "closed" in shown[3]["error"], f"the keyboard survived the setup screen: {shown}")


def check_osk_pad() -> None:
    """A game controller's d-pad moves the selection from key to key by geometry (the selected keycap gets the bright edge), A presses the selected
    key, B closes the keyboard and the picture is the machine's alone again."""
    cwd, main = jet_cwd("osk_pad")
    shots = {name: cwd / f"{name}.png" for name in ("before", "moved", "closed")}
    commands = ["until 5 JET", "osk on", "frames 3", f"shot {shots['before']}", "osk keys", "osk pad up"] + ["osk pad left"] * 8 + \
               [f"shot {shots['moved']}", "osk pad down", "osk pad up", "osk pad right", "osk pad left", "osk pad a", "frames 5", "text",
                "osk pad b", "frames 3", f"shot {shots['closed']}", "quit"]
    answers = control(main, commands, "--config", str(cwd / "zetro.yaml"))
    need(all(a["ok"] for a in answers), f"{[a for a in answers if not a['ok']]}")
    rects = osk_rects(answers[4]["text"])
    moves = [a.get("data") for c, a in zip(commands, answers) if c.startswith("osk pad")]
    # from the middle (G) up to T, then left along the row and up to the first key, which has nothing further left
    need(moves[:9] == ["T", "R", "E", "W", "Q", "1", "1", "1", "1"], f"the selection went {moves}")
    need(moves[9] != "1" and moves[10] == "1" and moves[11] != "1" and moves[12] == "1" and moves[13] == "1", f"down, up, right, left and A: {moves[9:]}")
    one = rects["1"]
    before, moved, closed = (png_rows(shots[n]) for n in ("before", "moved", "closed"))
    need(OSK_EDGE not in before[one[1] + one[3] // 2][one[0]:one[0] + one[2]], "a key is selected before the pad was used")
    need(moved[one[1] + one[3] // 2][one[0]] == OSK_EDGE, "the selected keycap has no bright edge")
    # A pressed 1 on the machine: the line being typed shows it
    typed = [a["text"] for c, a in zip(commands, answers) if c == "text"][0].splitlines()
    need(any(line.startswith("1") for line in typed[-3:]), f"A did not type the key: {typed[-3:]!r}")
    need(not any(OSK_KEY in row for row in closed), "B did not close the keyboard")


LAYOUT_ROMS = {"jet": ["jet.rom"], "hc85": ["hc85.rom", "if85.rom"], "hc88": ["hc88.rom"], "cip01": ["cip01.rom"], "tims": ["tims-standard.rom"],
               "timsplus": ["plus-loader.rom", "plus3-system.rom"], "cobra80": ["cobra.rom"], "cubz14": ["cubz-v14.rom"], "amic": ["amic.rom"],
               "prae36": ["prae-3.6.rom"]}


def check_osk_layouts() -> None:
    """The keyboard of every family on its own frame (352 x 288, 704 x 288, 544 x 288, 320 x 288): each keycap inside the frame and clear of the others,
    each with a legend, nothing drawn outside the keycaps but the panel's black; the screenshots are work/verify/osk_layouts/MODEL.png."""
    main = main_checkout()
    cwd = folder("osk_layouts")
    ran = 0
    for model, roms in LAYOUT_ROMS.items():
        if not all((main / "roms" / rom).is_file() for rom in roms):
            continue
        shot = cwd / f"{model}.png"
        config = hcp_config(cwd, model, model)
        answers = control(main, ["frames 20", "osk on", "frames 3", f"shot {shot}", "osk keys", "quit"], "--config", str(config))
        need(all(a["ok"] for a in answers), f"{model}: {[a for a in answers if not a['ok']]}")
        rows = png_rows(shot)
        width, height = len(rows[0]), len(rows)
        rects = osk_rects(answers[4]["text"])
        need(len(rects) >= 40, f"{model}: {len(rects)} keycaps")
        names = list(rects)
        for i, a in enumerate(names):
            x, y, w, h = rects[a]
            need(x >= 0 and y >= height // 2 - 20 and x + w <= width and y + h <= height and w >= 8 and h >= 8, f"{model}: {a} {rects[a]} is off the frame {width}x{height}")
            need(osk_painted(rows, rects[a], OSK_KEY) >= w * h // 3 and sum(px != OSK_KEY for row in rows[y + 1:y + h - 1] for px in row[x + 1:x + w - 1]) >= 3, f"{model}: {a} has no legend")
            for b in names[i + 1:]:
                x2, y2, w2, h2 = rects[b]
                need(x + w <= x2 or x2 + w2 <= x or y + h <= y2 or y2 + h2 <= y, f"{model}: {a} and {b} overlap")
        # outside the keycaps the panel is black (a legend that spills over would show)
        covered = [bytearray(width) for _ in range(height)]
        for x, y, w, h in rects.values():
            for row in covered[y:y + h]:
                row[x:x + w] = b"\x01" * w
        top = min(r[1] for r in rects.values()) - 1
        bottom = max(r[1] + r[3] for r in rects.values())
        stray = [(x, y) for y in range(top, bottom) for x in range(width) if not covered[y][x] and rows[y][x] != (0, 0, 0)]
        need(not stray, f"{model}: {len(stray)} pixels outside the keycaps are not the panel's, the first at {stray[:1]}")
        ran += 1
    if not ran:
        raise Skipped(f"no ROMs in {main / 'roms'} (python tools/fetch_roms.py)")


# --- the release ------------------------------------------------------------------------------------------------------


def release_platform() -> str:
    import platform
    arm = platform.machine().lower() in ("arm64", "aarch64")
    return ("windows-" if os.name == "nt" else "linux-") + ("arm64" if arm else "x64")


def check_release() -> None:
    """A fresh extraction of this host's archive in dist/ starts the default machine, using no file of the repository: exactly the
    release's files (the program, its documents and the ROMs: no software for the machines, no manuals), the program's version,
    every ROM found, and the test screen."""
    sys.path.insert(0, str(ROOT / "tools"))
    import package
    name = release_platform()
    archive = ROOT / "dist" / "releases" / package.archive_name(name)
    need(archive.is_file(), f"{archive} is not there: build it first (tools/build_dist.ps1 or tools/build_linux.sh)")
    d = WORK / "release"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    members = package.read_back(name, archive)
    need(sorted(members) == package.expected_files(name),
         f"the archive's members are not the release's list: {sorted(set(members) ^ set(package.expected_files(name)))}")
    for member, data in members.items():
        (d / member).parent.mkdir(parents=True, exist_ok=True)
        (d / member).write_bytes(data)
    program = d / package.program_name(name)
    if os.name != "nt":
        program.chmod(0o755)

    def start(*args: str, stdin: str | None = None) -> str:
        run = subprocess.run([str(program), *args], cwd=d, input=stdin, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
        need(run.returncode == 0, f"{program.name} {' '.join(args)}: exit {run.returncode}: {run.stderr.strip() or run.stdout.strip()}")
        return run.stdout

    need(start("--version").strip() == f"ZetRo {package.version()}", "the extracted program is not this version")
    need("zetro.yaml" not in members and not any(m.startswith("assets/") for m in members), "the archive holds a configuration or assets")
    need("assets/" not in members["README.md"].decode("utf-8"), "the archive's README shows a picture the archive does not hold")
    listing = start("--list-roms")
    need("missing" not in listing, f"a ROM of the archive is not found by the program: {listing}")
    # a first start: the program writes its configuration (the test screen, the CRT look); every ROM is found
    commands = ["frames 100", "text", f"shot {d / 'demo.png'}", "quit"]
    out = start("--hidden", "--control", "stdin", "--screenshot", str(d / "shipped.png"), stdin="\n".join(commands) + "\n")
    answers = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
    need(len(answers) == len(commands) and all(a["ok"] for a in answers), f"the control channel answered {answers}")
    need("test screen" in answers[1]["text"] and "ROMs found: 0" not in answers[1]["text"], f"the default screen: {answers[1]['text']!r}")
    for shot in ("shipped.png", "demo.png"):
        need((d / shot).read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", f"{shot} is not a PNG")
    need(struct.unpack(">II", (d / "shipped.png").read_bytes()[16:24])[0] > 352, "the first start did not give the CRT look's picture")
    need("model: test" in (d / "zetro.yaml").read_text(encoding="utf-8"), "the first start did not write a configuration for the test screen")
    need(not any(m.startswith(("media/", "manuals/")) for m in members), "the archive holds software or manuals")
    for member in ("roms/hc91.rom",):
        need(hashlib.sha256((d / member).read_bytes()).hexdigest() == hashlib.sha256(members[member]).hexdigest(), f"a start changed {member}")
    print(f"        {archive.name}: {archive.stat().st_size:,} bytes, {len(members)} files, the test screen starts")


CHECKS = {name[6:]: fn for name, fn in sorted(globals().items()) if name.startswith("check_")}
# Not in the quick tier: it needs the archive that tools/build_dist.ps1 or tools/build_linux.sh makes.
NAMED_ONLY = {"release"}


def main() -> int:
    names = sys.argv[1:]
    if names == ["--list"]:
        print("\n".join(CHECKS))
        return 0
    unknown = [n for n in names if n not in CHECKS]
    if unknown:
        print(f"No such check: {', '.join(unknown)} (python tests/verify.py --list)")
        return 2
    if not EXE.is_file() and set(names) != {"release"}:
        print(f"{EXE} is not built: pwsh -File tools/build.ps1")
        return 2
    failed = 0
    for name in names or [n for n in CHECKS if n not in NAMED_ONLY]:
        try:
            CHECKS[name]()
            print(f"ok      {name}")
        except Skipped as e:
            print(f"skip    {name}: {e}")
        except (Failed, subprocess.TimeoutExpired, OSError, KeyError, ValueError) as e:
            failed += 1
            print(f"FAILED  {name}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
