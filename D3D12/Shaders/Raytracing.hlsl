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

#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#include "Shaders/HLSL_Types.h"
#include "Shaders/Random.hlsl"
#include "Shaders/Atmosphere.hlsl"
#include "Shaders/ACES.hlsl"

typedef BuiltInTriangleIntersectionAttributes IntersectionAttributes;

ConstantBuffer<GlobalConstantBuffer> global_constant_buffer : register(b0, space1);

//TODO: 
// Use PrimitiveIndex to index into vertices to retrieve vertex data
// Per-Instance data (via instance ID of TLAS (big buffer of per-instance data)

[shader("raygeneration")]
void Raygen()
{
    RWTexture2D<float4> LightingBuffer = ResourceDescriptorHeap[global_constant_buffer.lighting_buffer_index]; //Accumulate data here
    RWTexture2D<float4> RenderTarget = ResourceDescriptorHeap[global_constant_buffer.output_buffer_index];
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[global_constant_buffer.tlas_buffer_index];

    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.f - 1.f);

    // Perspective Camera
    RayDesc ray;
    ray.Origin = mul(global_constant_buffer.view_inverse, float4(0, 0, 0, 1)).xyz;
    float4 target = mul(global_constant_buffer.projection_inverse, float4(d.x, -d.y, 1, 1));
    ray.Direction = mul(global_constant_buffer.view_inverse, float4(target.xyz, 0)).xyz;
    ray.TMin = 0.001;
    ray.TMax = 100000.0;
    RayPayload payload = { float4(0, 0, 0, 0) };
    TraceRay(tlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);

    //Note: a very basic path-tracer-esque test, accumulated over multiple frames
    if (global_constant_buffer.frames_rendered <= 1)
    {
        LightingBuffer[DispatchRaysIndex().xy] = payload.color;
    }
    else
    {
        LightingBuffer[DispatchRaysIndex().xy] += payload.color;
    }

    const float divisor = global_constant_buffer.frames_rendered > 0 ? float(global_constant_buffer.frames_rendered) : 1;
    RenderTarget[DispatchRaysIndex().xy] = LightingBuffer[DispatchRaysIndex().xy] / divisor;

    //uint seed = hash3(uint3(DispatchRaysIndex().xy, global_constant_buffer.random));
    //RenderTarget[DispatchRaysIndex().xy] = float4(abs(randf_in_unit_sphere(seed)), 1);
}

#define HIT_ATTRIBUTE(vertex_array, attribute, barycentrics) vertex_array[0].attribute +\
barycentrics.x * (vertex_array[1].attribute - vertex_array[0].attribute) +\
barycentrics.y * (vertex_array[2].attribute - vertex_array[0].attribute);

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in IntersectionAttributes attr)
{
    StructuredBuffer<GpuInstanceData> instances = ResourceDescriptorHeap[global_constant_buffer.instance_buffer_index];
    GpuInstanceData instance = instances[0]; //TODO: Testing instances buffer
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[instance.index_buffer_index];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[instance.vertex_buffer_index];

    uint triangles_per_primitive = 3;
    uint first_index = PrimitiveIndex() * triangles_per_primitive;

    Vertex vertex0 = vertices[indices[first_index + 0]];
    Vertex vertex1 = vertices[indices[first_index + 1]];
    Vertex vertex2 = vertices[indices[first_index + 2]];
    Vertex vertex_array[3] = { vertex0, vertex1, vertex2 };
    float3 hit_color = HIT_ATTRIBUTE(vertex_array, color, attr.barycentrics);
    float3 hit_normal = HIT_ATTRIBUTE(vertex_array, normal, attr.barycentrics);
    /*payload.color = float4(hit_color, 1);*/

    //Spawn a shadow ray
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[global_constant_buffer.tlas_buffer_index];
    const float3 sun_dir = normalize(global_constant_buffer.sun_dir.xyz);

    RayDesc secondary_ray;
    secondary_ray.Origin = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    uint seed = hash3(uint3(DispatchRaysIndex().xy, global_constant_buffer.random));
    secondary_ray.Direction = randf_in_hemisphere(hit_normal, seed);
    secondary_ray.TMin = 0.01;
    secondary_ray.TMax = 100000.0;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(
        tlas,
        0 /*flags*/,
        1  /*instance mask*/,
        secondary_ray
    );

    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        //uint first_index = q.CommittedPrimitiveIndex() * triangles_per_primitive;
        //float2 barycentrics = q.CommittedTriangleBarycentrics();

        //Vertex vertex0 = vertices[indices[first_index + 0]];
        //Vertex vertex1 = vertices[indices[first_index + 1]];
        //Vertex vertex2 = vertices[indices[first_index + 2]];
        //Vertex vertex_array[3] = { vertex0, vertex1, vertex2 };
        //float3 secondary_hit_color = HIT_ATTRIBUTE(vertex_array, color, barycentrics);

        //payload.color = float4(secondary_hit_color, 1);
    }
    else
    {
        payload.color = float4(hit_color, 1);
    }

    //if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    //{
    //    RayDesc tertiary_ray;
    //    tertiary_ray.Origin = q.WorldRayOrigin() + q.CommittedRayT() * q.WorldRayDirection();
    //    tertiary_ray.Direction = sun_dir;
    //    tertiary_ray.TMin = 0.01;
    //    tertiary_ray.TMax = 100000.0;

    //    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q2;
    //    q2.TraceRayInline(
    //        tlas,
    //        0 /*flags*/,
    //        1  /*instance mask*/,
    //        tertiary_ray
    //    );

    //    q2.Proceed();
    //    if (q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    //    {
    //        payload.color = float4(0, 0, 0, 0);
    //    }
    //}
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    const float3 sun_dir = normalize(global_constant_buffer.sun_dir.xyz);
    const float3 ray_dir = normalize(WorldRayDirection());

    // Felix Atmosphere
    float3 atmosphere_color = AtmosphereColor(WorldRayOrigin(), ray_dir, sun_dir);

    // Sun Disk (From Kajiya - Tomasz Stachowiak AKA h3r2tic)
    {
        const float sun_angular_radius = 0.53 * 0.5 * PI / 180.0;
        const float sun_angular_radius_cos = cos(sun_angular_radius);

        // Conserve the sun's energy by making it dimmer as it increases in size
        // Note that specular isn't quite correct with this since we're not using area lights.
        float current_sun_angular_radius = acos(sun_angular_radius_cos);
        float sun_radius_ratio = sun_angular_radius / current_sun_angular_radius;

        if (dot(ray_dir, sun_dir) > sun_angular_radius_cos)
        {
            atmosphere_color += 800 * sun_color_in_direction(ray_dir) * pow(sun_radius_ratio, 2);
        }
    }

    atmosphere_color = ACESFitted(atmosphere_color); //FCS TODO: do this in post-process pass
    payload.color = float4(atmosphere_color, 1);
}

#endif // RAYTRACING_HLSL