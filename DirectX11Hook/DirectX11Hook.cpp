// DirectX11Hook.cpp : Defines the exported functions for the DLL application.
//

// Standard imports
#include "stdafx.h"
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_set>
#include <mutex>

// Check windows
#if _WIN32 || _WIN64
#if _WIN64
#define ENV64BIT
#else
#define ENV32BIT
#endif
#endif

// Detours imports
#include "detours.h"

// DX11 imports
#include <d3d11.h>
#include <D3Dcompiler.h>
#pragma comment(lib, "D3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")
#define SAFE_RELEASE(p)      { if(p) { (p)->Release(); (p)=NULL; } }


//ImGUI imports
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// D3X HOOK DEFINITIONS
typedef HRESULT(__fastcall *IDXGISwapChainPresent)(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags);
typedef void(__stdcall *ID3D11DrawIndexed)(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
// Definition of WndProc Hook. Its here to avoid dragging dependencies on <windows.h> types.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


// Main D3D11 Objects
ID3D11DeviceContext *pContext = NULL;
ID3D11Device *pDevice = NULL;
ID3D11RenderTargetView *mainRenderTargetView;
static IDXGISwapChain*  pSwapChain = NULL;
static WNDPROC OriginalWndProcHandler = nullptr;
HWND window = nullptr;
IDXGISwapChainPresent fnIDXGISwapChainPresent;
DWORD_PTR* pDeviceContextVTable = NULL;
ID3D11DrawIndexed fnID3D11DrawIndexed;
UINT iIndexCount = 0;
UINT iStartIndexLocation;
INT iBaseVertexLocation;

// Boolean
BOOL g_bInitialised = false;
bool g_ShowMenu = false;
bool bDrawIndexed = true;
BOOL bModelLogging;
bool bCurrent;
bool g_PresentHooked = false;

//vertex
UINT veStartSlot;
UINT veNumBuffers;
ID3D11Buffer *veBuffer;
UINT Stride;
UINT veBufferOffset;
D3D11_BUFFER_DESC vedesc;

//index
ID3D11Buffer *inBuffer;
DXGI_FORMAT inFormat;
UINT        inOffset;
D3D11_BUFFER_DESC indesc;

//psgetConstantbuffers
UINT pscStartSlot;
UINT pscNumBuffers;
ID3D11Buffer *pscBuffer;
D3D11_BUFFER_DESC pscdesc;

//Textures
ID3D11Texture2D* textureRed = nullptr;
ID3D11ShaderResourceView* textureView;
ID3D11SamplerState *pSamplerState;
ID3D11PixelShader* pShaderRed = NULL;
bool bShader = 1;
bool bTexture = 0;

// Model Structures
struct propertiesModel
{
	UINT stride;
	UINT vedesc_ByteWidth;
	UINT indesc_ByteWidth;
	UINT pscdesc_ByteWidth;
};

//Z-Buffering variables
ID3D11DepthStencilState *m_DepthStencilState;
ID3D11DepthStencilState *m_origDepthStencilState;
UINT pStencilRef;
bool bWallhack = false;


// LightHack
bool bLighthack = false;

bool operator==(const propertiesModel& lhs, const propertiesModel& rhs)
{
	if (lhs.stride != rhs.stride
		|| lhs.vedesc_ByteWidth != rhs.vedesc_ByteWidth
		|| lhs.indesc_ByteWidth != rhs.indesc_ByteWidth
		|| lhs.pscdesc_ByteWidth != rhs.pscdesc_ByteWidth)
	{
		return false;
	}
	else
	{
		//std::cout << "true" << std::endl;
		return true;
	}
}


namespace std {
	template<> struct hash<propertiesModel>
	{
		std::size_t operator()(const propertiesModel& obj) const noexcept
		{
			std::size_t h1 = std::hash<int>{}(obj.stride);
			std::size_t h2 = std::hash<int>{}(obj.vedesc_ByteWidth);
			std::size_t h3 = std::hash<int>{}(obj.indesc_ByteWidth);
			std::size_t h4 = std::hash<int>{}(obj.pscdesc_ByteWidth);
			return (h1 ^ h3 + h4) ^ (h2 << 1);
		}
	};

}


struct propertiesModel	currentParams;
std::unordered_set<propertiesModel> seenParams;
std::unordered_set<propertiesModel> wallhackParams;
int currentParamPosition = 1;
std::mutex g_propertiesModels;



void ConsoleSetup()
{
	// With this trick we'll be able to print content to the console, and if we have luck we could get information printed by the game.
	AllocConsole();
	SetConsoleTitle("[+] Hooking DirectX 11 by Niemand");
	freopen("CONOUT$", "w", stdout);
	freopen("CONOUT$", "w", stderr);
	freopen("CONIN$", "r", stdin);
}


LRESULT CALLBACK hWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	ImGuiIO& io = ImGui::GetIO();
	POINT mPos;
	GetCursorPos(&mPos);
	ScreenToClient(window, &mPos);
	ImGui::GetIO().MousePos.x = mPos.x;
	ImGui::GetIO().MousePos.y = mPos.y;

	if (uMsg == WM_KEYUP)
	{
		if (wParam == VK_INSERT)
		{
			g_ShowMenu = !g_ShowMenu;
		}

	}

	if (g_ShowMenu)
	{
		ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
		return true;
	}

	return CallWindowProc(OriginalWndProcHandler, hWnd, uMsg, wParam, lParam);
}

