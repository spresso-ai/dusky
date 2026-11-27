// Sfeerpakket: deeltjes per gebied.
//
//   1 = vallende bladeren (Faron)       2 = bloemblaadjes (Ordon, veld)
//   3 = gloeiende vonken (Death Mountain) 4 = sneeuw (Snowpeak)
//   5 = waaiend zand (woestijn)         6 = schemerdeeltjes (Twilight)
//
// Procedureel zoals de vuurvliegjes: elke instantie leidt zijn plek af uit zijn index, in een
// blok rond de camera dat meewikkelt. Voorvermenigvuldigde alpha: rgb wordt opgeteld, alpha
// dekt af. Zo kan één pipeline zowel dekkende bladeren als gloeiende vonken tekenen.

struct Uniforms {
    proj_from_world: mat4x4f,
    cam_right: vec3f,
    time: f32,
    cam_up: vec3f,
    count: f32,
    eye: vec3f,
    kind: f32,
    light: vec3f,      // omgevingslicht (lineair) voor niet-gloeiende deeltjes
    box: f32,
    wind: vec3f,       // windrichting * snelheid (eenheden per seconde)
    size: f32,
    sun_dir: vec3f,
    intensity: f32,
}

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) corner: vec2f,
    @location(1) color: vec4f,   // rgb = kleur, a = dekking
    @location(2) glow: vec3f,    // extra gloed (wordt opgeteld)
    @location(3) params: vec2f,  // x = soort, y = vorm-variatie
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

fn wrap_around_eye(p: vec3f, box: vec3f) -> vec3f {
    let rel = p - u.eye;
    return u.eye + (fract(rel / box + 0.5) - 0.5) * box;
}

