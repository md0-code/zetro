"""Puts the machines' ROMs in roms/ under their standard names.

    python tools/fetch_roms.py [--mister PATH | --archive FILE] [--esxdos-archive FILE] [--check] [--lock]

The ROMs are the ones the MiSTer cores archive (the MiSTerRO repository,
src/core-*/assets/roms; their sources and hashes are in its
docs/<core>/sources.lock.json). They are copied from a checkout of that
repository: --mister PATH, the environment variable ZETRO_MISTER, or the
folder MiSTerRO beside this repository. Every file is checked against the
SHA-256 that src/core/romdb.c knows it by, so a wrong file is never put in
place. --check only reports what roms/ holds.

Without that repository, --archive FILE takes the ROMs (and the esxDOS
firmware) from the roms/ folder of a ZetRo release archive (.zip or .tar.gz),
checked in the same way.

The firmware of the DivMMC and the DivIDE is esxDOS 0.8.9 (ESXMMC.BIN and
ESXIDE.BIN), downloaded from its own distribution on esxdos.org
(esxdos089.zip, pinned by the SHA-256 below; --esxdos-archive FILE or
ZETRO_ESXDOS names a copy of the archive to use instead of downloading it).
The two images go to roms/ as esxmmc.bin and esxide.bin, and the whole
distribution (its BIN, SYS and TMP folders, which a card needs) is unpacked
to work/esxdos/0.8.9/, where tools/make_card.py finds it.

--lock writes tools/sources.lock.json, the release's provenance manifest: every
ROM with its size, SHA-256, the place it has in the MiSTerRO repository and,
where that repository's own docs/<core>/sources.lock.json records one, the
address it was fetched from; what the cores built themselves says "derived".

The ROMs keep their owners' rights and are not part of this repository:
roms/ and work/ are git-ignored.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROMDB = ROOT / "src" / "core" / "romdb.c"

# Where each ROM of romdb.c is in the MiSTerRO repository, by its standard file name.
SOURCES = {
    "jet.rom": "src/core-jet/assets/roms/boot.rom",
    "hc85.rom": "src/core-hc/assets/roms/hc85.rom",
    "hc90.rom": "src/core-hc/assets/roms/hc90.rom",
    "hc91.rom": "src/core-hc/assets/roms/hc91.rom",
    "hc88.rom": "src/core-hc/assets/roms/hc88.rom",
    "hc128.rom": "src/core-hc/assets/roms/hc128.rom",
    "hc2000-0.rom": "src/core-hc/assets/roms/hc2000-0.rom",
    "hc2000-1.rom": "src/core-hc/assets/roms/hc2000-1.rom",
    "if85.rom": "src/core-hc/assets/roms/if85.rom",
    "if90.rom": "src/core-hc/assets/roms/if90.rom",
    "hc-family-cpm.rom": "src/core-hc/assets/roms/hc-family-cpm.rom",
    "cip01.rom": "src/core-cip/assets/roms/cip01.rom",
    "cip03.rom": "src/core-cip/assets/roms/cip03.rom",
    "cip04.rom": "src/core-cip/assets/roms/cip04.rom",
    "tims-standard.rom": "src/core-tims/assets/roms/tims-standard.rom",
    "plus-loader.rom": "src/core-tims/assets/roms/plus-loader.rom",
    "plus3-system.rom": "src/core-tims/assets/roms/plus3-system.rom",
    "cobra.rom": "src/core-cobra/assets/roms/boot.rom",
    "amic.rom": "src/core-amic/assets/roms/boot.rom",
    "prae-3.6.rom": "src/core-prae/assets/roms/prae-3.6-inverted.rom",
    "prae-3.5.rom": "src/core-prae/assets/roms/prae-3.5-inverted.rom",
    "cubz-v14.rom": "src/core-cubz/assets/roms/cubz-v14.rom",
    "cubz-v13.rom": "src/core-cubz/assets/roms/cubz-v13.rom",
}

# esxDOS, from its own distribution (esxdos.org); the archive is pinned by its SHA-256, the two firmware images by romdb.c's.
ESXDOS_VERSION = "0.8.9"
ESXDOS_URLS = ("https://www.esxdos.org/files/esxdos089.zip", "http://www.esxdos.org/files/esxdos089.zip")
ESXDOS_SHA256 = "d455888361cd13d455e362a4c841452b6f4568d30f9069443044affdd40de9a4"
ESXDOS_MEMBERS = {"esxmmc.bin": "ESXMMC.BIN", "esxide.bin": "ESXIDE.BIN"}     # file in roms/ -> file in the archive
ESXDOS_FOLDER = ROOT / "work" / "esxdos" / ESXDOS_VERSION                       # the distribution, for tools/make_card.py


LOCK = ROOT / "tools" / "sources.lock.json"


def known_roms() -> dict[str, tuple[int, str]]:
    """The table of romdb.c: standard file name -> (size, sha256)."""
    text = ROMDB.read_text(encoding="utf-8")
    rows = re.findall(r'\{"[^"]+",\s*"[^"]+",\s*(\d+)u,\s*"([0-9a-f]{64})",\s*"([^"]+)"\}', text)
    return {name: (int(size), sha) for size, sha, name in rows}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def esxdos_archive(given: str | None) -> bytes | None:
    """The distribution archive's bytes (a given file, or downloaded), checked against the pinned SHA-256; None with a message."""
    if given:
        data = Path(given).read_bytes()
    else:
        data = None
        for url in ESXDOS_URLS:
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "zetro-fetch-roms"})
                with urllib.request.urlopen(request, timeout=60) as reply:
                    data = reply.read()
                break
            except OSError as e:
                print(f"  esxDOS: {url}: {e}")
        if data is None:
            print("  esxDOS: cannot download the distribution: give a copy of esxdos089.zip with --esxdos-archive FILE")
            return None
    if hashlib.sha256(data).hexdigest() != ESXDOS_SHA256:
        print(f"  esxDOS: the archive is not esxDOS {ESXDOS_VERSION} (SHA-256 {hashlib.sha256(data).hexdigest()}, expected {ESXDOS_SHA256})")
        return None
    return data


