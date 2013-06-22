//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2001 Avery Lee
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

#include "stdafx.h"

#include <windows.h>

#include <map>

#include "vdserver.h"

#include "AudioSource.h"
#include "VideoSource.h"
#include <vd2/system/error.h>
#include <vd2/system/file.h>
#include <vd2/system/filesys.h>
#include <vd2/Dita/services.h>
#include "FrameSubset.h"

#include "filters.h"
#include "dub.h"
#include "DubUtils.h"
#include "gui.h"
#include "audio.h"
#include "command.h"
#include "prefs.h"

#include "server.h"
#include "resource.h"

extern HINSTANCE g_hInst;
extern HWND g_hWnd;

extern wchar_t g_szInputAVIFile[MAX_PATH];

// VideoSource.cpp

extern void DIBconvert(void *src, BITMAPINFOHEADER *srcfmt, void *dst, BITMAPINFOHEADER *dstfmt);

///////////////////////////////////////////////////////////////////////////

enum {
	kFileDialog_Signpost		= 'sign'
};

//////////////////////////////////////////////////////////////////////////

class FrameserverSession {
private:
	HANDLE hArena;

public:
	FrameserverSession *next, *prev;

	char *arena;
	long arena_size;
	DWORD id;

	FrameserverSession();
	DWORD Init(LONG arena_size, DWORD session_id);
	~FrameserverSession();
};

FrameserverSession::FrameserverSession() {
	next = prev = NULL;
	hArena = INVALID_HANDLE_VALUE;
}

DWORD FrameserverSession::Init(LONG arena_size, DWORD session_id) {
	char buf[16];

	wsprintf(buf, "VDUBF%08lx", session_id);

	if (INVALID_HANDLE_VALUE == (hArena = OpenFileMapping(FILE_MAP_WRITE, FALSE, buf)))
		return NULL;

	if (!(arena = (char *)MapViewOfFile(hArena, FILE_MAP_WRITE, 0, 0, arena_size)))
		return NULL;

	this->id = (DWORD)this;
	this->arena_size = arena_size;

	return this->id;
}

FrameserverSession::~FrameserverSession() {
	if (arena) UnmapViewOfFile(arena);
	if (hArena != INVALID_HANDLE_VALUE) CloseHandle(hArena);
}

///////////////////////////////////

class Frameserver {
private:
	DubOptions			*opt;
	HWND				hwnd;
	AudioSource			*aSrc;
	VideoSource			*vSrc;

	DubAudioStreamInfo	aInfo;
	DubVideoStreamInfo	vInfo;
	FrameSubset			audioset;
	long				lVideoSamples;
	long				lAudioSamples;
	FilterStateInfo		fsi;
	VDRenderFrameMap	mVideoFrameMap;

	DWORD_PTR			dwUserSave;
	DWORD_PTR			dwProcSave;

	long			lRequestCount, lFrameCount, lAudioSegCount;

	HWND			hwndStatus;

	std::vector<char>	mInputBuffer;

	typedef std::map<uint32, FrameserverSession *> tSessions;
	tSessions	mSessions;

	char *lpszFsname;

public:
	Frameserver(VideoSource *video, AudioSource *audio, HWND hwndParent, DubOptions *xopt);
	~Frameserver();

	void Go(IVDubServerLink *ivdsl, char *name);

	static LONG APIENTRY WndProc( HWND hWnd, UINT message, UINT wParam, LONG lParam);
	LONG APIENTRY WndProc2( HWND hWnd, UINT message, UINT wParam, LONG lParam);

	static BOOL APIENTRY StatusDlgProc( HWND hWnd, UINT message, UINT wParam, LONG lParam);
	BOOL APIENTRY StatusDlgProc2( HWND hWnd, UINT message, UINT wParam, LONG lParam);

	FrameserverSession *SessionLookup(LPARAM lParam);
	LRESULT SessionOpen(LPARAM mmapID, WPARAM arena_len);
	LRESULT SessionClose(LPARAM lParam);
	LRESULT SessionStreamInfo(LPARAM lParam, WPARAM stream);
	LRESULT SessionFormat(LPARAM lParam, WPARAM stream);
	LRESULT SessionFrame(LPARAM lParam, WPARAM sample);
	LRESULT SessionAudio(LPARAM lParam, WPARAM lStart);
	LRESULT SessionAudioInfo(LPARAM lParam, WPARAM lStart);
};

