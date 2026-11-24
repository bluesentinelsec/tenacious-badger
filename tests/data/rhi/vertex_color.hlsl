// Vertices from a vertex buffer, with their own colors, drawn indexed.
struct VertexIn
{
    float3 Position : POSITION;
    float4 Color : COLOR0;
};

struct VertexOut
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

VertexOut MainVS(VertexIn input)
{
    VertexOut output;
    output.Position = float4(input.Position, 1);
    output.Color = input.Color;
    return output;
}

float4 MainPS(VertexOut input) : SV_Target
{
    return input.Color;
}
