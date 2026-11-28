// Sfeerpakket: screen-space global illumination (compute, halve resolutie).
//
// Per pixel wordt in een paar richtingen over het scherm gezocht naar oppervlakken in de buurt.
// Een oppervlak dat zichtbaar is vanaf dit punt (boven de tot dan toe hoogste horizon) geeft
// zijn kleur door als teruggekaatst licht, gewogen met de hoek ten opzichte van de normaal en
// de afstand. Zo krijgt een schaduw op het gras een groene gloed en een muur naast een
// zonverlicht pad een warme. Ruis per frame wordt door de TAA weggemiddeld.

struct Uniforms {
    view_from_proj: mat4x4f,
    proj_from_view: mat4x4f,
    full_size: vec2f,
    half_size: vec2f,
    radius: f32,          // wereldeenheden
    intensity: f32,
    frame: f32,
    reversed_z: f32,
    sky_depth: f32,
    input_is_srgb: f32,
    steps: f32,
    dirs: f32,
}

@group(0) @binding(0) var color_tex: texture_2d<f32>;
@group(0) @binding(1) var depth_tex: texture_2d<f32>;
@group(0) @binding(4) var out_tex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<uniform> u: Uniforms;

fn srgb_to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn is_sky(d: f32) -> bool {
    return select(d >= u.sky_depth, d <= u.sky_depth, u.reversed_z > 0.5);
}

fn view_pos(uv: vec2f, d: f32) -> vec3f {
    let p = u.view_from_proj * vec4f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, d, 1.0);
    return p.xyz / p.w;
}

fn depth_at(px: vec2i) -> f32 {
    let dims = vec2i(u.full_size);
    return textureLoad(depth_tex, clamp(px, vec2i(0), dims - 1), 0).r;
}

fn ign(p: vec2f) -> f32 {
    return fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let hdims = vec2i(u.half_size);
    let hp = vec2i(gid.xy);
    if hp.x >= hdims.x || hp.y >= hdims.y {
        return;
    }
    let uv = (vec2f(hp) + 0.5) / u.half_size;
    let fp = vec2i(uv * u.full_size);
    let d = depth_at(fp);
    if is_sky(d) {
        textureStore(out_tex, hp, vec4f(0.0, 0.0, 0.0, 1.0));
        return;
    }
    let inv_full = 1.0 / u.full_size;
    let P = view_pos(uv, d);

    // Normaal uit de diepte: neem per as de kleinste sprong (voorkomt fouten op randen).
    let pr = view_pos(uv + vec2f(inv_full.x, 0.0), depth_at(fp + vec2i(1, 0)));
    let pl = view_pos(uv - vec2f(inv_full.x, 0.0), depth_at(fp - vec2i(1, 0)));
    let pd = view_pos(uv + vec2f(0.0, inv_full.y), depth_at(fp + vec2i(0, 1)));
    let pu = view_pos(uv - vec2f(0.0, inv_full.y), depth_at(fp - vec2i(0, 1)));
    let dx = select(P - pl, pr - P, abs(pr.z - P.z) < abs(P.z - pl.z));
    let dy = select(P - pu, pd - P, abs(pd.z - P.z) < abs(P.z - pu.z));
    var N = normalize(cross(dy, dx));
    if dot(N, -P) < 0.0 {
        N = -N;
    }

    // Zoekstraal in uv: wereldradius geprojecteerd op deze diepte.
    let proj_scale = u.proj_from_view[1][1] * 0.5 / max(-P.z, 1.0);
    let r_uv_y = clamp(u.radius * proj_scale, 6.0 * inv_full.y, 0.25);
    let aspect = u.full_size.y / u.full_size.x;

    let noise = ign(vec2f(fp) + vec2f(u.frame * 5.588, u.frame * 3.117));
    let noise2 = ign(vec2f(fp.yx) + vec2f(u.frame * 7.31, 1.7));
    let dirs = i32(u.dirs);
    let steps = i32(u.steps);
    var gi = vec3f(0.0);
    for (var i = 0; i < dirs; i++) {
        let ang = (f32(i) + noise) * (6.2831853 / f32(dirs));
        let dir = vec2f(cos(ang) * aspect, sin(ang)) * r_uv_y;
        var horizon = 0.0;
        for (var s = 0; s < steps; s++) {
            var t = (f32(s) + noise2) / f32(steps);
            t = t * t;
            let suv = uv + dir * t;
            if any(suv < vec2f(0.0)) || any(suv > vec2f(1.0)) {
                break;
            }
            let sp = vec2i(suv * u.full_size);
            let sd = depth_at(sp);
            if is_sky(sd) {
                continue;
            }
            let S = view_pos(suv, sd);
            let D = S - P;
            let dist = length(D);
            if dist < 2.0 {
                continue;
            }
            let cosN = dot(N, D / dist);
            if cosN <= horizon {
                continue;  // verborgen achter iets dat dichterbij al hoger stond
            }
            let falloff = saturate(1.0 - dist / u.radius);
            var c = textureLoad(color_tex, sp, 0).rgb;
            if u.input_is_srgb < 0.5 {
                c = srgb_to_linear(c);
            }
            gi += c * (cosN - horizon) * falloff * falloff;
            horizon = cosN;
        }
    }
    gi = gi * (u.intensity / f32(dirs));
    textureStore(out_tex, hp, vec4f(gi, 1.0));
}
