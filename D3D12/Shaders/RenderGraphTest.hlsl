
#include "Shaders/HLSL_Types.h"

struct PsInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

ConstantBuffer<SceneConstantBuffer> scene_constant_buffer : register(b0, space1);

//For testing

static matrix identity =
{
    { 1, 0, 0, 0 },
    { 0, 1, 0, 0 },
    { 0, 0, 1, 0 },
    { 0, 0, 0, 1 }
};

float4x4 m_translate(float x, float y, float z)
{
    float4x4 m = identity;
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
    return m;
}

PsInput FirstNodeVertexShader(const float3 position : SV_POSITION, const float3 normal : NORMAL, const float3 color : COLOR)
{
    //const float4x4 model_mat = m_translate(0, 0, 2);
    //float4 world_pos = mul(model_mat, float4(position, 1));

    const float4x4 proj_view = mul(scene_constant_buffer.projection, scene_constant_buffer.view);
    float4 out_position = mul(proj_view, float4(position, 1));

    PsInput ps_input;
    ps_input.position = out_position;
    ps_input.normal = normal;
    ps_input.color = color;
    return ps_input;
}

float4 FirstNodePixelShader(const PsInput input) : SV_TARGET
{
    return float4(input.color, 1);
}