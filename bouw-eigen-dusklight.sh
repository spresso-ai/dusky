#!/bin/bash
# Bouwt jullie eigen versie van Dusklight met de patches uit fork/ (nu: belichting per pixel).
#
#   bash bouw-eigen-dusklight.sh            bouwen -> ~/Applications/Dusklight Sfeer.app
#   bash bouw-eigen-dusklight.sh --start    en daarna starten met de Sfeerpakket-preset
#
# De gewone Dusklight.app blijft gewoon staan. Beide gebruiken dezelfde saves, instellingen en
# modmap. De eigen versie wordt gebouwd van dezelfde versie als je geïnstalleerde app, zodat je
# mods (gebouwd met installeer-op-mac.sh) erin blijven werken. De patches raken alleen de
# shadergenerator van de renderer, niet de game-ABI.
#
# Eerste keer: 15-40 minuten (alle afhankelijkheden worden gedownload en gebouwd).
#
# Instelbaar: DUSKLIGHT_REF (git-tag of commit), WERKMAP (standaard ~/Dusklight-mods).

set -euo pipefail
HIER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WERKMAP="${WERKMAP:-$HOME/Dusklight-mods}"
REPO="https://github.com/TwilitRealm/dusklight.git"
DOEL="$HOME/Applications/Dusklight Sfeer.app"
PRESET="macos-default-relwithdebinfo"

stap() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fout() { printf '\n\033[1;31mFout: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fout "dit script is voor macOS."

stap "Gereedschap controleren"
xcode-select -p >/dev/null 2>&1 || { xcode-select --install || true; fout "installeer eerst de Xcode Command Line Tools en start opnieuw."; }
for tool in cmake ninja python3; do
    if ! command -v "$tool" >/dev/null; then
        if [ "$tool" = python3 ]; then fout "python3 ontbreekt"; fi
        command -v brew >/dev/null || fout "$tool ontbreekt en Homebrew is niet geïnstalleerd (https://brew.sh)"
        echo "$tool installeren via Homebrew..."
        brew install "$tool"
    fi
done

stap "Versie bepalen"
REF="${DUSKLIGHT_REF:-}"
if [ -z "$REF" ]; then
    VERSIE=""
    for app in "/Applications/Dusklight.app" "$HOME/Applications/Dusklight.app"; do
        if [ -f "$app/Contents/Info.plist" ]; then
            VERSIE="$(defaults read "$app/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || true)"
            break
        fi
    done
    TAGS="$(git ls-remote --tags --refs "$REPO" | sed 's#.*refs/tags/##')"
    if [ -n "$VERSIE" ] && echo "$TAGS" | grep -qx "v$VERSIE"; then
        REF="v$VERSIE"
    else
        REF="v$(echo "$TAGS" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' \
            | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
    fi
fi
echo "Dusklight $REF"

stap "Broncode ophalen (met alle submodules)"
SRC="$WERKMAP/eigen/dusklight-$REF"
mkdir -p "$WERKMAP/eigen"
if [ ! -d "$SRC/.git" ]; then
    git clone --branch "$REF" --depth 1 "$REPO" "$SRC" 2>/dev/null || { git clone "$REPO" "$SRC"; git -C "$SRC" checkout "$REF"; }
fi
git -C "$SRC" -c advice.detachedHead=false submodule update --init --recursive --depth 1

stap "Patches toepassen"
for patch in "$HIER"/fork/aurora-*.patch; do
    [ -f "$patch" ] || continue
    naam="$(basename "$patch")"
    if git -C "$SRC/extern/aurora" apply --reverse --check "$patch" 2>/dev/null; then
        echo "  al toegepast: $naam"
    elif git -C "$SRC/extern/aurora" apply --check "$patch" 2>/dev/null; then
        git -C "$SRC/extern/aurora" apply "$patch"
        echo "  toegepast:    $naam"
    else
        fout "$naam past niet op deze Dusklight-versie ($REF)."
    fi
done

stap "Bouwen ($PRESET) - dit duurt even"
cd "$SRC"
cmake --preset "$PRESET" >"$WERKMAP/eigen/configure-$REF.log" 2>&1 \
    || { tail -40 "$WERKMAP/eigen/configure-$REF.log"; fout "configureren mislukt (log: $WERKMAP/eigen/configure-$REF.log)"; }
LOG="$WERKMAP/eigen/build-$REF.log"
set +e +o pipefail
# Voortgang: elke 200e regel van ninja ("[1234/5678] ...") tonen, alles naar het log.
cmake --build --preset "$PRESET" 2>&1 | tee "$LOG" | awk '/^\[[0-9]+\/[0-9]+\]/ && NR % 200 == 0 {print; fflush()}'
STATUS=${PIPESTATUS[0]}
set -e -o pipefail
[ "$STATUS" = 0 ] || { tail -40 "$LOG"; fout "bouwen mislukt (log: $LOG)"; }
APP="$SRC/build/$PRESET/Dusklight.app"
[ -d "$APP" ] || fout "Dusklight.app niet gevonden na het bouwen"

stap "Installeren als '$(basename "$DOEL")'"
mkdir -p "$(dirname "$DOEL")"
rm -rf "$DOEL"
ditto "$APP" "$DOEL"
echo "Klaar: $DOEL"
echo "Tip: vergelijk met de gewone Dusklight.app; de belichting van lantaarns, fakkels en"
echo "      de zon op Link en NPC's is per pixel nu veel gladder."

if [ "${1:-}" = "--start" ]; then
    DUSKLIGHT_APP="$DOEL" exec bash "$HIER/start-sfeer.sh"
fi
