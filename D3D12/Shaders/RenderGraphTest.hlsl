
struct PsInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PsInput FirstNodeVertexShader(const float2 position : POSITION, const  float2 uv : TEXCOORD0)
{
    PsInput ps_input;
    ps_input.position = float4(position, 0, 1);
    ps_input.uv = uv;
    return ps_input;
}

float4 FirstNodePixelShader(const PsInput input) : SV_TARGET
{
    float4 dummy = float4(0.5, 0.5, 0.5, 1.0);
    return dummy;
}

[numthreads(1, 1, 1)]
void SecondNodeComputeShader()
{

}