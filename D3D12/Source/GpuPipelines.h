#pragma once

#include <cassert>
#include <windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <vector>
#include <optional>
#include <wrl.h>
#include <cstdint>
#include <string>
#include "../dxc/inc/dxcapi.h"
#include "debugapi.h"

#include "Common.h"
#include "ShaderCompiler.h"

using STL_IMPL::optional;
using STL_IMPL::vector;
using STL_IMPL::wstring;
using Microsoft::WRL::ComPtr;

struct GraphicsPipelineBuilder
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;

	optional<CompiledShader> vertex_shader;
	optional<CompiledShader> pixel_shader;
	vector<D3D12_INPUT_ELEMENT_DESC> input_element_descs;

	wstring debug_name;

	GraphicsPipelineBuilder()
	{
		pso_desc = {};

		pso_desc.RasterizerState = {};
		pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
		pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		pso_desc.RasterizerState.DepthClipEnable = TRUE;
		pso_desc.RasterizerState.MultisampleEnable = FALSE;
		pso_desc.RasterizerState.AntialiasedLineEnable = FALSE;
		pso_desc.RasterizerState.ForcedSampleCount = 0;
		pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pso_desc.DepthStencilState.DepthEnable = FALSE;
		pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		pso_desc.DepthStencilState.StencilEnable = FALSE;

		pso_desc.BlendState = {};
		pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
		pso_desc.BlendState.IndependentBlendEnable = FALSE;
		for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
		{
			pso_desc.BlendState.RenderTarget[i] =
			{
				FALSE,FALSE,
				D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
				D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
				D3D12_LOGIC_OP_NOOP,
				D3D12_COLOR_WRITE_ENABLE_ALL,
			};
		}

		pso_desc.SampleMask = UINT_MAX;

		pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

		pso_desc.SampleDesc.Count = 1;
	}

	GraphicsPipelineBuilder& with_root_signature(const ComPtr<ID3D12RootSignature> in_root_signature)
	{
		pso_desc.pRootSignature = in_root_signature.Get();
		return *this;
	}

	GraphicsPipelineBuilder& with_vs(const CompiledShader& in_shader)
	{
		vertex_shader = in_shader;
		pso_desc.VS.pShaderBytecode = vertex_shader->blob->GetBufferPointer();
		pso_desc.VS.BytecodeLength = vertex_shader->blob->GetBufferSize();
		return *this;
	}

	GraphicsPipelineBuilder& with_ps(const CompiledShader& in_shader)
	{
		pixel_shader = in_shader;
		pso_desc.PS.pShaderBytecode = pixel_shader->blob->GetBufferPointer();
		pso_desc.PS.BytecodeLength = pixel_shader->blob->GetBufferSize();
		return *this;
	}

	GraphicsPipelineBuilder& with_rtv_formats(const std::initializer_list<DXGI_FORMAT> in_rtv_formats)
	{
		assert(in_rtv_formats.size() <= 8);
		vector<DXGI_FORMAT> rtv_formats(in_rtv_formats);

		pso_desc.NumRenderTargets = static_cast<UINT>(rtv_formats.size());
		for (UINT i = 0; i < pso_desc.NumRenderTargets; ++i)
		{
			pso_desc.RTVFormats[i] = rtv_formats[i];
		}

		return *this;
	}

	GraphicsPipelineBuilder& with_dsv_format(const DXGI_FORMAT in_format)
	{
		pso_desc.DSVFormat = in_format;
		return *this;
	}

	GraphicsPipelineBuilder& with_depth_enabled(const bool in_enable_depth)
	{
		pso_desc.DepthStencilState.DepthEnable = in_enable_depth;
		return *this;
	}

	GraphicsPipelineBuilder& with_primitive_topology(const D3D12_PRIMITIVE_TOPOLOGY_TYPE in_primitive_topology)
	{
		pso_desc.PrimitiveTopologyType = in_primitive_topology;
		return *this;
	}

	GraphicsPipelineBuilder& with_cull_mode(const D3D12_CULL_MODE in_cull_mode)
	{
		pso_desc.RasterizerState.CullMode = in_cull_mode;
		return *this;
	}

	GraphicsPipelineBuilder& with_debug_name(wstring in_debug_name)
	{
		debug_name = in_debug_name;
		return *this;
	}

	static DXGI_FORMAT get_format_from_parameter_reflection(const D3D12_SIGNATURE_PARAMETER_DESC& in_reflection)
	{
		if (in_reflection.Mask == 1)
		{
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_UINT32) { return DXGI_FORMAT_R32_UINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_SINT32) { return DXGI_FORMAT_R32_SINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) { return DXGI_FORMAT_R32_FLOAT; }
		}
		else if (in_reflection.Mask <= 3)
		{
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_UINT32) { return DXGI_FORMAT_R32G32_UINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_SINT32) { return DXGI_FORMAT_R32G32_SINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) { return DXGI_FORMAT_R32G32_FLOAT; }
		}
		else if (in_reflection.Mask <= 7)
		{
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_UINT32) { return DXGI_FORMAT_R32G32B32_UINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_SINT32) { return DXGI_FORMAT_R32G32B32_SINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) { return DXGI_FORMAT_R32G32B32_FLOAT; }
		}
		else if (in_reflection.Mask <= 15)
		{
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_UINT32) { return DXGI_FORMAT_R32G32B32A32_UINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_SINT32) { return DXGI_FORMAT_R32G32B32A32_SINT; }
			if (in_reflection.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) { return DXGI_FORMAT_R32G32B32A32_FLOAT; }
		}

		return DXGI_FORMAT_UNKNOWN;
	}

	ComPtr<ID3D12PipelineState> build(ComPtr<ID3D12Device> device)
	{
		assert(vertex_shader && pixel_shader);
		assert(pso_desc.pRootSignature != nullptr);
		assert(pso_desc.VS.pShaderBytecode != nullptr);
		assert(pso_desc.PS.pShaderBytecode != nullptr);

		ComPtr<ID3D12ShaderReflection> vertex_shader_reflection = vertex_shader->reflection;

		D3D12_SHADER_DESC vertex_shader_desc;
		HR_CHECK(vertex_shader_reflection->GetDesc(&vertex_shader_desc));

		//Set InputLayout from vertex-shader inputs
		input_element_descs.clear();
		input_element_descs.reserve(vertex_shader_desc.InputParameters);
		for (UINT i = 0; i < vertex_shader_desc.InputParameters; ++i)
		{
			D3D12_SIGNATURE_PARAMETER_DESC input_element_reflection;
			HR_CHECK(vertex_shader_reflection->GetInputParameterDesc(i, &input_element_reflection));

			D3D12_INPUT_ELEMENT_DESC input_element_desc = {};
			input_element_desc.SemanticName = input_element_reflection.SemanticName;
			input_element_desc.SemanticIndex = input_element_reflection.SemanticIndex;
			input_element_desc.Format = get_format_from_parameter_reflection(input_element_reflection);
			input_element_desc.InputSlot = 0;
			input_element_desc.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
			input_element_desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
			input_element_desc.InstanceDataStepRate = 0;
			input_element_descs.push_back(input_element_desc);
		}

		pso_desc.InputLayout.NumElements = static_cast<UINT>(input_element_descs.size());
		pso_desc.InputLayout.pInputElementDescs = input_element_descs.data();

		D3D12_SIGNATURE_PARAMETER_DESC input_parameter_desc;
		HR_CHECK(vertex_shader_reflection->GetInputParameterDesc(0, &input_parameter_desc));

		ComPtr<ID3D12PipelineState> out_pipeline_state;
		HR_CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&out_pipeline_state)));
		if (!debug_name.empty())
		{
			out_pipeline_state->SetName(debug_name.c_str());
		}
		return out_pipeline_state;
	}
};