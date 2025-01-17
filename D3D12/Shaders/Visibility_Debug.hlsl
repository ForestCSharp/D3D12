
#include "Shaders/HLSL_Types.h"

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

struct DrawConstants
{
	uint visibility_buffer_id;
	uint output_buffer_id;
};
ConstantBuffer<DrawConstants> draw_constants : register(b1, space0);

[numthreads(32,32,1)]
void CSMain( uint3 id : SV_DispatchThreadID )
{
	RWTexture2D<float4> visibility_buffer = ResourceDescriptorHeap[draw_constants.visibility_buffer_id];
	RWTexture2D<float4> output_buffer = ResourceDescriptorHeap[draw_constants.output_buffer_id];

	//output_buffer[id.xy] = float4(0, 0, 0, 1);
	output_buffer[id.xy] = visibility_buffer.Load(id.xy) * float4(1.5, 1.5, 1.5, 1.0);
}