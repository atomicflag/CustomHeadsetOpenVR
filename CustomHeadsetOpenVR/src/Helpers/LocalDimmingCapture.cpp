#include "LocalDimmingCapture.h"
#include "../Config/ConfigLoader.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

// Every local dimming line is appended to this file, once per event, behind one mutex. The file is opened per
// line rather than kept open, because a run is a handful of lines and a line that is not flushed is not
// evidence.
static std::mutex localDimmingLogLock;

void LocalDimmingLog(const char* format, ...){
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	std::lock_guard<std::mutex> lock(localDimmingLogLock);

	std::ofstream file(driverConfigLoader.GetConfigFolder() + "localdimming.log", std::ios::app);
	if(!file.is_open()){
		return;
	}
	file << buf << "\n";
	file.flush();
}

#ifdef _WIN32
#include <d3dcompiler.h>
// The capture creates its own device, so this is the first place in the DLL that calls D3D11CreateDevice. The
// pragma is written here rather than relying on ShaderReplacement.cpp having put d3d11.lib on the link line.
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")

namespace LocalDimmingCapture{

// The capture the hook records the handle for. Written by the object that owns the runner for the life of the
// process and read by the hook without a lock.
static std::atomic<SharedTextureCapture*> registeredCapture{ nullptr };

// The first plausible texture the hook saw while nothing was registered, kept for the capture that registers
// later. The hook is installed by ShaderReplacement before the object that owns the capture is created, so the
// composited image can arrive first. Keeping the texture and not only its desc is the whole point: the reference
// taken here is what keeps it alive until the replay, so what is recorded is the texture the hook actually saw.
// The first plausible desc is the one kept, because a second one is not kept: by the time it arrives the capture
// decides which texture it reads.
static std::mutex pendingTextureLock;
static ID3D11Texture2D* pendingTexture = nullptr;

// The compiler answers with a multi line message, and the log is read one line at a time, so a message is put on
// one line before it is recorded. A long message is still cut by the log line's buffer, which is why the message
// is the last part of the line.
static std::string OneLine(const std::string& text){
	std::string out;
	out.reserve(text.size());
	for(char c : text){
		if(c == '\n' || c == '\r'){
			c = ' ';
		}
		out.push_back(c);
	}
	while(!out.empty() && out.back() == ' '){
		out.pop_back();
	}
	return out;
}

void RegisterCapture(SharedTextureCapture* capture){
	registeredCapture.store(capture, std::memory_order_relaxed);

	if(capture == nullptr){
		return;
	}

	// The desc is read from the pending texture here, because it holds a reference and GetDesc is not an entry this
	// module detours. The replay goes through the same record path a desc that arrives after registration uses.
	ID3D11Texture2D* pending = nullptr;
	{
		std::lock_guard<std::mutex> lock(pendingTextureLock);
		if(pendingTexture != nullptr){
			pending = pendingTexture;
			pendingTexture = nullptr;

			D3D11_TEXTURE2D_DESC desc = {};
			pending->GetDesc(&desc);
			LocalDimmingLog(
				"LocalDimmingCapture: a plausible desc was seen before any capture was registered, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, and that texture was kept and replayed for the capture that registered, through the same record path a desc that arrives after registration uses",
				static_cast<int>(desc.Width), static_cast<int>(desc.Height), static_cast<int>(desc.ArraySize),
				static_cast<int>(desc.BindFlags), static_cast<int>(desc.Usage), static_cast<int>(desc.Format),
				static_cast<int>(desc.MipLevels), static_cast<int>(desc.MiscFlags));
		}
	}

	if(pending != nullptr){
		capture->RecordTexture(pending);
		pending->Release();
	}
}

bool RecordSharedTextureFromHook(ID3D11Texture2D* texture){
	SharedTextureCapture* capture = registeredCapture.load(std::memory_order_relaxed);
	if(texture == nullptr){
		return false;
	}

	if(capture != nullptr){
		capture->RecordTexture(texture);
		return true;
	}

	// The hook is installed for the feature, but the object that owns the capture is not there yet. The desc line
	// the hook writes still stands on its own, and the texture is kept for the capture that registers, so the run
	// records the texture that arrived first rather than the next plausible one. Returns false because nothing was
	// recorded for this call; the record happens at the replay.
	std::lock_guard<std::mutex> lock(pendingTextureLock);
	if(pendingTexture == nullptr){
		pendingTexture = texture;
		pendingTexture->AddRef();
	}

	return false;
}

/*
 The compute source is the pure core's: BlockReduceSource assembles the defines it generates from its own weight
 constants with the body it holds, so the input a run compiles is the input the tests check.
*/
SharedTextureCapture::SharedTextureCapture(){
	// The hook records the handle for whichever capture is registered. Registering here rather than in the code
	// that owns the runner keeps the registration and the unregistration in the same object, so a run that
	// rebuilds the capture does not leave a pointer to an object that is gone.
	RegisterCapture(this);
}

SharedTextureCapture::~SharedTextureCapture(){
	if(registeredCapture.load(std::memory_order_relaxed) == this){
		RegisterCapture(nullptr);
	}

	// The opened texture and each eye's view set go through the same release the change path uses. The capture is
	// no longer registered at this point, so no hook call can reach the object and the release does not have to be
	// inside the lock.
	ReleaseOpenedResources();

	for(int eye = 0; eye < LocalDimming::EyeCount; eye++){
		EyeState& state = eyes[eye];
		if(state.staging != nullptr){
			state.staging->Release();
		}
		if(state.gridUav != nullptr){
			state.gridUav->Release();
		}
		if(state.gridBuffer != nullptr){
			state.gridBuffer->Release();
		}
		if(state.constants != nullptr){
			state.constants->Release();
		}
	}

	if(shader != nullptr){
		shader->Release();
	}
	if(context != nullptr){
		context->Release();
	}
	if(device != nullptr){
		device->Release();
	}
	// the reference QueryInterface made for the adapter is kept for the life of the capture, because the device
	// is created on it
	if(adapter != nullptr){
		adapter->Release();
	}
}

// Called with stateLock held, or by the destructor. The opened texture and each eye's view set belong to the
// resource the capture has open, so when that resource is a different one they cannot be used for it: they are
// released here and the sample path recreates them, which is what it does when it finds them null. The buffers are
// left as they are, because their size depends only on the grid and not on the texture.
void SharedTextureCapture::ReleaseOpenedResources(){
	if(openedTexture != nullptr){
		openedTexture->Release();
		openedTexture = nullptr;
	}

	for(int eye = 0; eye < LocalDimming::EyeCount; eye++){
		EyeState& state = eyes[eye];
		if(state.sourceUav != nullptr){
			state.sourceUav->Release();
			state.sourceUav = nullptr;
		}
	}
}

// Called with stateLock held. The two routes for a shared handle, in the same words the first grab line uses, so
// a re-grab for a new texture answers through the same route and a run reads the same thing for both.
//
// IDXGIResource1 is the route for feature level 11_1 and above, and a resource below that does not answer to it,
// so the fallback is IDXGIResource, which is the route for 11_0. Which one answered is part of the line, because a
// handle grabbed through the wrong route for the feature level is a different outcome from a resource that was not
// created shareable. Both answer with a result and an out parameter, so a null handle with a failing result is the
// failure this line reports.
//
// The adapter the resource's device is on is recorded here so the device the capture creates is on the same one.
// A shared handle names memory on an adapter, and a device on a different adapter cannot open it, so this is not a
// nicety: without it the open can fail on a machine with more than one adapter. QueryInterface adds the reference
// this capture keeps for its life, and QueryInterface is not an entry the hook detours, so nothing here comes back
// into the hook. GetDevice has no result to check in the SDK headers, so the out pointer is the only thing that
// says whether it answered. The adapter is recorded on the first grab and kept, because a texture made later by
// the same device is on the same adapter and the reference this capture holds is the one its device was created on.
uint64_t SharedTextureCapture::GrabSharedHandle(ID3D11Texture2D* texture, std::string& route){
	uint64_t handle = 0;

	IDXGIResource1* resource1 = nullptr;
	if(SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource1)))){
		HANDLE sharedHandle = nullptr;
		HRESULT hr = resource1->GetSharedHandle(&sharedHandle);
		handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
		route = "IDXGIResource1 answered, which is the route for feature level 11_1 and above, result " +
			std::to_string(static_cast<unsigned int>(hr));
		resource1->Release();
	}else{
		IDXGIResource* dxgiResource = nullptr;
		if(SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiResource)))){
			HANDLE sharedHandle = nullptr;
			HRESULT hr = dxgiResource->GetSharedHandle(&sharedHandle);
			handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
			route = "IDXGIResource1 did not answer, so the fallback for 11_0, IDXGIResource, was used, result " +
				std::to_string(static_cast<unsigned int>(hr));
			dxgiResource->Release();
		}else{
			route = "neither IDXGIResource1 nor IDXGIResource answered, so there is no handle route for this resource";
		}
	}

	if(adapter == nullptr){
		ID3D11Device* resourceDevice = nullptr;
		texture->GetDevice(&resourceDevice);
		if(resourceDevice != nullptr){
			IDXGIDevice* dxgiDevice = nullptr;
			if(SUCCEEDED(resourceDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))){
				IDXGIAdapter* foundAdapter = nullptr;
				if(SUCCEEDED(dxgiDevice->GetAdapter(&foundAdapter)) && foundAdapter != nullptr){
					adapter = foundAdapter;
				}
				dxgiDevice->Release();
			}
			resourceDevice->Release();
		}
	}

	return handle;
}

