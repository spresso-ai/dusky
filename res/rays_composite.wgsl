// Sfeerpakket: lichtstralen, stap 2 van 2. Telt de stralentextuur (halve resolutie, bilineair
// opgeschaald) op bij de scene, gekleurd met de zon- of maankleur.

struct Uniforms {
    sun_color: vec3f,    // al in weergaveruimte
    intensity: f32,
    rays_size: vec2f,
    sun_uv: vec2f,       // zonpositie op het scherm
    aspect: f32,         // breedte / hoogte
    debug_view: f32,
    falloff: f32,        // hoe snel de stralen verder van de zon afnemen
    _pad: f32,
}

@group(0) @binding(0) var rays: texture_2d<f32>;
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

// Handmatig bilineair (r32float is zonder extra feature niet filterbaar).
fn sample_rays(uv: vec2f) -> f32 {
    let c = uv * u.rays_size - 0.5;
    let b = floor(c);
    let f = c - b;
    let maxc = vec2i(u.rays_size) - 1;
    let p00 = clamp(vec2i(b), vec2i(0), maxc);
    let p11 = clamp(vec2i(b) + 1, vec2i(0), maxc);
    let v00 = textureLoad(rays, vec2i(p00.x, p00.y), 0).r;
    let v10 = textureLoad(rays, vec2i(p11.x, p00.y), 0).r;
    let v01 = textureLoad(rays, vec2i(p00.x, p11.y), 0).r;
    let v11 = textureLoad(rays, vec2i(p11.x, p11.y), 0).r;
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r = sample_rays(in.uv);
    if u.debug_view > 0.5 {
        return vec4f(vec3f(r), 1.0);
    }
    // Zachte knie zodat volle stralen niet uitbranden, en afname met de afstand tot de zon
    // zodat een open lucht niet in z'n geheel oplicht.
    let shaped = r * r * (3.0 - 2.0 * r);
    let d = (in.uv - u.sun_uv) * vec2f(u.aspect, 1.0);
    let radial = 1.0 / (1.0 + dot(d, d) * u.falloff);
    return vec4f(u.sun_color * shaped * radial * u.intensity, 0.0);
}
