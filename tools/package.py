"""Assembles a ZetRo release archive and checks it. tools/build_dist.ps1 (Windows) and tools/build_linux.sh (Linux)
call it after they have built the program; tests/verify.py's `release` check uses it too.

    python tools/package.py PLATFORM BINARY [--libretro] [--out DIR] [--run-folder DIR]
    python tools/package.py --list PLATFORM [--libretro]

PLATFORM is windows-x64, windows-arm64, linux-x64 or linux-arm64. BINARY is the desktop executable or, with
--libretro, the shared core. The archive uses ZetRo or ZetRo-Libretro before the version and platform.
The .zip (Windows) or .tar.gz (Linux, under a folder of the same name) goes to --out
(default dist/releases/), which holds the current version only: archives of other versions are removed. --run-folder also copies the files over
a folder that is used as a run folder (dist/ for windows-x64): its zetro.yaml and everything else in it stay.

No zetro.yaml is in the archive: the program writes the whole commented file on its first start. The pictures of assets/ are not either:
the READMEs go in without the lines that show them.

Both archives include the documents and the machines' ROMs. The Libretro archive ships the library and its info file.
Software and manuals for the machines are supplied by the user. The binaries must match the platform's format and
architecture, and every ROM must be the one whose SHA-256 is known (src/core/romdb.c through tools/fetch_roms.py),
tools/sources.lock.json must name each of them with that SHA-256, every relative link of the shipped documents must resolve
inside the archive, and the finished archive is opened again: its members must be exactly the list and their bytes the
staged ones. Anything else stops the build.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import struct
import sys
import tarfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_roms  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
NAME = "ZetRo"
PLATFORMS = ("windows-x64", "windows-arm64", "linux-x64", "linux-arm64")

# What the archive holds besides the program and the ROMs: archive path = path in the repository.
DOCS = ["README.md", "README.ro.md", "LICENSE"] + [f"docs/{n}" for n in ("THIRD_PARTY.md", "CHANGELOG.md", "NOTES.md", "NOTES.ro.md")]
ROMS = sorted(fetch_roms.known_roms())

# The lines of a README that show the pictures of assets/ (the logo, centred; the screenshot), which the archive does not hold.
PICTURE_LINE = re.compile(r"^(?:!\[[^\]]*\]\(assets/[^)]*\)|<p align=\"center\">.*\bassets/.*</p>)\r?\n\r?\n", re.M)

EXECUTABLES = {  # format, machine
    "windows-x64": ("pe", 0x8664), "windows-arm64": ("pe", 0xAA64),
    "linux-x64": ("elf", 0x3E), "linux-arm64": ("elf", 0xB7),
}


class PackageError(Exception):
    pass


def version() -> str:
    text = (ROOT / "src" / "VERSION").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", text):
        raise PackageError(f"src/VERSION must hold MAJOR.MINOR.PATCH, not {text!r}")
    return text


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def program_name(platform: str) -> str:
    return "zetro.exe" if platform.startswith("windows") else "zetro"


def core_name(platform: str) -> str:
    return "zetro_libretro.dll" if platform.startswith("windows") else "zetro_libretro.so"


CORE_INFO = "info/zetro_libretro.info"


def binary_member(platform: str, libretro: bool) -> str:
    return f"cores/{core_name(platform)}" if libretro else program_name(platform)


def archive_name(platform: str, libretro: bool = False) -> str:
    name = f"{NAME}-Libretro" if libretro else NAME
    return f"{name}-{version()}-{platform}" + (".zip" if platform.startswith("windows") else ".tar.gz")


def expected_files(platform: str, libretro: bool = False) -> list[str]:
    """The archive's members (without the Linux folder), sorted: the whole list of what a release holds."""
    files = [binary_member(platform, libretro), *DOCS, *(f"roms/{r}" for r in ROMS)]
    if libretro:
        files.append(CORE_INFO)
    return sorted(files)


def check_executable(platform: str, path: Path, shared: bool = False) -> None:
    kind, machine = EXECUTABLES[platform]
    with path.open("rb") as binary:
        head = binary.read(4096)
    if kind == "pe":
        offset = struct.unpack_from("<I", head, 0x3C)[0] if head[:2] == b"MZ" and len(head) > 0x40 else 0
        if (head[:2] != b"MZ" or offset + 24 > len(head) or head[offset:offset + 4] != b"PE\0\0" or
                struct.unpack_from("<H", head, offset + 4)[0] != machine or
                bool(struct.unpack_from("<H", head, offset + 22)[0] & 0x2000) != shared):
            raise PackageError(f"{path} is not a Windows {'DLL' if shared else 'executable'} for {platform}")
    else:
        elf_types = (3,) if shared else (2, 3)  # ET_DYN, including position-independent executables, and ET_EXEC
        if (len(head) < 64 or head[:6] != b"\x7fELF\x02\x01" or
                struct.unpack_from("<H", head, 18)[0] != machine or
                struct.unpack_from("<H", head, 16)[0] not in elf_types or
                (struct.unpack_from("<Q", head, 24)[0] == 0) != shared):
            raise PackageError(f"{path} is not a 64-bit little-endian ELF {'library' if shared else 'executable'} for {platform}")


