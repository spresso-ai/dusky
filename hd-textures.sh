#!/bin/bash
# HD-textures maken uit je eigen texture-dumps en installeren als mod.
#
# Stap 1 (in de game): Mods-venster -> Sfeerpakket -> "Textures dumpen" aan. Loop rond door de
#         gebieden die je wilt opknappen (elk gebied laadt zijn textures). Zet het daarna uit.
# Stap 2 (Terminal):   bash hd-textures.sh
#
# Opties (omgevingsvariabelen):
#   MODEL=realesrgan-x4plus        natuurlijker (standaard)
#   MODEL=realesrgan-x4plus-anime  strakker, meer "getekend"
#   OPSCHALER=lanczos              geen AI, alleen scherp vergroten
#   DUMPS=/pad/naar/texture_dumps  als de map niet automatisch gevonden wordt
#   NORMAL_MAPS=1                  ook normal maps schatten (voor de eigen Dusklight-build)
#   INPAKKEN=1                     alleen opnieuw inpakken (na het weggooien van slechte textures)
#
# Let op: het resultaat is gemaakt uit Nintendo's textures; alleen voor eigen gebruik.

set -euo pipefail
HIER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WERKMAP="${WERKMAP:-$HOME/Dusklight-mods}"
TOOLS="$WERKMAP/tools"
MODMAP="$HOME/Library/Application Support/TwilitRealm/Dusklight/mods"
MODEL="${MODEL:-realesrgan-x4plus}"
OPSCHALER="${OPSCHALER:-auto}"
ESRGAN_URL="https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-macos.zip"

stap() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fout() { printf '\n\033[1;31mFout: %s\033[0m\n' "$*" >&2; exit 1; }

if [ "${INPAKKEN:-0}" = 1 ]; then
    python3 "$HIER/tools/hd_textures.py" --dumps . --werkmap "$WERKMAP/hd-textures" --alleen-inpakken --installeer "$MODMAP"
    exit $?
fi

stap "Python controleren"
command -v python3 >/dev/null || fout "python3 ontbreekt (installeer de Xcode Command Line Tools: xcode-select --install)"
if ! python3 -c "import PIL, numpy" 2>/dev/null; then
    echo "Pillow en numpy installeren (alleen voor jouw gebruiker)..."
    python3 -m pip install --user --quiet pillow numpy || fout "kon Pillow/numpy niet installeren"
fi

stap "Dumps zoeken"
if [ -z "${DUMPS:-}" ]; then
    DUMPS="$(find "$HOME/Library" -type d -name texture_dumps -path "*Dusklight*" 2>/dev/null | head -1 || true)"
fi
[ -n "$DUMPS" ] && [ -d "$DUMPS" ] || fout "geen texture_dumps-map gevonden. Zet eerst 'Textures dumpen' aan in de game (zie bovenaan dit script), of geef DUMPS=/pad op."
AANTAL="$(find "$DUMPS" -name 'tex1_*' | wc -l | tr -d ' ')"
echo "$DUMPS ($AANTAL bestanden)"
[ "$AANTAL" -gt 0 ] || fout "de dumpmap is leeg; loop eerst rond in de game met 'Textures dumpen' aan."

EXTRA_ARGS=()
[ "${NORMAL_MAPS:-0}" = 1 ] && EXTRA_ARGS+=(--normal-maps)
ESRGAN_ARGS=()
if [ "$OPSCHALER" != "lanczos" ]; then
    stap "Real-ESRGAN klaarzetten"
    BIN="$TOOLS/realesrgan/realesrgan-ncnn-vulkan"
    if [ ! -x "$BIN" ]; then
        mkdir -p "$TOOLS/realesrgan"
        echo "Downloaden ($ESRGAN_URL, ~50 MB)..."
        curl -fL --progress-bar -o "$TOOLS/realesrgan.zip" "$ESRGAN_URL" || fout "download mislukt"
        unzip -o -q "$TOOLS/realesrgan.zip" -d "$TOOLS/realesrgan"
        chmod +x "$BIN"
        # curl zet geen quarantaine, maar voor de zekerheid:
        xattr -dr com.apple.quarantine "$TOOLS/realesrgan" 2>/dev/null || true
    fi
    ESRGAN_ARGS=(--esrgan "$BIN" --model "$MODEL")
fi

stap "Opschalen en inpakken"
python3 "$HIER/tools/hd_textures.py" --dumps "$DUMPS" --werkmap "$WERKMAP/hd-textures" \
    --opschaler "$OPSCHALER" ${ESRGAN_ARGS[@]+"${ESRGAN_ARGS[@]}"} ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} \
    --installeer "$MODMAP"

stap "Klaar"
cat <<EOF
De mod 'HD-textures (eigen)' staat in je modmap. In de game: Mods-venster -> Reload (of herstart).
Bekijk het resultaat per texture:  open "$WERKMAP/hd-textures/vergelijking/index.html"
Niet tevreden over een texture? Verwijder die uit $WERKMAP/hd-textures/pakket/textures en draai
  INPAKKEN=1 bash hd-textures.sh
of probeer alles opnieuw met MODEL=realesrgan-x4plus-anime.
EOF
