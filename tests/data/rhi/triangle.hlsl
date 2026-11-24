// A triangle that covers the target, colored by the draw's push constants.
// Its pixels are all the color, which a test reads back.
struct DrawConstants
{
    float4 Color;
};
ConstantBuffer<DrawConstants> Draw : register(b0);

float4 MainVS(uint id : SV_VertexID) : SV_Position
{
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 MainPS() : SV_Target
{
    return Draw.Color;
}
