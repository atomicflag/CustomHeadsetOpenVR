#pragma once

#include "../DriverLog.h"

// #include "../../../../ThirdParty/minhook/include/MinHook.h"
#include "./vtablehook.h"
#include <map>
#include <string>
#include <vector>

// one vtable entry the detour is installed at, and the interface version it belongs to
struct VtableHookEntry{
	int version = 0;
	void* object = nullptr;
	void* array = nullptr;
	int offset = -1;
	void* original = nullptr;
};


class IHook
{
public:
	const std::string name;

	IHook(const std::string &name) : name(name) { }
	virtual ~IHook() { }

	virtual void Destroy() = 0;

	static bool Exists(const std::string &name);
	static void Register(IHook *hook);
	static void Unregister(IHook *hook);
	static void DestroyAll();

private:
	static std::map<std::string, IHook *> hooks;
};

template<class FuncType> class Hook : public IHook
{
public:
	FuncType originalFunc = nullptr;
	int originalVTableOffset = -1;
	void* originalObject = nullptr;
	std::vector<VtableHookEntry> entries;
	Hook(const std::string &name) : IHook(name) { }

	bool CreateHookInObjectVTable(void *object, int vtableOffset, FuncType detourFunction)
	{
		/*
		// For virtual objects, VC++ adds a pointer to the vtable as the first member.
		// To access the vtable, we simply dereference the object.
		void **vtable = *((void ***)object);

		// The vtable itself is an array of pointers to member functions,
		// in the order they were declared in.
		targetFunc = vtable[vtableOffset];

		auto err = MH_CreateHook(targetFunc, detourFunction, (LPVOID *)&originalFunc);
		if (err != MH_OK)
		{
			DriverLog("Failed to create hook for %s, error: %s", name.c_str(), MH_StatusToString(err));
			return false;
		}

		err = MH_EnableHook(targetFunc);
		if (err != MH_OK)
		{
			DriverLog("Failed to enable hook for %s, error: %s", name.c_str(), MH_StatusToString(err));
			MH_RemoveHook(targetFunc);
			return false;
		}
		*/
		if(originalFunc){
			DriverLog("Hook for %s already exists", name.c_str());
			return false;
		}
		
		originalFunc = (FuncType)vtablehook_hook(object, (void*)detourFunction, vtableOffset);
		originalObject = object;
		originalVTableOffset = vtableOffset;
		if(originalFunc){
			entries.push_back({0, object, vtablehook_array(object), vtableOffset, originalFunc});
		}
		
		DriverLog("Enabled hook for %s", name.c_str());
		enabled = true;
		return true;
	}

	bool AddVersionEntry(int version, void* object, int vtableOffset, FuncType detourFunction)
	{
		void* original = vtablehook_hook(object, (void*)detourFunction, vtableOffset);
		if(!original){
			return false;
		}
		entries.push_back({version, object, vtablehook_array(object), vtableOffset, original});
		enabled = true;
		return true;
	}

	int VersionOfArray(const void* instance) const
	{
		void* array = vtablehook_array(instance);
		if(!array){
			return -1;
		}
		for(const auto& entry : entries){
			if(entry.array == array){
				return entry.version;
			}
		}
		return -1;
	}

	const VtableHookEntry* EntryForVersion(int version) const
	{
		for(const auto& entry : entries){
			if(entry.version == version){
				return &entry;
			}
		}
		return nullptr;
	}

	void Destroy()
	{
		if (enabled)
		{
			// MH_RemoveHook(targetFunc);
			if(entries.empty() && originalFunc){
				vtablehook_hook(originalObject, (void*)originalFunc, originalVTableOffset);
			}
			for(const auto& entry : entries){
				vtablehook_hook(entry.object, entry.original, entry.offset);
			}
			entries.clear();
			originalFunc = nullptr;
			enabled = false;
		}
	}

private:
	bool enabled = false;
	void* targetFunc = nullptr;
};