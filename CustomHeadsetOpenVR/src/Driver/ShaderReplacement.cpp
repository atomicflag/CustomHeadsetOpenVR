#include "ShaderReplacement.h"
#include "DriverLog.h"
#include "Hooking/Hooking.h"
#include "../Config/ConfigLoader.h"
#include "../Helpers/ShaderDecode.h"
#include "../Helpers/LocalDimmingCapture.h"
#include <mutex>
#include <map>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include <locale>
#include <codecvt>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdint>

#include "../../../ThirdParty/easywsclient/easywsclient.hpp"

#ifdef _WIN32
#include "d3d11.h"
// The interface versions the context can be called through. Each one is declared in its own header, and the
// version's object comes from QueryInterface, so these are needed to read a version's own array.
#include "d3d11_1.h"
#include "d3d11_2.h"
#include "d3d11_3.h"
#include "d3d11_4.h"
#include "d3dcompiler.h"
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "D3DCompiler.lib")

#include "../../../ThirdParty/minhook/include/MinHook.h"


// treats the current file as a directory to get relative paths
std::string shaderDevPath = __FILE__ "/../../../DriverFiles/resources/shaders/d3d11/";

std::string getShaderPath(){
	std::string shaderPath = driverConfigLoader.info.driverResources + "shaders/d3d11/";
	if(std::filesystem::exists(shaderDevPath)){
		// pull from my dev path instead
		shaderPath = shaderDevPath;
	}
	return shaderPath;
}

/*
virtual HRESULT STDMETHODCALLTYPE CreatePixelShader( 
_In_reads_(BytecodeLength)  const void *pShaderBytecode,
_In_  SIZE_T BytecodeLength,
_In_opt_  ID3D11ClassLinkage *pClassLinkage,
_COM_Outptr_opt_  ID3D11PixelShader **ppPixelShader) = 0;
*/
static Hook<void(*)(ID3D11Device*, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)> 
	CreatePixelShaderHook("ID3D11Device::CreatePixelShader");

/*
 * Resolves the vtable offset a detour should be installed at instead of assuming the constant the original
 * scaffold recorded, and logs what the lookup answered. Two lookups are made because they answer different
 * questions:
 *
 *   - the scan compares the object's entries with the member pointer. For a COM interface method that can
 *     never match, because a pointer to a member is not an address: MSVC stores the address of the dispatch
 *     thunk it generates, so the scan answers -1 even when the offset is right.
 *   - the thunk itself says which slot a call through the member pointer dispatches through, which is the
 *     lookup that can actually confirm a constant. It is compiler specific, so it is a lookup rather than a
 *     proof.
 *
 * When nothing matches, the code says so plainly. It still installs the detour at the constant, because an
 * entry nothing is known about is not a wrong entry, and a hook that is not installed can never show a
 * call, which is the question the run is read against.
 */
static int ResolveHookOffset(const char* name, void* object, const void* memberPointerBytes, size_t memberPointerSize, int constantOffset){
	intptr_t memberPointerValue = 0;
	if(memberPointerSize == sizeof(intptr_t)){
		memcpy(&memberPointerValue, memberPointerBytes, sizeof(memberPointerValue));
	}

	int scanned = memberPointerValue ? vtablehook_find_offset(object, (void*)memberPointerValue, 64) : -1;
	int resolved = vtablehook_member_slot(memberPointerBytes, memberPointerSize);
	int offset = scanned >= 0 ? scanned : (resolved >= 0 ? resolved : constantOffset);

	LocalDimmingLog("vtablehook %s: scan %s, dispatch slot %d, constant %d, agreement %s, hooked at %d, entry there is %p",
		name,
		scanned >= 0 ? (scanned == constantOffset ? "matched, and agrees with the constant" : "matched at a different offset") : "found no entry equal to the member pointer",
		resolved, constantOffset, (resolved == constantOffset) ? "yes" : "no", offset, vtablehook_entry(object, offset));

	return offset;
}

// holder for bytecode
struct Bytecode{
	char* data;
	size_t length;
};

// map of functions to replace shader bytecode with based on the first 32 bytes of the shader bytecode
static std::map<std::string, Bytecode(*)()> shaderReplacements;

// Shader cache configuration
static const size_t MAX_SHADER_CACHE_SIZE = 100;

// Mutex for thread-safe access to the shader cache
static std::mutex shaderCacheMutex;

// Cached shader bytecode structure
struct CachedShader {
	char* data;
	size_t length;
	CachedShader() : data(nullptr), length(0) {}
	~CachedShader() {
		delete[] data;
	}
	// Move constructor
	CachedShader(CachedShader&& other) noexcept : data(other.data), length(other.length) {
		other.data = nullptr;
		other.length = 0;
	}
	// Move assignment operator
	CachedShader& operator=(CachedShader&& other) noexcept {
		if(this != &other){
			delete[] data;
			data = other.data;
			length = other.length;
			other.data = nullptr;
			other.length = 0;
		}
		return *this;
	}
	// Disable copy constructor and copy assignment
	CachedShader(const CachedShader&) = delete;
	CachedShader& operator=(const CachedShader&) = delete;
};

// Hash map for quick lookup of compiled shaders by configuration hash
static std::unordered_map<uint64_t, CachedShader> shaderCache;

// Ordered list of hashes for LRU eviction (oldest at front)
static std::list<uint64_t> shaderCacheOrder;

// FNV-1a hash function for generating unique hashes from data
static uint64_t FNV1aHash(const void* data, size_t length){
	uint64_t hash = 0xCBF29CE484222325; // FNV offset basis
	for(size_t i = 0; i < length; i++){
		hash ^= ((const uint8_t*)data)[i];
		hash *= 0x100000001B3; // FNV prime
	}
	return hash;
}

// Combine multiple hashes into a single hash
static uint64_t CombineHashes(uint64_t hash1, uint64_t hash2){
	// XOR the second hash into the first and then hash the result
	uint64_t combined = hash1 ^ hash2;
	combined ^= combined >> 33;
	combined *= 0xFF51AFD7ED558CCD;
	combined ^= combined >> 33;
	combined *= 0xC4CEB9FE1A85EC53;
	combined ^= combined >> 33;
	return combined;
}

// Remove the oldest cached shader entry when the cache exceeds the maximum size
static void EvictOldestShaderCacheEntry(){
	if(shaderCacheOrder.empty()){
		return;
	}
	uint64_t oldestHash = shaderCacheOrder.front();
	shaderCacheOrder.pop_front();
	auto it = shaderCache.find(oldestHash);
	if(it != shaderCache.end()){
		shaderCache.erase(it);
	}
}

