#include "GpuResources.h"

GpuBuffer::GpuBuffer(const GpuBufferDesc& in_desc)
{
	m_buffer_desc = in_desc;
	assert(m_buffer_desc.allocator);
	assert(m_buffer_desc.size > 0);

	m_resource_desc = {};
	m_resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	m_resource_desc.Alignment = 0;
	m_resource_desc.Width = m_buffer_desc.size;
	m_resource_desc.Height = 1;
	m_resource_desc.DepthOrArraySize = 1;
	m_resource_desc.MipLevels = 1;
	m_resource_desc.Format = DXGI_FORMAT_UNKNOWN;
	m_resource_desc.SampleDesc.Count = 1;
	m_resource_desc.SampleDesc.Quality = 0;
	m_resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	m_resource_desc.Flags = m_buffer_desc.resource_flags;

	// Actually allocate our buffer
	Resize(m_buffer_desc.size);
};

uint32_t GpuBuffer::GetBindlessResourceIndex() const
{
	assert(bindless_resource_data.has_value());
	return bindless_resource_data->descriptor_index;
}

void GpuBuffer::UnregisterBindlessResource()
{
	if (bindless_resource_data.has_value())
	{
		bindless_resource_data->manager->UnregisterResource(*this);
	}
}

void GpuBuffer::Map(void** ppData)
{
	D3D12_RANGE read_range = { 0, 0 };
	HR_CHECK(m_resource->Map(0, &read_range, ppData));
}

void GpuBuffer::Write(void* in_data, size_t data_size)
{
	if (data_size > GetSize())
	{
		Resize(data_size);
	}

	assert(m_buffer_desc.heap_type == D3D12_HEAP_TYPE_UPLOAD);
	assert(data_size <= GetSize());
	UINT8* mapped_data = nullptr;
	D3D12_RANGE read_range = { 0, 0 };
	HR_CHECK(m_resource->Map(0, &read_range, reinterpret_cast<void**>(&mapped_data)));
	memcpy(mapped_data, in_data, data_size);
	m_resource->Unmap(0, nullptr);
}

void GpuBuffer::Resize(size_t new_size)
{
	m_buffer_desc.size = new_size;
	m_resource_desc.Width = new_size;

	D3D12MA::ALLOCATION_DESC alloc_desc = {};
	alloc_desc.HeapType = m_buffer_desc.heap_type;

	HR_CHECK(m_buffer_desc.allocator->CreateResource(
		&alloc_desc,
		&m_resource_desc,
		m_buffer_desc.resource_state,
		nullptr,
		&m_allocation,
		IID_PPV_ARGS(&m_resource)
	));
}

// ---------------------------------------- GpuTexture -------------------------------------------------//
GpuTexture::GpuTexture(const GpuTextureDesc& in_desc)
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

uint32_t GpuTexture::GetBindlessResourceIndex() const
{
	assert(bindless_resource_data.has_value());
	return bindless_resource_data->descriptor_index;
}

void GpuTexture::UnregisterBindlessResource()
{
	if (bindless_resource_data.has_value())
	{
		bindless_resource_data->manager->UnregisterResource(*this);
	}
}