void SharedTextureCapture::RecordTexture(ID3D11Texture2D* texture){
	if(texture == nullptr){
		return;
	}

	{
		std::lock_guard<std::mutex> lock(stateLock);

		D3D11_TEXTURE2D_DESC desc = {};
		texture->GetDesc(&desc);

		int width = static_cast<int>(desc.Width);
		int height = static_cast<int>(desc.Height);
		int arraySize = static_cast<int>(desc.ArraySize);

		bool firstTexture = !grabAttempted;

		if(!firstTexture && width == recorded.width && height == recorded.height){
			// once per texture, and the hook calls this for a texture it created, so a second call for the
			// texture that was already recorded does nothing. A texture is created once, so this is not per
			// frame work on the hook thread.
			return;
		}

		int oldWidth = recorded.width;
		int oldHeight = recorded.height;

		grabAttempted = true;

		recorded.width = width;
		recorded.height = height;
		recorded.arraySize = arraySize;
		recorded.bindFlags = static_cast<int>(desc.BindFlags);
		recorded.usage = static_cast<int>(desc.Usage);
		recorded.format = static_cast<int>(desc.Format);
		recorded.mips = static_cast<int>(desc.MipLevels);
		recorded.miscFlags = static_cast<int>(desc.MiscFlags);

		std::string route;
		uint64_t handle = GrabSharedHandle(texture, route);

		// The handle replaces the one recorded for the texture that was: the old handle names the old resource,
		// which the compositor can have released, so holding on to it after a change is the staleness this
		// decision exists to remove.
		recorded.handle = handle;
		recorded.haveHandle = handle != 0;

		if(firstTexture){
			if(recorded.haveHandle){
				LocalDimmingLog(
					"LocalDimmingCapture: shared handle grabbed for the first plausible texture, handle %llu, %s, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, read through the unordered access view on the shared texture, adapter %p recorded for this module's own device",
					static_cast<unsigned long long>(recorded.handle), route.c_str(), recorded.width, recorded.height,
					recorded.arraySize, recorded.bindFlags, recorded.usage, recorded.format, recorded.mips,
					recorded.miscFlags, static_cast<const void*>(adapter));
			}else{
				// A plain failure, stated as one line. The most likely reason is misc flags without the shared
				// bit, which is a texture that cannot be opened anywhere else.
				LocalDimmingLog(
					"LocalDimmingCapture: shared handle grab failed, %s, handle 0, desc %dx%d, array size %d, bind flags %d, misc flags %d, so the capture has nothing to open on its own device and nothing can be read back",
					route.c_str(), recorded.width, recorded.height, recorded.arraySize, recorded.bindFlags, recorded.miscFlags);
			}
		}else{
			// One line for the change, which is the only thing a run can read for a render resolution change:
			// the size the recorded texture had and the size it has now.
			LocalDimmingLog(
				"LocalDimmingCapture: the texture the capture reads changed, desc %dx%d became %dx%d, handle %llu, %s, so the opened texture and the per-eye views are released and recreated for the new resource",
				oldWidth, oldHeight, width, height,
				static_cast<unsigned long long>(handle), route.c_str());
		}

		if(!firstTexture){
			// The view set that points at the old resource cannot be used for the new one, so the opened texture and
			// each eye's view set are released here and the sample path recreates them when they are null. The open
			// goes through the route the sample path already has, on this module's own device, and only when the
			// device exists: it is created on the local dimming thread, not on the hook thread.
			ReleaseOpenedResources();
			if(recorded.haveHandle && device != nullptr){
				OpenSharedTexture();
			}
		}
	}
}

