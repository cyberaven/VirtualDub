//	VirtualDub - Video processing and capture application
//	A/V interface library
//	Copyright (C) 1998-2005 Avery Lee
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program; if not, write to the Free Software
//	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
//
//	This file is the DirectX 9 driver for the video display subsystem.
//	It does traditional point sampled and bilinearly filtered upsampling
//	as well as a special multipass algorithm for emulated bicubic
//	filtering.
//

#include <vd2/system/vdtypes.h>

#define DIRECTDRAW_VERSION 0x0900
#define INITGUID
#include <d3d9.h>
#include <vd2/system/vdtypes.h>
#include <vd2/system/vdalloc.h>
#include <vd2/system/refcount.h>
#include <vd2/system/math.h>
#include <vd2/system/time.h>
#include <vd2/system/vdstl.h>
#include <vd2/system/w32assist.h>
#include <vd2/Riza/direct3d.h>

///////////////////////////////////////////////////////////////////////////

#define VDDEBUG_D3D VDDEBUG

using namespace nsVDD3D9;

///////////////////////////////////////////////////////////////////////////

class VDD3D9Texture : public IVDD3D9Texture, public vdlist_node {
public:
	VDD3D9Texture();
	~VDD3D9Texture();

	int AddRef();
	int Release();

	bool Init(VDD3D9Manager *pManager, IVDD3D9TextureGenerator *pGenerator);
	void Shutdown();

	const char *GetName() const { return mName.data(); }
	void SetName(const char *name) { mName.assign(name, name+strlen(name)+1); }

	int GetWidth();
	int GetHeight();

	void SetD3DTexture(IDirect3DTexture9 *pTexture);
	IDirect3DTexture9 *GetD3DTexture();

protected:
	vdfastvector<char> mName;
	IDirect3DTexture9 *mpD3DTexture;
	IVDD3D9TextureGenerator *mpGenerator;
	int mWidth;
	int mHeight;
	int mRefCount;
};

VDD3D9Texture::VDD3D9Texture()
	: mpD3DTexture(NULL)
	, mpGenerator(NULL)
	, mWidth(0)
	, mHeight(0)
	, mRefCount(0)
{
	mListNodeNext = mListNodePrev = this;
}

VDD3D9Texture::~VDD3D9Texture() {
}

int VDD3D9Texture::AddRef() {
	return ++mRefCount;
}

int VDD3D9Texture::Release() {
	int rc = --mRefCount;

	if (!rc) {
		vdlist_base::unlink(*this);
		Shutdown();
		delete this;
	}

	return rc;
}

bool VDD3D9Texture::Init(VDD3D9Manager *pManager, IVDD3D9TextureGenerator *pGenerator) {
	if (mpGenerator)
		mpGenerator->Release();
	if (pGenerator)
		pGenerator->AddRef();
	mpGenerator = pGenerator;

	if (mpGenerator) {
		if (!mpGenerator->GenerateTexture(pManager, this))
			return false;
	}

	return true;
}

void VDD3D9Texture::Shutdown() {
	if (mpGenerator) {
		mpGenerator->Release();
		mpGenerator = NULL;
	}

	if (mpD3DTexture) {
		mpD3DTexture->Release();
		mpD3DTexture = NULL;
	}
}

int VDD3D9Texture::GetWidth() {
	return mWidth;
}

int VDD3D9Texture::GetHeight() {
	return mHeight;
}

void VDD3D9Texture::SetD3DTexture(IDirect3DTexture9 *pTexture) {
	if (mpD3DTexture)
		mpD3DTexture->Release();
	if (pTexture) {
		pTexture->AddRef();

		D3DSURFACE_DESC desc;
		HRESULT hr = pTexture->GetLevelDesc(0, &desc);

		if (SUCCEEDED(hr)) {
			mWidth = desc.Width;
			mHeight = desc.Height;
		} else {
			mWidth = 1;
			mHeight = 1;
		}
	}
	mpD3DTexture = pTexture;
}

IDirect3DTexture9 *VDD3D9Texture::GetD3DTexture() {
	return mpD3DTexture;
}

///////////////////////////////////////////////////////////////////////////

static VDD3D9Manager g_VDDirect3D9;