// raiders posted this here - http://www.unknowncheats.me/forum/direct3d/65135-directx-10-generateshader.html
HRESULT GenerateShader(ID3D11Device* pD3DDevice, ID3D11PixelShader** pShader, float r, float g, float b)
{
	char szCast[] = "struct VS_OUT"
		"{"
		"    float4 Position   : SV_Position;"
		"    float4 Color    : COLOR0;"
		"};"

		"float4 main( VS_OUT input ) : SV_Target"
		"{"
		"    float4 fake;"
		"    fake.a = 1.0;"
		"    fake.r = %f;"
		"    fake.g = %f;"
		"    fake.b = %f;"
		"    return fake;"
		"}";
	ID3D10Blob*    pBlob;
	char szPixelShader[1000];

	sprintf(szPixelShader, szCast, r, g, b);

	HRESULT hr = D3DCompile(szPixelShader, sizeof(szPixelShader), "shader", NULL, NULL, "main", "ps_4_0", NULL, NULL, &pBlob, NULL);

	if (FAILED(hr))
		return hr;

	hr = pD3DDevice->CreatePixelShader((DWORD*)pBlob->GetBufferPointer(), pBlob->GetBufferSize(), NULL, pShader);

	if (FAILED(hr))
		return hr;

	return S_OK;
}

