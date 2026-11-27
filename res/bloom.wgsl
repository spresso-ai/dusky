// Sfeerpakket: zachte gloed (bloom) op kwart-resolutie, voor de dromerige, lichtgevende look.
//
//  down : leest de scene (volle resolutie), middelt 4x4 pixels, houdt alleen het heldere
//         deel over (zachte drempel met "knie") en schrijft naar een rgba16float-textuur.
//  blur : gescheiden Gauss-vervaging (eerst horizontaal, dan verticaal) met 25 monsters.
// Het resultaat wordt in grade.wgsl bij de scene opgeteld.

struct Uniforms {
    src_size: vec2f,
    dst_size: vec2f,
    dir: vec2f,          // (1,0) of (0,1) voor blur
    threshold: f32,      // helderheid (0..1, weergaveruimte) vanaf waar iets gloeit
    knee: f32,
}

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> u: Uniforms;

fn bright_part(c: vec3f) -> vec3f {
    let br = max(c.r, max(c.g, c.b));
    let soft = clamp(br - u.threshold + u.knee, 0.0, 2.0 * u.knee);
    let curve = soft * soft / (4.0 * u.knee + 1e-4);
    let contrib = max(curve, br - u.threshold) / max(br, 1e-4);
    return c * contrib;
}

@compute @workgroup_size(8, 8)
fn down(@builtin(global_invocation_id) gid: vec3u) {
    if f32(gid.x) >= u.dst_size.x || f32(gid.y) >= u.dst_size.y {
        return;
    }
    let scale = u.src_size / u.dst_size;
    let base = vec2f(gid.xy) * scale;
    let maxp = vec2i(u.src_size) - 1;
    var acc = vec3f(0.0);
    for (var y = 0; y < 4; y = y + 1) {
        for (var x = 0; x < 4; x = x + 1) {
            let p = vec2i(base + (vec2f(f32(x), f32(y)) + 0.5) * scale / 4.0);
            acc += textureLoad(src, clamp(p, vec2i(0), maxp), 0).rgb;
        }
    }
    textureStore(dst, vec2i(gid.xy), vec4f(bright_part(acc / 16.0), 1.0));
}

@compute @workgroup_size(8, 8)
fn blur(@builtin(global_invocation_id) gid: vec3u) {
    if f32(gid.x) >= u.dst_size.x || f32(gid.y) >= u.dst_size.y {
        return;
    }
    let maxp = vec2i(u.dst_size) - 1;
    let sigma = 6.0;
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var i = -12; i <= 12; i = i + 1) {
        let w = exp(-f32(i * i) / (2.0 * sigma * sigma));
        let p = clamp(vec2i(gid.xy) + vec2i(u.dir * f32(i)), vec2i(0), maxp);
        acc += textureLoad(src, p, 0).rgb * w;
        wsum += w;
    }
    textureStore(dst, vec2i(gid.xy), vec4f(acc / wsum, 1.0));
}
