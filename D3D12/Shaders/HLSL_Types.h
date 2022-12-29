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

#ifndef RAYTRACINGHLSLCOMPAT_H
#define RAYTRACINGHLSLCOMPAT_H

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

struct Vertex
{
    float3 position;
    float3 normal;
    float3 color;

#ifdef __cplusplus
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
    uint vertex_buffer_index;
    uint index_buffer_index;
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

    // Gpu Scene
    uint instance_buffer_index;
    uint instance_buffer_count;
    // uint draw_to_instance_buffer_index; //Maps DrawInstanced instance ID to an index in instance_buffer
    // uint draw_to_instance_buffer_count;
};

struct RayPayload
{
    float4 color;
};

static uint NUM_BINDLESS_DESCRIPTORS_PER_TYPE = 8192;

#endif // RAYTRACINGHLSLCOMPAT_H