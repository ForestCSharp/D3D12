#pragma once

#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;

#define HR_CHECK(expr)  \
{\
	HRESULT result = (expr);\
	if (FAILED(result))\
	{\
		printf("FAILED HRESULT: line: %u, code: %s, error: %x", __LINE__, #expr, result);\
		DebugBreak();\
		exit(-1);\
	}\
}\

#if defined(_DEBUG) || defined(DBG)
#define NAME_D3D12_OBJECT(obj, name) obj->SetName(name);
#else
#define NAME_D3D12_OBJECT(obj, name) 
#endif

#define XOR(a,b) a ? !b : b

#define STL_IMPL std

#define DISALLOW_COPY(CLASS)                    \
	CLASS(const CLASS&) = delete;               \
	CLASS& operator=(const CLASS&) = delete

#define DEFAULT_COPY(CLASS)                 	\
	CLASS(const CLASS&) = default;              \
	CLASS& operator=(const CLASS&) = default

#define DISALLOW_MOVE(CLASS)                    \
	CLASS(CLASS&&) = delete;                    \
	CLASS& operator=(CLASS&&) = delete

#define DEFAULT_MOVE(CLASS)                    \
	CLASS(CLASS&&) = default;                  \
	CLASS& operator=(CLASS&&) = default

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

inline float randf()
{
	return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

inline float map(float input_number, float input_min, float input_max, float ouptut_min, float output_max)
{
	return ouptut_min + (input_number - input_min) * (output_max - ouptut_min) / (input_max - input_min);
}

inline float rand_range(float min, float max)
{
	return map(randf(), 0.0f, 1.0f, min, max);
}