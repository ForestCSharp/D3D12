#pragma once

#include <d3d12.h>

#include "GpuResources.h"

// Assumes src + dst are the same size (or more accurately that dst is at least as big as src)
static inline void CmdCopyTexture2D(ComPtr<ID3D12GraphicsCommandList4> in_command_list, ID3D12Resource* in_dst, ID3D12Resource* in_src)
{
	D3D12_TEXTURE_COPY_LOCATION dst =
	{
		.pResource = in_dst,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};

	D3D12_TEXTURE_COPY_LOCATION src =
	{
		.pResource = in_src,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};

	D3D12_BOX src_box =
	{
		.left = 0,
		.top = 0,
		.front = 0,
		.right = (UINT) in_dst->GetDesc().Width,
		.bottom = in_dst->GetDesc().Height,
		.back = 1,
	};

	in_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &src_box);
}

/*  Almost identical to CD3DX12_RESOURCE_BARRIER::Transition in d3dx12.h
	(helper file provided by microsoft that we've opted to not use) */
static inline D3D12_RESOURCE_BARRIER Transition(
	_In_ ID3D12Resource* pResource,
	D3D12_RESOURCE_STATES stateBefore,
	D3D12_RESOURCE_STATES stateAfter,
	UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE)
{
	D3D12_RESOURCE_BARRIER result = {};
	result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	result.Flags = flags;
	result.Transition.pResource = pResource;
	result.Transition.StateBefore = stateBefore;
	result.Transition.StateAfter = stateAfter;
	result.Transition.Subresource = subresource;
	return result;
}

static inline void CmdBarrier(ComPtr<ID3D12GraphicsCommandList4> in_command_list, ID3D12Resource* in_resource, D3D12_RESOURCE_STATES in_state_before, D3D12_RESOURCE_STATES in_state_after)
{
	D3D12_RESOURCE_BARRIER barrier[] =
	{
		Transition(in_resource, in_state_before, in_state_after),
	};
	in_command_list->ResourceBarrier(_countof(barrier), barrier);
}