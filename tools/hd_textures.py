#!/usr/bin/env python3
"""HD-texturepijplijn voor Dusklight: dumps -> opschalen -> .dusk-mod.

Leest de texture-dumps van Dusklight (RGBA8-DDS met Dolphin-namen, bv.
tex1_256x128_e6a4c7be9bf48305_14.dds), schaalt ze op en verpakt het resultaat als
asset-mod (textures/*.png + mipmaps), klaar voor de modmap.

Opschalen:
  - esrgan : Real-ESRGAN (ncnn/Vulkan; op de Mac via Metal). Beste kwaliteit.
  - lanczos: ingebouwde terugval (Lanczos + verscherping). Snel, geen extra software.

Details die het verschil maken bij game-textures:
  - Tegelbare textures: rondom "wrap"-opvulling voor het opschalen, zodat er geen naden
    ontstaan waar de texture zich herhaalt.
  - Transparantie: kleur onder volledig transparante pixels wordt eerst opgevuld met de
    kleur van de buren (anders krijg je donkere randjes rond bladeren en gras); alpha wordt
    apart opgeschaald, en een oorspronkelijk harde alpha (alleen 0/255, typisch voor
    bladeren en hekwerk) blijft hard.
  - Mipmaps: _mipN-bestanden ernaast, zodat verre textures niet flikkeren.

Gebruik:
  python3 hd_textures.py --dumps DIR --werkmap DIR [--opschaler auto|esrgan|lanczos]
                         [--esrgan PAD_NAAR_BINARY] [--model realesrgan-x4plus]
                         [--installeer MODMAP]
"""

from __future__ import annotations

import argparse
import html
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

try:
    import numpy as np
    from PIL import Image, ImageFilter
except ImportError:  # pragma: no cover
    sys.exit("Pillow en numpy zijn nodig:  python3 -m pip install --user pillow numpy")

PAD = 8          # wrap-opvulling in bronpixels
MIN_SIZE = 8     # kleinere textures overslaan
MAX_OUT = 2048   # maximale uitvoergrootte per zijde


# ----------------------------------------------------------------------------------------------
# Inlezen
# ----------------------------------------------------------------------------------------------

def read_dds_rgba8(path: Path) -> Image.Image:
    """Leest een ongecomprimeerde 32-bit DDS (zoals Aurora die dumpt)."""
    data = path.read_bytes()
    if data[:4] != b"DDS ":
        raise ValueError("geen DDS")
    height, width = struct.unpack_from("<II", data, 12)
    # DDS_PIXELFORMAT begint op offset 76: size, flags, fourCC, bitcount, R/G/B/A-maskers
    _pf_flags, fourcc, bits, rmask, gmask, bmask, amask = struct.unpack_from("<I4sIIIII", data, 80)
    offset = 128
    if fourcc == b"DX10":
        offset += 20
    if bits != 32:
        raise ValueError(f"niet-ondersteund DDS-formaat ({bits} bits, fourcc {fourcc!r})")
    px = np.frombuffer(data, dtype=np.uint8, count=width * height * 4, offset=offset)
    px = px.reshape(height, width, 4)
    # Kanaalvolgorde uit de maskers afleiden (Aurora: R=0xFF, G=0xFF00, B=0xFF0000, A=0xFF000000)
    order = []
    for mask in (rmask, gmask, bmask, amask):
        order.append({0xFF: 0, 0xFF00: 1, 0xFF0000: 2, 0xFF000000: 3}.get(mask, len(order)))
    px = px[..., order]
    return Image.fromarray(np.ascontiguousarray(px), "RGBA")


def read_texture(path: Path) -> Image.Image:
    if path.suffix.lower() == ".dds":
        return read_dds_rgba8(path)
    return Image.open(path).convert("RGBA")


# ----------------------------------------------------------------------------------------------
# Voorbewerking
# ----------------------------------------------------------------------------------------------

