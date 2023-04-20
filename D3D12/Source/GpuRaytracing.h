#pragma once

// TODO:
// 0. (Already exists in ShaderCompiler.h) CompileShaderLibrary
// 1. DXR State Object
// 2. TLAS and BLAS
// 3. Shader Table (w/ Shader Records)
// 4. Algo specific buffer creation (already done, just use GpuBuffer)
// 5. Render Graph Node in main.cpp

struct RaytracingStateObject
{
    RaytracingStateObject(ComPtr<IDxcBlob> in_shader_library)
    {
        memset(subobjects, 0, sizeof(subobjects));
    }


    D3D12_STATE_SUBOBJECT subobjects[5];
};