Frameserver::Frameserver(VideoSource *video, AudioSource *audio, HWND hwndParent, DubOptions *xopt) {
	opt				= xopt;
	hwnd			= hwndParent;

	aSrc			= audio;
	vSrc			= video;

	lFrameCount = lRequestCount = lAudioSegCount = 0;

	InitStreamValuesStatic(vInfo, aInfo, video, audio, opt);

	mVideoFrameMap.Init(video, vInfo.start_src, vInfo.frameRateIn / vInfo.frameRate, inputSubset, vInfo.end_dst, false);

	long lOffsetStart = video->msToSamples(opt->video.lStartOffsetMS);
	long lOffsetEnd = video->msToSamples(opt->video.lEndOffsetMS);

	FrameSubset			videoset(*inputSubset);

	if (opt->audio.fEndAudio)
		videoset.deleteRange(videoset.getTotalFrames() - lOffsetEnd, videoset.getTotalFrames());

	if (opt->audio.fStartAudio)
		videoset.deleteRange(0, lOffsetStart);

	VDDEBUG("Video subset:\n");
	videoset.dump();

	if (audio)
		AudioTranslateVideoSubset(audioset, videoset, vInfo.frameRateIn, audio->getWaveFormat());

	VDDEBUG("Audio subset:\n");
	audioset.dump();

	if (audio) {
		audioset.offset(audio->msToSamples(-opt->audio.offset));
		lAudioSamples = audioset.getTotalFrames();
	} else
		lAudioSamples = 0;

	lVideoSamples = mVideoFrameMap.size();
}

Frameserver::~Frameserver() {
	{
		for(tSessions::iterator it(mSessions.begin()), itEnd(mSessions.end()); it!=itEnd; ++it) {
			FrameserverSession *pSession = (*it).second;

			delete pSession;
		}

		mSessions.clear();
	}

	filters.DeinitFilters();
	filters.DeallocateBuffers();
}

void Frameserver::Go(IVDubServerLink *ivdsl, char *name) {
	int server_index;

	lpszFsname = name;
	
	// prepare the sources...

	if (vSrc) {
		if (!vSrc->setDecompressedFormat(16+8*g_dubOpts.video.inputDepth))
			if (!vSrc->setDecompressedFormat(32))
				if (!vSrc->setDecompressedFormat(24))
					if (!vSrc->setDecompressedFormat(16))
						if (!vSrc->setDecompressedFormat(8))
							throw MyError("The decompression codec cannot decompress to an RGB format. This is very unusual. Check that any \"Force YUY2\" options are not enabled in the codec's properties.");

		vSrc->streamBegin(true);

		BITMAPINFOHEADER *bmih = vSrc->getDecompressedFormat();

		filters.initLinearChain(&g_listFA, (Pixel *)(bmih+1), bmih->biWidth, bmih->biHeight, 32 /*bmih->biBitCount*/, 16+8*opt->video.outputDepth);

		if (filters.getFrameLag())
			MessageBox(g_hWnd,
			"One or more filters in the filter chain has a non-zero lag. This will cause the served "
			"video to lag behind the audio!"
			, "VirtualDub warning", MB_OK);

		fsi.lMicrosecsPerFrame		= vInfo.usPerFrame;
		fsi.lMicrosecsPerSrcFrame	= vInfo.usPerFrameIn;

		if (filters.ReadyFilters(&fsi))
			throw MyError("Error readying filters.");
	}

	if (aSrc)
		aSrc->streamBegin(true);

	// usurp the window

	dwUserSave = GetWindowLongPtr(hwnd, GWL_USERDATA);
	dwProcSave = GetWindowLongPtr(hwnd, GWL_WNDPROC );
	SetWindowLongPtr(hwnd, GWL_USERDATA, (DWORD)this);
	SetWindowLongPtr(hwnd, GWL_WNDPROC	, (DWORD)Frameserver::WndProc);
	guiSetTitle(hwnd, IDS_TITLE_FRAMESERVER);

	// create dialog box

	if (hwndStatus = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_SERVER), hwnd, Frameserver::StatusDlgProc, (LPARAM)this)) {

		// hide the main window

		ShowWindow(hwnd, SW_HIDE);

		// create the frameserver

		server_index = ivdsl->CreateFrameServer(name, hwnd);

		if (server_index>=0) {

			// kick us into high priority

			SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

			// enter window loop

			{
				MSG msg;

				while(GetMessage(&msg, NULL, 0, 0)) {
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			}

			// return to normal priority

			SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

			ivdsl->DestroyFrameServer(server_index);
		}

		if (IsWindow(hwndStatus)) DestroyWindow(hwndStatus);

		// show the main window

		ShowWindow(hwnd, SW_SHOW);
	}

	// restore everything

	SetWindowLongPtr(hwnd, GWL_WNDPROC	, dwProcSave);
	SetWindowLongPtr(hwnd, GWL_USERDATA, dwUserSave);

	if (vSrc) {
		vSrc->streamEnd();
	}

	if (server_index<0) throw MyError("Couldn't create frameserver");
}