void __stdcall hookD3D11DrawIndexed(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	if (GetAsyncKeyState(VK_DELETE) & 1)
	{
		bDrawIndexed = true;
		std::cout << "[+] Model Log cleared" << std::endl;
		seenParams.clear();
	}

	//get stride & vedesc.ByteWidth
	pContext->IAGetVertexBuffers(0, 1, &veBuffer, &Stride, &veBufferOffset);
	if (veBuffer)
		veBuffer->GetDesc(&vedesc);
	if (veBuffer != NULL) { veBuffer->Release(); veBuffer = NULL; }

	//get indesc.ByteWidth
	pContext->IAGetIndexBuffer(&inBuffer, &inFormat, &inOffset);
	if (inBuffer)
		inBuffer->GetDesc(&indesc);
	if (inBuffer != NULL) { inBuffer->Release(); inBuffer = NULL; }

	//get pscdesc.ByteWidth
	pContext->PSGetConstantBuffers(pscStartSlot, 1, &pscBuffer);
	if (pscBuffer != NULL)
		pscBuffer->GetDesc(&pscdesc);
	if (pscBuffer != NULL) { pscBuffer->Release(); pscBuffer = NULL; }

	propertiesModel paramsModel;
	paramsModel.stride = Stride;
	paramsModel.vedesc_ByteWidth = vedesc.ByteWidth;
	paramsModel.indesc_ByteWidth = indesc.ByteWidth;
	paramsModel.pscdesc_ByteWidth = pscdesc.ByteWidth;
	g_propertiesModels.lock();
	seenParams.insert(paramsModel);

	// We need to restore this to avoid future problems
	if (bWallhack)
	{
		pContext->OMGetDepthStencilState(&m_origDepthStencilState, &pStencilRef);
	}
	
	
	if (bDrawIndexed)
	{
		std::cout << "[+] DrawIndexed Hooked succesfully" << std::endl;
		bDrawIndexed = false;
		// Set this for the first time its called
		currentParams = paramsModel;
		std::cout << std::dec << currentParams.stride << " :: "
			<< currentParams.vedesc_ByteWidth << " :: "
			<< currentParams.indesc_ByteWidth << " :: "
			<< currentParams.pscdesc_ByteWidth << std::endl;
	}

	auto current = seenParams.find(currentParams);

	if (GetAsyncKeyState(VK_F9) & 1)
	{
		bShader = !bShader;
		bTexture = !bTexture;
		if (bShader)
		{
			std::cout << "[+] Shader Mode Enabled" << std::endl;
		} 
		else
		{
			std::cout << "[+] Texture Mode Enabled" << std::endl;
		}
	}
	if (GetAsyncKeyState(VK_F10) & 1)
	{
		bWallhack = !bWallhack;
		if (bWallhack)
		{
			std::cout << "[+] Wallhack Enabled" << std::endl;
		}
		else
		{
			std::cout << "[+] Wallhack Disabled" << std::endl;
		}
	}
	if (GetAsyncKeyState(VK_PRIOR) & 1)
	{
		//FIX cannot dereference element, because end and begin return an iterator instead of an element
		if (current == seenParams.end())
		{
			std::cout << "Position " << std::dec << currentParamPosition << " of " << std::dec << seenParams.size() << std::endl;
			// TODO: I need a fix for this I get error "Cannot dereference end list iterator"
			current = seenParams.begin();
			currentParamPosition = 1;
		}
		else
		{
			current++;
			currentParamPosition++;
		}
		currentParams = *current;
	}
	if (GetAsyncKeyState(VK_NEXT) & 1)
	{
		if (current == seenParams.begin())
		{
			std::cout << "Position " << std::dec << currentParamPosition << " of " << std::dec << seenParams.size() << std::endl;
			// TODO: I need a fix for this I get error "Cannot dereference end list iterator"
			current = seenParams.end();
			currentParamPosition = seenParams.size();
		}
		else
		{
			current--;
			currentParamPosition--;
		}
		currentParams = *current;
	}
	
	if (GetAsyncKeyState(0x4C) & 1)
	{
		std::cout << std::dec << currentParams.stride << " :: "
			<< currentParams.vedesc_ByteWidth << " :: "
			<< currentParams.indesc_ByteWidth << " :: "
			<< currentParams.pscdesc_ByteWidth << std::endl;
		std::cout << "Position " << std::dec << currentParamPosition << " of " << std::dec << seenParams.size() << std::endl;
	}
	if ((paramsModel == currentParams || wallhackParams.find(paramsModel) != wallhackParams.end() )&& bShader)
	{
		//std::cout << "[+]SAME!1" << std::endl;
		pContext->PSSetShader(pShaderRed, NULL, NULL);
		if (bWallhack)
		{
			pContext->OMSetDepthStencilState(m_DepthStencilState, 0);
		}
	}
	else if ( (paramsModel == currentParams || wallhackParams.find(paramsModel) != wallhackParams.end()) && bTexture)
	{
		//std::cout << "[+]SAME!2" << std::endl;
		for (int x1 = 0; x1 <= 10; x1++)
		{
			pContext->PSSetShaderResources(x1, 1, &textureView);
		}
		pContext->PSSetSamplers(0, 1, &pSamplerState);
		if (bWallhack)
		{
			pContext->OMSetDepthStencilState(m_DepthStencilState, 0);
		}
	}
	//pContext->PSSetShader(pShaderRed, NULL, NULL);
	g_propertiesModels.unlock();
	//std::cout << Stride << " :: " << vedesc.ByteWidth << " :: " << indesc.ByteWidth << " :: " << pscdesc.ByteWidth << std::endl;
	fnID3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
	if (bWallhack)
	{
		pContext->OMSetDepthStencilState(m_origDepthStencilState, pStencilRef);
		SAFE_RELEASE(m_origDepthStencilState);
	}	
}

HRESULT GetDeviceAndCtxFromSwapchain(IDXGISwapChain *pSwapChain, ID3D11Device **ppDevice, ID3D11DeviceContext **ppContext)
{
	HRESULT ret = pSwapChain->GetDevice(__uuidof(ID3D11Device), (PVOID*)ppDevice);

	if (SUCCEEDED(ret))
		(*ppDevice)->GetImmediateContext(ppContext);

	return ret;
}

