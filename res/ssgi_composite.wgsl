// Sfeerpakket: teruggekaatst licht (SSGI) bij de scene optellen, vóór mist en kleurcorrectie.
//
// Het licht van halve resolutie wordt diepte-bewust opgeschaald, getint met de kleur van het
// ontvangende oppervlak (benadering van het albedo) en vooral in schaduw toegevoegd.

struct Uniforms {
    view_from_proj: mat4x4f,
    full_size: vec2f,
    half_size: vec2f,
    strength: f32,
    input_is_srgb: f32,
    output_srgb: f32,
    debug: f32,
    shadow_bias: f32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var color_tex: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: Uniforms;
@group(0) @binding(2) var depth_tex: texture_2d<f32>;
@group(0) @binding(3) var gi_tex: texture_2d<f32>;

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

fn srgb_to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn linear_to_srgb(c: vec3f) -> vec3f {
    let x = clamp(c, vec3f(0.0), vec3f(1.0));
    let lo = x * 12.92;
    let hi = 1.055 * pow(x, vec3f(1.0 / 2.4)) - 0.055;
    return select(hi, lo, x <= vec3f(0.0031308));
}

fn view_z(px: vec2i) -> f32 {
    let dims = vec2i(u.full_size);
    let d = textureLoad(depth_tex, clamp(px, vec2i(0), dims - 1), 0).r;
    let p = u.view_from_proj * vec4f(0.0, 0.0, d, 1.0);
    return -p.z / p.w;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let px = vec2i(in.position.xy);
    var col = textureLoad(color_tex, px, 0).rgb;
    if u.input_is_srgb < 0.5 {
        col = srgb_to_linear(col);
    }

    // Diepte-bewuste bilineaire opschaling van de halve-resolutie GI.
    let z0 = view_z(px);
    let hpos = (vec2f(px) + 0.5) * (u.half_size / u.full_size) - 0.5;
    let base = vec2i(floor(hpos));
    let f = hpos - vec2f(base);
    let hdims = vec2i(u.half_size);
    var gi = vec3f(0.0);
    var wsum = 0.0;
    for (var j = 0; j < 2; j++) {
        for (var i = 0; i < 2; i++) {
            let hp = clamp(base + vec2i(i, j), vec2i(0), hdims - 1);
            let bw = select(1.0 - f.x, f.x, i == 1) * select(1.0 - f.y, f.y, j == 1);
            let fpx = vec2i((vec2f(hp) + 0.5) * (u.full_size / u.half_size));
            let zs = view_z(fpx);
            let dw = exp(-abs(zs - z0) / max(z0 * 0.04, 1.0));
            let w = bw * dw + 1e-4;
            gi += textureLoad(gi_tex, hp, 0).rgb * w;
            wsum += w;
        }
    }
    gi = gi / wsum;

    // Albedo-benadering: tint van het oppervlak zelf, op vaste helderheid.
    let mx = max(col.r, max(col.g, col.b));
    let albedo = clamp(col / max(mx, 0.03) * 0.55, vec3f(0.0), vec3f(1.0));
    let lum = dot(col, vec3f(0.2126, 0.7152, 0.0722));
    let shadow = 1.0 - smoothstep(0.05, 0.5, lum) * u.shadow_bias;
    // Toegevoegd licht schaalt met wat er al is: donkere kleding blijft donker, maar krijgt
    // wel de kleur van de omgeving; voorkomt een grijze waas over personen in de schaduw.
    let added = min(gi * albedo * u.strength * shadow, col * 1.2 + vec3f(0.002));
    var outc = col + added;
    if u.debug > 0.5 {
        outc = gi * u.strength;
    }
    if u.output_srgb < 0.5 {
        outc = linear_to_srgb(outc);
    }
    return vec4f(outc, 1.0);
}
