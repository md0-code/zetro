"""Makes a card image for the DivMMC or the DivIDE with esxDOS on it: a raw image with a partition table and one FAT16 partition
holding the SYS, BIN and TMP folders of the esxDOS distribution (fetched by tools/fetch_roms.py into work/esxdos/0.8.9/).

    python tools/make_card.py card.img [--size MIB] [--files FOLDER] [--add FILE[:CARDPATH]]...

--size   the image's size in MiB (default 64; 16 to 2048)
--files  the folder holding SYS, BIN and TMP (default work/esxdos/0.8.9)
--add    a file of the host put on the card (CARDPATH such as GAMES/TEST.TAP, default its name in the root)

The formatter is this file's own (no external tool): FAT16 with 8.3 names, which is what esxDOS reads and writes. The image is
sparse where the host allows it: only the parts written take room. read_card() reads such an image back (the checks use it).
Without a distribution folder the card is blank but for what --add puts on it (esxDOS will not start from it).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISTRIBUTION = ROOT / "work" / "esxdos" / "0.8.9"
SECTOR = 512
RESERVED = 1                        # the boot sector
ROOT_ENTRIES = 512
PARTITION_START = 2048              # 1 MiB in, where the tools of the day put the first partition
DATE = (2026 - 1980) << 9 | 11 << 5 | 13      # a fixed date, 13 November 2026, so that an image is the same every time
TIME = 12 << 11


def pick_cluster_size(partition_sectors: int) -> tuple[int, int, int]:
    """(sectors per cluster, clusters, sectors of one FAT) of a FAT16 volume of this size: the smallest cluster that keeps the
    count below 65000 (FAT16 holds 4085 to 65524)."""
    spc = 1
    while True:
        fat = 1
        for _ in range(8):
            clusters = (partition_sectors - RESERVED - ROOT_ENTRIES * 32 // SECTOR - 2 * fat) // spc
            fat = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if clusters <= 65000 or spc >= 64:
            return spc, clusters, fat
        spc *= 2


def short_name(name: str) -> bytes:
    """An 8.3 directory name, upper case, in 11 bytes."""
    base, dot, ext = name.upper().rpartition(".")
    if not dot:
        base, ext = ext, ""
    if not base or len(base) > 8 or len(ext) > 3 or any(c in ' "*+,/:;<=>?[\\]|' for c in base + ext):
        raise ValueError(f"{name!r} is not an 8.3 name")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def entry(name: bytes, attribute: int, cluster: int, size: int) -> bytes:
    return struct.pack("<11sBBBHHHHHHHI", name, attribute, 0, 0, TIME, DATE, DATE, 0, TIME, DATE, cluster, size)


class Volume:
    """A FAT16 volume being built: files and folders placed in order, then written out."""

    def __init__(self, size: int) -> None:
        self.total = size // SECTOR
        self.part = self.total - PARTITION_START
        self.spc, self.clusters, self.fat_sectors = pick_cluster_size(self.part)
        self.cluster_bytes = self.spc * SECTOR
        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xFFF8, 0xFFFF
        self.next = 2
        self.data: dict[int, bytes] = {}          # first cluster -> the bytes of its chain (padded to clusters)
        self.root = bytearray()

    def allocate(self, content: bytes) -> int:
        """A chain of clusters holding content; its first cluster (0 for nothing)."""
        count = (len(content) + self.cluster_bytes - 1) // self.cluster_bytes
        if not count:
            return 0
        if self.next + count > self.clusters + 2:
            raise ValueError("the files do not fit on the card")
        first = self.next
        for i in range(count):
            self.fat[first + i] = first + i + 1 if i + 1 < count else 0xFFFF
        self.next += count
        self.data[first] = content.ljust(count * self.cluster_bytes, b"\0")
        return first

    def reserve(self, entries: int) -> int:
        """The clusters of a folder of this many entries ("." and ".." too), taken now so that its files can name it as their parent;
        its first cluster. fill() puts the entries in."""
        count = (((entries + 2) * 32) + self.cluster_bytes - 1) // self.cluster_bytes
        first = self.next
        for i in range(count):
            self.fat[first + i] = first + i + 1 if i + 1 < count else 0xFFFF
        self.next += count
        self.data[first] = b"\0" * (count * self.cluster_bytes)
        return first

    def fill(self, first: int, entries: list[tuple[str, int, int, int]], parent: int) -> None:
        body = bytearray(entry(b".          ", 0x10, first, 0) + entry(b"..         ", 0x10, parent, 0))
        for name, attribute, cluster, size in entries:
            body += entry(short_name(name), attribute, cluster, size)
        self.data[first] = bytes(body).ljust(len(self.data[first]), b"\0")

    def write(self, f) -> None:
        mbr = bytearray(SECTOR)
        end = self.total - 1
        chs_end = bytes([0xFE, 0xFF, 0xFF]) if end > 1023 * 255 * 63 else bytes([0xFE, 0xFF, 0xFF])
        mbr[446:462] = bytes([0x00, 0x00, 0x21, 0x00, 0x06]) + chs_end + struct.pack("<II", PARTITION_START, self.part)
        mbr[510:512] = b"\x55\xAA"
        f.seek(0)
        f.write(mbr)
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x3C\x90"
        boot[3:11] = b"ZETRO   "
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, self.spc, RESERVED, 2, ROOT_ENTRIES, 0, 0xF8, self.fat_sectors, 63, 255,
                         PARTITION_START, self.part)
        struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, 0x5A455452, b"ZETRO      ", b"FAT16   ")
        boot[510:512] = b"\x55\xAA"
        f.seek(PARTITION_START * SECTOR)
        f.write(boot)
        fat = b"".join(struct.pack("<H", v) for v in self.fat).ljust(self.fat_sectors * SECTOR, b"\0")
        for copy in range(2):
            f.seek((PARTITION_START + RESERVED + copy * self.fat_sectors) * SECTOR)
            f.write(fat)
        root_at = PARTITION_START + RESERVED + 2 * self.fat_sectors
        f.seek(root_at * SECTOR)
        f.write(bytes(self.root).ljust(ROOT_ENTRIES * 32, b"\0"))
        data_at = root_at + ROOT_ENTRIES * 32 // SECTOR
        for first, content in self.data.items():
            f.seek((data_at + (first - 2) * self.spc) * SECTOR)
            f.write(content)


def build_card(path: Path, size_mib: int, files: dict[str, bytes]) -> None:
    """A card image of size_mib MiB with the files (card path such as "SYS/CONFIG/ESXDOS.CFG" -> bytes; a path ending in "/" is a
    folder, which may be empty). 8.3 names."""
    if not 16 <= size_mib <= 2048:
        raise ValueError("the size is 16 to 2048 MiB")
    tree: dict = {}
    for card_path, content in files.items():
        node = tree
        parts = card_path.strip("/").split("/")
        for part in parts[:-1] if not card_path.endswith("/") else parts:
            node = node.setdefault(part, {})
        if not card_path.endswith("/"):
            node[parts[-1]] = content
    v = Volume(size_mib << 20)

    def emit(node: dict, parent: int) -> list[tuple[str, int, int, int]]:
        """The entries of a folder whose own cluster is `parent`'s child: files allocated, folders reserved first."""
        out: list[tuple[str, int, int, int]] = []
        for name, item in sorted(node.items()):
            if isinstance(item, dict):
                first = v.reserve(len(item))
                v.fill(first, emit(item, first), parent)
                out.append((name, 0x10, first, 0))
            else:
                out.append((name, 0x20, v.allocate(item), len(item)))
        return out

    root = bytearray()
    for name, attribute, cluster, size in emit(tree, 0):
        root += entry(short_name(name), attribute, cluster, size)
    if len(root) > ROOT_ENTRIES * 32:
        raise ValueError("too many files in the root")
    v.root = root
    with open(path, "wb") as f:
        f.truncate(size_mib << 20)
        v.write(f)


