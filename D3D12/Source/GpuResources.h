#pragma once

#include <cassert>
#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <optional>
#include <wrl.h>
#include <cstdint>

#include "Common.h"
#include "../Shaders/HLSL_Types.h"
#include "D3D12MemAlloc/D3D12MemAlloc.h"

using STL_IMPL::optional;
using STL_IMPL::vector;

using Microsoft::WRL::ComPtr;

//TODO: Staging buffer uploads

struct GpuBufferDesc
{
	D3D12MA::Allocator* allocator = nullptr;
	uint64_t size = 0;
	D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON;
};

struct GpuBuffer
{
public:
	GpuBuffer() {}
	GpuBuffer(const GpuBufferDesc& in_desc)
	{
		assert(in_desc.allocator);
		assert(in_desc.size > 0);

		D3D12MA::ALLOCATION_DESC alloc_desc = {};
		alloc_desc.HeapType = in_desc.heap_type;

		m_resource_desc = {};
		m_resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		m_resource_desc.Alignment = 0;
		m_resource_desc.Width = in_desc.size;
		m_resource_desc.Height = 1;
		m_resource_desc.DepthOrArraySize = 1;
		m_resource_desc.MipLevels = 1;
		m_resource_desc.Format = DXGI_FORMAT_UNKNOWN;
		m_resource_desc.SampleDesc.Count = 1;
		m_resource_desc.SampleDesc.Quality = 0;
		m_resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		m_resource_desc.Flags = in_desc.resource_flags;

		HR_CHECK(in_desc.allocator->CreateResource(
			&alloc_desc,
			&m_resource_desc,
			in_desc.resource_state,
			nullptr,
			&m_allocation,
			IID_PPV_ARGS(&m_resource)
		));
	};

	bool IsValid() { return m_resource != nullptr && m_resource_desc.Width > 0; }
	ID3D12Resource* GetResource() const { return m_resource.Get(); }
	size_t GetSize() const { return m_resource_desc.Width; }
	D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return GetResource()->GetGPUVirtualAddress(); }

	void Map(void** ppData)
	{
		D3D12_RANGE read_range = { 0, 0 };
		HR_CHECK(m_resource->Map(0, &read_range, ppData));
	}

	void Write(void* in_data, size_t data_size)
	{
		//TODO: check/assert that this buffer is cpu writeable...
		UINT8* mapped_data = nullptr;
		D3D12_RANGE read_range = { 0, 0 };
		HR_CHECK(m_resource->Map(0, &read_range, reinterpret_cast<void**>(&mapped_data)));
		memcpy(mapped_data, in_data, data_size);
		m_resource->Unmap(0, nullptr);
	}

protected:
	D3D12_RESOURCE_DESC m_resource_desc = {};
	ComPtr<ID3D12Resource> m_resource;
	ComPtr<D3D12MA::Allocation> m_allocation;
	optional<uint32_t> m_bindless_resource_index;

	friend struct BindlessResourceManager;
};

struct GpuTextureDesc
{
	D3D12MA::Allocator* allocator = nullptr;
	UINT64 width = 0;
	UINT height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON;
	optional<D3D12_CLEAR_VALUE> optimized_clear_value = STL_IMPL::nullopt;
};

struct GpuTexture
{
public:
	GpuTexture() {}

	GpuTexture(const GpuTextureDesc& in_desc)
	{
		assert(in_desc.allocator);
		assert(in_desc.width > 0);
		assert(in_desc.height > 0);

		D3D12MA::ALLOCATION_DESC alloc_desc = {};
		alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

		m_resource_desc = {};
		m_resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		m_resource_desc.Alignment = 0;
		m_resource_desc.Width = in_desc.width;
		m_resource_desc.Height = in_desc.height;
		m_resource_desc.DepthOrArraySize = 1;
		m_resource_desc.MipLevels = 1;
		m_resource_desc.Format = in_desc.format;
		m_resource_desc.SampleDesc.Count = 1;
		m_resource_desc.SampleDesc.Quality = 0;
		m_resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		m_resource_desc.Flags = in_desc.resource_flags;

		//if (m_resource_desc.Flags & )
		D3D12_CLEAR_VALUE clear_value = {};
		const bool has_clear_value = in_desc.optimized_clear_value.has_value();
		if (has_clear_value)
		{
			clear_value = *in_desc.optimized_clear_value;
		}

		HR_CHECK(in_desc.allocator->CreateResource(
			&alloc_desc,
			&m_resource_desc,
			in_desc.resource_state,
			has_clear_value ? &clear_value : nullptr,
			&m_allocation,
			IID_PPV_ARGS(&m_resource)
		));
	}

	bool IsValid() { return m_resource != nullptr; }
	ID3D12Resource* GetResource() const { return m_resource.Get(); }
	D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return GetResource()->GetGPUVirtualAddress(); }
