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

// Resources used to perform the transfer
struct BufferUploadResources
{
	ComPtr<ID3D12Device5> device;
	ComPtr<ID3D12CommandQueue> command_queue;
	ComPtr<ID3D12CommandAllocator> command_allocator;
	ComPtr<ID3D12GraphicsCommandList> command_list;
	mutable GpuBuffer staging_buffer;
};

// Actual data we want to upload
struct BufferUploadData
{
	GpuBufferDesc buffer_desc;
	void* buffer_data;
	size_t buffer_data_size;
};

//FCS TODO: Should create new buffer with COPY_DEST and then transition to final resource state with a barrier
GpuBuffer staging_upload_helper(const BufferUploadResources& upload_context, const BufferUploadData& upload_data)
{
	const bool needs_staging_buffer = !(upload_data.buffer_desc.heap_type & D3D12_HEAP_TYPE_UPLOAD);
	assert(needs_staging_buffer);

	upload_context.staging_buffer.Write(upload_data.buffer_data, upload_data.buffer_data_size);

	GpuBufferDesc modified_buffer_desc = upload_data.buffer_desc;
	modified_buffer_desc.resource_state |= D3D12_RESOURCE_STATE_COPY_DEST;
	GpuBuffer new_buffer(modified_buffer_desc);

	HR_CHECK(upload_context.command_list->Reset(upload_context.command_allocator.Get(), nullptr));
	upload_context.command_list->CopyBufferRegion(new_buffer.GetResource(), 0, upload_context.staging_buffer.GetResource(), 0, upload_data.buffer_data_size);
	HR_CHECK(upload_context.command_list->Close());

	ID3D12CommandList* ppCommandLists[] = { upload_context.command_list.Get() };
	upload_context.command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	//TODO: Just wait on some event. No need to wait for idle
	wait_gpu_idle(upload_context.device, upload_context.command_queue);

	return new_buffer;
}

// Simple wrapper around more generic staging_upload_helper
GpuBuffer gltf_staging_upload_helper(const GltfLoadContext& in_load_ctx, const GpuBufferDesc& in_buffer_desc, void* in_data, size_t in_data_size)
{
	BufferUploadResources upload_context = 
	{
		.device = in_load_ctx.device,
		.command_queue = in_load_ctx.command_queue,
		.command_allocator = in_load_ctx.command_allocator,
		.command_list = in_load_ctx.command_list,
		.staging_buffer = in_load_ctx.staging_buffer
	};

	BufferUploadData upload_data =
	{
		.buffer_desc = in_buffer_desc,
		.buffer_data = in_data,
		.buffer_data_size = in_data_size,
	};

	return staging_upload_helper(upload_context, upload_data);
}

//BEGIN FCS TODO: MOVE TO OWN FILE

// 1. Add single SG to octree (keep color for debug vis?)
// 2. Draw sphere at ea. octree leaf center and debug vis that SG value
// 3. Use randomly generated SG to view "Global Illumination"
// 4. Replace single SG with SG Basis and debug vis that
// 5. Again, randomly generate
// 6. Actually compute "real" values for each SGBasis, using raytracing

struct UVSphereDesc
{
	ComPtr<ID3D12Device5> device;
	ComPtr<ID3D12CommandQueue> command_queue;
	ComPtr<ID3D12CommandAllocator> command_allocator;
	ComPtr<ID3D12GraphicsCommandList> command_list;
	D3D12MA::Allocator* allocator;

	float radius;
	int latitudes;
	int longitudes;
};

struct UVSphere
{
	GpuBuffer vertex_buffer;
	uint32_t vertices_count;
	GpuBuffer index_buffer;
	uint32_t indices_count;

