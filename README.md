# Sfeerpakket voor Dusklight (0.3.0)

Een native mod die Twilight Princess richting de sfeer van moderne remakes duwt. Elke laag zet je apart aan of uit in het Mods-venster.

| Laag | Wat het doet | Waar in het frame |
|---|---|---|
| **Kleurstemming** | Filmische curve, witbalans, split toning (koele schaduwen, warme hoge lichten), verzadiging, vignet, dithering tegen banding | `FRAME_BEFORE_HUD`, dus de HUD blijft ongemoeid |
| **Atmosferische mist** | Hoogtemist (dik bij de grond, dunner naar boven), blauwige nevel in de verte en een warme gloed richting zon of maan | `SCENE_AFTER_OPAQUE` |
| **Lichtstralen** | God rays vanaf de zon door de gaten in het bladerdak en tussen de stammen (compute-pass op halve resolutie) | `SCENE_AFTER_OPAQUE` |
| **Vuurvliegjes en stof** | Knipperende vuurvliegjes (vooral 's nachts in Faron), zwevend stof en pollen die oplichten in tegenlicht | `SCENE_AFTER_OPAQUE`, met dieptetest: ze verdwijnen achter bomen |
| **Dichter gras en bloemen** | Extra graspollen en bloemen naast de originele, binnen de vaste pool van de game | Bij het laden van een kamer |
| **Licht per gebied** | Stuurt de ambient-, mist- en luchtkleuren van de game per gebied bij | Post-hook op `dScnKy_env_light_c::setLight()` |

Water, effecten en feeën worden ná de mist en de stralen getekend en blijven dus helder.

**Nieuw in 0.3, de zware onderdelen** (zie [ZWAAR.md](ZWAAR.md)):

| Script | Wat het doet |
|---|---|
| `hd-textures.sh` | Dumps → AI-opschaling (Real-ESRGAN) → mod met HD-textures. De dumpfunctie zet je aan in het Sfeerpakket-paneel |
| `modellen.sh` | Disc uitpakken, `.arc`-archieven openen, gewijzigde modellen als overlay-mod terugzetten |
| `bouw-eigen-dusklight.sh` | Bouwt jullie eigen Dusklight met belichting per pixel (`fork/`) |

## Gebiedsprofielen

| Profiel | Stages | Karakter |
|---|---|---|
| Faron Woods | `F_SP108`, Lost Woods `F_SP117`, grot `D_SB10` | Dichte warmgroene mist, sterke stralen, veel vuurvliegjes en stof |
| Ordon | `F_SP103`, `F_SP00`, `F_SP104` | Gouden, warm licht, lichte mist, wat stof |
| Hyrule Field | `F_SP121`, `F_SP122`, Castle Town `F_SP116` | Heldere blauwe verte, verzadigd, pollen |
| Buiten (standaard) | overige buitengebieden | Mild |
| Binnen | interieurs en kerkers | Nauwelijks mist, geen stralen |
| Schemerwereld | | Origineel licht, geen extra's |

Bij een gebiedswissel schuiven alle waarden geleidelijk naar het nieuwe profiel. De mist- en luchtkleur volgen de game, dus ze lopen mee met tijd van de dag en weer. Vuurvliegjes en stof passen zich aan dag en nacht aan.

## Installeren op de Mac (één commando)

Pak de zip uit, bijvoorbeeld in Downloads, en draai in Terminal:

```sh
bash ~/Downloads/sfeerpakket/installeer-op-mac.sh --start
```

Het script:

1. controleert de Xcode Command Line Tools en `cmake` (en installeert `cmake` via Homebrew als dat ontbreekt);
2. zoekt Dusklight.app en leest de versie;
3. haalt de Dusklight-broncode van **precies die versie** op, naar `~/Dusklight-mods`;
4. bouwt Sfeerpakket plus de demo-mods **Ambient Occlusion** en **Dynamic Shadows** (die zitten niet in de release);
5. kopieert alles naar `~/Library/Application Support/TwilitRealm/Dusklight/mods`;
6. start met `--start` meteen Dusklight met de preset.

De eerste keer duurt het een paar minuten (downloads). Daarna gaat het veel sneller.

## AO-, schaduw- en grafische preset (`start-sfeer.sh`)

| Instelling | Waarde |
|---|---|
| Ambient occlusion | aan, kwaliteit High, radius 90, intensiteit 75% |
| Dynamische schaduwen | aan, map 4096, sterkte 55%, zachte randen 5×5, bereik 5000, contactschaduwen aan |
| Bloom | Dusk, sterkte 1,25 |
| Depth of field | Dusk |
| Schaduwresolutie van de game | ×4 |

Deze waarden gelden alleen voor die sessie. Bevalt het, zet ze dan in de game vast.

## Testen: checklist

1. **Mods-venster → Sfeerpakket.** Bovenaan staan het actieve gebied en het aantal extra graspollen.
2. **Voor/na-vergelijking** aanzetten: links origineel, rechts met kleurstemming.
3. **Faron Woods overdag**, kijk richting de zon: lichtstralen tussen de bomen, mist bij de grond, stof in tegenlicht. Zet "Toon alleen stralen" of "Toon alleen mist" aan om die laag apart te zien.
4. **Faron Woods 's nachts**: vuurvliegjes.
5. **Hyrule Field**: blauwe verte, pollen, verzadigde kleuren.
6. **Gras**: loop een kamer in en uit. In het statusregel loopt het aantal extra pollen op. Maai wat gras: de extra pollen laten nooit items vallen.
7. Geen effect of foutmelding? Kijk in het logvenster van Dusklight naar regels met `nl.sfeerpakket.atmosphere`.

## Instellingen in de game

- **Kleurstemming**: aan/uit, sterkte, vignet, voor/na-vergelijking
- **Atmosferische mist**: aan/uit, dichtheid, zonnegloed, debugweergave
- **Lichtstralen**: aan/uit, sterkte, kwaliteit (0 = 24, 1 = 40, 2 = 64 stappen), debugweergave
- **Vuurvliegjes en stof**: aan/uit, hoeveelheid, helderheid
- **Gras en bloemen**: aan/uit, grasdichtheid (standaard 200%), bloemdichtheid (150%); werkt na een kamerwissel
- **Licht per gebied**: aan/uit, sterkte, overgangssnelheid
- **Gereedschap: HD-textures**: textures dumpen aan/uit, met de map en het aantal dumps

## Profielen afstellen

De waarden staan bovenaan `src/mod.cpp` (`kFaron`, `kOrdon`, `kField`, ...). Het zijn eerste schattingen op basis van de referentiebeelden. Werkwijze: waarde aanpassen, `installeer-op-mac.sh` opnieuw draaien, en in de game op **Reload** klikken.

Wereldeenheden zijn ongeveer centimeters: Link is zo'n 150 eenheden lang.

| Veld | Effect |
|---|---|
| `density`, `falloff`, `baseOffset` | Dikte van de mist, hoe snel die naar boven verdwijnt, en op welke hoogte (t.o.v. Link) hij het dikst is |
| `startDist`, `aerialDist`, `aerial` | Mistvrije zone, en vanaf welke afstand de mist de (getinte) luchtkleur aanneemt |
| `sunGlow` | Gloed in de mist richting de zon |
| `actorAmb`, `bgAmb`, `fogCol`, `sky` | Vermenigvuldigers op de kleuren van de game (1.0 = ongewijzigd) |
| `temperature`, `tint`, `saturation`, `contrast`, `filmic`, `shadowTint`, `highlightTint` | Kleurstemming |
| `rays` | Sterkte van de lichtstralen |
| `fireflyDay`, `fireflyNight`, `dust` | Aantal vuurvliegjes (dag/nacht) en stofdeeltjes |

Nieuw gebied toevoegen: kopieer een profiel, zet de stagenamen erin (lijst in `src/dusk/map_loader_definitions.h` van Dusklight) en voeg het toe aan `kNamedProfiles`.

## Hoe het werkt (technisch)

- **Kleur**: de scene wordt in 8 bits gerenderd. De filmische curve rekt de hoge lichten eerst op (inverse Reinhard, k = 0.8) en rolt ze dan af met ACES (Narkowicz, schaal 0.62). Middengrijs 0.18 blijft gelijk.
- **Mist**: een analytische integraal van de hoogtemist langs elke kijkstraal, uit de dieptebuffer; één fullscreen-pass. Lucht = alles voorbij 90% van het verre vlak.
- **Stralen**: screen-space radiale verstrooiing. Per pixel (halve resolutie) 40 stappen richting de zon, waarbij geteld wordt hoeveel daarvan lucht zijn; met interleaved-gradient-ruis tegen banding. Daarna wordt het additief samengevoegd, met afname vanaf de zon en een zachte knie. Het valt weg als de zon achter de camera of ver buiten beeld staat.
- **Deeltjes**: volledig procedureel in de vertex-shader. Er is geen deeltjesbuffer: elke instantie leidt uit zijn index een plek af in een blok dat met de camera meewikkelt.
- **Gras**: een pre/post-hook op `daGrass_create`. Daar zie ik welke pool-plekken nieuw bezet zijn en zet ik extra pollen binnen 38 eenheden ernaast. De game zet ze zelf op de grond. Er blijven altijd 250 grasplekken en 150 bloemplekken vrij voor originele pollen.
- **Licht**: een post-hook op `setLight()`, met een beveiliging tegen dubbel toepassen.

**Gecontroleerd:**

- de mod compileert tegen Dusklight `v2.0.3` (nieuwste release) en `main`;
- alle vijf WGSL-shaders zijn gevalideerd met naga en vertaald naar Metal, en de geheugenindeling van elke uniform-struct komt overeen met de C++-kant;
- het stralen-algoritme is in Python nagebootst op een testscene en geeft stralen tussen de stammen;
- het installatiescript is van begin tot eind gedraaid, met nagebootste macOS-commando's.

**Nog niet in de game getest.** Verwacht dat de profielwaarden bijgesteld moeten worden.

## Bekende beperkingen

- Lichtstralen zijn screen-space: ze bestaan alleen als de zon ongeveer in beeld is. Volumetrische stralen van opzij vragen de shadow map van `shadow_mod`.
- Extra gras kan aan de rand van een grasveld net op een pad of tegen een rand terechtkomen; dat zou je in de game moeten zien. Verlaag in dat geval de grasdichtheid.
- De mist wordt niet over transparante objecten gelegd. Die gebruiken alleen de eigen mist van de game.