LONG APIENTRY Frameserver::WndProc( HWND hWnd, UINT message, UINT wParam, LONG lParam) {
	return ((Frameserver *)GetWindowLong(hWnd, GWL_USERDATA))->WndProc2(hWnd, message, wParam, lParam);
}

LONG APIENTRY Frameserver::WndProc2( HWND hWnd, UINT message, UINT wParam, LONG lParam)
{
    switch (message) {
	case WM_SIZE:
		guiRedoWindows(hWnd);
		break;

	case WM_CLOSE:
	case WM_DESTROY:                  // message: window being destroyed
		PostQuitMessage(1);
		break;

	case VDSRVM_BIGGEST:
		{
			long size=sizeof(AVISTREAMINFO);

			if (vSrc) {
				if (size < sizeof(BITMAPINFOHEADER))
					size = sizeof(BITMAPINFOHEADER);

				if (size < filters.OutputBitmap()->size)
					size = filters.OutputBitmap()->size;
			}

			if (aSrc) {
				if (size < aSrc->getWaveFormat()->nAvgBytesPerSec)
					size = aSrc->getWaveFormat()->nAvgBytesPerSec;

				if (aSrc->getFormatLen()>size)
					size=aSrc->getFormatLen();
			}

			if (size < 65536) size=65536;

			_RPT1(0,"VDSRVM_BIGGEST: allocate a frame of size %ld bytes\n", size);
			return size;
		}

	case VDSRVM_OPEN:
		++lRequestCount;
		_RPT2(0,"VDSRVM_OPEN(arena size %ld, mmap ID %08lx)\n", wParam, lParam);
		return SessionOpen(lParam, wParam);

	case VDSRVM_CLOSE:
		++lRequestCount;
		_RPT1(0,"[session %08lx] VDSRVM_CLOSE()\n", lParam);
		return SessionClose(lParam);

	case VDSRVM_REQ_STREAMINFO:
		++lRequestCount;
		_RPT2(0,"[session %08lx] VDSRVM_REQ_STREAMINFO(stream %d)\n", lParam, wParam);
		return SessionStreamInfo(lParam, wParam);

	case VDSRVM_REQ_FORMAT:
		++lRequestCount;
		_RPT2(0,"[session %08lx] VDSRVM_REQ_FORMAT(stream %d)\n", lParam, wParam);
		return SessionFormat(lParam, wParam);

	case VDSRVM_REQ_FRAME:
		++lFrameCount;
		_RPT2(0,"[session %08lx] VDSRVM_REQ_FRAME(sample %ld)\n", lParam, wParam);
		return SessionFrame(lParam, wParam);

	case VDSRVM_REQ_AUDIO:
		++lAudioSegCount;
		return SessionAudio(lParam, wParam);

	case VDSRVM_REQ_AUDIOINFO:
		++lAudioSegCount;
		_RPT2(0,"[session %08lx] VDSRVM_REQ_AUDIOINFO(sample %ld)\n", lParam, wParam);
		return SessionAudioInfo(lParam, wParam);

	default:
		return (DefWindowProc(hWnd, message, wParam, lParam));
    }
    return (0);
}

