// Vitox 0.0.24.54 (M9A.6): the virtual touch cursor, drawn by the host presenter over the presented frame.
// One quad around the cursor; the pixel shader composites signed-distance layers (premultiplied alpha, anti-aliased):
// soft violet glow, dark contrast halo, translucent fill, gradient ring, press ripple, white centre dot.
// Built by build_shaders.bat (fxc, SM 5.0) into src/vitox_touch_cursor_shaders.h.
cbuffer Cursor : register(b0) {
    float2 center;  // pixels
    float2 target;  // render target size, pixels
    float radius;   // pixels, at rest
    float press;    // 0..1, eased
    float appear;   // 0..1, eased fade/scale-in
    float ripple;   // 0..1 while the press ripple runs, otherwise >= 1
};

struct VsOut {
    float4 pos : SV_Position;
    float2 px : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    const float2 corner = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0);
    const float extent = radius * 2.0 + 24.0;
    VsOut o;
    o.px = center + corner * extent;
    o.pos = float4(o.px.x / target.x * 2.0 - 1.0, 1.0 - o.px.y / target.y * 2.0, 0.0, 1.0);
    return o;
}

// coverage of a band |d - r| <= w/2 with a one-pixel anti-aliased edge
float band(float d, float r, float w) { return saturate(w * 0.5 - abs(d - r) + 0.5); }
float disc(float d, float r) { return saturate(r - d + 0.5); }
float4 over(float4 dst, float3 rgb, float a) { return float4(rgb * a, a) + dst * (1.0 - a); }

float4 ps_main(VsOut i) : SV_Target {
    const float3 violet = float3(0.545, 0.361, 0.965);   // #8B5CF6
    const float3 lilac = float3(0.769, 0.710, 0.992);    // #C4B5FD
    const float3 deep = float3(0.427, 0.157, 0.851);     // #6D28D9
    const float2 v = i.px - center;
    const float d = length(v);
    const float r = radius * lerp(0.82, 1.0, appear) * lerp(1.0, 0.86, press);

    float4 c = 0;
    // glow: gaussian around the ring, stronger while pressed
    const float g = exp(-((d - r) * (d - r)) / (2.0 * 7.0 * 7.0));
    c = over(c, violet, g * lerp(0.28, 0.5, press));
    // contrast halo just outside the ring, so the cursor reads on bright scenes too
    c = over(c, float3(0.04, 0.02, 0.08), band(d, r + 2.6, 2.2) * 0.45);
    // fill
    c = over(c, lerp(violet, deep, 0.35), disc(d, r) * lerp(0.14, 0.38, press));
    // ring: lilac at the top fading to violet at the bottom, plus a faint inner highlight
    const float t = saturate(v.y / max(r, 1.0) * 0.5 + 0.5);
    c = over(c, lerp(lilac, violet, t), band(d, r, 3.0));
    c = over(c, float3(1, 1, 1), band(d, r - 2.4, 1.0) * 0.18);
    // press ripple: expands and fades
    if (ripple < 1.0) {
        const float rr = r * (1.0 + 0.9 * ripple);
        c = over(c, lilac, band(d, rr, lerp(3.0, 1.0, ripple)) * (1.0 - ripple) * 0.75);
    }
    // centre dot with a violet rim
    c = over(c, violet, disc(d, 4.6));
    c = over(c, float3(1, 1, 1), disc(d, 3.0));
    return c * appear;
}
