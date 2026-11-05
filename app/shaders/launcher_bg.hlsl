// Vitox 0.0.25.0: the launcher's animated background (D3D11, one full-screen triangle, drawn every frame before the
// Direct2D UI). Everything is procedural and cheap (no textures): a slowly rotating violet gradient field, four large
// flowing light ribbons, drifting glow clouds, three parallax layers of twinkling particles, floating outlines of the
// four PlayStation face-button symbols (0.0.31.0: cross, circle, triangle, square only) and a soft light sweep - always moving, also with no input. The selected card's halo is
// drawn here too (rounded-rect distance field), so the glow costs no blur pass. Built by build_ui_shaders.bat.
cbuffer Bg : register(b0) {
    float2 screen;   // pixels
    float time;      // seconds
    float k;         // pixel scale (1 = 1080p)
    float2 parallax; // -1..1, follows the selection (smoothed)
    float glow;      // halo strength 0..1 (selection change pulse included)
    float hue;       // 0..1 accent shift (follows the selected app)
    float4 card;     // halo rect in pixels: x0 y0 x1 y1 (zero size = none)
    float radius;    // halo rect corner radius, pixels
    float fade;      // 0..1 overall brightness (launch / return transitions)
    float2 pad_;
};

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float hash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float sd_round_box(float2 p, float2 b, float r) {
    const float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// 0.0.31.0: the cross: two diagonal strokes of half-length r (distance to the nearer one)
float sd_cross(float2 p, float r) {
    p = abs(p);
    const float t = clamp((p.x + p.y) * 0.5, 0.0, r * 0.7071068);
    return length(p - float2(t, t));
}

float sd_triangle(float2 p, float r) {
    const float s = 1.732050808;
    p.x = abs(p.x) - r;
    p.y = p.y + r / s;
    if (p.x + s * p.y > 0.0)
        p = float2(p.x - s * p.y, -s * p.x - p.y) / 2.0;
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}

float2 rot(float2 p, float a) {
    const float c = cos(a), s = sin(a);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float3 accent(float t) {
    // vibrant purple -> violet -> magenta, shifted by the selection hue
    const float3 a = float3(0.55, 0.25, 1.00), b = float3(0.36, 0.20, 0.95), c = float3(0.85, 0.25, 0.85);
    const float x = frac(t);
    return x < 0.5 ? lerp(a, b, x * 2.0) : lerp(b, c, x * 2.0 - 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
    const float2 px = pos.xy;
    const float2 uv = px / screen;
    const float asp = screen.x / screen.y;
    float2 q = float2((uv.x - 0.5) * asp, uv.y - 0.5); // centred, height = 1
    const float t = time;

    // ---- gradient field: near-black violet with a slowly rotating light ----
    const float2 lq = rot(q, t * 0.05);
    float3 col = lerp(float3(0.020, 0.008, 0.045), float3(0.075, 0.025, 0.150), saturate(0.55 - lq.y * 0.9 + 0.15 * sin(lq.x * 2.2 + t * 0.30)));
    col += float3(0.10, 0.03, 0.22) * saturate(1.0 - length(q - float2(0.55 * sin(t * 0.11), -0.35)) * 1.3);

    // ---- drifting glow clouds (parallax: far) ----
    const float2 pf = q + parallax * 0.025;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float fi = i;
        const float2 c = float2(0.75 * sin(t * (0.07 + fi * 0.023) + fi * 2.1), 0.32 * cos(t * (0.09 + fi * 0.017) + fi * 1.3));
        const float2 d = pf - c;
        col += accent(hue + fi * 0.21) * 0.20 * exp(-dot(d, d) * (3.0 + fi));
    }

    // ---- four large flowing light ribbons (parallax: mid) ----
    const float2 pm = q + parallax * 0.05;
    [unroll] for (int w = 0; w < 4; ++w) {
        const float fw = w;
        const float freq = 1.4 + fw * 0.55, speed = 0.35 + fw * 0.12, amp = 0.10 + 0.035 * fw;
        const float y0 = -0.05 + fw * 0.11 - 0.12;
        const float yc = y0 + amp * sin(pm.x * freq + t * speed + fw * 1.7) + 0.045 * sin(pm.x * (freq * 2.3) - t * speed * 1.6 + fw);
        const float dist = pm.y - yc;
        const float thick = 0.006 + 0.004 * sin(t * 0.7 + fw + pm.x * 1.5);
        const float3 ac = accent(hue + 0.12 * fw + 0.05 * sin(t * 0.2));
        // a bright core line, its glow, and the lit band under it
        col += ac * (exp(-abs(dist) / thick) * 0.55 + exp(-abs(dist) / (thick * 9.0)) * 0.22);
        col += ac * 0.10 * saturate(1.0 - abs(dist + 0.06) / 0.10) * step(dist, 0.0) * (0.6 + 0.4 * sin(pm.x * 3.0 - t * 0.8 + fw));
    }

    // ---- floating geometric outlines (parallax: near) ----
    const float2 pn = q + parallax * 0.09;
    [unroll] for (int s = 0; s < 8; ++s) {
        const float fs = s;
        const float2 c = float2(frac(0.13 + fs * 0.173 + t * (0.006 + 0.002 * fs)) * (asp + 0.6) - (asp + 0.6) * 0.5,
            0.38 * sin(t * (0.10 + 0.03 * fs) + fs * 2.4) + 0.08 * sin(fs * 5.1));
        const float size = 0.035 + 0.025 * frac(fs * 0.618);
        const float2 lp = rot(pn - c, t * (0.15 + 0.07 * fs) * (fmod(fs, 2.0) < 1.0 ? 1.0 : -1.0));
        // cross, circle, triangle, square in turn (two of each)
        const float sel = fmod(fs, 4.0);
        const float d = sel < 1.0 ? sd_cross(lp, size * 1.05) : sel < 2.0 ? abs(length(lp) - size * 0.85)
                      : sel < 3.0 ? sd_triangle(lp, size) : abs(sd_round_box(lp, float2(size, size) * 0.72, size * 0.06));
        const float line_w = 0.0016;
        col += accent(hue + 0.3 + fs * 0.07) * (exp(-abs(d) / line_w) * 0.40 + exp(-abs(d) / (line_w * 10.0)) * 0.10);
    }

    // ---- twinkling particles: three parallax layers drifting up ----
    [unroll] for (int l = 0; l < 3; ++l) {
        const float fl = l;
        const float cell = (0.030 + 0.022 * fl);
        const float2 pp = (q + parallax * (0.03 + 0.05 * fl)) / cell + float2(0.15 * sin(t * 0.2 + fl), t * (0.35 + 0.25 * fl));
        const float2 id = floor(pp);
        const float h = hash(id + fl * 31.0);
        const float2 off = float2(hash(id + 4.3), hash(id + 9.1)) - 0.5;
        const float d = length(frac(pp) - 0.5 - off * 0.6);
        const float tw = 0.5 + 0.5 * sin(t * (1.5 + 2.0 * h) + h * 60.0);
        col += lerp(float3(0.75, 0.62, 1.0), accent(hue + h), 0.4) * step(0.80, h) * smoothstep(0.10 + 0.04 * fl, 0.0, d) * tw * (0.35 + 0.25 * fl);
    }

    // ---- a soft diagonal light sweep every few seconds ----
    const float sweep_x = frac(t * 0.08) * (asp + 1.6) - (asp + 1.6) * 0.5;
    col += float3(0.30, 0.18, 0.55) * 0.12 * exp(-pow((q.x + q.y * 0.6 - sweep_x) / 0.18, 2.0));

    // ---- the selected card's halo (screen pixels) ----
    if (card.z > card.x) {
        const float2 cc = (card.xy + card.zw) * 0.5, ch = (card.zw - card.xy) * 0.5;
        const float d = sd_round_box(px - cc, ch, radius);
        const float3 ac = accent(hue);
        col += ac * glow * (exp(-max(d, 0.0) / (34.0 * k)) * 0.75 + exp(-max(d, 0.0) / (110.0 * k)) * 0.35);
    }

    // vignette + the transition fade
    col *= 1.0 - 0.42 * pow(saturate(length(q * float2(0.85, 1.1))), 2.2);
    col *= fade;
    return float4(col, 1.0);
}
