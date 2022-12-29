
#include "Shaders/HLSL_Types.h"

struct PsInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

//For testing

static matrix identity =
{
    { 1, 0, 0, 0 },
    { 0, 1, 0, 0 },
    { 0, 0, 1, 0 },
    { 0, 0, 0, 1 }
};

float4x4 m_translate(float x, float y, float z)
{
    float4x4 m = identity;
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
    return m;
}

PsInput FirstNodeVertexShader(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[global_constant_buffer.instance_buffer_index];
    GpuInstanceData instance = instances[0]; //FCS TODO: Get the correct instance, using our mapping buffer that maps instance_id to index in instances array
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];

    Vertex vertex = vertices[indices[vertex_id]];

    const float4x4 proj_view = mul(global_constant_buffer.projection, global_constant_buffer.view);
    float4 out_position = mul(proj_view, float4(vertex.position, 1));

    PsInput ps_input;
    ps_input.position = out_position;
    ps_input.normal = vertex.normal;
    ps_input.color = vertex.color;
    return ps_input;
}

float4 FirstNodePixelShader(const PsInput input) : SV_TARGET
{
    return float4(input.color, 1);
}