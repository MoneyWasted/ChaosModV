#pragma once

#include <cstddef>
#include <vector>

#include "Util/Color.h"
#include "Util/Events.h"

struct IDXGISwapChain;

namespace Drawing
{
	struct LineVertex
	{
		float x, y;
		float r, g, b, a;
	};

	struct Line
	{
		float x1, y1;
		float x2, y2;
		float thickness;
		Color color;
	};

	void BeginFrame(size_t estimatedLineCount = 0);
	void QueueLine(float x1, float y1, float x2, float y2, Color color, float thickness = 0.0f);
	void EndFrame();
	void ClearFrame();
	void Render(IDXGISwapChain *swapChain);
	void Cleanup();
}