def esxdos_unpack(data: bytes) -> bool:
    """The distribution into work/esxdos/<version>/ (a fresh folder; no member may leave it)."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        folder = ESXDOS_FOLDER.resolve()
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        for info in z.infolist():
            target = (folder / info.filename).resolve()
            if folder != target and folder not in target.parents:
                print(f"  esxDOS: the archive names {info.filename}, outside its folder")
                return False
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(z.read(info))
    return True


def esxdos_fetch(known: dict[str, tuple[int, str]], roms: Path, given: str | None, check: bool) -> int:
    """The two firmware images in roms/ and the distribution unpacked; the number of things still missing."""
    wanted = [n for n in ESXDOS_MEMBERS if not (roms / n).is_file() or sha256(roms / n) != known[n][1]]
    unpacked = (ESXDOS_FOLDER / "SYS" / "ESXDOS.SYS").is_file()
    for name in ESXDOS_MEMBERS:
        if name not in wanted:
            print(f"  {name}: present")
    if check:
        for name in wanted:
            print(f"  {name}: missing")
        if not unpacked:
            print(f"  esxDOS distribution: not unpacked in {ESXDOS_FOLDER}")
        return len(wanted) + (0 if unpacked else 1)
    if not wanted and unpacked:
        print(f"  esxDOS distribution: present in {ESXDOS_FOLDER}")
        return 0
    data = esxdos_archive(given or os.environ.get("ZETRO_ESXDOS"))
    if data is None or not esxdos_unpack(data):
        return len(wanted) + (0 if unpacked else 1)
    missing = 0
    for name, member in ESXDOS_MEMBERS.items():
        source = ESXDOS_FOLDER / member
        if not source.is_file() or sha256(source) != known[name][1] or source.stat().st_size != known[name][0]:
            print(f"  {name}: {member} of the archive is not the firmware romdb.c knows (its hash differs)")
            missing += 1
        elif name in wanted:
            roms.mkdir(exist_ok=True)
            (roms / name).write_bytes(source.read_bytes())
            print(f"  {name}: fetched from esxDOS {ESXDOS_VERSION}")
    print(f"  esxDOS distribution: unpacked in {ESXDOS_FOLDER}")
    return missing


# Where the MiSTer cores' own tools took what they built (docs/<core>/provenance.md of the MiSTerRO repository).
DERIVED = {
    "amic": "built by the aMIC core's tools from the archive https://www.z80-romania.ro/aMIC-95 (docs/amic/provenance.md)",
    "jet": "built by the JET core's tools from the archive https://www.z80-romania.ro/JET-256 (docs/jet/provenance.md)",
    "prae": "built by the PRAE core's tools from the archive https://www.z80-romania.ro/PRAE-4 (docs/prae/provenance.md)",
    "tims": "built by the TIM-S core's tools from the archives https://www.z80-romania.ro/TIM_S-68 and https://www.z80-romania.ro/TIM_S_Plus-97 "
            "(docs/tims/provenance.md)",
    "cobra": "built by the CoBra core's tools from the archive https://www.z80-romania.ro/CoBra-64 and the CoBra Project, "
             "https://cobrasov.com/CoBra%20Project/ (docs/cobra/provenance.md)",
}


def core_lock(mister: Path, source: str) -> tuple[dict, str]:
    """The sources.lock.json of the core a mister path belongs to, and the key the path has in it."""
    parts = source.removeprefix("src/").split("/")
    if parts[0].startswith("core-"):
        core, key = parts[0][5:], "/".join(parts[1:])
    else:
        core, key = parts[1], "docs/" + "/".join(parts[2:])
    lock = mister / "docs" / core / "sources.lock.json"
    return (json.loads(lock.read_text(encoding="utf-8")) if lock.is_file() else {}), key


def origin(mister: Path, source: str, digest: str) -> str | None:
    """The address the core's own lock records for a file (or, for a ROM cut from an archive, for the archive); None: derived."""
    lock, key = core_lock(mister, source)
    entry = lock.get(key)
    if entry is None:
        entry = next((e for e in lock.values() if isinstance(e, dict) and e.get("sha256") == digest and e.get("url")), None)
    if entry and entry.get("from"):
        entry = lock.get(entry["from"])
    return entry.get("url") if entry else None


