
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
    float3 normal : NORMAL;
    float3 color : COLOR;
	float2 texcoord : TEXCOORD;
	float3 world_position : TEXCOORD1;
};

PsInput FirstNodeVertexShader(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    //TODO: Common helper fn to fetch vertex from vertex_id and instance_id
	StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[draw_constants.instance_buffer_index];
	GpuInstanceData instance = instances[draw_constants.instance_id];

    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];
    Vertex vertex = vertices[indices[vertex_id]];

    const float4x4 proj_view = mul(global_constant_buffer.projection, global_constant_buffer.view);
    const float4 world_position = mul(instance.transform, float4(vertex.position, 1));
	const float4 world_normal = mul(instance.transform, float4(vertex.normal, 0));
    const float4 out_position = mul(proj_view, world_position);

    PsInput ps_input;
    ps_input.position = out_position;
    ps_input.normal = world_normal.xyz;
	ps_input.color = vertex.color;
	ps_input.texcoord = vertex.texcoord;
	ps_input.world_position = world_position.xyz;
    return ps_input;
}

float4 FirstNodePixelShader(const PsInput input) : SV_TARGET
{
	OctreeNode octree_node;
	if (Octree_Search(global_constant_buffer.octree_index, input.world_position, octree_node))
	{
		float3 result = SG_Evaluate(octree_node.sg, input.normal);
		return float4(result, 1);
	}

	return float4(0,0,0,0);
}