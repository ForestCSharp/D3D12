//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#ifndef HLSL_TYPES_H
#define HLSL_TYPES_H

#ifdef __cplusplus
#include <d3d12.h>
#include "SimpleMath/SimpleMath.h"
using namespace DirectX::SimpleMath;

typedef Matrix float4x4;

typedef Vector4 float4;
typedef Vector3 float3;
typedef Vector2 float2;

typedef UINT uint;
struct uint4 { uint x, y, z, w; };
struct uint3 { uint x, y, z;  };
struct uint2 { uint x, y; };
#endif

#include "SG.h"

struct Vertex
{
	float3 position;
	float3 normal;
	float3 color;
	float2 texcoord;

#ifdef __cplusplus
	Vertex() = default;
    Vertex(float3 position, float3 normal, float3 color)
        : position(position)
        , normal(normal)
        , color(color)
    {}
#endif
};

struct Viewport
{
    float left;
    float top;
    float right;
    float bottom;
};

struct GpuInstanceData
{
    float4x4 transform;
    uint vertex_buffer_index;
    uint index_buffer_index;
};

#ifdef __cplusplus
#define INDIRECT_DRAW_ARGS D3D12_DRAW_ARGUMENTS
#else
#define INDIRECT_DRAW_ARGS uint4
#endif

struct IndirectDrawData
{
	// Index in our resource descriptor heap to our instances buffer
	uint instance_buffer_index;

	// Which instance in our instance buffer are we drawing
	uint instance_id;
	
	// Bindless index to instance buffer
	//uint instance_buffer_index; //FCS TODO:

	// Our actual Indirect Draw Args
	INDIRECT_DRAW_ARGS draw_arguments;
};

struct GlobalConstantBuffer
{
    // Camera data
    float4x4 view;
    float4x4 view_inverse;
    float4x4 projection;
    float4x4 projection_inverse;

    // Old Raytracing-pass specific data
    float4 sun_dir;
    uint lighting_buffer_index;
    uint output_buffer_index;
    uint tlas_buffer_index;
    uint frames_rendered;
    uint random;

	//SG octree
	uint octree_index;
};

struct RayPayload
{
    float4 color;
};

static uint NUM_BINDLESS_DESCRIPTORS_PER_TYPE = 32768;

#endif // #ifndef HLSL_TYPES_H