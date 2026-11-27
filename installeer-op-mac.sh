#!/bin/bash
# Sfeerpakket: bouwen en installeren op de Mac, in één keer.
#
#   bash installeer-op-mac.sh            # bouwen + installeren
#   bash installeer-op-mac.sh --start    # en daarna Dusklight starten met de preset
#
# Wat het doet:
#  1. Zoekt Dusklight.app en leest de versie (bv. 2.0.3).
#  2. Haalt de Dusklight-broncode van precies die versie op (nodig: mods moeten tegen dezelfde
#     versie gebouwd worden als de app die je speelt).
#  3. Bouwt drie mods: Sfeerpakket, en de demo-mods Ambient Occlusion en Dynamic Shadows
#     (die zitten niet in de release van Dusklight).
#  4. Kopieert de .dusk-bestanden naar je modmap.
#
# Instelbaar via omgevingsvariabelen:
#   DUSKLIGHT_APP   pad naar Dusklight.app        (standaard: automatisch zoeken)
#   DUSKLIGHT_REF   git-tag of commit om tegen te bouwen (standaard: v<appversie>)
#   WERKMAP         waar broncode en builds komen  (standaard: ~/Dusklight-mods)

set -euo pipefail

HIER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WERKMAP="${WERKMAP:-$HOME/Dusklight-mods}"
MODMAP="$HOME/Library/Application Support/TwilitRealm/Dusklight/mods"
REPO="https://github.com/TwilitRealm/dusklight.git"
START=0
[ "${1:-}" = "--start" ] && START=1

stap() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fout() { printf '\n\033[1;31mFout: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fout "dit script is voor macOS."
[ -f "$HIER/CMakeLists.txt" ] && [ -f "$HIER/src/mod.cpp" ] || fout "start het script vanuit de map sfeerpakket."

# --- 1. Gereedschap -------------------------------------------------------------------------
stap "Gereedschap controleren"
if ! xcode-select -p >/dev/null 2>&1; then
    echo "De Xcode Command Line Tools ontbreken. Er verschijnt zo een installatievenster;"
    echo "rond dat af en start dit script daarna opnieuw."
    xcode-select --install || true
    exit 1
fi
command -v git >/dev/null || fout "git ontbreekt (hoort bij de Xcode Command Line Tools)."
if ! command -v cmake >/dev/null; then
    if command -v brew >/dev/null; then
        echo "cmake ontbreekt, installeren via Homebrew..."
        brew install cmake
    else
        fout "cmake ontbreekt. Installeer Homebrew (https://brew.sh) en daarna: brew install cmake"
    fi
fi
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
echo "clang: $(clang --version | head -1)"
echo "cmake: $(cmake --version | head -1)"

# --- 2. Dusklight-app en versie ---------------------------------------------------------------
stap "Dusklight zoeken"
APP="${DUSKLIGHT_APP:-}"
if [ -z "$APP" ]; then
    for kandidaat in "/Applications/Dusklight.app" "$HOME/Applications/Dusklight.app"; do
        [ -d "$kandidaat" ] && APP="$kandidaat" && break
    done
fi
if [ -z "$APP" ]; then
    APP="$(mdfind "kMDItemCFBundleIdentifier == 'dev.twilitrealm.dusk'" 2>/dev/null | head -1 || true)"
fi
VERSIE=""
if [ -n "$APP" ] && [ -f "$APP/Contents/Info.plist" ]; then
    VERSIE="$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || true)"
    echo "Gevonden: $APP (versie ${VERSIE:-onbekend})"
else
    echo "Dusklight.app niet gevonden; ik bouw tegen de nieuwste release."
fi

REF="${DUSKLIGHT_REF:-}"
if [ -z "$REF" ]; then
    TAGS="$(git ls-remote --tags --refs "$REPO" | sed 's#.*refs/tags/##')"
    if [ -n "$VERSIE" ] && echo "$TAGS" | grep -qx "v$VERSIE"; then
        REF="v$VERSIE"
    else
        # Nieuwste vX.Y.Z (zonder sort -V, dat ontbreekt in oudere macOS-versies)
        REF="v$(echo "$TAGS" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' \
            | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
        [ -n "$VERSIE" ] && echo "Let op: geen tag v$VERSIE gevonden; ik gebruik $REF."
    fi
fi
echo "Bouwen tegen Dusklight $REF"

# --- 3. Broncode ophalen ------------------------------------------------------------------------
stap "Dusklight-broncode ophalen ($REF)"
mkdir -p "$WERKMAP"
SRC="$WERKMAP/dusklight-$REF"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$REF" "$REPO" "$SRC" 2>/dev/null || {
        # REF is een commit in plaats van een tag
        git clone "$REPO" "$SRC"
        git -C "$SRC" checkout "$REF"
    }
fi
git -C "$SRC" -c advice.detachedHead=false submodule update --init --depth 1 extern/aurora

# --- 4. Bouwen ---------------------------------------------------------------------------------
bouw() {  # bouw <naam> <bronmap>
    local naam="$1" bron="$2" uit="$WERKMAP/build-$1-$REF"
    stap "Bouwen: $naam"
    cmake -S "$bron" -B "$uit" ${GEN[@]+"${GEN[@]}"} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDUSK_DIR="$SRC" \
        >"$uit.log" 2>&1 || { tail -30 "$uit.log"; fout "configureren van $naam mislukt (log: $uit.log)"; }
    cmake --build "$uit" >>"$uit.log" 2>&1 || { tail -40 "$uit.log"; fout "bouwen van $naam mislukt (log: $uit.log)"; }
    ls "$uit"/mods/*.dusk >/dev/null 2>&1 || fout "geen .dusk gevonden voor $naam"
    echo "OK: $(ls "$uit"/mods/*.dusk)"
}
mkdir -p "$WERKMAP"
bouw sfeerpakket "$HIER"
bouw ao_mod "$SRC/mods/ao_mod"
bouw shadow_mod "$SRC/mods/shadow_mod"

# --- 5. Installeren ----------------------------------------------------------------------------
stap "Installeren in $MODMAP"
mkdir -p "$MODMAP"
for naam in sfeerpakket ao_mod shadow_mod; do
    cp -f "$WERKMAP/build-$naam-$REF"/mods/*.dusk "$MODMAP/"
done
for f in "$MODMAP"/sfeerpakket.dusk "$MODMAP"/ao_mod.dusk "$MODMAP"/shadow_mod.dusk; do
    [ -f "$f" ] && echo "  $(basename "$f")  ($(du -h "$f" | cut -f1))"
done

stap "Klaar"
cat <<EOF
Geïnstalleerd: Sfeerpakket, Ambient Occlusion en Dynamic Shadows (gebouwd tegen Dusklight $REF).

Testen:
  1. Start Dusklight met de preset:   bash "$HIER/start-sfeer.sh"
  2. Open het Mods-venster en kies Sfeerpakket voor alle instellingen.
  3. Zet "Voor/na-vergelijking" aan om origineel en resultaat naast elkaar te zien.
  4. Ga naar Faron Woods, Ordon en Hyrule Field; het actieve gebied staat bovenaan het paneel.
  5. Extra gras verschijnt pas na een kamer- of gebiedswissel.

Na een aanpassing aan de code: draai dit script opnieuw en klik in de game op Reload.
EOF

if [ "$START" = 1 ]; then
    [ -n "$APP" ] && export DUSKLIGHT_APP="$APP"
    exec bash "$HIER/start-sfeer.sh"
fi
