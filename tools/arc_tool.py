#!/usr/bin/env python3
"""RARC-archieven (.arc) van Twilight Princess uitpakken en bekijken, met Yaz0-ondersteuning.

  python3 arc_tool.py lijst    res/Object/Kmdl.arc
  python3 arc_tool.py uitpakken res/Object/Kmdl.arc  uitvoermap/
  python3 arc_tool.py zoek     disc/files/res/Object  al.bmd      (zoekt in alle .arc's)

Bij uitpakken blijft de mapstructuur binnen het archief behouden, inclusief de naam van de
hoofdmap (bv. "archive/bmwr/al.bmd"). Dat is precies het pad dat Dusklight onder overlay/
verwacht: overlay/res/Object/Kmdl/archive/bmwr/al.bmd vervangt dat ene bestand in Kmdl.arc.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path


# ----------------------------------------------------------------------------------------------
# Yaz0
# ----------------------------------------------------------------------------------------------

def yaz0_decompress(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        return data
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(size)
    src, dst = 16, 0
    while dst < size:
        code = data[src]
        src += 1
        for bit in range(7, -1, -1):
            if dst >= size:
                break
            if code & (1 << bit):
                out[dst] = data[src]
                src += 1
                dst += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                start = dst - dist
                if start < 0:
                    raise ValueError("ongeldige Yaz0-terugverwijzing")
                for k in range(n):  # byte voor byte: overlappende kopieën zijn toegestaan
                    out[dst] = out[start + k]
                    dst += 1
    return bytes(out)


# ----------------------------------------------------------------------------------------------
# RARC
# ----------------------------------------------------------------------------------------------

@dataclass
class ArcFile:
    path: str      # pad binnen het archief, inclusief hoofdmap
    data: bytes


def read_rarc(raw: bytes) -> list[ArcFile]:
    data = yaz0_decompress(raw)
    if data[:4] != b"RARC":
        raise ValueError("geen RARC-archief")
    _size, header_len, data_off, _data_len = struct.unpack_from(">IIII", data, 4)
    info = header_len  # info-blok volgt direct op de header (0x20)
    num_nodes, nodes_off, num_entries, entries_off, _str_len, str_off = struct.unpack_from(
        ">IIIIII", data, info)
    nodes_off += info
    entries_off += info
    str_off += info
    file_data = header_len + data_off

    def name_at(off: int) -> str:
        end = data.index(b"\0", str_off + off)
        return data[str_off + off:end].decode("shift_jis", errors="replace")

    nodes = []
    for i in range(num_nodes):
        _type, name_off, _hash, count, first = struct.unpack_from(">4sIHHI", data, nodes_off + i * 0x10)
        nodes.append((name_at(name_off), count, first))

    files: list[ArcFile] = []

    def walk(node_index: int, prefix: str, seen: set[int]) -> None:
        if node_index in seen:
            return
        seen.add(node_index)
        name, count, first = nodes[node_index]
        base = f"{prefix}{name}/"
        for e in range(first, first + count):
            off = entries_off + e * 0x14
            _id, _hash, type_name, d_off, d_size = struct.unpack_from(">HHIII", data, off)
            flags = type_name >> 24
            ename = name_at(type_name & 0x00FFFFFF)
            if ename in (".", ".."):
                continue
            if flags & 0x02:  # map: d_off is de index van de knoop
                walk(d_off, base, seen)
            else:
                start = file_data + d_off
                content = data[start:start + d_size]
                files.append(ArcFile(base + ename, yaz0_decompress(content) if content[:4] == b"Yaz0" else content))

    if nodes:
        walk(0, "", set())
    return files


# ----------------------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("lijst", "uitpakken", "zoek"):
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "zoek":
        root, needle = Path(argv[2]), argv[3].lower() if len(argv) > 3 else ""
        for arc in sorted(root.rglob("*.arc")):
            try:
                for f in read_rarc(arc.read_bytes()):
                    if needle in f.path.lower():
                        print(f"{arc}  ->  {f.path}  ({len(f.data)} bytes)")
            except Exception as e:  # noqa: BLE001
                print(f"  (overgeslagen {arc}: {e})", file=sys.stderr)
        return 0
    arc = Path(argv[2])
    files = read_rarc(arc.read_bytes())
    if cmd == "lijst":
        for f in files:
            print(f"{len(f.data):>9}  {f.path}")
        print(f"{len(files)} bestanden")
        return 0
    out = Path(argv[3]) if len(argv) > 3 else arc.with_suffix("")
    for f in files:
        dest = out / f.path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(f.data)
    print(f"{len(files)} bestanden uitgepakt naar {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
