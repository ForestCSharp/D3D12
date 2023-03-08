#pragma once

#define CGLTF_IMPLEMENTATION //TODO: Move to cpp file if we include cgltf.h multiple places
#include "cgltf/cgltf.h"

#include <cassert>
#include <optional>
#include <vector>

#include "GpuResources.h"
#include "SimpleMath/SimpleMath.h"
using namespace DirectX::SimpleMath;
#include "../Shaders/HLSL_Types.h"

//TODO: Rename file to Scene.h

struct GltfInitData
{
	ComPtr<ID3D12Device5> device = nullptr;
	D3D12MA::Allocator* allocator = nullptr;
	ComPtr<ID3D12CommandQueue> command_queue = nullptr;
	BindlessResourceManager* bindless_resource_manager = nullptr;
};

struct GltfLoadContext
{
	// Passed via GltfInitData 
	ComPtr<ID3D12Device5> device = nullptr;
	D3D12MA::Allocator* allocator = nullptr;
	ComPtr<ID3D12CommandQueue> command_queue = nullptr;
	BindlessResourceManager* bindless_resource_manager = nullptr;

	// Setup using above
	GpuBuffer staging_buffer;
	ComPtr<ID3D12CommandAllocator> command_allocator;
	ComPtr<ID3D12GraphicsCommandList> command_list;
};

struct GltfRenderData
{
	Matrix transform;
	GpuBuffer vertex_buffer;
	optional<GpuBuffer> index_buffer;
};

//FCS TODO: Staging buffer for buffer uploads...
//FCS TODO: Generic accessor data loading

void recurse_node(GltfLoadContext& load_ctx, cgltf_node* in_node, const Matrix& in_matrix, std::vector<GltfRenderData>& result)
{
	//printf("Node Name: %s\n", in_node->name);

	Matrix current_matrix = in_node->has_matrix ? Matrix(in_node->matrix) * in_matrix : in_matrix;

	if (in_node->mesh)
	{
		for (int32_t primtive_idx = 0; primtive_idx < in_node->mesh->primitives_count; ++primtive_idx)
		{
			cgltf_primitive& primitive = in_node->mesh->primitives[primtive_idx];

			optional<GpuBuffer> index_buffer;
			if (cgltf_accessor* indices_accessor = primitive.indices)
			{
				uint8_t* indices_buffer = (uint8_t*) indices_accessor->buffer_view->buffer->data;
				indices_buffer += indices_accessor->offset + indices_accessor->buffer_view->offset;
				cgltf_size indices_byte_stride = indices_accessor->stride;

				cgltf_size indices_count = indices_accessor->count;
				assert(indices_byte_stride == 4); //TODO: Support 16-bit indices
				uint32_t* index_data = (uint32_t*) calloc(indices_count, indices_byte_stride);
				for (int i = 0; i < indices_count; ++i) {
					memcpy(&index_data[i], indices_buffer, indices_byte_stride);
					indices_buffer += indices_byte_stride;
				}

				cgltf_size index_buffer_size = indices_count * indices_byte_stride;
				load_ctx.staging_buffer.Write(index_data, index_buffer_size);
				free(index_data);

				GpuBufferDesc index_buffer_desc = {
					.allocator = load_ctx.allocator,
					.size = index_buffer_size,
					.heap_type = D3D12_HEAP_TYPE_DEFAULT,
					.resource_flags = D3D12_RESOURCE_FLAG_NONE,
					.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
				};
				index_buffer = GpuBuffer(index_buffer_desc);

				HR_CHECK(load_ctx.command_list->Reset(load_ctx.command_allocator.Get(), nullptr));
				load_ctx.command_list->CopyBufferRegion(index_buffer->GetResource(), 0, load_ctx.staging_buffer.GetResource(), 0, index_buffer_size);
				HR_CHECK(load_ctx.command_list->Close());

				ID3D12CommandList* ppCommandLists[] = { load_ctx.command_list.Get() };
				load_ctx.command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

				//TODO: Just wait on some event. No need to wait for idle
				wait_gpu_idle(load_ctx.device, load_ctx.command_queue);

				// Finally, register index buffer with bindless resource manager
				load_ctx.bindless_resource_manager->RegisterSRV(*index_buffer, (uint32_t) indices_count, (uint32_t) indices_byte_stride);
			}

			auto FindAttribute = [](cgltf_primitive& in_primitive, cgltf_attribute_type in_type) -> cgltf_attribute*
			{
				for (int32_t attr_idx = 0; attr_idx < in_primitive.attributes_count; ++attr_idx)
				{
					cgltf_attribute* attribute = &in_primitive.attributes[attr_idx];
					if (attribute->type == in_type)
					{
						return attribute;
					}
				}
				return nullptr;
			};

			GpuBuffer vertex_buffer;
			std::vector<Vertex> vertices;
			if (cgltf_attribute* positions_attribute = FindAttribute(primitive, cgltf_attribute_type_position))
			{
				if (cgltf_accessor* positions_accessor = positions_attribute->data)
				{
					assert(!positions_accessor->is_sparse);

					vertices.reserve(positions_accessor->count);

					uint8_t* positions_buffer = (uint8_t*) positions_accessor->buffer_view->buffer->data;
					positions_buffer += positions_accessor->offset + positions_accessor->buffer_view->offset;
					cgltf_size positions_buffer_stride = positions_accessor->stride;

					//TODO: Normals
					//cgltf_attribute* normals_attribute = FindAttribute(primitive, cgltf_attribute_type_normal);

					//TODO: UVs
					//cgltf_attribute* uvs_attribute = FindAttribute(primitive, cgltf_attribute_type_texcoord);

					auto rand_norm = []() -> float
					{
						return rand_range(0.0f, 1.0f);
					};
					float3 instance_color = float3(rand_norm(), rand_norm(), rand_norm());

					cgltf_size vertices_count = positions_accessor->count;
					vertices.reserve(vertices_count); //TODO: Add unititialized... (just calloc?)
					for (int32_t i = 0; i < vertices_count; ++i)
					{
						Vertex new_vertex;

						memcpy(&new_vertex.position, positions_buffer, positions_buffer_stride);

						new_vertex.color = instance_color;

						vertices.emplace_back(new_vertex);

						positions_buffer += positions_buffer_stride;
					}

					cgltf_size vertex_buffer_size = vertices.size() * sizeof(Vertex);
					load_ctx.staging_buffer.Write(vertices.data(), vertex_buffer_size);

					GpuBufferDesc vertex_buffer_desc = {
						.allocator = load_ctx.allocator,
						.size = vertex_buffer_size,
						.heap_type = D3D12_HEAP_TYPE_DEFAULT,
						.resource_flags = D3D12_RESOURCE_FLAG_NONE,
						.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
					};
					vertex_buffer = GpuBuffer(vertex_buffer_desc);

					HR_CHECK(load_ctx.command_list->Reset(load_ctx.command_allocator.Get(), nullptr));
					load_ctx.command_list->CopyBufferRegion(vertex_buffer.GetResource(), 0, load_ctx.staging_buffer.GetResource(), 0, vertex_buffer_size);
					HR_CHECK(load_ctx.command_list->Close());

					ID3D12CommandList* ppCommandLists[] = { load_ctx.command_list.Get() };
					load_ctx.command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

					//TODO: Just wait on some event. No need to wait for idle
					wait_gpu_idle(load_ctx.device, load_ctx.command_queue);

					load_ctx.bindless_resource_manager->RegisterSRV(vertex_buffer, (uint32_t) vertices_count, sizeof(Vertex));
				}
			}

			result.emplace_back(GltfRenderData {
					.transform = current_matrix,
					.vertex_buffer = vertex_buffer,
					.index_buffer = index_buffer,
				}
			);
		}
	}

	for (int32_t child_node_idx = 0; child_node_idx < in_node->children_count; ++child_node_idx)
	{
		recurse_node(load_ctx, in_node->children[child_node_idx], current_matrix, result);
	}
}