// Compile a shader from file with caching based on file content and defines
// Hashes all shader files in the directory together with all defines to create a unique cache key
// Returns the compiled shader bytecode (either from cache or newly compiled)
// Failures are not cached
// errorBlob is optionally output on failure for error reporting
static Bytecode D3DCompileFromFileCached(const wchar_t* filePath, D3D_SHADER_MACRO* defines, LPCSTR entryPoint, LPCSTR shaderModel, UINT flags, ID3DBlob** errorBlob = nullptr){
	// Hash all shader files in the directory to detect any changes to included files
	uint64_t directoryHash = 0;
	std::string shaderDir = getShaderPath();
	std::vector<std::filesystem::directory_entry> entries;
	for(const auto& entry : std::filesystem::directory_iterator(shaderDir)){
		if(entry.is_regular_file()){
			entries.push_back(entry);
		}
	}
	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b){
		return a.path() < b.path();
	});
	for(const auto& entry : entries){
		std::string fullPath = entry.path().string();
		std::ifstream file(fullPath, std::ios::binary);
		if(file){
			file.seekg(0, std::ios::end);
			size_t fileSize = file.tellg();
			file.seekg(0, std::ios::beg);
			std::vector<char> fileData(fileSize);
			file.read(fileData.data(), fileSize);
			
			// Hash the filename to ensure different filenames produce different hashes
			uint64_t fileNameHash = FNV1aHash(fullPath.c_str(), fullPath.length());
			// Hash the file contents
			uint64_t fileContentsHash = FNV1aHash(fileData.data(), fileSize);
			// Combine into directory hash
			directoryHash = CombineHashes(directoryHash, CombineHashes(fileNameHash, fileContentsHash));
		}
	}
	
	// Hash all the defines to create a unique configuration hash
	uint64_t definesHash = 0;
	if(defines){
		for(int i = 0; defines[i].Name != nullptr && defines[i].Name[0] != '\0'; i++){
			uint64_t nameHash = FNV1aHash(defines[i].Name, strlen(defines[i].Name));
			uint64_t valueHash = 0;
			if(defines[i].Definition){
				valueHash = FNV1aHash(defines[i].Definition, strlen(defines[i].Definition));
			}
			definesHash = CombineHashes(definesHash, CombineHashes(nameHash, valueHash));
		}
	}
	
	// Combine directory hash and defines hash into a single cache key
	uint64_t cacheKey = CombineHashes(directoryHash, definesHash);
	
	// Check cache with lock
	{
		std::lock_guard<std::mutex> lock(shaderCacheMutex);
		auto it = shaderCache.find(cacheKey);
		if(it != shaderCache.end()){
			// Cache hit - return a copy of the cached bytecode
			DriverLog("Shader cache hit for key %llu", (unsigned long long)cacheKey);
			Bytecode result = {new char[it->second.length], it->second.length};
			memcpy(result.data, it->second.data, it->second.length);
			return result;
		}
	}
	
	// Cache miss - compile the shader using D3DCompileFromFile
	ID3DBlob* shaderBlob = nullptr;
	ID3DBlob* localErrorBlob = nullptr;
	HRESULT hr = D3DCompileFromFile(
		filePath,
		defines,
		D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entryPoint,
		shaderModel,
		flags,
		0,
		&shaderBlob,
		&localErrorBlob
	);
	
	if(FAILED(hr) || !shaderBlob){
		DriverLog("Failed to compile shader file: %ls", filePath);
		if(localErrorBlob){
			DriverLog("Error: %s", (char*)localErrorBlob->GetBufferPointer());
			if(errorBlob){
				*errorBlob = localErrorBlob;
			}else{
				localErrorBlob->Release();
			}
		}else if(errorBlob){
			*errorBlob = nullptr;
		}
		if(shaderBlob){
			shaderBlob->Release();
		}
		return {nullptr, 0};
	}
	
	// Store in cache
	{
		std::lock_guard<std::mutex> lock(shaderCacheMutex);
		
		// Evict oldest entries if cache is full
		while(shaderCache.size() >= MAX_SHADER_CACHE_SIZE){
			EvictOldestShaderCacheEntry();
		}
		
		CachedShader cached;
		cached.data = new char[shaderBlob->GetBufferSize()];
		cached.length = shaderBlob->GetBufferSize();
		memcpy(cached.data, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
		
		shaderCache[cacheKey] = std::move(cached);
		shaderCacheOrder.push_back(cacheKey);
		
		DriverLog("Shader cached with key %llu, cache size: %zu", (unsigned long long)cacheKey, shaderCache.size());
	}
	
	// Return the compiled bytecode
	Bytecode result = {new char[shaderBlob->GetBufferSize()], shaderBlob->GetBufferSize()};
	memcpy(result.data, shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
	
	shaderBlob->Release();
	return result;
}

// debug shaders by logging the first 32 bytes of the shader bytecode
void LogShaderIdentifier(std::string identifierString, size_t length){
	DriverLog("Shader identifier: %c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c, length: %zu", identifierString[0], identifierString[1], identifierString[2], identifierString[3], identifierString[4], identifierString[5], identifierString[6], identifierString[7], identifierString[8], identifierString[9], identifierString[10], identifierString[11], identifierString[12], identifierString[13], identifierString[14], identifierString[15], identifierString[16], identifierString[17], identifierString[18], identifierString[19], identifierString[20], identifierString[21], identifierString[22], identifierString[23], identifierString[24], identifierString[25], identifierString[26], identifierString[27], identifierString[28], identifierString[29], identifierString[30], identifierString[31], length);
}

int dumpId = 0;

// The first call through each detour is logged once per process. Without these lines a run that produced
// nothing cannot tell a function that is never called from a hook installed at the wrong vtable offset,
// which is the whole question the next run is read against. The exchange is the guard, so no lock is taken
// and every log call happens with nothing held.
static std::atomic<bool> createPixelShaderFirstCallLogged{ false };
static std::atomic<bool> copySubresourceRegionFirstCallLogged{ false };
static std::atomic<bool> copyResourceFirstCallLogged{ false };

// function that hooks the CreatePixelShader function of the ID3D11Device interface
static void DetourCreatePixelShader(ID3D11Device* _this, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader){
	if(!createPixelShaderFirstCallLogged.exchange(true)){
		LocalDimmingLog("ID3D11Device::CreatePixelShader detour first call, hooked at vtable offset %d, bytecode length %zu",
			CreatePixelShaderHook.originalVTableOffset, BytecodeLength);
	}
 	// DriverLog("DetourCreatePixelShader Called");
	if(pShaderBytecode == nullptr || BytecodeLength < 32){
		CreatePixelShaderHook.originalFunc(_this, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
		return;
	}
	// identify shader by first 32 bytes which contains includes a hash
	std::string identifierString((char*)pShaderBytecode, 32);
	// LogShaderIdentifier(identifierString, BytecodeLength);
	if(driverConfig.customShader.enable && shaderReplacements.find(identifierString) != shaderReplacements.end()){
		Bytecode newBytecode = shaderReplacements[identifierString]();
		// DriverLog("==================================");
		if(newBytecode.data == nullptr || newBytecode.length == 0){
			DriverLog("Failed to get new bytecode");
		}else{
			DriverLog("Replacing shader bytecode");
			CreatePixelShaderHook.originalFunc(_this, newBytecode.data, newBytecode.length, pClassLinkage, ppPixelShader);
			delete[] newBytecode.data;
			return;
		}
	}
	// dump shader bytecode to file
	// std::string dumpPath = driverConfigLoader.info.driverResources + "shaders/dump/" + std::to_string(dumpId++) + ".fxo";
	// FILE* dumpFile = fopen(dumpPath.c_str(), "wb+");
	// if(dumpFile){
	// 	fwrite(pShaderBytecode, 1, BytecodeLength, dumpFile);
	// 	fclose(dumpFile);
	// 	DriverLog("Dumped shader bytecode to %s", dumpPath.c_str());
	// }else{
	// 	DriverLog("Failed to dump shader bytecode to %s", dumpPath.c_str());
	// }
	// if no replacement is found, call the original function with the original bytecode
	CreatePixelShaderHook.originalFunc(_this, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
}

/*
 * The local dimming hook that is left. Runs 14 and 15 closed the render thread route: no draw detour and no
 * Flush detour ever fired, and the per interface version lookup showed all four versions answering to the same
 * object and the same array, so there is one cumulative array and nothing left to hook per version, and the
 * compositor's context is not the object that was hooked. A render target read cannot be reached from a draw
 * detour, so nothing is installed on the context any more: the four draw detours, the Flush detour and the two
 * copy detours are removed, because a hook that fires on every draw costs per frame even when it only decides
 * and returns, and the capture no longer depends on any of them. It reads the shared texture on the device its
 * own module created, on the local dimming thread.
 *
 * CreatePixelShader stays for the custom shader feature, and CreateTexture2D stays for the desc observation and
 * for the handle grab. Both are installed only when local dimming is on and both check the same setting at call
 * time, so a hook that is present but not wanted does nothing but pass the call on. The first call flag is an
 * atomic and the grab is done once for the first plausible desc.
 */
typedef HRESULT(*CreateTexture2DFunction)(ID3D11Device*, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D);
static Hook<CreateTexture2DFunction>
	CreateTexture2DHook("ID3D11Device::CreateTexture2D");

// Whether the object a detour fired on is the instance Initialize() hooked. The hook is installed on one object,
// so a call that fires on a different object is a call on an instance this module has no hook on, which is a
// different outcome than the hook not firing at all and is the other half of what a first call line answers. The
// handle is grabbed for the resource the call that fired returned, so the same fact has to be readable on the
// line that reports the grab: a first call from an unhooked instance names a device this module did not record.
static bool IsHookedInstance(const void* hookedObject, const void* firingObject){
	return hookedObject != nullptr && firingObject == hookedObject;
}

static std::atomic<bool> createTexture2DFirstPlausibleDescLogged{ false };

/*
 * Resolves the offset for a detour that has no recorded constant, which is the hook added here. The dispatch
 * slot from the pointer-to-member is the only source. When it does not resolve, nothing is installed: an offset
 * nobody is sure of would make a first call line name a function that call is not a call to, which is worse than
 * no line at all. The scan is kept alongside the slot because it is the other half of the lookup the last run
 * used, and it is logged even though it cannot match a pointer-to-member.
 */
static int ResolveLookupHookOffset(const char* name, void* object, const void* memberPointerBytes, size_t memberPointerSize){
	intptr_t memberPointerValue = 0;
	if(memberPointerSize == sizeof(intptr_t)){
		memcpy(&memberPointerValue, memberPointerBytes, sizeof(memberPointerValue));
	}

	int scanned = memberPointerValue ? vtablehook_find_offset(object, (void*)memberPointerValue, 64) : -1;
	int resolved = vtablehook_member_slot(memberPointerBytes, memberPointerSize);
	int offset = scanned >= 0 ? scanned : resolved;

	LocalDimmingLog("vtablehook %s: scan %s, dispatch slot %d, no constant to compare with, %s, entry there is %p",
		name,
		scanned >= 0 ? (scanned == resolved ? "matched the dispatch slot" : "matched at a different offset") : "found no entry equal to the member pointer",
		resolved,
		offset >= 0 ? "installing at that offset" : "nothing resolved, not installed",
		offset >= 0 ? vtablehook_entry(object, offset) : nullptr);

	return offset;
}

/*
 * The install, with no per interface version step. Run 15 showed all four context versions answering to the same
 * object and the same array, so a call through any of them dispatches through the entry that is already hooked,
 * and there is nothing left to scan. The earlier runs were read against a per interface scan limit and an offset,
 * which are no longer decisions to make because there is no second array to choose an offset in.
 */
template<class FuncType, class MemberPointerType>
static void InstallLookupHook(Hook<FuncType>& hook, void* object, MemberPointerType memberPointer, FuncType detour){
	unsigned char memberPointerBytes[sizeof(memberPointer)];
	memcpy(memberPointerBytes, &memberPointer, sizeof(memberPointerBytes));

	int offset = ResolveLookupHookOffset(hook.name.c_str(), object, memberPointerBytes, sizeof(memberPointerBytes));
	if(offset < 0){
		return;
	}

	hook.CreateHookInObjectVTable(object, offset, detour);
	IHook::Register(&hook);
}

/*
 * The desc observation and the handle grab, which is the whole of the local dimming hook now. The desc line is the
 * one the runs already read, with the instance it fired on added, because the capture opens the handle on a device
 * of its own and a run has to be able to tell which device the desc came from.
 *
 * The grab is after the call that made the resource, once for the first plausible texture and once for a later
 * desc that names a different texture, which is the render resolution case: a texture is created once, so this is
 * once per texture and not per frame work on the hook thread. The capture records the handle, the desc and the
 * adapter, and it does not create anything or sample here; on a change it releases what it had open and opens the
 * new resource on its own device, which is one device call for one texture. QueryInterface and GetDesc are not
 * entries this module detours, so nothing in the grab comes back into the hook. When no capture is registered yet
 * the texture is not dropped: the capture keeps it with a reference and records it when the capture registers, so
 * the order between this hook being installed and the plugin object being created does not decide which texture a
 * run reads.
 */
static HRESULT DetourCreateTexture2D(ID3D11Device* _this, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D){
	if(!IsLocalDimmingEnabled()){
		return CreateTexture2DHook.originalFunc(_this, pDesc, pInitialData, ppTexture2D);
	}

	// The flag is only set for a desc the helper accepts, so the line is the first plausible eye texture rather
	// than the first texture of any size. The helper takes the format the runs observed, one mip level, and the
	// bounds the layout needs, so neither a mipmapped resource nor a texture smaller than one pixel per zone
	// reports as a plausible eye image. This module creates buffers and views, not textures, so the first line a
	// run sees is a texture the compositor made.
	bool plausibleDesc = pDesc != nullptr &&
		LocalDimming::LooksLikeEyeTexture(static_cast<int>(pDesc->Width), static_cast<int>(pDesc->Height),
			static_cast<int>(pDesc->MipLevels), static_cast<int>(pDesc->Format));

	if(plausibleDesc && !createTexture2DFirstPlausibleDescLogged.exchange(true)){
		// MiscFlags is the field that decides whether the route this design uses is open at all. A composited
		// texture created shareable (D3D11_RESOURCE_MISC_SHARED 0x2, or the NT handle form 0x800) can be opened
		// on another device, which is what the local dimming module does on its own thread. A desc without those
		// flags cannot be opened anywhere else, and that is the outcome the grab line reports as a failure.
		LocalDimmingLog("ID3D11Device::CreateTexture2D detour first plausible desc, hooked at vtable offset %d, %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, device this %p, is the instance Initialize() hooked %s",
			CreateTexture2DHook.originalVTableOffset, static_cast<int>(pDesc->Width), static_cast<int>(pDesc->Height),
			static_cast<int>(pDesc->ArraySize), static_cast<int>(pDesc->BindFlags), static_cast<int>(pDesc->Usage),
			static_cast<int>(pDesc->Format), static_cast<int>(pDesc->MipLevels), static_cast<int>(pDesc->MiscFlags),
			static_cast<const void*>(_this), IsHookedInstance(CreateTexture2DHook.originalObject, _this) ? "yes" : "no");
	}

	HRESULT hr = CreateTexture2DHook.originalFunc(_this, pDesc, pInitialData, ppTexture2D);

	// The record is not gated on the first plausible desc the way the line is: the render resolution option can
	// change during a run, which makes the compositor create a new composited texture at a different size, so
	// every plausible desc is passed to the capture. The capture does nothing for a desc it has already recorded,
	// so this stays one event per texture rather than per frame work on the hook thread.
	if(plausibleDesc && ppTexture2D != nullptr && *ppTexture2D != nullptr){
		LocalDimmingCapture::RecordSharedTextureFromHook(*ppTexture2D);
	}

	return hr;
}

std::string ConvertWideToUtf8(const std::wstring& wstr){
	int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
	std::string str(count, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], (int)count, NULL, NULL);
	return str;
}

std::wstring ConvertUtf8ToWide(const std::string& str){
	int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0);
	std::wstring wstr(count, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &wstr[0], count);
	return wstr;
}


// convert sRGB into the section of the dci-p3 color space it occupies based on the display color points
static std::map<Config::HeadsetType, std::vector<double>> srgbColorCorrectionMatrices = {
	{Config::HeadsetType::MeganeX8K, {
		0.8224619687143621, 0.17753803128563772, 0.0, 
		0.033194198850961636, 0.9668058011490385, -1.3877787807814457e-17, 
		0.01708263072112004, 0.07239744066396342, 0.9105199286149167

	}},
	{Config::HeadsetType::DreamAir, {
		// convert sRGB into the section of the dci-p3 color space it occupies
		0.7304695448285958, 0.2440673763281347, 0.025463078843269038,
		0.03779320999796168, 0.9518287439140447, 0.010378046087993819,
		0.01344114195073836, 0.0475204153264534, 0.9390384427228086
	}},
	{Config::HeadsetType::DreamAirSE, {
		// convert sRGB into the section of the dci-p3 color space it occupies
		0.7788634159921426, 0.25247739019974275, -0.03379484171697117,
		0.0037476676008244884, 0.994944069950487, 0.0022145460147468266,
		0.012678379084786844, 0.049011831438184814, 0.9391692651968393
	}},
};


// corrects to the official white point but results in a warmer image
static std::map<Config::HeadsetType, std::vector<double>> srgbColorCorrectionWithWhiteMatrices = {
	{Config::HeadsetType::MeganeX8K, {
		// best guess for an official white point
		// the data sheet list it as a mixture of infrared and ultraviolet
		0.8224619687143621, 0.17753803128563772, 0.0, 
		0.033194198850961636, 0.9668058011490385, -1.3877787807814457e-17, 
		0.01708263072112004, 0.07239744066396342, 0.9105199286149167

	}},
	{Config::HeadsetType::DreamAir, {
		0.8086585139263373, 0.2701921842418791, 0.02818862968779734,
		0.037776947709785444, 0.9514191752817748, 0.010373580450483824,
		0.009672316745800479, 0.03419594187859908, 0.6757370235197377
	}},
	{Config::HeadsetType::DreamAirSE, {
		0.7807794765648906, 0.25309850291724767, -0.033877979482138554,
		0.0037442742266261438, 0.9940431849480164, 0.002212540825357545,
		0.012667491683254182, 0.048969743132956656, 0.9383627651837891
	}},
};


// Decode the original shader bytecode, read the source hlsl file,
// replace SHADER_VARIABLES with the decoded content, and write a .processed.hlsl file.
// Always writes the output file (even if blank) to prevent stale data from being used
// when the decoder fails on new bytecode. Returns the absolute path to the .processed.hlsl file.
static std::string DecodeShaderAndWriteProcessedFile(std::string sourceShaderName, std::string originalShaderName){
    
    // Create temp directory within the shader folder
    std::string shaderPath = getShaderPath();
    std::string tempDir = shaderPath + "temp/";
    if(!std::filesystem::exists(tempDir)){
        std::filesystem::create_directories(tempDir);
        DriverLog("Created temp directory: %s", tempDir.c_str());
    }
    
    // Create output path: temp/<original-shader-name>.processed.hlsl
    std::filesystem::path processedPath = std::filesystem::path(originalShaderName).replace_extension(".processed.hlsl");
    std::string processedFullPath = (std::filesystem::path(tempDir) / processedPath).string();
    
    // Read the source hlsl file
    std::string sourceFullPath = shaderPath + sourceShaderName;
    std::ifstream sourceFile(sourceFullPath, std::ios::binary);
    std::string sourceContent;
    if(!sourceFile){
        DriverLog("Failed to open source shader file: %s", sourceFullPath.c_str());
    }else{
        sourceContent.assign(std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>());
        sourceFile.close();
        // DriverLog("Read source shader file: %s (%zu bytes)", sourceFullPath.c_str(), sourceContent.length());
    }
    
    // Decode the original shader bytecode
    std::string hlsl;
    std::string bytecodeFullPath = driverConfigLoader.info.steamvrResources + "shaders/d3d11/" + originalShaderName;
    FILE* inFile = fopen(bytecodeFullPath.c_str(), "rb");
    if(!inFile){
        DriverLog("Failed to open original shader file: %s", bytecodeFullPath.c_str());
    }else{
        fseek(inFile, 0, SEEK_END);
        size_t bytecodeLength = ftell(inFile);
        fseek(inFile, 0, SEEK_SET);
        std::vector<uint8_t> bytecode(bytecodeLength);
        fread(bytecode.data(), 1, bytecodeLength, inFile);
        fclose(inFile);
        // DriverLog("Read original shader bytecode from %s (%zu bytes)", bytecodeFullPath.c_str(), bytecodeLength);
        
        // Decode shader variables
        hlsl = DecodeShaderVariables(bytecode.data(), static_cast<uint32_t>(bytecodeLength));
        if(hlsl.empty()){
            DriverLog("Failed to decode shader variables for %s", originalShaderName.c_str());
        }else{
            // Post-process decoded HLSL to fix invalid identifiers
            // Must replace <unnamed>::<unnamed> BEFORE <unnamed> to avoid partial matches
            std::string temp;
            size_t pos2;
            while((pos2 = hlsl.find("<unnamed>::<unnamed>")) != std::string::npos){
                hlsl.replace(pos2, 20, "UnnamedInnerStruct");
            }
            while((pos2 = hlsl.find("<unnamed>")) != std::string::npos){
                hlsl.replace(pos2, 9, "UnnamedStruct");
            }
            // Replace param1/2/3 with uv1/2/3 for TEXCOORD inputs
            while((pos2 = hlsl.find("param1 : TEXCOORD0")) != std::string::npos){
                hlsl.replace(pos2, 18, "uv1 : TEXCOORD0");
            }
            while((pos2 = hlsl.find("param2 : TEXCOORD1")) != std::string::npos){
                hlsl.replace(pos2, 18, "uv2 : TEXCOORD1");
            }
            while((pos2 = hlsl.find("param3 : TEXCOORD2")) != std::string::npos){
                hlsl.replace(pos2, 18, "uv3 : TEXCOORD2");
            }
            // Replace param2 : BLENDINDICES with param4 : BLENDINDICES
            while((pos2 = hlsl.find("uint param2 : BLENDINDICES;")) != std::string::npos){
                hlsl.replace(pos2, 27, "uint param4 : BLENDINDICES;");
            }
            // Replace g_tScene[N] with g_tSceneN
            while((pos2 = hlsl.find("g_tScene[0]")) != std::string::npos){
                hlsl.replace(pos2, 11, "g_tScene0");
            }
            while((pos2 = hlsl.find("g_tScene[1]")) != std::string::npos){
                hlsl.replace(pos2, 11, "g_tScene1");
            }
            while((pos2 = hlsl.find("g_tScene[2]")) != std::string::npos){
                hlsl.replace(pos2, 11, "g_tScene2");
            }
        }
    }
    
    // Replace SHADER_VARIABLES in source with decoded HLSL
    std::string processedContent = sourceContent;
    std::string shaderVariablesPlaceholder = "SHADER_VARIABLES";
    size_t pos = processedContent.find(shaderVariablesPlaceholder);
    if(pos != std::string::npos){
        processedContent.replace(pos, shaderVariablesPlaceholder.length(), hlsl);
    }else{
        // DriverLog("WARNING: SHADER_VARIABLES shaderVariablesPlaceholder not found in %s", sourceShaderName.c_str());
    }
    
    // Write processed file (always, even if empty or decode failed)
    FILE* outFile = fopen(processedFullPath.c_str(), "wb");
    if(!outFile){
        DriverLog("Failed to write processed shader file: %s", processedFullPath.c_str());
    }else{
        fwrite(processedContent.c_str(), 1, processedContent.length(), outFile);
        fclose(outFile);
        // DriverLog("Wrote processed shader to %s (%zu bytes)", processedFullPath.c_str(), processedContent.length());
    }
    
    return processedFullPath;
}

// compile the new distortion shader from source
// originalShaderName is the name of the original SteamVR shader file (e.g. "distort_ps_layered.fxo")
Bytecode DistortionShader(bool muraCorrection = false, bool noDistortion = false, const char* originalShaderName = "distort_ps_layered.fxo"){
	if(!IsCustomShaderEnabled()){
		// don't replace
		return {nullptr, 0};
	}
	
	std::string fullSourcePath = getShaderPath() + "distort_ps_layered.hlsl";
	
	// Decode the original shader bytecode, replace SHADER_VARIABLES, and write .processed.hlsl
	std::string fullProcessedPath = DecodeShaderAndWriteProcessedFile("distort_ps_layered.hlsl", originalShaderName);
	
	// create defines for shader settings
	D3D_SHADER_MACRO defines[60] = {};
	int definesCount = 0;
	
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::MeganeX8K){
		defines[definesCount++] = {"MEGANEX8K", "1"};
		if(driverConfig.customShader.subpixelShift && driverConfig.meganeX8K.subpixelShift != 0 && driverConfig.meganeX8K.resolutionY == 3552){
			// only do this if the subpixel shift is not zero and it is running at full resolution
			defines[definesCount++] = {"SUBPIXEL_SHIFT_MEGANEX8K", "1"};
		}
	}
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::Vive){
		defines[definesCount++] = {"VIVE", "1"};
		if(driverConfig.customShader.subpixelShift){
			defines[definesCount++] = {"SUBPIXEL_SHIFT_VIVE", "1"};
		}
	}
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::DreamAir || driverConfigLoader.info.connectedHeadset == Config::HeadsetType::CrystalSuperMicroOLED){
		defines[definesCount++] = {"DREAMAIR", "1"};
		if(driverConfig.customShader.subpixelShift && driverConfig.dreamAir.subpixelShift != 0 ){
			defines[definesCount++] = {"SUBPIXEL_SHIFT_DREAMAIR", "1"};
		}
	}
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::DreamAirSE){
		defines[definesCount++] = {"DREAMAIRSE", "1"};
		if(driverConfig.customShader.subpixelShift && driverConfig.dreamAirSE.subpixelShift != 0 ){
			defines[definesCount++] = {"SUBPIXEL_SHIFT_DREAMAIRSE", "1"};
		}
	}
	if(Config::IsPimaxHeadset(driverConfigLoader.info.connectedHeadset)){
		defines[definesCount++] = {"PIMAX", "1"};
	}
	std::string resolutionX = std::to_string(driverConfigLoader.info.outputResolutionX);
	std::string resolutionY = std::to_string(driverConfigLoader.info.outputResolutionY);
	if(driverConfigLoader.info.outputResolutionX && driverConfigLoader.info.outputResolutionY){
		// currently only the MeganeX8K can get the resolution to define these but it also the only one that uses it.
		defines[definesCount++] = {"OUTPUT_RESOLUTION_X", resolutionX.c_str()};
		defines[definesCount++] = {"OUTPUT_RESOLUTION_Y", resolutionY.c_str()};
	}
	double contrastMultiplier = driverConfig.customShader.contrast / 50.0;
	std::string contrastMultiplierString = std::to_string(contrastMultiplier);
	if(contrastMultiplier != 1){
		defines[definesCount++] = {"CONTRAST_MULTIPLIER", contrastMultiplierString.c_str()};
	}
	double contrastOffset = driverConfig.customShader.contrastMidpoint / 100.0;
	contrastOffset = -contrastOffset  * contrastMultiplier + contrastOffset;
	std::string contrastOffsetString = std::to_string(contrastOffset);
	if(contrastOffset != 0){
		defines[definesCount++] = {"CONTRAST_OFFSET", contrastOffsetString.c_str()};
	}
	if(driverConfig.customShader.contrastLinear){
		defines[definesCount++] = {"CONTRAST_LINEAR", "1"};
	}
	std::string contrastMultiplierLeft = "";
	std::string contrastOffsetLeft = "";
	std::string contrastMultiplierRight = "";
	std::string contrastOffsetRight = "";
	if(driverConfig.customShader.contrastPerEye){
		double contrastMultiplierLeftValue = driverConfig.customShader.contrastLeft / 50.0;
		double contrastOffsetLeftValue = driverConfig.customShader.contrastMidpointLeft / 100.0;
		contrastOffsetLeftValue = -contrastOffsetLeftValue  * contrastMultiplierLeftValue + contrastOffsetLeftValue;
		double contrastMultiplierRightValue = driverConfig.customShader.contrastRight / 50.0;
		double contrastOffsetRightValue = driverConfig.customShader.contrastMidpointRight / 100.0;
		contrastOffsetRightValue = -contrastOffsetRightValue  * contrastMultiplierRightValue + contrastOffsetRightValue;
		if(contrastMultiplierLeftValue != 1){
			contrastMultiplierLeft = std::to_string(contrastMultiplierLeftValue);
			defines[definesCount++] = {"CONTRAST_MULTIPLIER_LEFT", contrastMultiplierLeft.c_str()};
		}
		if(contrastOffsetLeftValue != 0){
			contrastOffsetLeft = std::to_string(contrastOffsetLeftValue);
			defines[definesCount++] = {"CONTRAST_OFFSET_LEFT", contrastOffsetLeft.c_str()};
		}
		if(contrastMultiplierRightValue != 1){
			contrastMultiplierRight = std::to_string(contrastMultiplierRightValue);
			defines[definesCount++] = {"CONTRAST_MULTIPLIER_RIGHT", contrastMultiplierRight.c_str()};
		}
		if(contrastOffsetRightValue != 0){
			contrastOffsetRight = std::to_string(contrastOffsetRightValue);
			defines[definesCount++] = {"CONTRAST_OFFSET_RIGHT", contrastOffsetRight.c_str()};
		}
		if(driverConfig.customShader.contrastPerEyeLinear){
			defines[definesCount++] = {"CONTRAST_PER_EYE_LINEAR", "1"};
		}
	}
	double saturation = driverConfig.customShader.saturation / 50.0;
	std::string saturationString = std::to_string(saturation);
	if(saturation != 1){
		defines[definesCount++] = {"SATURATION", saturationString.c_str()};
	}
	std::string gammaString = std::to_string(driverConfig.customShader.gamma);
	if(driverConfig.customShader.gamma != 2.2){
		defines[definesCount++] = {"GAMMA", gammaString.c_str()};
	}
	if(muraCorrection){
		defines[definesCount++] = {"MURA_CORRECTION", "1"};
		if(driverConfig.customShader.disableMuraCorrection){
			defines[definesCount++] = {"DISABLE_MURA_CORRECTION", "1"};
		}
	}
	if(driverConfig.customShader.disableBlackLevels){
		defines[definesCount++] = {"DISABLE_BLACK_LEVELS", "1"};
	}
	std::string colorMatrixString = "";
	if(driverConfig.customShader.srgbColorCorrection){
		std::vector<double>* colorMatrix = nullptr;
		if(driverConfig.customShader.srgbWhitePointCorrection && srgbColorCorrectionWithWhiteMatrices.find(driverConfigLoader.info.connectedHeadset) != srgbColorCorrectionWithWhiteMatrices.end()){
			colorMatrix = &srgbColorCorrectionWithWhiteMatrices[driverConfigLoader.info.connectedHeadset];
		}else{
			if(srgbColorCorrectionMatrices.find(driverConfigLoader.info.connectedHeadset) != srgbColorCorrectionMatrices.end()){
				colorMatrix = &srgbColorCorrectionMatrices[driverConfigLoader.info.connectedHeadset];
			}
		}
		if(driverConfig.customShader.srgbColorCorrectionMatrix.size() == 9){
			colorMatrix = &driverConfig.customShader.srgbColorCorrectionMatrix;
		}
		if(colorMatrix){
			for(int i = 0; i < 9; i++){
				colorMatrixString += std::to_string((*colorMatrix)[i]);
				if(i < 8){
					colorMatrixString += ", ";
				}
			}
			defines[definesCount++] = {"COLOR_CORRECTION_MATRIX", colorMatrixString.c_str()};
		}
	}
	if(driverConfig.customShader.lensColorCorrection){
		defines[definesCount++] = {"LENS_COLOR_CORRECTION", "1"};
	}
	if(driverConfig.customShader.dither10Bit){
		defines[definesCount++] = {"DITHER_10BIT", "1"};
	}
	if(!driverConfig.customShader.enableFilterForOverlay && !(driverConfig.customShader.enableFilterForDashboard && driverConfigLoader.info.isDashboardOpen)){
		defines[definesCount++] = {"NO_OVERLAY_FILTER", "1"};
	}
	std::string samplingFilterString = "FILTER_" + driverConfig.customShader.samplingFilter;
	std::transform(samplingFilterString.begin(), samplingFilterString.end(), samplingFilterString.begin(), ::toupper);
	defines[definesCount++] = {samplingFilterString.c_str(), "1"};
	std::string samplingFilterHorizontalPercent;
	std::string samplingFilterVerticalPercent;
	if(driverConfig.customShader.samplingFilter != "None" && (driverConfig.customShader.samplingFilterHorizontalPercent != 100.0 || driverConfig.customShader.samplingFilterVerticalPercent != 100.0)){
		samplingFilterHorizontalPercent = std::to_string(driverConfig.customShader.samplingFilterHorizontalPercent);
		samplingFilterVerticalPercent = std::to_string(driverConfig.customShader.samplingFilterVerticalPercent);
		defines[definesCount++] = {"FILTER_HORIZONTAL_PERCENT", samplingFilterHorizontalPercent.c_str()};
		defines[definesCount++] = {"FILTER_VERTICAL_PERCENT", samplingFilterVerticalPercent.c_str()};
	}
	std::string sharpStrengthString, sharpClampString, patternString, radiusString, contrastString;
	if(driverConfig.customShader.samplingFilter == "FXAA2" && driverConfig.customShader.samplingFilterFXAA2SharpenStrength != 0){
		sharpStrengthString = std::to_string(driverConfig.customShader.samplingFilterFXAA2SharpenStrength);
		defines[definesCount++] = {"sharp_strength", sharpStrengthString.c_str()};
		sharpClampString = std::to_string(driverConfig.customShader.samplingFilterFXAA2SharpenClamp);
		defines[definesCount++] = {"sharp_clamp", sharpClampString.c_str()};
	}
	if(driverConfig.customShader.samplingFilter == "FXAA2CAS" && driverConfig.customShader.samplingFilterFXAA2CASStrength != 0){
		sharpStrengthString = std::to_string(driverConfig.customShader.samplingFilterFXAA2CASStrength);
		defines[definesCount++] = {"CAS_SHARPENING", sharpStrengthString.c_str()};
		contrastString = std::to_string(driverConfig.customShader.samplingFilterFXAA2CASContrast);
		defines[definesCount++] = {"CAS_CONTRAST", contrastString.c_str()};
	}
	if(driverConfig.customShader.samplingFilter == "LumaSharpen"){
		sharpStrengthString = std::to_string(driverConfig.customShader.samplingFilterLumaSharpenStrength);
		defines[definesCount++] = {"sharp_strength", sharpStrengthString.c_str()};
		sharpClampString = std::to_string(driverConfig.customShader.samplingFilterLumaSharpenClamp);
		defines[definesCount++] = {"sharp_clamp", sharpClampString.c_str()};
		patternString = std::to_string(driverConfig.customShader.samplingFilterLumaSharpenPattern);
		defines[definesCount++] = {"pattern", patternString.c_str()};
		radiusString = std::to_string(driverConfig.customShader.samplingFilterLumaSharpenRadius);
		defines[definesCount++] = {"offset_bias", radiusString.c_str()};
	}
	if(driverConfig.customShader.samplingFilter == "CAS"){
		sharpStrengthString = std::to_string(driverConfig.customShader.samplingFilterCASStrength);
		defines[definesCount++] = {"CAS_SHARPENING", sharpStrengthString.c_str()};
		contrastString = std::to_string(driverConfig.customShader.samplingFilterCASContrast);
		defines[definesCount++] = {"CAS_CONTRAST", contrastString.c_str()};
	}
	std::string colorMultiplierString = "";
	if(driverConfig.customShader.colorMultiplier.r != 1.0 || driverConfig.customShader.colorMultiplier.g != 1.0 || driverConfig.customShader.colorMultiplier.b != 1.0) {
		colorMultiplierString = std::to_string(driverConfig.customShader.colorMultiplier.r) + ", " + std::to_string(driverConfig.customShader.colorMultiplier.g) + ", " + std::to_string(driverConfig.customShader.colorMultiplier.b);
		defines[definesCount++] = {"COLOR_MULTIPLIER", colorMultiplierString.c_str()};
	}
	if(noDistortion){
		defines[definesCount++] = {"NO_DISTORTION", "1"};
		defines[definesCount++] = {"NO_LAYER", "1"};
	}
	if(std::filesystem::exists(getShaderPath() + "distort_ps_layered_after_test.hlsl")){
		defines[definesCount++] = {"AFTER_TEST", "1"};
	}
	
	defines[definesCount++] = {nullptr, nullptr}; // end of array
	
	
	std::string errorPath = fullSourcePath + "_error" + (muraCorrection ? "_mura" : "") + (noDistortion ? "_nd" : "") + ".txt";
	
	// compile shader from hlsl using cached compilation
	ID3DBlob* errorBlob = nullptr;
	Bytecode bytecode = D3DCompileFromFileCached(
		ConvertUtf8ToWide(fullProcessedPath).c_str(),
		defines,
		"main",
		"ps_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		&errorBlob
	);
	if(bytecode.data == nullptr || bytecode.length == 0){
		DriverLog("Failed to compile shader file: %s", fullProcessedPath.c_str());
		if(errorBlob){
			DriverLog("Error: %s", (char*)errorBlob->GetBufferPointer());
			// output to file beside shader
			FILE* errorFile = fopen(errorPath.c_str(), "wb+");
			if(errorFile){
				fwrite(errorBlob->GetBufferPointer(), 1, errorBlob->GetBufferSize() - 1, errorFile);
				fclose(errorFile);
			}
			errorBlob->Release();
		}
		return {nullptr, 0};
	}else{
		DriverLog("Successfully compiled shader file: %s", fullProcessedPath.c_str());
		// delete error file
		remove(errorPath.c_str());
		return bytecode;
	}
}


