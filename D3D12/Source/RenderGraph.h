#pragma once

#include <cassert>
#include <cstdint>

#include <map>
#include <vector>
#include <string>
#include <optional>
#include <functional>
#include <variant>

#include <unordered_map>

using std::optional;
using std::string;
using std::function;
using std::variant;
using std::get_if;
using std::vector;
using std::unordered_map;
using std::multimap;

#include <d3d12.h>
#include <wrl.h>

#include "D3D12MemAlloc/D3D12MemAlloc.h"
#include "ShaderCompiler.h"
#include "GpuResources.h"
#include "GpuCommands.h"
#include "Common.h"

using Microsoft::WRL::ComPtr;

struct RenderGraphBufferDesc
{
	UINT size = 0;
	D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON;
};

struct RenderGraphBuffer
{
	// Constructor sets up desc, but doesn't create resource yet
	RenderGraphBuffer(const RenderGraphBufferDesc& in_desc)
		: desc(in_desc)
	{}

	void CreateResource(D3D12MA::Allocator* in_allocator)
	{
		buffer = GpuBuffer(GpuBufferDesc
		{
			.allocator = in_allocator,
			.size = desc.size,
			.heap_type = desc.heap_type,
			.resource_flags = desc.resource_flags,
			.resource_state = desc.resource_state,
		});
	}

	RenderGraphBufferDesc desc;
	optional<GpuBuffer> buffer;
};

struct RenderGraphTextureDesc
{
	UINT width = 0;
	UINT height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON;
	optional<D3D12_CLEAR_VALUE> optimized_clear_value = std::nullopt;
};

struct RenderGraphTexture
{
	// Constructor sets up desc, but doesn't create resource yet
	RenderGraphTexture(const RenderGraphTextureDesc& desc)
		: desc(desc)
	{}

	void CreateResource(D3D12MA::Allocator* in_allocator)
	{
		texture = GpuTexture(GpuTextureDesc
		{
			.allocator = in_allocator,
			.width = desc.width,
			.height = desc.height,
			.format = desc.format,
			.resource_flags = desc.resource_flags,
			.resource_state = desc.resource_state,
			.optimized_clear_value = desc.optimized_clear_value,
		});
	};

	RenderGraphTextureDesc desc;
	optional<GpuTexture> texture;
};

struct RenderGraphOutput
{
	RenderGraphOutput(const RenderGraphBufferDesc& buffer_desc)
		: resource(RenderGraphBuffer(buffer_desc))
	{}

	RenderGraphOutput(const RenderGraphTextureDesc& texture_desc)
		: resource(RenderGraphTexture(texture_desc))
	{}

	void CreateResource(D3D12MA::Allocator* in_allocator)
	{
		if (RenderGraphBuffer* buffer = get_if<RenderGraphBuffer>(&resource))
		{
			buffer->CreateResource(in_allocator);
		}
		else if (RenderGraphTexture* texture = get_if<RenderGraphTexture>(&resource))
		{
			texture->CreateResource(in_allocator);
		}
	}

	ID3D12Resource* GetD3D12Resource()
	{
		if (RenderGraphBuffer* buffer = get_if<RenderGraphBuffer>(&resource))
		{
			return buffer->buffer->GetResource();
		}
		else if (RenderGraphTexture* texture = get_if<RenderGraphTexture>(&resource))
		{
			return texture->texture->GetResource();
		}
		
		assert(false);
		return nullptr;
	}