def bleed_transparent(img: Image.Image, iterations: int = 16) -> Image.Image:
    """Vult de kleur van volledig transparante pixels met het gemiddelde van dekkende buren."""
    a = np.asarray(img).astype(np.float32)
    rgb, alpha = a[..., :3], a[..., 3]
    known = alpha > 0
    if known.all() or not known.any():
        return img
    rgb = rgb * known[..., None]
    weight = known.astype(np.float32)
    for _ in range(iterations):
        if (weight > 0).all():
            break
        s = np.zeros_like(rgb)
        w = np.zeros_like(weight)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            s += np.roll(rgb, (dy, dx), (0, 1))
            w += np.roll(weight, (dy, dx), (0, 1))
        fill = (weight == 0) & (w > 0)
        rgb[fill] = s[fill] / w[fill][:, None]
        weight = np.where(fill, 1.0, weight)
    out = np.dstack([np.clip(rgb, 0, 255), alpha]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def wrap_pad(img: Image.Image, pad: int) -> Image.Image:
    a = np.asarray(img)
    return Image.fromarray(np.pad(a, ((pad, pad), (pad, pad), (0, 0)), mode="wrap"), img.mode)


def is_flat(img: Image.Image) -> bool:
    a = np.asarray(img).astype(np.float32).reshape(-1, 4)
    return float(a.std(axis=0).max()) < 1.0  # per kanaal: geen variatie = niets op te schalen


def hard_alpha(img: Image.Image) -> bool:
    al = np.asarray(img)[..., 3]
    return bool(np.isin(al, (0, 255)).all()) and al.min() == 0


def scale_for(w: int, h: int) -> int:
    m = max(w, h)
    for s in (4, 2):
        if m * s <= MAX_OUT and m <= 256 * (4 // s):
            return s
    return 1


# ----------------------------------------------------------------------------------------------
# Opschalen
# ----------------------------------------------------------------------------------------------

def upscale_lanczos(img: Image.Image, scale: int) -> Image.Image:
    up = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    return up.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def upscale_esrgan_dir(in_dir: Path, out_dir: Path, binary: Path, model: str, scale: int) -> None:
    """Draait Real-ESRGAN op een hele map invoer-PNG's (veel sneller dan per bestand)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(binary), "-i", str(in_dir), "-o", str(out_dir), "-n", model, "-s", str(scale), "-f", "png"]
    models_dir = binary.parent / "models"
    if models_dir.is_dir():
        cmd += ["-m", str(models_dir)]
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)


# ----------------------------------------------------------------------------------------------
# Hoofdprogramma
# ----------------------------------------------------------------------------------------------

def make_normal_map(img: Image.Image, strength: float = 2.0) -> Image.Image:
    """Schat een tangent-space normal map uit de helderheid (Sobel, tegelbaar).

    Bedoeld voor de toekomstige normal-map-ondersteuning in de eigen Dusklight-build; de
    gewone Dusklight gebruikt deze bestanden niet."""
    a = np.asarray(img.convert("L")).astype(np.float32) / 255.0
    def sh(dy, dx):
        return np.roll(a, (dy, dx), (0, 1))
    gx = (sh(-1, -1) + 2 * sh(0, -1) + sh(1, -1)) - (sh(-1, 1) + 2 * sh(0, 1) + sh(1, 1))
    gy = (sh(-1, -1) + 2 * sh(-1, 0) + sh(-1, 1)) - (sh(1, -1) + 2 * sh(1, 0) + sh(1, 1))
    n = np.dstack([gx * strength, gy * strength, np.ones_like(a)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8), "RGB")


def make_mips(img: Image.Image, base: Path) -> int:
    level, w, h = 1, img.width, img.height
    count = 0
    while w > 1 or h > 1:
        w, h = max(w // 2, 1), max(h // 2, 1)
        mip = img.resize((w, h), Image.LANCZOS if min(w, h) >= 4 else Image.BOX)
        mip.save(base.with_name(f"{base.stem}_mip{level}.png"))
        level += 1
        count += 1
    return count


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dumps", required=True, type=Path)
    ap.add_argument("--werkmap", required=True, type=Path)
    ap.add_argument("--opschaler", choices=("auto", "esrgan", "lanczos"), default="auto")
    ap.add_argument("--esrgan", type=Path, help="pad naar realesrgan-ncnn-vulkan")
    ap.add_argument("--model", default="realesrgan-x4plus",
                    help="realesrgan-x4plus (natuurlijk) of realesrgan-x4plus-anime (strakker, getekend)")
    ap.add_argument("--naam", default="hd_textures")
    ap.add_argument("--installeer", type=Path, help="modmap om de .dusk in te zetten")
    ap.add_argument("--geen-mips", action="store_true")
    ap.add_argument("--normal-maps", action="store_true",
                    help="ook normal maps schatten (naar werkmap/normalmaps, voor de eigen build)")
    ap.add_argument("--alleen-inpakken", action="store_true",
                    help="niets opschalen; alleen werkmap/pakket opnieuw tot .dusk inpakken")
    args = ap.parse_args()

    if args.alleen_inpakken:
        return pack(args.werkmap / "pakket", args.werkmap / f"{args.naam}.dusk", args.installeer)

    sources = sorted([p for p in args.dumps.rglob("tex1_*") if p.suffix.lower() in (".dds", ".png")
                      and "_mip" not in p.stem])
    if not sources:
        print(f"Geen dumps gevonden in {args.dumps}. Zet 'Textures dumpen' aan in Sfeerpakket en loop rond.")
        return 1

    use_esrgan = args.opschaler == "esrgan" or (args.opschaler == "auto" and args.esrgan and args.esrgan.exists())
    if args.opschaler == "esrgan" and not (args.esrgan and args.esrgan.exists()):
        sys.exit("Real-ESRGAN niet gevonden; geef --esrgan of kies --opschaler lanczos")
    print(f"{len(sources)} textures, opschaler: {'Real-ESRGAN (' + args.model + ')' if use_esrgan else 'Lanczos'}")

    work = args.werkmap
    stage_in = {s: work / f"in_x{s}" for s in (2, 4)}
    stage_out = {s: work / f"out_x{s}" for s in (2, 4)}
    pack_root = work / "pakket"
    tex_out = pack_root / "textures"
    for d in list(stage_in.values()) + list(stage_out.values()) + [pack_root]:
        shutil.rmtree(d, ignore_errors=True)
    tex_out.mkdir(parents=True)

    meta = {}
    skipped = {"te klein": 0, "egale kleur": 0, "al groot": 0, "onleesbaar": 0}
    for src in sources:
        try:
            img = read_texture(src)
        except Exception as e:  # noqa: BLE001
            skipped["onleesbaar"] += 1
            print(f"  overgeslagen {src.name}: {e}")
            continue
        if min(img.size) < MIN_SIZE:
            skipped["te klein"] += 1
            continue
        if is_flat(img):
            skipped["egale kleur"] += 1
            continue
        s = scale_for(*img.size)
        if s == 1:
            skipped["al groot"] += 1
            continue
        name = src.stem + ".png"
        prepared = wrap_pad(bleed_transparent(img), PAD)
        stage_in[s].mkdir(parents=True, exist_ok=True)
        prepared.convert("RGB").save(stage_in[s] / name)
        meta[name] = {"src": src, "size": img.size, "scale": s, "hard_alpha": hard_alpha(img),
                      "has_alpha": bool(np.asarray(img)[..., 3].min() < 255), "padded": prepared}

    # Kleur opschalen
    for s in (2, 4):
        names = [n for n, m in meta.items() if m["scale"] == s]
        if not names:
            continue
        stage_out[s].mkdir(parents=True, exist_ok=True)
        if use_esrgan:
            # Real-ESRGAN-modellen zijn x4; voor x2 schalen we x4 en daarna terug.
            upscale_esrgan_dir(stage_in[s], stage_out[s], args.esrgan, args.model, 4)
            if s == 2:
                for n in names:
                    p = stage_out[s] / n
                    im = Image.open(p)
                    im.resize((im.width // 2, im.height // 2), Image.LANCZOS).save(p)
        else:
            for n in names:
                upscale_lanczos(Image.open(stage_in[s] / n), s).save(stage_out[s] / n)

    # Alpha erbij, opvulling eraf, mips, wegschrijven
    total_mips = 0
    report_rows = []
    for name, m in meta.items():
        s = m["scale"]
        up_path = stage_out[s] / name
        if not up_path.exists():
            print(f"  ontbreekt na opschalen: {name}")
            continue
        rgb = Image.open(up_path).convert("RGB")
        pw, ph = m["padded"].size
        if rgb.size != (pw * s, ph * s):
            rgb = rgb.resize((pw * s, ph * s), Image.LANCZOS)
        if m["has_alpha"]:
            alpha = m["padded"].getchannel("A").resize(rgb.size, Image.LANCZOS)
            if m["hard_alpha"]:
                alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
        else:
            alpha = Image.new("L", rgb.size, 255)
        out = Image.merge("RGBA", (*rgb.split(), alpha))
        w, h = m["size"]
        out = out.crop((PAD * s, PAD * s, PAD * s + w * s, PAD * s + h * s))
        dest = tex_out / name
        out.save(dest, optimize=True)
        if args.normal_maps:
            nm_dir = work / "normalmaps"
            nm_dir.mkdir(exist_ok=True)
            make_normal_map(out).save(nm_dir / name.replace(".png", "_n.png"))
        if not args.geen_mips:
            total_mips += make_mips(out, dest)
        report_rows.append((m["src"], name, w, h, s))

    # Vergelijkingspagina
    previews = work / "vergelijking"
    shutil.rmtree(previews, ignore_errors=True)
    previews.mkdir()
    rows = []
    for src, name, w, h, s in report_rows[:400]:
        before = previews / f"voor_{name}"
        read_texture(src).resize((w * s, h * s), Image.NEAREST).save(before)
        shutil.copy(tex_out / name, previews / f"na_{name}")
        rows.append(f"<tr><td>{html.escape(name)}<br>{w}x{h} → {w*s}x{h*s}</td>"
                    f"<td><img src='voor_{html.escape(name)}'></td><td><img src='na_{html.escape(name)}'></td></tr>")
    (previews / "index.html").write_text(
        "<!doctype html><meta charset=utf-8><title>HD-textures: voor en na</title>"
        "<style>body{font-family:system-ui;background:#222;color:#ddd}img{max-width:420px;"
        "image-rendering:auto;background:repeating-conic-gradient(#555 0 25%,#777 0 50%) 0/16px 16px}"
        "td{padding:6px;vertical-align:top}</style><h1>Voor (origineel, vergroot) en na</h1><table>"
        + "".join(rows) + "</table>", encoding="utf-8")

    # Verpakken als .dusk
    (pack_root / "mod.json").write_text(json.dumps({
        "id": f"nl.sfeerpakket.{args.naam}",
        "name": "HD-textures (eigen)",
        "version": "1.0.0",
        "author": "Sfeerpakket",
        "description": f"{len(report_rows)} opgeschaalde textures, gemaakt uit je eigen texture-dumps. "
                       "Alleen voor eigen gebruik: niet verspreiden.",
    }, indent=2), encoding="utf-8")
    print(f"\n{len(report_rows)} textures (+{total_mips} mipmaps) opgeschaald")
    print("Overgeslagen: " + ", ".join(f"{k}: {v}" for k, v in skipped.items()))
    print(f"Vergelijking: {previews / 'index.html'}")
    return pack(pack_root, work / f"{args.naam}.dusk", args.installeer)


def pack(pack_root: Path, dusk: Path, install_dir: Path | None) -> int:
    if not (pack_root / "mod.json").exists():
        print(f"Geen pakket gevonden in {pack_root}")
        return 1
    # Losse mipmaps zonder basis-texture (na handmatig verwijderen) opruimen.
    tex = pack_root / "textures"
    for mip in list(tex.glob("*_mip*.png")):
        if not (tex / (mip.stem.split("_mip")[0] + ".png")).exists():
            mip.unlink()
    with zipfile.ZipFile(dusk, "w", zipfile.ZIP_STORED) as z:  # PNG is al gecomprimeerd
        for p in sorted(pack_root.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(pack_root).as_posix())
    count = len([p for p in tex.glob("*.png") if "_mip" not in p.stem])
    print(f"Ingepakt: {count} textures, {dusk.stat().st_size / 1e6:.1f} MB -> {dusk}")
    if install_dir:
        install_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy(dusk, install_dir / dusk.name)
        print(f"Geïnstalleerd in {install_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
