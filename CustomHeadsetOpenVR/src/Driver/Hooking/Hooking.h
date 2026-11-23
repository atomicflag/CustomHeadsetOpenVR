#pragma once

#include "../DriverLog.h"

// #include "../../../../ThirdParty/minhook/include/MinHook.h"
#include "./vtablehook.h"
#include <map>
#include <string>
#include <vector>

/*
 * One entry a detour is installed at, and the interface version that entry belongs to. Version 0 is the
 * interface the hook was created for. A higher version is an object the same COM object answers to through
 * QueryInterface, and it has its own array, so a call made through that version dispatches through that array
 * rather than through the entry version 0 names. The array is recorded at install time, which is what a first
 * call line compares the object that fired against.
 */
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
	// Every entry the detour is installed at, the base interface's first. `originalFunc`, `originalVTableOffset`
	// and `originalObject` are the base install and stay that way, because the entry a version's array holds is
	// the same function address as the base entry, so one stored original covers the calls that come through any
	// of the recorded arrays.
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

	/*
	 * Install the same detour at an entry that belongs to an interface version, and record which version it is.
	 * The entry is chosen by the caller, which is the offset the scan found, so nothing here assumes where a
	 * version's entry sits. `originalFunc` is not replaced: the entry a version's array holds is the same
	 * function address as the base entry, so the base install's stored original is the right thing for the
	 * detour to call no matter which array the call came through.
	 */
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

	/*
	 * Which recorded array a call on instance dispatches through, which is the version number, or -1 when the
	 * object that fired is not one this hook is installed on. Only the vtable pointer is read and compared, so
	 * this is not a call, and it is used for the first call line rather than for every call.
	 */
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

	// The recorded entry for one version, so a first call line can name the offset the detour is installed at in
	// that array, or nullptr when the hook is not installed for that version.
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
			// Every recorded entry is restored, not only the base one, so an interface version installed here is
			// not left patched after the hook is taken down. The base entry is in `entries`, so it is not written
			// back twice; the fallback is only for a hook that was installed before any entry was recorded.
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