VDD3D9Manager *VDInitDirect3D9(VDD3D9Client *pClient) {
	return g_VDDirect3D9.Attach(pClient) ? &g_VDDirect3D9 : NULL;
}

void VDDeinitDirect3D9(VDD3D9Manager *p, VDD3D9Client *pClient) {
	VDASSERT(p == &g_VDDirect3D9);

	p->Detach(pClient);
}

VDD3D9Manager::VDD3D9Manager()
	: mhmodDX9(NULL)
	, mpD3D(NULL)
	, mpD3DDevice(NULL)
	, mpD3DRTMain(NULL)
	, mhwndDevice(NULL)
	, mbDeviceValid(false)
	, mbInScene(false)
	, mpD3DVB(NULL)
	, mpD3DIB(NULL)
	, mpD3DQuery(NULL)
	, mpD3DVD(NULL)
	, mRefCount(0)
{
}

ATOM VDD3D9Manager::sDevWndClass;

VDD3D9Manager::~VDD3D9Manager() {
	VDASSERT(!mRefCount);
}

bool VDD3D9Manager::Attach(VDD3D9Client *pClient) {
	bool bSuccess = false;

	mClients.push_back(pClient);

	if (++mRefCount == 1)
		bSuccess = Init();
	else
		bSuccess = CheckDevice();

	if (!bSuccess)
		Detach(pClient);

	return bSuccess;
}

void VDD3D9Manager::Detach(VDD3D9Client *pClient) {
	VDASSERT(mRefCount > 0);

	mClients.erase(mClients.fast_find(pClient));

	if (!--mRefCount)
		Shutdown();
}