///////////////////////

BOOL APIENTRY Frameserver::StatusDlgProc( HWND hWnd, UINT message, UINT wParam, LONG lParam) {
	return ((Frameserver *)GetWindowLong(hWnd, DWL_USER))->StatusDlgProc2(hWnd, message, wParam, lParam);
}

BOOL APIENTRY Frameserver::StatusDlgProc2( HWND hWnd, UINT message, UINT wParam, LONG lParam)
{
    switch (message) {
	case WM_INITDIALOG:
		SetWindowLong(hWnd, DWL_USER, lParam);
		SetDlgItemText(hWnd, IDC_STATIC_FSNAME, ((Frameserver *)lParam)->lpszFsname);
		SetTimer(hWnd,1,1000,NULL);

		{
			HKEY hkey;
			HIC hic;
			BOOL fAVIFile = FALSE, fVCM = FALSE;

			if (RegOpenKeyEx(HKEY_CLASSES_ROOT, "CLSID\\{894288E0-0948-11D2-8109-004845000EB5}\\InProcServer32\\AVIFile", 0, KEY_QUERY_VALUE, &hkey)==ERROR_SUCCESS) {
				RegCloseKey(hkey);
				fAVIFile = TRUE;
			}

			if (hic = ICOpen('CDIV', 'TSDV', ICMODE_DECOMPRESS)) {
				ICClose(hic);
				fVCM = TRUE;
			}

			if (fAVIFile && fVCM)
				SetDlgItemText(hWnd, IDC_STATIC_FCINSTALLED, "AVIFile and VCM");
			else if (fAVIFile)
				SetDlgItemText(hWnd, IDC_STATIC_FCINSTALLED, "AVIFile only");
			else if (fVCM)
				SetDlgItemText(hWnd, IDC_STATIC_FCINSTALLED, "VCM only");
		}
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) != IDOK) break;
	case WM_CLOSE:
		PostQuitMessage(1);
		return TRUE;
	case WM_TIMER:
		SetDlgItemInt(hWnd, IDC_STATIC_REQCOUNT, lRequestCount, FALSE);
		SetDlgItemInt(hWnd, IDC_STATIC_FRAMECNT, lFrameCount, FALSE);
		SetDlgItemInt(hWnd, IDC_STATIC_AUDIOSEGS, lAudioSegCount, FALSE);
		return TRUE;
    }
    return FALSE;
}

////////////////////////////////////////////////////////

FrameserverSession *Frameserver::SessionLookup(LPARAM lParam) {
	tSessions::const_iterator it(mSessions.find(lParam));

	if (it != mSessions.end())
		return (*it).second;

	_RPT1(0,"Session lookup failed on %08lx\n", lParam);

	return NULL;
}

LRESULT Frameserver::SessionOpen(LPARAM mmapID, WPARAM arena_len) {
	FrameserverSession *fs;
	DWORD id;

	if (fs = new FrameserverSession()) {
		if (id = fs->Init(arena_len, mmapID)) {
			mSessions[id] = fs;
			return id;
		}
		delete fs;
	}

	return NULL;
}

LRESULT Frameserver::SessionClose(LPARAM lParam) {
	FrameserverSession *fs = SessionLookup(lParam);

	if (!fs) return VDSRVERR_BADSESSION;

	mSessions.erase(lParam);

	return VDSRVERR_OK;
}

