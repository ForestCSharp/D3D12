#pragma once

#define CGLTF_IMPLEMENTATION //TODO: Move to cpp file if we include cgltf.h multiple places
#include "cgltf/cgltf.h"

#include <cassert>
#include <optional>
#include <vector>

using std::optional;
using std::nullopt;

#include "GpuResources.h"
#include "SimpleMath/SimpleMath.h"
using namespace DirectX::SimpleMath;
#include "../Shaders/HLSL_Types.h"

struct GltfInitData
{
	const char* file = nullptr;
	Matrix transform = Matrix::Identity();
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

// Holds the actual vertex/index buffer
struct GltfRenderData
{
	Matrix transform;
	GpuBuffer vertex_buffer;
	optional<GpuBuffer> index_buffer;
	size_t indices_count;
};

cgltf_attribute* FindAttribute(cgltf_primitive& in_primitive, cgltf_attribute_type in_type)
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

struct GltfBufferData
{
	uint8_t* buffer;
	cgltf_size count;
	cgltf_size stride;
};

optional<GltfBufferData> GetAccessorBuffer(cgltf_accessor* accessor)
{
	if (accessor && !accessor->is_sparse)
	{
		uint8_t* buffer = (uint8_t*) accessor->buffer_view->buffer->data;
		buffer += accessor->offset + accessor->buffer_view->offset;
		cgltf_size stride = accessor->stride;

		GltfBufferData out_data = {
			.buffer = buffer,
			.count = accessor->count,
			.stride = stride,
		};

		return out_data;
	}

	return nullopt;
}

optional<GltfBufferData> GetAttributeBuffer(cgltf_primitive& in_primitive, cgltf_attribute_type in_type)
{
	if (cgltf_attribute* attribute = FindAttribute(in_primitive, in_type))
	{
		return GetAccessorBuffer(attribute->data);
	}
	return nullopt;
};

// Takes in a staging buffer and buffer desc
GpuBuffer staging_upload_helper(GltfLoadContext& in_load_ctx, const GpuBufferDesc& in_buffer_desc, void* in_data, size_t in_data_size)
{
	const bool needs_staging_buffer = !(in_buffer_desc.heap_type & D3D12_HEAP_TYPE_UPLOAD);
	assert(needs_staging_buffer);

	in_load_ctx.staging_buffer.Write(in_data, in_data_size);

	GpuBuffer new_buffer(in_buffer_desc);

	HR_CHECK(in_load_ctx.command_list->Reset(in_load_ctx.command_allocator.Get(), nullptr));
	in_load_ctx.command_list->CopyBufferRegion(new_buffer.GetResource(), 0, in_load_ctx.staging_buffer.GetResource(), 0, in_data_size);
	HR_CHECK(in_load_ctx.command_list->Close());

	ID3D12CommandList* ppCommandLists[] = { in_load_ctx.command_list.Get() };
	in_load_ctx.command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	//TODO: Just wait on some event. No need to wait for idle
	wait_gpu_idle(in_load_ctx.device, in_load_ctx.command_queue);

	return new_buffer;
}

//FCS TODO: Generic accessor data loading

void recurse_node(GltfLoadContext& load_ctx, cgltf_node* in_node, const Matrix& in_matrix, std::vector<GltfRenderData>& result)
{	
	Matrix current_matrix = in_node->has_matrix ? Matrix(in_node->matrix) * in_matrix : in_matrix;

	if (in_node->mesh)
	{
		for (int32_t primtive_idx = 0; primtive_idx < in_node->mesh->primitives_count; ++primtive_idx)
		{
			cgltf_primitive& primitive = in_node->mesh->primitives[primtive_idx];

			optional<GpuBuffer> index_buffer;
			cgltf_size indices_count = 0;
			if (optional<GltfBufferData> indices_data = GetAccessorBuffer(primitive.indices))
			{
				uint32_t* index_data = (uint32_t*) calloc(indices_data->count, sizeof(uint32_t));

				for (int i = 0; i < indices_data->count; ++i)
				{
					if (indices_data->stride == 2)
					{
						memcpy(&index_data[i], indices_data->buffer, indices_data->stride);
					}
					else if (indices_data->stride == 4)
					{
						memcpy(&index_data[i], indices_data->buffer, indices_data->stride);
					}

					indices_data->buffer += indices_data->stride;
				}

				cgltf_size index_buffer_size = indices_data->count * sizeof(uint32_t);

				GpuBufferDesc index_buffer_desc = {
					.allocator = load_ctx.allocator,
					.size = index_buffer_size,
					.heap_type = D3D12_HEAP_TYPE_DEFAULT,
					.resource_flags = D3D12_RESOURCE_FLAG_NONE,
					.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
				};
				index_buffer = staging_upload_helper(load_ctx, index_buffer_desc, index_data, index_buffer_size);
				indices_count = indices_data->count;

				free(index_data);

				// Finally, register index buffer with bindless resource manager
				load_ctx.bindless_resource_manager->RegisterSRV(*index_buffer, (uint32_t) indices_data->count, sizeof(uint32_t));
			}
			assert(index_buffer.has_value());
			assert(indices_count > 0);

			GpuBuffer vertex_buffer;
			std::vector<Vertex> vertices;

			const float3 instance_color = float3(rand_norm(), rand_norm(), rand_norm());
			if (optional<GltfBufferData> positions_data = GetAttributeBuffer(primitive, cgltf_attribute_type_position))
			{
				optional<GltfBufferData> normals_data = GetAttributeBuffer(primitive, cgltf_attribute_type_normal);
				optional<GltfBufferData> color_data = GetAttributeBuffer(primitive, cgltf_attribute_type_color);
				optional<GltfBufferData> texcoord_data = GetAttributeBuffer(primitive, cgltf_attribute_type_texcoord);

				cgltf_size vertices_count = positions_data->count;
				vertices.reserve(vertices_count);
				for (int32_t i = 0; i < vertices_count; ++i)
				{
					Vertex new_vertex;

					memcpy(&new_vertex.position, positions_data->buffer, positions_data->stride);
					positions_data->buffer += positions_data->stride;

					if (normals_data)
					{
						memcpy(&new_vertex.normal, normals_data->buffer, normals_data->stride);
						normals_data->buffer += normals_data->stride;
					}

					if (color_data)
					{
						memcpy(&new_vertex.color, color_data->buffer, color_data->stride);
						color_data->buffer += color_data->stride;
					}

					if (texcoord_data)
					{
						memcpy(&new_vertex.texcoord, texcoord_data->buffer, texcoord_data->stride);
						texcoord_data->buffer += texcoord_data->stride;
					}

					vertices.emplace_back(new_vertex);
				}

				cgltf_size vertex_buffer_size = vertices.size() * sizeof(Vertex);

				GpuBufferDesc vertex_buffer_desc = {
					.allocator = load_ctx.allocator,
					.size = vertex_buffer_size,
					.heap_type = D3D12_HEAP_TYPE_DEFAULT,
					.resource_flags = D3D12_RESOURCE_FLAG_NONE,
					.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
				};
				vertex_buffer = staging_upload_helper(load_ctx, vertex_buffer_desc, vertices.data(), vertex_buffer_size);
				load_ctx.bindless_resource_manager->RegisterSRV(vertex_buffer, (uint32_t) vertices_count, sizeof(Vertex));
			}

			result.emplace_back(GltfRenderData {
				.transform = current_matrix,
				.vertex_buffer = vertex_buffer,
				.index_buffer = index_buffer,
				.indices_count = indices_count,
			});
		}
	}

	for (int32_t child_node_idx = 0; child_node_idx < in_node->children_count; ++child_node_idx)
	{
		recurse_node(load_ctx, in_node->children[child_node_idx], current_matrix, result);
	}
}

struct GltfScene
{
	GltfScene(const GltfInitData& init_data)
	{
		cgltf_options options = {};
		cgltf_data* data = NULL;
		assert(cgltf_parse_file(&options, init_data.file, &data) == cgltf_result_success);
		assert(cgltf_load_buffers(&options, data, init_data.file) == cgltf_result_success);
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
			HR_CHECK(init_data.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&command_allocator)));

