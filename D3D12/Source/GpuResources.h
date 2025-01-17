#pragma once

#include <cassert>
#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <optional>
#include <wrl.h>
#include <cstdint>
#include <mutex>

#include "Common.h"
#include "../Shaders/HLSL_Types.h"
#include "D3D12MemAlloc/D3D12MemAlloc.h"

using STL_IMPL::optional;
using STL_IMPL::vector;

using Microsoft::WRL::ComPtr;

struct GpuBufferDesc
{
	D3D12MA::Allocator* allocator = nullptr;
	uint64_t size = 0;
	D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON;
};

struct BindlessResourceManager;

struct BindlessResourceData
{
	BindlessResourceManager* manager = nullptr;
	optional<UINT64> frame_index;
	UINT32 descriptor_index;
};

struct GpuBuffer
{
public:
	GpuBuffer() = default;
	GpuBuffer(const GpuBufferDesc& in_desc);

	bool IsValid() { return m_resource != nullptr && m_resource_desc.Width > 0; }
	ID3D12Resource* GetResource() const { return m_resource.Get(); }
	size_t GetSize() const { return m_resource_desc.Width; }
	DXGI_FORMAT GetFormat() const { return m_resource_desc.Format; }
	D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return GetResource()->GetGPUVirtualAddress(); }

	uint32_t GetBindlessResourceIndex() const;
	void UnregisterBindlessResource();
	void Map(void** ppData);
	void Write(void* in_data, size_t data_size);
	void Resize(size_t new_size);

protected:
	GpuBufferDesc m_buffer_desc = {};
	D3D12_RESOURCE_DESC m_resource_desc = {};
	ComPtr<ID3D12Resource> m_resource;
	ComPtr<D3D12MA::Allocation> m_allocation;

	optional<BindlessResourceData> bindless_resource_data = std::nullopt;

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
	optional<D3D12_CLEAR_VALUE> optimized_clear_value = std::nullopt;
};

struct GpuTexture
{
public:
	GpuTexture() = default;
	GpuTexture(const GpuTextureDesc& in_desc);

	bool IsValid() { return m_resource != nullptr; }
	ID3D12Resource* GetResource() const { return m_resource.Get(); }
	DXGI_FORMAT GetFormat() const { return m_resource_desc.Format; }
	D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return GetResource()->GetGPUVirtualAddress(); }
	uint32_t GetBindlessResourceIndex() const;
	void UnregisterBindlessResource();
protected:
	D3D12_RESOURCE_DESC m_resource_desc = {};
	ComPtr<ID3D12Resource> m_resource;
	ComPtr<D3D12MA::Allocation> m_allocation;

	optional<BindlessResourceData> bindless_resource_data = std::nullopt;

	friend struct BindlessResourceManager;
};

// Actual data we want to upload
struct BufferUploadData
{
	GpuBufferDesc buffer_desc;
	void* buffer_data;
	size_t buffer_data_size;
};

struct BufferUploadDesc
{
	ComPtr<ID3D12Device5> device;
	D3D12MA::Allocator* allocator;
	ComPtr<ID3D12GraphicsCommandList> command_list;
	BufferUploadData upload_data;
};

struct BufferUploadResult
{
	GpuBuffer staging_buffer;
	GpuBuffer result_buffer;
};

