#include <stdafx.h>

#include "Memory/Drawing.h"

namespace Drawing
{
	static std::mutex g_LineMutex;
	static std::vector<Line> g_BuildLines;
	static std::shared_ptr<const std::vector<Line>> g_RenderLines = std::make_shared<std::vector<Line>>();
	static std::vector<LineVertex> g_Vertices;

	static ID3D11Device *g_Device                = nullptr;
	static ID3D11DeviceContext *g_Context        = nullptr;
	static ID3D11VertexShader *g_VertexShader    = nullptr;
	static ID3D11PixelShader *g_PixelShader      = nullptr;
	static ID3D11InputLayout *g_InputLayout      = nullptr;
	static ID3D11Buffer *g_VertexBuffer          = nullptr;
	static ID3D11BlendState *g_BlendState        = nullptr;
	static ID3D11DepthStencilState *g_DepthState = nullptr;
	static ID3D11RasterizerState *g_RasterState  = nullptr;
	static ID3D11RenderTargetView *g_RTV         = nullptr;
	static IDXGISwapChain *g_RTVSwapChain        = nullptr;

	static UINT g_BackBufferWidth                = 0;
	static UINT g_BackBufferHeight               = 0;
	static size_t g_VertexBufferCapacity         = 0;
	static bool g_Initialized                    = false;

	static const char *g_VS                      = R"(
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

	static const char *g_PS                      = R"(
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

	static LineVertex MakeVertex(float x, float y, Color color)
	{
		return LineVertex { .x = x * 2.0f - 1.0f,
			                .y = 1.0f - y * 2.0f,
			                .r = color.R / 255.0f,
			                .g = color.G / 255.0f,
			                .b = color.B / 255.0f,
			                .a = color.A / 255.0f };
	}

	static void ReleaseRenderTarget()
	{
		if (g_RTV)
		{
			g_RTV->Release();
			g_RTV = nullptr;
		}

		if (g_RTVSwapChain)
		{
			g_RTVSwapChain->Release();
			g_RTVSwapChain = nullptr;
		}

		g_BackBufferWidth  = 0;
		g_BackBufferHeight = 0;
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

	static bool CreateStates()
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

		if (FAILED(g_Device->CreateBlendState(&blendDesc, &g_BlendState)))
			return false;

		D3D11_DEPTH_STENCIL_DESC depthDesc {};
		depthDesc.DepthEnable    = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc      = D3D11_COMPARISON_ALWAYS;

		if (FAILED(g_Device->CreateDepthStencilState(&depthDesc, &g_DepthState)))
			return false;

		D3D11_RASTERIZER_DESC rasterDesc {};
		rasterDesc.FillMode              = D3D11_FILL_SOLID;
		rasterDesc.CullMode              = D3D11_CULL_NONE;
		rasterDesc.ScissorEnable         = FALSE;
		rasterDesc.DepthClipEnable       = TRUE;
		rasterDesc.MultisampleEnable     = FALSE;
		rasterDesc.AntialiasedLineEnable = FALSE;

		if (FAILED(g_Device->CreateRasterizerState(&rasterDesc, &g_RasterState)))
			return false;

		return true;
	}