HRESULT __fastcall Present(IDXGISwapChain *pChain, UINT SyncInterval, UINT Flags)
{
	if (!g_bInitialised) {
		g_PresentHooked = true;
		std::cout << "\t[+] Present Hook called by first time" << std::endl;
		if (FAILED(GetDeviceAndCtxFromSwapchain(pChain, &pDevice, &pContext)))
			return fnIDXGISwapChainPresent(pChain, SyncInterval, Flags);
		pSwapChain = pChain;
		DXGI_SWAP_CHAIN_DESC sd;
		pChain->GetDesc(&sd);
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		window = sd.OutputWindow;

		//Set OriginalWndProcHandler to the Address of the Original WndProc function
		OriginalWndProcHandler = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)hWndProc);

		// Creating red texture https://docs.microsoft.com/en-us/windows/desktop/direct3d11/overviews-direct3d-11-resources-textures-how-to
		static const uint32_t color = 0xff0000ff;
		D3D11_SUBRESOURCE_DATA initData = { &color, sizeof(uint32_t), 0 };
		D3D11_TEXTURE2D_DESC desc;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		memset(&desc, 0, sizeof(desc));
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr;
		hr = pDevice->CreateTexture2D(&desc, &initData, &textureRed);

		GenerateShader(pDevice, &pShaderRed, 1.0f, 0.0f, 0.0f);


		// Disabling Z-Buffering
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
		depthStencilDesc.DepthEnable = TRUE;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depthStencilDesc.StencilEnable = FALSE;
		depthStencilDesc.StencilReadMask = 0xFF;
		depthStencilDesc.StencilWriteMask = 0xFF;

		// Stencil operations if pixel is front-facing
		depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
		depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

		// Stencil operations if pixel is back-facing
		depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
		depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

		
		pDevice->CreateDepthStencilState(&depthStencilDesc, &m_DepthStencilState);
		


		if (SUCCEEDED(hr) && textureRed != 0)
		{
			
			D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
			memset(&SRVDesc, 0, sizeof(SRVDesc));
			SRVDesc.Format = format;
			SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			SRVDesc.Texture2D.MipLevels = 1;

			hr = pDevice->CreateShaderResourceView(textureRed, &SRVDesc, &textureView);
			if (FAILED(hr))
			{
				textureRed->Release();
				return hr;
			}
			
		}


		ImGui_ImplWin32_Init(window);
		ImGui_ImplDX11_Init(pDevice, pContext);
		ImGui::GetIO().ImeWindowHandle = window;

		ID3D11Texture2D* pBackBuffer;

		pChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
		pBackBuffer->Release();

		g_bInitialised = true;
	}
	/*
	if (bLighthack)
	{
		pDevice->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRBG(100, 100, 100));
	}
	*/
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplDX11_NewFrame();

	ImGui::NewFrame();
	//Menu is displayed when g_ShowMenu is TRUE
	if (g_ShowMenu)
	{
		bool bShow = true;
		ImGui::ShowDemoWindow(&bShow);
	}
	ImGui::EndFrame();

	ImGui::Render();

	pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	return fnIDXGISwapChainPresent(pChain, SyncInterval, Flags);
}


void detourDirectXPresent()
{
	std::cout << "[+] Calling fnIDXGISwapChainPresent Detour" << std::endl;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	// Detours the original fnIDXGISwapChainPresent with our Present
	DetourAttach(&(LPVOID&)fnIDXGISwapChainPresent, (PBYTE)Present);
	DetourTransactionCommit();
}

void detourDirectXDrawIndexed()
{
	std::cout << "[+] Calling fnID3D11DrawIndexed Detour" << std::endl;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	// Detours the original fnIDXGISwapChainPresent with our Present fnID3D11DrawIndexed, (PBYTE)hookD3D11DrawIndexed
	DetourAttach(&(LPVOID&)fnID3D11DrawIndexed, (PBYTE)hookD3D11DrawIndexed);
	DetourTransactionCommit();
}

void retrieveValues()
{
	DWORD_PTR hDxgi = (DWORD_PTR)GetModuleHandle("dxgi.dll");
	#if defined(ENV64BIT)
		fnIDXGISwapChainPresent = (IDXGISwapChainPresent)((DWORD_PTR)hDxgi + 0x5070);
	#elif defined (ENV32BIT)
		fnIDXGISwapChainPresent = (IDXGISwapChainPresent)((DWORD_PTR)hDxgi + 0x10230);
	#endif
	std::cout << "[+] Present Addr: " << std::hex << fnIDXGISwapChainPresent << std::endl;
}

