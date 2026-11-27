// Sfeerpakket voor Dusklight
//
// Lagen, elk apart aan/uit te zetten in het Mods-venster:
//
//  1. Kleurstemming (GFX_STAGE_FRAME_BEFORE_HUD): filmische curve, witbalans, split toning,
//     verzadiging en vignet over de 3D-scene, voordat de HUD getekend wordt.
//  2. Atmosferische mist (GFX_STAGE_SCENE_AFTER_OPAQUE): hoogtemist, blauwige verre nevel en
//     een warme gloed richting de zon, berekend uit de dieptebuffer.
//  3. Lichtstralen (GFX_STAGE_SCENE_AFTER_OPAQUE): screen-space god rays vanaf de zon, door
//     de gaten in het bladerdak (compute-pass op halve resolutie + additieve composite).
//  4. Vuurvliegjes en stof (GFX_STAGE_SCENE_AFTER_OPAQUE): procedurele deeltjes rond de
//     camera, met dieptetest zodat ze achter bomen verdwijnen.
//  5. Licht per gebied: een post-hook op dScnKy_env_light_c::setLight() die de door de game
//     berekende ambient-, mist- en luchtkleuren per gebied bijstuurt.
//  6. Dichter gras: hooks op het aanmaken van gras/bloemen die extra pollen bijplaatsen naast
//     de originele, binnen de vaste pool van de game.
//  8. Toetsenbord: een standaard toetsindeling (WASD, spatie, muisknoppen) voor speler 1,
//     alleen als er nog geen eigen toetsindeling is ingesteld. De muis-camera zelf is een
//     ingebouwde instelling van Dusklight (game.enableMouseCamera).
//  7. Gereedschap "Textures dumpen": zet de (in de release uitgeschakelde) texture-dump van
//     de Aurora-renderer aan, als bron voor een HD-texturepakket (tools/hd_textures.py).
//
// Alle lagen lezen een "gebiedsprofiel" (Faron Woods, Ordon, Hyrule Field, ...). Bij een
// gebiedswissel schuiven de waarden geleidelijk naar het nieuwe profiel.
//
// Patronen voor pipelines, compute en draw callbacks volgen mods/ao_mod; de berekening van
// de zon- en maanrichting is overgenomen uit mods/shadow_mod (Twilit Realm).

#include "d/actor/d_a_grass.h"
#include <dolphin/pad.h>
#include "d/actor/d_flower.h"
#include "d/actor/d_grass.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_com_inf_actor.h"
#include "m_Do/m_Do_controller_pad.h"
#include "f_op/f_op_actor_mng.h"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <aurora/aurora.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&dScnKy_env_light_c::setLight, EnvSetLight);
// daGrass_create staat in de methodetabel van de gras-actor en wordt dus nooit ge-inlined.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_grass.cpp#daGrass_create", int(daGrass_c*), GrassCreate);
class dMeter2_c;
DEFINE_HOOK_SYMBOL("src/d/d_meter2.cpp#dMeter2_Draw", int(dMeter2_c*), MeterDraw);

namespace {

// ---------------------------------------------------------------------------------------------
// Gebiedsprofielen
// ---------------------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};

struct AreaProfile {
    const char* name;
    const char* stages[6];  // stagenamen uit de game (zie src/dusk/map_loader_definitions.h)

    // Licht: vermenigvuldigers op de kleuren die de game zelf berekent
    Rgb actorAmb;  // ambient op Link, NPC's en objecten
    Rgb bgAmb;     // ambient op de omgeving
    Rgb fogCol;    // de eigen GX-mist van de game
    Rgb sky;       // luchtkleur van de skybox

    // Mist-shader (wereldeenheden: ongeveer centimeters)
    float density;      // extinctie per eenheid op basishoogte
    float falloff;      // hoe snel de mist naar boven dunner wordt
    float baseOffset;   // basishoogte t.o.v. Links hoogte
    float startDist;    // mistvrije zone rond de camera
    float aerialDist;   // afstand waarop verre nevel volledig is
    float maxOpacity;
    float sunGlow;
    Rgb aerial;         // tint op de luchtkleur van de game voor verre nevel

    // Kleurstemming
    float exposure, contrast, saturation, temperature, tint, filmic;
    Rgb shadowTint, highlightTint;

    // Lichtstralen en deeltjes
    float rays;          // sterkte van de lichtstralen
    float fireflyDay;    // aantal vuurvliegjes overdag
    float fireflyNight;  // aantal vuurvliegjes 's nachts
    float dust;          // aantal stofdeeltjes / pollen
};

// Eerste instellingen, afgeleid van de referentiebeelden. Afstellen gebeurt in de game.
constexpr AreaProfile kFaron = {
    "Faron Woods",
    {"F_SP108", "F_SP117", "D_SB10", nullptr},
    {1.10f, 1.06f, 0.94f}, {1.12f, 1.16f, 1.00f}, {1.10f, 1.12f, 0.92f}, {1.00f, 1.00f, 0.98f},
    0.00035f, 0.0009f, -60.0f, 250.0f, 9000.0f, 0.85f, 0.90f, {0.85f, 0.97f, 0.88f},
    0.10f, 1.02f, 1.00f, 0.15f, 0.10f, 0.55f,
    {0.92f, 1.04f, 1.00f}, {1.05f, 1.01f, 0.93f},
    1.60f, 160.0f, 320.0f, 700.0f,
};

constexpr AreaProfile kOrdon = {
    "Ordon",
    {"F_SP103", "F_SP00", "F_SP104", nullptr},
    {1.08f, 1.04f, 0.97f}, {1.12f, 1.14f, 1.02f}, {1.06f, 1.02f, 0.95f}, {1.01f, 1.00f, 0.98f},
    0.00016f, 0.0006f, -120.0f, 500.0f, 16000.0f, 0.75f, 0.85f, {0.92f, 0.96f, 1.04f},
    0.08f, 1.02f, 1.02f, 0.12f, 0.04f, 0.55f,
    {0.95f, 1.02f, 1.03f}, {1.04f, 1.00f, 0.95f},
    1.30f, 50.0f, 180.0f, 450.0f,
};

constexpr AreaProfile kField = {
    "Hyrule Field",
    {"F_SP121", "F_SP122", "F_SP116", nullptr},
    {1.06f, 1.05f, 1.00f}, {1.10f, 1.12f, 1.04f}, {0.85f, 0.95f, 1.15f}, {0.95f, 1.00f, 1.08f},
    0.00008f, 0.0002f, -250.0f, 1200.0f, 35000.0f, 0.75f, 0.70f, {0.72f, 0.86f, 1.18f},
    0.12f, 1.04f, 1.10f, 0.06f, 0.06f, 0.55f,
    {0.95f, 1.01f, 1.06f}, {1.04f, 1.02f, 0.96f},
    1.00f, 0.0f, 100.0f, 520.0f,
};

constexpr AreaProfile kOutdoor = {
    "Buiten (standaard)",
    {nullptr},
    {1.05f, 1.03f, 0.99f}, {1.10f, 1.10f, 1.02f}, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f},
    0.00012f, 0.0005f, -200.0f, 800.0f, 25000.0f, 0.70f, 0.70f, {0.90f, 0.95f, 1.05f},
    0.06f, 1.03f, 1.02f, 0.08f, 0.02f, 0.55f,
    {0.96f, 1.01f, 1.03f}, {1.03f, 1.00f, 0.96f},
    1.00f, 20.0f, 120.0f, 360.0f,
};

constexpr AreaProfile kIndoor = {
    "Binnen",
    {nullptr},
    {1.02f, 1.00f, 0.97f}, {1.02f, 1.00f, 0.97f}, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f},
    0.00002f, 0.0010f, -100.0f, 600.0f, 8000.0f, 0.35f, 0.00f, {1.00f, 1.00f, 1.00f},
    0.00f, 1.05f, 0.98f, 0.15f, 0.00f, 0.50f,
    {0.98f, 1.00f, 1.02f}, {1.04f, 1.00f, 0.94f},
    0.0f, 0.0f, 0.0f, 260.0f,
};

// In de Schemerwereld laten we het originele licht met rust.
constexpr AreaProfile kTwilight = {
    "Schemerwereld",
    {nullptr},
    {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f},
    0.00003f, 0.0005f, -200.0f, 1000.0f, 20000.0f, 0.40f, 0.00f, {1.00f, 1.00f, 1.00f},
    0.00f, 1.04f, 1.00f, 0.00f, 0.00f, 0.40f,
    {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f},
    0.0f, 0.0f, 0.0f, 0.0f,
};

constexpr const AreaProfile* kNamedProfiles[] = {&kFaron, &kOrdon, &kField};

// Het profiel dat nu actief is: schuift elk frame richting het doelprofiel.
AreaProfile g_current = kOutdoor;
const AreaProfile* g_target = &kOutdoor;
bool g_currentInitialized = false;
float g_smoothedPlayerY = 0.0f;
bool g_playerYInitialized = false;

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
Rgb lerp_rgb(const Rgb& a, const Rgb& b, float t) {
    return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t)};
}

void blend_profile(AreaProfile& cur, const AreaProfile& tgt, float t) {
    cur.name = tgt.name;
    cur.actorAmb = lerp_rgb(cur.actorAmb, tgt.actorAmb, t);
    cur.bgAmb = lerp_rgb(cur.bgAmb, tgt.bgAmb, t);
    cur.fogCol = lerp_rgb(cur.fogCol, tgt.fogCol, t);
    cur.sky = lerp_rgb(cur.sky, tgt.sky, t);
    // Dichtheid logaritmisch mengen: die waarden verschillen een factor 5.
    cur.density = std::exp(lerpf(std::log(cur.density), std::log(tgt.density), t));
    cur.falloff = lerpf(cur.falloff, tgt.falloff, t);
    cur.baseOffset = lerpf(cur.baseOffset, tgt.baseOffset, t);
    cur.startDist = lerpf(cur.startDist, tgt.startDist, t);
    cur.aerialDist = lerpf(cur.aerialDist, tgt.aerialDist, t);
    cur.maxOpacity = lerpf(cur.maxOpacity, tgt.maxOpacity, t);
    cur.sunGlow = lerpf(cur.sunGlow, tgt.sunGlow, t);
    cur.aerial = lerp_rgb(cur.aerial, tgt.aerial, t);
    cur.exposure = lerpf(cur.exposure, tgt.exposure, t);
    cur.contrast = lerpf(cur.contrast, tgt.contrast, t);
    cur.saturation = lerpf(cur.saturation, tgt.saturation, t);
    cur.temperature = lerpf(cur.temperature, tgt.temperature, t);
    cur.tint = lerpf(cur.tint, tgt.tint, t);
    cur.filmic = lerpf(cur.filmic, tgt.filmic, t);
    cur.shadowTint = lerp_rgb(cur.shadowTint, tgt.shadowTint, t);
    cur.highlightTint = lerp_rgb(cur.highlightTint, tgt.highlightTint, t);
    cur.rays = lerpf(cur.rays, tgt.rays, t);
    cur.fireflyDay = lerpf(cur.fireflyDay, tgt.fireflyDay, t);
    cur.fireflyNight = lerpf(cur.fireflyNight, tgt.fireflyNight, t);
    cur.dust = lerpf(cur.dust, tgt.dust, t);
}

const AreaProfile* pick_profile() {
    if (dKy_darkworld_check() != 0) {
        return &kTwilight;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && stage[0] != '\0') {
        for (const AreaProfile* profile : kNamedProfiles) {
            for (const char* const* s = profile->stages; *s != nullptr; ++s) {
                if (std::strncmp(stage, *s, 8) == 0) {
                    return profile;
                }
            }
        }
    }
    // Zelfde test als dKy_Outdoor_check(), maar veilig zolang er nog geen stage geladen is.
    dStage_stageDt_c* stageDt = dComIfGp_getStage();
    if (stageDt == nullptr || stageDt->getStagInfo() == nullptr) {
        return &kOutdoor;
    }
    return dKy_Outdoor_check() ? &kOutdoor : &kIndoor;
}

// ---------------------------------------------------------------------------------------------
// Instellingen
// ---------------------------------------------------------------------------------------------

