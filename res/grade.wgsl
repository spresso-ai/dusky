// Sfeerpakket: filmische kleurstemming (draait op GFX_STAGE_FRAME_BEFORE_HUD).
//
// Leest een momentopname van de scene en schrijft het gegradeerde beeld terug over de
// scene-target (zonder blending). De game rendert naar een 8-bit doel, dus de "tonemapping"
// is een filmische curve op LDR: de hoge lichten worden eerst iets opgerekt (inverse
// Reinhard) en dan met de ACES-benadering van Narkowicz zacht afgerold. Gekalibreerd zodat
// middengrijs (0.18) gelijk blijft.

struct Uniforms {
    shadow_tint: vec3f,
    exposure: f32,          // stops
    highlight_tint: vec3f,
    contrast: f32,          // 1 = neutraal
    size: vec2f,            // grootte van de snapshot in pixels
    saturation: f32,        // 1 = neutraal
    temperature: f32,       // -1 koel .. +1 warm
    filmic: f32,            // 0 = uit, 1 = volledige curve
    vignette: f32,          // 0 = uit
    strength: f32,          // menging met het origineel
    input_is_srgb: f32,     // 1 als de snapshot al lineair binnenkomt (sRGB-formaat)
    frame: f32,             // voor dithering
    tint: f32,              // -1 magenta .. +1 groen
    debug_split: f32,       // 1 = links origineel, rechts gegradeerd
    _pad: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
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

fn srgb_to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn linear_to_srgb(c: vec3f) -> vec3f {
    let lo = c * 12.92;
    let hi = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3f(0.0031308));
}

fn luma(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

fn aces(x: vec3f) -> vec3f {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3f(0.0), vec3f(1.0));
}

fn filmic_curve(c: vec3f) -> vec3f {
    // Inverse Reinhard (k = 0.8) rekt 8-bit-hooglichten op tot een pseudo-HDR-bereik,
    // daarna ACES met schaal 0.62. Zie de kalibratie in de README.
    let expanded = c / max(vec3f(1.0) - 0.8 * c, vec3f(0.05));
    return aces(expanded * 0.62);
}

// Driehoeksverdeelde ruis van ~1/255 tegen banding in de mist en lucht.
fn dither(p: vec2f) -> f32 {
    let seed = p + vec2f(u.frame * 7.13, u.frame * 3.71);
    let a = fract(sin(dot(seed, vec2f(12.9898, 78.233))) * 43758.5453);
    let b = fract(sin(dot(seed, vec2f(39.3468, 11.135))) * 24634.6345);
    return (a + b - 1.0) / 255.0;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let max_px = vec2i(u.size) - 1;
    let px = clamp(vec2i(in.uv * u.size), vec2i(0), max_px);
    let src = textureLoad(scene_color, px, 0);

    if u.debug_split > 0.5 && in.uv.x < 0.5 {
        if abs(in.uv.x - 0.5) * u.size.x < 1.0 {
            return vec4f(1.0, 1.0, 1.0, src.a);
        }
        return src;
    }

    var c = src.rgb;
    if u.input_is_srgb < 0.5 {
        c = srgb_to_linear(c);
    }

    // Belichting
    c = c * exp2(u.exposure);

    // Witbalans: warm/koel en groen/magenta, met behoud van helderheid
    let before = max(luma(c), 1e-5);
    c = c * vec3f(1.0 + 0.12 * u.temperature, 1.0 + 0.06 * u.tint, 1.0 - 0.12 * u.temperature);
    c = c * (before / max(luma(c), 1e-5));

    // Filmische curve
    c = mix(c, filmic_curve(c), u.filmic);

    // Contrast rond middengrijs
    c = 0.18 * pow(max(c, vec3f(0.0)) / 0.18, vec3f(u.contrast));

    // Split toning: schaduwen en hoge lichten een eigen tint
    let l = luma(c);
    let t = smoothstep(0.02, 0.6, l);
    let tone = mix(u.shadow_tint, u.highlight_tint, t);
    c = c * tone / max(luma(tone), 1e-5);

    // Verzadiging
    let l2 = luma(c);
    c = mix(vec3f(l2), c, u.saturation);

    // Vignet
    let d = (in.uv - 0.5) * vec2f(u.size.x / u.size.y, 1.0);
    let v = 1.0 - u.vignette * smoothstep(0.35, 1.05, length(d) * 1.25);
    c = c * v;

    c = clamp(c, vec3f(0.0), vec3f(1.0));
    var outc = c;
    if u.input_is_srgb < 0.5 {
        outc = linear_to_srgb(c) + vec3f(dither(in.position.xy));
    }
    let base = src.rgb;
    return vec4f(clamp(mix(base, outc, u.strength), vec3f(0.0), vec3f(1.0)), src.a);
}
