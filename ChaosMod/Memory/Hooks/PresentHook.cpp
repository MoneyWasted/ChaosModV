#include <stdafx.h>

#include <d3d12.h>

#include "PresentHook.h"

#include "Memory/Drawing.h"
#include "Memory/Hooks/Hook.h"
#include "Memory/Memory.h"

static void **ms_PresentAddr                    = nullptr;

static ID3D12CommandQueue *ms_D3D12CommandQueue = nullptr;

HRESULT (*OG_IDXGISwapChain_Present)(IDXGISwapChain *, UINT, UINT);
void (*OG_ID3D12CommandQueue_ExecuteCommandLists)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

namespace Hooks
{
	void SetD3D12CommandQueue(ID3D12CommandQueue *commandQueue)
	{
		if (ms_D3D12CommandQueue == commandQueue)
			return;

		if (commandQueue)
			commandQueue->AddRef();

		if (ms_D3D12CommandQueue)
			ms_D3D12CommandQueue->Release();

		ms_D3D12CommandQueue = commandQueue;
	}

	ID3D12CommandQueue *GetD3D12CommandQueue()
	{
		return ms_D3D12CommandQueue;
	}
}

static void HK_ID3D12CommandQueue_ExecuteCommandLists(ID3D12CommandQueue *commandQueue, UINT numCommandLists,
                                                      ID3D12CommandList *const *commandLists)
{
	Hooks::SetD3D12CommandQueue(commandQueue);
	OG_ID3D12CommandQueue_ExecuteCommandLists(commandQueue, numCommandLists, commandLists);
}

HRESULT HK_IDXGISwapChain_Present(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags)
{
	if (!(flags & DXGI_PRESENT_TEST))
	{
		Hooks::OnPresent.Fire();
		Drawing::Render(swapChain);
	}

	return OG_IDXGISwapChain_Present(swapChain, syncInterval, flags);
}

static bool HookD3D12CommandQueue()
{
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
	                             reinterpret_cast<void **>(&device))))
		return false;

	D3D12_COMMAND_QUEUE_DESC queueDesc {};
	queueDesc.Type            = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ID3D12CommandQueue *queue = nullptr;
	if (FAILED(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue))))
	{
		device->Release();
		return false;
	}

	void **vtable = *reinterpret_cast<void ***>(queue);
	const MH_STATUS status =
	    Memory::AddHook(reinterpret_cast<void *>(vtable[10]), HK_ID3D12CommandQueue_ExecuteCommandLists,
	                    &OG_ID3D12CommandQueue_ExecuteCommandLists);

	queue->Release();
	device->Release();

	return status == MH_OK || status == MH_ERROR_ALREADY_CREATED;
}

static bool OnHook()
{
	if (IsEnhanced() && !HookD3D12CommandQueue())
		return false;

	Handle handle;

	handle = Memory::FindPattern("00 80 7E 10 00 48 8B", "48 8B 18 48 8D 0D");
	if (!handle.IsValid())
		return false;

	handle                    = *handle.At(IsLegacy() ? 7 : 5).Into().Value<DWORD64 *>();
	ms_PresentAddr            = handle.At(64).Get<void *>();
	OG_IDXGISwapChain_Present = *(HRESULT(**)(IDXGISwapChain *, UINT, UINT))ms_PresentAddr;

	Memory::Write<void *>(ms_PresentAddr, reinterpret_cast<void *>(HK_IDXGISwapChain_Present));
	return true;
}

static void OnCleanup()
{
	Drawing::Cleanup();

	if (ms_PresentAddr && *ms_PresentAddr == reinterpret_cast<void *>(HK_IDXGISwapChain_Present))
		Memory::Write<void *>(ms_PresentAddr, reinterpret_cast<void *>(OG_IDXGISwapChain_Present));

	Hooks::SetD3D12CommandQueue(nullptr);

	ms_PresentAddr                            = nullptr;
	OG_IDXGISwapChain_Present                 = nullptr;
	OG_ID3D12CommandQueue_ExecuteCommandLists = nullptr;
}

static RegisterHook registerHook(OnHook, OnCleanup, "IDXGISwapChain::Present", true);