ConfigVarHandle g_cvarGradeEnabled = 0;
ConfigVarHandle g_cvarGradeStrength = 0;
ConfigVarHandle g_cvarVignette = 0;
ConfigVarHandle g_cvarSplitView = 0;
ConfigVarHandle g_cvarFogEnabled = 0;
ConfigVarHandle g_cvarFogDensity = 0;
ConfigVarHandle g_cvarSunGlow = 0;
ConfigVarHandle g_cvarFogDebug = 0;
ConfigVarHandle g_cvarRaysEnabled = 0;
ConfigVarHandle g_cvarRaysIntensity = 0;
ConfigVarHandle g_cvarRaysQuality = 0;
ConfigVarHandle g_cvarRaysDebug = 0;
ConfigVarHandle g_cvarParticlesEnabled = 0;
ConfigVarHandle g_cvarParticleAmount = 0;
ConfigVarHandle g_cvarParticleBrightness = 0;
ConfigVarHandle g_cvarAreaEnabled = 0;
ConfigVarHandle g_cvarAreaAmount = 0;
ConfigVarHandle g_cvarAreaIntensity = 0;
ConfigVarHandle g_cvarPhoto = 0;
ConfigVarHandle g_cvarPhotoFreeze = 0;
ConfigVarHandle g_cvarPhotoHud = 0;
ConfigVarHandle g_cvarPhotoSpeed = 0;
ConfigVarHandle g_cvarPhotoFov = 0;
ConfigVarHandle g_cvarPhotoHour = 0;
ConfigVarHandle g_cvarGrassEnabled = 0;
ConfigVarHandle g_cvarGrassDensity = 0;
ConfigVarHandle g_cvarFlowerDensity = 0;
ConfigVarHandle g_cvarTextureDump = 0;
ConfigVarHandle g_cvarAaEnabled = 0;
ConfigVarHandle g_cvarAaDebug = 0;
ConfigVarHandle g_cvarSharpen = 0;
ConfigVarHandle g_cvarVibrance = 0;
ConfigVarHandle g_cvarFlareEnabled = 0;
ConfigVarHandle g_cvarFlareStrength = 0;
ConfigVarHandle g_cvarGrain = 0;
ConfigVarHandle g_cvarBloom = 0;
ConfigVarHandle g_cvarBloomThreshold = 0;
ConfigVarHandle g_cvarLift = 0;
ConfigVarHandle g_cvarKeyboardDefaults = 0;
ConfigVarHandle g_cvarLightingEnabled = 0;
ConfigVarHandle g_cvarLightingStrength = 0;
ConfigVarHandle g_cvarTransitionSpeed = 0;

