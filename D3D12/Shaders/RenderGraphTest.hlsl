
#include "Shaders/HLSL_Types.h"

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

struct PsInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

PsInput FirstNodeVertexShader(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    //TODO: Common helper fn to fetch vertex from vertex_id and instance_id
    StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[global_constant_buffer.instance_buffer_index];
    GpuInstanceData instance = instances[0]; //FCS TODO: Get the correct instance, using our mapping buffer that maps instance_id to index in instances array
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];
    Vertex vertex = vertices[indices[vertex_id]];

    const float4x4 proj_view = mul(global_constant_buffer.projection, global_constant_buffer.view);
    float4 world_position = mul(instance.world_matrix, float4(vertex.position, 1));
    float4 out_position = mul(proj_view, world_position);

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