// This function assumes in_upload_desc.command_list is in a valid, recordable state
inline BufferUploadResult staging_upload_helper(const BufferUploadDesc& in_upload_desc)
{
	const BufferUploadData& upload_data = in_upload_desc.upload_data;

	const bool needs_staging_buffer = !(upload_data.buffer_desc.heap_type & D3D12_HEAP_TYPE_UPLOAD);
	assert(needs_staging_buffer);

	GpuBuffer staging_buffer = GpuBuffer(GpuBufferDesc{
		.allocator = in_upload_desc.allocator,
		.size = upload_data.buffer_data_size,
		.heap_type = D3D12_HEAP_TYPE_UPLOAD,
		.resource_flags = D3D12_RESOURCE_FLAG_NONE,
		.resource_state = D3D12_RESOURCE_STATE_COPY_SOURCE,
	});
	staging_buffer.Write(
		in_upload_desc.upload_data.buffer_data, 
		in_upload_desc.upload_data.buffer_data_size
	);

	GpuBufferDesc modified_buffer_desc = upload_data.buffer_desc;
	modified_buffer_desc.resource_state |= D3D12_RESOURCE_STATE_COPY_DEST;
	GpuBuffer result_buffer(modified_buffer_desc);

	in_upload_desc.command_list->CopyBufferRegion(
		result_buffer.GetResource(), 0, 
		staging_buffer.GetResource(), 0, 
		upload_data.buffer_data_size
	);

	BufferUploadResult upload_result = {
		.staging_buffer = staging_buffer,
		.result_buffer = result_buffer,
	};

	return upload_result;
}

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

	void BeginFrame(UINT64 frame_idx)
	{
		frame_free_lists.insert({ frame_idx, {} });
	}

	void CleanupFrame(UINT64 frame_idx)
	{
		auto found_frame_free_list = frame_free_lists.find(frame_idx);
		if (found_frame_free_list != frame_free_lists.end())
		{
			const FreeList& frame_free_list = found_frame_free_list->second;
			default_free_list.insert(
				default_free_list.end(), 
				frame_free_list.begin(), 
				frame_free_list.end()
			);
			frame_free_lists.erase(frame_idx);
		}
	}

	optional<UINT32> GetFreeListEntry(optional<UINT64> frame_idx)
	{
		// If we have a frame_idx set, search there first
		if (frame_idx.has_value())
		{
			auto found_frame_free_list = frame_free_lists.find(*frame_idx);
			if (found_frame_free_list != frame_free_lists.end())
			{
				FreeList& frame_free_list = found_frame_free_list->second;
				if (frame_free_list.size() > 0)
				{
					UINT32 out_index = frame_free_list.back();
					frame_free_list.pop_back();
					return out_index;
				}
			}
		}
		
		// Then search the default free list
		if (default_free_list.size() > 0)
		{
			UINT32 out_index = default_free_list.back();
			default_free_list.pop_back();
			return out_index;
		}

		// No free list entry found, we'll have to increment m_num_descriptors_allocated in AllocateDescriptor
		return std::nullopt;
	}

	void ReturnFreeListEntry(UINT32 descriptor_index, optional<UINT64> frame_idx)
	{
		if (frame_idx.has_value())
		{
			auto found_frame_free_list = frame_free_lists.find(*frame_idx);
			if (found_frame_free_list != frame_free_lists.end())
			{
				FreeList& frame_free_list = found_frame_free_list->second;
				frame_free_list.push_back(descriptor_index);
			}
		}
		else
		{
			default_free_list.push_back(descriptor_index);
		}
	}

	/*	
		All Register Functions can optionally take a frame_idx. 
		This should be used for resources that are intended to only exist for a single frame.
		For resources not bound to a specific frame, that argument can be omitted
	*/

	UINT32 RegisterCBV(GpuBuffer& in_buffer, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_buffer.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE cbuffer_cpu_handle;
		in_buffer.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&cbuffer_cpu_handle, frame_idx),
		};
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc =
		{
			.BufferLocation = in_buffer.GetGPUVirtualAddress(),
			.SizeInBytes = static_cast<UINT>(in_buffer.GetSize()),
		};
		m_device->CreateConstantBufferView(&cbv_desc, cbuffer_cpu_handle);
		return in_buffer.bindless_resource_data->descriptor_index;
	}

	UINT32 RegisterUAV(GpuBuffer& in_buffer, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_buffer.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle;
		in_buffer.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&uav_cpu_handle, frame_idx),
		};		
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
		uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		m_device->CreateUnorderedAccessView(in_buffer.GetResource(), nullptr, &uav_desc, uav_cpu_handle);
		return in_buffer.bindless_resource_data->descriptor_index;
	}

	UINT32 RegisterUAV(GpuTexture& in_texture, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_texture.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle;
		in_texture.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&uav_cpu_handle, frame_idx),
		};
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
		uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		m_device->CreateUnorderedAccessView(in_texture.GetResource(), nullptr, &uav_desc, uav_cpu_handle);
		return in_texture.bindless_resource_data->descriptor_index;
	}

	UINT32 RegisterSRV(GpuBuffer& in_buffer, UINT32 num_elements, UINT32 element_size, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_buffer.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle;
		in_buffer.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&srv_cpu_handle, frame_idx),
		};	
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		if (element_size == 0)
		{
			srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			srv_desc.Buffer =
			{
				.FirstElement = 0,
				.NumElements = num_elements,
				.StructureByteStride = 0,
			};
		}
		else
		{
			srv_desc.Format = DXGI_FORMAT_UNKNOWN;
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			srv_desc.Buffer =
			{
				.FirstElement = 0,
				.NumElements = num_elements,
				.StructureByteStride = element_size,
			};
		}
		m_device->CreateShaderResourceView(in_buffer.GetResource(), &srv_desc, srv_cpu_handle);
		return in_buffer.bindless_resource_data->descriptor_index;
	}

	UINT32 RegisterSRV(GpuTexture& in_texture, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_texture.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle;
		in_texture.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&srv_cpu_handle, frame_idx),
		};
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc =
		{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.Texture2D = 
			{
				.MostDetailedMip = 0,
				.MipLevels = 1,
				.PlaneSlice = 0,
				.ResourceMinLODClamp = 0,
			}
		};
		m_device->CreateShaderResourceView(nullptr, &srv_desc, srv_cpu_handle);
		return in_texture.bindless_resource_data->descriptor_index;
	}

	UINT32 RegisterAccelerationStructure(GpuBuffer& in_buffer, optional<UINT64> frame_idx = std::nullopt)
	{
		assert(!in_buffer.bindless_resource_data.has_value());

		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle;
		in_buffer.bindless_resource_data = BindlessResourceData {
			.manager = this,
			.frame_index = frame_idx,
			.descriptor_index = AllocateDescriptor(&srv_cpu_handle, frame_idx),
		};	
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc =
		{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.RaytracingAccelerationStructure =
			{
				.Location = in_buffer.GetGPUVirtualAddress(),
			}
		};
		m_device->CreateShaderResourceView(nullptr, &srv_desc, srv_cpu_handle);
		return in_buffer.bindless_resource_data->descriptor_index;
	}

	void UnregisterResource(GpuBuffer& buffer)
	{
		std::lock_guard scope_lock(m_mutex);
		assert(buffer.bindless_resource_data.has_value());
		const UINT32 descriptor_index = buffer.bindless_resource_data->descriptor_index;
		const optional<UINT64> frame_index = buffer.bindless_resource_data->frame_index;
		ReturnFreeListEntry(descriptor_index, frame_index);
		buffer.bindless_resource_data.reset();
	}

	void UnregisterResource(GpuTexture& texture)
	{
		std::lock_guard scope_lock(m_mutex);
		assert(texture.bindless_resource_data.has_value());
		const UINT32 descriptor_index = texture.bindless_resource_data->descriptor_index;
		const optional<UINT64> frame_index = texture.bindless_resource_data->frame_index;
		ReturnFreeListEntry(descriptor_index, frame_index);
		texture.bindless_resource_data.reset();
	}

	ComPtr<ID3D12DescriptorHeap> GetDescriptorHeap() { return m_descriptor_heap; }
	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() { return m_descriptor_heap->GetGPUDescriptorHandleForHeapStart(); }

protected:
	UINT AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu_descriptor, optional<UINT64> frame_idx = std::nullopt)
	{
		std::lock_guard scope_lock(m_mutex);
		UINT descriptor_index_to_use = UINT_MAX;
		bool from_free_list = false;
		if (optional<UINT32> free_list_entry = GetFreeListEntry(frame_idx))
		{
			descriptor_index_to_use = *free_list_entry;
			from_free_list = true;
		}
		else
		{
			descriptor_index_to_use = m_num_descriptors_allocated++;
		}
		assert(descriptor_index_to_use < NUM_BINDLESS_DESCRIPTORS_PER_TYPE);

		printf("Allocation Descriptor: %u, m_num_descriptors_allocated: %u\n", descriptor_index_to_use, m_num_descriptors_allocated);

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
	std::mutex m_mutex;

	using FreeList = vector<UINT32>;
	HashMap<UINT64, FreeList> frame_free_lists;
	FreeList default_free_list;
};

//FCS TODO: Support for using BindlessResourceManager during rendering
//FCS TODO: BindlessResourceManager needs to persist bindless bindings until they aren't needed any more
//FCS TODO: Need to Create array of free-lists tagged by their allocation frame