// same as DistortionShader
Bytecode DistortionShaderPlain(){
	return DistortionShader(false, false, "distort_ps_layered.fxo");
}

// same as DistortionShader but with mura correction enabled
Bytecode DistortionShaderMuraCorrection(){
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::MeganeX8K){
		// don't compile for headsets that will not use it
		return {nullptr, 0};
	}
	return DistortionShader(true, false, "distort_ps_layered_mc.fxo");
}

// same as DistortionShader but with no distortion enabled
Bytecode DistortionShaderNoDistortion(){
	if(driverConfigLoader.info.connectedHeadset == Config::HeadsetType::MeganeX8K || driverConfigLoader.info.connectedHeadset == Config::HeadsetType::Vive){
		// don't compile for headsets that will not use it
		return {nullptr, 0};
	}
	return DistortionShader(false, true, "distort_ps_achromatic_nd.fxo");
}

// Precompile all registered shaders to populate the cache before they are needed in the hot path
void PrecompileShaders(){
	if(!IsCustomShaderEnabled()){
		return;
	}
	DriverLog("Precompiling all registered shaders...");
	// Iterate over all registered shader replacement functions and call them to populate the cache
	for(auto& pair : shaderReplacements){
		Bytecode bytecode = pair.second();
		// Free the returned bytecode since we only need the cache entry
		if(bytecode.data){
			delete[] bytecode.data;
			bytecode.data = nullptr;
		}
	}
	DriverLog("Precompiled all registered shaders, cache size: %zu", shaderCache.size());
}