int64_t get_int(ConfigVarHandle h, int64_t fallback) {
    int64_t v = fallback;
    if (h == 0 || svc_config->get_int(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

bool get_bool(ConfigVarHandle h, bool fallback) {
    bool v = fallback;
    if (h == 0 || svc_config->get_bool(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

float get_percent(ConfigVarHandle h, int64_t fallback, int64_t maxValue) {
    return static_cast<float>(std::clamp<int64_t>(get_int(h, fallback), 0, maxValue)) / 100.0f;
}

// ---------------------------------------------------------------------------------------------
// Zon, kleuren, matrices
// ---------------------------------------------------------------------------------------------

constexpr float kSunMoonDistance = 80000.0f;
constexpr float kSunMoonZDistance = -48000.0f;

float wrap_daytime(float daytime) {
    if (!std::isfinite(daytime)) {
        return 180.0f;
    }
    float wrapped = std::fmod(daytime, 360.0f);
    return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
}

float daytime_percent(float max, float min, float value) {
    const float range = max - min;
    return range == 0.0f ? 1.0f : (value - min) / range;
}

// Spiegelt dScnKy_env_light_c::setSunpos() (overgenomen uit mods/shadow_mod).
float sun_moon_angle(float daytime) {
    daytime = wrap_daytime(daytime);
    if (daytime >= 90.0f && daytime <= 270.0f) {
        return daytime_percent(270.0f, 90.0f, daytime) * 150.0f + 105.0f;
    }
    float angle = daytime < 90.0f ? daytime + 360.0f : daytime;
    angle = daytime_percent(450.0f, 270.0f, angle) * 210.0f + 255.0f;
    return angle > 360.0f ? angle - 360.0f : angle;
}

struct SunState {
    float dir[3] = {0.0f, 1.0f, 0.0f};
    float color[3] = {0.0f, 0.0f, 0.0f};  // lineair, al geschaald met hoogte
    bool isNight = false;
};

SunState compute_sun() {
    SunState s;
    const float daytime = wrap_daytime(dComIfGs_getTime());
    const float angle = sun_moon_angle(daytime) * 3.14159265f / 180.0f;
    float x = std::sin(angle) * kSunMoonDistance;
    float y = -std::cos(angle) * kSunMoonDistance;
    float z = std::cos(angle) * kSunMoonZDistance;
    if (y <= 0.0f) {  // zon onder de horizon: gebruik de maan
        const float a2 = sun_moon_angle(daytime + 180.0f) * 3.14159265f / 180.0f;
        x = std::sin(a2) * kSunMoonDistance;
        y = -std::cos(a2) * kSunMoonDistance;
        z = std::cos(a2) * kSunMoonZDistance;
        s.isNight = true;
    }
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1.0f) {
        return s;
    }
    s.dir[0] = x / len;
    s.dir[1] = y / len;
    s.dir[2] = z / len;
    const float elevation = std::clamp(s.dir[1] / 0.25f, 0.0f, 1.0f);
    if (s.isNight) {
        s.color[0] = 0.25f * elevation;
        s.color[1] = 0.30f * elevation;
        s.color[2] = 0.45f * elevation;
    } else {
        // Laag bij de horizon oranjer, hoog wit-geel.
        const float high = std::clamp(s.dir[1] / 0.6f, 0.0f, 1.0f);
        s.color[0] = elevation * 1.00f;
        s.color[1] = elevation * lerpf(0.62f, 0.90f, high);
        s.color[2] = elevation * lerpf(0.35f, 0.72f, high);
    }
    return s;
}

float srgb8_to_linear(float v255) {
    const float c = std::clamp(v255 / 255.0f, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_display(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

bool is_srgb_format(WGPUTextureFormat f) {
    return f == WGPUTextureFormat_BGRA8UnormSrgb || f == WGPUTextureFormat_RGBA8UnormSrgb;
}

// Kolom-major 4x4 (CameraService-conventie) maal kolomvector.
void mat_mul_vec4(const float m[16], const float v[4], float out[4]) {
    for (int r = 0; r < 4; ++r) {
        out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] +
                 m[3 * 4 + r] * v[3];
    }
}

// Ruwe dieptewaarde van een punt op afstand `dist` recht voor de camera. Alles voorbij die
// diepte telt als lucht (skydome of gewiste pixels).
constexpr float kSkyDistance = 30000.0f;  // ~300 m: verder weg = lucht/skydome

float sky_distance(const CameraInfo& cam) {
    return std::max(std::min(cam.far_plane * 0.9f, kSkyDistance), 1000.0f);
}

float sky_depth_threshold(const CameraInfo& cam, bool reversedZ) {
    const float dist = sky_distance(cam);
    const float viewPoint[4] = {0.0f, 0.0f, -dist, 1.0f};
    float clip[4];
    mat_mul_vec4(cam.proj_from_view, viewPoint, clip);
    if (std::fabs(clip[3]) < 1e-6f) {
        return reversedZ ? 0.0f : 1.0f;
    }
    const float d = clip[2] / clip[3];
    return std::clamp(d, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------
// Licht per gebied: post-hook op setLight()
// ---------------------------------------------------------------------------------------------

// setLight() rekent de kleuren elk frame opnieuw uit. Voor het geval een codepad een kleur
// overslaat, onthouden we wat wij schreven: staat die waarde er nog, dan passen we niet
// nog eens toe (anders zou de vermenigvuldiging zich frame na frame opstapelen).
struct ColorGuard {
    GXColorS10 written{};
    bool valid = false;
};

ColorGuard g_guardActor;
ColorGuard g_guardBg[4];
ColorGuard g_guardFog;
ColorGuard g_guardSky;

bool same_color(const GXColorS10& a, const GXColorS10& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

void apply_tint(GXColorS10& c, const Rgb& mul, float strength, ColorGuard& guard) {
    if (guard.valid && same_color(c, guard.written)) {
        return;
    }
    const auto scale = [&](s16 v, float m) {
        const float f = 1.0f + (m - 1.0f) * strength;
        return static_cast<s16>(std::clamp(static_cast<int>(std::lround(v * f)), 0, 255));
    };
    c.r = scale(c.r, mul.r);
    c.g = scale(c.g, mul.g);
    c.b = scale(c.b, mul.b);
    guard.written = c;
    guard.valid = true;
}

void on_set_light_post(ModContext*, void* args, void*, void*) {
    if (!get_bool(g_cvarLightingEnabled, true) || g_target == &kTwilight) {
        return;
    }
    auto* env = mods::arg<dScnKy_env_light_c*>(args, 0);
    if (env == nullptr) {
        return;
    }
    const float s = get_percent(g_cvarLightingStrength, 100, 200);
    apply_tint(env->actor_amb_col, g_current.actorAmb, s, g_guardActor);
    for (int i = 0; i < 4; ++i) {
        apply_tint(env->bg_amb_col[i], g_current.bgAmb, s, g_guardBg[i]);
    }
    apply_tint(env->fog_col, g_current.fogCol, s, g_guardFog);
    apply_tint(env->vrbox_sky_col, g_current.sky, s, g_guardSky);
}

// ---------------------------------------------------------------------------------------------
// Dichter gras: hook op daGrass_create
// ---------------------------------------------------------------------------------------------
//
// Elke gras-actor in een kamer plaatst bij het aanmaken een plukje graspollen (of bloemen) in
// de gedeelde pool van de game (1500 gras, 1000 bloemen). Vóór het aanmaken onthouden we welke
// plekken in de pool bezet zijn; daarna weten we precies welke pollen nieuw zijn en zetten we
// er extra pollen vlak naast. De game zet ze daarna zelf op de grond (checkGroundY), ze wiegen
// mee en zijn gewoon te maaien, maar laten nooit items vallen.

constexpr int kGrassPool = 1500;
constexpr int kFlowerPool = 1000;
constexpr int kGrassReserve = 250;   // altijd vrij houden voor originele pollen van later
constexpr int kFlowerReserve = 150;
constexpr float kExtraRadius = 38.0f;

std::bitset<kGrassPool> g_grassUsedBefore;
std::bitset<kFlowerPool> g_flowerUsedBefore;
int g_grassExtraTotal = 0;

bool grass_slot_used(const dGrass_data_c& d) { return d.m_state != 0; }
bool flower_slot_used(const dFlower_data_c& d) { return (d.m_state & 2) != 0; }

uint32_t g_rng = 0x9E3779B9u;
float rnd01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return static_cast<float>(g_rng & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

// Aantal extra's per originele pol: bv. 250% -> 1 of 2 (gemiddeld 1,5).
int extras_for(float multiplier) {
    const float extra = std::max(multiplier - 1.0f, 0.0f);
    int n = static_cast<int>(extra);
    if (rnd01() < extra - static_cast<float>(n)) {
        ++n;
    }
    return n;
}

HookAction on_grass_create_pre(ModContext*, void*, void*, void*) {
    g_grassUsedBefore.reset();
    g_flowerUsedBefore.reset();
    if (dGrass_packet_c* grass = daGrass_c::getGrass()) {
        for (int i = 0; i < kGrassPool; ++i) {
            g_grassUsedBefore[i] = grass_slot_used(grass->m_data[i]);
        }
    }
    if (dFlower_packet_c* flower = daGrass_c::getFlower()) {
        for (int i = 0; i < kFlowerPool; ++i) {
            g_flowerUsedBefore[i] = flower_slot_used(flower->m_data[i]);
        }
    }
    return HOOK_CONTINUE;
}

void on_grass_create_post(ModContext*, void* args, void*, void*) {
    if (!get_bool(g_cvarGrassEnabled, true)) {
        return;
    }
    daGrass_c* actor = mods::arg<daGrass_c*>(args, 0);
    if (actor == nullptr) {
        return;
    }
    const int roomNo = fopAcM_GetRoomNo(actor);
    if (roomNo < 0 || roomNo >= 64) {
        return;
    }

    if (dGrass_packet_c* grass = daGrass_c::getGrass()) {
        const float mult = get_percent(g_cvarGrassDensity, 300, 400);
        struct Parent {
            cXyz pos;
            s16 addCol;
            u8 param;
        };
        std::vector<Parent> parents;
        int freeSlots = 0;
        for (int i = 0; i < kGrassPool; ++i) {
            const dGrass_data_c& d = grass->m_data[i];
            if (!grass_slot_used(d)) {
                ++freeSlots;
            } else if (!g_grassUsedBefore[i]) {
                parents.push_back({d.m_pos, d.m_addCol, d.field_0x05});
            }
        }
        for (const Parent& p : parents) {
            const int n = extras_for(mult);
            for (int k = 0; k < n && freeSlots > kGrassReserve; ++k) {
                const float a = rnd01() * 6.2831853f;
                const float r = kExtraRadius * (0.45f + 0.55f * rnd01());
                cXyz pos(p.pos.x + std::cos(a) * r, p.pos.y, p.pos.z + std::sin(a) * r);
                if (grass->newData(pos, roomNo, dItemNo_NONE_e, 0xFF, p.addCol, p.param) ==
                    nullptr)
                {
                    freeSlots = 0;
                    break;
                }
                --freeSlots;
                ++g_grassExtraTotal;
            }
        }
    }

    if (dFlower_packet_c* flower = daGrass_c::getFlower()) {
        const float mult = get_percent(g_cvarFlowerDensity, 250, 400);
        struct Parent {
            cXyz pos;
            s16 addCol;
            s8 kind;
        };
        std::vector<Parent> parents;
        int freeSlots = 0;
        for (int i = 0; i < kFlowerPool; ++i) {
            const dFlower_data_c& d = flower->m_data[i];
            if (!flower_slot_used(d)) {
                ++freeSlots;
            } else if (!g_flowerUsedBefore[i]) {
                parents.push_back(
                    {d.m_pos, d.field_0x04, static_cast<s8>((d.m_state & 0x40) ? 2 : 1)});
            }
        }
        for (const Parent& p : parents) {
            const int n = extras_for(mult);
            for (int k = 0; k < n && freeSlots > kFlowerReserve; ++k) {
                const float a = rnd01() * 6.2831853f;
                const float r = kExtraRadius * (0.5f + 0.7f * rnd01());
                cXyz pos(p.pos.x + std::cos(a) * r, p.pos.y, p.pos.z + std::sin(a) * r);
                if (flower->newData(p.kind, pos, roomNo, static_cast<s8>(dItemNo_NONE_e),
                        p.addCol) == nullptr)
                {
                    freeSlots = 0;
                    break;
                }
                --freeSlots;
            }
        }
    }
}


// ---------------------------------------------------------------------------------------------
// Gereedschap: textures dumpen
// ---------------------------------------------------------------------------------------------
//
// Aurora kan elke texture zonder vervanging als DDS wegschrijven naar <cache>/texture_dumps,
// maar Dusklight zet AuroraConfig::allowTextureDumps bij het starten vast op false. De
// configuratie leeft in het globale object aurora::g_config; we zoeken het tijdens het draaien
// op (dlsym), zodat de mod ook laadt als het symbool in een build niet geëxporteerd is.

AuroraConfig* g_auroraConfig = nullptr;
bool g_dumpLookupDone = false;
bool g_dumpOriginal = false;
bool g_dumpActive = false;
std::string g_dumpDir;
int g_dumpFileCount = 0;
uint32_t g_dumpCountTick = 0;

AuroraConfig* find_aurora_config() {
    if (g_dumpLookupDone) {
        return g_auroraConfig;
    }
    g_dumpLookupDone = true;
#if defined(_WIN32)
    g_auroraConfig = nullptr;  // MSVC-mangling verschilt; op Windows niet ondersteund
#else
    g_auroraConfig = static_cast<AuroraConfig*>(dlsym(RTLD_DEFAULT, "_ZN6aurora8g_configE"));
#endif
    if (g_auroraConfig != nullptr) {
        g_dumpOriginal = g_auroraConfig->allowTextureDumps;
        if (g_auroraConfig->cachePath != nullptr) {
            g_dumpDir = (std::filesystem::path(g_auroraConfig->cachePath) / "texture_dumps").string();
        }
    }
    return g_auroraConfig;
}

void apply_texture_dump(bool enabled) {
    AuroraConfig* cfg = find_aurora_config();
    if (cfg == nullptr) {
        if (enabled) {
            svc_log->warn(mod_ctx, "textures dumpen niet beschikbaar in deze Dusklight-build");
        }
        return;
    }
    if (enabled && !g_dumpDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_dumpDir, ec);  // Aurora maakt de map alleen bij het starten
    }
    cfg->allowTextureDumps = enabled ? true : g_dumpOriginal;
    if (enabled != g_dumpActive) {
        g_dumpActive = enabled;
        char text[512];
        std::snprintf(text, sizeof(text), "textures dumpen %s (%s)", enabled ? "AAN" : "uit",
            g_dumpDir.c_str());
        svc_log->info(mod_ctx, text);
    }
}

void on_texture_dump_changed(ModContext*, ConfigVarHandle, const ConfigVarValue* value,
    const ConfigVarValue*, void*) {
    apply_texture_dump(value != nullptr && value->bool_value);
}

void count_dump_files() {
    if (!g_dumpActive || g_dumpDir.empty() || (++g_dumpCountTick % 120u) != 0) {
        return;
    }
    int n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(g_dumpDir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    g_dumpFileCount = n;
}


// ---------------------------------------------------------------------------------------------
// Toetsenbord: standaard toetsindeling
// ---------------------------------------------------------------------------------------------
//
// Dusklight heeft geen standaardtoetsen: elke toets staat op "niet ingesteld" tot je hem zelf
// koppelt. We zetten een indeling voor speler 1, maar alléén als er nog geen eigen indeling
// is (PADGetKeyButtonBindings geeft dan nullptr). Wie later in Instellingen > Besturing iets
// aanpast, krijgt een opgeslagen eigen indeling en die blijft voortaan staan.
//
// Toetscodes zijn SDL-scancodes (= USB-HID-codes), onafhankelijk van de toetsenbordindeling.

namespace sc {
constexpr s32 A = 4, C = 6, D = 7, E = 8, F = 9, I = 12, Q = 20, R = 21, S = 22, V = 25, W = 26;
constexpr s32 Return = 40, Tab = 43, Space = 44, Right = 79, Left = 80, Down = 81, Up = 82;
constexpr s32 LShift = 225;
}  // namespace sc

bool g_keyboardApplied = false;

void apply_keyboard_defaults() {
    u32 count = 0;
    if (PADGetKeyButtonBindings(0, &count) != nullptr) {
        svc_log->info(mod_ctx, "eigen toetsindeling gevonden; standaardindeling niet gezet");
        return;
    }
    const PADKeyButtonBinding buttons[] = {
        {sc::Space, PAD_BUTTON_A},             // actie / springen / praten
        {PAD_KEY_MOUSE_LEFT, PAD_BUTTON_B},    // zwaard
        {sc::Q, PAD_BUTTON_X},                 // voorwerp X
        {sc::E, PAD_BUTTON_Y},                 // voorwerp Y
        {sc::Return, PAD_BUTTON_START},        // pauze / menu
        {sc::F, PAD_TRIGGER_Z},                // Midna
        {PAD_KEY_MOUSE_RIGHT, PAD_TRIGGER_L},  // richten / vastzetten
        {sc::LShift, PAD_TRIGGER_R},           // schild / bukken
        {sc::I, PAD_BUTTON_UP},                // D-pad omhoog (voorwerpen)
        {sc::C, PAD_BUTTON_DOWN},              // D-pad omlaag
        {sc::V, PAD_BUTTON_LEFT},              // D-pad links
        {sc::Tab, PAD_BUTTON_RIGHT},           // D-pad rechts (kaart)
    };
    const PADKeyAxisBinding axes[] = {
        {sc::D, PAD_AXIS_LEFT_X_POS, 100},     // lopen
        {sc::A, PAD_AXIS_LEFT_X_NEG, 100},
        {sc::W, PAD_AXIS_LEFT_Y_POS, 100},
        {sc::S, PAD_AXIS_LEFT_Y_NEG, 100},
        {sc::Right, PAD_AXIS_RIGHT_X_POS, 100},  // camera met pijltjes (naast de muis)
        {sc::Left, PAD_AXIS_RIGHT_X_NEG, 100},
        {sc::Up, PAD_AXIS_RIGHT_Y_POS, 100},
        {sc::Down, PAD_AXIS_RIGHT_Y_NEG, 100},
        {PAD_KEY_MOUSE_RIGHT, PAD_AXIS_TRIGGER_L, 100},
        {sc::LShift, PAD_AXIS_TRIGGER_R, 100},
    };
    for (const auto& b : buttons) {
        PADSetKeyButtonBinding(0, b);
    }
    for (const auto& a : axes) {
        PADSetKeyAxisBinding(0, a);
    }
    PADSetKeyboardActive(0, TRUE);
    g_keyboardApplied = true;
    svc_log->info(mod_ctx, "standaard toetsindeling gezet (WASD, spatie, muisknoppen)");
}

// ---------------------------------------------------------------------------------------------
// GPU-objecten
// ---------------------------------------------------------------------------------------------

ResourceBuffer g_gradeSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_fogSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_raysComputeSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_raysCompositeSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_particlesSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;

GfxDrawTypeHandle g_gradeDrawType = 0;
GfxDrawTypeHandle g_fogDrawType = 0;
GfxDrawTypeHandle g_raysDrawType = 0;
GfxDrawTypeHandle g_particlesDrawType = 0;
GfxDrawTypeHandle g_areaDrawType = 0;
ResourceBuffer g_areaSource = RESOURCE_BUFFER_INIT;
GfxComputeTypeHandle g_raysComputeType = 0;
GfxStageHookHandle g_beforeHudHook = 0;
GfxStageHookHandle g_afterOpaqueHook = 0;

enum class Blend { Replace, Alpha, Additive, Premultiplied };

struct PipelineSlot {
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    uint64_t key = 0;
    bool built = false;
};

PipelineSlot g_gradePipeline;
PipelineSlot g_fxaaPipeline;
WGPUSampler g_linearSampler = nullptr;
GfxDrawTypeHandle g_fxaaDrawType = 0;
ResourceBuffer g_fxaaSource = RESOURCE_BUFFER_INIT;

// Zon op het scherm, berekend in de wereldcamera-stage en gebruikt door de kleurpass
// (die stage heeft zelf geen camera).
struct ScreenSun {
    bool valid = false;
    float uv[2] = {0.5f, 0.5f};
    float strength = 0.0f;     // kijkrichting * buiten-beeld-afzwakking * zonhoogte
    float color[3] = {0.0f, 0.0f, 0.0f};
    float skyDepth = 0.0f;
    uint32_t frame = 0;
};
ScreenSun g_screenSun;

ResourceBuffer g_bloomSource = RESOURCE_BUFFER_INIT;
WGPUComputePipeline g_bloomDownPipeline = nullptr;
WGPUComputePipeline g_bloomBlurPipeline = nullptr;
WGPUBindGroupLayout g_bloomDownLayout = nullptr;
WGPUBindGroupLayout g_bloomBlurLayout = nullptr;
GfxComputeTypeHandle g_bloomComputeType = 0;

// Wat er het laatste frame echt getekend is (voor het paneel).
struct Diag {
    bool fog = false;
    const char* rays = "-";
    uint32_t particles = 0;
    const char* area = "-";
    uint32_t areaCount = 0;
    bool photo = false;
    bool bloom = false;
    bool aa = false;
    bool grade = false;
    bool depth = false;
};
Diag g_diag;
float g_sunElevation = 1.0f;
PipelineSlot g_fogPipeline;
PipelineSlot g_raysPipeline;
PipelineSlot g_particlesPipeline;
PipelineSlot g_areaPipeline;
WGPUComputePipeline g_raysComputePipeline = nullptr;
WGPUBindGroupLayout g_raysComputeLayout = nullptr;

uint32_t g_frameCounter = 0;
bool g_warnedFogUnavailable = false;
const auto g_startTime = std::chrono::steady_clock::now();

// Stralentextuur (halve resolutie). Oude texturen blijven een paar frames bestaan, omdat
// payloads van vorige frames er nog naar kunnen verwijzen.
struct RaysTarget {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};
RaysTarget g_raysTarget;
struct RetiredTarget {
    RaysTarget target;
    int framesLeft;
};
std::vector<RetiredTarget> g_retiredTargets;

// Spiegels van de WGSL-structs (houd synchroon met res/*.wgsl).
struct GradeUniforms {
    float shadow_tint[3];
    float exposure;
    float highlight_tint[3];
    float contrast;
    float size[2];
    float saturation;
    float temperature;
    float filmic;
    float vignette;
    float strength;
    float input_is_srgb;
    float frame;
    float tint;
    float debug_split;
    float sharpen;
    float sun_uv[2];
    float flare;
    float sky_depth;
    float sun_color[3];
    float reversed_z;
    float vibrance;
    float grain;
    float aspect;
    float has_depth;
    float bloom_size[2];
    float bloom;
    float lift;
};
static_assert(sizeof(GradeUniforms) == 144);

struct BloomUniforms {
    float src_size[2];
    float dst_size[2];
    float dir[2];
    float threshold;
    float knee;
};
static_assert(sizeof(BloomUniforms) == 32);

struct BloomPayload {
    WGPUTextureView src;
    WGPUTextureView dst;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t width;
    uint32_t height;
    uint32_t blur;  // 0 = down, 1 = blur
};
static_assert(sizeof(BloomPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

struct FxaaUniforms {
    float size[2];
    float inv_size[2];
    float subpix;
    float edge_threshold;
    float edge_threshold_min;
    float debug_view;
};
static_assert(sizeof(FxaaUniforms) == 32);

struct FogUniforms {
    float world_from_proj[16];
    float eye[3];
    float density;
    float fog_color[3];
    float height_falloff;
    float sun_dir[3];
    float sun_glow;
    float sun_color[3];
    float base_height;
    float aerial_color[3];
    float max_opacity;
    float start_distance;
    float aerial_distance;
    float sky_distance;
    float reversed_z;
    float size[2];
    float output_srgb;
    float debug_view;
    float sky_depth;
    float sky_dist;
    float _pad[2];
};
static_assert(sizeof(FogUniforms) == 192);

struct RaysComputeUniforms {
    float sun_uv[2];
    float out_size[2];
    float depth_size[2];
    float sky_depth;
    float reversed_z;
    float samples;
    float decay;
    float density;
    float weight;
};
static_assert(sizeof(RaysComputeUniforms) == 48);

struct RaysCompositeUniforms {
    float sun_color[3];
    float intensity;
    float rays_size[2];
    float sun_uv[2];
    float aspect;
    float debug_view;
    float falloff;
    float _pad;
};
static_assert(sizeof(RaysCompositeUniforms) == 48);

struct ParticleUniforms {
    float proj_from_world[16];
    float cam_right[3];
    float time;
    float cam_up[3];
    float firefly_box;
    float eye[3];
    float ground_y;
    float firefly_color[3];
    float firefly_count;
    float dust_color[3];
    float dust_count;
    float sun_dir[3];
    float firefly_size;
    float dust_size;
    float dust_box;
    float firefly_height;
    float brightness;
};
static_assert(sizeof(ParticleUniforms) == 176);

struct AreaParticleUniforms {
    float proj_from_world[16];
    float cam_right[3];
    float time;
    float cam_up[3];
    float count;
    float eye[3];
    float kind;
    float light[3];
    float box;
    float wind[3];
    float size;
    float sun_dir[3];
    float intensity;
};
static_assert(sizeof(AreaParticleUniforms) == 160);

struct DrawPayload {
    WGPUTextureView view;  // snapshot of textuur van dit frame (mag null zijn voor deeltjes)
    WGPUTextureView view2; // tweede textuur (diepte voor de kleurpass), of null
    WGPUTextureView view3; // derde textuur (bloom voor de kleurpass), of null
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t instance_count;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

struct RaysComputePayload {
    WGPUTextureView depth;
    WGPUTextureView output;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(RaysComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<RaysComputePayload>);

WGPUShaderModule create_shader_module(const char* label, const ResourceBuffer& source) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    desc.label = {label, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_deviceInfo.device, &desc);
}

void release_slot(PipelineSlot& slot) {
    if (slot.pipeline != nullptr) {
        wgpuRenderPipelineRelease(slot.pipeline);
    }
    if (slot.layout != nullptr) {
        wgpuBindGroupLayoutRelease(slot.layout);
    }
    slot = PipelineSlot{};
}

// Pipeline voor de scene-target. depthTest: test tegen de scenediepte (deeltjes); anders
// "always" (fullscreen-passes).
bool ensure_pipeline(PipelineSlot& slot, const GfxRenderTargetLayout& target, const char* label,
    const ResourceBuffer& source, Blend blendMode, bool depthTest) {
    if (slot.built && slot.key == target.key) {
        return slot.pipeline != nullptr;
    }
    release_slot(slot);
    slot.built = true;
    slot.key = target.key;

    WGPUShaderModule module = create_shader_module(label, source);
    if (module == nullptr) {
        return false;
    }

    const WGPUBlendState alphaBlend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_SrcAlpha,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    const WGPUBlendState additiveBlend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_One,
            .dstFactor = WGPUBlendFactor_One},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    const WGPUBlendState premulBlend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_One,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    const WGPUBlendState* blend = blendMode == Blend::Premultiplied ? &premulBlend :
                                  blendMode == Blend::Alpha    ? &alphaBlend :
                                  blendMode == Blend::Additive ? &additiveBlend :
                                                                 nullptr;
    WGPUColorTargetState colorTargets[GFX_MAX_COLOR_ATTACHMENTS];
    const uint32_t colorTargetCount =
        gfx_init_color_target_states(&target, colorTargets, blend, WGPUColorWriteMask_All);

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = colorTargetCount;
    fragment.targets = colorTargets;

    // De diepte-instelling moet bij de pass passen; we schrijven nooit diepte.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = target.depth_stencil_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;
    if (depthTest) {
        depthStencil.depthCompare = g_deviceInfo.uses_reversed_z ?
                                        WGPUCompareFunction_GreaterEqual :
                                        WGPUCompareFunction_LessEqual;
    }

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {label, WGPU_STRLEN};
    desc.vertex.module = module;
    desc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.depthStencil =
        target.depth_stencil_format != WGPUTextureFormat_Undefined ? &depthStencil : nullptr;
    desc.multisample.count = std::max<uint32_t>(target.sample_count, 1u);
    desc.fragment = &fragment;
    slot.pipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &desc);
    wgpuShaderModuleRelease(module);
    if (slot.pipeline == nullptr) {
        return false;
    }
    slot.layout = wgpuRenderPipelineGetBindGroupLayout(slot.pipeline, 0);
    return slot.layout != nullptr;
}

bool build_rays_compute_pipeline() {
    WGPUShaderModule module = create_shader_module("Sfeerpakket rays", g_raysComputeSource);
    if (module == nullptr) {
        return false;
    }
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {"Sfeerpakket rays", WGPU_STRLEN};
    desc.compute.module = module;
    desc.compute.entryPoint = {"main", WGPU_STRLEN};
    g_raysComputePipeline = wgpuDeviceCreateComputePipeline(g_deviceInfo.device, &desc);
    wgpuShaderModuleRelease(module);
    if (g_raysComputePipeline == nullptr) {
        return false;
    }
    g_raysComputeLayout = wgpuComputePipelineGetBindGroupLayout(g_raysComputePipeline, 0);
    return g_raysComputeLayout != nullptr;
}

void release_rays_target(RaysTarget& t) {
    if (t.view != nullptr) {
        wgpuTextureViewRelease(t.view);
    }
    if (t.texture != nullptr) {
        wgpuTextureRelease(t.texture);
    }
    t = RaysTarget{};
}

void tick_retired_targets() {
    for (auto it = g_retiredTargets.begin(); it != g_retiredTargets.end();) {
        if (--it->framesLeft <= 0) {
            release_rays_target(it->target);
            it = g_retiredTargets.erase(it);
        } else {
            ++it;
        }
    }
}

bool ensure_rays_target(uint32_t width, uint32_t height) {
    if (g_raysTarget.texture != nullptr && g_raysTarget.width == width &&
        g_raysTarget.height == height)
    {
        return true;
    }
    if (g_raysTarget.texture != nullptr) {
        g_retiredTargets.push_back({std::exchange(g_raysTarget, RaysTarget{}), 4});
    }
    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.label = {"Sfeerpakket rays", WGPU_STRLEN};
    texDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    texDesc.size = {width, height, 1};
    texDesc.format = WGPUTextureFormat_R32Float;
    texDesc.mipLevelCount = 1;
    g_raysTarget.texture = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
    if (g_raysTarget.texture == nullptr) {
        return false;
    }
    g_raysTarget.view = wgpuTextureCreateView(g_raysTarget.texture, nullptr);
    if (g_raysTarget.view == nullptr) {
        release_rays_target(g_raysTarget);
        return false;
    }
    g_raysTarget.width = width;
    g_raysTarget.height = height;
    return true;
}

// Twee bloom-texturen op kwart-resolutie (rgba16float), pingpong voor de vervaging.
RaysTarget g_bloomA;
RaysTarget g_bloomB;

bool make_bloom_target(RaysTarget& t, uint32_t w, uint32_t h, const char* label) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.label = {label, WGPU_STRLEN};
    d.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    d.size = {w, h, 1};
    d.format = WGPUTextureFormat_RGBA16Float;
    d.mipLevelCount = 1;
    t.texture = wgpuDeviceCreateTexture(g_deviceInfo.device, &d);
    if (t.texture == nullptr) {
        return false;
    }
    t.view = wgpuTextureCreateView(t.texture, nullptr);
    t.width = w;
    t.height = h;
    return t.view != nullptr;
}

bool ensure_bloom_targets(uint32_t w, uint32_t h) {
    if (g_bloomA.texture != nullptr && g_bloomA.width == w && g_bloomA.height == h) {
        return true;
    }
    if (g_bloomA.texture != nullptr) {
        g_retiredTargets.push_back({std::exchange(g_bloomA, RaysTarget{}), 4});
        g_retiredTargets.push_back({std::exchange(g_bloomB, RaysTarget{}), 4});
    }
    return make_bloom_target(g_bloomA, w, h, "Sfeerpakket bloom A") &&
           make_bloom_target(g_bloomB, w, h, "Sfeerpakket bloom B");
}

bool build_bloom_pipelines() {
    WGPUShaderModule module = create_shader_module("Sfeerpakket bloom", g_bloomSource);
    if (module == nullptr) {
        return false;
    }
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.compute.module = module;
    desc.label = {"Sfeerpakket bloom down", WGPU_STRLEN};
    desc.compute.entryPoint = {"down", WGPU_STRLEN};
    g_bloomDownPipeline = wgpuDeviceCreateComputePipeline(g_deviceInfo.device, &desc);
    desc.label = {"Sfeerpakket bloom blur", WGPU_STRLEN};
    desc.compute.entryPoint = {"blur", WGPU_STRLEN};
    g_bloomBlurPipeline = wgpuDeviceCreateComputePipeline(g_deviceInfo.device, &desc);
    wgpuShaderModuleRelease(module);
    if (g_bloomDownPipeline == nullptr || g_bloomBlurPipeline == nullptr) {
        return false;
    }
    g_bloomDownLayout = wgpuComputePipelineGetBindGroupLayout(g_bloomDownPipeline, 0);
    g_bloomBlurLayout = wgpuComputePipelineGetBindGroupLayout(g_bloomBlurPipeline, 0);
    return g_bloomDownLayout != nullptr && g_bloomBlurLayout != nullptr;
}

WGPUBindGroupEntry texture_entry(uint32_t binding, WGPUTextureView view) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.textureView = view;
    return e;
}

WGPUBindGroupEntry uniform_entry(uint32_t binding, WGPUBuffer buffer, uint32_t offset, uint32_t size) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.buffer = buffer;
    e.offset = offset;
    e.size = size;
    return e;
}

WGPUBindGroup make_bind_group(
    WGPUDevice device, WGPUBindGroupLayout layout, const WGPUBindGroupEntry* entries, size_t n) {
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = layout;
    desc.entryCount = n;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(device, &desc);
}

// Render-thread: fullscreen-driehoek met (textuur, uniforms) of alleen uniforms (deeltjes).
enum class Extra { None, Sampler, SecondView };

void draw_with(const GfxDrawContext* ctx, PipelineSlot& slot, const DrawPayload& data,
    uint32_t vertexCount, uint32_t instanceCount, Extra extra = Extra::None) {
    WGPUBindGroupEntry entries[4];
    size_t n = 0;
    if (data.view != nullptr) {
        entries[n++] = texture_entry(0, data.view);
        entries[n++] = uniform_entry(1, ctx->uniform_buffer, data.uniform_offset, data.uniform_size);
        if (extra == Extra::Sampler) {
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = 2;
            e.sampler = g_linearSampler;
            entries[n++] = e;
        } else if (extra == Extra::SecondView) {
            // Zonder diepte binden we de kleur nogmaals (de shader weet het via has_depth).
            entries[n++] = texture_entry(2, data.view2 != nullptr ? data.view2 : data.view);
            entries[n++] = texture_entry(3, data.view3 != nullptr ? data.view3 : data.view);
        }
    } else {
        entries[n++] = uniform_entry(0, ctx->uniform_buffer, data.uniform_offset, data.uniform_size);
    }
    WGPUBindGroup bindGroup = make_bind_group(ctx->device, slot.layout, entries, n);
    if (bindGroup == nullptr) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, slot.pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, vertexCount, instanceCount, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

bool read_payload(const void* payload, size_t size, DrawPayload& out) {
    if (size != sizeof(DrawPayload)) {
        return false;
    }
    std::memcpy(&out, payload, sizeof(out));
    return true;
}

void on_grade_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.view != nullptr &&
        ensure_pipeline(g_gradePipeline, ctx->layout, "Sfeerpakket grade", g_gradeSource,
            Blend::Replace, false))
    {
        draw_with(ctx, g_gradePipeline, data, 3, 1, Extra::SecondView);
    }
}

void on_fxaa_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.view != nullptr && g_linearSampler != nullptr &&
        ensure_pipeline(g_fxaaPipeline, ctx->layout, "Sfeerpakket fxaa", g_fxaaSource,
            Blend::Replace, false))
    {
        draw_with(ctx, g_fxaaPipeline, data, 3, 1, Extra::Sampler);
    }
}

void on_fog_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.view != nullptr &&
        ensure_pipeline(
            g_fogPipeline, ctx->layout, "Sfeerpakket fog", g_fogSource, Blend::Alpha, false))
    {
        draw_with(ctx, g_fogPipeline, data, 3, 1);
    }
}

void on_rays_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.view != nullptr &&
        ensure_pipeline(g_raysPipeline, ctx->layout, "Sfeerpakket rays composite",
            g_raysCompositeSource, Blend::Additive, false))
    {
        draw_with(ctx, g_raysPipeline, data, 3, 1);
    }
}

void on_particles_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.instance_count > 0 &&
        ensure_pipeline(g_particlesPipeline, ctx->layout, "Sfeerpakket particles",
            g_particlesSource, Blend::Additive, true))
    {
        data.view = nullptr;
        draw_with(ctx, g_particlesPipeline, data, 6, data.instance_count);
    }
}