const CORNERS = array<vec2f, 6>(
    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),
    vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VertexOutput {
    var out: VertexOutput;
    let corner = CORNERS[vi % 6u];
    let kind = u32(u.kind + 0.5);
    let t = u.time;
    let h = hash3(ii * 3u + 17u);
    let h2 = hash3(ii * 3u + 65537u);
    let h3 = hash3(ii * 3u + 999331u);

    var vel = u.wind;
    var sway = vec3f(0.0);
    var size = u.size * (0.6 + h2.x * 0.8);
    var color = vec4f(1.0);
    var glow = vec3f(0.0);
    var stretch = vec2f(1.0, 1.0);   // vorm in schermruimte (x, y)
    var spin = t * (0.5 + h3.x * 2.0) + h3.y * 6.283;
    var box = vec3f(u.box, u.box * 0.7, u.box);

    switch kind {
        case 1u: { // bladeren: langzaam vallen, slingeren, tuimelen
            vel = vel * 0.6 + vec3f(0.0, -28.0 - h2.y * 18.0, 0.0);
            sway = vec3f(sin(t * (0.7 + h.x) + h.y * 6.283) * 45.0, 0.0,
                         cos(t * (0.6 + h.z) + h.x * 6.283) * 45.0);
            let leafCol = mix(mix(vec3f(0.30, 0.42, 0.10), vec3f(0.62, 0.55, 0.14), h3.z),
                              vec3f(0.70, 0.32, 0.08), step(0.8, h2.z));
            color = vec4f(leafCol * u.light, 0.95);
            stretch = vec2f(abs(cos(spin)) * 0.85 + 0.15, 1.0);
        }
        case 2u: { // bloemblaadjes: licht, veel zijwaarts drijven
            vel = vel * 1.2 + vec3f(0.0, -14.0 - h2.y * 10.0, 0.0);
            sway = vec3f(sin(t * (1.1 + h.x) + h.y * 6.283) * 30.0,
                         sin(t * 0.9 + h.z * 6.283) * 10.0,
                         cos(t * (0.9 + h.z) + h.x * 6.283) * 30.0);
            let petal = mix(vec3f(1.0, 0.97, 0.92), vec3f(1.0, 0.78, 0.86), step(0.6, h3.z));
            color = vec4f(petal * u.light, 0.85);
            size = size * 0.7;
            stretch = vec2f(abs(cos(spin)) * 0.7 + 0.3, 0.8);
        }
        case 3u: { // vonken: opstijgen, flikkeren, uitdoven
            vel = vel * 0.5 + vec3f(0.0, 70.0 + h2.y * 60.0, 0.0);
            sway = vec3f(sin(t * (2.0 + h.x * 2.0) + h.y * 6.283) * 18.0, 0.0,
                         cos(t * (1.7 + h.z * 2.0) + h.x * 6.283) * 18.0);
            let flicker = 0.55 + 0.45 * sin(t * (9.0 + h3.x * 8.0) + h3.y * 6.283);
            let hot = mix(vec3f(1.0, 0.35, 0.05), vec3f(1.0, 0.75, 0.30), h3.z);
            glow = hot * flicker * 2.2;
            color = vec4f(0.0);
            size = size * 0.55;
            box = vec3f(u.box, u.box * 0.9, u.box);
        }
        case 4u: { // sneeuw: gestaag vallen met wind
            vel = vel + vec3f(0.0, -70.0 - h2.y * 50.0, 0.0);
            sway = vec3f(sin(t * (0.8 + h.x) + h.y * 6.283) * 20.0, 0.0,
                         cos(t * (0.7 + h.z) + h.x * 6.283) * 20.0);
            color = vec4f(vec3f(0.92, 0.95, 1.0) * max(u.light, vec3f(0.35)), 0.9);
            glow = vec3f(0.05, 0.06, 0.08);
        }
        case 5u: { // zand: snel horizontaal, lage strepen
            vel = vel * 2.5 + vec3f(0.0, sin(t * 0.5 + h.x * 6.283) * 8.0, 0.0);
            color = vec4f(vec3f(0.85, 0.68, 0.45) * u.light, 0.35);
            size = size * 0.5;
            stretch = vec2f(3.0, 0.5);
            spin = 0.0;
            box = vec3f(u.box, u.box * 0.35, u.box);
        }
        default: { // schemerdeeltjes: zwarte vlokjes met blauwe gloeirand, langzaam omhoog
            vel = vec3f(0.0, 22.0 + h2.y * 18.0, 0.0);
            sway = vec3f(sin(t * (0.4 + h.x * 0.4) + h.y * 6.283) * 40.0, 0.0,
                         cos(t * (0.35 + h.z * 0.4) + h.x * 6.283) * 40.0);
            color = vec4f(0.0, 0.0, 0.0, 0.85);
            glow = vec3f(0.15, 0.85, 1.0) * (0.6 + 0.4 * sin(t * 1.3 + h3.x * 6.283));
            spin = t * (0.3 + h3.x * 0.6) + h3.y * 6.283;
        }
    }

    var world = h * box + vel * t + sway;
    world = wrap_around_eye(world, box);

    // Niet vlak voor de lens en niet hard aan de rand van het blok.
    let dist = distance(world, u.eye);
    let fade = smoothstep(30.0, 120.0, dist) * (1.0 - smoothstep(u.box * 0.30, u.box * 0.48, dist));

    // Tegenlicht: dunne deeltjes lichten op richting de zon.
    let view_dir = normalize(world - u.eye);
    let back = pow(max(dot(view_dir, u.sun_dir), 0.0), 6.0);
    if kind == 1u || kind == 2u || kind == 4u {
        glow = glow + color.rgb * back * 1.2;
    }

    // Draaien in het schermvlak.
    let c = cos(spin);
    let s = sin(spin);
    let local = corner * stretch;
    let rot = vec2f(local.x * c - local.y * s, local.x * s + local.y * c);
    world = world + (u.cam_right * rot.x + u.cam_up * rot.y) * size;

    out.position = u.proj_from_world * vec4f(world, 1.0);
    out.corner = corner;
    out.color = vec4f(color.rgb, color.a * fade);
    out.glow = glow * fade * u.intensity;
    out.params = vec2f(f32(kind), h3.z);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let kind = u32(in.params.x + 0.5);
    let p = in.corner;
    var shape = 0.0;
    var edge = 0.0;
    switch kind {
        case 1u: { // blad: spitse ovaal met nerf
            let w = 0.55 * (1.0 - p.y * p.y);
            shape = 1.0 - smoothstep(w - 0.08, w, abs(p.x));
            shape = shape * (1.0 - 0.25 * (1.0 - smoothstep(0.0, 0.06, abs(p.x))));
        }
        case 2u: { // bloemblad: druppel
            let r = length(vec2f(p.x, p.y * 0.8 + 0.15 * p.y * p.y));
            shape = 1.0 - smoothstep(0.7, 0.9, r);
        }
        case 6u: { // vierkant vlokje met gloeiende rand
            let d = max(abs(p.x), abs(p.y));
            shape = 1.0 - smoothstep(0.62, 0.7, d);
            edge = smoothstep(0.3, 0.66, d) * shape + exp(-max(d - 0.66, 0.0) * 12.0) * 0.35;
        }
        default: { // ronde zachte stip (vonk, sneeuw, zand)
            let r2 = dot(p, p);
            shape = exp(-r2 * 4.0);
            edge = exp(-r2 * 10.0);
        }
    }
    if shape < 0.004 && edge < 0.004 {
        discard;
    }
    let a = in.color.a * shape;
    var glow = in.glow * shape;
    if kind == 6u {
        glow = in.glow * edge;
    } else if kind == 3u {
        glow = in.glow * (shape * 0.6 + edge * 1.4);
    }
    return vec4f(in.color.rgb * a + glow, a);
}
