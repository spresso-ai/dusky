// Sfeerpakket: TAA-resultaat terug naar het beeld, met een lichte verscherping
// (de accumulatie maakt het beeld iets zachter).

struct Uniforms {
    size: vec2f,
    sharpen: f32,
    debug: f32,
}

@group(0) @binding(0) var src: texture_2d<f32>;
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

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(src));
    let px = clamp(vec2i(in.position.xy), vec2i(0), dims - 1);
    let c = textureLoad(src, px, 0).rgb;
    let n = textureLoad(src, clamp(px + vec2i(0, -1), vec2i(0), dims - 1), 0).rgb;
    let s = textureLoad(src, clamp(px + vec2i(0, 1), vec2i(0), dims - 1), 0).rgb;
    let e = textureLoad(src, clamp(px + vec2i(1, 0), vec2i(0), dims - 1), 0).rgb;
    let w = textureLoad(src, clamp(px + vec2i(-1, 0), vec2i(0), dims - 1), 0).rgb;
    let lo = min(c, min(min(n, s), min(e, w)));
    let hi = max(c, max(max(n, s), max(e, w)));
    let sharp = c + (c * 4.0 - (n + s + e + w)) * (u.sharpen * 0.25);
    var outc = clamp(sharp, lo, hi);
    if u.debug > 0.5 && in.uv.x < 0.5 {
        outc = c;  // linkerhelft zonder verscherping, ter vergelijking
    }
    return vec4f(outc, 1.0);
}