bool VDD3D9Manager::Init() {
	HINSTANCE hInst = VDGetLocalModuleHandleW32();

	if (!sDevWndClass) {
		WNDCLASS wc;

		wc.cbClsExtra		= 0;
		wc.cbWndExtra		= 0;
		wc.hbrBackground	= NULL;
		wc.hCursor			= NULL;
		wc.hIcon			= NULL;
		wc.hInstance		= hInst;
		wc.lpfnWndProc		= DefWindowProc;
		wc.lpszClassName	= "RizaD3DDeviceWindow";
		wc.lpszMenuName		= NULL;
		wc.style			= 0;

		sDevWndClass = RegisterClass(&wc);
		if (!sDevWndClass)
			return false;
	}

	mhwndDevice = CreateWindow((LPCTSTR)sDevWndClass, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
	if (!mhwndDevice) {
		Shutdown();
		return false;
	}

	// attempt to load D3D9.DLL
	mhmodDX9 = LoadLibrary("d3d9.dll");
	if (!mhmodDX9) {
		Shutdown();
		return false;
	}

	IDirect3D9 *(APIENTRY *pDirect3DCreate9)(UINT) = (IDirect3D9 *(APIENTRY *)(UINT))GetProcAddress(mhmodDX9, "Direct3DCreate9");
	if (!pDirect3DCreate9) {
		Shutdown();
		return false;
	}

	// create Direct3D9 object
	mpD3D = pDirect3DCreate9(D3D_SDK_VERSION);
	if (!mpD3D) {
		Shutdown();
		return false;
	}

	// create device
	memset(&mPresentParms, 0, sizeof mPresentParms);
	mPresentParms.Windowed			= TRUE;
	mPresentParms.SwapEffect		= D3DSWAPEFFECT_COPY;
	// BackBufferFormat is set below.
	mPresentParms.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
//	mPresentParms.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

#if 1
	mPresentParms.BackBufferWidth	= GetSystemMetrics(SM_CXMAXIMIZED);
	mPresentParms.BackBufferHeight	= GetSystemMetrics(SM_CYMAXIMIZED);
#else
	mPresentParms.BackBufferWidth	= 1600;
	mPresentParms.BackBufferHeight	= 1200;
	mPresentParms.BackBufferCount	= 3;
#endif

	HRESULT hr;

	// Look for the NVPerfHUD 2.0 driver
	
	const UINT adapters = mpD3D->GetAdapterCount();
	DWORD dwFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_NOWINDOWCHANGES;
	UINT adapter = D3DADAPTER_DEFAULT;
	D3DDEVTYPE type = D3DDEVTYPE_HAL;

	for(UINT n=0; n<adapters; ++n) {
		D3DADAPTER_IDENTIFIER9 ident;

		if (SUCCEEDED(mpD3D->GetAdapterIdentifier(n, 0, &ident))) {
			if (!strcmp(ident.Description, "NVIDIA NVPerfHUD")) {
				adapter = n;
				type = D3DDEVTYPE_REF;

				// trim the size a bit so we can see the controls
				mPresentParms.BackBufferWidth -= 256;
				mPresentParms.BackBufferHeight -= 192;
				break;
			}
		}
	}

	mAdapter = adapter;
	mDevType = type;

	D3DDISPLAYMODE mode;
	hr = mpD3D->GetAdapterDisplayMode(adapter, &mode);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Failed to get current adapter mode.\n");
		Shutdown();
		return false;
	}

	// Make sure we have at least X8R8G8B8 for a texture format
	hr = mpD3D->CheckDeviceFormat(adapter, type, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Device does not support X8R8G8B8 textures.\n");
		Shutdown();
		return false;
	}

	// Make sure we have at least X8R8G8B8 or A8R8G8B8 for a backbuffer format
	mPresentParms.BackBufferFormat	= D3DFMT_X8R8G8B8;
	hr = mpD3D->CheckDeviceFormat(adapter, type, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_X8R8G8B8);
	if (FAILED(hr)) {
		mPresentParms.BackBufferFormat	= D3DFMT_A8R8G8B8;
		hr = mpD3D->CheckDeviceFormat(adapter, type, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8);

		if (FAILED(hr)) {
			VDDEBUG_D3D("VideoDisplay/DX9: Device does not support X8R8G8B8 or A8R8G8B8 render targets.\n");
			Shutdown();
			return false;
		}
	}

	// Check if at least vertex shader 1.1 is supported; if not, force SWVP.
	hr = mpD3D->GetDeviceCaps(adapter, type, &mDevCaps);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Couldn't retrieve device caps.\n");
		Shutdown();
		return false;
	}

	if (mDevCaps.VertexShaderVersion >= D3DVS_VERSION(1, 1))
		dwFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
	else
		dwFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;

	// Create the device.
	hr = mpD3D->CreateDevice(adapter, type, mhwndDevice, dwFlags, &mPresentParms, &mpD3DDevice);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Failed to create device.\n");
		Shutdown();
		return false;
	}

	mbDeviceValid = true;

	// retrieve device caps
	memset(&mDevCaps, 0, sizeof mDevCaps);
	hr = mpD3DDevice->GetDeviceCaps(&mDevCaps);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Failed to retrieve device caps.\n");
		Shutdown();
		return false;
	}

	// Check for Virge/Rage Pro/Riva128
	if (Is3DCardLame()) {
		Shutdown();
		return false;
	}

	const D3DVERTEXELEMENT9 kVertexDecl[]={
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
		D3DDECL_END()
	};

	hr = mpD3DDevice->CreateVertexDeclaration(kVertexDecl, &mpD3DVD);
	if (FAILED(hr)) {
		Shutdown();
		return false;
	}

	if (!InitVRAMResources()) {
		Shutdown();
		return false;
	}
	return true;
}

bool VDD3D9Manager::InitVRAMResources() {
	// retrieve display mode
	HRESULT hr = mpD3D->GetAdapterDisplayMode(mAdapter, &mDisplayMode);
	if (FAILED(hr)) {
		VDDEBUG_D3D("VideoDisplay/DX9: Failed to get current adapter mode.\n");
		Shutdown();
		return false;
	}

	// retrieve back buffer
	if (!mpD3DRTMain) {
		hr = mpD3DDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &mpD3DRTMain);
		if (FAILED(hr)) {
			ShutdownVRAMResources();
			return false;
		}
	}

	// create vertex buffer
	if (!mpD3DVB) {
		hr = mpD3DDevice->CreateVertexBuffer(kVertexBufferSize * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2, D3DPOOL_DEFAULT, &mpD3DVB, NULL);
		if (FAILED(hr)) {
			VDDEBUG_D3D("VideoDisplay/DX9: Failed to create vertex buffer.\n");
			ShutdownVRAMResources();
			return false;
		}
		mVertexBufferPt = 0;
	}

	// create index buffer
	if (!mpD3DIB) {
		hr = mpD3DDevice->CreateIndexBuffer(kIndexBufferSize * sizeof(uint16), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &mpD3DIB, NULL);
		if (FAILED(hr)) {
			VDDEBUG_D3D("VideoDisplay/DX9: Failed to create index buffer.\n");
			ShutdownVRAMResources();
			return false;
		}
		mIndexBufferPt = 0;
	}

	// create flush event
	hr = mpD3DDevice->CreateQuery(D3DQUERYTYPE_EVENT, NULL);
	if (SUCCEEDED(hr)) {
		hr = mpD3DDevice->CreateQuery(D3DQUERYTYPE_EVENT, &mpD3DQuery);
	}

	return true;
}

