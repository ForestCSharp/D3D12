
#include "Shaders/HLSL_Types.h"

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

struct DrawConstants
{
	uint visibility_buffer_id;
	uint output_buffer_id;
	uint num_instances;
};
ConstantBuffer<DrawConstants> draw_constants : register(b1, space0);

#define NUM_INSTANCE_COLORS 12

[numthreads(32,32,1)]
void CSMain( uint3 id : SV_DispatchThreadID )
{
	RWTexture2D<uint2> visibility_buffer = ResourceDescriptorHeap[draw_constants.visibility_buffer_id];
	RWTexture2D<float4> output_buffer = ResourceDescriptorHeap[draw_constants.output_buffer_id];

	float4 instance_colors[NUM_INSTANCE_COLORS] = 
	{
		float4(1,0,0,1),
		float4(0,1,0,1),
		float4(0,0,1,1),
		float4(1,1,0,1),
		float4(1,0,1,1),
		float4(0,1,1,1),
		float4(0.5, 0, 0, 1),
		float4(0, 0.5, 0, 1),
		float4(0, 0, 0.5, 1),
		float4(0.5, 0.5, 0, 1),
		float4(0.5, 0, 0.5, 1),
		float4(0, 0.5, 0.5, 1)
	};

	uint2 visbuffer_value = visibility_buffer.Load(id.xy);
	uint instance_id = visbuffer_value.x;
	uint triangle_id = visbuffer_value.y;

	if (instance_id < draw_constants.num_instances)
	{
		output_buffer[id.xy] = instance_colors[instance_id % NUM_INSTANCE_COLORS];
	}
	else
	{
		output_buffer[id.xy] = float4(0, 0, 0, 1);
	}
}