void printValues()
{
	std::cout << "[+] ID3D11DeviceContext Addr: " << std::hex << pContext << std::endl;
	std::cout << "[+] ID3D11Device Addr: " << std::hex << pDevice << std::endl;
	std::cout << "[+] ID3D11RenderTargetView Addr: " << std::hex << mainRenderTargetView << std::endl;
	std::cout << "[+] IDXGISwapChain Addr: " << std::hex << pSwapChain << std::endl;
}

void setupWallhack() {
	propertiesModel wallhackParamsItem;
	wallhackParamsItem.stride = 8;
	wallhackParamsItem.vedesc_ByteWidth = 16552;
	wallhackParamsItem.indesc_ByteWidth = 10164;
	wallhackParamsItem.pscdesc_ByteWidth = 832;
	wallhackParams.insert(wallhackParamsItem);
}

LRESULT CALLBACK DXGIMsgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProc(hwnd, uMsg, wParam, lParam); }

void GetPresent()
{
	WNDCLASSEXA wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DXGIMsgProc, 0L, 0L, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "DX", NULL };
	RegisterClassExA(&wc);
	HWND hWnd = CreateWindowA("DX", NULL, WS_OVERLAPPEDWINDOW, 100, 100, 300, 300, NULL, NULL, wc.hInstance, NULL);

	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 1;
	sd.BufferDesc.Width = 2;
	sd.BufferDesc.Height = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	D3D_FEATURE_LEVEL FeatureLevelsRequested = D3D_FEATURE_LEVEL_11_0;
	UINT numFeatureLevelsRequested = 1;
	D3D_FEATURE_LEVEL FeatureLevelsSupported;
	HRESULT hr;
	IDXGISwapChain *swapchain = 0;
	ID3D11Device *dev = 0;
	ID3D11DeviceContext *devcon = 0;
	if (FAILED(hr = D3D11CreateDeviceAndSwapChain(NULL,
		D3D_DRIVER_TYPE_HARDWARE,
		NULL,
		0,
		&FeatureLevelsRequested,
		numFeatureLevelsRequested,
		D3D11_SDK_VERSION,
		&sd,
		&swapchain,
		&dev,
		&FeatureLevelsSupported,
		&devcon)))
	{
		std::cout << "[-] Failed to hook Present with VT method." << std::endl;
		return;		
	}
	DWORD_PTR* pSwapChainVtable = NULL;
	pSwapChainVtable = (DWORD_PTR*)swapchain;
	pSwapChainVtable = (DWORD_PTR*)pSwapChainVtable[0];
	fnIDXGISwapChainPresent = (IDXGISwapChainPresent)(DWORD_PTR)pSwapChainVtable[8];
	g_PresentHooked = true;
	std::cout << "[+] Present Addr:" << fnIDXGISwapChainPresent << std::endl;
	Sleep(2000);
}

int WINAPI main()
{
	ConsoleSetup();

	GetPresent();

	// If GetPresent failed we have this backup method to get Present Address
	if (!g_PresentHooked) {
		retrieveValues();
	}
	
	setupWallhack();

	// After this call, Present should be hooked and controlled by me.
	detourDirectXPresent();
	while (!g_bInitialised) {
		Sleep(1000);
	}
	
	printValues();

	std::cout << "[+] pDeviceContextVTable0 Addr: " << std::hex << pContext << std::endl;
	pDeviceContextVTable = (DWORD_PTR*)pContext;
	std::cout << "[+] pDeviceContextVTable1 Addr: " << std::hex << pDeviceContextVTable << std::endl;
	pDeviceContextVTable = (DWORD_PTR*)pDeviceContextVTable[0];
	std::cout << "[+] pDeviceContextVTable2 Addr: " << std::hex << pDeviceContextVTable << std::endl;
	//fnID3D11DrawIndexed
	fnID3D11DrawIndexed = (ID3D11DrawIndexed)pDeviceContextVTable[12];

	std::cout << "[+] pDeviceContextVTable Addr: " << std::hex << pDeviceContextVTable << std::endl;
	std::cout << "[+] fnID3D11DrawIndexed Addr: " << std::hex << fnID3D11DrawIndexed << std::endl;
	detourDirectXDrawIndexed();
	Sleep(4000);
	
}



BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);
		CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)main, NULL, NULL, NULL);
	}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

