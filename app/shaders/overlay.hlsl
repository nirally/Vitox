// Vitox 0.0.25.0: the quick-menu (R3) overlay, drawn by the host presenter over the game frame (premultiplied alpha,
// ONE / INV_SRC_ALPHA). One full-screen triangle: a dimmed violet veil with drifting glows and particles, the side panel
// texture (overlay_panel.cpp; 0.0.28.0: four variants: OFF normal | OFF confirm | ON normal | ON confirm) sliding in from the left, a lit panel edge with a moving
// sheen, and the focused item's pulse, sweep and press ripple. Built by build_overlay_shaders.bat (fxc, SM 5.0).
cbuffer Overlay : register(b0) {
    float2 screen;   // render target size, pixels
    float anim;      // 0 closed .. 1 open (eased)
    float t_open;    // seconds since the menu opened
    float2 panel;    // panel size on screen, pixels (= one texture variant)
    float2 tex;      // texture size, pixels (2 * panel.x, panel.y)
    float4 item;     // focused item card in panel pixels: x0 y0 x1 y1
    float confirm;   // 0..1 eased confirmation state
    float press;     // seconds since the last A press, < 0 = none
    float gt;        // global seconds (continuous animation)
    float k;         // pixel scale (1 = 1080p)
    float base;      // 0.0.28.0: u offset of the panel variant pair (0 = statistics overlay OFF, 0.5 = ON)
    float3 pad;
};

Texture2D<float4> panel_tex : register(t0);
SamplerState samp : register(s0);

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

float glow_blob(float2 uv, float2 c, float asp, float sharp) {
    const float2 d = (uv - c) * float2(asp, 1.0);
    return exp(-dot(d, d) * sharp);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
    const float2 p = pos.xy;
    const float2 uv = p / screen;
    const float asp = screen.x / screen.y;

    // ---- veil: dark violet, two drifting glows, two layers of twinkling particles ----
    const float a_dim = 0.70 * anim;
    float3 veil = lerp(float3(0.030, 0.010, 0.060), float3(0.090, 0.025, 0.150), saturate(uv.x * 0.9 + 0.12 * sin(gt * 0.5 + uv.y * 3.0)));
    veil += float3(0.42, 0.14, 0.86) * 0.40 * glow_blob(uv, float2(0.62 + 0.14 * sin(gt * 0.33), 0.36 + 0.12 * cos(gt * 0.27)), asp, 5.0);
    veil += float3(0.80, 0.22, 0.90) * 0.28 * glow_blob(uv, float2(0.86 + 0.09 * cos(gt * 0.21), 0.74 + 0.14 * sin(gt * 0.29)), asp, 7.0);
    float part = 0.0;
    [unroll] for (int l = 0; l < 2; ++l) {
        const float cell = (l == 0 ? 64.0 : 112.0) * k;
        const float2 q = p / cell + float2(0.07 * gt * (l + 1), -gt * (l == 0 ? 0.22 : 0.36));
        const float2 id = floor(q);
        const float h = hash(id + l * 17.0);
        const float2 off = float2(hash(id + 3.1), hash(id + 7.7)) - 0.5;
        const float d = length(frac(q) - 0.5 - off * 0.6);
        const float tw = 0.55 + 0.45 * sin(gt * 2.3 + h * 40.0);
        part += step(0.78, h) * smoothstep(l == 0 ? 0.07 : 0.10, 0.0, d) * tw * (l == 0 ? 0.55 : 0.85);
    }
    veil += float3(0.85, 0.70, 1.0) * part * 0.55;
    float4 c = float4(veil * a_dim, a_dim);

    // ---- side panel ----
    const float ox = (anim - 1.0) * (panel.x + 80.0 * k);
    const float2 lp = float2(p.x - ox, p.y);
    float3 add = 0;
    if (lp.x >= 0.0 && lp.x < panel.x) {
        const float2 tuv = float2(base + (lp.x + 0.5) / tex.x, (lp.y + 0.5) / tex.y);
        const float4 n = panel_tex.SampleLevel(samp, tuv, 0);
        const float4 cf = panel_tex.SampleLevel(samp, tuv + float2(panel.x / tex.x, 0.0), 0);
        float4 s = lerp(n, cf, confirm) * saturate(anim * 1.6);
        // a soft light sweeping down the glass
        const float sweep = exp(-pow((lp.y - fmod(gt * 260.0 * k, panel.y * 2.0) + panel.y * 0.3) / (220.0 * k), 2.0));
        s.rgb += float3(0.30, 0.16, 0.55) * 0.10 * sweep * s.a;
        c = s + c * (1.0 - s.a);

        // the focused item: pulsing outline glow, a periodic sheen, the press ripple
        const float2 ic = (item.xy + item.zw) * 0.5, ih = (item.zw - item.xy) * 0.5;
        const float d = sd_round_box(lp - ic, ih, 20.0 * k);
        const float pulse = 0.55 + 0.45 * sin(t_open * 3.4);
        const float3 hot = lerp(float3(0.70, 0.40, 1.00), float3(1.00, 0.38, 0.72), confirm);
        add += hot * (exp(-abs(d) / (5.0 * k)) * 0.85 + exp(-max(d, 0.0) / (22.0 * k)) * 0.25) * pulse * anim;
        if (d < 0.0) {
            const float span = (item.z - item.x) * 2.6;
            const float sx = item.x - 200.0 * k + fmod(t_open * 520.0 * k + 200.0 * k, span);
            add += float3(0.9, 0.8, 1.0) * 0.10 * exp(-pow((lp.x - sx + (lp.y - ic.y) * 0.35) / (36.0 * k), 2.0));
            if (press >= 0.0 && press < 0.6) {
                const float rr = press * 900.0 * k;
                add += hot * 0.6 * exp(-abs(length(lp - ic) - rr) / (10.0 * k)) * (1.0 - press / 0.6);
            }
        }
    }
    // ---- the panel's lit edge (inside and outside it), with a sheen running down ----
    const float de = abs(lp.x - panel.x);
    const float sheen = exp(-pow((p.y - fmod(gt * 520.0 * k, screen.y * 1.6) + screen.y * 0.3) / (150.0 * k), 2.0));
    add += float3(0.62, 0.36, 1.0) * (exp(-de / (3.0 * k)) * 0.9 + exp(-de / (26.0 * k)) * 0.30) * (0.55 + 0.9 * sheen) * saturate(anim * 1.3);

    c.rgb += add * saturate(c.a + 0.35);
    return c;
}
