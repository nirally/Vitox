// Vitox 0.0.28.0: the performance statistics overlay. The panel (perf_stats.cpp, premultiplied BGRA drawn with Direct2D)
// is copied 1:1 into the top-right corner of the frame with a fade (alpha); the scissor limits the work to the panel.
// Blend: ONE / INV_SRC_ALPHA. Built by build_ui_shaders.bat (fxc, SM 5.0).
cbuffer Stats : register(b0) {
    float4 rect;  // x0 y0 w h, pixels
    float alpha;  // fade 0..1
    float3 pad;
};

Texture2D<float4> panel_tex : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
    const int2 p = int2(pos.xy - rect.xy);
    if (p.x < 0 || p.y < 0 || p.x >= (int)rect.z || p.y >= (int)rect.w)
        discard;
    return panel_tex.Load(int3(p, 0)) * alpha;
}
