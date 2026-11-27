// Sfeerpakket: lichtstralen (god rays), stap 1 van 2.
//
// Screen-space radiale lichtverstrooiing: voor elke pixel lopen we in stappen richting de zon
// op het scherm en tellen we hoeveel van die monsters "lucht" zijn (niet afgedekt door
// geometrie). Waar het bladerdak gaten heeft, ontstaan zo stralen. Draait op halve resolutie;
// stap 2 (rays_composite.wgsl) telt het resultaat op bij de scene.

struct Uniforms {
    sun_uv: vec2f,       // zonpositie op het scherm (mag buiten 0..1 liggen)
    out_size: vec2f,     // grootte van de stralentextuur
    depth_size: vec2f,   // grootte van de dieptebuffer
    sky_depth: f32,      // ruwe diepte vanaf waar een pixel als lucht telt
    reversed_z: f32,
    samples: f32,
    decay: f32,          // afname per stap (0.9-0.99)
    density: f32,        // welk deel van de weg naar de zon we aflopen
    weight: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var rays_out: texture_storage_2d<r32float, write>;
@group(0) @binding(2) var<uniform> u: Uniforms;

fn sky_at(uv: vec2f) -> f32 {
    let p = clamp(vec2i(uv * u.depth_size), vec2i(0), vec2i(u.depth_size) - 1);
    let d = textureLoad(scene_depth, p, 0).r;
    return select(select(0.0, 1.0, d >= u.sky_depth), select(0.0, 1.0, d <= u.sky_depth),
        u.reversed_z > 0.5);
}

// Interleaved gradient noise (Jimenez) om banding tussen de stappen te breken.
fn ign(p: vec2f) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    if f32(gid.x) >= u.out_size.x || f32(gid.y) >= u.out_size.y {
        return;
    }
    let uv = (vec2f(gid.xy) + 0.5) / u.out_size;
    let steps = max(u32(u.samples), 1u);
    let delta = (uv - u.sun_uv) * (u.density / f32(steps));

    var p = uv - delta * ign(vec2f(gid.xy));
    var illum = 0.0;
    var falloff = 1.0;
    var norm = 0.0;
    for (var i = 0u; i < steps; i = i + 1u) {
        illum = illum + sky_at(p) * falloff;
        norm = norm + falloff;
        falloff = falloff * u.decay;
        p = p - delta;
    }
    let value = illum / max(norm, 1e-4) * u.weight;
    textureStore(rays_out, vec2i(gid.xy), vec4f(value, 0.0, 0.0, 1.0));
}
