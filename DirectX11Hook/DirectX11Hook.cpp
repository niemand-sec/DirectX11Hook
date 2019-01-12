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

// Detours imports
#include "detours.h"

// DX11 imports
#include <d3d11.h>
#include <D3Dcompiler.h>
#pragma comment(lib, "D3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")


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
		if (wParam == VK_DELETE)
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
		std::cout << "current: " << &current << std::endl;
	}
	if ( (paramsModel == currentParams) && bShader)
	{
		//std::cout << "[+]SAME!1" << std::endl;
		pContext->PSSetShader(pShaderRed, NULL, NULL);
	}
	else if ( (paramsModel == currentParams) && bTexture)
	{
		//std::cout << "[+]SAME!2" << std::endl;
		for (int x1 = 0; x1 <= 10; x1++)
		{
			pContext->PSSetShaderResources(x1, 1, &textureView);
		}
		pContext->PSSetSamplers(0, 1, &pSamplerState);
	}
	//pContext->PSSetShader(pShaderRed, NULL, NULL);
	g_propertiesModels.unlock();
	//std::cout << Stride << " :: " << vedesc.ByteWidth << " :: " << indesc.ByteWidth << " :: " << pscdesc.ByteWidth << std::endl;
	return fnID3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
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


/*void detourDirectX(LPVOID original, PBYTE dst)
{
	std::cout << "[+] Calling DirectX Detour" << std::endl;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	// Detours the original fnIDXGISwapChainPresent with our Present
	DetourAttach(&(LPVOID&)original, (PBYTE)dst);
	DetourTransactionCommit();
}*/

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
	fnIDXGISwapChainPresent = (IDXGISwapChainPresent)((DWORD_PTR)hDxgi + 0x5070);
	std::cout << "[+] Present Addr: " << std::hex << fnIDXGISwapChainPresent << std::endl;
}

void printValues()
{
	std::cout << "[+] ID3D11DeviceContext Addr: " << std::hex << pContext << std::endl;
	std::cout << "[+] ID3D11Device Addr: " << std::hex << pDevice << std::endl;
	std::cout << "[+] ID3D11RenderTargetView Addr: " << std::hex << mainRenderTargetView << std::endl;
	std::cout << "[+] IDXGISwapChain Addr: " << std::hex << pSwapChain << std::endl;
}


int WINAPI main()
{
	ConsoleSetup();
	retrieveValues();
	// After this call, Present should be hooked and controlled by me.
	detourDirectXPresent();
	Sleep(4000);
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

