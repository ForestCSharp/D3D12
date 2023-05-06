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
	float3 world_position : TEXCOORD1;
	uint octree_node_index : SV_InstanceID;
};

PsInput VertexShader(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    //TODO: Common helper fn to fetch vertex from vertex_id and instance_id
	StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[draw_constants.instance_buffer_index];
	GpuInstanceData instance = instances[draw_constants.instance_id];

    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];
    Vertex vertex = vertices[indices[vertex_id]];

	StructuredBuffer<OctreeNode> octree = ResourceDescriptorHeap[global_constant_buffer.octree_index];
	const OctreeNode octree_node = octree[instance_id];
	const float3 octree_node_position = (octree_node.min + octree_node.max) / 2.0f;

    const float4x4 proj_view = mul(global_constant_buffer.projection, global_constant_buffer.view);
    float4 world_position = float4(octree_node_position + vertex.position, 1);
	const float4 world_normal = mul(instance.transform, float4(vertex.normal, 0));
    const float4 out_position = mul(proj_view, world_position);

    PsInput ps_input;
	//FCS TODO: Don't even try to render non-leaf octree nodes
	ps_input.position = octree_node.is_leaf ? out_position : float4(-1,-1,-1,1);
    ps_input.normal = world_normal.xyz;
	ps_input.world_position = world_position.xyz;
	ps_input.octree_node_index = instance_id;
    return ps_input;
}

float4 PixelShader(const PsInput input) : SV_TARGET
{
	StructuredBuffer<OctreeNode> octree = ResourceDescriptorHeap[global_constant_buffer.octree_index];
	const OctreeNode octree_node = octree[input.octree_node_index];
	float3 result = SG_Evaluate(octree_node.sg, input.normal);
	return float4(result,1);
}