def archive_roms(archive: Path) -> dict[str, bytes]:
    """The files of the roms/ folder of a release archive (.zip, or .tar.gz with its top folder), by name."""
    found: dict[str, bytes] = {}
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                parts = info.filename.split("/")
                if not info.is_dir() and len(parts) >= 2 and parts[-2] == "roms":
                    found[parts[-1]] = z.read(info)
    else:
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                parts = member.name.split("/")
                if member.isfile() and len(parts) >= 2 and parts[-2] == "roms":
                    found[parts[-1]] = t.extractfile(member).read()
    return found


def from_archive(known: dict[str, tuple[int, str]], roms: Path, archive: Path) -> int:
    """Every ROM of romdb.c from a release archive into roms/; the number still missing."""
    try:
        found = archive_roms(archive)
    except (OSError, zipfile.BadZipFile, tarfile.TarError) as e:
        print(f"Cannot read {archive}: {e}")
        return len(known)
    missing = 0
    for name, (size, sha) in known.items():
        target = roms / name
        if target.is_file() and sha256(target) == sha:
            print(f"  {name}: present")
        elif name not in found:
            print(f"  {name}: not in the archive")
            missing += 1
        elif len(found[name]) != size or hashlib.sha256(found[name]).hexdigest() != sha:
            print(f"  {name}: the archive's file is not the ROM romdb.c knows (its hash differs)")
            missing += 1
        else:
            target.write_bytes(found[name])
            print(f"  {name}: copied")
    return missing


def write_lock(mister: Path) -> int:
    known = known_roms()
    manifest: dict[str, dict] = {}
    for name in sorted(known):
        path, (size, digest) = f"roms/{name}", known[name]
        if name in ESXDOS_MEMBERS:
            manifest[path] = {"bytes": size, "sha256": digest, "source": f"esxDOS {ESXDOS_VERSION}, {ESXDOS_MEMBERS[name]}", "url": ESXDOS_URLS[0]}
            continue
        source = SOURCES[name]
        manifest[path] = {"bytes": size, "sha256": digest, "source": f"MiSTerRO/{source}", "url": origin(mister, source, digest)}
    for entry in manifest.values():
        if entry["url"] is None:
            core = entry["source"].split("/")[2].removeprefix("core-")
            del entry["url"]
            entry["derived"] = DERIVED.get(core, f"built by the {core} core's own tools, from the archives of its docs/{core}/sources.lock.json")
    with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(f"{LOCK.relative_to(ROOT)}: {len(manifest)} files")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mister", help="a checkout of the MiSTerRO repository")
    parser.add_argument("--archive", help="a ZetRo release archive to take the ROMs from, in place of that repository")
    parser.add_argument("--esxdos-archive", help="a copy of esxdos089.zip, in place of the download")
    parser.add_argument("--check", action="store_true", help="only report what roms/ holds")
    parser.add_argument("--lock", action="store_true", help="write tools/sources.lock.json")
    args = parser.parse_args()
    known = known_roms()
    if set(known) != set(SOURCES) | set(ESXDOS_MEMBERS):
        print("tools/fetch_roms.py and src/core/romdb.c name different ROMs:", sorted(set(known) ^ (set(SOURCES) | set(ESXDOS_MEMBERS))))
        return 2
    roms = ROOT / "roms"
    mister = Path(args.mister or os.environ.get("ZETRO_MISTER") or ROOT.parent / "MiSTerRO")
    if args.lock:
        return write_lock(mister)
    if args.archive and not args.check:
        roms.mkdir(exist_ok=True)
        missing = from_archive(known, roms, Path(args.archive))
        print(f"{len(known) - missing} of {len(known)} ROMs in {roms}")
        return 1 if missing else 0
    if not args.check and not mister.is_dir():
        print(f"The MiSTerRO repository is not at {mister}: give its place with --mister PATH, "
              "or a ZetRo release archive with --archive FILE")
        return 1
    roms.mkdir(exist_ok=True)
    missing = 0
    for name, (size, sha) in known.items():
        if name in ESXDOS_MEMBERS:
            continue
        target = roms / name
        if target.is_file() and sha256(target) == sha:
            print(f"  {name}: present")
            continue
        if args.check:
            print(f"  {name}: missing")
            missing += 1
            continue
        source = mister / SOURCES[name]
        if not source.is_file():
            print(f"  {name}: {source} is not there")
            missing += 1
        elif source.stat().st_size != size or sha256(source) != sha:
            print(f"  {name}: {source} is not the archived ROM (its hash differs)")
            missing += 1
        else:
            target.write_bytes(source.read_bytes())
            print(f"  {name}: copied")
    missing += esxdos_fetch(known, roms, args.esxdos_archive, args.check)
    print(f"{len(known) - missing} of {len(known)} ROMs in {roms}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