void on_area_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    DrawPayload data;
    if (read_payload(payload, size, data) && data.instance_count > 0 &&
        ensure_pipeline(g_areaPipeline, ctx->layout, "Sfeerpakket area particles", g_areaSource,
            Blend::Premultiplied, true))
    {
        data.view = nullptr;
        draw_with(ctx, g_areaPipeline, data, 6, data.instance_count);
    }
}

void on_rays_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(RaysComputePayload) || g_raysComputePipeline == nullptr) {
        return;
    }
    RaysComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.depth == nullptr || data.output == nullptr) {
        return;
    }
    const WGPUBindGroupEntry entries[3] = {texture_entry(0, data.depth),
        texture_entry(1, data.output),
        uniform_entry(2, ctx->uniform_buffer, data.uniform_offset, data.uniform_size)};
    WGPUBindGroup group = make_bind_group(ctx->device, g_raysComputeLayout, entries, 3);
    if (group == nullptr) {
        return;
    }
    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"Sfeerpakket rays", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    wgpuComputePassEncoderSetPipeline(pass, g_raysComputePipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (data.width + 7) / 8, (data.height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

void on_bloom_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(BloomPayload)) {
        return;
    }
    BloomPayload data;
    std::memcpy(&data, payload, sizeof(data));
    WGPUComputePipeline pipeline = data.blur ? g_bloomBlurPipeline : g_bloomDownPipeline;
    WGPUBindGroupLayout layout = data.blur ? g_bloomBlurLayout : g_bloomDownLayout;
    if (pipeline == nullptr || data.src == nullptr || data.dst == nullptr) {
        return;
    }
    const WGPUBindGroupEntry entries[3] = {texture_entry(0, data.src), texture_entry(1, data.dst),
        uniform_entry(2, ctx->uniform_buffer, data.uniform_offset, data.uniform_size)};
    WGPUBindGroup group = make_bind_group(ctx->device, layout, entries, 3);
    if (group == nullptr) {
        return;
    }
    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (data.width + 7) / 8, (data.height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

// ---------------------------------------------------------------------------------------------
// Stage-hooks (game-thread)
// ---------------------------------------------------------------------------------------------

void copy_rgb(float out[3], const Rgb& c) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
}

void push_fxaa() {
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr || resolved.width == 0 || resolved.height == 0)
    {
        return;
    }
    FxaaUniforms u{};
    u.size[0] = static_cast<float>(resolved.width);
    u.size[1] = static_cast<float>(resolved.height);
    u.inv_size[0] = 1.0f / u.size[0];
    u.inv_size[1] = 1.0f / u.size[1];
    u.subpix = 0.75f;
    u.edge_threshold = 0.125f;
    u.edge_threshold_min = 0.0312f;
    u.debug_view = get_bool(g_cvarAaDebug, false) ? 1.0f : 0.0f;
    GfxRange range{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &u, sizeof(u), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.color, nullptr, nullptr, range.offset, range.size, 1};
    svc_gfx->push_draw(mod_ctx, g_fxaaDrawType, &payload, sizeof(payload));
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    ++g_frameCounter;
    // 1. Anti-aliasing: schrijft over de scene; de volgende opname ziet het resultaat.
    g_diag.aa = false;
    g_diag.grade = false;
    if (get_bool(g_cvarAaEnabled, true)) {
        push_fxaa();
        g_diag.aa = true;
    }
    if (!get_bool(g_cvarGradeEnabled, true)) {
        return;
    }
    // 2. Verscherping + kleur + lens in één pass.
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr || resolved.width == 0 || resolved.height == 0)
    {
        return;
    }

    // Gloed: drempel + verkleinen, dan horizontaal en verticaal vervagen.
    WGPUTextureView bloomView = nullptr;
    float bloomStrength = get_percent(g_cvarBloom, 70, 300);
    g_diag.bloom = false;
    if (bloomStrength > 0.001f && g_bloomDownPipeline != nullptr) {
        const uint32_t bw = std::max(resolved.width / 4, 1u);
        const uint32_t bh = std::max(resolved.height / 4, 1u);
        if (ensure_bloom_targets(bw, bh)) {
            auto push_bloom = [&](WGPUTextureView src, WGPUTextureView dst, bool blur, float dx,
                                  float dy) -> bool {
                BloomUniforms bu{};
                bu.src_size[0] = blur ? static_cast<float>(bw) : static_cast<float>(resolved.width);
                bu.src_size[1] = blur ? static_cast<float>(bh) : static_cast<float>(resolved.height);
                bu.dst_size[0] = static_cast<float>(bw);
                bu.dst_size[1] = static_cast<float>(bh);
                bu.dir[0] = dx;
                bu.dir[1] = dy;
                bu.threshold = get_percent(g_cvarBloomThreshold, 55, 100);
                bu.knee = 0.25f;
                GfxRange r{0, 0};
                if (svc_gfx->push_uniform(mod_ctx, &bu, sizeof(bu), &r) != MOD_OK) {
                    return false;
                }
                const BloomPayload bp{src, dst, r.offset, r.size, bw, bh, blur ? 1u : 0u};
                return svc_gfx->push_compute(mod_ctx, g_bloomComputeType, &bp, sizeof(bp)) == MOD_OK;
            };
            if (push_bloom(resolved.color, g_bloomA.view, false, 0, 0) &&
                push_bloom(g_bloomA.view, g_bloomB.view, true, 1, 0) &&
                push_bloom(g_bloomB.view, g_bloomA.view, true, 0, 1))
            {
                bloomView = g_bloomA.view;
                g_diag.bloom = true;
            }
        }
    }

    const AreaProfile& p = g_current;
    GradeUniforms u{};
    copy_rgb(u.shadow_tint, p.shadowTint);
    copy_rgb(u.highlight_tint, p.highlightTint);
    u.exposure = p.exposure;
    u.contrast = p.contrast;
    u.size[0] = static_cast<float>(resolved.width);
    u.size[1] = static_cast<float>(resolved.height);
    u.saturation = p.saturation;
    // Bij lage zon is het licht van de game al warm: dan minder extra warmte.
    const float sunHigh = std::clamp(g_sunElevation / 0.45f, 0.0f, 1.0f);
    u.temperature = p.temperature > 0.0f ? p.temperature * lerpf(0.25f, 1.0f, sunHigh) : p.temperature;
    u.tint = p.tint;
    u.filmic = p.filmic;
    u.vignette = get_percent(g_cvarVignette, 25, 100);
    u.strength = get_percent(g_cvarGradeStrength, 100, 150);
    u.input_is_srgb = is_srgb_format(resolved.color_format) ? 1.0f : 0.0f;
    u.frame = static_cast<float>(g_frameCounter % 1024u);
    u.debug_split = get_bool(g_cvarSplitView, false) ? 1.0f : 0.0f;
    u.sharpen = get_percent(g_cvarSharpen, 40, 100);
    u.vibrance = get_percent(g_cvarVibrance, 35, 100);
    u.grain = get_percent(g_cvarGrain, 0, 100) * 0.08f;
    u.aspect = u.size[0] / std::max(u.size[1], 1.0f);
    const bool sunFresh = g_screenSun.valid && g_screenSun.frame + 2 >= g_frameCounter;
    if (sunFresh && get_bool(g_cvarFlareEnabled, true) && g_target != &kIndoor &&
        g_target != &kTwilight)
    {
        u.sun_uv[0] = g_screenSun.uv[0];
        u.sun_uv[1] = g_screenSun.uv[1];
        u.flare = g_screenSun.strength * get_percent(g_cvarFlareStrength, 100, 300);
        u.sun_color[0] = g_screenSun.color[0];
        u.sun_color[1] = g_screenSun.color[1];
        u.sun_color[2] = g_screenSun.color[2];
        u.sky_depth = g_screenSun.skyDepth;
    }
    u.reversed_z = g_deviceInfo.uses_reversed_z ? 1.0f : 0.0f;
    u.has_depth = resolved.depth != nullptr ? 1.0f : 0.0f;
    if (bloomView != nullptr) {
        u.bloom_size[0] = static_cast<float>(g_bloomA.width);
        u.bloom_size[1] = static_cast<float>(g_bloomA.height);
        u.bloom = bloomStrength;
    }
    u.lift = get_percent(g_cvarLift, 10, 100);
    g_diag.grade = true;

    GfxRange range{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &u, sizeof(u), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.color, resolved.depth, bloomView, range.offset, range.size, 1};
    svc_gfx->push_draw(mod_ctx, g_gradeDrawType, &payload, sizeof(payload));
}