	D3D12_RESOURCE_STATES GetResourceState()
	{
		if (RenderGraphBuffer* buffer = get_if<RenderGraphBuffer>(&resource))
		{
			return buffer->desc.resource_state;
		}
		else if (RenderGraphTexture* texture = get_if<RenderGraphTexture>(&resource))
		{
			return texture->desc.resource_state;
		}

		assert(false);
		return D3D12_RESOURCE_STATE_COMMON;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE& GetRtvHandle(ComPtr<ID3D12Device5> in_device)
	{
		if (!rtv_data)
		{
			rtv_data = DescriptorData();

			const UINT rtv_heap_offset = in_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			//create texture descriptor heap
			D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
			descriptor_heap_desc.NumDescriptors = 1;
			// This heap contains SRV, UAV or CBVs -- in our case one SRV
			descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			descriptor_heap_desc.NodeMask = 0;
			descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

			HR_CHECK(in_device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&rtv_data->descriptor_heap)));
			rtv_data->descriptor_heap->SetName(TEXT("texture_descriptor_heap_rtv"));
			rtv_data->descriptor_handle = rtv_data->descriptor_heap->GetCPUDescriptorHandleForHeapStart();

			//FCS TODO: This currently has to be a texture 2D
			ID3D12Resource* d3d12_resource = GetD3D12Resource();
			D3D12_RENDER_TARGET_VIEW_DESC rtv_desc =
			{
				.Format = d3d12_resource->GetDesc().Format,
				.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = 
				{
					.MipSlice = 0,
					.PlaneSlice = 0,
				},
			};
			in_device->CreateRenderTargetView(d3d12_resource, &rtv_desc, rtv_data->descriptor_handle);
		}

		return rtv_data->descriptor_handle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE& GetDsvHandle(ComPtr<ID3D12Device5> in_device)
	{
		if (!dsv_data)
		{
			dsv_data = DescriptorData();

			const UINT rtv_heap_offset = in_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			//create texture descriptor heap
			D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
			descriptor_heap_desc.NumDescriptors = 1;
			// This heap contains SRV, UAV or CBVs -- in our case one SRV
			descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			descriptor_heap_desc.NodeMask = 0;
			descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

			HR_CHECK(in_device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&dsv_data->descriptor_heap)));
			dsv_data->descriptor_heap->SetName(TEXT("texture_descriptor_heap_dsv"));
			dsv_data->descriptor_handle = dsv_data->descriptor_heap->GetCPUDescriptorHandleForHeapStart();

			//FCS TODO: This currently has to be a texture 2D
			ID3D12Resource* d3d12_resource = GetD3D12Resource();
			D3D12_RENDER_TARGET_VIEW_DESC rtv_desc =
			{
				.Format = d3d12_resource->GetDesc().Format,
				.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
				.Texture2D =
				{
					.MipSlice = 0,
					.PlaneSlice = 0,
				},
			};

			D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc =
			{
				.Format = d3d12_resource->GetDesc().Format,
				.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
				.Texture2D =
				{
					.MipSlice = 0,
				},
			};

			in_device->CreateDepthStencilView(d3d12_resource, &dsv_desc, dsv_data->descriptor_handle);
		}

		return dsv_data->descriptor_handle;
	}

	variant<RenderGraphBuffer, RenderGraphTexture> resource;

	struct DescriptorData
	{
		ComPtr<ID3D12DescriptorHeap> descriptor_heap;
		D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle = {};
	};

	optional<DescriptorData> rtv_data;
	optional<DescriptorData> dsv_data;
};

struct RenderGraphInput
{
	RenderGraphInput(const RenderGraphBufferDesc& buffer_desc)
		: desc(buffer_desc)
	{}

	RenderGraphInput(const RenderGraphTextureDesc& texture_desc)
		: desc(texture_desc)
	{}

	ID3D12Resource* GetD3D12Resource()
	{
		return incoming_resource ? incoming_resource->GetD3D12Resource() : nullptr;
	}

	D3D12_RESOURCE_STATES GetResourceState()
	{
		return	std::holds_alternative<RenderGraphBufferDesc>(desc)
			? std::get<RenderGraphBufferDesc>(desc).resource_state
			: std::get<RenderGraphTextureDesc>(desc).resource_state;
	}

	variant<RenderGraphBufferDesc, RenderGraphTextureDesc> desc;
	struct RenderGraphOutput* incoming_resource = nullptr;
};

