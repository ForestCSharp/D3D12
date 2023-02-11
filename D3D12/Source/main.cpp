#define WIN32_LEAN_AND_MEAN

// FCS TODO:
// Render Graph
// Standardize formatting
// EASTL
// Remove m_ from member vars
// Make sure agility sdk is set up
// Rename sln / project and setup Github repo
// More cpp files, move defs into cpp

#include "GpuCommands.h"
#include "RenderGraph.h"
#include "ShaderCompiler.h"

#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <cstdio>
#include <vector>
#include <optional>

#include "SimpleMath/SimpleMath.h"
using namespace DirectX::SimpleMath;

#include "Common.h"
#include "GpuResources.h"
#include "GpuPipelines.h"
#include "../Shaders/HLSL_Types.h"

using STL_IMPL::optional;
using STL_IMPL::vector;
using STL_IMPL::wstring;
using STL_IMPL::move;
using Microsoft::WRL::ComPtr;

static constexpr UINT frame_count = 3;

//Agility SDK Setup
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 706; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

float randf()
{
	return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

float map(float input_number, float input_min, float input_max, float ouptut_min, float output_max)
{
	return ouptut_min + (input_number - input_min) * (output_max - ouptut_min) / (input_max - input_min);
}

float rand_range(float min, float max)
{
	return map(randf(), 0.0f, 1.0f, min, max);
}

bool IsKeyPressed(int in_key)
{
	return GetKeyState(in_key) < 0;
}

#define ROUND_UP(num, multiple) ((num + multiple - 1) / multiple) * multiple

inline void wait_gpu_idle(ComPtr<ID3D12Device> device, ComPtr<ID3D12CommandQueue> command_queue)
{
	ComPtr<ID3D12Fence> fence;
	HR_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	const HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	assert(fence_event);

	HR_CHECK(command_queue->Signal(fence.Get(), 1));
	HR_CHECK(fence->SetEventOnCompletion(1, fence_event));
	WaitForSingleObject(fence_event, INFINITE);
}

const wchar_t* hit_group_name = L"MyHitGroup";
const wchar_t* raygen_shader_name = L"Raygen";
const wchar_t* closest_hit_shader_name = L"ClosestHit";
const wchar_t* miss_shader_name = L"Miss";

struct FrameDataDesc
{
	UINT width;
	UINT height;
	ComPtr<ID3D12Device5> device;
	ComPtr<IDXGIFactory4> factory;
	ComPtr<ID3D12CommandQueue> command_queue;
	HWND window;
};

struct FrameData
{
	// Resources
	DXGI_FORMAT swap_chain_format;
	ComPtr<ID3D12Resource> render_targets[frame_count];
	ComPtr<IDXGISwapChain3> swapchain;
	ComPtr<ID3D12CommandAllocator> command_allocators[frame_count];

	// Synchronization
	UINT current_backbuffer_index = 0;
	HANDLE fence_event;
	ComPtr<ID3D12Fence> fence;
	UINT64 fence_values[frame_count];

	FrameData(const FrameDataDesc& create_info)
	{
		resize(create_info);

		for (UINT i = 0; i < frame_count; i++)
		{
			// 7. Create a command allocator per-frame, which will be used to create our command lists
			HR_CHECK(create_info.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[i])));
		}

		HR_CHECK(create_info.device->CreateFence(fence_values[0], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

		// Create an event handle to use for frame synchronization.
		fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (fence_event == nullptr)
		{
			HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	void resize(const FrameDataDesc& create_info)
	{
		swapchain.Reset();
		for (UINT i = 0; i < frame_count; ++i)
		{
			render_targets[i].Reset();
		}

		swap_chain_format = DXGI_FORMAT_R8G8B8A8_UNORM;

		DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
		swap_chain_desc.BufferCount = frame_count;
		swap_chain_desc.Width = create_info.width;
		swap_chain_desc.Height = create_info.height;
		swap_chain_desc.Format = swap_chain_format;
		swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swap_chain_desc.SampleDesc.Count = 1;

		ComPtr<IDXGISwapChain1> swapchain_1;
		HR_CHECK(create_info.factory->CreateSwapChainForHwnd(
			create_info.command_queue.Get(), // Swap chain needs the queue so that it can force a flush on it.
			create_info.window,
			&swap_chain_desc,
			nullptr,
			nullptr,
			&swapchain_1
		));

		HR_CHECK(create_info.factory->MakeWindowAssociation(create_info.window, 0));
		HR_CHECK(swapchain_1.As(&swapchain));
		current_backbuffer_index = swapchain->GetCurrentBackBufferIndex();

		ComPtr<ID3D12DescriptorHeap> rtv_descriptor_heap;
		const UINT rtv_heap_offset = create_info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		{
			// 5. Create descriptor heaps (1 per frame)
			D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
			rtv_heap_desc.NumDescriptors = frame_count;
			rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			HR_CHECK(create_info.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_descriptor_heap)));

			// 6. Create render target views (1 per frame), associating them with corresponding rtv descriptor heaps
			{
				D3D12_CPU_DESCRIPTOR_HANDLE rtv_descriptor_handle(rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart());

				// Create a render target view for each frame. Reset all fence_values to zero
				for (UINT i = 0; i < frame_count; i++)
				{
					HR_CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));

					D3D12_RENDER_TARGET_VIEW_DESC render_target_view_desc = {};
					render_target_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
					render_target_view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

					create_info.device->CreateRenderTargetView(render_targets[i].Get(), &render_target_view_desc, rtv_descriptor_handle);
					rtv_descriptor_handle.ptr += rtv_heap_offset;

					fence_values[i] = 0;
				}
			}
		}
	}

	void reset()
	{
		swapchain.Reset();

		for (UINT i = 0; i < frame_count; ++i)
		{
			render_targets[i].Reset();
			command_allocators[i].Reset();
		}

		//TODO: free up other resources
	}

	ID3D12CommandAllocator* get_command_allocator() const
	{
		return command_allocators[current_backbuffer_index].Get();
	}

	ID3D12Resource* get_render_target() const
	{
		return render_targets[current_backbuffer_index].Get();
	}

	void present()
	{
		HR_CHECK(swapchain->Present(1, 0));
	}

	//TODO: Method for registering other per-frame resources
	unordered_map<UINT64, vector<RenderGraph>> pending_render_graphs;
	void register_graph(RenderGraph&& in_resource)
	{
		pending_render_graphs[fence_values[current_backbuffer_index]].push_back(move(in_resource));
	}

	void wait_for_previous_frame(ComPtr<ID3D12CommandQueue> command_queue)
	{
		// Signal The current fence value
		const UINT64 current_fence_value = fence_values[current_backbuffer_index];
		HR_CHECK(command_queue->Signal(fence.Get(), current_fence_value));

		// Update Current Backbuffer Index
		current_backbuffer_index = swapchain->GetCurrentBackBufferIndex();
		
		// Check the fence's current completed value. If it is less than the value at our new current_back_buffer_index, we need to wait
		if (fence->GetCompletedValue() < fence_values[current_backbuffer_index])
		{
			HR_CHECK(fence->SetEventOnCompletion(fence_values[current_backbuffer_index], fence_event));
			WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
		}

		// Clean up any resources associated with that fence_value
		pending_render_graphs.erase(fence_values[current_backbuffer_index]);

		// Update our fence value for next usage
		fence_values[current_backbuffer_index] = current_fence_value + 1;
	}
};