void push_fog(const CameraInfo& camera, const GfxResolvedTargets& resolved,
    const dScnKy_env_light_c* env, const SunState& sun, float skyDepth) {
    const AreaProfile& p = g_current;
    FogUniforms u{};
    std::memcpy(u.world_from_proj, camera.world_from_proj, sizeof(u.world_from_proj));
    std::memcpy(u.eye, camera.eye, sizeof(u.eye));
    u.density = p.density * get_percent(g_cvarFogDensity, 100, 400);
    u.height_falloff = p.falloff;
    u.base_height = g_smoothedPlayerY + p.baseOffset;
    u.start_distance = p.startDist;
    u.aerial_distance = p.aerialDist;
    u.max_opacity = p.maxOpacity;
    u.sky_distance = p.aerialDist * 1.5f;
    u.reversed_z = g_deviceInfo.uses_reversed_z ? 1.0f : 0.0f;
    u.size[0] = static_cast<float>(resolved.width);
    u.size[1] = static_cast<float>(resolved.height);
    u.output_srgb = is_srgb_format(g_deviceInfo.color_format) ? 1.0f : 0.0f;
    u.debug_view = get_bool(g_cvarFogDebug, false) ? 1.0f : 0.0f;
    u.sky_depth = skyDepth;
    u.sky_dist = sky_distance(camera);

    // Mistkleur = de (al door ons getinte) mistkleur van de game; verre nevel = de luchtkleur
    // van de game met de profieltint. Zo lopen beide mee met tijd van de dag en weer.
    u.fog_color[0] = srgb8_to_linear(env->fog_col.r);
    u.fog_color[1] = srgb8_to_linear(env->fog_col.g);
    u.fog_color[2] = srgb8_to_linear(env->fog_col.b);
    u.aerial_color[0] = std::min(srgb8_to_linear(env->vrbox_sky_col.r) * p.aerial.r, 1.0f);
    u.aerial_color[1] = std::min(srgb8_to_linear(env->vrbox_sky_col.g) * p.aerial.g, 1.0f);
    u.aerial_color[2] = std::min(srgb8_to_linear(env->vrbox_sky_col.b) * p.aerial.b, 1.0f);

    std::memcpy(u.sun_dir, sun.dir, sizeof(u.sun_dir));
    std::memcpy(u.sun_color, sun.color, sizeof(u.sun_color));
    u.sun_glow = p.sunGlow * get_percent(g_cvarSunGlow, 100, 300);

    GfxRange range{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &u, sizeof(u), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.depth, nullptr, nullptr, range.offset, range.size, 1};
    g_diag.fog = svc_gfx->push_draw(mod_ctx, g_fogDrawType, &payload, sizeof(payload)) == MOD_OK;
}

void push_rays(const CameraInfo& camera, const GfxResolvedTargets& resolved, const SunState& sun,
    float skyDepth) {
    const float strength = g_current.rays * get_percent(g_cvarRaysIntensity, 100, 300);
    if (strength <= 0.001f) {
        g_diag.rays = "uit in dit gebied";
        return;
    }
    // Zonpositie op het scherm: richting (w = 0) door proj_from_world.
    const float dir[4] = {sun.dir[0], sun.dir[1], sun.dir[2], 0.0f};
    float clip[4];
    mat_mul_vec4(camera.proj_from_world, dir, clip);
    float viewDir[4];
    mat_mul_vec4(camera.view_from_world, dir, viewDir);
    const float facing = -viewDir[2];  // cos(hoek tussen kijkrichting en zon)
    if (facing <= 0.02f || clip[3] <= 1e-5f) {
        g_diag.rays = "zon achter de camera";
        return;
    }
    const float sunU = clip[0] / clip[3] * 0.5f + 0.5f;
    const float sunV = 0.5f - clip[1] / clip[3] * 0.5f;
    // Afzwakken als de zon ver buiten beeld staat of laag aan de horizon.
    const float outside = std::max({0.0f, -sunU, sunU - 1.0f, -sunV, sunV - 1.0f});
    // Ook met de zon (ruim) buiten beeld vallen er stralen het beeld in.
    const float screenFade = 1.0f - std::clamp(outside / 1.6f, 0.0f, 1.0f);
    const float facingFade = std::clamp((facing - 0.02f) / 0.30f, 0.0f, 1.0f);
    const float fade = screenFade * facingFade;
    const float sunBrightness = std::max({sun.color[0], sun.color[1], sun.color[2]});
    if (fade <= 0.001f || sunBrightness <= 0.001f) {
        g_diag.rays = sunBrightness <= 0.001f ? "zon onder" : "zon te ver buiten beeld";
        return;
    }

    const uint32_t w = std::max(resolved.width / 2, 1u);
    const uint32_t h = std::max(resolved.height / 2, 1u);
    if (!ensure_rays_target(w, h)) {
        return;
    }
    static const float kSamples[] = {24.0f, 40.0f, 64.0f};
    RaysComputeUniforms cu{};
    cu.sun_uv[0] = sunU;
    cu.sun_uv[1] = sunV;
    cu.out_size[0] = static_cast<float>(w);
    cu.out_size[1] = static_cast<float>(h);
    cu.depth_size[0] = static_cast<float>(resolved.width);
    cu.depth_size[1] = static_cast<float>(resolved.height);
    cu.sky_depth = skyDepth;
    cu.reversed_z = g_deviceInfo.uses_reversed_z ? 1.0f : 0.0f;
    cu.samples = kSamples[std::clamp<int64_t>(get_int(g_cvarRaysQuality, 2), 0, 2)];
    cu.decay = 0.975f;
    cu.density = 1.0f;
    cu.weight = 1.0f;
    GfxRange computeRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &cu, sizeof(cu), &computeRange) != MOD_OK) {
        return;
    }
    const RaysComputePayload cp{resolved.depth, g_raysTarget.view, computeRange.offset,
        computeRange.size, w, h};
    if (svc_gfx->push_compute(mod_ctx, g_raysComputeType, &cp, sizeof(cp)) != MOD_OK) {
        return;
    }

    RaysCompositeUniforms ru{};
    const float scale = strength * fade;
    ru.sun_color[0] = linear_to_display(sun.color[0]);
    ru.sun_color[1] = linear_to_display(sun.color[1]);
    ru.sun_color[2] = linear_to_display(sun.color[2]);
    ru.intensity = scale * 1.8f;
    ru.rays_size[0] = static_cast<float>(w);
    ru.rays_size[1] = static_cast<float>(h);
    ru.sun_uv[0] = sunU;
    ru.sun_uv[1] = sunV;
    ru.aspect = static_cast<float>(resolved.width) / static_cast<float>(std::max(resolved.height, 1u));
    ru.falloff = 1.2f;
    ru.debug_view = get_bool(g_cvarRaysDebug, false) ? 1.0f : 0.0f;
    GfxRange drawRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &ru, sizeof(ru), &drawRange) != MOD_OK) {
        return;
    }
    const DrawPayload payload{g_raysTarget.view, nullptr, nullptr, drawRange.offset, drawRange.size, 1};
    if (svc_gfx->push_draw(mod_ctx, g_raysDrawType, &payload, sizeof(payload)) == MOD_OK) {
        g_diag.rays = "actief";
    }
}

