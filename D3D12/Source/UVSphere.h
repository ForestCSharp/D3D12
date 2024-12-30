#pragma once

#include <vector>

#include "GpuResources.h"
#include "SimpleMath/SimpleMath.h"
using namespace DirectX::SimpleMath;
#include "../Shaders/HLSL_Types.h"

struct UVSphereDesc
{
	ComPtr<ID3D12Device5> device;
	D3D12MA::Allocator* allocator;
	ComPtr<ID3D12CommandQueue> command_queue;

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
		// Set up command allocator/list for staging buffer copies
		ComPtr<ID3D12CommandAllocator> command_allocator;
		HR_CHECK(desc.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&command_allocator)));

		ComPtr<ID3D12GraphicsCommandList> command_list;
		HR_CHECK(desc.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, command_allocator.Get(), nullptr, IID_PPV_ARGS(&command_list)));
		HR_CHECK(command_list->Close());
		HR_CHECK(command_list->Reset(command_allocator.Get(), nullptr));

		vector<BufferUploadResult> pending_uploads;

		vector<Vertex> vertices;
		vector<uint32_t> indices;

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
			float xy = radius * cosf(latitudeAngle); /* r * cos(phi) */
			float z = radius * sinf(latitudeAngle); /* r * sin(phi )*/

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

				vertex.position.x = xy * cosf(longitudeAngle); /* x = r * cos(phi) * cos(theta)  */
				vertex.position.y = xy * sinf(longitudeAngle); /* y = r * cos(phi) * sin(theta) */
				vertex.position.z = z; /* z = r * sin(phi) */
				vertex.texcoord.x = (float) j / longitudes; /* s */
				vertex.texcoord.y = (float) i / latitudes; /* t */

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
		for (int i = 0; i < latitudes; ++i)
		{
			k1 = i * (longitudes + 1);
			k2 = k1 + longitudes + 1;
			// 2 Triangles per latitude block excluding the first and last longitudes blocks
			for (int j = 0; j < longitudes; ++j, ++k1, ++k2)
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

		{
			size_t vertex_buffer_size = vertices.size() * sizeof(Vertex);
			const BufferUploadData vertex_upload_data =
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

			const BufferUploadDesc vertex_buffer_upload_desc =
			{
				.device = desc.device,
				.allocator = desc.allocator,
				.command_list = command_list,
				.upload_data = vertex_upload_data,
			};

			BufferUploadResult vertex_upload_result = staging_upload_helper(vertex_buffer_upload_desc);
			vertex_buffer = vertex_upload_result.result_buffer;
			vertices_count = (uint32_t) vertices.size();

			pending_uploads.push_back(vertex_upload_result);
		}

		{
			size_t index_buffer_size = indices.size() * sizeof(uint32_t);
			const BufferUploadData index_upload_data =
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

			const BufferUploadDesc index_buffer_upload_desc =
			{
				.device = desc.device,
				.allocator = desc.allocator,
				.command_list = command_list,
				.upload_data = index_upload_data,
			};

			BufferUploadResult index_upload_result = staging_upload_helper(index_buffer_upload_desc);
			index_buffer = index_upload_result.result_buffer;
			indices_count = (uint32_t) indices.size();

			pending_uploads.push_back(index_upload_result);
		}

		HR_CHECK(command_list->Close());
		ID3D12CommandList* ppCommandLists[] = { command_list.Get() };
		desc.command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		wait_gpu_idle(desc.device, desc.command_queue);
		pending_uploads.clear();
	}
	
};
