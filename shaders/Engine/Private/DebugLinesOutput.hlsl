// Debug lines in the raster lighting mode, drawn over the tone-mapped
// output so that their colors are as given: a line tested against depth
// reads the scene's depth at its pixel and is hidden where the scene is
// nearer (depth is reversed); one on top is not tested.
struct LineConstants { float4x4 Clip; uint Depth; uint Test; float2 Scale; };
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
float4 MainPS(VertexOut input) : SV_Target
{
    if (Lines.Test != 0)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[Lines.Depth];
        const int2 texel = int2(input.Position.xy * Lines.Scale);
        if (input.Position.z < depth.Load(int3(texel, 0)))
        {
            discard;
        }
    }
    return input.Color;
}
