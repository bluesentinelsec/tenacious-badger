// dxc: -T ps_6_6 -E main
struct Inner { float a; float2 b; };
struct Light { float3 color; float range; Inner inner; float tail; };
cbuffer Layout
{
    float x;
    float3 v3;
    float2 v2;
    float2 w2;
    float s1;
    float3 after_s1;
    float arr[3];
    float tail1;
    Light light;
    float tail2;
    float4x3 m43;
    row_major float4x3 rm43;
    float2x2 m22;
    float tail3;
    double d;
    float tail4;
    min16float mf;
    bool flag;
    half h;
    Light lights[2];
    float3x2 m32arr[2];
    float tail5;
};
cbuffer Packed
{
    float p0 : packoffset(c1.y);
    float4 p1 : packoffset(c0);
    float p2 : packoffset(c2);
};
float4 main() : SV_Target
{
    return x + v3.x + v2.x + w2.x + s1 + after_s1.x + arr[2] + tail1 + light.inner.b.y + tail2 + m43[0][0] + rm43[0][0] + m22[0][0] + tail3 + (float)d + tail4 + mf + flag + h + lights[1].tail + m32arr[1][0][0] + tail5 + p0 + p1.x + p2;
}
