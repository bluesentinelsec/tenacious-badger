// dxc: -T vs_6_6 -E main
// Constant buffers taken from the resource heap by an index the vertex
// shader is given: neither is bound, and neither has a layout to report.
struct Camera
{
    float4x4 view_projection;
    float3 position;
};

struct Object
{
    float4x4 world;
    uint camera;
};

float4 main(float3 position : POSITION, uint object_index : OBJECT) : SV_Position
{
    ConstantBuffer<Object> object = ResourceDescriptorHeap[object_index];
    ConstantBuffer<Camera> camera = ResourceDescriptorHeap[object.camera];
    const float4 world = mul(float4(position, 1), object.world);
    return mul(world - float4(camera.position, 0), camera.view_projection);
}
