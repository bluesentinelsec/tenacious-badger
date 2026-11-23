// dxc: -T cs_6_6 -E main
RWByteAddressBuffer buf;
RWStructuredBuffer<uint64_t> counters;
RaytracingAccelerationStructure scene;
Texture2D textures[] : register(t0, space1);
SamplerState s;
cbuffer C { double scale; };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint total = WaveActiveSum(id.x);
    uint64_t wide = counters[id.x] + 1;
    InterlockedAdd(counters[0], wide);
    RayQuery<0> q;
    RayDesc ray = (RayDesc)0;
    q.TraceRayInline(scene, 0, 0xff, ray);
    q.Proceed();
    float4 c = textures[NonUniformResourceIndex(id.x)].SampleLevel(s, float2(0, 0), 0);
    buf.Store(0, total + (uint)(scale * c.x) + (uint)wide);
}