LRESULT Frameserver::SessionStreamInfo(LPARAM lParam, WPARAM stream) {
	FrameserverSession *fs = SessionLookup(lParam);

	if (!fs) return VDSRVERR_BADSESSION;

	if (stream<0 || stream>2) return VDSRVERR_NOSTREAM;

	if (stream==0) {
		AVISTREAMINFO *lpasi = (AVISTREAMINFO *)(fs->arena+8);

		if (!vSrc) return VDSRVERR_NOSTREAM;

		*(long *)(fs->arena+0) = 0;										//vSrc->lSampleFirst;
		*(long *)(fs->arena+4) = lVideoSamples;			//vSrc->lSampleLast;
		memcpy(lpasi, &vSrc->getStreamInfo(), sizeof(AVISTREAMINFO));

		lpasi->fccHandler	= ' BID';
		lpasi->dwLength		= *(long *)(fs->arena+4);
		lpasi->dwRate		= vInfo.frameRate.getHi();
		lpasi->dwScale		= vInfo.frameRate.getLo();

		SetRect(&lpasi->rcFrame, 0, 0, filters.OutputBitmap()->w, filters.OutputBitmap()->h);

		lpasi->dwSuggestedBufferSize = filters.OutputBitmap()->size;

	} else {
		if (!aSrc) return VDSRVERR_NOSTREAM;

		*(long *)(fs->arena+0) = 0;
		*(long *)(fs->arena+4) = lAudioSamples;
		memcpy(fs->arena+8, &aSrc->getStreamInfo(), sizeof(AVISTREAMINFO));

		((AVISTREAMINFO *)(fs->arena+8))->dwLength = audioset.getTotalFrames();
	}

	return VDSRVERR_OK;
}

LRESULT Frameserver::SessionFormat(LPARAM lParam, WPARAM stream) {
	FrameserverSession *fs = SessionLookup(lParam);
	DubSource *ds;
	long len;

	if (!fs) return VDSRVERR_BADSESSION;

	if (stream<0 || stream>2) return VDSRVERR_NOSTREAM;

	ds = stream ? (DubSource *)aSrc : (DubSource *)vSrc;

	if (!ds) return VDSRVERR_NOSTREAM;

	if (stream) {
		len = aSrc->getFormatLen();

		if (len > fs->arena_size) return VDSRVERR_TOOBIG;

		memcpy(fs->arena, aSrc->getFormat(), len);
	} else {
		BITMAPINFOHEADER *bmih;

		len = sizeof(BITMAPINFOHEADER);
		if (len > fs->arena_size) return VDSRVERR_TOOBIG;

		memcpy(fs->arena, vSrc->getDecompressedFormat(), len);

		bmih = (BITMAPINFOHEADER *)fs->arena;
//		bmih->biSize		= sizeof(BITMAPINFOHEADER);
		bmih->biWidth		= filters.LastBitmap()->w;
		bmih->biHeight		= filters.LastBitmap()->h;
		bmih->biPlanes		= 1;
		bmih->biCompression	= BI_RGB;
		bmih->biBitCount	= 24;
		bmih->biSizeImage	= ((bmih->biWidth*3+3)&-4)*bmih->biHeight;
		bmih->biClrUsed		= 0;
		bmih->biClrImportant= 0;
	}

	return len;
}

