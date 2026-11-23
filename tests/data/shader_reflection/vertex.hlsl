// dxc: -T vs_6_6 -E main
cbuffer Camera : register(b0) { row_major float4x4 view_projection; };
struct VSOut { float4 position : SV_Position; float3 normal : NORMAL; float2 uv[2] : TEXCOORD1; };
VSOut main(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0, uint id : SV_VertexID)
{
    VSOut o;
    o.position = mul(float4(position, 1), view_projection);
    o.normal = normal;
    o.uv[0] = uv;
    o.uv[1] = uv * id;
    return o;
}
