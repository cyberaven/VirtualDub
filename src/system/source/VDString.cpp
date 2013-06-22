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

#include "stdafx.h"
#include <vd2/system/VDString.h>

const VDStringSpanA::value_type VDStringSpanA::sNull[1] = {0};

void VDStringA::push_back_extend() {
	VDASSERT(mpEOS == mpEnd);
	size_type current_size = (size_type)(mpEnd - mpBegin);

	reserve_slow(current_size * 2 + 1, current_size);
}

void VDStringA::resize_slow(size_type n, size_type current_size) {
	resize_slow(n, current_size, 0);
}

void VDStringA::resize_slow(size_type n, size_type current_size, value_type c) {
	VDASSERT(n > current_size);

	size_type current_capacity = (size_type)(mpEOS - mpBegin);
	if (n > current_capacity)
		reserve_slow(n, current_capacity);

	memset(mpBegin + current_size, c, n - current_size);
	mpEnd = mpBegin + n;
	*mpEnd = 0;
}

void VDStringA::reserve_slow(size_type n, size_type current_capacity) {
	VDASSERT(n > current_capacity);

	size_type current_size = (size_type)(mpEnd - mpBegin);
	value_type *s = new value_type[n + 1];
	memcpy(s, mpBegin, (current_size + 1) * sizeof(value_type));
	if (mpBegin != sNull)
		delete[] mpBegin;

	mpBegin = s;
	mpEnd = s + current_size;
	mpEOS = s + n;
}

void VDStringA::reserve_amortized_slow(size_type n, size_type current_size, size_type current_capacity) {
	n += current_size;

	size_type doublesize = current_size * 2;
	if (n < doublesize)
		n = doublesize;

	reserve_slow(n, current_capacity);
}

VDStringA& VDStringA::sprintf(const value_type *format, ...) {
	va_list val;
	va_start(val, format);
	assign(VDFastTextVprintfA(format, val));
	va_end(val);
	VDFastTextFree();
	return *this;
}

///////////////////////////////////////////////////////////////////////////////

const VDStringSpanW::value_type VDStringSpanW::sNull[1] = {0};

void VDStringW::push_back_extend() {
	VDASSERT(mpEOS == mpEnd);
	size_type current_size = (size_type)(mpEnd - mpBegin);

	reserve_slow(current_size * 2 + 1, current_size);
}

void VDStringW::resize_slow(size_type n, size_type current_size) {
	VDASSERT(n > current_size);

	size_type current_capacity = (size_type)(mpEOS - mpBegin);
	if (n > current_capacity)
		reserve_slow(n, current_capacity);

	mpEnd = mpBegin + n;
	*mpEnd = 0;
}

void VDStringW::reserve_slow(size_type n, size_type current_capacity) {
	VDASSERT(current_capacity == (size_type)(mpEOS - mpBegin));
	VDASSERT(n > current_capacity);

	size_type current_size = (size_type)(mpEnd - mpBegin);
	value_type *s = new value_type[n + 1];
	memcpy(s, mpBegin, (current_size + 1) * sizeof(value_type));
	if (mpBegin != sNull)
		delete[] mpBegin;

	mpBegin = s;
	mpEnd = s + current_size;
	mpEOS = s + n;
}

void VDStringW::reserve_amortized_slow(size_type n, size_type current_size, size_type current_capacity) {
	n += current_size;

	size_type doublesize = current_size * 2;
	if (n < doublesize)
		n = doublesize;

	reserve_slow(n, current_capacity);
}

VDStringW& VDStringW::sprintf(const value_type *format, ...) {
	va_list val;
	va_start(val, format);
	assign(VDFastTextVprintfW(format, val));
	va_end(val);
	VDFastTextFree();
	return *this;
}