def rom_images() -> dict[str, Path]:
    found = {}
    known = fetch_roms.known_roms()
    for name in ROMS:
        path = ROOT / "roms" / name
        if not path.is_file():
            raise PackageError(f"roms/{name} is missing: python tools/fetch_roms.py")
        data = path.read_bytes()
        if len(data) != known[name][0] or sha256(data) != known[name][1]:
            raise PackageError(f"roms/{name} is not the image ZetRo knows by that name (size, SHA-256)")
        found[name] = path
    return found


def check_manifest(entries: dict[str, bytes | Path]) -> None:
    """tools/sources.lock.json names every ROM of the release with the SHA-256 it has."""
    manifest = json.loads((ROOT / "tools" / "sources.lock.json").read_text(encoding="utf-8"))
    shipped = {name: content(item) for name, item in entries.items() if name.startswith("roms/")}
    if set(manifest) != set(shipped):
        raise PackageError(f"tools/sources.lock.json differs from the files shipped: {sorted(set(manifest) ^ set(shipped))} "
                           "(python tools/fetch_roms.py --lock)")
    for name, data in shipped.items():
        if manifest[name]["sha256"] != sha256(data) or manifest[name]["bytes"] != len(data):
            raise PackageError(f"tools/sources.lock.json has another size or SHA-256 for {name}")


def gather(platform: str, binary: Path, libretro: bool = False) -> dict[str, bytes | Path]:
    """Archive path -> the file or the bytes that go there; every input checked."""
    check_executable(platform, binary, shared=libretro)
    entries: dict[str, bytes | Path] = {binary_member(platform, libretro): binary}
    if libretro:
        info = ROOT / "src" / "libretro" / Path(CORE_INFO).name
        if not info.is_file():
            raise PackageError(f"{info} is missing")
        entries[CORE_INFO] = info
    for f in DOCS:
        if not (ROOT / f).is_file():
            raise PackageError(f"{f} is missing")
        entries[f] = ROOT / f
        if f.endswith(("README.md", "README.ro.md")):
            text = PICTURE_LINE.sub("", (ROOT / f).read_bytes().decode("utf-8"))
            if "](assets/" in text or '="assets/' in text:
                raise PackageError(f"{f} shows a picture of assets/ that is not on a line of its own")
            entries[f] = text.encode("utf-8")
    for name, path in rom_images().items():
        entries[f"roms/{name}"] = path
    check_manifest(entries)
    return entries


def content(item: bytes | Path) -> bytes:
    return item if isinstance(item, bytes) else item.read_bytes()


# --- the documents' links ------------------------------------------------------------------------------------------

LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def broken_links(files: dict[str, bytes | Path] | list[str], read=None) -> list[str]:
    """Relative links of the Markdown files in the list (read: archive path -> text) that point at nothing in it."""
    names = set(files)
    folders = {str(Path(n).parent).replace("\\", "/") for n in names}
    problems = []
    for name in sorted(n for n in names if n.endswith(".md")):
        text = read(name) if read else content(files[name]).decode("utf-8")  # type: ignore[index]
        base = Path(name).parent
        for target in LINK.findall(text):
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith("#"):
                continue
            path = (base / target.split("#")[0]).as_posix()
            path = re.sub(r"(^|/)\./", r"\1", path).rstrip("/")
            parts = []
            for p in path.split("/"):
                if p == "..":
                    if not parts:
                        break
                    parts.pop()
                elif p and p != ".":
                    parts.append(p)
            else:
                resolved = "/".join(parts)
                if resolved in names or resolved in folders:
                    continue
            problems.append(f"{name}: link to {target}")
    return problems


# --- the archive -----------------------------------------------------------------------------------------------------