	UVSphere(const UVSphereDesc& desc)
	{
		vector<Vertex> vertices;
		vector<uint32_t> indices;
		//TODO: Reserve space in vertices and indices

		const float radius = desc.radius;
		const int latitudes = max(2, desc.latitudes);
		const int longitudes = max(3, desc.longitudes);

		float deltaLatitude = (float)Constants::PI / latitudes;
		float deltaLongitude = 2.0f * (float)Constants::PI / longitudes;
		float latitudeAngle;
		float longitudeAngle;

		// Compute all vertices first except normals
		for (int i = 0; i <= latitudes; ++i)
		{
			latitudeAngle = (float)Constants::PI / 2.0f - i * deltaLatitude; /* Starting -pi/2 to pi/2 */
			float xy = radius * cosf(latitudeAngle);    /* r * cos(phi) */
			float z = radius * sinf(latitudeAngle);     /* r * sin(phi )*/

			/*
				* We add (latitudes + 1) vertices per longitude because of equator,
				* the North pole and South pole are not counted here, as they overlap.
				* The first and last vertices have same position and normal, but
				* different tex coords.
				*/
			for (int j = 0; j <= longitudes; ++j)
			{
				longitudeAngle = j * deltaLongitude;

				Vertex vertex;

				vertex.position.x = xy * cosf(longitudeAngle);       /* x = r * cos(phi) * cos(theta)  */
				vertex.position.y = xy * sinf(longitudeAngle);       /* y = r * cos(phi) * sin(theta) */
				vertex.position.z = z;                               /* z = r * sin(phi) */
				vertex.texcoord.x = (float) j/longitudes;             /* s */
				vertex.texcoord.y = (float) i/latitudes;              /* t */

				const float lengthInv = 1.0f / radius; 
				vertex.normal.x = vertex.position.x * lengthInv;
				vertex.normal.y = vertex.position.y * lengthInv;
				vertex.normal.z = vertex.position.z * lengthInv;
				vertices.push_back(vertex);
			}
		}

		/*
			*  Indices
			*  k1--k1+1
			*  |  / |
			*  | /  |
			*  k2--k2+1
			*/
		unsigned int k1, k2;
		for(int i = 0; i < latitudes; ++i)
		{
			k1 = i * (longitudes + 1);
			k2 = k1 + longitudes + 1;
			// 2 Triangles per latitude block excluding the first and last longitudes blocks
			for(int j = 0; j < longitudes; ++j, ++k1, ++k2)
			{
				if (i != 0)
				{
					indices.push_back(k1);
					indices.push_back(k2);
					indices.push_back(k1 + 1);
				}

				if (i != (latitudes - 1))
				{
					indices.push_back(k1 + 1);
					indices.push_back(k2);
					indices.push_back(k2 + 1);
				}
			}
		}

		BufferUploadResources upload_resources =
		{
			.device = desc.device,
			.command_queue = desc.command_queue,
			.command_allocator = desc.command_allocator,
			.command_list = desc.command_list,
			.staging_buffer = GpuBuffer(GpuBufferDesc{
				.allocator = desc.allocator,
				.size = 1024,
				.heap_type = D3D12_HEAP_TYPE_UPLOAD,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_COPY_SOURCE,
			})
		};

		size_t vertex_buffer_size = vertices.size() * sizeof(Vertex);
		BufferUploadData vertex_upload_data =
		{
			.buffer_desc = 
			{
				.allocator = desc.allocator,
				.size = vertex_buffer_size,
				.heap_type = D3D12_HEAP_TYPE_DEFAULT,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
			},
			.buffer_data = vertices.data(),
			.buffer_data_size = vertex_buffer_size,
		};

		vertex_buffer = staging_upload_helper(upload_resources, vertex_upload_data);
		vertices_count = (uint32_t) vertices.size();

		size_t index_buffer_size = indices.size() * sizeof(uint32_t);
		BufferUploadData index_upload_data =
		{
			.buffer_desc = 
			{
				.allocator = desc.allocator,
				.size = index_buffer_size,
				.heap_type = D3D12_HEAP_TYPE_DEFAULT,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ,
			},
			.buffer_data = indices.data(),
			.buffer_data_size = index_buffer_size,
		};
		index_buffer = staging_upload_helper(upload_resources, index_upload_data);
		indices_count = (uint32_t) indices.size();
	}
	
};
//END FCS TODO: MOVE TO OWN FILE

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
				index_buffer = gltf_staging_upload_helper(load_ctx, index_buffer_desc, index_data, index_buffer_size);
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
				vertex_buffer = gltf_staging_upload_helper(load_ctx, vertex_buffer_desc, vertices.data(), vertex_buffer_size);
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
				.resource_state = D3D12_RESOURCE_STATE_COPY_SOURCE,
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
			indirect_draw_gpu_buffer = gltf_staging_upload_helper(load_ctx, indirect_draw_buffer_desc, indirect_draw_array.data(), indirect_draw_buffer_size);

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