// Called with stateLock held. The device is created once, on the local dimming thread, on the adapter the hook
// recorded. When the hook recorded no adapter, the default adapter is used and the line says so, because that is
// a different outcome from a failed open on a machine with more than one adapter.
bool SharedTextureCapture::EnsureDevice(){
	if(device != nullptr){
		return true;
	}

	D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
	// With no adapter the driver type has to be the hardware default, because UNKNOWN plus a null adapter is not
	// a device this call can create.
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, &chosen, &context);

	if(FAILED(hr) || device == nullptr || context == nullptr){
		if(!loggedDevice){
			loggedDevice = true;
			LocalDimmingLog(
				"LocalDimmingCapture: could not create this module's own device, adapter %s, result %08x, so nothing can be opened and nothing can be read back",
				adapter != nullptr ? "recorded by the hook" : "the default, because the hook recorded no adapter",
				static_cast<unsigned int>(hr));
		}
		return false;
	}

	if(!loggedDevice){
		loggedDevice = true;
		LocalDimmingLog(
			"LocalDimmingCapture: this module's own device, device %p, context %p, feature level %d, created on the local dimming thread, used only on that thread, no entry on it is hooked, adapter %s",
			static_cast<const void*>(device), static_cast<const void*>(context), static_cast<int>(chosen),
			adapter != nullptr ? "the one the hook recorded" : "the default, because the hook recorded no adapter");
	}

	return true;
}