			ComPtr<ID3D12GraphicsCommandList> command_list;
			HR_CHECK(init_data.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, command_allocator.Get(), nullptr, IID_PPV_ARGS(&command_list)));
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

			for (int32_t root_node_idx = 0; root_node_idx < scene->nodes_count; ++root_node_idx)
			{
				recurse_node(load_ctx, scene->nodes[root_node_idx], init_data.transform, render_data_array);
			}

			const uint num_instances = (uint) render_data_array.size();

			//Create this first and register as bindless resource so we can reference it in indirect draw args
			const size_t instances_buffer_size = num_instances * sizeof(GpuInstanceData);
			instances_gpu_buffer = GpuBuffer(GpuBufferDesc{
				.allocator = load_ctx.allocator,
				.size = instances_buffer_size,
				.heap_type = D3D12_HEAP_TYPE_UPLOAD,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
			});
			load_ctx.bindless_resource_manager->RegisterSRV(instances_gpu_buffer, num_instances, sizeof(GpuInstanceData));

			// Create our gpu_instance_data and indirect_draw_data from our GltfRenderData
			instances_array.reserve(num_instances);
			indirect_draw_array.reserve(num_instances);
			for (uint render_data_index = 0; render_data_index < num_instances; ++render_data_index)
			{
				const GltfRenderData& render_data = render_data_array[render_data_index];
				//FCS TODO: Need to make sure index buffer is valid
				//FCS TODO: If no index buffer is found, could "convert" a vertex-buffer-only mesh to use a generated idx buffer

				GpuInstanceData gpu_instance_data = {
					.transform = render_data.transform,
					.vertex_buffer_index = render_data.vertex_buffer.GetBindlessResourceIndex(),
					.index_buffer_index = render_data.index_buffer->GetBindlessResourceIndex(),
				};
				
				instances_array.emplace_back(gpu_instance_data);

				IndirectDrawData indirect_draw_data = {
					.instance_buffer_index = instances_gpu_buffer.GetBindlessResourceIndex(),
					.instance_id = render_data_index,
					.draw_arguments = {
						.VertexCountPerInstance = (uint32_t) render_data.index_buffer->GetSize() / sizeof(uint32_t),
						.InstanceCount = 1,
						.StartVertexLocation = 0,
						.StartInstanceLocation = 0,
					},
				};
				indirect_draw_array.emplace_back(indirect_draw_data);
			}

			//Create and upload indirect draw buffer
			const size_t indirect_draw_buffer_size = num_instances * sizeof(IndirectDrawData);
			GpuBufferDesc indirect_draw_buffer_desc = {
				.allocator = load_ctx.allocator,
				.size = indirect_draw_buffer_size,
				.heap_type = D3D12_HEAP_TYPE_DEFAULT,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			};
			indirect_draw_gpu_buffer = staging_upload_helper(load_ctx, indirect_draw_buffer_desc, indirect_draw_array.data(), indirect_draw_buffer_size);

			// Write out GPU Data for instances
			//TODO: instances_buffer can now be D3D12_HEAP_TYPE_DEFAULT
			instances_gpu_buffer.Write(instances_array.data(), instances_buffer_size);
		}

		cgltf_free(data);
	}

	/* Manages/Holds the actual render resources for each primitive */
	std::vector<GltfRenderData> render_data_array;

	/* Sent to the GPU to allow bindless access to vtx/idx buffer in render_data_array */
	std::vector<GpuInstanceData> instances_array;

	/* Buffer that holds our gpu scene instance data */
	GpuBuffer instances_gpu_buffer;

	/* Indirect Draw Args */
	std::vector<IndirectDrawData> indirect_draw_array;

	/* Buffer for indirect_draw_data */
	GpuBuffer indirect_draw_gpu_buffer;
};

// TODO: properly handle instances (i.e. multiple nodes pointing to the same mesh)
// ^ A new instance will be added with its own xform, but the vertex/index buffer bindless indices will be shared

/* 	TODO: Prefer loading binary. Load GLTF and Re-Save to Binary if
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