// get first 32 bytes of an existing shader for identification
std::string GetExistingShaderIdentifier(std::string name){
	std::string fullPath = driverConfigLoader.info.steamvrResources + "shaders/d3d11/" + name;
	FILE* file = fopen(fullPath.c_str(), "rb");
	if(!file){
		DriverLog("Failed to open shader file: %s", fullPath.c_str());
		return "";
	}
	DriverLog("Reading shader file: %s", fullPath.c_str());
	char buffer[32];
	fread(buffer, 1, 32, file);
	fclose(file);
	// ID3DBlob* blob;
	// // std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	// // std::wstring shaderPath = converter.from_bytes(fullPath);
	// std::wstring shaderPath = std::wstring(fullPath.begin(), fullPath.end());
	// if(FAILED(D3DReadFileToBlob(shaderPath.c_str(), &blob))){
	// 	DriverLog("Failed to read shader file: %s", fullPath.c_str());
	// 	return "";
	// }
	// if(blob->GetBufferSize() < 32){
	// 	DriverLog("Shader file too small: %s", fullPath.c_str());
	// 	blob->Release();
	// 	return "";
	// }
	// char buffer[32];
	// memcpy(buffer, blob->GetBufferPointer(), 32);
	// blob->Release();
	return std::string(buffer, 32);
}