def write_archive(platform: str, entries: dict[str, bytes | Path], target: Path, libretro: bool = False) -> None:
    now = time.time()
    if platform.startswith("windows"):
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(entries):
                info = zipfile.ZipInfo(name, time.localtime(now)[:6])
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                z.writestr(info, content(entries[name]))
    else:
        top = archive_name(platform, libretro).removesuffix(".tar.gz")
        with tarfile.open(target, "w:gz", compresslevel=9, format=tarfile.GNU_FORMAT) as t:
            folders = sorted({str(parent) for n in entries for parent in Path(n).parents})
            for folder in folders:
                info = tarfile.TarInfo(top if folder == "." else f"{top}/{Path(folder).as_posix()}")
                info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, int(now)
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                t.addfile(info)
            for name in sorted(entries):
                data = content(entries[name])
                info = tarfile.TarInfo(f"{top}/{name}")
                info.size, info.mtime = len(data), int(now)
                info.mode = 0o755 if name == program_name(platform) else 0o644
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                t.addfile(info, io.BytesIO(data))


def read_back(platform: str, archive: Path, libretro: bool = False) -> dict[str, bytes]:
    """The members of an archive by path (the Linux folder removed), files only."""
    found: dict[str, bytes] = {}
    if platform.startswith("windows"):
        with zipfile.ZipFile(archive) as z:
            if z.testzip():
                raise PackageError(f"{archive}: a member is corrupt")
            for info in z.infolist():
                if info.is_dir():
                    continue
                if info.filename in found or info.filename.startswith(("/", "../")) or "\\" in info.filename:
                    raise PackageError(f"{archive}: bad member name {info.filename!r}")
                found[info.filename] = z.read(info)
    else:
        top = archive_name(platform, libretro).removesuffix(".tar.gz") + "/"
        with tarfile.open(archive, "r:gz") as t:
            for info in t:
                if info.isdir():
                    continue
                if not info.isfile() or not info.name.startswith(top):
                    raise PackageError(f"{archive}: unexpected member {info.name!r}")
                name = info.name[len(top):]
                if name in found or ".." in name.split("/"):
                    raise PackageError(f"{archive}: bad member name {info.name!r}")
                found[name] = t.extractfile(info).read()  # type: ignore[union-attr]
    return found


def package(platform: str, binary: Path, out: Path, run_folder: Path | None = None, *, libretro: bool = False) -> Path:
    entries = gather(platform, binary, libretro)
    expected = expected_files(platform, libretro)
    problems = broken_links(entries)
    if problems:
        raise PackageError("links that lead nowhere in the archive:\n  " + "\n  ".join(problems))
    out.mkdir(parents=True, exist_ok=True)
    final = out / archive_name(platform, libretro)
    work = ROOT / "work" / "package"
    work.mkdir(parents=True, exist_ok=True)
    temp = work / final.name
    write_archive(platform, entries, temp, libretro)
    back = read_back(platform, temp, libretro)
    if sorted(back) != expected:
        raise PackageError(f"the archive holds {sorted(set(back) ^ set(expected))} beyond or short of the list")
    for name, item in entries.items():
        if back[name] != content(item):
            raise PackageError(f"the archive's {name} differs from the staged one")
    # the current version only: every archive of another version goes
    pattern = re.compile(rf"{NAME}(?:-Libretro)?-(\d+\.\d+\.\d+)-[a-z]+-[a-z0-9]+\.(zip|tar\.gz)")
    for old in out.glob(f"{NAME}-*"):
        match = pattern.fullmatch(old.name)
        if match and match.group(1) != version():
            old.unlink(missing_ok=True)
    shutil.move(str(temp), final)
    if run_folder is not None:
        for name in sorted(entries):
            target = run_folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content(entries[name]))
    print(final.name)
    for folder in ("", "cores/", "info/", "docs/", "roms/"):
        names = [n for n in expected if n.startswith(folder) and "/" not in n[len(folder):]]
        if not names:
            continue
        size = sum(len(content(entries[n])) for n in names)
        print(f"  {folder or './':<10} {len(names):>3} files {size:>14,} bytes")
    print(f"OK: {final} ({final.stat().st_size:,} bytes)")
    return final


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("binary", nargs="?", type=Path)
    parser.add_argument("--libretro", action="store_true", help="package the shared core")
    parser.add_argument("--out", type=Path, default=ROOT / "dist" / "releases")
    parser.add_argument("--run-folder", type=Path)
    parser.add_argument("--list", action="store_true", help="print the files of the archive and stop")
    args = parser.parse_args(argv)
    if args.list:
        print("\n".join(expected_files(args.platform, args.libretro)))
        return 0
    if not args.binary:
        parser.error("a built executable or library is required")
    try:
        package(args.platform, args.binary, args.out, args.run_folder, libretro=args.libretro)
    except (PackageError, OSError) as e:
        print(f"package: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