void push_particles(const CameraInfo& camera, const SunState& sun) {
    const float amount = get_percent(g_cvarParticleAmount, 100, 300);
    const AreaProfile& p = g_current;
    const float flies = (sun.isNight ? p.fireflyNight : p.fireflyDay) * amount;
    const float dust = p.dust * amount * (sun.isNight ? 0.5f : 1.0f);
    const uint32_t nFly = static_cast<uint32_t>(std::clamp(flies, 0.0f, 2000.0f));
    const uint32_t nDust = static_cast<uint32_t>(std::clamp(dust, 0.0f, 4000.0f));
    if (nFly + nDust == 0) {
        return;
    }
    ParticleUniforms u{};
    std::memcpy(u.proj_from_world, camera.proj_from_world, sizeof(u.proj_from_world));
    // Rij 0 en 1 van view_from_world = rechts- en omhoog-vector van de camera in de wereld.
    const float* v = camera.view_from_world;
    u.cam_right[0] = v[0];
    u.cam_right[1] = v[4];
    u.cam_right[2] = v[8];
    u.cam_up[0] = v[1];
    u.cam_up[1] = v[5];
    u.cam_up[2] = v[9];
    std::memcpy(u.eye, camera.eye, sizeof(u.eye));
    u.time = std::fmod(
        std::chrono::duration<float>(std::chrono::steady_clock::now() - g_startTime).count(),
        3600.0f);
    u.ground_y = g_smoothedPlayerY;
    u.firefly_box = 3000.0f;
    u.dust_box = 1400.0f;
    u.firefly_color[0] = 0.80f;
    u.firefly_color[1] = 1.00f;
    u.firefly_color[2] = 0.38f;
    u.dust_color[0] = 0.85f * std::max(linear_to_display(sun.color[0]), 0.35f);
    u.dust_color[1] = 0.80f * std::max(linear_to_display(sun.color[1]), 0.35f);
    u.dust_color[2] = 0.68f * std::max(linear_to_display(sun.color[2]), 0.35f);
    u.firefly_count = static_cast<float>(nFly);
    u.dust_count = static_cast<float>(nDust);
    std::memcpy(u.sun_dir, sun.dir, sizeof(u.sun_dir));
    u.firefly_size = 9.0f;
    u.dust_size = 4.5f;
    u.firefly_height = 320.0f;
    u.brightness = get_percent(g_cvarParticleBrightness, 100, 300);

    GfxRange range{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &u, sizeof(u), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{nullptr, nullptr, nullptr, range.offset, range.size, nFly + nDust};
    if (svc_gfx->push_draw(mod_ctx, g_particlesDrawType, &payload, sizeof(payload)) == MOD_OK) {
        g_diag.particles = nFly + nDust;
    }
}

// Deeltjes per gebied: soort, aantal, blokgrootte, grootte en wind.
struct AreaKind {
    int kind;
    const char* name;
    float count;
    float box;
    float size;
    float wind[3];
    bool outdoorOnly;
};
constexpr AreaKind kAreaLeaves{1, "bladeren", 550, 1600, 7.5f, {22, 0, 12}, true};
constexpr AreaKind kAreaPetals{2, "bloemblaadjes", 380, 1600, 5.0f, {40, 0, 25}, true};
constexpr AreaKind kAreaPetalsField{2, "bloemblaadjes", 220, 2000, 5.0f, {70, 0, 40}, true};
constexpr AreaKind kAreaEmbers{3, "vonken", 650, 1400, 5.0f, {14, 0, 8}, false};
constexpr AreaKind kAreaSnow{4, "sneeuw", 2600, 1500, 3.5f, {70, 0, 35}, true};
constexpr AreaKind kAreaSand{5, "zand", 1600, 2000, 3.0f, {260, 0, 120}, true};
constexpr AreaKind kAreaTwilight{6, "schemer", 450, 1800, 6.5f, {0, 0, 0}, false};

const AreaKind* pick_area_kind() {
    if (dKy_darkworld_check() != 0) {
        return &kAreaTwilight;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr || stage[0] == '\0') {
        return nullptr;
    }
    auto is = [&](const char* name) { return std::strncmp(stage, name, 8) == 0; };
    const AreaKind* k = nullptr;
    if (is("F_SP108") || is("F_SP117")) k = &kAreaLeaves;
    else if (is("F_SP103") || is("F_SP00") || is("F_SP104")) k = &kAreaPetals;
    else if (is("F_SP121") || is("F_SP122")) k = &kAreaPetalsField;
    else if (is("F_SP110") || is("D_MN04") || is("R_SP110")) k = &kAreaEmbers;
    else if (is("F_SP114")) k = &kAreaSnow;
    else if (is("F_SP124")) k = &kAreaSand;
    if (k != nullptr && k->outdoorOnly) {
        dStage_stageDt_c* stageDt = dComIfGp_getStage();
        if (stageDt == nullptr || stageDt->getStagInfo() == nullptr || !dKy_Outdoor_check()) {
            return nullptr;
        }
    }
    return k;
}

void push_area_particles(const CameraInfo& camera, const SunState& sun, dScnKy_env_light_c* env) {
    const AreaKind* k = pick_area_kind();
    if (k == nullptr) {
        return;
    }
    const float amount = get_percent(g_cvarAreaAmount, 100, 300);
    const uint32_t n = static_cast<uint32_t>(std::clamp(k->count * amount, 0.0f, 6000.0f));
    if (n == 0) {
        return;
    }
    AreaParticleUniforms u{};
    std::memcpy(u.proj_from_world, camera.proj_from_world, sizeof(u.proj_from_world));
    const float* v = camera.view_from_world;
    u.cam_right[0] = v[0];
    u.cam_right[1] = v[4];
    u.cam_right[2] = v[8];
    u.cam_up[0] = v[1];
    u.cam_up[1] = v[5];
    u.cam_up[2] = v[9];
    std::memcpy(u.eye, camera.eye, sizeof(u.eye));
    u.time = std::fmod(
        std::chrono::duration<float>(std::chrono::steady_clock::now() - g_startTime).count(),
        3600.0f);
    u.count = static_cast<float>(n);
    u.kind = static_cast<float>(k->kind);
    // Licht in beeldruimte: omgevingslicht van de game plus een deel zonlicht.
    const GXColorS10& amb = env->actor_amb_col;
    const float ambC[3] = {amb.r / 255.0f, amb.g / 255.0f, amb.b / 255.0f};
    for (int i = 0; i < 3; ++i) {
        u.light[i] = std::clamp(ambC[i] * 0.9f + linear_to_display(sun.color[i]) * 0.55f, 0.08f, 1.4f);
    }
    u.box = k->box;
    std::memcpy(u.wind, k->wind, sizeof(u.wind));
    u.size = k->size;
    std::memcpy(u.sun_dir, sun.dir, sizeof(u.sun_dir));
    u.intensity = get_percent(g_cvarAreaIntensity, 100, 300);

    GfxRange range{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &u, sizeof(u), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{nullptr, nullptr, nullptr, range.offset, range.size, n};
    if (svc_gfx->push_draw(mod_ctx, g_areaDrawType, &payload, sizeof(payload)) == MOD_OK) {
        g_diag.area = k->name;
        g_diag.areaCount = n;
    }
}

// Zonpositie op het scherm voor de lens-effecten in de kleurpass.
void update_screen_sun(const CameraInfo& camera, const SunState& sun) {
    g_sunElevation = sun.isNight ? 1.0f : std::max(sun.dir[1], 0.0f);
    const float dir[4] = {sun.dir[0], sun.dir[1], sun.dir[2], 0.0f};
    float clip[4];
    mat_mul_vec4(camera.proj_from_world, dir, clip);
    float viewDir[4];
    mat_mul_vec4(camera.view_from_world, dir, viewDir);
    const float facing = -viewDir[2];
    g_screenSun.valid = false;
    if (facing <= 0.05f || clip[3] <= 1e-5f || sun.isNight) {
        return;
    }
    const float su = clip[0] / clip[3] * 0.5f + 0.5f;
    const float sv = 0.5f - clip[1] / clip[3] * 0.5f;
    const float outside = std::max({0.0f, -su, su - 1.0f, -sv, sv - 1.0f});
    const float fade = (1.0f - std::clamp(outside / 0.3f, 0.0f, 1.0f)) *
                       std::clamp((facing - 0.05f) / 0.35f, 0.0f, 1.0f);
    const float bright = std::max({sun.color[0], sun.color[1], sun.color[2]});
    g_screenSun.uv[0] = su;
    g_screenSun.uv[1] = sv;
    g_screenSun.strength = fade * std::min(bright, 1.0f);
    g_screenSun.color[0] = linear_to_display(sun.color[0]);
    g_screenSun.color[1] = linear_to_display(sun.color[1]);
    g_screenSun.color[2] = linear_to_display(sun.color[2]);
    g_screenSun.skyDepth = sky_depth_threshold(camera, g_deviceInfo.uses_reversed_z);
    g_screenSun.frame = g_frameCounter;
    g_screenSun.valid = g_screenSun.strength > 0.001f;
}

void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired_targets();
    g_diag.fog = false;
    g_diag.rays = "-";
    g_diag.particles = 0;
    g_diag.depth = false;
    g_diag.area = "-";
    g_diag.areaCount = 0;
    const bool fogOn = get_bool(g_cvarFogEnabled, true);
    const bool raysOn = get_bool(g_cvarRaysEnabled, true);
    const bool particlesOn = get_bool(g_cvarParticlesEnabled, true);
    const bool areaOn = get_bool(g_cvarAreaEnabled, true);
    if ((!fogOn && !raysOn && !particlesOn && !areaOn) || stageCtx == nullptr ||
        stageCtx->struct_size < sizeof(GfxStageContext) || stageCtx->game_view == nullptr)
    {
        return;
    }
    CameraInfo camera = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &camera) != MOD_OK) {
        return;
    }
    dScnKy_env_light_c* env = dKy_getEnvlight();
    if (env == nullptr) {
        return;
    }
    const SunState sun = compute_sun();
    update_screen_sun(camera, sun);

    if (fogOn || raysOn) {
        GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
        resolveDesc.color = false;
        resolveDesc.depth = true;
        GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) == MOD_OK &&
            resolved.depth != nullptr)
        {
            const float skyDepth = sky_depth_threshold(camera, g_deviceInfo.uses_reversed_z);
            if (fogOn) {
                push_fog(camera, resolved, env, sun, skyDepth);
            }
            if (raysOn) {
                push_rays(camera, resolved, sun, skyDepth);
            }
            g_diag.depth = true;
        } else if (!g_warnedFogUnavailable) {
            g_warnedFogUnavailable = true;
            svc_log->warn(mod_ctx, "geen dieptebuffer beschikbaar; mist en lichtstralen staan uit");
        }
    }
    if (particlesOn) {
        push_particles(camera, sun);
    }
    if (areaOn) {
        push_area_particles(camera, sun, env);
    }
}

// ---------------------------------------------------------------------------------------------
// Fotomodus: vrije camera, wereld bevroren, HUD weg. P schakelt, of via het paneel.
// ---------------------------------------------------------------------------------------------

using SdlKeyboardStateFn = const bool* (*)(int*);
using SdlMouseStateFn = uint32_t (*)(float*, float*);
SdlKeyboardStateFn g_sdlKeys = nullptr;
SdlMouseStateFn g_sdlMouse = nullptr;
CameraOperatorHandle g_photoOperator = 0;

struct PhotoState {
    bool active = false;
    bool frozeWorld = false;
    bool keyWasDown = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float eye[3] = {0, 0, 0};
    float lastMouse[2] = {-1.0f, -1.0f};
};
PhotoState g_photo;