void ShaderReplacement::Initialize(){
	if(started){
		return;
	}
	started = true;
	DriverLog("ShaderReplacement::Initialize called");
	
	// get and shim ID3D11Device
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	HRESULT hr = D3D11CreateDevice(
		NULL,
		D3D_DRIVER_TYPE_HARDWARE,
		NULL,
		NULL,
		NULL,
		NULL,
		D3D11_SDK_VERSION,
		&device,
		NULL,
		&context
	);
	if(FAILED(hr)){
		DriverLog("ShaderReplacement::Initialize Failed to create D3D11 device and context");
		return;
	}else{
		DriverLog("ShaderReplacement::Initialize Successfully created D3D11 device and context");
	}
	// device->CreatePixelShader. This is the control: the hook is known to work in practice, so the line the
	// lookup writes for it says whether the lookup and the hook mechanism are sound, separately from the
	// capture hook.
	auto createPixelShaderPmf = &ID3D11Device::CreatePixelShader;
	unsigned char createPixelShaderBytes[sizeof(createPixelShaderPmf)];
	memcpy(createPixelShaderBytes, &createPixelShaderPmf, sizeof(createPixelShaderBytes));
	int createPixelShaderOffset = ResolveHookOffset("ID3D11Device::CreatePixelShader", device, createPixelShaderBytes, sizeof(createPixelShaderBytes), 15);
	if(createPixelShaderOffset >= 0){
		CreatePixelShaderHook.CreateHookInObjectVTable(device, createPixelShaderOffset, &DetourCreatePixelShader);
		IHook::Register(&CreatePixelShaderHook);
	}
	// }
	
	// Nothing is installed on the context any more, and this is the reason the capture moved off it. Runs 14 and
	// 15 showed that the compositor's context is not the object this module hooked: the draw and Flush detours
	// stayed silent while the device detours fired, and every interface version answered to the same array. The
	// capture reads the shared texture on the device its own module created, on the local dimming thread, so the
	// two copy detours, the four draw detours and the Flush detour are gone with this line. They are named here
	// because their silence is what the earlier runs were read against, and a run that does start routing the eye
	// image through a copy will show that in the desc line's bind flags, not in a first call line.

	// The local dimming hook that is left, installed only when local dimming is on, so a user who has the feature
	// off is not affected. The offset comes from the member pointer: for this one there is no recorded constant to
	// check an offset against, so the dispatch slot is the only source and an unresolved lookup means the hook is
	// not installed. There is no per interface version install, because run 15 showed one cumulative array.
	if(IsLocalDimmingEnabled()){
		// the template argument is written out because the hook's FuncType and the type of &Detour* deduce
		// separately, and MSVC will not pick between them
		InstallLookupHook<CreateTexture2DFunction>(CreateTexture2DHook, device, &ID3D11Device::CreateTexture2D, &DetourCreateTexture2D);
	}

	// The context this module created is not hooked and the capture does not use it, because it has its own device
	// on the same adapter, created on the local dimming thread. The line is here so a run can tell this object
	// from the one the capture creates, which is the difference the two sets of lines are read against.
	DriverLog("ShaderReplacement::Initialize context %p is not hooked; the local dimming capture creates its own device and context on the local dimming thread", static_cast<const void*>(context));
	
	// DriverLog("ShaderReplacement::Initialize CopySubresourceRegion function pointer %p %p", (void (ID3D11DeviceContext::*)())&ID3D11DeviceContext::CopySubresourceRegion, CopySubresourceRegionHook.originalFunc);
	
	// // There are multiple ID3D11DeviceContexts, so hook the function that it points to
	// auto err = MH_Initialize();
	// if(err == MH_OK){
	// 	if(MH_CreateHook(CopySubresourceRegionHook.originalFunc, &DetourCopySubresourceRegion, reinterpret_cast<LPVOID*>(&originalCopySubresourceRegion)) == MH_OK){
	// 		if(MH_EnableHook(CopySubresourceRegionHook.originalFunc) == MH_OK){
	// 			DriverLog("ShaderReplacement::Initialize successfully enabled deep CopySubresourceRegion hook");
	// 		}
	// 	}
	// }
	
	
	DriverLog("ShaderReplacement::Initialize loading shader replacement table");
	// LogShaderIdentifier(GetExistingShaderIdentifier("distort_ps_layered.fxo"), 32);
	shaderReplacements[GetExistingShaderIdentifier("distort_ps_layered.fxo")] = DistortionShaderPlain;
	shaderReplacements[GetExistingShaderIdentifier("distort_ps_layered_mc.fxo")] = DistortionShaderMuraCorrection;
	// this is for headsets with custom compositors
	// this only works for games when there is an overlay open, otherwise there the textures never seem to pass through a shader
	// a transparent overlay could be opened to force this
	shaderReplacements[GetExistingShaderIdentifier("distort_ps_achromatic_nd.fxo")] = DistortionShaderNoDistortion;
	
	#ifdef _WIN32
		// startup winsock for websocket connections
		WSADATA wsaData;
		int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if(wsaStartupResult){
			printf("WSAStartup Failed %i.\n", wsaStartupResult);
		}
	#endif
	
	// start threads
	
	std::thread watchShadersThread(&ShaderReplacement::WatchShadersThread, this);
	watchShadersThread.detach();
	
	std::thread checkSettingsThread(&ShaderReplacement::CheckSettingsThread, this);
	checkSettingsThread.detach();
	
	
	// reloading shaders immediately does not seem to work so try a few times with delays
	std::this_thread::sleep_for(std::chrono::seconds(5));
	if(driverConfig.customShader.enable){
		ReloadShaders();
	}
	std::this_thread::sleep_for(std::chrono::seconds(5));
	if(driverConfig.customShader.enable){
		ReloadShaders();
	}
	std::this_thread::sleep_for(std::chrono::seconds(5));
	if(driverConfig.customShader.enable){
		ReloadShaders();
	}
}