// Called with stateLock held. The handle is opened once for each texture the capture records, because the texture
// is one texture read at two regions or two slices, and it is opened again after a change releases it. The desc
// the opened texture reports has to agree with the desc the hook recorded, and whether it does is part of the
// line: an opened resource that reports a different desc is not the same resource, and a run could not tell that
// apart without this.
bool SharedTextureCapture::OpenSharedTexture(){
	if(openedTexture != nullptr){
		return true;
	}

	if(!recorded.haveHandle){
		return false;
	}

	ID3D11Texture2D* opened = nullptr;
	HRESULT hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(recorded.handle), __uuidof(ID3D11Texture2D),
		reinterpret_cast<void**>(&opened));

	if(FAILED(hr) || opened == nullptr){
		// Stated plainly, once. The most likely reasons are a handle that was not grabbed and a device on a
		// different adapter, and both are named by the lines that come before this one.
		if(!loggedOpenFailed){
			loggedOpenFailed = true;
			LocalDimmingLog(
				"LocalDimmingCapture: OpenSharedResource failed for handle %llu on this module's own device, result %08x, so nothing can be read back",
				static_cast<unsigned long long>(recorded.handle), static_cast<unsigned int>(hr));
		}
		return false;
	}

	openedTexture = opened;

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	bool matches = static_cast<int>(desc.Width) == recorded.width &&
		static_cast<int>(desc.Height) == recorded.height &&
		static_cast<int>(desc.ArraySize) == recorded.arraySize &&
		static_cast<int>(desc.BindFlags) == recorded.bindFlags &&
		static_cast<int>(desc.Format) == recorded.format &&
		static_cast<int>(desc.MipLevels) == recorded.mips;

	if(!loggedOpenDesc){
		loggedOpenDesc = true;
		LocalDimmingLog(
			"LocalDimmingCapture: the shared texture opened on this module's own device, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, agrees with the desc the hook recorded %s, read through the unordered access view on the shared texture",
			static_cast<int>(desc.Width), static_cast<int>(desc.Height), static_cast<int>(desc.ArraySize),
			static_cast<int>(desc.BindFlags), static_cast<int>(desc.Usage), static_cast<int>(desc.Format),
			static_cast<int>(desc.MipLevels), static_cast<int>(desc.MiscFlags), matches ? "yes" : "no");
	}

	return true;
}