	static bool Init(IDXGISwapChain *swapChain)
	{
		if (g_Initialized)
			return true;

		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_Device))))
			return false;

		g_Device->GetImmediateContext(&g_Context);
		if (!g_Context)
			return false;

		ID3DBlob *vsBlob    = nullptr;
		ID3DBlob *psBlob    = nullptr;
		ID3DBlob *errorBlob = nullptr;

		HRESULT hr =
		    D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
		if (FAILED(hr))
		{
			if (errorBlob)
				errorBlob->Release();
			return false;
		}

		hr = D3DCompile(g_PS, strlen(g_PS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
		if (FAILED(hr))
		{
			if (vsBlob)
				vsBlob->Release();
			if (errorBlob)
				errorBlob->Release();
			return false;
		}

		hr =
		    g_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VertexShader);
		if (FAILED(hr))
		{
			if (vsBlob)
				vsBlob->Release();
			if (psBlob)
				psBlob->Release();
			return false;
		}

		hr = g_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PixelShader);
		if (FAILED(hr))
		{
			if (vsBlob)
				vsBlob->Release();
			if (psBlob)
				psBlob->Release();
			return false;
		}

		D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 2, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		hr =
		    g_Device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_InputLayout);
		if (FAILED(hr))
		{
			if (vsBlob)
				vsBlob->Release();
			if (psBlob)
				psBlob->Release();
			return false;
		}

		if (!CreateStates())
		{
			if (vsBlob)
				vsBlob->Release();
			if (psBlob)
				psBlob->Release();
			return false;
		}

		vsBlob->Release();
		psBlob->Release();

		g_Initialized = true;
		return true;
	}

	static bool EnsureVertexBuffer(size_t vertexCount)
	{
		if (vertexCount == 0)
			return false;

		if (g_VertexBuffer && vertexCount <= g_VertexBufferCapacity)
			return true;

		if (g_VertexBuffer)
		{
			g_VertexBuffer->Release();
			g_VertexBuffer = nullptr;
		}

		g_VertexBufferCapacity = vertexCount + 128;

		D3D11_BUFFER_DESC desc {};
		desc.Usage          = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth      = static_cast<UINT>(sizeof(LineVertex) * g_VertexBufferCapacity);
		desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		return SUCCEEDED(g_Device->CreateBuffer(&desc, nullptr, &g_VertexBuffer));
	}

	static bool EnsureRenderTarget(IDXGISwapChain *swapChain)
	{
		DXGI_SWAP_CHAIN_DESC sd {};
		if (FAILED(swapChain->GetDesc(&sd)))
			return false;

		const UINT width  = std::max(sd.BufferDesc.Width, 1u);
		const UINT height = std::max(sd.BufferDesc.Height, 1u);

		if (g_RTV && g_RTVSwapChain == swapChain && g_BackBufferWidth == width && g_BackBufferHeight == height)
			return true;

		ReleaseRenderTarget();

		ID3D11Texture2D *backBuffer = nullptr;
		if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer))))
			return false;

		const HRESULT hr = g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RTV);
		backBuffer->Release();

		if (FAILED(hr))
			return false;

		g_RTVSwapChain = swapChain;
		g_RTVSwapChain->AddRef();
		g_BackBufferWidth  = width;
		g_BackBufferHeight = height;

		return true;
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

	void Render(IDXGISwapChain *swapChain)
	{
		if (!swapChain || !Init(swapChain))
			return;

		std::shared_ptr<const std::vector<Line>> lines;
		{
			std::lock_guard lock(g_LineMutex);
			lines = g_RenderLines;
		}

		if (!lines || lines->empty())
			return;

		if (!EnsureRenderTarget(swapChain))
			return;

		g_Vertices.clear();
		g_Vertices.reserve(lines->size() * 6);

		const float width  = static_cast<float>(g_BackBufferWidth);
		const float height = static_cast<float>(g_BackBufferHeight);

		for (const auto &line : *lines)
			AppendLineVertices(g_Vertices, line, width, height);

		if (g_Vertices.empty() || !EnsureVertexBuffer(g_Vertices.size()))
			return;

		D3D11_MAPPED_SUBRESOURCE mapped {};
		if (FAILED(g_Context->Map(g_VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;

		memcpy(mapped.pData, g_Vertices.data(), sizeof(LineVertex) * g_Vertices.size());
		g_Context->Unmap(g_VertexBuffer, 0);

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

		g_Context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
		g_Context->RSGetViewports(&oldViewportCount, oldViewports);
		g_Context->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);
		g_Context->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);
		g_Context->RSGetState(&oldRasterState);
		g_Context->IAGetInputLayout(&oldInputLayout);
		g_Context->IAGetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
		g_Context->IAGetPrimitiveTopology(&oldTopology);
		g_Context->VSGetShader(&oldVS, nullptr, nullptr);
		g_Context->PSGetShader(&oldPS, nullptr, nullptr);

		D3D11_VIEWPORT vp {};
		vp.TopLeftX          = 0.0f;
		vp.TopLeftY          = 0.0f;
		vp.Width             = width;
		vp.Height            = height;
		vp.MinDepth          = 0.0f;
		vp.MaxDepth          = 1.0f;

		const UINT stride    = sizeof(LineVertex);
		const UINT offset    = 0;
		FLOAT blendFactor[4] = { 0, 0, 0, 0 };

		g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
		g_Context->RSSetViewports(1, &vp);
		g_Context->OMSetBlendState(g_BlendState, blendFactor, 0xFFFFFFFF);
		g_Context->OMSetDepthStencilState(g_DepthState, 0);
		g_Context->RSSetState(g_RasterState);

		g_Context->IASetInputLayout(g_InputLayout);
		g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_Context->IASetVertexBuffers(0, 1, &g_VertexBuffer, &stride, &offset);

		g_Context->VSSetShader(g_VertexShader, nullptr, 0);
		g_Context->PSSetShader(g_PixelShader, nullptr, 0);

		g_Context->Draw(static_cast<UINT>(g_Vertices.size()), 0);

		g_Context->OMSetRenderTargets(1, &oldRTV, oldDSV);
		g_Context->RSSetViewports(oldViewportCount, oldViewports);
		g_Context->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
		g_Context->OMSetDepthStencilState(oldDepthState, oldStencilRef);
		g_Context->RSSetState(oldRasterState);
		g_Context->IASetInputLayout(oldInputLayout);
		g_Context->IASetPrimitiveTopology(oldTopology);
		g_Context->IASetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
		g_Context->VSSetShader(oldVS, nullptr, 0);
		g_Context->PSSetShader(oldPS, nullptr, 0);

		if (oldRTV)
			oldRTV->Release();
		if (oldDSV)
			oldDSV->Release();
		if (oldBlendState)
			oldBlendState->Release();
		if (oldDepthState)
			oldDepthState->Release();
		if (oldRasterState)
			oldRasterState->Release();
		if (oldInputLayout)
			oldInputLayout->Release();
		if (oldVertexBuffer)
			oldVertexBuffer->Release();
		if (oldVS)
			oldVS->Release();
		if (oldPS)
			oldPS->Release();
	}

	void Cleanup()
	{
		{
			std::lock_guard lock(g_LineMutex);
			g_BuildLines.clear();
			g_RenderLines = std::make_shared<std::vector<Line>>();
		}

		g_Vertices.clear();
		ReleaseRenderTarget();

		if (g_VertexBuffer)
		{
			g_VertexBuffer->Release();
			g_VertexBuffer = nullptr;
		}

		if (g_BlendState)
		{
			g_BlendState->Release();
			g_BlendState = nullptr;
		}

		if (g_DepthState)
		{
			g_DepthState->Release();
			g_DepthState = nullptr;
		}

		if (g_RasterState)
		{
			g_RasterState->Release();
			g_RasterState = nullptr;
		}

		if (g_InputLayout)
		{
			g_InputLayout->Release();
			g_InputLayout = nullptr;
		}

		if (g_VertexShader)
		{
			g_VertexShader->Release();
			g_VertexShader = nullptr;
		}

		if (g_PixelShader)
		{
			g_PixelShader->Release();
			g_PixelShader = nullptr;
		}

		if (g_Context)
		{
			g_Context->Release();
			g_Context = nullptr;
		}

		if (g_Device)
		{
			g_Device->Release();
			g_Device = nullptr;
		}

		g_VertexBufferCapacity = 0;
		g_Initialized          = false;
	}
}