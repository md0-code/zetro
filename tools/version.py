"""The release version, src/VERSION (MAJOR.MINOR.PATCH).

    python tools/version.py show              print it
    python tools/version.py bump patch        0.1.0 -> 0.1.1  (bugfixes, or fewer than 3 features)
    python tools/version.py bump minor        0.1.3 -> 0.2.0  (a work block with 3 or more features)
    python tools/version.py bump major        0.8.2 -> 1.0.0  (only on explicit request)
    python tools/version.py check             validate the file and this tool's arithmetic

CMake reads the file into the project version and zr_version.h; the
distribution archive is named after it.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

VERSION_FILE = Path(__file__).resolve().parents[1] / "src" / "VERSION"
PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse(text: str) -> tuple[int, int, int]:
    m = PATTERN.match(text.strip())
    if not m:
        raise ValueError(f"not MAJOR.MINOR.PATCH: {text.strip()!r}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def bump(version: str, part: str) -> str:
    major, minor, patch = parse(version)
    if part == "patch":
        patch += 1
    elif part == "minor":
        minor, patch = minor + 1, 0
    elif part == "major":
        major, minor, patch = major + 1, 0, 0
    else:
        raise ValueError(f"unknown part {part!r} (patch, minor or major)")
    return f"{major}.{minor}.{patch}"


def read() -> str:
    text = VERSION_FILE.read_text(encoding="ascii")
    parse(text)
    return text.strip()


def check() -> list[str]:
    errors = []
    try:
        read()
    except (OSError, ValueError) as e:
        errors.append(f"src/VERSION: {e}")
    cases = [("0.1.0", "patch", "0.1.1"), ("0.7.9", "patch", "0.7.10"), ("0.1.3", "minor", "0.2.0"),
             ("0.9.4", "minor", "0.10.0"), ("0.8.2", "major", "1.0.0")]
    for before, part, want in cases:
        got = bump(before, part)
        if got != want:
            errors.append(f"bump {part} {before}: {got}, expected {want}")
    for bad in ("0.7", "0.7.0.1", "v0.7.0", "0.7.x", ""):
        try:
            parse(bad)
            errors.append(f"{bad!r} accepted")
        except ValueError:
            pass
    return errors


def main(argv: list[str]) -> int:
    if len(argv) == 1 and argv[0] == "show":
        print(read())
        return 0
    if len(argv) == 2 and argv[0] == "bump":
        new = bump(read(), argv[1])
        VERSION_FILE.write_text(new + "\n", encoding="ascii", newline="\n")
        print(new)
        return 0
    if len(argv) == 1 and argv[0] == "check":
        errors = check()
        for e in errors:
            print(e)
        return 1 if errors else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
