#!/usr/bin/env python3
# Based on the extractor from NocturneRecomp (https://github.com/birabittoh/NocturneRecomp,
# MIT License, see LICENSE-NocturneRecomp.txt). Generalised to read the header
# size and file table position from the package, so DLC packages work too.
"""Extract an Xbox 360 XBLA package (game or DLC) from its LIVE/STFS container."""

from __future__ import annotations

import argparse
import math
from pathlib import Path


BLOCK_SIZE = 0x1000
FILE_TABLE_ENTRY_SIZE = 0x40
ENTRIES_PER_BLOCK = BLOCK_SIZE // FILE_TABLE_ENTRY_SIZE
ROOT_PARENT = 0xFFFF
END_OF_CHAIN = 0xFFFFFF
BLOCKS_PER_TABLE = 0xAA        # data blocks covered by one level-0 hash table
BLOCKS_PER_L1 = 0xAA * 0xAA    # data blocks covered by one level-1 hash table

CONTENT_TYPE_MARKETPLACE = 0x00000002
CONTENT_TYPE_ARCADE = 0x000D0000


class Entry:
    def __init__(
        self,
        index: int,
        name: str,
        flags: int,
        blocks: int,
        start_block: int,
        parent: int,
        size: int,
    ) -> None:
        self.index = index
        self.name = name
        self.flags = flags
        self.blocks = blocks
        self.start_block = start_block
        self.parent = parent
        self.size = size

    @property
    def is_directory(self) -> bool:
        return (self.flags & 0x80) != 0


