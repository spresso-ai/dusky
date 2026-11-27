// Sfeerpakket: anti-aliasing (FXAA-stijl, eigen implementatie naar het algoritme van
// Timothy Lottes). Draait als eerste pass op GFX_STAGE_FRAME_BEFORE_HUD.
//
// Werkwijze per pixel: zoek een helderheidsrand (contrast met de buren), bepaal of die
// horizontaal of verticaal loopt, zoek langs de rand naar beide uiteinden en verschuif het
// monster loodrecht op de rand, evenredig met de positie op de rand. Daarbij een
// sub-pixel-correctie voor dunne lijnen (takken, hekken, gras).

struct Uniforms {
    size: vec2f,
    inv_size: vec2f,
    subpix: f32,            // 0..1, hoeveel sub-pixel-verzachting
    edge_threshold: f32,    // minimaal relatief contrast voor een rand
    edge_threshold_min: f32,// minimaal absoluut contrast (donkere delen)
    debug_view: f32,        // 1 = randen tonen
}

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: Uniforms;
@group(0) @binding(2) var samp: sampler;

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

fn luma_of(c: vec3f) -> f32 {
    // In gamma-ruimte, zoals FXAA verwacht.
    return dot(c, vec3f(0.299, 0.587, 0.114));
}

fn sample_rgb(uv: vec2f) -> vec3f {
    return textureSampleLevel(src, samp, uv, 0.0).rgb;
}

fn luma_at(uv: vec2f) -> f32 {
    return luma_of(sample_rgb(uv));
}

const STEPS: array<f32, 10> = array<f32, 10>(1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0, 12.0);

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let uv = in.uv;
    let center = textureSampleLevel(src, samp, uv, 0.0);
    let px = u.inv_size;

    let lM = luma_of(center.rgb);
    let lN = luma_at(uv + vec2f(0.0, -px.y));
    let lS = luma_at(uv + vec2f(0.0, px.y));
    let lE = luma_at(uv + vec2f(px.x, 0.0));
    let lW = luma_at(uv + vec2f(-px.x, 0.0));

    let lMax = max(max(max(lN, lS), max(lE, lW)), lM);
    let lMin = min(min(min(lN, lS), min(lE, lW)), lM);
    let range = lMax - lMin;
    if range < max(u.edge_threshold_min, lMax * u.edge_threshold) {
        if u.debug_view > 0.5 {
            return vec4f(center.rgb * 0.3, center.a);
        }
        return center;
    }

    let lNW = luma_at(uv + vec2f(-px.x, -px.y));
    let lNE = luma_at(uv + vec2f(px.x, -px.y));
    let lSW = luma_at(uv + vec2f(-px.x, px.y));
    let lSE = luma_at(uv + vec2f(px.x, px.y));

    // Sub-pixel: hoe sterk wijkt het midden af van het gemiddelde van de buren?
    let lAvg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    let subBlend = smoothstep(0.0, 1.0, clamp(abs(lAvg - lM) / range, 0.0, 1.0));
    let subOffset = subBlend * subBlend * u.subpix;

    // Richting van de rand
    let edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    let edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    let horizontal = edgeH >= edgeV;

    var stepLen = select(px.x, px.y, horizontal);
    let lPos = select(lE, lS, horizontal);
    let lNeg = select(lW, lN, horizontal);
    let gPos = abs(lPos - lM);
    let gNeg = abs(lNeg - lM);
    var lOpp: f32;
    var gradient: f32;
    if gNeg >= gPos {
        stepLen = -stepLen;
        lOpp = lNeg;
        gradient = gNeg;
    } else {
        lOpp = lPos;
        gradient = gPos;
    }

    // Startpunt op de rand (halverwege naar de buurpixel)
    var edgeUv = uv;
    if horizontal {
        edgeUv.y += stepLen * 0.5;
    } else {
        edgeUv.x += stepLen * 0.5;
    }
    let along = select(vec2f(0.0, px.y), vec2f(px.x, 0.0), horizontal);
    let lEdge = 0.5 * (lM + lOpp);
    let scaledGradient = gradient * 0.25;

    // Langs de rand zoeken naar beide uiteinden
    var uvP = edgeUv + along;
    var uvN = edgeUv - along;
    var dP = luma_at(uvP) - lEdge;
    var dN = luma_at(uvN) - lEdge;
    var doneP = abs(dP) >= scaledGradient;
    var doneN = abs(dN) >= scaledGradient;
    for (var i = 1; i < 10; i = i + 1) {
        if doneP && doneN {
            break;
        }
        if !doneP {
            uvP += along * STEPS[i];
            dP = luma_at(uvP) - lEdge;
            doneP = abs(dP) >= scaledGradient;
        }
        if !doneN {
            uvN -= along * STEPS[i];
            dN = luma_at(uvN) - lEdge;
            doneN = abs(dN) >= scaledGradient;
        }
    }

    let distP = select(uvP.y - uv.y, uvP.x - uv.x, horizontal);
    let distN = select(uv.y - uvN.y, uv.x - uvN.x, horizontal);
    let nearestIsN = distN < distP;
    let nearest = min(distN, distP);
    let edgeLen = distN + distP;
    let pixelOffset = -nearest / max(edgeLen, 1e-6) + 0.5;

    // Alleen verschuiven als de luma aan het dichtstbijzijnde uiteinde de goede kant op loopt
    let centerSmaller = (lM - lEdge) < 0.0;
    let endDelta = select(dP, dN, nearestIsN);
    let correct = (endDelta < 0.0) != centerSmaller;
    let edgeOffset = select(0.0, pixelOffset, correct);

    let finalOffset = max(edgeOffset, subOffset);
    var finalUv = uv;
    if horizontal {
        finalUv.y += finalOffset * stepLen;
    } else {
        finalUv.x += finalOffset * stepLen;
    }
    if u.debug_view > 0.5 {
        return vec4f(select(vec3f(0.2, 0.6, 1.0), vec3f(1.0, 0.5, 0.1), horizontal), center.a);
    }
    return vec4f(sample_rgb(finalUv), center.a);
}
