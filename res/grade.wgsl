// Sfeerpakket: verscherping, kleurstemming en lens-effecten (tweede pass op
// GFX_STAGE_FRAME_BEFORE_HUD, na de anti-aliasing).
//
//  1. Verscherping: contrast-adaptief (naar het idee van AMD FidelityFX CAS): randen die al
//     scherp zijn worden niet overscherpt, vage texturen en verte winnen detail.
//  2. Kleur: belichting, witbalans, filmische curve (alleen op midden- en hoge tonen, zodat
//     schaduwen niet dichtlopen), contrast, split toning, vibrance (verzadigt vooral flets
//     kleur; al verzadigde kleur zoals een oranje zonsondergang blijft zoals hij is) en
//     ontzadiging van de felste hoge lichten tegen "uitbranden".
//  3. Lens: zachte gloed rond de zon, spookbeelden (ghosts) en een horizontale streep,
//     alleen als de zon echt zichtbaar is (getest in de dieptebuffer: achter een boom
//     verdwijnt de flare).
//  4. Vignet, optionele filmkorrel en dithering tegen banding.

struct Uniforms {
    shadow_tint: vec3f,
    exposure: f32,
    highlight_tint: vec3f,
    contrast: f32,
    size: vec2f,
    saturation: f32,
    temperature: f32,
    filmic: f32,
    vignette: f32,
    strength: f32,
    input_is_srgb: f32,
    frame: f32,
    tint: f32,
    debug_split: f32,
    sharpen: f32,           // 0..1
    sun_uv: vec2f,
    flare: f32,             // 0 = uit; al vermenigvuldigd met zonhoogte en kijkrichting
    sky_depth: f32,
    sun_color: vec3f,       // weergaveruimte
    reversed_z: f32,
    vibrance: f32,
    grain: f32,
    aspect: f32,
    has_depth: f32,
    bloom_size: vec2f,      // grootte van de bloom-textuur (0 = geen bloom)
    bloom: f32,             // sterkte van de gloed
    lift: f32,              // schaduwen optillen (0..1)
    clarity: f32,           // lokaal contrast (0..1)
    _pad_c0: f32,
    _pad_c1: f32,
    _pad_c2: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: Uniforms;
@group(0) @binding(2) var scene_depth: texture_2d<f32>;
@group(0) @binding(3) var bloom_tex: texture_2d<f32>;

// Lage-doorlaat-helderheid (alpha van de bloom-textuur), bilineair.
fn sample_low_luma(uv: vec2f) -> f32 {
    let c = uv * u.bloom_size - 0.5;
    let b = floor(c);
    let f = c - b;
    let maxc = vec2i(u.bloom_size) - 1;
    let p0 = clamp(vec2i(b), vec2i(0), maxc);
    let p1 = clamp(vec2i(b) + 1, vec2i(0), maxc);
    let a = textureLoad(bloom_tex, vec2i(p0.x, p0.y), 0).a;
    let bb = textureLoad(bloom_tex, vec2i(p1.x, p0.y), 0).a;
    let cc = textureLoad(bloom_tex, vec2i(p0.x, p1.y), 0).a;
    let d = textureLoad(bloom_tex, vec2i(p1.x, p1.y), 0).a;
    return mix(mix(a, bb, f.x), mix(cc, d, f.x), f.y);
}

// Handmatig bilineair uit de (kwart-resolutie) bloom-textuur.
fn sample_bloom(uv: vec2f) -> vec3f {
    if u.bloom_size.x < 1.0 {
        return vec3f(0.0);
    }
    let c = uv * u.bloom_size - 0.5;
    let b = floor(c);
    let f = c - b;
    let maxc = vec2i(u.bloom_size) - 1;
    let p0 = clamp(vec2i(b), vec2i(0), maxc);
    let p1 = clamp(vec2i(b) + 1, vec2i(0), maxc);
    let a = textureLoad(bloom_tex, vec2i(p0.x, p0.y), 0).rgb;
    let bb = textureLoad(bloom_tex, vec2i(p1.x, p0.y), 0).rgb;
    let cc = textureLoad(bloom_tex, vec2i(p0.x, p1.y), 0).rgb;
    let d = textureLoad(bloom_tex, vec2i(p1.x, p1.y), 0).rgb;
    return mix(mix(a, bb, f.x), mix(cc, d, f.x), f.y);
}

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
    let hi = pow((max(c, vec3f(0.0)) + 0.055) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn linear_to_srgb(c: vec3f) -> vec3f {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3f(0.0)), vec3f(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3f(0.0031308));
}

fn luma(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

fn aces(x: vec3f) -> vec3f {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3f(0.0), vec3f(1.0));
}

fn filmic_curve(c: vec3f) -> vec3f {
    let expanded = c / max(vec3f(1.0) - 0.8 * c, vec3f(0.05));
    return aces(expanded * 0.62);
}

fn hash12(p: vec2f) -> f32 {
    var p3 = fract(vec3f(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

fn load_px(p: vec2i) -> vec3f {
    let maxp = vec2i(u.size) - 1;
    return textureLoad(scene_color, clamp(p, vec2i(0), maxp), 0).rgb;
}

// Contrast-adaptieve verscherping op een 3x3-kruis.
fn sharpened(p: vec2i) -> vec3f {
    let b = load_px(p + vec2i(0, -1));
    let d = load_px(p + vec2i(-1, 0));
    let e = load_px(p);
    let f = load_px(p + vec2i(1, 0));
    let h = load_px(p + vec2i(0, 1));
    if u.sharpen <= 0.001 {
        return e;
    }
    let mn = min(min(min(b, d), min(f, h)), e);
    let mx = max(max(max(b, d), max(f, h)), e);
    let amp = sqrt(clamp(min(mn, vec3f(1.0) - mx) / max(mx, vec3f(1e-4)), vec3f(0.0), vec3f(1.0)));
    let peak = -1.0 / mix(8.0, 5.0, clamp(u.sharpen, 0.0, 1.0));
    let w = amp * peak;
    return clamp(((b + d + f + h) * w + e) / (vec3f(1.0) + 4.0 * w), vec3f(0.0), vec3f(1.0));
}

// Is de zon zichtbaar? Fractie lucht in een klein raster rond de zon.
fn sun_visibility() -> f32 {
    if u.has_depth < 0.5 {
        return 1.0;
    }
    let dims = vec2f(textureDimensions(scene_depth));
    var vis = 0.0;
    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            let suv = u.sun_uv + vec2f(f32(x), f32(y)) * vec2f(0.006 / u.aspect, 0.006);
            if any(suv < vec2f(0.0)) || any(suv > vec2f(1.0)) {
                vis += 0.5;  // buiten beeld: half meetellen
                continue;
            }
            let d = textureLoad(scene_depth, vec2i(suv * dims), 0).r;
            let sky = select(d >= u.sky_depth, d <= u.sky_depth, u.reversed_z > 0.5);
            vis += select(0.0, 1.0, sky);
        }
    }
    return vis / 25.0;
}

fn disc(uv: vec2f, center: vec2f, radius: f32, softness: f32) -> f32 {
    let d = length((uv - center) * vec2f(u.aspect, 1.0));
    return 1.0 - smoothstep(radius * (1.0 - softness), radius, d);
}

fn lens_flare(uv: vec2f, vis: f32) -> vec3f {
    if u.flare <= 0.001 || vis <= 0.001 {
        return vec3f(0.0);
    }
    let s = u.flare * vis;
    let toSun = (uv - u.sun_uv) * vec2f(u.aspect, 1.0);
    let dSun = length(toSun);
    // Gloed rond de zon
    var col = u.sun_color * (exp(-dSun * dSun * 60.0) * 0.55 + exp(-dSun * 9.0) * 0.12);
    // Horizontale streep (anamorf)
    let streak = exp(-abs(toSun.y) * 140.0) * exp(-abs(toSun.x) * 2.5);
    col += u.sun_color * vec3f(0.8, 0.9, 1.0) * streak * 0.35;
    // Spookbeelden langs de lijn zon -> beeldmidden -> overkant
    let axis = vec2f(0.5) - u.sun_uv;
    let ghosts = array<vec4f, 5>(
        vec4f(0.45, 0.045, 0.9, 0.7),   // positie langs de as, straal, zachtheid, kleurmix
        vec4f(0.85, 0.020, 0.6, 0.2),
        vec4f(1.25, 0.080, 0.95, 0.9),
        vec4f(1.55, 0.030, 0.7, 0.4),
        vec4f(2.10, 0.120, 0.98, 1.0));
    for (var i = 0; i < 5; i = i + 1) {
        let g = ghosts[i];
        let center = u.sun_uv + axis * g.x;
        let tintc = mix(vec3f(1.0, 0.75, 0.45), vec3f(0.45, 0.7, 1.0), g.w);
        col += tintc * u.sun_color * disc(uv, center, g.y, g.z) * 0.09;
    }
    return col * s;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let px = clamp(vec2i(in.uv * u.size), vec2i(0), vec2i(u.size) - 1);
    let src = textureLoad(scene_color, px, 0);

    if u.debug_split > 0.5 && in.uv.x < 0.5 {
        if abs(in.uv.x - 0.5) * u.size.x < 1.0 {
            return vec4f(1.0, 1.0, 1.0, src.a);
        }
        return src;
    }

    var c = sharpened(px);
    if u.input_is_srgb < 0.5 {
        c = srgb_to_linear(c);
    }

    c = c * exp2(u.exposure);

    // Lokaal contrast: versterk het verschil met de omgeving (vormen op personen en rotsen
    // komen los van een vlakke, waasachtige achtergrond). Tint blijft behouden.
    if u.clarity > 0.001 && u.bloom_size.x >= 1.0 {
        var low = max(sample_low_luma(in.uv), 0.0);
        if u.input_is_srgb < 0.5 {
            low = pow(low, 2.2);
        }
        let l = luma(c);
        let k = clamp((l + 0.002) / (low + 0.002), 0.3, 3.0);
        let gain = pow(k, u.clarity) ;
        // in de donkerste schaduwen minder, anders wordt ruis versterkt
        c = c * mix(1.0, gain, smoothstep(0.003, 0.03, l));
    }

    // Gloed erbij (lineair, vóór de filmische curve, zodat felle gloed zacht afrolt)
    if u.bloom > 0.001 {
        var g = sample_bloom(in.uv);
        if u.input_is_srgb < 0.5 {
            g = srgb_to_linear(g);
        }
        c += g * u.bloom;
    }

    // Witbalans met behoud van helderheid
    let before = max(luma(c), 1e-5);
    c = c * vec3f(1.0 + 0.12 * u.temperature, 1.0 + 0.06 * u.tint, 1.0 - 0.12 * u.temperature);
    c = c * (before / max(luma(c), 1e-5));

    // Filmische curve alleen op midden/hoge tonen: schaduwen blijven open
    let lf = luma(c);
    c = mix(c, filmic_curve(c), u.filmic * smoothstep(0.06, 0.4, lf));

    // Contrast rond middengrijs
    c = 0.18 * pow(max(c, vec3f(0.0)) / 0.18, vec3f(u.contrast));

    // Split toning
    let l = luma(c);
    let tone = mix(u.shadow_tint, u.highlight_tint, smoothstep(0.02, 0.6, l));
    c = c * tone / max(luma(tone), 1e-5);

    // Verzadiging, vibrance en bescherming van felle hoge lichten
    let l2 = luma(c);
    c = mix(vec3f(l2), c, u.saturation);
    let sat = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / max(max(c.r, max(c.g, c.b)), 1e-4);
    c = mix(vec3f(luma(c)), c, 1.0 + u.vibrance * (1.0 - sat));
    let hl = smoothstep(0.55, 0.95, luma(c));
    c = mix(c, vec3f(luma(c)), hl * 0.35 * sat);

    // Vignet
    let dv = (in.uv - 0.5) * vec2f(u.aspect, 1.0);
    c = c * (1.0 - u.vignette * smoothstep(0.35, 1.05, length(dv) * 1.25));

    c = clamp(c, vec3f(0.0), vec3f(1.0));
    var outc = c;
    if u.input_is_srgb < 0.5 {
        outc = linear_to_srgb(c);
    }
    // Schaduwen optillen (indirect licht / lichte, luchtige look), hoge lichten blijven
    // Getint met de schaduwtint van het profiel (groen/koel in het bos), niet grijs.
    let lo = 1.0 - luma(outc);
    let liftCol = clamp(u.shadow_tint * vec3f(0.55, 0.65, 0.6), vec3f(0.0), vec3f(1.0));
    outc = outc + u.lift * liftCol * lo * lo * lo * 0.6;

    // Lens-effecten in weergaveruimte (blijven helder en scherp)
    outc += lens_flare(in.uv, sun_visibility());

    // Filmkorrel (vooral in de middentonen) en dithering
    let n = hash12(in.position.xy + vec2f(u.frame * 17.0, u.frame * 7.0)) - 0.5;
    let mid = 1.0 - abs(luma(outc) * 2.0 - 1.0);
    outc += vec3f(n * u.grain * mid);
    if u.input_is_srgb < 0.5 {
        outc += vec3f((hash12(in.position.yx + vec2f(u.frame)) - 0.5) / 255.0);
    }

    return vec4f(clamp(mix(src.rgb, outc, u.strength), vec3f(0.0), vec3f(1.0)), src.a);
}