protected:
	D3D12_RESOURCE_DESC m_resource_desc = {};
	ComPtr<ID3D12Resource> m_resource;
	ComPtr<D3D12MA::Allocation> m_allocation;
	optional<uint32_t> m_bindless_resource_index;

	friend struct BindlessResourceManager;
};

struct BindlessResourceManager
{
public:
	BindlessResourceManager(ComPtr<ID3D12Device5> in_device)
		:m_device(in_device)
	{
		D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
		descriptor_heap_desc.NumDescriptors = NUM_BINDLESS_DESCRIPTORS_PER_TYPE; //CBV, SRV, UAV
		descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		descriptor_heap_desc.NodeMask = 0;
		m_device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&m_descriptor_heap));
		m_descriptor_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	UINT32 RegisterCBV(GpuBuffer& constant_buffer)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE cbuffer_cpu_handle;
		constant_buffer.m_bindless_resource_index = AllocateDescriptor(&cbuffer_cpu_handle);
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc =
		{
			.BufferLocation = constant_buffer.GetGPUVirtualAddress(),
			.SizeInBytes = static_cast<UINT>(constant_buffer.GetSize()),
		};
		m_device->CreateConstantBufferView(&cbv_desc, cbuffer_cpu_handle);
		return *constant_buffer.m_bindless_resource_index;
	}

	//TODO: UINT32 RegisterUAV(Buffer& buffer)

	UINT32 RegisterUAV(GpuTexture& texture)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle;
		texture.m_bindless_resource_index = AllocateDescriptor(&uav_cpu_handle);
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		m_device->CreateUnorderedAccessView(texture.GetResource(), nullptr, &UAVDesc, uav_cpu_handle);
		return *texture.m_bindless_resource_index;
	}

	UINT32 RegisterSRV(GpuBuffer& buffer, UINT32 num_elements, UINT32 element_size)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle;
		buffer.m_bindless_resource_index = AllocateDescriptor(&srv_cpu_handle);
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		if (element_size == 0)
		{
			SRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			SRVDesc.Buffer =
			{
				.FirstElement = 0,
				.NumElements = num_elements,
				.StructureByteStride = 0,
			};
		}
		else
		{
			SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
			SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			SRVDesc.Buffer =
			{
				.FirstElement = 0,
				.NumElements = num_elements,
				.StructureByteStride = element_size,
			};
		}
		m_device->CreateShaderResourceView(buffer.GetResource(), &SRVDesc, srv_cpu_handle);
		return *buffer.m_bindless_resource_index;
	}

	//TODO: RegisterSRV(Texture& texture, ...)

	UINT32 RegisterAccelerationStructure(GpuBuffer& acceleration_structure)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE accel_structure_cpu_handle;
		acceleration_structure.m_bindless_resource_index = AllocateDescriptor(&accel_structure_cpu_handle);
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc =
		{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.RaytracingAccelerationStructure =
			{
				.Location = acceleration_structure.GetGPUVirtualAddress(),
			}
		};
		m_device->CreateShaderResourceView(nullptr, &SRVDesc, accel_structure_cpu_handle);
		return *acceleration_structure.m_bindless_resource_index;
	}

	void UnregisterResource(GpuBuffer& buffer)
	{
		assert(buffer.m_bindless_resource_index.has_value());
		m_free_list.push_back(*buffer.m_bindless_resource_index);
		buffer.m_bindless_resource_index.reset();
	}

	void UnregisterResource(GpuTexture& texture)
	{
		assert(texture.m_bindless_resource_index.has_value());
		m_free_list.push_back(*texture.m_bindless_resource_index);
		texture.m_bindless_resource_index.reset();
	}

	ComPtr<ID3D12DescriptorHeap> GetDescriptorHeap() { return m_descriptor_heap; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() { return m_descriptor_heap->GetGPUDescriptorHandleForHeapStart(); }

protected:
	UINT AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu_descriptor)
	{
		UINT descriptor_index_to_use = UINT_MAX;
		if (m_free_list.size() > 0)
		{
			descriptor_index_to_use = m_free_list.back();
			m_free_list.pop_back();
		}
		else
		{
			descriptor_index_to_use = m_num_descriptors_allocated++;
		}
		assert(descriptor_index_to_use != UINT_MAX);

		D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
		INT64 cpu_descriptor_heap_offset = INT64(descriptor_index_to_use) * INT64(m_descriptor_size);
		descriptor_handle.ptr += cpu_descriptor_heap_offset;
		*cpu_descriptor = descriptor_handle;
		return descriptor_index_to_use;
	};

	ComPtr<ID3D12Device5> m_device;
	ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
	UINT m_descriptor_size = 0;
	UINT m_num_descriptors_allocated = 0;
	vector<UINT32> m_free_list;
};