# --- reading a card back ------------------------------------------------------------------------------------------------


def read_card(path: Path) -> dict[str, bytes]:
    """The files of a card image's first FAT16 partition, by their paths in upper case ("SYS/ESXDOS.SYS"); long-name entries are
    skipped (esxDOS writes 8.3 names). A folder is a key ending in "/"."""
    with open(path, "rb") as f:
        mbr = f.read(SECTOR)
        if mbr[510:512] != b"\x55\xAA":
            raise ValueError("no partition table")
        start = struct.unpack_from("<I", mbr, 446 + 8)[0]
        f.seek(start * SECTOR)
        boot = f.read(SECTOR)
        bps, spc, reserved, fats, root_entries, total16, _, fat_size = struct.unpack_from("<HBHBHHBH", boot, 11)
        if bps != SECTOR or boot[54:59] != b"FAT16":
            raise ValueError("not a FAT16 partition")
        f.seek((start + reserved) * SECTOR)
        fat = struct.unpack(f"<{fat_size * SECTOR // 2}H", f.read(fat_size * SECTOR))
        root_at = start + reserved + fats * fat_size
        data_at = root_at + root_entries * 32 // SECTOR

        def chain(first: int) -> bytes:
            out, cluster = bytearray(), first
            while 2 <= cluster < 0xFFF8:
                f.seek((data_at + (cluster - 2) * spc) * SECTOR)
                out += f.read(spc * SECTOR)
                cluster = fat[cluster]
            return bytes(out)

        def walk(raw: bytes, prefix: str, out: dict[str, bytes]) -> None:
            for at in range(0, len(raw), 32):
                e = raw[at:at + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08 or e[0] == 0x2E:
                    continue
                name = e[:8].decode("latin1").rstrip() + ("." + e[8:11].decode("latin1").rstrip() if e[8:11].strip() else "")
                cluster, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
                if e[11] & 0x10:
                    out[prefix + name + "/"] = b""
                    walk(chain(cluster), prefix + name + "/", out)
                else:
                    out[prefix + name] = chain(cluster)[:size] if size else b""

        f.seek(root_at * SECTOR)
        files: dict[str, bytes] = {}
        walk(f.read(root_entries * 32), "", files)
        return files


def distribution_files(folder: Path) -> dict[str, bytes]:
    """The SYS, BIN and TMP folders of the esxDOS distribution as card paths."""
    files: dict[str, bytes] = {}
    for top in ("SYS", "BIN", "TMP"):
        base = folder / top
        if not base.is_dir():
            continue
        files[top + "/"] = b""
        for path in sorted(base.rglob("*")):
            relative = path.relative_to(folder).as_posix().upper()
            if path.is_dir():
                files[relative + "/"] = b""
            else:
                files[relative] = path.read_bytes()
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", help="the image to write (an existing file is replaced)")
    parser.add_argument("--size", type=int, default=64, help="the image's size in MiB (default 64)")
    parser.add_argument("--files", default=str(DISTRIBUTION), help="the folder with SYS, BIN and TMP (default work/esxdos/0.8.9)")
    parser.add_argument("--add", action="append", default=[], metavar="FILE[:CARDPATH]", help="a host file to put on the card")
    args = parser.parse_args()
    folder = Path(args.files)
    files = distribution_files(folder)
    if not files:
        print(f"No esxDOS distribution in {folder}: python tools/fetch_roms.py fetches it. The card is made without esxDOS.")
    for item in args.add:
        host, _, card_path = item.partition(":")
        files[(card_path or Path(host).name).upper()] = Path(host).read_bytes()
    try:
        build_card(Path(args.image), args.size, files)
    except (ValueError, OSError) as e:
        print(f"Cannot make the card: {e}")
        return 1
    print(f"{args.image}: {args.size} MiB, {sum(1 for k in files if not k.endswith('/'))} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
