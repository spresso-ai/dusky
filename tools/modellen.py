#!/usr/bin/env python3
"""Werkplaats voor modellen en andere game-bestanden, als overlay-mod voor Dusklight.

Mappen (onder ~/Dusklight-mods/modellen):
  origineel/<Arc>/archive/...   onaangeroerde kopie (niet aankomen)
  werk/<Arc>/archive/...        hier bewerk/vervang je bestanden (bv. een nieuwe al.bmd)

  python3 modellen.py uitpakken <disc-map> <Arc>     bv. Kmdl  (res/Object/Kmdl.arc)
  python3 modellen.py zoek <disc-map> <tekst>        zoekt bestandsnamen in alle .arc's
  python3 modellen.py inpakken <naam> [--installeer MODMAP]
        maakt <naam>.dusk met alléén de bestanden die in werk/ verschillen van origineel/,
        op het pad dat Dusklight verwacht: overlay/res/Object/<Arc>/archive/...
"""

from __future__ import annotations

import filecmp
import json
import shutil
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arc_tool  # noqa: E402

ROOT = Path.home() / "Dusklight-mods" / "modellen"


def find_object_dir(disc: Path) -> Path:
    for cand in (disc / "files" / "res" / "Object", disc / "res" / "Object", disc / "DATA" / "files" / "res" / "Object"):
        if cand.is_dir():
            return cand
    hits = [p for p in disc.rglob("Object") if p.parent.name == "res" and p.is_dir()]
    if not hits:
        sys.exit(f"Geen res/Object gevonden in {disc}. Is de disc uitgepakt?")
    return hits[0]


def cmd_uitpakken(disc: Path, arc_name: str) -> int:
    arc_path = find_object_dir(disc) / f"{arc_name}.arc"
    if not arc_path.exists():
        sys.exit(f"{arc_path} bestaat niet. Gebruik 'zoek' om het juiste archief te vinden.")
    files = arc_tool.read_rarc(arc_path.read_bytes())
    for sub in ("origineel", "werk"):
        dest_root = ROOT / sub / arc_name
        if sub == "werk" and dest_root.exists():
            print(f"werk/{arc_name} bestaat al; niet overschreven (je bewerkingen blijven staan)")
            continue
        for f in files:
            dest = dest_root / f.path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(f.data)
    print(f"{len(files)} bestanden uit {arc_path.name}:")
    for f in files:
        if f.path.lower().endswith((".bmd", ".bdl", ".bti", ".bck", ".btk", ".brk", ".bpk", ".bva")):
            print(f"  {f.path}")
    print(f"\nBewerk in: {ROOT / 'werk' / arc_name}")
    return 0


def cmd_zoek(disc: Path, needle: str) -> int:
    return arc_tool.main(["arc_tool", "zoek", str(find_object_dir(disc)), needle])


def cmd_inpakken(name: str, install: Path | None) -> int:
    work, orig = ROOT / "werk", ROOT / "origineel"
    changed: list[tuple[Path, str]] = []
    for f in sorted(work.rglob("*")):
        if not f.is_file() or f.name.startswith("."):
            continue
        rel = f.relative_to(work)                  # <Arc>/archive/...
        o = orig / rel
        if o.exists() and filecmp.cmp(f, o, shallow=False):
            continue
        overlay_path = "overlay/res/Object/" + rel.as_posix()
        changed.append((f, overlay_path))
    if not changed:
        print("Geen gewijzigde bestanden in werk/ gevonden.")
        return 1
    out = ROOT / f"{name}.dusk"
    meta = {"id": f"nl.sfeerpakket.{name.lower().replace('-', '_').replace(' ', '_')}",
            "name": name, "version": "1.0.0", "author": "Sfeerpakket",
            "description": f"Vervangt {len(changed)} bestand(en) via overlay. Alleen voor eigen gebruik."}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mod.json", json.dumps(meta, indent=2))
        for src, arcname in changed:
            z.write(src, arcname)
            print(f"  {arcname}")
    print(f"{out} ({len(changed)} bestanden)")
    if install:
        install.mkdir(parents=True, exist_ok=True)
        shutil.copy(out, install / out.name)
        print(f"Geïnstalleerd in {install}. In de game: Reload (Link en Midna verversen bij een gebiedswissel).")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    if argv[1] == "uitpakken" and len(argv) >= 4:
        return cmd_uitpakken(Path(argv[2]).expanduser(), argv[3])
    if argv[1] == "zoek" and len(argv) >= 4:
        return cmd_zoek(Path(argv[2]).expanduser(), argv[3])
    if argv[1] == "inpakken":
        install = None
        if "--installeer" in argv:
            install = Path(argv[argv.index("--installeer") + 1]).expanduser()
        return cmd_inpakken(argv[2], install)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
