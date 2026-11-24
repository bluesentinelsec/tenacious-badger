// Each thread skins a vertex: its position and normal transformed by the
// sum of its joints' matrices, weighed, which is a sum of each column. A
// joint past the draw's matrices takes the last, so no vertex reads another
// draw's. Normals are transformed as directions by the same matrix, which
// is right for rotations and uniform scales.
struct SkinConstants
{
    uint Source; uint Joints; uint JointOffset; uint JointCount;
    uint Dest; uint DestOffset; uint VertexCount; uint Unused;
};
ConstantBuffer<SkinConstants> Skin : register(b0);
float4 Column(ByteAddressBuffer joints, uint4 index, float4 weights, uint column)
{
    const uint last = Skin.JointCount - 1;
    const uint at = Skin.JointOffset + column * 16;
    float4 sum = joints.Load<float4>(at + min(index.x, last) * 64) * weights.x;
    sum += joints.Load<float4>(at + min(index.y, last) * 64) * weights.y;
    sum += joints.Load<float4>(at + min(index.z, last) * 64) * weights.z;
    sum += joints.Load<float4>(at + min(index.w, last) * 64) * weights.w;
    return sum;
}
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Skin.VertexCount)
    {
        return;
    }
    ByteAddressBuffer source = ResourceDescriptorHeap[Skin.Source];
    ByteAddressBuffer joints = ResourceDescriptorHeap[Skin.Joints];
    RWByteAddressBuffer dest = ResourceDescriptorHeap[Skin.Dest];
    const uint from = id.x * 56;
    const float3 position = source.Load<float3>(from);
    const float3 normal = source.Load<float3>(from + 12);
    const float2 uv = source.Load<float2>(from + 24);
    const uint2 packed = source.Load<uint2>(from + 32);
    const float4 weights = source.Load<float4>(from + 40);
    const uint4 index =
        uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
    const float4 x = Column(joints, index, weights, 0);
    const float4 y = Column(joints, index, weights, 1);
    const float4 z = Column(joints, index, weights, 2);
    const float4 w = Column(joints, index, weights, 3);
    const float3 skinned = (x * position.x + y * position.y + z * position.z + w).xyz;
    const float3 turned = (x * normal.x + y * normal.y + z * normal.z).xyz;
    const float size = dot(turned, turned);
    const uint to = Skin.DestOffset + id.x * 32;
    dest.Store<float3>(to, skinned);
    dest.Store<float3>(to + 12, size > 0.0 ? turned * rsqrt(size) : normal);
    dest.Store<float2>(to + 24, uv);
}
