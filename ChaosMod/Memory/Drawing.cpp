#include <stdafx.h>

#include "Memory/Drawing.h"
#include "Memory/Hooks/PresentHook.h"

namespace Drawing
{
	static std::mutex g_LineMutex;
	static std::vector<Line> g_BuildLines;
	static std::shared_ptr<const std::vector<Line>> g_RenderLines = std::make_shared<std::vector<Line>>();
	static std::vector<LineVertex> g_Vertices;

	static UINT g_BackBufferWidth  = 0;
	static UINT g_BackBufferHeight = 0;

	static const char *g_VS        = R"(
		struct VSInput
		{
			float2 Pos   : POSITION;
			float4 Color : COLOR0;
		};

		struct PSInput
		{
			float4 Pos   : SV_POSITION;
			float4 Color : COLOR0;
		};

		PSInput main(VSInput input)
		{
			PSInput output;
			output.Pos   = float4(input.Pos.xy, 0.0f, 1.0f);
			output.Color = input.Color;
			return output;
		}
	)";

	static const char *g_PS        = R"(
		struct PSInput
		{
			float4 Pos   : SV_POSITION;
			float4 Color : COLOR0;
		};

		float4 main(PSInput input) : SV_Target
		{
			return input.Color;
		}
	)";

	template <typename T> static void ReleasePtr(T *&ptr)
	{
		if (ptr)
		{
			ptr->Release();
			ptr = nullptr;
		}
	}

	static LineVertex MakeVertex(float x, float y, Color color)
	{
		return LineVertex { .x = x * 2.0f - 1.0f,
			                .y = 1.0f - y * 2.0f,
			                .r = color.R / 255.0f,
			                .g = color.G / 255.0f,
			                .b = color.B / 255.0f,
			                .a = color.A / 255.0f };
	}

	static void AppendLineVertices(std::vector<LineVertex> &vertices, const Line &line, float screenWidth,
	                               float screenHeight)
	{
		const float dxPixels = (line.x2 - line.x1) * screenWidth;
		const float dyPixels = (line.y2 - line.y1) * screenHeight;
		const float length   = std::sqrt(dxPixels * dxPixels + dyPixels * dyPixels);

		if (length <= 0.001f)
			return;

		const float thicknessPixels     = std::max(line.thickness, 1.0f);
		const float halfThicknessPixels = thicknessPixels * 0.5f;
		const float perpX               = -dyPixels / length;
		const float perpY               = dxPixels / length;

		const float offsetX             = (perpX * halfThicknessPixels) / screenWidth;
		const float offsetY             = (perpY * halfThicknessPixels) / screenHeight;

		const float ax                  = line.x1 - offsetX;
		const float ay                  = line.y1 - offsetY;
		const float bx                  = line.x1 + offsetX;
		const float by                  = line.y1 + offsetY;
		const float cx                  = line.x2 - offsetX;
		const float cy                  = line.y2 - offsetY;
		const float dx                  = line.x2 + offsetX;
		const float dy                  = line.y2 + offsetY;

		vertices.push_back(MakeVertex(ax, ay, line.color));
		vertices.push_back(MakeVertex(bx, by, line.color));
		vertices.push_back(MakeVertex(cx, cy, line.color));

		vertices.push_back(MakeVertex(cx, cy, line.color));
		vertices.push_back(MakeVertex(bx, by, line.color));
		vertices.push_back(MakeVertex(dx, dy, line.color));
	}

	void BeginFrame(size_t estimatedLineCount)
	{
		std::lock_guard lock(g_LineMutex);

		g_BuildLines.clear();
		if (estimatedLineCount > 0 && g_BuildLines.capacity() < estimatedLineCount)
			g_BuildLines.reserve(estimatedLineCount);
	}

	void QueueLine(float x1, float y1, float x2, float y2, Color color, float thickness)
	{
		if (x1 == x2 && y1 == y2)
			return;

		std::lock_guard lock(g_LineMutex);
		g_BuildLines.push_back(Line { .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2, .thickness = thickness, .color = color });
	}

	void EndFrame()
	{
		std::lock_guard lock(g_LineMutex);

		auto frame = std::make_shared<std::vector<Line>>();
		frame->swap(g_BuildLines);
		g_RenderLines = frame;
	}

	void ClearFrame()
	{
		std::lock_guard lock(g_LineMutex);

		g_BuildLines.clear();
		g_RenderLines = std::make_shared<std::vector<Line>>();
	}

	static bool BuildVertices()
	{
		std::shared_ptr<const std::vector<Line>> lines;
		{
			std::lock_guard lock(g_LineMutex);
			lines = g_RenderLines;
		}

		if (!lines || lines->empty())
			return false;

		g_Vertices.clear();
		g_Vertices.reserve(lines->size() * 6);

		const float width  = static_cast<float>(g_BackBufferWidth);
		const float height = static_cast<float>(g_BackBufferHeight);

		for (const auto &line : *lines)
			AppendLineVertices(g_Vertices, line, width, height);

		return !g_Vertices.empty();
	}

	// D3D11 backend
	static ID3D11Device *g_Device11                = nullptr;
	static ID3D11DeviceContext *g_Context11        = nullptr;
	static ID3D11VertexShader *g_VertexShader11    = nullptr;
	static ID3D11PixelShader *g_PixelShader11      = nullptr;
	static ID3D11InputLayout *g_InputLayout11      = nullptr;
	static ID3D11Buffer *g_VertexBuffer11          = nullptr;
	static ID3D11BlendState *g_BlendState11        = nullptr;
	static ID3D11DepthStencilState *g_DepthState11 = nullptr;
	static ID3D11RasterizerState *g_RasterState11  = nullptr;
	static ID3D11RenderTargetView *g_RTV11         = nullptr;
	static IDXGISwapChain *g_RTVSwapChain11        = nullptr;
	static size_t g_VertexBufferCapacity11         = 0;
	static bool g_Initialized11                    = false;

	static void ReleaseRenderTarget11()
	{
		ReleasePtr(g_RTV11);
		ReleasePtr(g_RTVSwapChain11);

		g_BackBufferWidth  = 0;
		g_BackBufferHeight = 0;
	}

	static bool CreateStates11()
	{
		D3D11_BLEND_DESC blendDesc {};
		blendDesc.RenderTarget[0].BlendEnable           = TRUE;
		blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		if (FAILED(g_Device11->CreateBlendState(&blendDesc, &g_BlendState11)))
			return false;

		D3D11_DEPTH_STENCIL_DESC depthDesc {};
		depthDesc.DepthEnable    = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc      = D3D11_COMPARISON_ALWAYS;

		if (FAILED(g_Device11->CreateDepthStencilState(&depthDesc, &g_DepthState11)))
			return false;

		D3D11_RASTERIZER_DESC rasterDesc {};
		rasterDesc.FillMode              = D3D11_FILL_SOLID;
		rasterDesc.CullMode              = D3D11_CULL_NONE;
		rasterDesc.ScissorEnable         = FALSE;
		rasterDesc.DepthClipEnable       = TRUE;
		rasterDesc.MultisampleEnable     = FALSE;
		rasterDesc.AntialiasedLineEnable = FALSE;

		if (FAILED(g_Device11->CreateRasterizerState(&rasterDesc, &g_RasterState11)))
			return false;

		return true;
	}

	static bool Init11(IDXGISwapChain *swapChain)
	{
		if (g_Initialized11)
			return true;

		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_Device11))))
			return false;

		g_Device11->GetImmediateContext(&g_Context11);
		if (!g_Context11)
			return false;

		ID3DBlob *vsBlob    = nullptr;
		ID3DBlob *psBlob    = nullptr;
		ID3DBlob *errorBlob = nullptr;

		HRESULT hr =
		    D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
		if (FAILED(hr))
		{
			ReleasePtr(errorBlob);
			return false;
		}

		hr = D3DCompile(g_PS, strlen(g_PS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(errorBlob);
			return false;
		}

		hr = g_Device11->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
		                                    &g_VertexShader11);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			return false;
		}

		hr = g_Device11->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
		                                   &g_PixelShader11);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			return false;
		}

		D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 2, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		hr = g_Device11->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
		                                   &g_InputLayout11);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			return false;
		}

		if (!CreateStates11())
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			return false;
		}

		ReleasePtr(vsBlob);
		ReleasePtr(psBlob);

		g_Initialized11 = true;
		return true;
	}

	static bool EnsureVertexBuffer11(size_t vertexCount)
	{
		if (vertexCount == 0)
			return false;

		if (g_VertexBuffer11 && vertexCount <= g_VertexBufferCapacity11)
			return true;

		ReleasePtr(g_VertexBuffer11);

		g_VertexBufferCapacity11 = vertexCount + 128;

		D3D11_BUFFER_DESC desc {};
		desc.Usage          = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth      = static_cast<UINT>(sizeof(LineVertex) * g_VertexBufferCapacity11);
		desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		return SUCCEEDED(g_Device11->CreateBuffer(&desc, nullptr, &g_VertexBuffer11));
	}

	static bool EnsureRenderTarget11(IDXGISwapChain *swapChain)
	{
		DXGI_SWAP_CHAIN_DESC sd {};
		if (FAILED(swapChain->GetDesc(&sd)))
			return false;

		const UINT width  = std::max(sd.BufferDesc.Width, 1u);
		const UINT height = std::max(sd.BufferDesc.Height, 1u);

		if (g_RTV11 && g_RTVSwapChain11 == swapChain && g_BackBufferWidth == width && g_BackBufferHeight == height)
			return true;

		ReleaseRenderTarget11();

		ID3D11Texture2D *backBuffer = nullptr;
		if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer))))
			return false;

		const HRESULT hr = g_Device11->CreateRenderTargetView(backBuffer, nullptr, &g_RTV11);
		backBuffer->Release();

		if (FAILED(hr))
			return false;

		g_RTVSwapChain11 = swapChain;
		g_RTVSwapChain11->AddRef();
		g_BackBufferWidth  = width;
		g_BackBufferHeight = height;

		return true;
	}

	static void Render11(IDXGISwapChain *swapChain)
	{
		if (!swapChain || !Init11(swapChain) || !EnsureRenderTarget11(swapChain))
			return;

		if (!BuildVertices() || !EnsureVertexBuffer11(g_Vertices.size()))
			return;

		D3D11_MAPPED_SUBRESOURCE mapped {};
		if (FAILED(g_Context11->Map(g_VertexBuffer11, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;

		memcpy(mapped.pData, g_Vertices.data(), sizeof(LineVertex) * g_Vertices.size());
		g_Context11->Unmap(g_VertexBuffer11, 0);

		ID3D11RenderTargetView *oldRTV         = nullptr;
		ID3D11DepthStencilView *oldDSV         = nullptr;
		ID3D11BlendState *oldBlendState        = nullptr;
		ID3D11DepthStencilState *oldDepthState = nullptr;
		ID3D11RasterizerState *oldRasterState  = nullptr;
		ID3D11InputLayout *oldInputLayout      = nullptr;
		ID3D11Buffer *oldVertexBuffer          = nullptr;
		ID3D11VertexShader *oldVS              = nullptr;
		ID3D11PixelShader *oldPS               = nullptr;

		FLOAT oldBlendFactor[4]                = {};
		UINT oldSampleMask                     = 0xFFFFFFFF;
		UINT oldStencilRef                     = 0;
		UINT oldStride                         = 0;
		UINT oldOffset                         = 0;
		D3D11_PRIMITIVE_TOPOLOGY oldTopology;
		D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

		g_Context11->OMGetRenderTargets(1, &oldRTV, &oldDSV);
		g_Context11->RSGetViewports(&oldViewportCount, oldViewports);
		g_Context11->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);
		g_Context11->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);
		g_Context11->RSGetState(&oldRasterState);
		g_Context11->IAGetInputLayout(&oldInputLayout);
		g_Context11->IAGetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
		g_Context11->IAGetPrimitiveTopology(&oldTopology);
		g_Context11->VSGetShader(&oldVS, nullptr, nullptr);
		g_Context11->PSGetShader(&oldPS, nullptr, nullptr);

		D3D11_VIEWPORT vp {};
		vp.TopLeftX          = 0.0f;
		vp.TopLeftY          = 0.0f;
		vp.Width             = static_cast<float>(g_BackBufferWidth);
		vp.Height            = static_cast<float>(g_BackBufferHeight);
		vp.MinDepth          = 0.0f;
		vp.MaxDepth          = 1.0f;

		const UINT stride    = sizeof(LineVertex);
		const UINT offset    = 0;
		FLOAT blendFactor[4] = { 0, 0, 0, 0 };

		g_Context11->OMSetRenderTargets(1, &g_RTV11, nullptr);
		g_Context11->RSSetViewports(1, &vp);
		g_Context11->OMSetBlendState(g_BlendState11, blendFactor, 0xFFFFFFFF);
		g_Context11->OMSetDepthStencilState(g_DepthState11, 0);
		g_Context11->RSSetState(g_RasterState11);
		g_Context11->IASetInputLayout(g_InputLayout11);
		g_Context11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_Context11->IASetVertexBuffers(0, 1, &g_VertexBuffer11, &stride, &offset);
		g_Context11->VSSetShader(g_VertexShader11, nullptr, 0);
		g_Context11->PSSetShader(g_PixelShader11, nullptr, 0);
		g_Context11->Draw(static_cast<UINT>(g_Vertices.size()), 0);

		g_Context11->OMSetRenderTargets(1, &oldRTV, oldDSV);
		g_Context11->RSSetViewports(oldViewportCount, oldViewports);
		g_Context11->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
		g_Context11->OMSetDepthStencilState(oldDepthState, oldStencilRef);
		g_Context11->RSSetState(oldRasterState);
		g_Context11->IASetInputLayout(oldInputLayout);
		g_Context11->IASetPrimitiveTopology(oldTopology);
		g_Context11->IASetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
		g_Context11->VSSetShader(oldVS, nullptr, 0);
		g_Context11->PSSetShader(oldPS, nullptr, 0);

		ReleasePtr(oldRTV);
		ReleasePtr(oldDSV);
		ReleasePtr(oldBlendState);
		ReleasePtr(oldDepthState);
		ReleasePtr(oldRasterState);
		ReleasePtr(oldInputLayout);
		ReleasePtr(oldVertexBuffer);
		ReleasePtr(oldVS);
		ReleasePtr(oldPS);
	}

	static void Cleanup11()
	{
		ReleaseRenderTarget11();
		ReleasePtr(g_VertexBuffer11);
		ReleasePtr(g_BlendState11);
		ReleasePtr(g_DepthState11);
		ReleasePtr(g_RasterState11);
		ReleasePtr(g_InputLayout11);
		ReleasePtr(g_VertexShader11);
		ReleasePtr(g_PixelShader11);
		ReleasePtr(g_Context11);
		ReleasePtr(g_Device11);

		g_VertexBufferCapacity11 = 0;
		g_Initialized11          = false;
	}

	// D3D12 backend
	struct D3D12FrameContext
	{
		ID3D12CommandAllocator *CommandAllocator = nullptr;
		ID3D12Resource *RenderTarget             = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle    = {};
		UINT64 FenceValue                        = 0;
	};

	static ID3D12Device *g_Device12                   = nullptr;
	static ID3D12CommandQueue *g_CommandQueue12       = nullptr;
	static IDXGISwapChain3 *g_SwapChain12             = nullptr;
	static ID3D12RootSignature *g_RootSignature12     = nullptr;
	static ID3D12PipelineState *g_PipelineState12     = nullptr;
	static ID3D12GraphicsCommandList *g_CommandList12 = nullptr;
	static ID3D12DescriptorHeap *g_RtvHeap12          = nullptr;
	static ID3D12Resource *g_VertexBuffer12           = nullptr;
	static ID3D12Fence *g_Fence12                     = nullptr;
	static HANDLE g_FenceEvent12                      = nullptr;
	static std::vector<D3D12FrameContext> g_Frames12;
	static D3D12_VERTEX_BUFFER_VIEW g_VertexBufferView12 = {};
	static UINT g_RtvDescriptorSize12                    = 0;
	static UINT64 g_NextFenceValue12                     = 1;
	static size_t g_VertexBufferCapacity12               = 0;
	static DXGI_FORMAT g_BackBufferFormat12              = DXGI_FORMAT_R8G8B8A8_UNORM;
	static bool g_Initialized12                          = false;

	static bool WaitForFence12(UINT64 fenceValue)
	{
		if (!g_Fence12 || fenceValue == 0)
			return true;

		if (g_Fence12->GetCompletedValue() >= fenceValue)
			return true;

		if (!g_FenceEvent12)
			return false;

		if (FAILED(g_Fence12->SetEventOnCompletion(fenceValue, g_FenceEvent12)))
			return false;

		return WaitForSingleObject(g_FenceEvent12, INFINITE) == WAIT_OBJECT_0;
	}

	static void Flush12()
	{
		if (!g_CommandQueue12 || !g_Fence12)
			return;

		const UINT64 fenceValue = g_NextFenceValue12++;
		if (SUCCEEDED(g_CommandQueue12->Signal(g_Fence12, fenceValue)))
			WaitForFence12(fenceValue);
	}

	static void ResetFrameResources12()
	{
		Flush12();

		for (auto &frame : g_Frames12)
		{
			ReleasePtr(frame.RenderTarget);
			ReleasePtr(frame.CommandAllocator);
			frame.RtvHandle.ptr = 0;
			frame.FenceValue    = 0;
		}

		g_Frames12.clear();
		ReleasePtr(g_RtvHeap12);
		ReleasePtr(g_SwapChain12);

		g_BackBufferWidth  = 0;
		g_BackBufferHeight = 0;
	}

	static bool EnsureRenderTargets12(IDXGISwapChain *swapChain)
	{
		IDXGISwapChain3 *swapChain3 = nullptr;
		if (FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&swapChain3))))
			return false;

		DXGI_SWAP_CHAIN_DESC sd {};
		if (FAILED(swapChain3->GetDesc(&sd)))
		{
			swapChain3->Release();
			return false;
		}

		const UINT width       = std::max(sd.BufferDesc.Width, 1u);
		const UINT height      = std::max(sd.BufferDesc.Height, 1u);
		const UINT bufferCount = std::max(sd.BufferCount, 1u);

		if (g_SwapChain12 == swapChain3 && !g_Frames12.empty() && g_BackBufferWidth == width
		    && g_BackBufferHeight == height)
		{
			swapChain3->Release();
			return true;
		}

		ResetFrameResources12();

		g_SwapChain12      = swapChain3;
		g_BackBufferWidth  = width;
		g_BackBufferHeight = height;
		g_BackBufferFormat12 =
		    sd.BufferDesc.Format != DXGI_FORMAT_UNKNOWN ? sd.BufferDesc.Format : DXGI_FORMAT_R8G8B8A8_UNORM;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
		heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = bufferCount;

		if (FAILED(g_Device12->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap),
		                                            reinterpret_cast<void **>(&g_RtvHeap12))))
		{
			ResetFrameResources12();
			return false;
		}

		g_RtvDescriptorSize12 = g_Device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		g_Frames12.resize(bufferCount);

		auto handle = g_RtvHeap12->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < bufferCount; i++)
		{
			if (FAILED(g_Device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			                                              __uuidof(ID3D12CommandAllocator),
			                                              reinterpret_cast<void **>(&g_Frames12[i].CommandAllocator))))
			{
				ResetFrameResources12();
				return false;
			}

			if (FAILED(g_SwapChain12->GetBuffer(i, __uuidof(ID3D12Resource),
			                                    reinterpret_cast<void **>(&g_Frames12[i].RenderTarget))))
			{
				ResetFrameResources12();
				return false;
			}

			g_Frames12[i].RtvHandle = handle;
			g_Device12->CreateRenderTargetView(g_Frames12[i].RenderTarget, nullptr, handle);
			handle.ptr += g_RtvDescriptorSize12;
		}

		return true;
	}

	static bool EnsureVertexBuffer12(size_t vertexCount)
	{
		if (vertexCount == 0)
			return false;

		if (g_VertexBuffer12 && vertexCount <= g_VertexBufferCapacity12)
			return true;

		ReleasePtr(g_VertexBuffer12);

		g_VertexBufferCapacity12 = vertexCount + 128;

		D3D12_HEAP_PROPERTIES heapProps {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC resourceDesc {};
		resourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width            = sizeof(LineVertex) * g_VertexBufferCapacity12;
		resourceDesc.Height           = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels        = 1;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		if (FAILED(g_Device12->CreateCommittedResource(
		        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_VertexBuffer12))))
			return false;

		g_VertexBufferView12.BufferLocation = g_VertexBuffer12->GetGPUVirtualAddress();
		g_VertexBufferView12.SizeInBytes    = static_cast<UINT>(sizeof(LineVertex) * g_VertexBufferCapacity12);
		g_VertexBufferView12.StrideInBytes  = sizeof(LineVertex);

		return true;
	}

	static bool Init12(IDXGISwapChain *swapChain)
	{
		if (g_Initialized12)
			return EnsureRenderTargets12(swapChain);

		ID3D12CommandQueue *commandQueue = Hooks::GetD3D12CommandQueue();
		if (!commandQueue)
			return false;

		if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&g_Device12))))
			return false;

		g_CommandQueue12 = commandQueue;
		g_CommandQueue12->AddRef();

		if (!EnsureRenderTargets12(swapChain))
			return false;

		ID3DBlob *vsBlob      = nullptr;
		ID3DBlob *psBlob      = nullptr;
		ID3DBlob *rootSigBlob = nullptr;
		ID3DBlob *errorBlob   = nullptr;

		HRESULT hr =
		    D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
		if (FAILED(hr))
		{
			ReleasePtr(errorBlob);
			return false;
		}

		hr = D3DCompile(g_PS, strlen(g_PS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(errorBlob);
			return false;
		}

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc {};
		rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &errorBlob);
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(errorBlob);
			return false;
		}

		hr = g_Device12->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(),
		                                     __uuidof(ID3D12RootSignature),
		                                     reinterpret_cast<void **>(&g_RootSignature12));
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(rootSigBlob);
			return false;
		}

		D3D12_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 2,
			  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_BLEND_DESC blendDesc {};
		blendDesc.RenderTarget[0].BlendEnable           = TRUE;
		blendDesc.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		D3D12_RASTERIZER_DESC rasterDesc {};
		rasterDesc.FillMode        = D3D12_FILL_MODE_SOLID;
		rasterDesc.CullMode        = D3D12_CULL_MODE_NONE;
		rasterDesc.DepthClipEnable = TRUE;

		D3D12_DEPTH_STENCIL_DESC depthDesc {};
		depthDesc.DepthEnable    = FALSE;
		depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc {};
		psoDesc.pRootSignature        = g_RootSignature12;
		psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
		psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
		psoDesc.BlendState            = blendDesc;
		psoDesc.SampleMask            = UINT_MAX;
		psoDesc.RasterizerState       = rasterDesc;
		psoDesc.DepthStencilState     = depthDesc;
		psoDesc.InputLayout           = { layout, 2 };
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets      = 1;
		psoDesc.RTVFormats[0]         = g_BackBufferFormat12;
		psoDesc.SampleDesc.Count      = 1;

		hr                            = g_Device12->CreateGraphicsPipelineState(&psoDesc, __uuidof(ID3D12PipelineState),
		                                                                        reinterpret_cast<void **>(&g_PipelineState12));
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(rootSigBlob);
			return false;
		}

		hr = g_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_Frames12[0].CommandAllocator,
		                                   g_PipelineState12, __uuidof(ID3D12GraphicsCommandList),
		                                   reinterpret_cast<void **>(&g_CommandList12));
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(rootSigBlob);
			return false;
		}

		g_CommandList12->Close();

		hr = g_Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
		                             reinterpret_cast<void **>(&g_Fence12));
		if (FAILED(hr))
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(rootSigBlob);
			return false;
		}

		g_FenceEvent12 = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!g_FenceEvent12)
		{
			ReleasePtr(vsBlob);
			ReleasePtr(psBlob);
			ReleasePtr(rootSigBlob);
			return false;
		}

		ReleasePtr(vsBlob);
		ReleasePtr(psBlob);
		ReleasePtr(rootSigBlob);

		g_Initialized12 = true;
		return true;
	}

	static void Render12(IDXGISwapChain *swapChain)
	{
		if (!swapChain || !Init12(swapChain) || !EnsureRenderTargets12(swapChain))
			return;

		if (!BuildVertices() || !EnsureVertexBuffer12(g_Vertices.size()))
			return;

		void *mapped = nullptr;
		D3D12_RANGE readRange { 0, 0 };
		if (FAILED(g_VertexBuffer12->Map(0, &readRange, &mapped)))
			return;

		memcpy(mapped, g_Vertices.data(), sizeof(LineVertex) * g_Vertices.size());
		g_VertexBuffer12->Unmap(0, nullptr);

		const UINT backBufferIndex = g_SwapChain12->GetCurrentBackBufferIndex();
		auto &frame                = g_Frames12[backBufferIndex];

		if (!WaitForFence12(frame.FenceValue))
			return;

		if (FAILED(frame.CommandAllocator->Reset()))
			return;

		if (FAILED(g_CommandList12->Reset(frame.CommandAllocator, g_PipelineState12)))
			return;

		D3D12_RESOURCE_BARRIER toRenderTarget {};
		toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toRenderTarget.Transition.pResource   = frame.RenderTarget;
		toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;

		D3D12_RESOURCE_BARRIER toPresent      = toRenderTarget;
		toPresent.Transition.StateBefore      = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toPresent.Transition.StateAfter       = D3D12_RESOURCE_STATE_PRESENT;

		D3D12_VIEWPORT vp {};
		vp.Width    = static_cast<float>(g_BackBufferWidth);
		vp.Height   = static_cast<float>(g_BackBufferHeight);
		vp.MaxDepth = 1.0f;

		D3D12_RECT scissor {};
		scissor.right  = static_cast<LONG>(g_BackBufferWidth);
		scissor.bottom = static_cast<LONG>(g_BackBufferHeight);

		g_CommandList12->ResourceBarrier(1, &toRenderTarget);
		g_CommandList12->OMSetRenderTargets(1, &frame.RtvHandle, FALSE, nullptr);
		g_CommandList12->RSSetViewports(1, &vp);
		g_CommandList12->RSSetScissorRects(1, &scissor);
		g_CommandList12->SetGraphicsRootSignature(g_RootSignature12);
		g_CommandList12->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_CommandList12->IASetVertexBuffers(0, 1, &g_VertexBufferView12);
		g_CommandList12->DrawInstanced(static_cast<UINT>(g_Vertices.size()), 1, 0, 0);
		g_CommandList12->ResourceBarrier(1, &toPresent);

		if (FAILED(g_CommandList12->Close()))
			return;

		ID3D12CommandList *commandLists[] = { g_CommandList12 };
		g_CommandQueue12->ExecuteCommandLists(1, commandLists);

		frame.FenceValue = g_NextFenceValue12++;
		g_CommandQueue12->Signal(g_Fence12, frame.FenceValue);
	}

	static void Cleanup12()
	{
		Flush12();
		ResetFrameResources12();

		ReleasePtr(g_VertexBuffer12);
		ReleasePtr(g_Fence12);
		ReleasePtr(g_CommandList12);
		ReleasePtr(g_PipelineState12);
		ReleasePtr(g_RootSignature12);
		ReleasePtr(g_CommandQueue12);
		ReleasePtr(g_Device12);

		if (g_FenceEvent12)
		{
			CloseHandle(g_FenceEvent12);
			g_FenceEvent12 = nullptr;
		}

		g_VertexBufferView12     = {};
		g_RtvDescriptorSize12    = 0;
		g_NextFenceValue12       = 1;
		g_VertexBufferCapacity12 = 0;
		g_Initialized12          = false;
	}

	void Render(IDXGISwapChain *swapChain)
	{
		if (IsEnhanced())
			Render12(swapChain);
		else
			Render11(swapChain);
	}

	void Cleanup()
	{
		{
			std::lock_guard lock(g_LineMutex);
			g_BuildLines.clear();
			g_RenderLines = std::make_shared<std::vector<Line>>();
		}

		g_Vertices.clear();

		Cleanup11();
		Cleanup12();
	}
}