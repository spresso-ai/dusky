// Sfeerpakket: atmosferische mist (draait op GFX_STAGE_SCENE_AFTER_OPAQUE).
//
// Reconstrueert per pixel de wereldpositie uit de dieptebuffer en legt er analytische
// hoogtemist over: dik bij de grond en in dalen, dunner naar boven. Verder weg schuift de
// kleur naar een blauwige luchtnevel (aerial perspective), en in de richting van de zon
// krijgt de mist een warme gloed (Mie-achtige voorwaartse verstrooiing).
// Uitvoer: kleur + dekking, alpha-geblend over de scene. Transparante objecten (water,
// effecten, feeen) worden pas daarna getekend en blijven dus helder.

struct Uniforms {
    world_from_proj: mat4x4f,
    eye: vec3f,
    density: f32,          // extinctie per wereldeenheid op basishoogte
    fog_color: vec3f,      // lineair
    height_falloff: f32,   // per wereldeenheid; groter = mist blijft lager hangen
    sun_dir: vec3f,        // genormaliseerd, richting naar de zon
    sun_glow: f32,
    sun_color: vec3f,      // lineair
    base_height: f32,      // wereld-Y waar de mist het dichtst is
    aerial_color: vec3f,   // lineair, kleur van verre nevel
    max_opacity: f32,
    start_distance: f32,   // eerste stuk zonder mist (rond Link)
    aerial_distance: f32,  // afstand waarop de nevel volledig naar aerial_color is
    sky_distance: f32,     // fictieve afstand voor luchtpixels (horizonnevel)
    reversed_z: f32,
    size: vec2f,
    output_srgb: f32,      // 1 als de scene-target een sRGB-formaat heeft
    debug_view: f32,       // 1 = toon alleen de mistdekking
    sky_depth: f32,        // ruwe diepte vanaf waar een pixel als lucht telt
    sky_dist: f32,         // wereldafstand vanaf waar een pixel als lucht/skydome telt
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn linear_to_srgb(c: vec3f) -> vec3f {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3f(0.0)), vec3f(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3f(0.0031308));
}

fn unproject(uv: vec2f, depth: f32) -> vec3f {
    let clip = vec4f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    let w = u.world_from_proj * clip;
    return w.xyz / w.w;
}

// Integraal van density * exp(-falloff * (y - base)) langs de straal, vanaf start_distance.
fn height_fog_amount(ray_dir: vec3f, dist: f32) -> f32 {
    let travel = max(dist - u.start_distance, 0.0);
    if travel <= 0.0 {
        return 0.0;
    }
    let start = u.eye + ray_dir * min(u.start_distance, dist);
    let k = u.height_falloff;
    let h0 = start.y - u.base_height;
    // Onder de basishoogte niet verder verdichten dan bij de basis zelf.
    let base_term = u.density * exp(-k * max(h0, 0.0));
    let dy = ray_dir.y * k;
    if abs(dy) < 1e-6 {
        return base_term * travel;
    }
    return base_term * (1.0 - exp(-dy * travel)) / dy;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(scene_depth));
    let px = clamp(vec2i(in.uv * vec2f(dims)), vec2i(0), dims - 1);
    let depth = textureLoad(scene_depth, px, 0).r;

    // Lucht = gewiste pixels of alles voorbij ~90% van het verre vlak (de skydome).
    var is_sky = select(depth >= u.sky_depth, depth <= u.sky_depth, u.reversed_z > 0.5);
    var world_pt = vec3f(0.0);
    if !is_sky {
        world_pt = unproject(in.uv, depth);
        // Skydome die als geometrie in de dieptebuffer staat: ook als lucht behandelen.
        is_sky = distance(world_pt, u.eye) > u.sky_dist;
    }

    var ray_dir: vec3f;
    var dist: f32;
    if is_sky {
        let far_point = unproject(in.uv, select(0.9999, 0.0001, u.reversed_z > 0.5));
        ray_dir = normalize(far_point - u.eye);
        dist = u.sky_distance;
    } else {
        let to_point = world_pt - u.eye;
        dist = length(to_point);
        ray_dir = to_point / max(dist, 1e-4);
    }

    var opacity = 1.0 - exp(-height_fog_amount(ray_dir, dist));
    if is_sky {
        // De lucht krijgt alleen nevel richting de horizon.
        opacity = opacity * (1.0 - smoothstep(0.0, 0.35, ray_dir.y));
    }
    opacity = clamp(opacity, 0.0, u.max_opacity);

    let aerial_t = smoothstep(0.0, 1.0, dist / max(u.aerial_distance, 1.0));
    var color = mix(u.fog_color, u.aerial_color, aerial_t);

    // Zonverlichte nevel: brede voorwaartse verstrooiing (Henyey-Greenstein-achtig) plus een
    // smalle kern. Kijk je richting de zon, dan licht de nevel warm op, zoals in tegenlicht.
    let cos_sun = max(dot(ray_dir, u.sun_dir), 0.0);
    let glow = pow(cos_sun, 3.0) * 0.55 + pow(cos_sun, 12.0) * 0.6 + pow(cos_sun, 96.0) * 0.9;
    color = color + u.sun_color * glow * u.sun_glow;
    // Ook loodrecht op de zon een beetje zonlicht in de nevel (de lucht zelf is verlicht).
    color = color + u.sun_color * 0.12 * u.sun_glow;

    if u.debug_view > 0.5 {
        return vec4f(vec3f(opacity), 1.0);
    }

    var out_color = color;
    if u.output_srgb < 0.5 {
        out_color = linear_to_srgb(color);
    }
    return vec4f(clamp(out_color, vec3f(0.0), vec3f(1.0)), opacity);
}
