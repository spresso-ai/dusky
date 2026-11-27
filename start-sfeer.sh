#!/bin/sh
# Start Dusklight met de Sfeerpakket-preset voor AO, schaduwen, bloom en depth of field.
#
# Gebruik:   ./start-sfeer.sh [extra Dusklight-argumenten, bv. --dvd ~/Games/tp.rvz]
# Eigen pad: DUSKLIGHT_APP=~/pad/naar/Dusklight.app ./start-sfeer.sh
#
# De --cvar-waarden gelden alleen voor deze sessie. Bevalt het? Zet ze dan in de game zelf vast
# (Mods-venster en Instellingen), dan worden ze in config.json bewaard.

APP="${DUSKLIGHT_APP:-}"
if [ -z "$APP" ]; then
    for kandidaat in "/Applications/Dusklight.app" "$HOME/Applications/Dusklight.app"; do
        [ -d "$kandidaat" ] && APP="$kandidaat" && break
    done
fi
if [ -z "$APP" ]; then
    APP="$(mdfind "kMDItemCFBundleIdentifier == 'dev.twilitrealm.dusk'" 2>/dev/null | head -1)"
fi
BIN="$APP/Contents/MacOS/Dusklight"
if [ ! -x "$BIN" ]; then
    echo "Dusklight niet gevonden op $APP. Zet DUSKLIGHT_APP naar het juiste pad." >&2
    exit 1
fi

AO="mod.dev_twilitrealm_ao__mod"
SH="mod.dev_twilitrealm_shadow__mod"

exec "$BIN" \
    --cvar "$AO.effectEnabled=true" \
    --cvar "$AO.quality=2" \
    --cvar "$AO.radius=120" \
    --cvar "$AO.intensity=100" \
    --cvar "$AO.halfRes=false" \
    --cvar "$SH.effectEnabled=true" \
    --cvar "$SH.mapSize=2" \
    --cvar "$SH.strength=70" \
    --cvar "$SH.pcf=2" \
    --cvar "$SH.bias=55" \
    --cvar "$SH.boxRadius=5000" \
    --cvar "$SH.contactShadows=true" \
    --cvar "game.bloomMode=2" \
    --cvar "game.bloomMultiplier=1.1" \
    --cvar "game.depthOfFieldMode=0" \
    --cvar "game.shadowResolutionMultiplier=4" \
    --cvar "game.enableMouseCamera=true" \
    --cvar "game.enableMouseAim=true" \
    "$@"