void ShaderReplacement::WatchShadersThread(){
	std::string shaderPath = getShaderPath();
	HANDLE hDir = CreateFileA(shaderPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if(hDir == INVALID_HANDLE_VALUE){
		DriverLog("Failed to open distortion shaders for watching: %d", GetLastError());
		return;
	}
	while(started){
		DWORD bytesReturned;
		char buffer[1024] = {0};
		FILE_NOTIFY_INFORMATION* pNotify;
		BOOL success = ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &bytesReturned, NULL, NULL);
		if(!success){
			DriverLog("Failed to read directory changes: %d", GetLastError());
			break;
		}
		pNotify = (FILE_NOTIFY_INFORMATION*)buffer;
		do{
			std::wstring fileName(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
			if(fileName.size() >= 5 && fileName.substr(fileName.size() - 5) == L".hlsl" && (pNotify->Action == FILE_ACTION_MODIFIED || pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME)){
				DriverLog("Shader changed, reloading... %ls %i", fileName.c_str(), pNotify->Action);
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				ReloadShaders();
				break;
			}
			pNotify = (FILE_NOTIFY_INFORMATION*)((char*)pNotify + pNotify->NextEntryOffset);
		}while(pNotify->NextEntryOffset != 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
}

#elif __linux__
void ShaderReplacement::Initialize(){
}
#endif



void ShaderReplacement::ReloadShaders(){
	// Precompile all shaders before sending the reload signal to populate the cache
	#ifdef _WIN32
	PrecompileShaders();
	#endif
	
	DriverLog("ShaderReplacement::ReloadShaders called");
	easywsclient::WebSocket* websocket = easywsclient::WebSocket::from_url("ws://127.0.0.1:27062/", "http://127.0.0.1:27062");
	if(websocket == nullptr){
		DriverLog("Failed to create websocket");
		return;
	}
	websocket->send("mailbox_send vrcompositor_mailbox {\"type\":\"shaders_force_reload\"}");
	websocket->poll();
	websocket->close();
	websocket->poll(); // final poll to close the connection
	delete websocket;
}

void ShaderReplacement::CheckSettingsThread(){
	while(started){
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		{
			std::lock_guard<std::mutex> lock(driverConfigLock);
			if(!driverConfig.hasBeenUpdated && !driverConfigLoader.info.hasBeenUpdated){
				continue;
			}
			bool reloadShaders = false;
			bool isNowEnabled = IsCustomShaderEnabled();
			reloadShaders |= isNowEnabled != enabled;
			enabled = isNowEnabled;
			if(isNowEnabled && driverConfig.hasBeenUpdated){
				reloadShaders |= driverConfig.meganeX8K.subpixelShift != driverConfigOld.meganeX8K.subpixelShift;
				reloadShaders |= driverConfig.dreamAir.subpixelShift != driverConfigOld.dreamAir.subpixelShift;
				reloadShaders |= driverConfig.customShader.enable != driverConfigOld.customShader.enable;
				reloadShaders |= driverConfig.customShader.contrast != driverConfigOld.customShader.contrast;
				reloadShaders |= driverConfig.customShader.contrastMidpoint != driverConfigOld.customShader.contrastMidpoint;
				reloadShaders |= driverConfig.customShader.contrastLinear != driverConfigOld.customShader.contrastLinear;
				reloadShaders |= driverConfig.customShader.contrastPerEye != driverConfigOld.customShader.contrastPerEye;
				reloadShaders |= driverConfig.customShader.contrastPerEyeLinear != driverConfigOld.customShader.contrastPerEyeLinear;
				reloadShaders |= driverConfig.customShader.contrastLeft != driverConfigOld.customShader.contrastLeft;
				reloadShaders |= driverConfig.customShader.contrastMidpointLeft != driverConfigOld.customShader.contrastMidpointLeft;
				reloadShaders |= driverConfig.customShader.contrastRight != driverConfigOld.customShader.contrastRight;
				reloadShaders |= driverConfig.customShader.contrastMidpointRight != driverConfigOld.customShader.contrastMidpointRight;
				reloadShaders |= driverConfig.customShader.saturation != driverConfigOld.customShader.saturation;
				reloadShaders |= driverConfig.customShader.gamma != driverConfigOld.customShader.gamma;
				reloadShaders |= driverConfig.customShader.subpixelShift != driverConfigOld.customShader.subpixelShift;
				reloadShaders |= driverConfig.customShader.disableMuraCorrection != driverConfigOld.customShader.disableMuraCorrection;
				reloadShaders |= driverConfig.customShader.disableBlackLevels != driverConfigOld.customShader.disableBlackLevels;
				reloadShaders |= driverConfig.customShader.srgbColorCorrection != driverConfigOld.customShader.srgbColorCorrection;
				reloadShaders |= driverConfig.customShader.srgbWhitePointCorrection != driverConfigOld.customShader.srgbWhitePointCorrection;
				reloadShaders |= driverConfig.customShader.srgbColorCorrectionMatrix.size() != driverConfigOld.customShader.srgbColorCorrectionMatrix.size();
				reloadShaders |= driverConfig.customShader.lensColorCorrection != driverConfigOld.customShader.lensColorCorrection;
				reloadShaders |= driverConfig.customShader.dither10Bit != driverConfigOld.customShader.dither10Bit;
				reloadShaders |= driverConfig.customShader.enableFilterForOverlay != driverConfigOld.customShader.enableFilterForOverlay;
				reloadShaders |= driverConfig.customShader.enableFilterForDashboard != driverConfigOld.customShader.enableFilterForDashboard;
				reloadShaders |= driverConfig.customShader.samplingFilter != driverConfigOld.customShader.samplingFilter;
				reloadShaders |= driverConfig.customShader.samplingFilterFXAA2SharpenStrength != driverConfigOld.customShader.samplingFilterFXAA2SharpenStrength;
				reloadShaders |= driverConfig.customShader.samplingFilterFXAA2SharpenClamp != driverConfigOld.customShader.samplingFilterFXAA2SharpenClamp;
				reloadShaders |= driverConfig.customShader.samplingFilterFXAA2CASStrength != driverConfigOld.customShader.samplingFilterFXAA2CASStrength;
				reloadShaders |= driverConfig.customShader.samplingFilterFXAA2CASContrast != driverConfigOld.customShader.samplingFilterFXAA2CASContrast;
				reloadShaders |= driverConfig.customShader.samplingFilterLumaSharpenStrength != driverConfigOld.customShader.samplingFilterLumaSharpenStrength;
				reloadShaders |= driverConfig.customShader.samplingFilterLumaSharpenClamp != driverConfigOld.customShader.samplingFilterLumaSharpenClamp;
				reloadShaders |= driverConfig.customShader.samplingFilterLumaSharpenPattern != driverConfigOld.customShader.samplingFilterLumaSharpenPattern;
				reloadShaders |= driverConfig.customShader.samplingFilterLumaSharpenRadius != driverConfigOld.customShader.samplingFilterLumaSharpenRadius;
				reloadShaders |= driverConfig.customShader.samplingFilterCASStrength != driverConfigOld.customShader.samplingFilterCASStrength;
				reloadShaders |= driverConfig.customShader.samplingFilterCASContrast != driverConfigOld.customShader.samplingFilterCASContrast;
				reloadShaders |= driverConfig.customShader.samplingFilterHorizontalPercent != driverConfigOld.customShader.samplingFilterHorizontalPercent;
				reloadShaders |= driverConfig.customShader.samplingFilterVerticalPercent != driverConfigOld.customShader.samplingFilterVerticalPercent;
				reloadShaders |= driverConfig.customShader.colorMultiplier.r != driverConfigOld.customShader.colorMultiplier.r || 
					driverConfig.customShader.colorMultiplier.g != driverConfigOld.customShader.colorMultiplier.g || 
					driverConfig.customShader.colorMultiplier.b != driverConfigOld.customShader.colorMultiplier.b;
				if(driverConfig.customShader.srgbColorCorrectionMatrix.size() == 9 && driverConfigOld.customShader.srgbColorCorrectionMatrix.size() == 9){
					for(int i = 0; i < 9; i++){
						reloadShaders |= driverConfig.customShader.srgbColorCorrectionMatrix[i] != driverConfigOld.customShader.srgbColorCorrectionMatrix[i];
					}
				}
			}
			
			
			if(isNowEnabled && driverConfigLoader.info.hasBeenUpdated){
				reloadShaders |= driverConfigLoader.info.connectedHeadset != lastConnectedHeadset;
				lastConnectedHeadset = driverConfigLoader.info.connectedHeadset;
				// reload shaders when dashboard state changes and dashboard filter is enabled
				if(driverConfig.customShader.enableFilterForDashboard && !driverConfig.customShader.enableFilterForOverlay && driverConfig.customShader.samplingFilter != "None"){
					bool dashboardStateChanged = driverConfigLoader.info.isDashboardOpen != lastDashboardOpenState;
					lastDashboardOpenState = driverConfigLoader.info.isDashboardOpen;
					if(dashboardStateChanged){
						reloadShaders = true;
					}
				}
			}
			
			
			
			if(reloadShaders){
				DriverLog("Shader settings changed, reloading...");
				ReloadShaders();
			}
			
			
			driverConfig.hasBeenUpdated = false;
			driverConfigLoader.info.hasBeenUpdated = false;
		}
	}
}