LRESULT Frameserver::SessionFrame(LPARAM lParam, WPARAM sample) {
	FrameserverSession *fs = SessionLookup(lParam);
	long original_frame = sample;

	if (!fs)
		return VDSRVERR_BADSESSION;

	try {
		void *ptr = vSrc->getFrameBuffer();
		BITMAPINFOHEADER *bmih = vSrc->getDecompressedFormat();
		BITMAPINFOHEADER bmih24;
		long frame;
		BOOL is_preroll;

		if (fs->arena_size < ((filters.LastBitmap()->w*3+3)&-4)*filters.LastBitmap()->h)
			return VDSRVERR_TOOBIG;

		sample = mVideoFrameMap.DisplayFrame(sample);

		if (sample < 0)
			return VDSRVERR_FAILED;

		vSrc->streamSetDesiredFrame(sample);

		frame = vSrc->streamGetNextRequiredFrame(&is_preroll);

		if (frame >= 0) {
			do {
				LONG lSize;
				int hr;

	//			_RPT1(0,"feeding frame %ld\n", frame);

				hr = vSrc->read(frame, 1, NULL, 0x7FFFFFFF, &lSize, NULL);
				if (hr)
					return VDSRVERR_FAILED;

				mInputBuffer.reserve((lSize + 65535) & ~65535);
				mInputBuffer.resize(lSize);

				hr = vSrc->read(frame, 1, &mInputBuffer[0], lSize, &lSize, NULL); 
				if (hr)
					return VDSRVERR_FAILED;

				ptr = vSrc->streamGetFrame(&mInputBuffer[0], lSize, vSrc->isKey(frame), is_preroll, frame);
			} while(-1 != (frame = vSrc->streamGetNextRequiredFrame(&is_preroll)));

		} else
			ptr = vSrc->streamGetFrame(NULL, 0, vSrc->isKey(sample), FALSE, vSrc->displayToStreamOrder(sample));

		if (!g_listFA.IsEmpty()) {
			VBitmap vbm = *filters.OutputBitmap();

			filters.InputBitmap()->BitBlt(0, 0, &VBitmap(vSrc->getFrameBuffer(), vSrc->getDecompressedFormat()), 0, 0, -1, -1);

			fsi.lCurrentFrame				= original_frame;
			fsi.lCurrentSourceFrame			= sample;
			fsi.lSourceFrameMS				= MulDiv(fsi.lCurrentSourceFrame, fsi.lMicrosecsPerSrcFrame, 1000);
			fsi.lDestFrameMS				= MulDiv(fsi.lCurrentFrame, fsi.lMicrosecsPerFrame, 1000);

			filters.RunFilters();

			vbm.data = (Pixel *)fs->arena;
			vbm.BitBlt(0, 0, filters.LastBitmap(), 0, 0, -1, -1);

		} else
			if (bmih->biBitCount != 24) {
				memcpy(&bmih24, bmih, sizeof(BITMAPINFOHEADER));
				bmih24.biBitCount = 24;
				DIBconvert(ptr, bmih, fs->arena, &bmih24);
			} else
				memcpy(fs->arena, ptr, bmih->biSizeImage);

	} catch(const MyError&) {
		return VDSRVERR_FAILED;
	}

	return VDSRVERR_OK;
}

LRESULT Frameserver::SessionAudio(LPARAM lParam, WPARAM lStart) {
	FrameserverSession *fs = SessionLookup(lParam);
	if (!fs) return VDSRVERR_BADSESSION;

	LONG lCount = *(LONG *)fs->arena;
	LONG cbBuffer = *(LONG *)(fs->arena+4);

	if (cbBuffer > fs->arena_size - 8) cbBuffer = fs->arena_size - 8;

	_RPT4(0,"[session %08lx] VDSRVM_REQ_AUDIO(sample %ld, count %d, cbBuffer %ld)\n", lParam, lCount, lStart, cbBuffer);

	// Do not return an error on an attempt to read beyond the end of
	// the audio stream -- this causes Panasonic to error.

	if (lStart >= lAudioSamples) {
		memset(fs->arena, 0, 8);
		return VDSRVERR_OK;
	}

	if (lStart+lCount > lAudioSamples)
		lCount = lAudioSamples;

	// Read subsets.

	long lTotalBytes = 0, lTotalSamples = 0;
	long lActualBytes, lActualSamples = 1;
	char *pDest = (char *)(fs->arena + 8);

	try {
		while(lCount>0 && lActualSamples>0) {
			int start, len;

			// Translate range.

			start = audioset.lookupRange(lStart, len);

			if (len > lCount)
				len = lCount;

			if (start < aSrc->getStart()) {
				start = aSrc->getStart();
				len = 1;
			}

			// Attempt read;

			switch(aSrc->read(start, len, pDest, cbBuffer, &lActualBytes, &lActualSamples)) {
			case AVIERR_OK:
				break;
			case AVIERR_BUFFERTOOSMALL:
				if (!lTotalSamples)
					return VDSRVERR_TOOBIG;
				goto out_of_space;
			default:
				return VDSRVERR_FAILED;
			}

			lCount -= lActualSamples;
			lStart += lActualSamples;
			cbBuffer -= lActualBytes;
			pDest += lActualBytes;
			lTotalSamples += lActualSamples;
			lTotalBytes += lActualBytes;
		}
out_of_space:
		;

	} catch(const MyError&) {
		return VDSRVERR_FAILED;
	}

	*(LONG *)(fs->arena + 0) = lTotalBytes;
	*(LONG *)(fs->arena + 4) = lTotalSamples;

	return VDSRVERR_OK;
}