struct RenderGraphNodeDesc
{
	// Node Name
	string name;

	// Compile fn (lambda) //Necessary? (b/w setup and render)
	function<void(struct RenderGraphNode& self)> setup;
	function<void(struct RenderGraphNode& self, ComPtr<ID3D12GraphicsCommandList4>)> execute;
};

struct RenderGraphNode
{
public:
	
	void AddBufferInput(const string& name, const RenderGraphBufferDesc& buffer_desc)
	{
		inputs.insert({name, RenderGraphInput(buffer_desc)});
	}

	void AddTextureInput(const string& name, const RenderGraphTextureDesc& texture_desc)
	{
		inputs.insert({name, RenderGraphInput(texture_desc)});
	}

	void AddBufferOutput(const string& name, const RenderGraphBufferDesc& buffer_desc)
	{
		outputs.insert({name, RenderGraphOutput(buffer_desc)});
	}

	void AddTextureOutput(const string& name, const RenderGraphTextureDesc& texture_desc)
	{
		outputs.insert({ name, RenderGraphOutput(texture_desc)});
	}

	RenderGraphInput& GetInput(const string& name)
	{
		return inputs.find(name)->second;
	}

	RenderGraphOutput& GetOutput(const string& name)
	{
		return outputs.find(name)->second;
	}

private: //Called by friend struct RenderGraph
	RenderGraphNode(const RenderGraphNodeDesc& desc)
		: desc(desc)
	{}

	void Setup(D3D12MA::Allocator* in_allocator)
	{
		desc.setup(*this);
		
		// Create output resources
		for (auto& output : outputs)
		{
			output.second.CreateResource(in_allocator);
		}
	}

	void Execute(struct RenderGraph& render_graph, ComPtr<ID3D12GraphicsCommandList4> in_command_list);
	
private:
	RenderGraphNodeDesc desc;
	unordered_map<string, RenderGraphInput> inputs;
	unordered_map<string, RenderGraphOutput> outputs;

	friend struct RenderGraph;
};

struct RenderGraphEdge
{
	string incoming_node;				// incoming node name
	optional<string> incoming_resource;	// resource name from incoming node's output resources
	string outgoing_node;				// outgoing node name
	optional<string> outgoing_resource;	// resource name from outgoing node's input resources
};

struct RenderGraphDesc
{
	ComPtr<ID3D12Device5> device;
	ComPtr<D3D12MA::Allocator> allocator;
	ComPtr<ID3D12GraphicsCommandList4> command_list;
};

struct RenderGraph
{
	RenderGraph(const RenderGraphDesc& create_info)
		: m_device(create_info.device)
		, m_allocator(create_info.allocator)
		, m_command_list(create_info.command_list)
	{}

	void AddNode(const RenderGraphNodeDesc&& in_desc);

	void AddEdge(const RenderGraphEdge&& in_edge);

	// Works backwards (using edges) to determine graph connectivity to node_names
	vector<RenderGraphNode*> RecurseNodes(const vector<string>& in_node_names, unordered_map<string, size_t> visited_nodes);

	void Execute();

	inline unordered_map<string, RenderGraphNode>& GetNodes() { return nodes; }

	inline multimap<string, RenderGraphEdge>& GetIncomingEdges() { return incoming_edges; }

private:
	// Nodes, keyed by node name
	unordered_map<string, RenderGraphNode> nodes;

	// Key is the "outgoing" node of the edge, so this map represents edges incoming to a given node
	multimap<string, RenderGraphEdge> incoming_edges;

	// Key is the "incoming" node of the edge, so this map represents edges outgoing from a given node
	multimap<string, RenderGraphEdge> outgoing_edges;

private:
	// D3D12 resources
	ComPtr<ID3D12Device5> m_device;
	ComPtr<D3D12MA::Allocator> m_allocator;
	ComPtr<ID3D12GraphicsCommandList4> m_command_list;
};
