#!/bin/bash
# Modellen vervangen in Dusklight: disc uitpakken, archief openen, bewerken, als mod terugzetten.
#
#   bash modellen.sh disc ~/Games/tp.rvz        disc eenmalig uitpakken (nodtool)
#   bash modellen.sh zoek al.bmd                 in welk archief zit een bestand?
#   bash modellen.sh uitpakken Kmdl              res/Object/Kmdl.arc openen in de werkplaats
#   bash modellen.sh blender                     Blender-importer (blemd, TP-editie) ophalen
#   bash modellen.sh inpakken MijnLink           gewijzigde bestanden -> MijnLink.dusk -> modmap
#
# Werkplaats: ~/Dusklight-mods/modellen/{origineel,werk}/<Arc>/archive/...
# Zie MODELLEN.md voor de complete werkwijze met Blender en SuperBMD.

set -euo pipefail
HIER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WERKMAP="${WERKMAP:-$HOME/Dusklight-mods}"
DISC="$WERKMAP/disc"
TOOLS="$WERKMAP/tools"
MODMAP="$HOME/Library/Application Support/TwilitRealm/Dusklight/mods"
PY="$HIER/tools/modellen.py"

fout() { printf '\033[1;31mFout: %s\033[0m\n' "$*" >&2; exit 1; }

nodtool_pad() {
    if command -v nodtool >/dev/null; then command -v nodtool; return; fi
    local bin="$TOOLS/nodtool"
    if [ ! -x "$bin" ]; then
        mkdir -p "$TOOLS"
        if [ "$(uname -m)" = "arm64" ]; then
            echo "nodtool downloaden (Apple Silicon)..." >&2
            curl -fL --progress-bar -o "$bin" \
                "https://github.com/encounter/nod/releases/latest/download/nodtool-macos-arm64" \
                || fout "download van nodtool mislukt"
            chmod +x "$bin"
        elif command -v cargo >/dev/null; then
            echo "nodtool bouwen met cargo (Intel-Mac)..." >&2
            cargo install --locked nodtool --root "$TOOLS/cargo" >&2
            ln -sf "$TOOLS/cargo/bin/nodtool" "$bin"
        else
            fout "geen nodtool voor Intel-Macs beschikbaar. Installeer Rust (https://rustup.rs) en probeer opnieuw."
        fi
    fi
    echo "$bin"
}

case "${1:-}" in
    disc)
        [ -n "${2:-}" ] && [ -f "$2" ] || fout "geef het pad naar je disc-image (.iso/.rvz/...)"
        NOD="$(nodtool_pad)"
        mkdir -p "$DISC"
        "$NOD" extract "$2" "$DISC"
        echo "Disc uitgepakt in $DISC"
        ;;
    zoek)
        [ -n "${2:-}" ] || fout "waar zoek je naar? bv. al.bmd"
        python3 "$PY" zoek "$DISC" "$2"
        ;;
    uitpakken)
        [ -n "${2:-}" ] || fout "welk archief? bv. Kmdl (voor res/Object/Kmdl.arc)"
        python3 "$PY" uitpakken "$DISC" "$2"
        ;;
    blender)
        mkdir -p "$TOOLS"
        if [ -d "$TOOLS/blemd/.git" ]; then
            git -C "$TOOLS/blemd" pull --ff-only
        else
            git clone --depth 1 https://github.com/F1mmel/blemd.git "$TOOLS/blemd"
        fi
        (cd "$TOOLS" && rm -f blemd.zip && zip -qr blemd.zip blemd -x 'blemd/.git/*')
        cat <<EOF
Blender-importer klaar: $TOOLS/blemd.zip
In Blender: Edit > Preferences > Add-ons > Install from Disk... > kies blemd.zip > zet aan.
Daarna: File > Import > Nintendo BMD/BDL.
EOF
        ;;
    inpakken)
        [ -n "${2:-}" ] || fout "geef een naam voor de mod, bv. MijnLink"
        python3 "$PY" inpakken "$2" --installeer "$MODMAP"
        ;;
    *)
        sed -n '2,12p' "$0"
        ;;
esac