LRESULT Frameserver::SessionAudioInfo(LPARAM lParam, WPARAM lStart) {
	FrameserverSession *fs = SessionLookup(lParam);
	if (!fs) return VDSRVERR_BADSESSION;

	LONG lCount = *(LONG *)fs->arena;
	LONG cbBuffer = *(LONG *)(fs->arena+4);

	if (lStart < 0)
		return VDSRVERR_FAILED;

	if (lStart + lCount > lAudioSamples)
		lCount = lAudioSamples - lStart;

	if (lCount < 0)
		lCount = 0;

	*(LONG *)(fs->arena + 0) = aSrc->getWaveFormat()->nBlockAlign * lCount;
	*(LONG *)(fs->arena + 4) = lCount;

	return VDSRVERR_OK;
}

//////////////////////////////////////////////////////////////////////////

extern vdrefptr<AudioSource> inputAudio;
extern vdrefptr<VideoSource> inputVideoAVI;

static HMODULE hmodServer;
static IVDubServerLink *ivdsl;

static BOOL InitServerDLL() {
	hmodServer = LoadLibrary("vdsvrlnk.dll");

	_RPT1(0,"VDSVRLNK handle: %p\n", hmodServer);

	if (hmodServer) {
		FARPROC fp;

		if (!(fp = GetProcAddress(hmodServer, "GetDubServerInterface")))
			return FALSE;

		ivdsl = ((IVDubServerLink *(*)(void))fp)();

		return TRUE;
	}

	return FALSE;
}

BOOL CALLBACK FrameServerSetupDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch(msg) {
	case WM_INITDIALOG:
		{
			char buf[32];

			ivdsl->GetComputerName(buf);
			strcat(buf,"/");

			SetDlgItemText(hDlg, IDC_COMPUTER_NAME, buf);
		}
		SetDlgItemText(hDlg, IDC_FSNAME, VDTextWToA(VDFileSplitPath(g_szInputAVIFile)).c_str());
		SetWindowLong(hDlg, DWL_USER, lParam);
		return TRUE;
	case WM_COMMAND:
		switch(LOWORD(wParam)) {
		case IDOK:
			SendDlgItemMessage(hDlg, IDC_FSNAME, WM_GETTEXT, 128, GetWindowLong(hDlg,DWL_USER));
			EndDialog(hDlg, TRUE);
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		}
		break;
	}

	return FALSE;
}

void ActivateFrameServerDialog(HWND hwnd) {
	static wchar_t fileFilters[]=
		L"VirtualDub AVIFile signpost (*.vdr)\0"		L"*.vdr\0"
		;

	char szServerName[128];

	if (!InitServerDLL()) return;

	if (!DialogBoxParam(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_SERVER_SETUP), hwnd, FrameServerSetupDlgProc, (LPARAM)szServerName))
		return;

	try {
		Frameserver fs(inputVideoAVI, inputAudio, hwnd, &g_dubOpts);

		const VDStringW fname(VDGetSaveFileName(kFileDialog_Signpost, (VDGUIHandle)hwnd, L"Save .VDR signpost for AVIFile handler", fileFilters, g_prefs.main.fAttachExtension ? L"vdr" : NULL, 0, 0));

		if (!fname.empty()) {
			long buf[5];
			char sname[128];
			int slen;

			ivdsl->GetComputerName(sname);
			strcat(sname,"/");
			strcat(sname,szServerName);
			slen = strlen(sname);
			slen += slen&1;

			buf[0] = 'FFIR';
			buf[1] = slen+12;
			buf[2] = 'MRDV';
			buf[3] = 'HTAP';
			buf[4] = slen;

			VDFile file(fname.c_str(), nsVDFile::kWrite | nsVDFile::kDenyRead | nsVDFile::kCreateAlways);

			file.write(buf, 20);
			file.write(sname, strlen(sname));
			if (strlen(sname) & 1)
				file.write("", 1);

			file.close();
		}

		_RPT0(0,"Attempting to initialize frameserver...\n");

		fs.Go(ivdsl, szServerName);

		_RPT0(0,"Frameserver exit.\n");

	} catch(const MyError& e) {
		e.post(hwnd, "Frameserver error");
	}
}