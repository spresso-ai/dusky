# De zware onderdelen: textures, modellen en belichting

Drie sporen, van "draait vandaag" naar "eigen versie van Dusklight".

| Spoor | Wat je krijgt | Wat je nodig hebt | Status |
|---|---|---|---|
| **1. HD-textures** | Scherpe, opgeschaalde textures voor alles waar je langs bent gelopen | Alleen je Mac; Real-ESRGAN wordt automatisch gedownload | Klaar om te gebruiken |
| **2. Modellen** | Eigen of verbeterde modellen (Link, NPC's, objecten) | Blender, plus SuperBMD (een Windows-programma) | Gereedschap klaar; het modelleerwerk is handwerk |
| **3. Eigen Dusklight** | Belichting per pixel in plaats van per hoekpunt; later normal maps | Xcode-tools, 15-40 min bouwtijd | Stap 1 klaar, stap 2 ontworpen |

---

## Spoor 1: HD-textures

### Werkwijze

1. Start Dusklight. Kies Mods → **Sfeerpakket** → **Gereedschap: HD-textures** → zet **Textures dumpen** aan.
2. Loop rond door alle gebieden die je wilt opknappen. Elke texture die op het scherm komt, wordt één keer weggeschreven; het paneel telt mee. Gebieden waar je al was voor je het aanzette: ga er nog eens heen.
3. Zet het weer uit. Het gaat ook vanzelf uit als je de mod uitzet of de game afsluit.
4. Draai in Terminal:
   ```sh
   bash ~/Downloads/sfeerpakket/hd-textures.sh
   ```
5. In de game: Mods → **Reload**. De mod "HD-textures (eigen)" staat erbij.
6. Bekijk alles naast elkaar: `open ~/Dusklight-mods/hd-textures/vergelijking/index.html`

### Wat het script slim doet

- **Real-ESRGAN** (AI-opschaling, via Metal op je GPU). Met `MODEL=realesrgan-x4plus-anime` krijg je een strakkere, meer getekende stijl. Met `OPSCHALER=lanczos` gebruik je geen AI.
- Kleine textures ×4, middelgrote ×2, maximaal 2048 pixels. Egale en piepkleine textures worden overgeslagen.
- **Geen naden** bij herhalende textures (gras, steen): vóór het opschalen wordt de texture rondom met zijn eigen andere kant opgevuld.
- **Bladeren en hekken blijven scherp:** harde transparantie blijft hard, en er komen geen donkere randjes rond bladeren.
- **Mipmaps** worden erbij gemaakt, tegen flikkeren in de verte.
- Met `NORMAL_MAPS=1` worden er ook normal maps geschat, voor spoor 3.

Niet tevreden over een texture? Gooi hem weg uit `~/Dusklight-mods/hd-textures/pakket/textures/` en draai `INPAKKEN=1 bash hd-textures.sh`.

> Het resultaat is gemaakt uit Nintendo's textures uit jouw eigen disc. Alleen voor eigen gebruik; niet verspreiden.

### Hoe het technisch zit

Aurora (de renderer van Dusklight) kan textures zonder vervanging als DDS wegschrijven, maar Dusklight zet die optie (`AuroraConfig::allowTextureDumps`) bij het starten vast op `false`. Sfeerpakket zoekt tijdens het spelen het globale object `aurora::g_config` op (via `dlsym`) en zet de optie aan of uit. Lukt dat niet in een bepaalde build, dan zegt het paneel "niet beschikbaar" en draait de rest gewoon door. De bestandsnamen zijn al in het Dolphin-formaat dat de vervangingsloader verwacht, dus de opgeschaalde PNG's passen er zonder hernoemen in.

---

## Spoor 2: Modellen

Dusklight laadt vervangende bestanden via `overlay/`, ook bestanden **binnen** een `.arc`-archief. Je hoeft dus nooit een archief opnieuw in te pakken.

### Werkwijze

```sh
bash modellen.sh disc ~/Games/tp.rvz     # 1. disc eenmalig uitpakken (nodtool)
bash modellen.sh zoek al.bmd             # 2. waar zit het model? (al = Link)
bash modellen.sh uitpakken Kmdl          # 3. archief naar de werkplaats
bash modellen.sh blender                 # 4. Blender-importer ophalen
#    ... bewerken (zie hieronder) ...
bash modellen.sh inpakken MijnLink       # 5. alleen wat je veranderde -> MijnLink.dusk -> modmap
```

De werkplaats staat in `~/Dusklight-mods/modellen/`:

- `origineel/` is een onaangeroerde kopie;
- `werk/` is waar je bestanden vervangt.

`inpakken` vergelijkt die twee en neemt alléén de gewijzigde bestanden op, op het juiste overlay-pad (bijvoorbeeld `overlay/res/Object/Kmdl/archive/bmwr/al.bmd`).

### Het bewerken zelf

1. **Importeren in Blender:** installeer `blemd.zip` (`modellen.sh blender` haalt hem op; dit is de versie die geoptimaliseerd is voor Twilight Princess). Daarna: File → Import → Nintendo BMD/BDL.
2. **Modelleren:** meer polygonen, betere UV's, nieuwe textures. Houd het skelet (de botten) en de botnamen gelijk, anders kloppen de animaties van de game niet meer.
3. **Terug naar `.bmd`:** exporteer als `.dae` en zet dat om met **SuperBMD** (RenolY2-versie; er is een "TP Model Helpers Edition"). SuperBMD is een Windows-programma; op de Mac draai je het met CrossOver/Wine, of op een Windows-pc. Neem de materiaalinstellingen van het origineel over (`-m materials.json`, die exporteert SuperBMD uit het originele model), anders zien de materialen er in de game anders uit.
4. Zet de nieuwe `.bmd` in `werk/` op de plek van het origineel en draai `modellen.sh inpakken`.

Link en Midna worden bij een gebiedswissel opnieuw geladen, dus je hoeft niet te herstarten.

---

## Spoor 3: Jullie eigen Dusklight

### Stap 1: belichting per pixel (klaar)

De GameCube berekent licht per hoekpunt (vertex), wat de typische vlekkerige, "geblokte" belichting op Link, NPC's en bij lantaarns geeft. Aurora heeft al een per-pixel pad, maar dat staat uit (`UsePerPixelLighting = false` in `lib/gx/gx.hpp`). De patch in `fork/aurora-per-pixel-lighting.patch` doet twee dingen:

- het per-pixel pad aanzetten;
- de normaal per pixel opnieuw normaliseren. Zonder dat wordt het licht tussen de hoekpunten te donker.

```sh
bash ~/Downloads/sfeerpakket/bouw-eigen-dusklight.sh --start
```

Dit bouwt `~/Applications/Dusklight Sfeer.app` van dezelfde versie als je gewone Dusklight, en laat die gewoon staan. Saves, instellingen en mods worden gedeeld.

**Getest:**

- Ik heb de echte shadergenerator van Aurora (v2.0.3) los gebouwd. Daarmee heb ik shaders gegenereerd voor tien TP-achtige materiaalcombinaties: register- en vertexkleur, spot- en specular-attenuatie, belichte alpha, onbelicht, met en zonder normal-target.
- Alle shaders, met de patch én zonder, zijn goedgekeurd door Dawn/Tint: dezelfde shadercompiler als in de game, headless gedraaid.
- De patch past op Dusklight v2.0.3.
- Niet getest: hoe het eruitziet in de game, en de volledige build op de Mac.

### Stap 2: normal maps (ontwerp)

Met belichting per pixel kan een normal map het licht laten reageren op reliëf in steen, schors en stof. Het plan in de Aurora-fork:

1. **Laden:** de texture-vervangingsloader leest naast `tex1_..._14.png` een `tex1_..._14_n.png` (normal map). De HD-pijplijn kan die al maken (`NORMAL_MAPS=1`).
2. **Binden:** als de texture van de eerste TEV-stage een normal map heeft, komt die mee als extra binding. Een vlag in de shaderconfiguratie zorgt dat er een aparte shadervariant voor wordt gegenereerd.
3. **Shader:** de tangentruimte wordt per pixel berekend uit `dpdx/dpdy` van positie en UV (GX-modellen hebben geen tangents). Daarmee wordt `in.mv_nrm` verstoord vóór de lichtberekening die in stap 1 al per pixel draait.
4. **Uit te zoeken:** de echte TEV-configuraties van TP-materialen. Welke stage draagt de "hoofd"-texture? En hoe zit het met emboss/bump-stages, die TP zelf soms gebruikt?

Dit raakt de texturebindingen en de pipelinesleutels van Aurora. Het is daarmee duidelijk meer werk dan stap 1, en het vraagt testen in de game na stap 1.