// Called with stateLock held. The view, the shader and the buffers are created once per eye on this module's own
// device, and the view set is created again after a change releases it, which is the path that rebuilds it for
// the new resource. The read path is the unordered access view on the opened texture, which is the one the runs
// observed. The format is not checked here: the hook only records a desc that is the eye texture, and the format
// is part of what decides that.
bool SharedTextureCapture::EnsureResources(int eye){
	EyeState& state = eyes[eye];

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	if(state.sourceUav == nullptr){
		// A view on a typeless resource has to name a member of its own family with the same bits per component,
		// which is the integer member the HLSL body reads as uint4.
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = static_cast<DXGI_FORMAT>(LocalDimming::ViewFormat);
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.MipSlice = 0;
		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = desc.ArraySize;
		HRESULT hr = device->CreateUnorderedAccessView(openedTexture, &uavDesc, &state.sourceUav);
		if(FAILED(hr) || state.sourceUav == nullptr){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				LocalDimmingLog(
					"LocalDimmingCapture eye %d: CreateUnorderedAccessView on the opened texture failed, format %d, result %08x, so nothing can be read back",
					eye, LocalDimming::ViewFormat, static_cast<unsigned int>(hr));
			}
			return false;
		}
	}

	if(shader == nullptr){
		// The whole input, from the pure core, so the source a run compiles is the source the tests hold.
		std::string source = LocalDimming::BlockReduceSource();

		ID3DBlob* bytecode = nullptr;
		ID3DBlob* compileError = nullptr;
		// A source name so the compiler's own message names this kernel instead of an anonymous shader.
		HRESULT hr = D3DCompile(source.c_str(), source.size(), "blockreduce", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &compileError);
		if(FAILED(hr) || bytecode == nullptr){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				std::string message = compileError != nullptr ? OneLine((const char*)compileError->GetBufferPointer()) : "no message";
				LocalDimmingLog(
					"LocalDimmingCapture eye %d: D3DCompile of the block reduce failed, result %08x, message %s, so nothing can be read back",
					eye, static_cast<unsigned int>(hr), message.c_str());
			}
		}else{
			HRESULT shaderResult = device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader);
			if(FAILED(shaderResult)){
				if(!state.loggedFailure){
					state.loggedFailure = true;
					LocalDimmingLog(
						"LocalDimmingCapture eye %d: D3DCompile of the block reduce answered with bytecode and CreateComputeShader failed on it, result %08x, so nothing can be read back",
						eye, static_cast<unsigned int>(shaderResult));
				}
			}
		}
		if(bytecode != nullptr){
			bytecode->Release();
		}
		if(compileError != nullptr){
			compileError->Release();
		}

		if(shader == nullptr){
			return false;
		}
	}

	int gridSizeSlots = LocalDimming::ZonesPerEye * LocalDimming::EyeCount;   // the whole grid, in ZoneIndex order

	if(state.constants == nullptr){
		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = static_cast<UINT>(sizeof(SampleConstants));
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if(FAILED(device->CreateBuffer(&cbDesc, nullptr, &state.constants))){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the constant buffer, so nothing can be read back", eye);
			}
			return false;
		}
	}

	if(state.gridBuffer == nullptr){
		D3D11_BUFFER_DESC bufferDesc = {};
		bufferDesc.ByteWidth = static_cast<UINT>(gridSizeSlots) * 4;
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		if(FAILED(device->CreateBuffer(&bufferDesc, nullptr, &state.gridBuffer))){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the grid buffer, so nothing can be read back", eye);
			}
			return false;
		}
	}

	if(state.gridUav == nullptr){
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32_UINT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = static_cast<UINT>(gridSizeSlots);
		if(FAILED(device->CreateUnorderedAccessView(state.gridBuffer, &uavDesc, &state.gridUav))){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the grid view, so nothing can be read back", eye);
			}
			return false;
		}
	}

	if(state.staging == nullptr){
		// The grid buffer is DEFAULT, so it cannot be mapped. This is the copy that can, and it is the only thing
		// that crosses to the CPU: one value per zone, not the texture.
		D3D11_BUFFER_DESC stagingDesc = {};
		stagingDesc.ByteWidth = static_cast<UINT>(gridSizeSlots) * 4;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if(FAILED(device->CreateBuffer(&stagingDesc, nullptr, &state.staging))){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the staging buffer, so nothing can be read back", eye);
			}
			return false;
		}
	}

	return true;
}

