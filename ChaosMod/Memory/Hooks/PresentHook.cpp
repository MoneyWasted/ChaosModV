#include <stdafx.h>

#include "PresentHook.h"

#include "Memory/Drawing.h"
#include "Memory/Hooks/Hook.h"

static void **ms_PresentAddr = nullptr;

HRESULT (*OG_IDXGISwapChain_Present)(IDXGISwapChain *, UINT, UINT);

HRESULT HK_IDXGISwapChain_Present(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags)
{
	if (!(flags & DXGI_PRESENT_TEST))
	{
		Hooks::OnPresent.Fire();
		Drawing::Render(swapChain);
	}

	return OG_IDXGISwapChain_Present(swapChain, syncInterval, flags);
}

static bool OnHook()
{
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

	ms_PresentAddr            = nullptr;
	OG_IDXGISwapChain_Present = nullptr;
}

static RegisterHook registerHook(OnHook, OnCleanup, "IDXGISwapChain::Present", true);