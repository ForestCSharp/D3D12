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
	UINT frame_index = 0;
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
		frame_index = swapchain->GetCurrentBackBufferIndex();

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
		return command_allocators[frame_index].Get();
	}

	ID3D12Resource* get_render_target() const
	{
		return render_targets[frame_index].Get();
	}

	void present()
	{
		HR_CHECK(swapchain->Present(1, 0));
	}

	void wait_for_previous_frame(ComPtr<ID3D12CommandQueue> command_queue)
	{
		// Signal and increment the fence value.
		const UINT64 current_fence_value = fence_values[frame_index];
		HR_CHECK(command_queue->Signal(fence.Get(), current_fence_value));

		frame_index = swapchain->GetCurrentBackBufferIndex();
		// Wait until the previous frame is finished.
		if (fence->GetCompletedValue() < fence_values[frame_index])
		{
			HR_CHECK(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
			WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
		}

		fence_values[frame_index] = current_fence_value + 1;
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

	UINT width = 1280; //TODO: Rename to render_width
	UINT height = 720; //TODO: Rename to render_height
	RECT window_rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
	AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND window = CreateWindow(
		window_class.lpszClassName,
		L"DXR_SingleFile",
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
		.width = width,
		.height = height,
		.device = device,
		.factory = factory,
		.command_queue = command_queue,
		.window = window,
	};
	FrameData frame_data(frame_data_create_info);

	//Compile raytracing shaders
	ComPtr<IDxcBlob> raytracing_shader_lib = CompileShaderLibrary(L"Shaders/Raytracing.hlsl");

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
	SceneConstantBuffer scene_constant_buffer_data =
	{
		.sun_dir = Vector4(0.5, 0.25, 1, 0),
	};
	const size_t scene_constant_buffer_size = ROUND_UP(sizeof(SceneConstantBuffer), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	GpuBuffer scene_constant_buffers[frame_count];
	for (int32_t frame_index = 0; frame_index < frame_count; ++frame_index)
	{
		GpuBuffer scene_constant_buffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = scene_constant_buffer_size,
			.heap_type = D3D12_HEAP_TYPE_UPLOAD,
			.resource_flags = D3D12_RESOURCE_FLAG_NONE,
			.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
		});
		scene_constant_buffer.Write(&scene_constant_buffer_data, sizeof(SceneConstantBuffer));
		scene_constant_buffers[frame_index] = scene_constant_buffer;
	}

	BindlessResourceManager bindless_resource_manager(device);
	
	ComPtr<ID3D12RootSignature> global_root_signature;
	ComPtr<ID3D12RootSignature> local_root_signature;
	{
		auto SerializeAndCreateRaytracingRootSignature = [&](D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* p_root_signature)
		{
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> error;

			HR_CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
			HR_CHECK(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*p_root_signature))));
		};

		{
			// FCS TODO: Don't add these to desc_table
			// Instead Send Constant buffer via table (with indices for these 2 resources accessed via "ResourceDescriptorHeap"
			// See https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_6.html
			// See https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3D12_DESCRIPTOR_RANGE descriptor_ranges[] =
			{
				{
				.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
				.NumDescriptors = NUM_BINDLESS_DESCRIPTORS_PER_TYPE,
				.BaseShaderRegister = 0,
				.RegisterSpace = 0,
				.OffsetInDescriptorsFromTableStart = 0, //Different Descriptor Types can overlap
				},
				{
				.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
				.NumDescriptors = NUM_BINDLESS_DESCRIPTORS_PER_TYPE,
				.BaseShaderRegister = 0,
				.RegisterSpace = 0,
				.OffsetInDescriptorsFromTableStart = 0, //Different Descriptor Types can overlap
				},
				{
				.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
				.NumDescriptors = NUM_BINDLESS_DESCRIPTORS_PER_TYPE,
				.BaseShaderRegister = 0,
				.RegisterSpace = 0,
				.OffsetInDescriptorsFromTableStart = 0, //Different Descriptor Types can overlap
				},
			};

			D3D12_ROOT_PARAMETER descriptor_table_root_param = 
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable =
				{
					.NumDescriptorRanges = _countof(descriptor_ranges),
					.pDescriptorRanges = descriptor_ranges,
				},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
			};

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

			D3D12_ROOT_PARAMETER root_params[] = { descriptor_table_root_param, scene_cbv_root_param };

			D3D12_ROOT_SIGNATURE_DESC root_signature_desc = 
			{
				.NumParameters = _countof(root_params),
				.pParameters = root_params,
				.NumStaticSamplers = 0,
				.pStaticSamplers = nullptr,
				.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
						| D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
			};
			SerializeAndCreateRaytracingRootSignature(root_signature_desc, &global_root_signature);
		}
		
		{
			// Create an empty local root signature
			D3D12_ROOT_SIGNATURE_DESC root_signature_desc = 
			{
				.NumParameters = 0,
				.pParameters = nullptr,
				.NumStaticSamplers = 0,
				.pStaticSamplers = nullptr,
				.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE,
			};
			SerializeAndCreateRaytracingRootSignature(root_signature_desc, &local_root_signature);
		}
	}

	ComPtr<ID3D12StateObject> dxr_state_object;
	{
		// Create the minimal number of D3D12_STATE_SUBOBJECT for raytracing

		//Need to set up subobject memory first so we can have back-references in the array
		D3D12_STATE_SUBOBJECT subobjects[7]; //FCS TODO: hard-coded for now
		memset(subobjects, 0, sizeof(subobjects));
		int32_t current_index = 0;

		// DXIL Library Subobject
		D3D12_SHADER_BYTECODE shader_bytecode = {};
		shader_bytecode.pShaderBytecode = raytracing_shader_lib->GetBufferPointer();
		shader_bytecode.BytecodeLength = raytracing_shader_lib->GetBufferSize();

		D3D12_EXPORT_DESC exports[] =
		{
			{
				.Name = raygen_shader_name,
				.ExportToRename = nullptr,
				.Flags = D3D12_EXPORT_FLAG_NONE,
			},
			{
				.Name = closest_hit_shader_name,
				.ExportToRename = nullptr,
				.Flags = D3D12_EXPORT_FLAG_NONE,
			},
			{
				.Name = miss_shader_name,
				.ExportToRename = nullptr,
				.Flags = D3D12_EXPORT_FLAG_NONE,
			}
		};

		D3D12_DXIL_LIBRARY_DESC dxil_lib_desc = {};
		dxil_lib_desc.DXILLibrary = shader_bytecode;
		dxil_lib_desc.NumExports = _countof(exports);
		dxil_lib_desc.pExports = exports;

		D3D12_STATE_SUBOBJECT& dxil_lib_subobject = subobjects[current_index++];
		dxil_lib_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		dxil_lib_subobject.pDesc = &dxil_lib_desc;

		//Hit Group Subobject
		D3D12_HIT_GROUP_DESC hit_group_desc = {};
		hit_group_desc.AnyHitShaderImport = nullptr;
		hit_group_desc.ClosestHitShaderImport = closest_hit_shader_name;
		hit_group_desc.IntersectionShaderImport = nullptr;
		hit_group_desc.HitGroupExport = hit_group_name;
		hit_group_desc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

		D3D12_STATE_SUBOBJECT& hit_group_subobject = subobjects[current_index++];
		hit_group_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		hit_group_subobject.pDesc = &hit_group_desc;

		//Shader config subobject
		D3D12_RAYTRACING_SHADER_CONFIG shader_config_desc = {};
		shader_config_desc.MaxPayloadSizeInBytes = sizeof(RayPayload);
		shader_config_desc.MaxAttributeSizeInBytes = 2 * sizeof(float);

		D3D12_STATE_SUBOBJECT& shader_config_subobject = subobjects[current_index++];
		shader_config_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		shader_config_subobject.pDesc = &shader_config_desc;

		//Local Root Signature subobject
		D3D12_LOCAL_ROOT_SIGNATURE local_root_signature_desc = {};
		local_root_signature_desc.pLocalRootSignature = local_root_signature.Get();

		D3D12_STATE_SUBOBJECT& local_root_signature_subobject = subobjects[current_index++];
		local_root_signature_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		local_root_signature_subobject.pDesc = &local_root_signature_desc;

		//Local Root Signature to Exports Association
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION local_root_signature_to_exports_desc = {};
		local_root_signature_to_exports_desc.pSubobjectToAssociate = &local_root_signature_subobject;
		local_root_signature_to_exports_desc.NumExports = 1;
		local_root_signature_to_exports_desc.pExports = &raygen_shader_name;

		D3D12_STATE_SUBOBJECT& local_root_signature_to_exports_subobject = subobjects[current_index++];
		local_root_signature_to_exports_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		local_root_signature_to_exports_subobject.pDesc = &local_root_signature_to_exports_desc;

		//Global Root Signature Subobject
		D3D12_GLOBAL_ROOT_SIGNATURE global_root_signature_desc = {};
		global_root_signature_desc.pGlobalRootSignature = global_root_signature.Get();

		D3D12_STATE_SUBOBJECT& global_root_signature_subobject = subobjects[current_index++];
		global_root_signature_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
		global_root_signature_subobject.pDesc = &global_root_signature_desc;

		//Pipeline Config
		D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config_desc = {};
		pipeline_config_desc.MaxTraceRecursionDepth = 1;

		D3D12_STATE_SUBOBJECT& pipeline_config_subobject = subobjects[current_index++];
		pipeline_config_subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		pipeline_config_subobject.pDesc = &pipeline_config_desc;

		//Build State Object
		D3D12_STATE_OBJECT_DESC state_object_desc = {};
		state_object_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		state_object_desc.NumSubobjects = _countof(subobjects);
		state_object_desc.pSubobjects = subobjects;

		HR_CHECK(device->CreateStateObject(&state_object_desc, IID_PPV_ARGS(&dxr_state_object)));
	}

	GpuBuffer bottom_level_acceleration_structure;
	GpuBuffer top_level_acceleration_structure;
	{
		D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
		geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometryDesc.Triangles.IndexBuffer = index_buffer.GetGPUVirtualAddress();
		geometryDesc.Triangles.IndexCount = static_cast<UINT>(index_buffer.GetSize()) / sizeof(uint32_t);
		geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geometryDesc.Triangles.Transform3x4 = 0;
		geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometryDesc.Triangles.VertexCount = static_cast<UINT>(vertex_buffer.GetSize()) / sizeof(Vertex);
		geometryDesc.Triangles.VertexBuffer.StartAddress = vertex_buffer.GetGPUVirtualAddress();
		geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

		// Mark the geometry as opaque. 
		// PERFORMANCE TIP: mark geometry as opaque whenever applicable as it can enable important ray processing optimizations.
		// Note: When rays encounter opaque geometry an any hit shader will not be executed whether it is present or not.
		geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

		const int32_t num_instances = 1000;

		// Get required sizes for an acceleration structure.
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top_level_inputs = {};
		top_level_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		top_level_inputs.Flags = buildFlags;
		top_level_inputs.NumDescs = num_instances;
		top_level_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&top_level_inputs, &topLevelPrebuildInfo);
		HR_CHECK(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomLevelInputs = {};
		bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		bottomLevelInputs.Flags = buildFlags;
		bottomLevelInputs.pGeometryDescs = &geometryDesc;
		bottomLevelInputs.NumDescs = 1;
		device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &bottomLevelPrebuildInfo);
		HR_CHECK(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

		uint64_t scratch_size = max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes);
		GpuBuffer scratch_buffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = scratch_size,
			.heap_type = D3D12_HEAP_TYPE_DEFAULT,
			.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			.resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		});

		bottom_level_acceleration_structure = GpuBuffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes,
			.heap_type = D3D12_HEAP_TYPE_DEFAULT,
			.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			.resource_state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
		});

		top_level_acceleration_structure = GpuBuffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = topLevelPrebuildInfo.ResultDataMaxSizeInBytes,
			.heap_type = D3D12_HEAP_TYPE_DEFAULT,
			.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			.resource_state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
		});

		//bottom_level_acceleration_structure = GpuBuffer(device, gpu_memory_allocator, bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		//top_level_acceleration_structure = GpuBuffer(device, gpu_memory_allocator, topLevelPrebuildInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

		vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_descs;
		for (int32_t i = 0; i < num_instances; ++i)
		{

			D3D12_RAYTRACING_INSTANCE_DESC new_instance_desc = {};

			const float random_span = num_instances * 1.25f;

			if (i == 0) // Ground
			{
				const float ground_dimensions = 5000.0f;

				// Scale
				new_instance_desc.Transform[0][0] = ground_dimensions;
				new_instance_desc.Transform[1][1] = 10.0f;
				new_instance_desc.Transform[2][2] = ground_dimensions;

				// Position
				new_instance_desc.Transform[0][3] = 0.0f;
				new_instance_desc.Transform[1][3] = -random_span / 2.0f;
				new_instance_desc.Transform[2][3] = 0.0f;
			}
			else if (i < 3) // Walls
			{
				const float wall_width = 1000.0f;
				const float wall_height = 1000.0f;
				const float wall_thickness = 10.0f;

				// Scale
				new_instance_desc.Transform[0][0] = i < 2 ? wall_width : wall_thickness;
				new_instance_desc.Transform[1][1] = wall_height;
				new_instance_desc.Transform[2][2] = i < 2 ? wall_thickness : wall_width;

				// Position
				new_instance_desc.Transform[0][3] = 2000.0f;
				new_instance_desc.Transform[1][3] = -random_span / 2.0f;
				new_instance_desc.Transform[2][3] = 2000.0f;
			}
			else // Random Cubes
			{
				Vector3 random_pos = Vector3(randf() - 0.5f, randf() - 0.5f, randf() - 0.5f) * random_span;
				const float cube_min_scale = 5.0f;
				const float cube_max_scale = 40.0;
				new_instance_desc.Transform[0][0] = rand_range(cube_min_scale, cube_max_scale);
				new_instance_desc.Transform[1][1] = rand_range(cube_min_scale, cube_max_scale);
				new_instance_desc.Transform[2][2] = rand_range(cube_min_scale, cube_max_scale);
				new_instance_desc.Transform[0][3] = random_pos.x;
				new_instance_desc.Transform[1][3] = random_pos.y;
				new_instance_desc.Transform[2][3] = random_pos.z;
			}

			new_instance_desc.InstanceID = i;
			new_instance_desc.InstanceMask = 1;
			new_instance_desc.Flags = 0;
			new_instance_desc.AccelerationStructure = bottom_level_acceleration_structure.GetGPUVirtualAddress();
			instance_descs.push_back(new_instance_desc);

		}
		//// Create an instance desc for the bottom-level acceleration structure.
		size_t instance_descs_size = instance_descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
		GpuBuffer instance_desc_buffer = GpuBuffer(GpuBufferDesc{
			.allocator = gpu_memory_allocator,
			.size = instance_descs_size,
			.heap_type = D3D12_HEAP_TYPE_UPLOAD,
			.resource_flags = D3D12_RESOURCE_FLAG_NONE,
			.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
		});

		instance_desc_buffer.Write(instance_descs.data(), instance_descs_size);

		// Bottom Level Acceleration Structure desc
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
		{
			bottomLevelBuildDesc.Inputs = bottomLevelInputs;
			bottomLevelBuildDesc.ScratchAccelerationStructureData = scratch_buffer.GetGPUVirtualAddress();
			bottomLevelBuildDesc.DestAccelerationStructureData = bottom_level_acceleration_structure.GetGPUVirtualAddress();
		}

		// Top Level Acceleration Structure desc
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {};
		{
			top_level_inputs.InstanceDescs = instance_desc_buffer.GetGPUVirtualAddress();
			topLevelBuildDesc.Inputs = top_level_inputs;
			topLevelBuildDesc.ScratchAccelerationStructureData = scratch_buffer.GetGPUVirtualAddress();
			topLevelBuildDesc.DestAccelerationStructureData = top_level_acceleration_structure.GetGPUVirtualAddress();
		}

		//// Build acceleration structure.
		command_list->Reset(frame_data.get_command_allocator(), nullptr);
		command_list->BuildRaytracingAccelerationStructure(&bottomLevelBuildDesc, 0, nullptr);
		const D3D12_RESOURCE_BARRIER barrier = 
		{
			.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
			.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
			.UAV = 
			{
				.pResource = bottom_level_acceleration_structure.GetResource(),
			}
		};
		command_list->ResourceBarrier(1, &barrier);
		command_list->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);
		HR_CHECK(command_list->Close());

		ID3D12CommandList* ppCommandLists[] = { command_list.Get() };
		command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		wait_gpu_idle(device, command_queue);
	}

	//Shader Tables
	void* raygen_shader_identifier = nullptr;
	void* miss_shader_identifier = nullptr;
	void* hit_group_shader_identifier = nullptr;

	auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
	{
		raygen_shader_identifier = stateObjectProperties->GetShaderIdentifier(raygen_shader_name);
		miss_shader_identifier = stateObjectProperties->GetShaderIdentifier(miss_shader_name);
		hit_group_shader_identifier = stateObjectProperties->GetShaderIdentifier(hit_group_name);
	};

	// Get shader identifiers.
	UINT shader_identifier_size;
	{
		ComPtr<ID3D12StateObjectProperties> state_object_properties;
		HR_CHECK(dxr_state_object.As(&state_object_properties));
		GetShaderIdentifiers(state_object_properties.Get());
		shader_identifier_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	}

	struct ShaderRecord
	{
		ShaderRecord(void* pShaderIdentifier, UINT shaderIdentifierSize) :
			shaderIdentifier(pShaderIdentifier, shaderIdentifierSize)
		{}

		ShaderRecord(void* pShaderIdentifier, UINT shaderIdentifierSize, void* pLocalRootArguments, UINT localRootArgumentsSize) :
			shaderIdentifier(pShaderIdentifier, shaderIdentifierSize),
			localRootArguments(pLocalRootArguments, localRootArgumentsSize)
		{}

		void CopyTo(void* dest) const
		{
			uint8_t* byteDest = static_cast<uint8_t*>(dest);
			memcpy(byteDest, shaderIdentifier.ptr, shaderIdentifier.size);
			if (localRootArguments.ptr)
			{
				memcpy(byteDest + shaderIdentifier.size, localRootArguments.ptr, localRootArguments.size);
			}
		}

		struct PointerWithSize {
			void* ptr;
			UINT size;

			PointerWithSize() : ptr(nullptr), size(0) {}
			PointerWithSize(void* _ptr, UINT _size) : ptr(_ptr), size(_size) {};
		};
		PointerWithSize shaderIdentifier;
		PointerWithSize localRootArguments;
	};

	class ShaderTable
	{
	public:
		inline UINT Align(UINT size, UINT alignment)
		{
			return (size + (alignment - 1)) & ~(alignment - 1);
		}

		ShaderTable(ComPtr<ID3D12Device5> in_device, D3D12MA::Allocator* in_gpu_memory_allocator, UINT numShaderRecords, UINT shaderRecordSize, LPCWSTR resource_name)
		{
			m_shader_record_size = Align(shaderRecordSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
			m_shader_records.reserve(numShaderRecords);
			UINT bufferSize = numShaderRecords * m_shader_record_size;

			m_buffer = GpuBuffer(GpuBufferDesc{
				.allocator = in_gpu_memory_allocator,
				.size = bufferSize,
				.heap_type = D3D12_HEAP_TYPE_UPLOAD,
				.resource_flags = D3D12_RESOURCE_FLAG_NONE,
				.resource_state = D3D12_RESOURCE_STATE_GENERIC_READ
			});
			m_buffer.Map(reinterpret_cast<void**>(&m_mapped_shader_records));
		}

		void push_back(const ShaderRecord& shaderRecord)
		{
			assert(m_shader_records.size() < m_shader_records.capacity());
			m_shader_records.push_back(shaderRecord);
			shaderRecord.CopyTo(m_mapped_shader_records);
			m_mapped_shader_records += m_shader_record_size;
		}

		UINT GetShaderRecordSize() { return m_shader_record_size; }

		ID3D12Resource* GetResource() const { return m_buffer.GetResource(); }
	protected:
		uint8_t* m_mapped_shader_records;
		UINT m_shader_record_size;

		// Debug support
		wstring m_name;
		vector<ShaderRecord> m_shader_records;

		GpuBuffer m_buffer;
	};

	UINT numShaderRecords = 1;
	UINT shaderRecordSize = shader_identifier_size;

	// Ray gen shader table
	ShaderTable raygen_shader_table(device, gpu_memory_allocator, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
	raygen_shader_table.push_back(ShaderRecord(raygen_shader_identifier, shader_identifier_size));

	// Miss shader table
	ShaderTable miss_shader_table(device, gpu_memory_allocator, numShaderRecords, shaderRecordSize, L"MissShaderTable");
	miss_shader_table.push_back(ShaderRecord(miss_shader_identifier, shader_identifier_size));

	// Hit group shader table
	ShaderTable hit_group_shader_table(device, gpu_memory_allocator, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
	hit_group_shader_table.push_back(ShaderRecord(hit_group_shader_identifier, shader_identifier_size));

	//Raytracing Output Resource
	GpuTexture lighting_buffer_texture(GpuTextureDesc{
		.allocator = gpu_memory_allocator,
		.width = width,
		.height = height,
		.format = DXGI_FORMAT_R32G32B32A32_FLOAT,
		.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		.resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	});

	GpuTexture raytracing_output_texture(GpuTextureDesc{
		.allocator = gpu_memory_allocator,
		.width = width,
		.height = height,
		.format = frame_data.swap_chain_format,
		.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		.resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	});

	// Actually assign Descriptors
	{
		//bindless_resource_manager.RegisterCBV(scene_constant_buffer);
		scene_constant_buffer_data.lighting_buffer_index = bindless_resource_manager.RegisterUAV(lighting_buffer_texture);
		scene_constant_buffer_data.output_buffer_index = bindless_resource_manager.RegisterUAV(raytracing_output_texture);
		scene_constant_buffer_data.tlas_buffer_index = bindless_resource_manager.RegisterAccelerationStructure(top_level_acceleration_structure);
		scene_constant_buffer_data.indices_index = bindless_resource_manager.RegisterSRV(index_buffer, num_indices, sizeof(uint32_t));
		scene_constant_buffer_data.vertices_index = bindless_resource_manager.RegisterSRV(vertex_buffer, num_vertices, sizeof(Vertex));
	}

	wait_gpu_idle(device, command_queue);

	Vector3 cam_pos = Vector3(0, 0, -5);
	Vector3 cam_up = Vector3(0, 1, 0);
	Vector3 cam_forward = Vector3(0, 0, 1);

	int previous_mouse_x = 0;
	int previous_mouse_y = 0;
	Vector2 mouse_delta(0.f, 0.f);

	scene_constant_buffer_data.frames_rendered = 0;

	while (!should_close)
	{
		RECT client_rect;
		if (GetClientRect(window, &client_rect))
		{
			LONG new_width = client_rect.right - client_rect.left;
			LONG new_height = client_rect.bottom - client_rect.top;
			if (new_width != width || new_height != height)
			{
				width = new_width;
				height = new_height;
				printf("Width: %lu Height: %lu\n", new_width, new_height);

				// Update width and height and call resize
				frame_data_create_info.width = width;
				frame_data_create_info.height = height;
				frame_data.resize(frame_data_create_info);

				//TODO: Make bindless unregister automatic(keep optional ref to resource manager when you register it, use that to unregister in destructor)
				
				// Resize other resources
				bindless_resource_manager.UnregisterResource(lighting_buffer_texture);
				lighting_buffer_texture = GpuTexture(GpuTextureDesc{
					.allocator = gpu_memory_allocator,
					.width = width,
					.height = height,
					.format = DXGI_FORMAT_R32G32B32A32_FLOAT,
					.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					.resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
				});
				scene_constant_buffer_data.lighting_buffer_index = bindless_resource_manager.RegisterUAV(lighting_buffer_texture);

				bindless_resource_manager.UnregisterResource(raytracing_output_texture);
				raytracing_output_texture = GpuTexture(GpuTextureDesc{
					.allocator = gpu_memory_allocator,
					.width = width,
					.height = height,
					.format = frame_data.swap_chain_format,
					.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					.resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
				});
				scene_constant_buffer_data.output_buffer_index = bindless_resource_manager.RegisterUAV(raytracing_output_texture);

				scene_constant_buffer_data.frames_rendered = 0;
			}
		}

		scene_constant_buffer_data.frames_rendered += 1;
		scene_constant_buffer_data.random = rand();

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
			scene_constant_buffer_data.sun_dir = Normalize(Vector4::Transform(scene_constant_buffer_data.sun_dir, quat_pitch * quat_yaw));
			scene_constant_buffer_data.frames_rendered = 0;
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
				scene_constant_buffer_data.frames_rendered = 0;
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
					scene_constant_buffer_data.frames_rendered = 0;
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
			scene_constant_buffer_data.view = view;
			scene_constant_buffer_data.view_inverse = view.Invert();

			float fieldOfView = 3.14159f / 4.0f; //PI / 4 : 90 degrees
			float aspectRatio = (float) width / (float) height;
			const Matrix proj = Matrix::CreatePerspectiveFieldOfView(fieldOfView, aspectRatio, 0.01f, 10000.0f);
			scene_constant_buffer_data.projection = proj;
			scene_constant_buffer_data.projection_inverse = proj.Invert();
		}

		//Update current frame's constant buffer
		scene_constant_buffers[frame_data.frame_index].Write(&scene_constant_buffer_data, sizeof(scene_constant_buffer_data));

		// Process any messages in the queue.
		MSG msg = {};
		while (PeekMessage(&msg, window, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		{ // Rendering

			// Raytracing
			const bool use_render_graph = true;
			if (use_render_graph)
			{
				//FCS TODO: RenderGraph should manage its own command_list... should pass in a command allocator to RenderGraphDesc
				HR_CHECK(frame_data.get_command_allocator()->Reset());
				HR_CHECK(command_list->Reset(frame_data.get_command_allocator(), nullptr));

				// FCS TODO: Basic Test (3 nodes)
				// 1. Rasterize scene in color, output that texture
				// 2. convert to grayscale
				// 3. copy to swapchain + present

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

				//FCS TODO: isn't living long enough
				ComPtr<ID3D12PipelineState> first_node_pipeline_state = GraphicsPipelineBuilder()
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
							.width = width,
							.height = height,
							.format = color_format,
							.resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
							.resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
							.optimized_clear_value = clear_color,
						});

						self.AddTextureOutput("depth", RenderGraphTextureDesc
						{
							.width = width,
							.height = height,
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

						command_list->SetGraphicsRootDescriptorTable(0, bindless_resource_manager.GetGpuHandle());
						command_list->SetGraphicsRootConstantBufferView(1, scene_constant_buffers[frame_data.frame_index].GetGPUVirtualAddress());

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
							.Width = static_cast<FLOAT>(width),
							.Height = static_cast<FLOAT>(height),
							.MinDepth = 0.0f,
							.MaxDepth = 1.0f,
						};
						command_list->RSSetViewports(1, &viewport);

						const D3D12_RECT scissor =
						{
							.left = 0,
							.top = 0,
							.right = (LONG) width,
							.bottom = (LONG) height,
						};
						command_list->RSSetScissorRects(1, &scissor);

						//TODO: Draw Meshes (Use GPU Scene)

						D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view =
						{
							.BufferLocation = vertex_buffer.GetGPUVirtualAddress(),
							.SizeInBytes = (UINT)vertex_buffer.GetSize(),
							.StrideInBytes = sizeof(Vertex),
						};
						command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);

						D3D12_INDEX_BUFFER_VIEW index_buffer_view =
						{
							.BufferLocation = index_buffer.GetGPUVirtualAddress(),
							.SizeInBytes = (UINT) index_buffer.GetSize(),
							.Format = DXGI_FORMAT_R32_UINT,
						};
						command_list->IASetIndexBuffer(&index_buffer_view);

						command_list->DrawIndexedInstanced(num_indices, 1, 0, 0, 0);
					},
				});

				render_graph.AddNode(RenderGraphNodeDesc
				{
					.name = "copy_to_backbuffer",
					.setup = [&](RenderGraphNode& self)
					{
						self.AddTextureInput("input", RenderGraphTextureDesc
						{
							.width = width,
							.height = height,
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
						frame_data.wait_for_previous_frame(command_queue);
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

				//FCS TODO: Pipeline isn't living long enough.
				//^ Add a pipeline manager or some way to keep resources around until a specific frame is done rendering...
				wait_gpu_idle(device, command_queue);
			}
			else
			{
				// Raytracing
				HR_CHECK(frame_data.get_command_allocator()->Reset());
				HR_CHECK(command_list->Reset(frame_data.get_command_allocator(), nullptr));

				D3D12_RESOURCE_BARRIER begin_raytracing_barriers[] =
				{
					Transition(raytracing_output_texture.GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
				};
				command_list->ResourceBarrier(_countof(begin_raytracing_barriers), begin_raytracing_barriers);

				CmdBarrier(command_list, raytracing_output_texture.GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

				// Bind the heaps, acceleration structure and dispatch rays.    
				command_list->SetDescriptorHeaps(1, bindless_resource_manager.GetDescriptorHeap().GetAddressOf());
				command_list->SetComputeRootSignature(global_root_signature.Get());

				command_list->SetPipelineState1(dxr_state_object.Get());
				command_list->SetComputeRootDescriptorTable(0, bindless_resource_manager.GetGpuHandle());
				command_list->SetComputeRootConstantBufferView(1, scene_constant_buffers[frame_data.frame_index].GetGPUVirtualAddress());


				D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
				// Since each shader table has only one shader record, the stride is same as the size.
				dispatchDesc.HitGroupTable.StartAddress = hit_group_shader_table.GetResource()->GetGPUVirtualAddress();
				dispatchDesc.HitGroupTable.SizeInBytes = hit_group_shader_table.GetResource()->GetDesc().Width;
				dispatchDesc.HitGroupTable.StrideInBytes = dispatchDesc.HitGroupTable.SizeInBytes;
				dispatchDesc.MissShaderTable.StartAddress = miss_shader_table.GetResource()->GetGPUVirtualAddress();
				dispatchDesc.MissShaderTable.SizeInBytes = miss_shader_table.GetResource()->GetDesc().Width;
				dispatchDesc.MissShaderTable.StrideInBytes = dispatchDesc.MissShaderTable.SizeInBytes;
				dispatchDesc.RayGenerationShaderRecord.StartAddress = raygen_shader_table.GetResource()->GetGPUVirtualAddress();
				dispatchDesc.RayGenerationShaderRecord.SizeInBytes = raygen_shader_table.GetResource()->GetDesc().Width;
				dispatchDesc.Width = static_cast<UINT>(width);
				dispatchDesc.Height = static_cast<UINT>(height);
				dispatchDesc.Depth = 1;
				command_list->DispatchRays(&dispatchDesc);

				//Copy to Backbuffer
				ID3D12Resource* current_render_target = frame_data.get_render_target();
				D3D12_RESOURCE_BARRIER finish_raytracing_barriers[] =
				{
					Transition(current_render_target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
					Transition(raytracing_output_texture.GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
				};
				command_list->ResourceBarrier(_countof(finish_raytracing_barriers), finish_raytracing_barriers);
				command_list->CopyResource(current_render_target, raytracing_output_texture.GetResource());

				// Indicate that the back buffer will now be used to present.
				D3D12_RESOURCE_BARRIER rt_to_present_barrier = Transition(frame_data.get_render_target(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
				command_list->ResourceBarrier(1, &rt_to_present_barrier);

				HR_CHECK(command_list->Close());

				// Execute the command list.
				ID3D12CommandList* ppCommandLists[] = { command_list.Get() };
				command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

				// Present the frame.
				frame_data.present();
				frame_data.wait_for_previous_frame(command_queue);
			}
		}

		if (IsKeyPressed(VK_ESCAPE))
		{
			should_close = true;
		}
	}

	wait_gpu_idle(device, command_queue);

	frame_data.reset();

	//TODO: More Resource Cleanup here

	return 0;
}