bool SharedTextureCapture::ReadGrid(int eye, std::array<uint8_t, LocalDimming::ZonesPerEye>& grid){
	bool accepted;
	{
		std::lock_guard<std::mutex> lock(stateLock);
		accepted = SampleEye(eye);
		if(accepted){
			grid = eyes[eye].grid;
		}
	}

	return accepted;
}

bool SharedTextureCapture::SampleEye(int eye){
	EyeState& state = eyes[eye];

	if(!recorded.haveHandle){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			LocalDimmingLog(
				"LocalDimmingCapture eye %d: read attempted with no shared handle recorded, so the hook has not grabbed a handle for the texture the compositor made",
				eye);
		}
		return false;
	}

	if(!EnsureDevice()){
		return false;
	}

	if(!OpenSharedTexture()){
		return false;
	}

	if(!EnsureResources(eye)){
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	// Only the part of the texture that belongs to this eye is reduced: the confirmed texture is one combined
	// image with the eyes side by side, so the region is this eye's half at the full height and the array slice is
	// 0. The zone block size has to come from that region and not from the whole texture, otherwise half of this
	// eye's zones measure the other eye's pixels.
	state.region = LocalDimming::CaptureRegionForEye(static_cast<int>(desc.Width), static_cast<int>(desc.Height), eye);

	// The filter that recorded this texture already requires one image wide enough for both eyes at one zone
	// each, so a block is at least one pixel.
	state.blockWidth = state.region.width / LocalDimming::GridCols;
	state.blockHeight = state.region.height / LocalDimming::GridRows;

	// The constant data is built once for the sample and handed to both reads, so the pair differs only in the
	// moment it ran. The channel order is the byte order the confirmed format stores, and nothing decodes: the
	// view gives the bytes as they are stored and the swizzle only names which index is which channel, which is
	// what the shader weights work on.
	LocalDimming::ChannelOrder order = LocalDimming::StoredChannelOrder;

	SampleConstants data = {};
	data.region[0] = state.region.x;
	data.region[1] = state.region.y;
	data.region[2] = state.region.width;
	data.region[3] = state.region.height;
	data.block[0] = state.blockWidth;
	data.block[1] = state.blockHeight;
	data.eyeInfo[0] = eye;
	// the channel index of R, G and B in the stored byte order
	data.swizzle[0] = order.r;
	data.swizzle[1] = order.g;
	data.swizzle[2] = order.b;

	// Two reads of the same texture for one sample. A dispatch runs whenever the scheduler runs it, so a read
	// that lands while the compositor is still drawing sees part of the frame drawn and part of it not,
	// which is the partial frame the flicker looks like. Two reads that come out the same are a pair that ran
	// in a quiet window, so the grid they agree on is a whole frame, and that is the only grid this sample
	// keeps: a sample that cannot agree holds the values the panel was last driven with rather than sending a
	// frame that is part drawn. The first sample has to agree before there is a grid at all.
	std::array<uint8_t, LocalDimming::ZonesPerEye> first;
	if(!DispatchAndRead(eye, data, first)){
		return false;
	}

	if(!state.loggedFirstRead){
		state.loggedFirstRead = true;
		LocalDimmingLog(
			"LocalDimmingCapture eye %d: first read succeeded, grid %dx%d, region %dx%d at %d,%d, block %dx%d, view format %d named for the resource format %d",
			eye, LocalDimming::GridRows, LocalDimming::GridCols, state.region.width, state.region.height, state.region.x,
			state.region.y, state.blockWidth, state.blockHeight, LocalDimming::ViewFormat, static_cast<int>(desc.Format));
	}

	std::array<uint8_t, LocalDimming::ZonesPerEye> second;
	if(!DispatchAndRead(eye, data, second)){
		return false;
	}

	if(first == second){
		state.grid = first;
		state.accepted = true;
	}else if(!state.loggedDisagreement){
		state.loggedDisagreement = true;
		LocalDimmingLog(
			"LocalDimmingCapture eye %d: two reads of the same texture disagreed, so at least one of them ran while the compositor was still drawing and read a frame only partly drawn, and the grid accepted before this sample is kept",
			eye);
	}

	// A sample that could not agree has nothing new to send, so the caller keeps the values the panel was last
	// driven with.
	return state.accepted;
}

