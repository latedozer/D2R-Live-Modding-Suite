// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Native tests only: the little of <windows.h> the API headers use (nothing is looked up).
#pragma once
typedef void* HMODULE;
typedef void (*FARPROC)();
inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline FARPROC GetProcAddress(HMODULE, const char*) { return nullptr; }
#ifndef __cdecl
#define __cdecl
#endif
