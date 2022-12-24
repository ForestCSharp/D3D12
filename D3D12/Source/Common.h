#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HR_CHECK(expr)  \
{\
	HRESULT result = (expr);\
	if (FAILED(result))\
	{\
		printf("FAILED HRESULT: line: %u, code: %s, error: %x", __LINE__, #expr, result);\
		DebugBreak();\
		exit(-1);\
	}\
}\

#if defined(_DEBUG) || defined(DBG)
#define NAME_D3D12_OBJECT(obj, name) obj->SetName(name);
#else
#define NAME_D3D12_OBJECT(obj, name) 
#endif

#define XOR(a,b) a ? !b : b

//STL Implementation
#define STL_IMPL std
//#define STL_IMPL EASTL