#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

#include <fstream>
#include <sstream>
#include <vector>
#include <d3d12.h>
#include <d3d12shader.h>
#include "../dxc/inc/dxcapi.h"
#include "Common.h"

using STL_IMPL::vector;

struct ShaderCompilerContext
{
	static const ShaderCompilerContext& Get()
	{
		if (!instance)
		{
			instance = new ShaderCompilerContext();
		}

		return *instance;
	}

	ShaderCompilerContext()
	{
		HR_CHECK(DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&compiler));
		HR_CHECK(DxcCreateInstance(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils));
		HR_CHECK(utils->CreateDefaultIncludeHandler(&include_handler));
	}

	ShaderCompilerContext(ShaderCompilerContext& other) = delete;
	void operator=(const ShaderCompilerContext&) = delete;

	ComPtr<IDxcCompiler3> compiler = nullptr;
	ComPtr<IDxcUtils> utils = nullptr;
	ComPtr<IDxcIncludeHandler> include_handler = nullptr;

private:
	static ShaderCompilerContext* instance;
};

struct ShaderCompilerOutput
{
	ComPtr<IDxcBlob> blob;
	ComPtr<IDxcResult> result;
};

inline ShaderCompilerOutput CompileShader(const LPCWSTR in_file_path, const LPCWSTR in_entry_point, const LPCWSTR in_target_profile)
{
	const ShaderCompilerContext& context = ShaderCompilerContext::Get();

	// Open and read the file
	const std::ifstream shaderFile(in_file_path);
	if (shaderFile.good() == false)
	{
		throw std::logic_error("Cannot find shader file");
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string sShader = strStream.str();

	// Create blob from the string
	IDxcBlobEncoding* pTextBlob = nullptr;
	HR_CHECK(context.utils->CreateBlob((LPBYTE)sShader.c_str(), (uint32_t)sShader.size(), CP_UTF8, &pTextBlob));

	vector<LPCWSTR> arguments;
	//-E for the entry point (eg. PSMain)
	arguments.push_back(L"-E");
	arguments.push_back(in_entry_point);

	//-T for the target profile (eg. ps_6_2)
	arguments.push_back(L"-T");
	arguments.push_back(in_target_profile);

	//Strip reflection data and pdbs (see later)
	//arguments.push_back(L"-Qstrip_debug");
	//arguments.push_back(L"-Qstrip_reflect");

	arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS); //-WX
	arguments.push_back(DXC_ARG_DEBUG); //-Zi

	DxcBuffer sourceBuffer = {};
	sourceBuffer.Ptr = pTextBlob->GetBufferPointer();
	sourceBuffer.Size = pTextBlob->GetBufferSize();
	sourceBuffer.Encoding = 0;

	// Compile
	ComPtr<IDxcResult> pResult;
	HR_CHECK(context.compiler->Compile(&sourceBuffer, arguments.data(), (UINT32)arguments.size(), context.include_handler.Get(), IID_PPV_ARGS(pResult.GetAddressOf())));

	ComPtr<IDxcBlobUtf8> pErrors;
	HR_CHECK(pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr));
	if (pErrors && pErrors->GetStringLength() > 0)
	{
		printf("CompileShader Error: %s\n", (char*)pErrors->GetBufferPointer());
		throw std::logic_error("Failed compile shader");
	}

	IDxcBlob* pBlob;
	HR_CHECK(pResult->GetResult(&pBlob));

	return ShaderCompilerOutput
	{
		.blob = pBlob,
		.result = pResult,
	};
}

template <class T> ComPtr<T> GetReflection(const ShaderCompilerOutput& in_shader_output)
{
	ComPtr<IDxcBlob> pReflectionData;
	HR_CHECK(in_shader_output.result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(pReflectionData.GetAddressOf()), nullptr));
	DxcBuffer reflectionBuffer = {};
	reflectionBuffer.Ptr = pReflectionData->GetBufferPointer();
	reflectionBuffer.Size = pReflectionData->GetBufferSize();
	reflectionBuffer.Encoding = 0;
	ComPtr<T> pShaderReflection;
	const ShaderCompilerContext& context = ShaderCompilerContext::Get();
	context.utils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(pShaderReflection.GetAddressOf()));
	return pShaderReflection;
}

inline ComPtr<IDxcBlob> CompileShaderLibrary(const LPCWSTR in_file_path)
{
	ShaderCompilerOutput output = CompileShader(in_file_path, L"", L"lib_6_6");

	//ComPtr<ID3D12LibraryReflection> library_reflection = GetReflection<ID3D12LibraryReflection>(output);

	return output.blob;
}

struct CompiledShader
{
	ComPtr<IDxcBlob> blob;
	ComPtr<ID3D12ShaderReflection> reflection;
};

inline CompiledShader CompileVertexShader(const LPCWSTR in_file_path, const LPCWSTR in_entry_point)
{
	ShaderCompilerOutput output = CompileShader(in_file_path, in_entry_point, L"vs_6_6");

	ComPtr<ID3D12ShaderReflection> shader_reflection = GetReflection<ID3D12ShaderReflection>(output);

	return CompiledShader
	{
		.blob = output.blob,
		.reflection = shader_reflection,
	};
}

inline CompiledShader CompilePixelShader(const LPCWSTR in_file_path, const LPCWSTR in_entry_point)
{
	ShaderCompilerOutput output = CompileShader(in_file_path, in_entry_point, L"ps_6_6");

	ComPtr<ID3D12ShaderReflection> shader_reflection = GetReflection<ID3D12ShaderReflection>(output);
	
	return CompiledShader
	{
		.blob = output.blob,
		.reflection = shader_reflection,
	};
}

inline CompiledShader CompileComputeShader(const LPCWSTR in_file_path, const LPCWSTR in_entry_point)
{
	ShaderCompilerOutput output = CompileShader(in_file_path, in_entry_point, L"cs_6_6");

	ComPtr<ID3D12ShaderReflection> shader_reflection = GetReflection<ID3D12ShaderReflection>(output);

	return CompiledShader
	{
		.blob = output.blob,
		.reflection = shader_reflection,
	};
}