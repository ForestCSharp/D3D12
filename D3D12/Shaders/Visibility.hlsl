
#include "Shaders/HLSL_Types.h"

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

struct DrawConstants
{
	uint instance_buffer_index;
    uint instance_id;
};
ConstantBuffer<DrawConstants> draw_constants : register(b1, space0);

struct PsInput
{
    float4 position : SV_POSITION;
	nointerpolation uint instance_id: TEXCOORD0;
	nointerpolation uint triangle_id: TEXCOORD1;
};

PsInput VertexShader(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    //TODO: Common helper fn to fetch vertex from vertex_id and instance_id
	StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[draw_constants.instance_buffer_index];
	GpuInstanceData instance = instances[draw_constants.instance_id];

    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];
    Vertex vertex = vertices[indices[vertex_id]];

    const float4x4 proj_view = mul(global_constant_buffer.projection, global_constant_buffer.view);
    const float4 world_position = mul(instance.transform, float4(vertex.position, 1));
	const float4 world_normal = mul(instance.transform, float4(vertex.normal, 0)); //FCS TODO: just multiply this by rotation, not entire transform
    const float4 out_position = mul(proj_view, world_position);

    PsInput ps_input;
    ps_input.position = out_position;
	ps_input.instance_id = draw_constants.instance_id;
	ps_input.triangle_id = vertex_id / 3;

    return ps_input;
}

struct PixelOut
{
	uint2 visbuffer;
};

PixelOut PixelShader(const PsInput input) : SV_TARGET
{
	PixelOut out_value;
	out_value.visbuffer[0] = input.instance_id;
	out_value.visbuffer[1] = input.triangle_id;
	return out_value;
}