//TODO: Store in some sort of App singleton?
bool should_close = false;
int mouse_x = 0;
int mouse_y = 0;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_CLOSE: 
		{
			should_close = true;
			break;
		}
		case WM_MOUSEMOVE:
		{
			mouse_x = GET_X_LPARAM(lParam);
			mouse_y = GET_Y_LPARAM(lParam);
			break;
		}
	}

	return DefWindowProc(window, message, wParam, lParam);
}

int main()
{
	//FCS TODO BEGIN: Organize all these variables (set them up as created farther down)
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<ID3D12Device5> device;

	ComPtr<ID3D12CommandQueue> command_queue;

	//FCS TODO END
	
	// 1. Create Our Window
	HINSTANCE h_instance = GetModuleHandle(nullptr);

	WNDCLASSEX window_class = { 0 };
	window_class.cbSize = sizeof(WNDCLASSEX);
	window_class.style = CS_HREDRAW | CS_VREDRAW;
	window_class.lpfnWndProc = WindowProc;
	window_class.hInstance = h_instance;
	window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
	window_class.lpszClassName = L"DXSampleClass";
	RegisterClassEx(&window_class);

	UINT render_width = 1280;
	UINT render_height = 720;
	RECT window_rect = { 0, 0, static_cast<LONG>(render_width), static_cast<LONG>(render_height)};
	AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND window = CreateWindow(
		window_class.lpszClassName,
		L"D3D12",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		window_rect.right - window_rect.left,
		window_rect.bottom - window_rect.top,
		nullptr, // We have no parent window.
		nullptr, // We aren't using menus.
		h_instance,
		NULL); //Could pass userdata to window proc here

	ShowWindow(window, SW_SHOW);

	ComPtr<ID3D12Debug> debug_controller;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))
	{
		debug_controller->EnableDebugLayer();
	}

	// 2. Create a D3D12 Factory, Adapter, and Device
	UINT dxgi_factory_flags = DXGI_CREATE_FACTORY_DEBUG;
	HR_CHECK(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&factory)));
	for (UINT adapter_index = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(adapter_index, &adapter); ++adapter_index)
	{
		DXGI_ADAPTER_DESC1 desc = {0};
		adapter->GetDesc1(&desc);

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			continue;
		}

		// Check to see if the adapter supports Direct3D 12, and create device if so
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
		{
			HR_CHECK(D3D12CreateDevice(
				adapter.Get(),
				D3D_FEATURE_LEVEL_12_0,
				IID_PPV_ARGS(&device)
			));

			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
			HR_CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
			if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
			{
				device.Reset();
				continue;
			}

			break;
		}
	}
	assert(device.Get() && adapter.Get());

	D3D12MA::ALLOCATOR_DESC allocator_desc = {};
	allocator_desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
	allocator_desc.pDevice = device.Get();
	allocator_desc.pAdapter = adapter.Get();

	D3D12MA::Allocator* gpu_memory_allocator = nullptr;
	HR_CHECK(D3D12MA::CreateAllocator(&allocator_desc, &gpu_memory_allocator));
	
	// 3. Create a command queue
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	HR_CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)));

	FrameDataDesc frame_data_create_info = 
	{
		.width = render_width,
		.height = render_height,
		.device = device,
		.factory = factory,
		.command_queue = command_queue,
		.window = window,
	};
	FrameData frame_data(frame_data_create_info);

	// 12. Create Command list using command allocator and pipeline state, and close it (we'll record it later)
	ComPtr<ID3D12GraphicsCommandList4> command_list;
	HR_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_data.get_command_allocator(), nullptr, IID_PPV_ARGS(&command_list)));
	HR_CHECK(command_list->Close());

	//Create The Vertex Buffer, copy vertices into it
	Vertex mesh_vertices[] =
	{
		// Top
		Vertex(Vector3(1, 1,  1), Vector3(0, 1, 0), Vector3(1, 0, 0)),
		Vertex(Vector3(1, 1, -1), Vector3(0, 1, 0), Vector3(1, 0, 0)),
		Vertex(Vector3(-1, 1,  1), Vector3(0, 1, 0), Vector3(1, 0, 0)),
		Vertex(Vector3(-1, 1, -1), Vector3(0, 1, 0), Vector3(1, 0, 0)),

		// Positive X Side
		Vertex(Vector3(1, 1, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)),
		Vertex(Vector3(1, 1, -1), Vector3(1, 0, 0), Vector3(0, 1, 0)),
		Vertex(Vector3(1, -1, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)),
		Vertex(Vector3(1, -1, -1), Vector3(1, 0, 0), Vector3(0, 1, 0)),

		// Negative X Side
		Vertex(Vector3(-1, 1, 1), Vector3(-1, 0, 0), Vector3(0, 0, 1)),
		Vertex(Vector3(-1, 1, -1), Vector3(-1, 0, 0), Vector3(0, 0, 1)),
		Vertex(Vector3(-1, -1, 1), Vector3(-1, 0, 0), Vector3(0, 0, 1)),
		Vertex(Vector3(-1, -1, -1), Vector3(-1, 0, 0), Vector3(0, 0, 1)),

		// Positive Z Side
		Vertex(Vector3(1, 1, 1), Vector3(0, 0, 1), Vector3(1, 1, 0)),
		Vertex(Vector3(1, -1, 1), Vector3(0, 0, 1), Vector3(1, 1, 0)),
		Vertex(Vector3(-1, 1, 1), Vector3(0, 0, 1), Vector3(1, 1, 0)),
		Vertex(Vector3(-1, -1, 1), Vector3(0, 0, 1), Vector3(1, 1, 0)),

		// Negative Z Side
		Vertex(Vector3(1, 1, -1), Vector3(0, 0, -1), Vector3(0, 1, 1)),
		Vertex(Vector3(1, -1, -1), Vector3(0, 0, -1), Vector3(0, 1, 1)),
		Vertex(Vector3(-1, 1, -1), Vector3(0, 0, -1), Vector3(0, 1, 1)),
		Vertex(Vector3(-1, -1, -1), Vector3(0, 0, -1), Vector3(0, 1, 1)),

		// Bottom
		Vertex(Vector3(-1, -1, -1), Vector3(0, -1, 0), Vector3(1, 1, 1)),
		Vertex(Vector3(-1, -1,  1), Vector3(0, -1, 0), Vector3(1, 1, 1)),
		Vertex(Vector3( 1, -1, -1), Vector3(0, -1, 0), Vector3(1, 1, 1)),
		Vertex(Vector3( 1, -1,  1), Vector3(0, -1, 0), Vector3(1, 1, 1)),
	};

	const UINT vertex_buffer_size = sizeof(mesh_vertices);
	const UINT num_vertices = vertex_buffer_size / sizeof(Vertex);
	GpuBuffer vertex_buffer(GpuBufferDesc{
		.allocator = gpu_memory_allocator,
		.size = vertex_buffer_size,
		.heap_type = D3D12_HEAP_TYPE_UPLOAD,
		.resource_flags = D3D12_RESOURCE_FLAG_NONE,
		.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
	});
	vertex_buffer.Write(mesh_vertices, vertex_buffer_size);

	// Index Buffer
	uint32_t mesh_indices[] =
	{
		0, 1, 2,	// Side 0
		2, 1, 3,
		4, 6, 5,	// Side 1
		6, 7, 5,
		8, 9, 10,	// Side 2
		10, 9, 11,
		12, 14, 13, // Side 3
		14, 15, 13,
		16, 17, 18, // Side 4
		18, 17, 19,
		21, 20, 22, // Side 5
		21, 22, 23,
	};
	const UINT index_buffer_size = sizeof(mesh_indices);
	const UINT num_indices = index_buffer_size / sizeof(uint32_t);
	GpuBuffer index_buffer(GpuBufferDesc{
		.allocator = gpu_memory_allocator,
		.size = index_buffer_size,
		.heap_type = D3D12_HEAP_TYPE_UPLOAD,
		.resource_flags = D3D12_RESOURCE_FLAG_NONE,
		.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ | D3D12_RESOURCE_STATE_INDEX_BUFFER
	});
	index_buffer.Write(mesh_indices, index_buffer_size);

	// Constant Buffers
	GlobalConstantBuffer global_constant_buffer_data =
	{
		.sun_dir = Vector4(0.5, 0.25, 1, 0),
	};
	const size_t global_constant_buffer_size = ROUND_UP(sizeof(GlobalConstantBuffer), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	GpuBuffer global_constant_buffers[frame_count];
	for (int32_t current_backbuffer_index = 0; current_backbuffer_index < frame_count; ++current_backbuffer_index)
	{
		GpuBuffer global_constant_buffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = global_constant_buffer_size,
			.heap_type = D3D12_HEAP_TYPE_UPLOAD,
			.resource_flags = D3D12_RESOURCE_FLAG_NONE,
			.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
		});
		global_constant_buffer.Write(&global_constant_buffer_data, sizeof(GlobalConstantBuffer));
		global_constant_buffers[current_backbuffer_index] = global_constant_buffer;
	}

	BindlessResourceManager bindless_resource_manager(device);
	
	ComPtr<ID3D12RootSignature> global_root_signature;
	{
		auto SerializeAndCreateRootSignature = [&](D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* p_root_signature)
		{
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> error;

			HR_CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
			HR_CHECK(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*p_root_signature))));
		};

		{
			// Instead Send Constant buffer via table (with indices for these 2 resources accessed via "ResourceDescriptorHeap"
			// See https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_6.html
			// See https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3D12_ROOT_PARAMETER scene_cbv_root_param =
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
				.Descriptor =
				{
					.ShaderRegister = 0,
					.RegisterSpace = 1,
				},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
			};

			D3D12_ROOT_PARAMETER root_params[] = { scene_cbv_root_param };

			D3D12_ROOT_SIGNATURE_DESC root_signature_desc = 
			{
				.NumParameters = _countof(root_params),
				.pParameters = root_params,
				.NumStaticSamplers = 0,
				.pStaticSamplers = nullptr,
				.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
						| D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
			};
			SerializeAndCreateRootSignature(root_signature_desc, &global_root_signature);
		}
	}

	const UINT32 index_buffer_index = bindless_resource_manager.RegisterSRV(index_buffer, num_indices, sizeof(uint32_t));
	const UINT32 vertex_buffer_index = bindless_resource_manager.RegisterSRV(vertex_buffer, num_vertices, sizeof(Vertex));

	Vector3 cam_pos = Vector3(5, 5, -5);
	Vector3 cam_up = Vector3(0, 1, 0);
	Vector3 cam_forward = Normalize(Vector3(0, 0, 1) - cam_pos);

	int previous_mouse_x = 0;
	int previous_mouse_y = 0;
	Vector2 mouse_delta(0.f, 0.f);

	global_constant_buffer_data.frames_rendered = 0;

	/** Gpu Scene (array of instances) */
	vector<GpuInstanceData> instances;
	instances.push_back(GpuInstanceData
	{
		.world_matrix = Matrix::Identity(),
		.vertex_buffer_index = vertex_buffer_index,
		.index_buffer_index = index_buffer_index,
	});
	const size_t instances_buffer_size = instances.size() * sizeof(GpuInstanceData);
	GpuBuffer instances_buffer(GpuBufferDesc{
		.allocator = gpu_memory_allocator,
		.size = instances_buffer_size,
		.heap_type = D3D12_HEAP_TYPE_UPLOAD,
		.resource_flags = D3D12_RESOURCE_FLAG_NONE,
		.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
	});
	instances_buffer.Write(instances.data(), instances_buffer_size);
	global_constant_buffer_data.instance_buffer_index = bindless_resource_manager.RegisterSRV(instances_buffer, (UINT32) instances.size(), sizeof(GpuInstanceData));
	global_constant_buffer_data.instance_buffer_count = (UINT32) instances.size();

	while (!should_close)
	{
		RECT client_rect;
		if (GetClientRect(window, &client_rect))
		{
			LONG new_width = client_rect.right - client_rect.left;
			LONG new_height = client_rect.bottom - client_rect.top;
			if (new_width != render_width || new_height != render_height)
			{
				render_width = new_width;
				render_height = new_height;
				printf("Width: %lu Height: %lu\n", new_width, new_height);

				// Update width and height and call resize
				frame_data_create_info.width = render_width;
				frame_data_create_info.height = render_height;
				frame_data.resize(frame_data_create_info);

				global_constant_buffer_data.frames_rendered = 0;
			}
		}

		global_constant_buffer_data.frames_rendered += 1;
		global_constant_buffer_data.random = rand();

		// Calculate Mouse Delta
		{
			mouse_delta.x = static_cast<float>(mouse_x - previous_mouse_x);
			mouse_delta.y = static_cast<float>(mouse_y - previous_mouse_y);
			previous_mouse_x = mouse_x;
			previous_mouse_y = mouse_y;
		}

		if (IsKeyPressed(VK_CONTROL) && IsKeyPressed(VK_LBUTTON)) 
		{
			// Handle Sun-Dir rotation
			const float speed = 0.0035f;
			const Vector3 cam_right = Cross(cam_forward, cam_up);
			const Quaternion quat_pitch = Quaternion::CreateFromAxisAngle(cam_right, speed * mouse_delta.y);
			const Quaternion quat_yaw = Quaternion::CreateFromAxisAngle(cam_up, speed * mouse_delta.x);
			global_constant_buffer_data.sun_dir = Normalize(Vector4::Transform(global_constant_buffer_data.sun_dir, quat_pitch * quat_yaw));
			global_constant_buffer_data.frames_rendered = 0;
		}
		else if (IsKeyPressed(VK_LBUTTON))
		{
			// Camera rotation
			const float speed = 0.0025f;
			const Vector3 cam_right = Cross(cam_forward, cam_up);
			const Quaternion quat_pitch = Quaternion::CreateFromAxisAngle(cam_right, -1.0f * speed * mouse_delta.y);
			const Quaternion quat_yaw = Quaternion::CreateFromAxisAngle(cam_up, -1.0f * speed * mouse_delta.x);
			cam_forward = Normalize(Vector3::Transform(cam_forward, quat_pitch * quat_yaw));

			if (abs(mouse_delta.x) > 0.0 || abs(mouse_delta.y) > 0.0)
			{
				global_constant_buffer_data.frames_rendered = 0;
			}
		}

		// Basic Fly-Camera
		{
			float move_speed = 0.5f; //TODO: delta_time
			if (IsKeyPressed(VK_SHIFT)) { move_speed *= 10.0f; }

			const Vector3 cam_right = Cross(cam_forward, cam_up);

			auto MoveInput = [&](int key, Vector3 move_vec)
			{
				if (IsKeyPressed(key))
				{
					cam_pos += move_vec;
					global_constant_buffer_data.frames_rendered = 0;
				}
			};

			MoveInput('W', cam_forward * move_speed);
			MoveInput('S', -cam_forward* move_speed);
			MoveInput('D', cam_right * move_speed);
			MoveInput('A', -cam_right * move_speed);
			MoveInput('E', cam_up * move_speed);
			MoveInput('Q', -cam_up* move_speed);
		}

		{
			Vector3 cam_target = cam_pos + cam_forward;
			const Matrix view = Matrix::CreateLookAt(cam_pos, cam_target, cam_up);
			global_constant_buffer_data.view = view;
			global_constant_buffer_data.view_inverse = view.Invert();

			float fieldOfView = 3.14159f / 4.0f; //PI / 4 : 90 degrees
			float aspectRatio = (float) render_width / (float) render_height;
			const Matrix proj = Matrix::CreatePerspectiveFieldOfView(fieldOfView, aspectRatio, 0.01f, 10000.0f);
			global_constant_buffer_data.projection = proj;
			global_constant_buffer_data.projection_inverse = proj.Invert();
		}

		//Update current frame's constant buffer
		global_constant_buffers[frame_data.current_backbuffer_index].Write(&global_constant_buffer_data, sizeof(global_constant_buffer_data));

		// Process any messages in the queue.
		MSG msg = {};
		while (PeekMessage(&msg, window, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		{ // Rendering

			//FCS TODO: RenderGraph should manage its own command_list(s)... should pass in a command allocator to RenderGraphDesc
			HR_CHECK(frame_data.get_command_allocator()->Reset());
			HR_CHECK(command_list->Reset(frame_data.get_command_allocator(), nullptr));

			// FCS TODO: Basic Test (3 nodes)
			// 1. [DONE] Rasterize scene in color, output that texture
			// 2. [TODO] convert to grayscale
			// 3. [DONE] copy to swapchain + present
			// Render Graph Testing
			RenderGraph render_graph(RenderGraphDesc
			{
				.device = device,
				.allocator = gpu_memory_allocator,
				.command_list = command_list,
			});

			const DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			const D3D12_CLEAR_VALUE clear_color =
			{
				.Format = color_format,
				.Color = { 0.39f, 0.58f, 0.93f, 1.0f },
			};

			const DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT;
			const D3D12_CLEAR_VALUE clear_depth =
			{
				.Format = depth_format,
				.DepthStencil =
				{
					.Depth = 1.0f,
					.Stencil = 0,
				},
			};

			// Static to prevent cleanup. TODO: pool these in some manager.
			static ComPtr<ID3D12PipelineState> first_node_pipeline_state = GraphicsPipelineBuilder()
				.with_root_signature(global_root_signature)
				.with_vs(CompileVertexShader(L"Shaders/RenderGraphTest.hlsl", L"FirstNodeVertexShader"))
				.with_ps(CompilePixelShader(L"Shaders/RenderGraphTest.hlsl", L"FirstNodePixelShader"))
				.with_primitive_topology(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
				.with_depth_enabled(true)
				.with_dsv_format(depth_format)
				.with_rtv_formats({ color_format })
				.with_debug_name(L"first_node_pipeline_state")
			.build(device);

			//Add some nodes
			render_graph.AddNode(RenderGraphNodeDesc
			{
				.name = "first_node",
				.setup = [&](RenderGraphNode& self)
				{
					self.AddTextureOutput("color", RenderGraphTextureDesc
					{
						.width = render_width,
						.height = render_height,
						.format = color_format,
						.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
						.resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
						.optimized_clear_value = clear_color,
					});

					self.AddTextureOutput("depth", RenderGraphTextureDesc
					{
						.width = render_width,
						.height = render_height,
						.format = depth_format,
						.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
						.resource_state = D3D12_RESOURCE_STATE_DEPTH_WRITE,
						.optimized_clear_value = clear_depth,
					});
				},
				.execute = [&](RenderGraphNode& self, ComPtr<ID3D12GraphicsCommandList4> command_list)
				{
					command_list->SetDescriptorHeaps(1, bindless_resource_manager.GetDescriptorHeap().GetAddressOf());
					command_list->SetGraphicsRootSignature(global_root_signature.Get());
					command_list->SetPipelineState(first_node_pipeline_state.Get());

					command_list->SetGraphicsRootConstantBufferView(0, global_constant_buffers[frame_data.current_backbuffer_index].GetGPUVirtualAddress());

					RenderGraphOutput& color_output = self.GetOutput("color");
					D3D12_CPU_DESCRIPTOR_HANDLE& rtv_handle = color_output.GetRtvHandle(device);
					RenderGraphOutput& depth_output = self.GetOutput("depth");
					D3D12_CPU_DESCRIPTOR_HANDLE& depth_handle = depth_output.GetDsvHandle(device);

					command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &depth_handle);
					command_list->ClearRenderTargetView(rtv_handle, clear_color.Color, 0, nullptr);
					command_list->ClearDepthStencilView(depth_handle, D3D12_CLEAR_FLAG_DEPTH, clear_depth.DepthStencil.Depth, 0, 0, nullptr);
					command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

					D3D12_VIEWPORT viewport =
					{
						.TopLeftX = 0.0f,
						.TopLeftY = 0.0f,
						.Width = static_cast<FLOAT>(render_width),
						.Height = static_cast<FLOAT>(render_height),
						.MinDepth = 0.0f,
						.MaxDepth = 1.0f,
					};
					command_list->RSSetViewports(1, &viewport);

					const D3D12_RECT scissor =
					{
						.left = 0,
						.top = 0,
						.right = (LONG) render_width,
						.bottom = (LONG) render_height,
					};
					command_list->RSSetScissorRects(1, &scissor);
					command_list->DrawInstanced(num_indices, 1, 0, 0);
				},
			});

			render_graph.AddNode(RenderGraphNodeDesc
			{
				.name = "convert_to_grayscale",
				.setup = [&](RenderGraphNode& self)
				{
					self.AddTextureInput("input", RenderGraphTextureDesc
					{
						.width = render_width,
						.height = render_height,
						.format = DXGI_FORMAT_R8G8B8A8_UNORM,
						.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
						.resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
					});

					self.AddTextureOutput("output", RenderGraphTextureDesc
					{
						.width = render_width,
						.height = render_height,
						.format = DXGI_FORMAT_R8G8B8A8_UNORM,
						.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
						.resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
						.optimized_clear_value = clear_color,
					});
				},
				.execute = [&](RenderGraphNode& self, ComPtr<ID3D12GraphicsCommandList4> command_list)
				{
					//FCS TODO: Need way to pass uniforms to shaders?
					//FCS TODO: Just use PSO root constants
					//FCS TODO: Compute shader to convert input to grayscale output
				},
			});

			render_graph.AddNode(RenderGraphNodeDesc
			{
				.name = "copy_to_backbuffer",
				.setup = [&](RenderGraphNode& self)
				{
					self.AddTextureInput("input", RenderGraphTextureDesc
					{
						.width = render_width,
						.height = render_height,
						.format = DXGI_FORMAT_R8G8B8A8_UNORM,
						.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
						.resource_state = D3D12_RESOURCE_STATE_COPY_SOURCE,
					});
				},
				.execute = [&](RenderGraphNode& self, ComPtr<ID3D12GraphicsCommandList4> command_list)
				{
					RenderGraphInput& input = self.GetInput("input");
					CmdCopyTexture2D(command_list, frame_data.get_render_target(), input.GetD3D12Resource());
					CmdBarrier(command_list, frame_data.get_render_target(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
				},
			});

			render_graph.AddNode(RenderGraphNodeDesc
			{
				.name = "present_node",
				.setup = [](RenderGraphNode& self)
				{},
				.execute = [&command_queue, &frame_data](RenderGraphNode& self, ComPtr<ID3D12GraphicsCommandList4> command_list)
				{
					HR_CHECK(command_list->Close()); //TODO: REMOVE

					ID3D12CommandList* ppCommandLists[] = { command_list.Get() };
					command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

					// Present the frame.
					frame_data.present();
				},
			});

			// Add connection
			render_graph.AddEdge(RenderGraphEdge
			{
				.incoming_node = "first_node",
				.incoming_resource = "color",
				.outgoing_node = "copy_to_backbuffer",
				.outgoing_resource = "input",
			});

			// Add connection with no resources
			render_graph.AddEdge(RenderGraphEdge
			{
				.incoming_node = "copy_to_backbuffer",
				.incoming_resource = STL_IMPL::nullopt,
				.outgoing_node = "present_node",
				.outgoing_resource = STL_IMPL::nullopt,
			});

			// Execute the render graph
			render_graph.Execute();
			// Prevent graph from being cleaned up until the frame is done presenting
			frame_data.register_graph(move(render_graph));
			// Potentially wait for a frame to free up
			frame_data.wait_for_previous_frame(command_queue);
		}

		if (IsKeyPressed(VK_ESCAPE))
		{
			should_close = true;
		}
	}

	wait_gpu_idle(device, command_queue);

	frame_data.reset();
	return 0;
}