bool SharedTextureCapture::DispatchAndRead(int eye, const SampleConstants& data, std::array<uint8_t, LocalDimming::ZonesPerEye>& out){
	EyeState& state = eyes[eye];

	context->CSSetShader(shader, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &state.constants);
	context->CSSetUnorderedAccessViews(0, 1, &state.gridUav, nullptr);
	context->CSSetUnorderedAccessViews(1, 1, &state.sourceUav, nullptr);
	context->UpdateSubresource(state.constants, 0, nullptr, &data, 0, 0);
	context->Dispatch(static_cast<UINT>(LocalDimming::ZonesPerEye), 1, 1);

	context->CopyResource(state.staging, state.gridBuffer);

	// Map on a staging buffer completes the work that was queued on this context before the read, so no separate
	// flush is needed, and this is the device this module created rather than the compositor's.
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if(FAILED(context->Map(state.staging, 0, D3D11_MAP_READ, 0, &mapped))){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			LocalDimmingLog("LocalDimmingCapture eye %d: failed to map the readback buffer", eye);
		}
		return false;
	}

	const uint32_t* values = static_cast<const uint32_t*>(mapped.pData);

	// This eye's grid, row major, read at the positions ZoneIndex gives. The grid buffer is the whole grid, so the
	// slots this dispatch did not write are the other eye's half, and they are not read here. Every position of the
	// grid is read, so the caller's array is written in full and needs no clearing.
	for(int row = 0; row < LocalDimming::GridRows; row++){
		for(int col = 0; col < LocalDimming::GridCols; col++){
			out[static_cast<size_t>(row) * LocalDimming::GridCols + col] =
				static_cast<uint8_t>(values[LocalDimming::ZoneIndex(eye, row, col)] & 0xFF);
		}
	}

	context->Unmap(state.staging, 0);

	return true;
}

}

#endif
