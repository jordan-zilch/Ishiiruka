// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/Common.h"
#include "Common/Atomic.h"
#include "Common/Thread.h"
#include "Common/LogManager.h"

#if defined(HAVE_WX) && HAVE_WX
#include "DolphinWX/VideoConfigDiag.h"
#include "DolphinWX/Debugger/DebuggerPanel.h"
#endif // HAVE_WX


#include "VideoCommon/MainBase.h"
#include "main.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "TextureCache.h"
#include "VideoCommon/BPStructs.h"
#include "VertexManager.h"
#include "FramebufferManager.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VertexShaderCache.h"
#include "PixelShaderCache.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "D3DTexture.h"
#include "D3DUtil.h"
#include "VideoCommon/EmuWindow.h"
#include "VideoCommon/VideoState.h"
#include "Render.h"
#include "VideoCommon/DLCache.h"
#include "VideoCommon/IndexGenerator.h"
#include "Common/IniFile.h"
#include "Core/Core.h"
#include "Core/Host.h"

#include "Core/ConfigManager.h"
#include "VideoBackend.h"
#include "PerfQuery.h"

namespace DX9
{

unsigned int VideoBackend::PeekMessages()
{
	MSG msg;
	while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return FALSE;
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return TRUE;
}

void VideoBackend::UpdateFPSDisplay(const char *text)
{
	TCHAR temp[512];
	swprintf_s(temp, sizeof(temp)/sizeof(TCHAR), _T("%hs | DX9 | %hs"), scm_rev_str, text);
	EmuWindow::SetWindowText(temp);
}

std::string VideoBackend::GetName()
{
	return "DX9";
}

std::string VideoBackend::GetDisplayName()
{
	return "Direct3D9";
}

void InitBackendInfo()
{
	DX9::D3D::Init();
	D3DCAPS9 device_caps = DX9::D3D::GetCaps();
	const int shaderModel = ((device_caps.PixelShaderVersion >> 8) & 0xFF);
	const int maxConstants = (shaderModel < 3) ? 32 : ((shaderModel < 4) ? 224 : 65536);
	g_Config.backend_info.APIType = shaderModel < 3 ? API_D3D9_SM20 : API_D3D9_SM30;
	g_Config.backend_info.bUseRGBATextures = false;
	g_Config.backend_info.bSupportsRGBATextures = D3D::CheckTextureSupport(0, D3DFMT_A8B8G8R8);
	g_Config.backend_info.bUseMinimalMipCount = true;
	g_Config.backend_info.bSupports3DVision = true;
	g_Config.backend_info.bSupportsPrimitiveRestart = false; // D3D9 does not support primitive restart
	g_Config.backend_info.bSupportsSeparateAlphaFunction = (device_caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) == D3DPMISCCAPS_SEPARATEALPHABLEND;
	// Dual source blend disabled by default until a proper method to test for support is found	
	g_Config.backend_info.bSupports3DVision = true;
	OSVERSIONINFO info;
	ZeroMemory(&info, sizeof(OSVERSIONINFO));
	info.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	if (GetVersionEx(&info))
	{
		// dual source blending is only supported in windows 7 o newer. sorry xp users
		// we cannot test for device caps because most drivers just declare the minimun caps
		// and don't expose their support for some functionalities              
		g_Config.backend_info.bSupportsDualSourceBlend = g_Config.backend_info.bSupportsSeparateAlphaFunction && (info.dwPlatformId == VER_PLATFORM_WIN32_NT) && ((info.dwMajorVersion > 6) || ((info.dwMajorVersion == 6) && info.dwMinorVersion >= 1));
	}
	else
	{
		g_Config.backend_info.bSupportsDualSourceBlend = false;
	}
	g_Config.backend_info.bSupportsFormatReinterpretation = true;
	g_Config.backend_info.bSupportsPixelLighting = C_PLIGHTS + 40 <= maxConstants && C_PMATERIALS + 4 <= maxConstants;
	g_Config.backend_info.bSupportsEarlyZ = true;
	g_Config.backend_info.bNeedBlendIndices = true;

	// adapters
	g_Config.backend_info.Adapters.clear();
	for (int i = 0; i < DX9::D3D::GetNumAdapters(); ++i)
		g_Config.backend_info.Adapters.push_back(DX9::D3D::GetAdapter(i).ident.Description);

	// aamodes
	g_Config.backend_info.AAModes.clear();
	if (g_Config.iAdapter < DX9::D3D::GetNumAdapters())
	{
		const DX9::D3D::Adapter &adapter = DX9::D3D::GetAdapter(g_Config.iAdapter);

		for (int i = 0; i < (int)adapter.aa_levels.size(); ++i)
			g_Config.backend_info.AAModes.push_back(adapter.aa_levels[i].name);
	}

	// Clear ppshaders string vector
	g_Config.backend_info.PPShaders.clear();

	DX9::D3D::Shutdown();
}

void VideoBackend::ShowConfig(void* parent)
{
#if defined(HAVE_WX) && HAVE_WX
	InitBackendInfo();
	VideoConfigDiag diag((wxWindow*)parent, _trans("Direct3D9"), "gfx_dx9");
	diag.ShowModal();
#endif
}

bool VideoBackend::Initialize(void *&window_handle)
{
	InitializeShared();
	InitBackendInfo();

	frameCount = 0;

	g_Config.Load((File::GetUserPath(D_CONFIG_IDX) + "gfx_dx9.ini").c_str());
	g_Config.GameIniLoad(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strGameIniDefault.c_str(),
	                     SConfig::GetInstance().m_LocalCoreStartupParameter.m_strGameIniLocal.c_str());
	g_Config.UpdateProjectionHack();
	g_Config.VerifyValidity();
	// as only some driver/hardware configurations support dual source blending only enable it if is 
	// configured by user
	g_Config.backend_info.bSupportsDualSourceBlend &= g_Config.bForceDualSourceBlend;
	UpdateActiveConfig();

	window_handle = (void*)EmuWindow::Create((HWND)window_handle, GetModuleHandle(0), _T("Loading - Please wait."));
	if (window_handle == NULL)
	{
		ERROR_LOG(VIDEO, "An error has occurred while trying to create the window.");
		return false;
	}
	else if (FAILED(DX9::D3D::Init()))
	{
		MessageBox(GetActiveWindow(), _T("Unable to initialize Direct3D. Please make sure that you have the latest version of DirectX 9.0c correctly installed."), _T("Fatal Error"), MB_ICONERROR|MB_OK);
		return false;
	}

	s_BackendInitialized = true;

	return true;
}

void VideoBackend::Video_Prepare()
{
	// Better be safe...
	s_efbAccessRequested = FALSE;
	s_FifoShuttingDown = FALSE;
	s_swapRequested = FALSE;

	// internal interfaces
	g_vertex_manager = new VertexManager;
	g_perf_query = new PerfQuery;
	g_renderer = new Renderer;
	g_texture_cache = new TextureCache;	
	// VideoCommon
	BPInit();
	Fifo_Init();
	IndexGenerator::Init();
	VertexLoaderManager::Init();
	OpcodeDecoder_Init();
	VertexShaderManager::Init();
	PixelShaderManager::Init();
	CommandProcessor::Init();
	PixelEngine::Init();
	DLCache::Init();	
	// Notify the core that the video backend is ready
	Host_Message(WM_USER_CREATE);
}

void VideoBackend::Shutdown()
{
	s_BackendInitialized = false;

	// TODO: should be in Video_Cleanup
	if (g_renderer)
	{
		s_efbAccessRequested = FALSE;
		s_FifoShuttingDown = FALSE;
		s_swapRequested = FALSE;

		// VideoCommon
		DLCache::Shutdown();
		Fifo_Shutdown();
		CommandProcessor::Shutdown();
		PixelShaderManager::Shutdown();
		VertexShaderManager::Shutdown();
		OpcodeDecoder_Shutdown();
		VertexLoaderManager::Shutdown();

		// internal interfaces
		PixelShaderCache::Shutdown();
		VertexShaderCache::Shutdown();
		delete g_texture_cache;
		delete g_renderer;
		delete g_perf_query;
		delete g_vertex_manager;
		g_renderer = NULL;
		g_texture_cache = NULL;
	}
	D3D::Shutdown();
}

void VideoBackend::Video_Cleanup() {
}

}
