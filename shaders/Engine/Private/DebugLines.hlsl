struct LineConstants { float4x4 Clip; };
ConstantBuffer<LineConstants> Lines : register(b0);
struct VertexIn { float3 Position : POSITION; float4 Color : COLOR0; };
struct VertexOut { float4 Position : SV_Position; float4 Color : COLOR0; };
VertexOut MainVS(VertexIn input)
{
    VertexOut output;
    output.Position = mul(Lines.Clip, float4(input.Position, 1));
    output.Color = input.Color;
    return output;
}
float4 MainPS(VertexOut input) : SV_Target { return input.Color; }