void VDD3D9Manager::ShutdownVRAMResources() {
	if (mpD3DQuery) {
		mpD3DQuery->Release();
		mpD3DQuery = NULL;
	}

	if (mpD3DRTMain) {
		mpD3DRTMain->Release();
		mpD3DRTMain = NULL;
	}

	if (mpD3DIB) {
		mpD3DIB->Release();
		mpD3DIB = NULL;
	}

	if (mpD3DVB) {
		mpD3DVB->Release();
		mpD3DVB = NULL;
	}
}

void VDD3D9Manager::Shutdown() {
	mbDeviceValid = false;

	ShutdownVRAMResources();

	while(!mSharedTextures.empty()) {
		VDD3D9Texture *pTexture = mSharedTextures.back();
		mSharedTextures.pop_back();

		pTexture->mListNodePrev = pTexture->mListNodeNext = pTexture;
		pTexture->Release();
	}

	if (mpD3DVD) {
		mpD3DVD->Release();
		mpD3DVD = NULL;
	}

	if (mpD3DDevice) {
		mpD3DDevice->Release();
		mpD3DDevice = NULL;
	}

	if (mpD3D) {
		mpD3D->Release();
		mpD3D = NULL;
	}

	if (mhmodDX9) {
		FreeLibrary(mhmodDX9);
		mhmodDX9 = NULL;
	}

	if (mhwndDevice) {
		DestroyWindow(mhwndDevice);
		mhwndDevice = NULL;
	}
}

bool VDD3D9Manager::Reset() {
	for(vdlist<VDD3D9Client>::iterator it(mClients.begin()), itEnd(mClients.end()); it!=itEnd; ++it) {
		VDD3D9Client& client = **it;

		client.OnPreDeviceReset();
	}

	ShutdownVRAMResources();

	HRESULT hr = mpD3DDevice->Reset(&mPresentParms);
	if (FAILED(hr)) {
		mbDeviceValid = false;
		return false;
	}

	mbInScene = false;
	if (!InitVRAMResources())
		return false;

	mbDeviceValid = true;

	for(vdlist<VDD3D9Client>::iterator it(mClients.begin()), itEnd(mClients.end()); it!=itEnd; ++it) {
		VDD3D9Client& client = **it;

		client.OnPostDeviceReset();
	}

	return true;
}

bool VDD3D9Manager::CheckDevice() {
	if (!mpD3DDevice)
		return false;

	if (!mbDeviceValid) {
		HRESULT hr = mpD3DDevice->TestCooperativeLevel();

		if (FAILED(hr)) {
			if (hr != D3DERR_DEVICENOTRESET)
				return false;

			if (!Reset())
				return false;
		}
	}

	return InitVRAMResources();
}

void VDD3D9Manager::AdjustTextureSize(int& texw, int& texh) {
	// use original texture size if device has no restrictions
	if (!(mDevCaps.TextureCaps & (D3DPTEXTURECAPS_NONPOW2CONDITIONAL | D3DPTEXTURECAPS_POW2)))
		return;

	// make power of two
	texw += texw - 1;
	texh += texh - 1;

	while(int tmp = texw & (texw-1))
		texw = tmp;
	while(int tmp = texh & (texh-1))
		texh = tmp;

	// enforce aspect ratio
	if (mDevCaps.MaxTextureAspectRatio) {
		while(texw * (int)mDevCaps.MaxTextureAspectRatio < texh)
			texw += texw;
		while(texh * (int)mDevCaps.MaxTextureAspectRatio < texw)
			texh += texh;
	}
}