constexpr int kScP = 19, kScW = 26, kScA = 4, kScS = 22, kScD = 7, kScSpace = 44, kScLCtrl = 224,
              kScLShift = 225, kScQ = 20, kScE = 8;

bool key_down(const bool* keys, int numKeys, int sc) {
    return keys != nullptr && sc < numKeys && keys[sc];
}

void photo_set_freeze(bool freeze) {
    if (freeze && !g_photo.frozeWorld && !g_dComIfAc_gameInfo.mPause) {
        g_dComIfAc_gameInfo.mPause = true;
        g_photo.frozeWorld = true;
    } else if (!freeze && g_photo.frozeWorld) {
        g_dComIfAc_gameInfo.mPause = false;
        g_photo.frozeWorld = false;
    }
}

void photo_leave() {
    photo_set_freeze(false);
    g_photo.active = false;
    g_photo.lastMouse[0] = g_photo.lastMouse[1] = -1.0f;
}

bool photo_operate(ModContext*, CameraOperatorState* st, void*) {
    int numKeys = 0;
    const bool* keys = g_sdlKeys != nullptr ? g_sdlKeys(&numKeys) : nullptr;

    // P schakelt de fotomodus (op de flank, niet zolang je hem vasthoudt).
    const bool pDown = key_down(keys, numKeys, kScP);
    if (pDown && !g_photo.keyWasDown) {
        svc_config->set_bool(mod_ctx, g_cvarPhoto, !get_bool(g_cvarPhoto, false));
    }
    g_photo.keyWasDown = pDown;

    const bool want = get_bool(g_cvarPhoto, false);
    if (!want) {
        if (g_photo.active) {
            photo_leave();
        }
        g_diag.photo = false;
        return false;
    }
    if (!g_photo.active) {
        g_photo.active = true;
        std::memcpy(g_photo.eye, st->eye, sizeof(g_photo.eye));
        const float dx = st->center[0] - st->eye[0];
        const float dy = st->center[1] - st->eye[1];
        const float dz = st->center[2] - st->eye[2];
        g_photo.yaw = std::atan2(dz, dx);
        g_photo.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    }
    g_diag.photo = true;
    photo_set_freeze(get_bool(g_cvarPhotoFreeze, true));

    float fwd = 0, side = 0, up = 0, lookX = 0, lookY = 0;
    bool fast = false;
    if (keys != nullptr) {
        fwd = (key_down(keys, numKeys, kScW) ? 1.f : 0.f) - (key_down(keys, numKeys, kScS) ? 1.f : 0.f);
        side = (key_down(keys, numKeys, kScD) ? 1.f : 0.f) - (key_down(keys, numKeys, kScA) ? 1.f : 0.f);
        up = (key_down(keys, numKeys, kScSpace) || key_down(keys, numKeys, kScE) ? 1.f : 0.f) -
             (key_down(keys, numKeys, kScLCtrl) || key_down(keys, numKeys, kScQ) ? 1.f : 0.f);
        fast = key_down(keys, numKeys, kScLShift);
    } else {
        interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(0);
        fwd = pad.mMainStickPosY;
        side = pad.mMainStickPosX;
        up = pad.mTriggerRight - pad.mTriggerLeft;
    }
    // Kijken: pijltjes / C-stick altijd, muis met rechterknop ingedrukt.
    interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(0);
    lookX = pad.mCStickPosX * 0.045f;
    lookY = pad.mCStickPosY * 0.035f;
    if (g_sdlMouse != nullptr) {
        float mx = 0, my = 0;
        const uint32_t buttons = g_sdlMouse(&mx, &my);
        if ((buttons & (1u << 2)) != 0 || (buttons & 1u) != 0) {
            if (g_photo.lastMouse[0] >= 0.0f) {
                lookX += (mx - g_photo.lastMouse[0]) * 0.004f;
                lookY -= (my - g_photo.lastMouse[1]) * 0.004f;
            }
            g_photo.lastMouse[0] = mx;
            g_photo.lastMouse[1] = my;
        } else {
            g_photo.lastMouse[0] = g_photo.lastMouse[1] = -1.0f;
        }
    }
    g_photo.yaw += lookX;
    g_photo.pitch = std::clamp(g_photo.pitch + lookY, -1.45f, 1.45f);

    const float speed = 14.0f * get_percent(g_cvarPhotoSpeed, 100, 500) * (fast ? 4.0f : 1.0f);
    const float cy = std::cos(g_photo.yaw), sy = std::sin(g_photo.yaw);
    const float cp = std::cos(g_photo.pitch), sp = std::sin(g_photo.pitch);
    g_photo.eye[0] += speed * (fwd * cy * cp - side * sy);
    g_photo.eye[1] += speed * (fwd * sp + up);
    g_photo.eye[2] += speed * (fwd * sy * cp + side * cy);

    std::memcpy(st->eye, g_photo.eye, sizeof(g_photo.eye));
    st->center[0] = g_photo.eye[0] + cy * cp * 100.0f;
    st->center[1] = g_photo.eye[1] + sp * 100.0f;
    st->center[2] = g_photo.eye[2] + sy * cp * 100.0f;
    st->bank_degrees = 0.0f;
    const float fov = get_percent(g_cvarPhotoFov, 0, 120) * 100.0f;
    if (fov >= 10.0f) {
        st->fovy = fov;
    }
    const float hour = get_percent(g_cvarPhotoHour, 0, 24) * 100.0f;
    if (hour >= 0.5f) {
        dComIfGs_setTime(std::fmod(hour, 24.0f) * 15.0f);
    }
    return true;
}

