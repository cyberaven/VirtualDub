//	VirtualDub - Video processing and capture application
//	System library component
//	Copyright (C) 1998-2004 Avery Lee, All Rights Reserved.
//
//	Beginning with 1.6.0, the VirtualDub system library is licensed
//	differently than the remainder of VirtualDub.  This particular file is
//	thus licensed as follows (the "zlib" license):
//
//	This software is provided 'as-is', without any express or implied
//	warranty.  In no event will the authors be held liable for any
//	damages arising from the use of this software.
//
//	Permission is granted to anyone to use this software for any purpose,
//	including commercial applications, and to alter it and redistribute it
//	freely, subject to the following restrictions:
//
//	1.	The origin of this software must not be misrepresented; you must
//		not claim that you wrote the original software. If you use this
//		software in a product, an acknowledgment in the product
//		documentation would be appreciated but is not required.
//	2.	Altered source versions must be plainly marked as such, and must
//		not be misrepresented as being the original software.
//	3.	This notice may not be removed or altered from any source
//		distribution.

#ifndef f_VD2_SYSTEM_W32ASSIST_H
#define f_VD2_SYSTEM_W32ASSIST_H

#include <windows.h>

#include <vd2/system/VDString.h>

inline bool VDIsWindowsNT() {
#ifdef _M_AMD64
	return true;
#else
	static bool is_nt = !(GetVersion() & 0x80000000);

	return is_nt;
#endif
}

// useful constants missing from the Platform SDK

enum {
#ifdef _M_AMD64
	MENUITEMINFO_SIZE_VERSION_400A = sizeof(MENUITEMINFOA),
	MENUITEMINFO_SIZE_VERSION_400W = sizeof(MENUITEMINFOW)
#else
	MENUITEMINFO_SIZE_VERSION_400A = (offsetof(MENUITEMINFOA, cch) + sizeof(UINT)),
	MENUITEMINFO_SIZE_VERSION_400W = (offsetof(MENUITEMINFOW, cch) + sizeof(UINT))
#endif
};

// helper functions

LPVOID		VDConvertThreadToFiberW32(LPVOID parm);
void		VDSwitchToFiberW32(LPVOID fiber);

int			VDGetSizeOfBitmapHeaderW32(const BITMAPINFOHEADER *pHdr);
void		VDSetWindowTextW32(HWND hwnd, const wchar_t *s);
VDStringW	VDGetWindowTextW32(HWND hwnd);
void		VDAppendMenuW32(HMENU hmenu, UINT flags, UINT id, const wchar_t *text);
void		VDCheckMenuItemByCommandW32(HMENU hmenu, UINT cmd, bool checked);
void		VDCheckRadioMenuItemByCommandW32(HMENU hmenu, UINT cmd, bool checked);
void		VDEnableMenuItemByCommandW32(HMENU hmenu, UINT cmd, bool checked);
void		VDSetMenuItemTextByCommandW32(HMENU hmenu, UINT cmd, const wchar_t *text);

LRESULT		VDDualCallWindowProcW32(WNDPROC wp, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT		VDDualDefWindowProcW32(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

EXECUTION_STATE VDSetThreadExecutionStateW32(EXECUTION_STATE esFlags);

bool		VDSetFilePointerW32(HANDLE h, sint64 pos, DWORD dwMoveMethod);
bool		VDGetFileSizeW32(HANDLE h, sint64& size);

HMODULE		VDGetLocalModuleHandleW32();

bool		VDDrawTextW32(HDC hdc, const wchar_t *s, int nCount, LPRECT lpRect, UINT uFormat);

bool		VDPatchModuleImportTableW32(HMODULE hmod, const char *srcModule, const char *name, void *pCompareValue, void *pNewValue, void *volatile *ppOldValue);

#endif
