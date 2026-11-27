// dxc: -T ps_6_0 -E main
struct Indices
{
    uint source;
    uint target;
    uint image;
};

struct Draw
{
    float4 tint;
    Indices passes[2];
};

ConstantBuffer<Draw> draw : register(b1);
TextureBuffer<Indices> lookup : register(t0);

cbuffer Options : register(b0)
{
    uint mask;
};

float4 main() : SV_Target
{
    return draw.tint * (draw.passes[1].source + draw.passes[0].target + lookup.image + mask);
}