struct GltfScene
{
	GltfScene(GltfInitData& init_data, const char* in_path)
	{
		cgltf_options options = {0};
		cgltf_data* data = NULL;
		assert(cgltf_parse_file(&options, in_path, &data) == cgltf_result_success);
		assert(cgltf_load_buffers(&options, data, in_path) == cgltf_result_success);
		// Process default scene
		if (const cgltf_scene* scene = data->scene)
		{
			// Set up staging buffer
			GpuBufferDesc staging_buffer_desc = {
				.allocator = init_data.allocator,
				.size = 1024,
				.heap_type = D3D12_HEAP_TYPE_UPLOAD,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
			};

			// Set up command allocator/list for staging buffer copies
			ComPtr<ID3D12CommandAllocator> command_allocator;
			HR_CHECK(init_data.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)));

			ComPtr<ID3D12GraphicsCommandList> command_list;
			HR_CHECK(init_data.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), nullptr, IID_PPV_ARGS(&command_list)));
			HR_CHECK(command_list->Close());

			GltfLoadContext load_ctx = 
			{
				.device = init_data.device,
				.allocator = init_data.allocator,
				.command_queue = init_data.command_queue,
				.bindless_resource_manager = init_data.bindless_resource_manager,
				.staging_buffer = GpuBuffer(staging_buffer_desc),
				.command_allocator = command_allocator,
				.command_list = command_list,
			};

			//FCS TODO: Properly handle multiple nodes referencing the same mesh
			for (int32_t root_node_idx = 0; root_node_idx < scene->nodes_count; ++root_node_idx)
			{
				recurse_node(load_ctx, scene->nodes[root_node_idx], Matrix::Identity(), render_data_array);
			}

			gpu_instance_array.reserve(render_data_array.size());
			for (const GltfRenderData& render_data : render_data_array)
			{
				//FCS TODO: Need to make sure index buffer is valid
				//FCS TODO: If no index buffer is found, could "convert" a vertex-buffer-only mesh to use a generated idx buffer

				GpuInstanceData gpu_instance_data = {
					.transform = render_data.transform,
					.vertex_buffer_index = render_data.vertex_buffer.GetBindlessResourceIndex(),
					.index_buffer_index = render_data.index_buffer->GetBindlessResourceIndex(),
					.index_count = (uint32_t) render_data.index_buffer->GetSize() / sizeof(uint32_t),
				};
				
				gpu_instance_array.emplace_back(gpu_instance_data);
			}
		}

		cgltf_free(data);
	}

	std::vector<GltfRenderData> render_data_array;
	std::vector<GpuInstanceData> gpu_instance_array;
};

//TODO: NEXT! Set up instances on GPU and... try to render. 

/* 	TODO: Prefer loading binary. Load GLTF and Re-Save to BInary if
 * 		- Binary doesn't exist
 * 		- GLTF File Age has changed
 * 		- Binary format version bump
 * 
 * 	Binary Format
 * 	GLTF File Age
 * 	Primitive Count
 *  Max Contiguous Size (size of largest vertex or index buffer in file, useful for preallocating staging buffer)
 * 	For each primitive ...
 * 		Total Primitive Data Size
 * 		Primitive Transform
 * 		Primitive Indices
 * 		Primitive Vertices (a change in vertex format will require a re-load of GLTF and re-save of binary data)
 */