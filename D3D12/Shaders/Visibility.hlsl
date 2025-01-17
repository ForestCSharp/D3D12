
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

	//float3 normal : NORMAL;
	//float3 color : COLOR;
	//float2 texcoord : TEXCOORD;
	//float3 world_position : TEXCOORD1;
};

//FCS TODO: Just Output 

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

 	//ps_input.normal = world_normal.xyz;
	//ps_input.color = vertex.color;
	//ps_input.texcoord = vertex.texcoord;
	//ps_input.world_position = world_position.xyz;

    return ps_input;
}

#define NUM_INSTANCE_COLORS 12

float4 PixelShader(const PsInput input) : SV_TARGET
{
	//float4 instance_colors[NUM_INSTANCE_COLORS] = 
	//{
	//	float4(1,0,0,1),
	//	float4(0,1,0,1),
	//	float4(0,0,1,1),
	//	float4(1,1,0,1),
	//	float4(1,0,1,1),
	//	float4(0,1,1,1),
	//	float4(0.5, 0, 0, 1),
	//	float4(0, 0.5, 0, 1),
	//	float4(0, 0, 0.5, 1),
	//	float4(0.5, 0.5, 0, 1),
	//	float4(0.5, 0, 0.5, 1),
	//	float4(0, 0.5, 0.5, 1)
	//};

	//float4 out_color = instance_colors[input.instance_id % NUM_INSTANCE_COLORS];
	//float4 out_color = instance_colors[input.triangle_id % NUM_INSTANCE_COLORS];

	float4 out_color = float4(0,0,0,1);
	out_color.r = input.instance_id;
	out_color.g = input.triangle_id;
	return out_color;
}
