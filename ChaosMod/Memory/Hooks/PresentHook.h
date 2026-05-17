#pragma once

#include "Util/Events.h"

struct ID3D12CommandQueue;

namespace Hooks
{
	inline ChaosEvent OnPresent;

	void SetD3D12CommandQueue(ID3D12CommandQueue *commandQueue);
	ID3D12CommandQueue *GetD3D12CommandQueue();
}