bool VDD3D9Manager::IsTextureFormatAvailable(D3DFORMAT format) {
	HRESULT hr = mpD3D->CheckDeviceFormat(mAdapter, mDevType, mDisplayMode.Format, 0, D3DRTYPE_TEXTURE, format);

	return SUCCEEDED(hr);
}

void VDD3D9Manager::ClearRenderTarget(IDirect3DTexture9 *pTexture) {
	IDirect3DSurface9 *pRTSurface;
	if (FAILED(pTexture->GetSurfaceLevel(0, &pRTSurface)))
		return;
	HRESULT hr = mpD3DDevice->SetRenderTarget(0, pRTSurface);
	pRTSurface->Release();

	if (FAILED(hr))
		return;

	if (SUCCEEDED(mpD3DDevice->BeginScene())) {
		mpD3DDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0, 0.f, 0);
		mpD3DDevice->EndScene();
	}
}

void VDD3D9Manager::ResetBuffers() {
	mVertexBufferPt = 0;
	mIndexBufferPt = 0;
}

Vertex *VDD3D9Manager::LockVertices(unsigned vertices) {
	VDASSERT(vertices <= kVertexBufferSize);
	if (mVertexBufferPt + vertices > kVertexBufferSize) {
		mVertexBufferPt = 0;
	}

	mVertexBufferLockSize = vertices;

	void *p;
	HRESULT hr;
	for(;;) {
		hr = mpD3DVB->Lock(mVertexBufferPt * sizeof(Vertex), mVertexBufferLockSize * sizeof(Vertex), &p, mVertexBufferPt ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
		if (hr != D3DERR_WASSTILLDRAWING)
			break;
		Sleep(1);
	}
	if (FAILED(hr)) {
		VDASSERT(false);
		return NULL;
	}

	return (Vertex *)p;
}

void VDD3D9Manager::UnlockVertices() {
	mVertexBufferPt += mVertexBufferLockSize;

	VDVERIFY(SUCCEEDED(mpD3DVB->Unlock()));
}

uint16 *VDD3D9Manager::LockIndices(unsigned indices) {
	VDASSERT(indices <= kIndexBufferSize);
	if (mIndexBufferPt + indices > kIndexBufferSize) {
		mIndexBufferPt = 0;
	}

	mIndexBufferLockSize = indices;

	void *p;
	HRESULT hr;
	for(;;) {
		hr = mpD3DIB->Lock(mIndexBufferPt * sizeof(uint16), mIndexBufferLockSize * sizeof(uint16), &p, mIndexBufferPt ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
		if (hr != D3DERR_WASSTILLDRAWING)
			break;
		Sleep(1);
	}
	if (FAILED(hr)) {
		VDASSERT(false);
		return NULL;
	}

	return (uint16 *)p;
}

void VDD3D9Manager::UnlockIndices() {
	mIndexBufferPt += mIndexBufferLockSize;

	VDVERIFY(SUCCEEDED(mpD3DIB->Unlock()));
}

bool VDD3D9Manager::BeginScene() {
	if (!mbInScene) {
		HRESULT hr = mpD3DDevice->BeginScene();

		if (FAILED(hr)) {
			VDDEBUG_D3D("VideoDisplay/DX9: BeginScene() failed! hr = %08x\n", hr);
			return false;
		}

		mbInScene = true;
	}

	return true;
}

bool VDD3D9Manager::EndScene() {
	if (mbInScene) {
		mbInScene = false;
		HRESULT hr = mpD3DDevice->EndScene();

		if (FAILED(hr)) {
			VDDEBUG_D3D("VideoDisplay/DX9: EndScene() failed! hr = %08x\n", hr);
			return false;
		}
	}

	return true;
}

HRESULT VDD3D9Manager::DrawArrays(D3DPRIMITIVETYPE type, UINT vertStart, UINT primCount) {
	HRESULT hr = mpD3DDevice->DrawPrimitive(type, mVertexBufferPt - mVertexBufferLockSize + vertStart, primCount);

	VDASSERT(SUCCEEDED(hr));

	return hr;
}

HRESULT VDD3D9Manager::DrawElements(D3DPRIMITIVETYPE type, UINT vertStart, UINT vertCount, UINT idxStart, UINT primCount) {
	// The documentation for IDirect3DDevice9::DrawIndexedPrimitive() was probably
	// written under a hallucinogenic state.

	HRESULT hr = mpD3DDevice->DrawIndexedPrimitive(type, mVertexBufferPt - mVertexBufferLockSize + vertStart, 0, vertCount, mIndexBufferPt - mIndexBufferLockSize + idxStart, primCount);

	VDASSERT(SUCCEEDED(hr));

	return hr;
}

HRESULT VDD3D9Manager::Present(const RECT *src, HWND hwndDest, bool vsync, float& syncdelta, VDD3DPresentHistory& history) {
	if (!mPresentParms.Windowed)
		return S_OK;

	HRESULT hr;

	if (vsync && (mDevCaps.Caps & D3DCAPS_READ_SCANLINE)) {
		if (mpD3DQuery) {
			hr = mpD3DQuery->Issue(D3DISSUE_END);
			if (SUCCEEDED(hr)) {
				while(S_FALSE == mpD3DQuery->GetData(NULL, 0, D3DGETDATA_FLUSH))
					::Sleep(1);
			}
		}

		RECT r;
		if (GetWindowRect(hwndDest, &r)) {
			int top = 0;
			int bottom = GetSystemMetrics(SM_CYSCREEN);

			// GetMonitorInfo() requires Windows 98. We might never fail on this because
			// I think DirectX 9.0c requires 98+, but we have to dynamically link anyway
			// to avoid a startup link failure on 95.
			typedef BOOL (APIENTRY *tpGetMonitorInfo)(HMONITOR mon, LPMONITORINFO lpmi);
			static tpGetMonitorInfo spGetMonitorInfo = (tpGetMonitorInfo)GetProcAddress(GetModuleHandle("user32"), "GetMonitorInfo");

			if (spGetMonitorInfo) {
				HMONITOR hmon = mpD3D->GetAdapterMonitor(mAdapter);
				MONITORINFO monInfo = {sizeof(MONITORINFO)};
				if (spGetMonitorInfo(hmon, &monInfo)) {
					top = monInfo.rcMonitor.top;
					bottom = monInfo.rcMonitor.bottom;
				}
			}

			if (r.top < top)
				r.top = top;
			if (r.bottom > bottom)
				r.bottom = bottom;

			top += VDRoundToInt(history.mPresentDelay);

			r.top -= top;
			r.bottom -= top;

			// Poll raster status, and wait until we can safely blit. We assume that the
			// blit can outrace the beam. 
			D3DRASTER_STATUS rastStatus;
			UINT maxScanline = 0;
			int firstScan = -1;
			while(SUCCEEDED(mpD3DDevice->GetRasterStatus(0, &rastStatus))) {
				if (firstScan < 0)
					firstScan = rastStatus.InVBlank ? 0 : (int)rastStatus.ScanLine;

				if (rastStatus.InVBlank) {
					if (history.mVBlankSuccess >= 0.5f)
						break;

					rastStatus.ScanLine = 0;
				}

				// Check if we have wrapped around without seeing the VBlank. If this
				// occurs, force an exit. This prevents us from potentially burning a lot
				// of CPU time if the CPU becomes busy and can't poll the beam in a timely
				// manner.
				if (rastStatus.ScanLine < maxScanline)
					break;

				// Check if we're outside of the danger zone.
				if ((int)rastStatus.ScanLine < r.top || (int)rastStatus.ScanLine >= r.bottom)
					break;

				// We're in the danger zone. If the delta is greater than one tenth of the
				// display, do a sleep.
				if ((r.bottom - (int)rastStatus.ScanLine) * 10 >= (int)mDisplayMode.Height)
					::Sleep(1);

				maxScanline = rastStatus.ScanLine;
			}

			syncdelta = (float)(firstScan - r.bottom) / (float)(int)mDisplayMode.Height;
			syncdelta -= floorf(syncdelta);
			if (syncdelta > 0.5f)
				syncdelta -= 1.0f;

			hr = mpD3DDevice->Present(src, NULL, hwndDest, NULL);
			if (FAILED(hr))
				return hr;

			D3DRASTER_STATUS rastStatus2;
			hr = mpD3DDevice->GetRasterStatus(0, &rastStatus2);
			if (SUCCEEDED(hr)) {
				if (rastStatus.InVBlank) {
					float success = rastStatus2.InVBlank || (int)rastStatus2.ScanLine <= r.top || (int)rastStatus2.ScanLine >= r.bottom ? 1.0f : 0.0f;

					history.mVBlankSuccess += (success - history.mVBlankSuccess) * 0.01f;
				}

				if (!rastStatus.InVBlank && !rastStatus2.InVBlank && rastStatus2.ScanLine > rastStatus.ScanLine) {
					float delta = (float)(int)(rastStatus2.ScanLine - rastStatus.ScanLine);

					history.mPresentDelay += (delta - history.mPresentDelay) * 0.01f;
				}
			}
		}
	} else {
		syncdelta = 0.0f;

		hr = mpD3DDevice->Present(src, NULL, hwndDest, NULL);
	}

	return hr;
}

HRESULT VDD3D9Manager::PresentFullScreen() {
	if (mPresentParms.Windowed)
		return S_OK;

	HRESULT hr;
	IDirect3DSwapChain9 *pSwapChain;
	hr = mpD3DDevice->GetSwapChain(0, &pSwapChain);
	if (FAILED(hr))
		return hr;
	
	for(;;) {
		hr = pSwapChain->Present(NULL, NULL, NULL, NULL, D3DPRESENT_DONOTWAIT);

		if (SUCCEEDED(hr) || hr != D3DERR_WASSTILLDRAWING)
			break;

		::Sleep(1);
	}

	pSwapChain->Release();

	return hr;
}

#define REQUIRE(x, reason) if (!(x)) { VDDEBUG_D3D("VideoDisplay/DX9: 3D device is lame -- reason: " reason "\n"); return true; } else ((void)0)
#define REQUIRECAPS(capsflag, bits, reason) REQUIRE(!(~mDevCaps.capsflag & (bits)), reason)

bool VDD3D9Manager::Is3DCardLame() {
	REQUIRE(mDevCaps.DeviceType != D3DDEVTYPE_SW, "software device detected");
	REQUIRECAPS(PrimitiveMiscCaps, D3DPMISCCAPS_CULLNONE, "primitive misc caps check failed");
	REQUIRECAPS(RasterCaps, D3DPRASTERCAPS_DITHER, "raster caps check failed");
	REQUIRECAPS(TextureCaps, D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP, "texture caps failed");
	REQUIRE(!(mDevCaps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY), "device requires square textures");
	REQUIRECAPS(TextureFilterCaps, D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR
								| D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR
								| D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR, "texture filtering modes insufficient");
	REQUIRECAPS(TextureAddressCaps, D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_WRAP, "texture addressing modes insufficient");
	REQUIRE(mDevCaps.MaxTextureBlendStages>0 && mDevCaps.MaxSimultaneousTextures>0, "not enough texture stages");
	return false;
}

bool VDD3D9Manager::CreateSharedTexture(const char *name, SharedTextureFactory factory, IVDD3D9Texture **ppTexture) {
	SharedTextures::iterator it(mSharedTextures.begin()), itEnd(mSharedTextures.end());
	for(; it!=itEnd; ++it) {
		VDD3D9Texture& texture = **it;

		if (!strcmp(texture.GetName(), name)) {
			*ppTexture = &texture;
			texture.AddRef();
			return true;
		}
	}

	vdrefptr<IVDD3D9TextureGenerator> pGenerator;
	if (factory) {
		if (!factory(~pGenerator))
			return false;
	}

	vdrefptr<VDD3D9Texture> pTexture(new_nothrow VDD3D9Texture);
	if (!pTexture)
		return false;

	pTexture->SetName(name);

	if (!pTexture->Init(this, pGenerator))
		return false;

	mSharedTextures.push_back(pTexture);

	*ppTexture = pTexture.release();
	return true;
}
