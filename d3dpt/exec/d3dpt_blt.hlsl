// The Direct3D 11 executor's converting Blt (d3dpt_exec_d11.cpp, blt):
// a DXGI present's copy between formats whose channels differ in order
// (R8G8B8A8 into B8G8R8A8) or between sizes (a scaled Blt), drawn so the
// GPU converts and filters. Compiled by fxc into d3dpt_blt.h, which is
// checked in (DXBC, which DXVK reads on every host):
//
//   fxc /nologo /T vs_4_0 /E vs_main /Vn d3dpt_blt_vs /Fh vs.h d3dpt_blt.hlsl
//   fxc /nologo /T ps_4_0 /E ps_main /Vn d3dpt_blt_ps /Fh ps.h d3dpt_blt.hlsl
//   (then vs.h and ps.h concatenated into d3dpt_blt.h)

Texture2D src : register(t0);
SamplerState smp : register(s0);

struct V {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// one triangle over the whole target, no vertex buffer
V vs_main(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 ps_main(V i) : SV_Target
{
    return src.Sample(smp, i.uv);
}
