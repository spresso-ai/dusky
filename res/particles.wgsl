// Sfeerpakket: vuurvliegjes en zwevende stofdeeltjes.
//
// Volledig procedureel: er is geen deeltjesbuffer. Elke instantie leidt uit zijn index een
// vaste willekeurige plek af in een blok rond de camera; dat blok "wikkelt" mee als de camera
// beweegt, zodat er altijd deeltjes om je heen zijn zonder dat de CPU iets bijhoudt.
// Getekend in de scene-pass met dieptetest (achter bomen verdwijnen ze), zonder diepte te
// schrijven, en additief geblend.

struct Uniforms {
    proj_from_world: mat4x4f,
    cam_right: vec3f,
    time: f32,
    cam_up: vec3f,
    firefly_box: f32,      // breedte van het blok voor vuurvliegjes
    eye: vec3f,
    ground_y: f32,         // ongeveer de grondhoogte (Links hoogte)
    firefly_color: vec3f,
    firefly_count: f32,
    dust_color: vec3f,
    dust_count: f32,
    sun_dir: vec3f,
    firefly_size: f32,
    dust_size: f32,
    dust_box: f32,
    firefly_height: f32,   // tot hoe hoog boven de grond vuurvliegjes zweven
    brightness: f32,
}

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) corner: vec2f,
    @location(1) color: vec3f,
    @location(2) kind: f32,     // 0 = vuurvlieg, 1 = stof
}

fn hash3(n: u32) -> vec3f {
    var x = n * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    let a = (x >> 22u) ^ x;
    var y = a * 747796405u + 1442695041u;
    y = ((y >> ((y >> 28u) + 4u)) ^ y) * 277803737u;
    let b = (y >> 22u) ^ y;
    var z = b * 747796405u + 3141592653u;
    z = ((z >> ((z >> 28u) + 4u)) ^ z) * 277803737u;
    let c = (z >> 22u) ^ z;
    return vec3f(f32(a & 0xFFFFu), f32(b & 0xFFFFu), f32(c & 0xFFFFu)) / 65535.0;
}

// Plek in een blok rond de camera dat meewikkelt.
fn wrap_around_eye(base: vec3f, box: vec3f) -> vec3f {
    let rel = base - u.eye;
    return u.eye + (fract(rel / box + 0.5) - 0.5) * box;
}

const CORNERS = array<vec2f, 6>(
    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),
    vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VertexOutput {
    var out: VertexOutput;
    let corner = CORNERS[vi % 6u];
    let n_fly = u32(u.firefly_count);
    let is_dust = ii >= n_fly;
    let t = u.time;

    var world: vec3f;
    var size: f32;
    var color: vec3f;
    if !is_dust {
        let h = hash3(ii * 2u + 1u);
        let h2 = hash3(ii * 2u + 7919u);
        let wander = vec3f(
            sin(t * (0.21 + h2.x * 0.2) + h.x * 6.283) * 70.0,
            sin(t * (0.53 + h2.y * 0.4) + h.y * 6.283) * 22.0,
            cos(t * (0.17 + h2.z * 0.2) + h.z * 6.283) * 70.0);
        let base = vec3f(h.x * u.firefly_box, 0.0, h.z * u.firefly_box) + wander;
        world = wrap_around_eye(base, vec3f(u.firefly_box, 1.0, u.firefly_box));
        world.y = u.ground_y + 20.0 + h.y * u.firefly_height + wander.y;
        // Knipperen: elk vliegje zijn eigen ritme, deels uit.
        let blink = smoothstep(0.35, 0.9, sin(t * (0.9 + h2.x * 1.3) + h2.z * 6.283) * 0.5 + 0.5);
        size = u.firefly_size * (0.7 + h2.y * 0.6);
        color = u.firefly_color * blink;
    } else {
        let id = ii - n_fly;
        let h = hash3(id * 3u + 104729u);
        let h2 = hash3(id * 3u + 15485863u);
        let drift = vec3f(t * 9.0 + sin(t * 0.3 + h2.x * 6.283) * 25.0,
                          sin(t * 0.21 + h2.y * 6.283) * 30.0 - t * 2.0,
                          t * 6.0 + cos(t * 0.27 + h2.z * 6.283) * 25.0);
        let base = h * u.dust_box + drift;
        world = wrap_around_eye(base, vec3f(u.dust_box, u.dust_box * 0.6, u.dust_box));
        // Stof licht op richting de zon (voorwaartse verstrooiing), zoals in tegenlicht.
        let view_dir = normalize(world - u.eye);
        let forward = pow(max(dot(view_dir, u.sun_dir), 0.0), 4.0);
        size = u.dust_size * (0.6 + h2.x * 0.8);
        color = u.dust_color * (0.25 + 1.5 * forward);
    }

    // Vervagen dichtbij (geen grote vlekken voor de camera) en aan de rand van het blok.
    let dist = distance(world, u.eye);
    let box = select(u.firefly_box, u.dust_box, is_dust);
    let fade = smoothstep(40.0, 160.0, dist) * (1.0 - smoothstep(box * 0.32, box * 0.5, dist));

    world = world + (u.cam_right * corner.x + u.cam_up * corner.y) * size;
    out.position = u.proj_from_world * vec4f(world, 1.0);
    out.corner = corner;
    out.color = color * fade * u.brightness;
    out.kind = select(0.0, 1.0, is_dust);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r2 = dot(in.corner, in.corner);
    if r2 > 1.0 {
        discard;
    }
    // Vuurvlieg: felle kern met brede gloed; stof: zachte stip.
    let fly = exp(-r2 * 16.0) * 1.6 + exp(-r2 * 3.5) * 0.45;
    let dust = exp(-r2 * 5.0) * 0.7;
    let a = mix(fly, dust, in.kind);
    return vec4f(in.color * a, 0.0);
}
