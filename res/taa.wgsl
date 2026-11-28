// Sfeerpakket: temporele anti-aliasing (compute, volledige resolutie).
//
// De camera wordt elk frame een fractie van een pixel verschoven (Halton 2,3). Hier wordt het
// nieuwe beeld gemengd met de geschiedenis van eerdere frames. De geschiedenis wordt via de
// dieptebuffer en de cameramatrices van het vorige frame teruggeprojecteerd, en begrensd tot
// het kleurbereik van de 3x3-buurt (YCoCg, variantie) zodat bewegende dingen niet uitsmeren.

struct Uniforms {
    world_from_proj: mat4x4f,       // huidig frame (met jitter)
    prev_proj_from_world: mat4x4f,  // vorig frame (met jitter)
    size: vec2f,
    jitter: vec2f,                  // huidige jitter in NDC
    prev_jitter: vec2f,             // vorige jitter in NDC
    reset: f32,
    reversed_z: f32,
    feedback: f32,                  // gewicht van het nieuwe frame in rust (bijv. 0.08)
    clip_gamma: f32,
    _pad0: f32,
    _pad1: f32,
}

@group(0) @binding(0) var cur_tex: texture_2d<f32>;
@group(0) @binding(1) var depth_tex: texture_2d<f32>;
@group(0) @binding(2) var hist_tex: texture_2d<f32>;
@group(0) @binding(4) var out_tex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<uniform> u: Uniforms;
@group(0) @binding(6) var lin_sampler: sampler;

fn to_ycocg(c: vec3f) -> vec3f {
    return vec3f(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5 * c.r - 0.5 * c.b,
                 -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

fn from_ycocg(c: vec3f) -> vec3f {
    let t = c.x - c.z;
    return vec3f(t + c.y, c.x + c.z, t - c.y);
}

fn luma(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

// Catmull-Rom met 9 bilineaire taps samengevat in 5 (scherper dan gewoon bilineair).
fn sample_history(uv: vec2f) -> vec3f {
    let pos = uv * u.size;
    let center = floor(pos - 0.5) + 0.5;
    let f = pos - center;
    let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    let w3 = f * f * (-0.5 + 0.5 * f);
    let w12 = w1 + w2;
    let tc12 = (center + w2 / w12) / u.size;
    let tc0 = (center - 1.0) / u.size;
    let tc3 = (center + 2.0) / u.size;
    var r = vec3f(0.0);
    r += textureSampleLevel(hist_tex, lin_sampler, vec2f(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y);
    r += textureSampleLevel(hist_tex, lin_sampler, vec2f(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y);
    r += textureSampleLevel(hist_tex, lin_sampler, vec2f(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y);
    r += textureSampleLevel(hist_tex, lin_sampler, vec2f(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y);
    r += textureSampleLevel(hist_tex, lin_sampler, vec2f(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
    let wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3f(0.0));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = vec2i(u.size);
    let px = vec2i(gid.xy);
    if px.x >= dims.x || px.y >= dims.y {
        return;
    }
    let cur = textureLoad(cur_tex, px, 0).rgb;

    // Buurt: gemiddelde en spreiding in YCoCg, plus de dichtstbijzijnde diepte voor de
    // terugprojectie (houdt randen van voorgrondobjecten scherp).
    var m1 = vec3f(0.0);
    var m2 = vec3f(0.0);
    var best = textureLoad(depth_tex, px, 0).r;
    for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
            let q = clamp(px + vec2i(dx, dy), vec2i(0), dims - 1);
            let c = to_ycocg(textureLoad(cur_tex, q, 0).rgb);
            m1 += c;
            m2 += c * c;
            let d = textureLoad(depth_tex, q, 0).r;
            best = select(min(best, d), max(best, d), u.reversed_z > 0.5);
        }
    }
    let mean = m1 / 9.0;
    let sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3f(0.0)));
    let bmin = mean - sigma * u.clip_gamma;
    let bmax = mean + sigma * u.clip_gamma;

    let uv = (vec2f(px) + 0.5) / u.size;
    let d = select(min(best, 0.9999995), max(best, 0.0000005), u.reversed_z > 0.5);
    let ndc = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let w = u.world_from_proj * vec4f(ndc, d, 1.0);
    let world = w.xyz / w.w;
    let pc = u.prev_proj_from_world * vec4f(world, 1.0);
    let prev_ndc = pc.xy / pc.w + u.jitter - u.prev_jitter;
    let hist_uv = vec2f(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);

    let offscreen = any(hist_uv < vec2f(0.0)) || any(hist_uv > vec2f(1.0)) || pc.w <= 0.0;
    if u.reset > 0.5 || offscreen {
        textureStore(out_tex, px, vec4f(cur, 1.0));
        return;
    }

    let hist_raw = sample_history(hist_uv);
    let hist = from_ycocg(clamp(to_ycocg(hist_raw), bmin, bmax));

    // Sneller vernieuwen bij beweging, rustig accumuleren als het stilstaat.
    let vel_px = length((uv - hist_uv) * u.size);
    let alpha = mix(u.feedback, 0.3, saturate(vel_px / 16.0));
    // Helderheidsgewogen mengen dempt flikkerende lichtpuntjes.
    let wc = alpha / (1.0 + luma(cur));
    let wh = (1.0 - alpha) / (1.0 + luma(hist));
    let outc = (cur * wc + hist * wh) / (wc + wh);
    textureStore(out_tex, px, vec4f(outc, 1.0));
}