class Package:
    """Read-only (LIVE/PIRS) STFS package: the layout of XBLA games and DLC.

    Positions come from the package header instead of fixed offsets, so games
    and DLC packages with other header sizes or longer file tables work too.
    Block addressing follows the STFS format as documented by the Free60 and
    Velocity projects (one hash table per level for read-only packages).
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        with path.open("rb") as f:
            header = f.read(0x1000)
        self.magic = header[:4]
        if self.magic not in (b"LIVE", b"PIRS"):
            raise RuntimeError(f"{path} is not a LIVE/PIRS (read-only STFS) package")
        self.header_size = int.from_bytes(header[0x340:0x344], "big")
        self.content_type = int.from_bytes(header[0x344:0x348], "big")
        self.title_id = int.from_bytes(header[0x360:0x364], "big")
        separation = header[0x37B]
        if not separation & 1:
            raise RuntimeError(f"{path}: writable (CON-style) STFS layout is not supported")
        self.file_table_blocks = int.from_bytes(header[0x37C:0x37E], "little")
        self.file_table_start = int.from_bytes(header[0x37E:0x381], "little")
        name = header[0x411:0x411 + 0x80].decode("utf-16-be", errors="replace")
        self.display_name = name.split("\x00", 1)[0].strip()
        # Block numbers below are counted from the first hash table.
        self.base_block = (self.header_size + BLOCK_SIZE - 1) // BLOCK_SIZE

    def physical_block(self, logical_block: int) -> int:
        """Physical block of a data block: skips the hash tables in between."""
        group = logical_block // BLOCKS_PER_TABLE
        level1_groups = logical_block // BLOCKS_PER_L1
        level1_overhead = level1_groups + 1 if level1_groups > 0 else 0
        return (self.base_block + logical_block + 1 + group + (1 if group > 0 else 0)
                + level1_overhead)

    def hash_entry_offset(self, logical_block: int) -> int:
        group = logical_block // BLOCKS_PER_TABLE
        index = logical_block % BLOCKS_PER_TABLE
        level1_groups = logical_block // BLOCKS_PER_L1
        level1_overhead = level1_groups + 1 if level1_groups > 0 else 0
        table_block = (self.base_block + group * (BLOCKS_PER_TABLE + 1)
                       + (1 if group > 0 else 0) + level1_overhead)
        return table_block * BLOCK_SIZE + index * 0x18

    def next_block(self, source, logical_block: int) -> int:
        source.seek(self.hash_entry_offset(logical_block) + 0x15)
        return read_uint24_be(source.read(3))

    def entries(self) -> list[Entry]:
        entries: list[Entry] = []
        with self.path.open("rb") as f:
            block = self.file_table_start
            for table_block in range(self.file_table_blocks):
                f.seek(self.physical_block(block) * BLOCK_SIZE)
                data = f.read(BLOCK_SIZE)
                for slot in range(ENTRIES_PER_BLOCK):
                    raw = data[slot * FILE_TABLE_ENTRY_SIZE:(slot + 1) * FILE_TABLE_ENTRY_SIZE]
                    name_flags = raw[0x28] if len(raw) == FILE_TABLE_ENTRY_SIZE else 0
                    name_len = name_flags & 0x3F
                    if name_len == 0:
                        return entries  # end of the table
                    entries.append(
                        Entry(
                            index=len(entries),
                            name=raw[:name_len].decode("ascii", errors="strict"),
                            flags=name_flags,
                            blocks=read_uint24_le(raw[0x29:0x2C]),
                            start_block=read_uint24_le(raw[0x2F:0x32]),
                            parent=int.from_bytes(raw[0x32:0x34], "big"),
                            size=int.from_bytes(raw[0x34:0x38], "big"),
                        )
                    )
                if table_block + 1 < self.file_table_blocks:
                    block = self.next_block(f, block)
        return entries


def read_uint24_le(data: bytes) -> int:
    return data[0] | (data[1] << 8) | (data[2] << 16)


def read_uint24_be(data: bytes) -> int:
    return (data[0] << 16) | (data[1] << 8) | data[2]


def entry_path(entry: Entry, entries: list[Entry]) -> Path:
    parts = [entry.name]
    parent = entry.parent
    seen = {entry.index}
    while parent != ROOT_PARENT:
        if parent >= len(entries) or parent in seen:
            raise RuntimeError(f"invalid parent chain for {entry.name}")
        parent_entry = entries[parent]
        parts.append(parent_entry.name)
        seen.add(parent)
        parent = parent_entry.parent
    return Path(*reversed(parts))


def extract_file(package: Package, entry: Entry, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    remaining = entry.size
    blocks_to_copy = max(entry.blocks, math.ceil(entry.size / BLOCK_SIZE))
    logical_block = entry.start_block

    with package.path.open("rb") as source, destination.open("wb") as output:
        for block_index in range(blocks_to_copy):
            if remaining <= 0:
                break
            if logical_block == END_OF_CHAIN:
                raise RuntimeError(f"unexpected end of block chain while extracting {entry.name}")

            source.seek(package.physical_block(logical_block) * BLOCK_SIZE)
            chunk = source.read(min(BLOCK_SIZE, remaining))
            if not chunk:
                raise RuntimeError(f"unexpected EOF while extracting {entry.name}")
            output.write(chunk)
            remaining -= len(chunk)

            if block_index + 1 < blocks_to_copy:
                logical_block = package.next_block(source, logical_block)


def extract(package_path: Path, out_dir: Path, require_xex: bool) -> Package:
    package = Package(package_path)
    entries = package.entries()
    extracted = 0
    default_xex = None
    for entry in entries:
        relative = entry_path(entry, entries)
        destination = out_dir / relative
        if entry.is_directory:
            destination.mkdir(parents=True, exist_ok=True)
            continue
        extract_file(package, entry, destination)
        extracted += 1
        if entry.name == "default.xex" and entry.parent == ROOT_PARENT:
            default_xex = destination
    if require_xex and default_xex is None:
        raise RuntimeError("extracted package did not contain root default.xex")
    print(f"Extracted {extracted} files to {out_dir}")
    return package


def find_default_package(repo_root: Path) -> Path:
    game_dir = repo_root / "game"
    if not game_dir.is_dir():
        raise RuntimeError("no game/ directory found; place the XBLA package there first")
    candidates = sorted(path for path in game_dir.iterdir() if path.is_file())
    if not candidates:
        raise RuntimeError("no package file found in game/")
    if len(candidates) > 1:
        raise RuntimeError("multiple files found in game/; pass the package path explicitly")
    return candidates[0]


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "package",
        nargs="?",
        type=Path,
        help="Path to the local XBLA LIVE/STFS package. Defaults to the only file in game/.",
    )
    parser.add_argument(
        "--assets",
        type=Path,
        default=repo_root / "assets",
        help="Directory that receives the package contents, including default.xex, for ReXGlue.",
    )
    parser.add_argument(
        "--content",
        action="store_true",
        help="Package is downloadable content (no default.xex expected).",
    )
    args = parser.parse_args()

    package = (args.package or find_default_package(repo_root)).resolve()
    extract(package, args.assets, require_xex=not args.content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
