struct DrawConstants { float4x4 Clip; float4x4 Model; float4 Color; float4 Light; };
ConstantBuffer<DrawConstants> Draw : register(b1);
struct VertexIn { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
struct VertexOut { float4 Position : SV_Position; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
VertexOut MainVS(VertexIn input)
{
    VertexOut output;
    output.Position = mul(Draw.Clip, float4(input.Position, 1));
    output.Normal = mul(Draw.Model, float4(input.Normal, 0)).xyz;
    output.UV = input.UV;
    return output;
}
float4 MainPS(VertexOut input) : SV_Target
{
    const float3 normal = normalize(input.Normal);
    const float facing = saturate(dot(normal, -Draw.Light.xyz));
    const float shade = lerp(1.0, 0.25 + 0.75 * facing, Draw.Light.w);
    return float4(Draw.Color.rgb * shade, Draw.Color.a);
}