HookAction on_meter_draw_pre(ModContext*, void*, void* retval, void*) {
    if (g_photo.active && !get_bool(g_cvarPhotoHud, false)) {
        if (retval != nullptr) {
            *static_cast<int*>(retval) = 1;
        }
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

UiElementHandle g_statusText = 0;
UiElementHandle g_dumpText = 0;
UiElementHandle g_diagText = 0;

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle var) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = label;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle var, int64_t min,
    int64_t max, int64_t step, const char* suffix = "%") {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = label;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    c.min = min;
    c.max = max;
    c.step = step;
    c.suffix = suffix;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Status");
    svc_ui->pane_add_text(mod_ctx, panel, "Gebied: ...", &g_statusText);
    svc_ui->pane_add_text(mod_ctx, panel, "Effecten: ...", &g_diagText);

    svc_ui->pane_add_section(mod_ctx, panel, "Beeldscherpte");
    add_toggle(panel, "Anti-aliasing (gladde randen)", g_cvarAaEnabled);
    add_number(panel, "Verscherping", g_cvarSharpen, 0, 100, 5);
    add_toggle(panel, "Toon AA-randen (debug)", g_cvarAaDebug);

    svc_ui->pane_add_section(mod_ctx, panel, "Gloed en licht");
    add_number(panel, "Gloed (bloom)", g_cvarBloom, 0, 300, 10);
    add_number(panel, "Gloed-drempel", g_cvarBloomThreshold, 0, 100, 5);
    add_number(panel, "Schaduwen optillen", g_cvarLift, 0, 100, 5);

    svc_ui->pane_add_section(mod_ctx, panel, "Lens");
    add_toggle(panel, "Lensflare en zongloed", g_cvarFlareEnabled);
    add_number(panel, "Flare-sterkte", g_cvarFlareStrength, 0, 300, 10);
    add_number(panel, "Filmkorrel", g_cvarGrain, 0, 100, 5);

    svc_ui->pane_add_section(mod_ctx, panel, "Kleurstemming");
    add_toggle(panel, "Aan", g_cvarGradeEnabled);
    add_number(panel, "Vibrance", g_cvarVibrance, 0, 100, 5);
    add_number(panel, "Sterkte", g_cvarGradeStrength, 0, 150, 5);
    add_number(panel, "Vignet", g_cvarVignette, 0, 100, 5);
    add_toggle(panel, "Voor/na-vergelijking (links origineel)", g_cvarSplitView);

    svc_ui->pane_add_section(mod_ctx, panel, "Atmosferische mist");
    add_toggle(panel, "Aan", g_cvarFogEnabled);
    add_number(panel, "Dichtheid", g_cvarFogDensity, 0, 300, 10);
    add_number(panel, "Zonnegloed", g_cvarSunGlow, 0, 300, 10);
    add_toggle(panel, "Toon alleen mist (debug)", g_cvarFogDebug);

    svc_ui->pane_add_section(mod_ctx, panel, "Lichtstralen");
    add_toggle(panel, "Aan", g_cvarRaysEnabled);
    add_number(panel, "Sterkte", g_cvarRaysIntensity, 0, 300, 10);
    add_number(panel, "Kwaliteit (0-2)", g_cvarRaysQuality, 0, 2, 1, "");
    add_toggle(panel, "Toon alleen stralen (debug)", g_cvarRaysDebug);

    svc_ui->pane_add_section(mod_ctx, panel, "Vuurvliegjes en stof");
    add_toggle(panel, "Aan", g_cvarParticlesEnabled);
    add_number(panel, "Hoeveelheid", g_cvarParticleAmount, 0, 300, 10);
    add_number(panel, "Helderheid", g_cvarParticleBrightness, 0, 300, 10);

    svc_ui->pane_add_section(mod_ctx, panel, "Deeltjes per gebied");
    add_toggle(panel, "Aan (bladeren, bloemblaadjes, vonken, sneeuw, zand, schemer)", g_cvarAreaEnabled);
    add_number(panel, "Hoeveelheid", g_cvarAreaAmount, 0, 300, 10);
    add_number(panel, "Gloed en tegenlicht", g_cvarAreaIntensity, 0, 300, 10);

    svc_ui->pane_add_section(mod_ctx, panel, "Fotomodus (toets P)");
    add_toggle(panel, "Fotomodus aan", g_cvarPhoto);
    add_toggle(panel, "Wereld bevriezen", g_cvarPhotoFreeze);
    add_toggle(panel, "HUD tonen", g_cvarPhotoHud);
    add_number(panel, "Vliegsnelheid", g_cvarPhotoSpeed, 10, 500, 10);
    add_number(panel, "Beeldhoek (0 = normaal)", g_cvarPhotoFov, 0, 120, 5, "");
    add_number(panel, "Tijdstip (0 = niet aanpassen)", g_cvarPhotoHour, 0, 24, 1, "");
    svc_ui->pane_add_text(mod_ctx, panel,
        "WASD vliegen, spatie/E omhoog, Ctrl/Q omlaag, Shift snel, muisknop ingedrukt of pijltjes om te kijken.",
        nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Gras en bloemen (na kamerwissel)");
    add_toggle(panel, "Aan", g_cvarGrassEnabled);
    add_number(panel, "Grasdichtheid", g_cvarGrassDensity, 100, 400, 25);
    add_number(panel, "Bloemdichtheid", g_cvarFlowerDensity, 100, 400, 25);

    svc_ui->pane_add_section(mod_ctx, panel, "Toetsenbord");
    add_toggle(panel, "Standaard toetsindeling als er nog geen eigen is", g_cvarKeyboardDefaults);
    svc_ui->pane_add_text(mod_ctx, panel,
        "WASD lopen, muis camera, spatie actie, linkermuis zwaard, rechtermuis richten, "
        "Shift schild, Q/E voorwerpen, F Midna, Tab kaart, I voorwerpen, Enter pauze",
        nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Gereedschap: HD-textures");
    add_toggle(panel, "Textures dumpen (voor tools/hd_textures.py)", g_cvarTextureDump);
    svc_ui->pane_add_text(mod_ctx, panel, "Dumps: ...", &g_dumpText);

    svc_ui->pane_add_section(mod_ctx, panel, "Licht per gebied");
    add_toggle(panel, "Aan", g_cvarLightingEnabled);
    add_number(panel, "Sterkte", g_cvarLightingStrength, 0, 200, 10);
    add_number(panel, "Overgangssnelheid", g_cvarTransitionSpeed, 1, 100, 1);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_statusText != 0) {
        char text[128];
        std::snprintf(text, sizeof(text), "Gebied: %s  |  extra graspollen: %d",
            g_target ? g_target->name : "-", g_grassExtraTotal);
        svc_ui->elem_set_text(mod_ctx, g_statusText, text);
    }
    if (g_diagText != 0) {
        char text[320];
        std::snprintf(text, sizeof(text),
            "Mist: %s | Stralen: %s | Deeltjes: %u + %s %u | Gloed: %s | AA: %s | Kleur: %s | Diepte: %s%s",
            g_diag.fog ? "aan" : "uit", g_diag.rays, g_diag.particles, g_diag.area, g_diag.areaCount,
            g_diag.bloom ? "aan" : "uit",
            g_diag.aa ? "aan" : "uit", g_diag.grade ? "aan" : "uit", g_diag.depth ? "ja" : "nee", g_diag.photo ? " | FOTOMODUS" : "");
        svc_ui->elem_set_text(mod_ctx, g_diagText, text);
    }
    if (g_dumpText != 0) {
        char text[640];
        if (find_aurora_config() == nullptr) {
            std::snprintf(text, sizeof(text), "Niet beschikbaar in deze Dusklight-build.");
        } else if (g_dumpActive) {
            std::snprintf(text, sizeof(text), "%d bestanden in %s  (loop rond en wissel van gebied)",
                g_dumpFileCount, g_dumpDir.c_str());
        } else {
            std::snprintf(text, sizeof(text), "Uit. Map: %s", g_dumpDir.c_str());
        }
        svc_ui->elem_set_text(mod_ctx, g_dumpText, text);
    }
    return MOD_OK;
}

ModResult register_bool(const char* name, bool def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = CONFIG_VAR_BOOL;
    d.default_bool = def;
    if (svc_config->register_var(mod_ctx, &d, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "kon instelling niet registreren");
    }
    return MOD_OK;
}

ModResult register_int(const char* name, int64_t def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = CONFIG_VAR_INT;
    d.default_int = def;
    if (svc_config->register_var(mod_ctx, &d, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "kon instelling niet registreren");
    }
    return MOD_OK;
}

ModResult load_shader(const char* path, ResourceBuffer& out, ModError* error) {
    if (svc_resource->load(mod_ctx, path, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "shader ontbreekt in res/");
    }
    return MOD_OK;
}

ModResult register_draw(const char* label, GfxDrawFn fn, GfxDrawTypeHandle& out, ModError* error) {
    GfxDrawTypeDesc desc = GFX_DRAW_TYPE_DESC_INIT;
    desc.label = label;
    desc.draw = fn;
    if (svc_gfx->register_draw_type(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "kon draw type niet registreren");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r = MOD_OK;
#define TRY(expr)                                                                                  \
    if ((r = (expr)) != MOD_OK) return r
    TRY(register_bool("gradeEnabled", true, g_cvarGradeEnabled, error));
    TRY(register_int("gradeStrength", 100, g_cvarGradeStrength, error));
    TRY(register_int("vignette", 25, g_cvarVignette, error));
    TRY(register_bool("splitView", false, g_cvarSplitView, error));
    TRY(register_bool("fogEnabled", true, g_cvarFogEnabled, error));
    TRY(register_int("fogDensity", 100, g_cvarFogDensity, error));
    TRY(register_int("sunGlow", 100, g_cvarSunGlow, error));
    TRY(register_bool("fogDebug", false, g_cvarFogDebug, error));
    TRY(register_bool("raysEnabled", true, g_cvarRaysEnabled, error));
    TRY(register_int("raysIntensity", 100, g_cvarRaysIntensity, error));
    TRY(register_int("raysQuality", 2, g_cvarRaysQuality, error));
    TRY(register_bool("raysDebug", false, g_cvarRaysDebug, error));
    TRY(register_bool("particlesEnabled", true, g_cvarParticlesEnabled, error));
    TRY(register_int("particleAmount", 100, g_cvarParticleAmount, error));
    TRY(register_int("particleBrightness", 100, g_cvarParticleBrightness, error));
    TRY(register_bool("areaEnabled", true, g_cvarAreaEnabled, error));
    TRY(register_int("areaAmount", 100, g_cvarAreaAmount, error));
    TRY(register_int("areaIntensity", 100, g_cvarAreaIntensity, error));
    TRY(register_bool("photoMode", false, g_cvarPhoto, error));
    TRY(register_bool("photoFreeze", true, g_cvarPhotoFreeze, error));
    TRY(register_bool("photoHud", false, g_cvarPhotoHud, error));
    TRY(register_int("photoSpeed", 100, g_cvarPhotoSpeed, error));
    TRY(register_int("photoFov", 0, g_cvarPhotoFov, error));
    TRY(register_int("photoHour", 0, g_cvarPhotoHour, error));
    TRY(register_bool("grassEnabled", true, g_cvarGrassEnabled, error));
    TRY(register_int("grassDensity", 300, g_cvarGrassDensity, error));
    TRY(register_int("flowerDensity", 250, g_cvarFlowerDensity, error));
    TRY(register_bool("textureDump", false, g_cvarTextureDump, error));
    TRY(register_bool("aaEnabled", true, g_cvarAaEnabled, error));
    TRY(register_bool("aaDebug", false, g_cvarAaDebug, error));
    TRY(register_int("sharpen", 40, g_cvarSharpen, error));
    TRY(register_int("vibrance", 35, g_cvarVibrance, error));
    TRY(register_bool("flareEnabled", true, g_cvarFlareEnabled, error));
    TRY(register_int("flareStrength", 100, g_cvarFlareStrength, error));
    TRY(register_int("grain", 0, g_cvarGrain, error));
    TRY(register_int("bloom", 70, g_cvarBloom, error));
    TRY(register_int("bloomThreshold", 55, g_cvarBloomThreshold, error));
    TRY(register_int("lift", 10, g_cvarLift, error));
    TRY(register_bool("keyboardDefaults", true, g_cvarKeyboardDefaults, error));
    TRY(register_bool("lightingEnabled", true, g_cvarLightingEnabled, error));
    TRY(register_int("lightingStrength", 100, g_cvarLightingStrength, error));
    TRY(register_int("transitionSpeed", 4, g_cvarTransitionSpeed, error));

    TRY(load_shader("grade.wgsl", g_gradeSource, error));
    TRY(load_shader("fxaa.wgsl", g_fxaaSource, error));
    TRY(load_shader("bloom.wgsl", g_bloomSource, error));
    TRY(load_shader("fog.wgsl", g_fogSource, error));
    TRY(load_shader("rays_compute.wgsl", g_raysComputeSource, error));
    TRY(load_shader("rays_composite.wgsl", g_raysCompositeSource, error));
    TRY(load_shader("particles.wgsl", g_particlesSource, error));
    TRY(load_shader("area_particles.wgsl", g_areaSource, error));
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "kon GPU-info niet opvragen");
    }
    {
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        g_linearSampler = wgpuDeviceCreateSampler(g_deviceInfo.device, &sd);
        if (g_linearSampler == nullptr) {
            return mods::set_error(error, MOD_ERROR, "kon sampler niet maken");
        }
    }
    if (!build_bloom_pipelines()) {
        svc_log->warn(mod_ctx, "bloom-pipelines niet beschikbaar; gloed staat uit");
    }
    if (!build_rays_compute_pipeline()) {
        return mods::set_error(error, MOD_ERROR, "kon compute-pipeline voor lichtstralen niet maken");
    }

    TRY(register_draw("Sfeerpakket grade", on_grade_draw, g_gradeDrawType, error));
    TRY(register_draw("Sfeerpakket fxaa", on_fxaa_draw, g_fxaaDrawType, error));
    TRY(register_draw("Sfeerpakket fog", on_fog_draw, g_fogDrawType, error));
    TRY(register_draw("Sfeerpakket rays", on_rays_draw, g_raysDrawType, error));
    TRY(register_draw("Sfeerpakket particles", on_particles_draw, g_particlesDrawType, error));
    TRY(register_draw("Sfeerpakket area particles", on_area_draw, g_areaDrawType, error));
#undef TRY

    {
        GfxComputeTypeDesc bloomDesc = GFX_COMPUTE_TYPE_DESC_INIT;
        bloomDesc.label = "Sfeerpakket bloom";
        bloomDesc.callback = on_bloom_compute;
        if (svc_gfx->register_compute_type(mod_ctx, &bloomDesc, &g_bloomComputeType) != MOD_OK) {
            return mods::set_error(error, MOD_ERROR, "kon bloom compute type niet registreren");
        }
    }
    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "Sfeerpakket rays";
    computeDesc.callback = on_rays_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_raysComputeType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "kon compute type niet registreren");
    }

    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_beforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "kon stage-hook niet registreren");
    }
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_afterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "kon stage-hook niet registreren");
    }

    r = mods::hook::add_post<EnvSetLight>(on_set_light_post);
    if (r != MOD_OK) {
        return mods::set_error(error, r, "kon setLight-hook niet installeren");
    }
    // Gras is optioneel: als het symbool in een andere Dusklight-versie anders heet, draait
    // de rest gewoon door.
    if (mods::hook::add_pre<GrassCreate>(on_grass_create_pre) != MOD_OK ||
        mods::hook::add_post<GrassCreate>(on_grass_create_post) != MOD_OK)
    {
        svc_log->warn(mod_ctx, "gras-hook niet beschikbaar; extra gras staat uit");
    }

    if (mods::hook::add_pre<MeterDraw>(on_meter_draw_pre) != MOD_OK) {
        svc_log->warn(mod_ctx, "HUD-hook niet beschikbaar; HUD blijft zichtbaar in fotomodus");
    }
    g_sdlKeys = reinterpret_cast<SdlKeyboardStateFn>(dlsym(RTLD_DEFAULT, "SDL_GetKeyboardState"));
    g_sdlMouse = reinterpret_cast<SdlMouseStateFn>(dlsym(RTLD_DEFAULT, "SDL_GetMouseState"));
    if (g_sdlKeys == nullptr) {
        svc_log->warn(mod_ctx, "toetsenbord niet direct leesbaar; fotomodus via paneel en controller");
    }
    svc_config->set_bool(mod_ctx, g_cvarPhoto, false);
    {
        CameraOperatorDesc op = CAMERA_OPERATOR_DESC_INIT;
        op.debug_name = "Sfeerpakket fotomodus";
        op.priority = 100;
        op.operate = photo_operate;
        if (svc_camera->register_camera_operator == nullptr ||
            svc_camera->register_camera_operator(mod_ctx, &op, &g_photoOperator) != MOD_OK)
        {
            svc_log->warn(mod_ctx, "camera-operator niet beschikbaar; fotomodus staat uit");
        }
    }
    svc_config->subscribe(mod_ctx, g_cvarTextureDump, on_texture_dump_changed, nullptr, nullptr);
    if (get_bool(g_cvarKeyboardDefaults, true)) {
        apply_keyboard_defaults();
    }
    apply_texture_dump(get_bool(g_cvarTextureDump, false));

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    svc_log->info(mod_ctx, "Sfeerpakket 0.7 geladen (gebiedsdeeltjes + fotomodus)");
    return MOD_OK;
}

// Game-thread, elk frame: kies het gebiedsprofiel en schuif er geleidelijk naartoe.
MOD_EXPORT ModResult mod_update(ModError*) {
    count_dump_files();
    g_target = pick_profile();
    if (!g_currentInitialized) {
        g_current = *g_target;
        g_currentInitialized = true;
    } else {
        const float t = get_percent(g_cvarTransitionSpeed, 4, 100);
        blend_profile(g_current, *g_target, t);
    }

    if (fopAc_ac_c* player = dComIfGp_getPlayer(0)) {
        const float y = player->current.pos.y;
        if (!g_playerYInitialized || std::fabs(y - g_smoothedPlayerY) > 5000.0f) {
            g_smoothedPlayerY = y;  // eerste frame of warp: direct overnemen
            g_playerYInitialized = true;
        } else {
            g_smoothedPlayerY = lerpf(g_smoothedPlayerY, y, 0.02f);
        }
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_auroraConfig != nullptr) {
        g_auroraConfig->allowTextureDumps = g_dumpOriginal;  // dumpen nooit laten aanstaan
    }
    release_slot(g_gradePipeline);
    release_slot(g_fxaaPipeline);
    photo_leave();
    if (g_photoOperator != 0) {
        svc_camera->unregister_camera_operator(mod_ctx, g_photoOperator);
        g_photoOperator = 0;
    }
    release_slot(g_areaPipeline);
    svc_resource->free(mod_ctx, &g_areaSource);
    if (g_bloomDownPipeline) wgpuComputePipelineRelease(g_bloomDownPipeline);
    if (g_bloomBlurPipeline) wgpuComputePipelineRelease(g_bloomBlurPipeline);
    if (g_bloomDownLayout) wgpuBindGroupLayoutRelease(g_bloomDownLayout);
    if (g_bloomBlurLayout) wgpuBindGroupLayoutRelease(g_bloomBlurLayout);
    g_bloomDownPipeline = g_bloomBlurPipeline = nullptr;
    g_bloomDownLayout = g_bloomBlurLayout = nullptr;
    release_rays_target(g_bloomA);
    release_rays_target(g_bloomB);
    svc_resource->free(mod_ctx, &g_bloomSource);
    if (g_linearSampler != nullptr) {
        wgpuSamplerRelease(g_linearSampler);
        g_linearSampler = nullptr;
    }
    svc_resource->free(mod_ctx, &g_fxaaSource);
    release_slot(g_fogPipeline);
    release_slot(g_raysPipeline);
    release_slot(g_particlesPipeline);
    if (g_raysComputePipeline != nullptr) {
        wgpuComputePipelineRelease(g_raysComputePipeline);
        g_raysComputePipeline = nullptr;
    }
    if (g_raysComputeLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_raysComputeLayout);
        g_raysComputeLayout = nullptr;
    }
    release_rays_target(g_raysTarget);
    for (auto& retired : g_retiredTargets) {
        release_rays_target(retired.target);
    }
    g_retiredTargets.clear();
    svc_resource->free(mod_ctx, &g_gradeSource);
    svc_resource->free(mod_ctx, &g_fogSource);
    svc_resource->free(mod_ctx, &g_raysComputeSource);
    svc_resource->free(mod_ctx, &g_raysCompositeSource);
    svc_resource->free(mod_ctx, &g_particlesSource);
    return MOD_OK;
}

}  // extern "C"
