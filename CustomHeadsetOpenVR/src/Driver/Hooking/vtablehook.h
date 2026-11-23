/*
The MIT License (MIT)

Copyright (c) 2013 Thordin

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#elif __linux__
#include <unistd.h>
#include <sys/mman.h>

#endif

inline int vtablehook_unprotect(void* region) {
#ifdef _WIN32
	MEMORY_BASIC_INFORMATION mbi = {};
	VirtualQuery((LPCVOID)region, &mbi, sizeof(mbi));
	if(!VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READWRITE, &mbi.Protect)){
		return -1;
	}
	return mbi.Protect;
#elif __linux__
	int vtablehook_pagesize = sysconf(_SC_PAGE_SIZE);
	int vtablehook_pagemask = ~(vtablehook_pagesize-1);
	if(mprotect((void*) ((intptr_t)region & vtablehook_pagemask), vtablehook_pagesize, PROT_READ|PROT_WRITE|PROT_EXEC)){
		return -1;
	}
	return PROT_READ|PROT_EXEC;
#endif
}

inline void vtablehook_protect(void* region, int protection) {
#ifdef _WIN32
	MEMORY_BASIC_INFORMATION mbi = {};
	VirtualQuery((LPCVOID)region, &mbi, sizeof(mbi));
	VirtualProtect(mbi.BaseAddress, mbi.RegionSize, protection, &mbi.Protect);
#elif __linux__
	int vtablehook_pagesize = sysconf(_SC_PAGE_SIZE);
	int vtablehook_pagemask = ~(vtablehook_pagesize-1);
	mprotect((void*) ((intptr_t)region & vtablehook_pagemask), vtablehook_pagesize, protection);
#endif
}

/*
* instance: pointer to an instance of a class
* hook: function to overwrite with
* offset: 0 = method 1, 1 = method 2 etc...
* return: original function
*/

inline void* vtablehook_hook(void* instance, void* hook, int offset) {
	intptr_t vtable = *((intptr_t*)instance);
	intptr_t entry = vtable + sizeof(intptr_t) * offset;
	intptr_t original = *((intptr_t*) entry);
	
	if(!hook){
		return (void*)original;
	}
	
	int original_protection = vtablehook_unprotect((void*)entry);
	if(original_protection == -1){
		return nullptr;
	}
	*((intptr_t*)entry) = (intptr_t)hook;
	vtablehook_protect((void*)entry, original_protection);

	return (void*)original;
}

/*
 * instance: pointer to an instance of a class
 * offset: the vtable entry to read
 * return: the entry, or nullptr when the object has no vtable
 */
inline void* vtablehook_entry(void* instance, int offset) {
	if(!instance){
		return nullptr;
	}
	intptr_t vtable = *((intptr_t*)instance);
	if(!vtable){
		return nullptr;
	}
	return (void*)*((intptr_t*)(vtable + sizeof(intptr_t) * offset));
}

/*
 * instance: pointer to an instance of a class
 * return: the array that instance dispatches through, which is the only thing that identifies which version
 * of an interface a call was made through. A COM object that implements several interface versions has one
 * array per version, so two objects answering to the same class can dispatch through different arrays.
 * Returns nullptr when the object has no vtable.
 */
inline void* vtablehook_array(const void* instance){
	if(!instance){
		return nullptr;
	}
	return (void*)*((const intptr_t*)instance);
}

/*
 * instance: pointer to an instance of a class
 * expected: the entry to look for. This has to be a plain function pointer: a pointer to a member
 * function is not an address, so the bytes of &ID3D11DeviceContext::CopySubresourceRegion are never equal
 * to the entry that call dispatches through. Use vtablehook_member_slot for that case.
 * limit: how many entries to scan
 * return: the vtable offset holding expected, or -1 when nothing matches
 */
inline int vtablehook_find_offset(void* instance, void* expected, int limit) {
	if(!instance || !expected){
		return -1;
	}
	for(int offset = 0; offset < limit; offset++){
		if(vtablehook_entry(instance, offset) == expected){
			return offset;
		}
	}
	return -1;
}

/*
 * memberPointer: the bytes of a pointer to a member function, copied into a buffer because a pointer to a
 * member has no type that can be compared with a vtable entry
 * size: the size of those bytes
 * return: the vtable offset a call through that member pointer dispatches through, or -1 when the bytes are
 * not the shape this expects
 */
inline int vtablehook_member_slot(const void* memberPointer, size_t size) {
	if(!memberPointer || size != sizeof(intptr_t)){
		return -1;
	}
	intptr_t value;
	memcpy(&value, memberPointer, sizeof(value));
	if(!value){
		return -1;
	}

#ifdef _MSC_VER
	// MSVC x64 stores the address of the dispatch thunk it generates, not the entry. The thunk is
	// `mov rax, [rcx]` followed by `jmp qword ptr [rax + disp]`, and disp / 8 is the slot.
	const unsigned char* code = (const unsigned char*)value;
	if(code[0] != 0x48 || code[1] != 0x8b || code[2] != 0x01 || code[3] != 0xff){
		return -1;
	}
	if(code[4] == 0x60){
		return (int)code[5] / (int)sizeof(intptr_t);
	}
	if(code[4] == 0x90 || code[4] == 0xa0){
		int32_t displacement = 0;
		memcpy(&displacement, code + 5, sizeof(displacement));
		if(displacement < 0){
			return -1;
		}
		return (int)(displacement / (int32_t)sizeof(intptr_t));
	}
	return -1;
#else
	// The Itanium ABI stores the slot plus one for a virtual member pointer.
	if(value <= 0){
		return -1;
	}
	return (int)((value - 1) / (intptr_t)sizeof(intptr_t));
#endif
}