#include "LocalDimmingCapture.h"
#include "../Config/ConfigLoader.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Every local dimming line is appended to this file, once per event, behind one mutex. The file is opened per
// line rather than kept open, because a run is a handful of lines and a line that is not flushed is not
// evidence, and because the two threads that log do not share any other state. The callers all release their
// own lock before calling this, so only this mutex is held while the file is written.
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
#include <chrono>
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
// later. Run 16 is the case this is for: the hook is installed by ShaderReplacement before the object that owns
// the capture is created, so the composited image can arrive first, and because nothing was registered the run
// recorded the next plausible texture instead. Keeping the texture and not only its desc is the whole point: the
// reference taken here is what keeps it alive until the replay, so what is recorded is the texture the hook
// actually saw rather than whatever allocation its desc named. The first plausible desc is the one kept, because
// that is the texture a run would have recorded if the capture had been there, and a second one is not kept
// because by the time it arrives the capture decides which texture it reads.
static std::mutex pendingTextureLock;
static ID3D11Texture2D* pendingTexture = nullptr;

// The same clock the runner's send interval uses, so the throttle decision and the tick share one base.
static double NowSeconds(){
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A line built while a lock is held, written after it is released. This is the rule the log file has, and it is
// why these helpers return strings instead of calling LocalDimmingLog directly.
static std::string LogLine(const char* format, ...){
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	return buf;
}

// The compiler answers with a multi line message, and the log is read one line at a time, so a message is put on
// one line before it is recorded. A long message is still cut by LogLine's buffer, which is why the message is
// the last part of the line.
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

	/*
	 One line for the texture that arrived before registration, and it names what was replayed. The replay goes
	 through the same record path a desc that arrives after registration uses, so the desc, the handle and the
	 adapter are recorded by the same code whichever order the run had, and the grab line that follows this one is
	 the line a run already reads. The desc is read from the pending texture here, because it holds a reference and
	 GetDesc is not an entry this module detours.
	*/
	std::string replayLine;
	ID3D11Texture2D* pending = nullptr;
	{
		std::lock_guard<std::mutex> lock(pendingTextureLock);
		if(pendingTexture != nullptr){
			pending = pendingTexture;
			pendingTexture = nullptr;

			D3D11_TEXTURE2D_DESC desc = {};
			pending->GetDesc(&desc);
			replayLine = LogLine(
				"LocalDimmingCapture: a plausible desc was seen before any capture was registered, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, and that texture was kept and replayed for the capture that registered, through the same record path a desc that arrives after registration uses",
				static_cast<int>(desc.Width), static_cast<int>(desc.Height), static_cast<int>(desc.ArraySize),
				static_cast<int>(desc.BindFlags), static_cast<int>(desc.Usage), static_cast<int>(desc.Format),
				static_cast<int>(desc.MipLevels), static_cast<int>(desc.MiscFlags));
		}
	}

	if(pending != nullptr){
		// outside the lock, which is the rule the log file has
		LocalDimmingLog("%s", replayLine.c_str());

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
 The compute source is the pure core’s: BlockReduceSource assembles the defines it generates from the
 constants Luminance uses with the body it holds, so the input a run compiles is the input the tests check.
 The neighbour kernel is not in it, because the kernel stays a pure-core decision on the grid and is not applied
 in this run.
*/
SharedTextureCapture::SharedTextureCapture(const LocalDimming::Config& config) :
	config(config){
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

	for(int eye = 0; eye < 2; eye++){
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

	for(int eye = 0; eye < 2; eye++){
		EyeState& state = eyes[eye];
		if(state.sourceUav != nullptr){
			state.sourceUav->Release();
			state.sourceUav = nullptr;
		}
		if(state.sourceView != nullptr){
			state.sourceView->Release();
			state.sourceView = nullptr;
		}
		if(state.copyTexture != nullptr){
			state.copyTexture->Release();
			state.copyTexture = nullptr;
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

	std::vector<std::string> pendingLines;

	{
		std::lock_guard<std::mutex> lock(stateLock);

		D3D11_TEXTURE2D_DESC desc = {};
		texture->GetDesc(&desc);

		int width = static_cast<int>(desc.Width);
		int height = static_cast<int>(desc.Height);
		int arraySize = static_cast<int>(desc.ArraySize);

		bool firstTexture = !grabAttempted;

		if(!firstTexture && !LocalDimming::DescChangedForCapture(recorded.width, recorded.height, recorded.arraySize, width, height, arraySize)){
			// once per texture, and the hook calls this for a texture it created, so a second call for the
			// texture that was already recorded does nothing. A texture is created once, so this is not per
			// frame work on the hook thread. A grab that failed is not tried again for the same texture, so the
			// run has one line for it rather than one per call.
			return;
		}

		int oldWidth = recorded.width;
		int oldHeight = recorded.height;
		int oldArraySize = recorded.arraySize;

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

		// The handle replaces the one recorded for the texture that was, rather than being kept alongside it: the
		// old handle names the old resource, which the compositor can have released, so holding on to it after a
		// change is the staleness this decision exists to remove. A desc that is a texture and still gives no
		// handle leaves the capture with nothing to open, which is the outcome the run already reads from the
		// line that says nothing can be read back.
		recorded.handle = handle;
		recorded.haveHandle = handle != 0;

		if(firstTexture){
			if(recorded.haveHandle){
				pendingLines.push_back(LogLine(
					"LocalDimmingCapture: shared handle grabbed for the first plausible texture, handle %llu, %s, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, read path by bind flags %s, adapter %p recorded for this module's own device",
					static_cast<unsigned long long>(recorded.handle), route.c_str(), recorded.width, recorded.height,
					recorded.arraySize, recorded.bindFlags, recorded.usage, recorded.format, recorded.mips,
					recorded.miscFlags, LocalDimming::CaptureReadPathName(LocalDimming::ResolveCaptureReadPath(recorded.bindFlags)).c_str(),
					static_cast<const void*>(adapter)));
			}else{
				// A plain failure, stated as one line. The most likely reason is misc flags without the shared bit,
				// which is a texture that cannot be opened anywhere else, and that is the outcome the desc line
				// already names by its misc flags field.
				pendingLines.push_back(LogLine(
					"LocalDimmingCapture: shared handle grab failed, %s, handle 0, desc %dx%d, array size %d, bind flags %d, misc flags %d, so the capture has nothing to open on its own device and nothing can be read back",
					route.c_str(), recorded.width, recorded.height, recorded.arraySize, recorded.bindFlags, recorded.miscFlags));
			}
		}else{
			// One line for the change, which is the only thing a run can read for a render resolution change: the
			// size the recorded texture had and the size it has now. The staleness line is read against this line,
			// because a grid that stops changing after a change like this is the capture reading the texture that
			// was, which is what this decision removes.
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture: the texture the capture reads changed, desc %dx%d, array size %d, became %dx%d, array size %d, handle %llu, %s, so the opened texture and the per-eye views are released and recreated for the new resource",
				oldWidth, oldHeight, oldArraySize, width, height, arraySize,
				static_cast<unsigned long long>(handle), route.c_str()));
		}

		if(!firstTexture){
			// The view set that points at the old resource cannot be used for the new one, so the opened texture and
			// each eye's view set are released here and the sample path recreates them when they are null. The open
			// goes through the route the sample path already has, on this module's own device: the device interface
			// is thread safe and nothing here touches the context, which is only used on the local dimming thread.
			// When the device does not exist yet the open is left to the next sample, because the device is created
			// on that thread and not on the hook thread.
			ReleaseOpenedResources();
			if(recorded.haveHandle && device != nullptr){
				OpenSharedTexture(pendingLines);
			}
		}
	}

	for(const std::string& line : pendingLines){
		LocalDimmingLog("%s", line.c_str());
	}
}

// Called with stateLock held. The device is created once, on the local dimming thread, on the adapter the hook
// recorded. When the hook recorded no adapter, the default adapter is used and the line says so, because that is
// a different outcome from a failed open on a machine with more than one adapter.
bool SharedTextureCapture::EnsureDevice(std::vector<std::string>& pendingLines){
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
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture: could not create this module's own device, adapter %s, result %08x, so nothing can be opened and nothing can be read back",
				adapter != nullptr ? "recorded by the hook" : "the default, because the hook recorded no adapter",
				static_cast<unsigned int>(hr)));
		}
		return false;
	}

	if(!loggedDevice){
		loggedDevice = true;
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture: this module's own device, device %p, context %p, feature level %d, created on the local dimming thread, used only on that thread, no entry on it is hooked, adapter %s",
			static_cast<const void*>(device), static_cast<const void*>(context), static_cast<int>(chosen),
			adapter != nullptr ? "the one the hook recorded" : "the default, because the hook recorded no adapter"));
	}

	return true;
}

// Called with stateLock held. The handle is opened once for each texture the capture records, because the texture
// is one texture read at two regions or two slices, and it is opened again after a change releases it. The desc
// the opened texture reports has to agree with the desc the hook recorded, and whether it does is part of the
// line: an opened resource that reports a different desc is not the same resource, and a run could not tell that
// apart without this.
bool SharedTextureCapture::OpenSharedTexture(std::vector<std::string>& pendingLines){
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
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture: OpenSharedResource failed for handle %llu on this module's own device, result %08x, so nothing can be read back",
				static_cast<unsigned long long>(recorded.handle), static_cast<unsigned int>(hr)));
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
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture: the shared texture opened on this module's own device, desc %dx%d, array size %d, bind flags %d, usage %d, format %d, mips %d, misc flags %d, agrees with the desc the hook recorded %s, read through %s",
			static_cast<int>(desc.Width), static_cast<int>(desc.Height), static_cast<int>(desc.ArraySize),
			static_cast<int>(desc.BindFlags), static_cast<int>(desc.Usage), static_cast<int>(desc.Format),
			static_cast<int>(desc.MipLevels), static_cast<int>(desc.MiscFlags), matches ? "yes" : "no",
			LocalDimming::CaptureReadPathName(LocalDimming::ResolveCaptureReadPath(desc.BindFlags)).c_str()));
	}

	return true;
}

// Called with stateLock held. The view, the shader and the buffers are created once per eye on this module's own
// device, and the view set is created again after a change releases it, which is the path that rebuilds it for
// the new resource. The view is the one the bind flags allow, so nothing here asks for a view the resource cannot
// have: a typed unordered access view needs the unordered access bind, a shader resource view needs the shader
// resource bind, and a texture with neither is copied into one this module created with its own bind flags, which
// is the copy that gets recreated for the new size.
bool SharedTextureCapture::EnsureResources(int eye, std::vector<std::string>& pendingLines){
	EyeState& state = eyes[eye];

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	if(config.gridWidth <= 0 || config.gridHeight <= 0){
		// a grid with no cells has no zone to reduce, so there is nothing to create for it
		return false;
	}

	LocalDimming::CaptureReadPath path = LocalDimming::ResolveCaptureReadPath(desc.BindFlags);

	// The channel order is the tested decision, and it is the family's, so a typeless resource gets the order
	// its family stores bytes in. Nothing here decodes: the view gives the bytes as they are stored and the
	// swizzle only names which index is which channel, which is what the tested Luminance works on.
	LocalDimming::ChannelOrder order = LocalDimming::ChannelOrderForFormat(static_cast<int>(desc.Format));

	SampleConstants data = {};
	data.region[0] = state.region.x;
	data.region[1] = state.region.y;
	data.region[2] = state.region.width;
	data.region[3] = state.region.height;
	data.block[0] = state.blockWidth;
	data.block[1] = state.blockHeight;
	data.eyeInfo[0] = eye;
	data.eyeInfo[1] = state.subresource;
	// the channel index of R, G and B in the source's byte order, for an integer view. For a BGRA texture the
	// stored order is B, G, R; for an RGBA texture it is R, G, B.
	data.swizzle[0] = order.r;
	data.swizzle[1] = order.g;
	data.swizzle[2] = order.b;

	// The format check is the tested decision, and it is one decision now: a format is readable when its family
	// has an unsigned integer member, because that is the class the kernel reads and the only format a legal view
	// can name for the resource. Format 27 and the integer member of its family, 30, pass. The B8G8R8A8 formats
	// that used to pass now fail, because that family has no unsigned integer member, so a view on it cannot name
	// the class the tested Luminance reads raw bytes in. Run 17 failed here on format 27, which the decision then
	// only took as a UNORM variant, so the right texture was rejected before anything was created.
	int format = static_cast<int>(desc.Format);
	if(!LocalDimming::AcceptedCaptureFormat(format)){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: unsupported texture format %d", eye, format));
		}
		return false;
	}

	// The format the view names is the decision `FamilyViewFormat` gives: the unsigned integer member of the
	// resource's own family, which is the class the HLSL body reads as uint4. For format 27 that is 30, not 3,
	// because a view on a typeless resource has to name a member of its own family with the same bits per
	// component. The failure line names the format that was asked for, so the run shows the decision the capture
	// used rather than what was assumed.
	int viewFormat = LocalDimming::FamilyViewFormat(format);

	ID3D11Resource* sourceResource = openedTexture;

	if(path == LocalDimming::CaptureReadPath::Neither){
		if(state.copyTexture == nullptr){
			D3D11_TEXTURE2D_DESC copyDesc = desc;
			copyDesc.BindFlags = LocalDimming::BindShaderResource;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, &state.copyTexture);
			if(FAILED(hr) || state.copyTexture == nullptr){
				if(!state.loggedFailure){
					state.loggedFailure = true;
					pendingLines.push_back(LogLine(
						"LocalDimmingCapture eye %d: bind flags %d allow neither view, so the copy path was taken, and CreateTexture2D for that copy failed, result %08x, so nothing can be read back",
						eye, static_cast<int>(desc.BindFlags), static_cast<unsigned int>(hr)));
				}
				return false;
			}
		}
		sourceResource = state.copyTexture;
	}

	if(path == LocalDimming::CaptureReadPath::UnorderedAccessView){
		if(state.sourceUav == nullptr){
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = static_cast<DXGI_FORMAT>(viewFormat);
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = 0;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = desc.ArraySize;
			HRESULT hr = device->CreateUnorderedAccessView(openedTexture, &uavDesc, &state.sourceUav);
			if(FAILED(hr) || state.sourceUav == nullptr){
				if(!state.loggedFailure){
					state.loggedFailure = true;
					pendingLines.push_back(LogLine(
						"LocalDimmingCapture eye %d: the bind flags allow the unordered access view, and CreateUnorderedAccessView on the opened texture failed, format %d, result %08x, so nothing can be read back",
						eye, viewFormat, static_cast<unsigned int>(hr)));
				}
				return false;
			}
		}
	}else if(state.sourceView == nullptr){
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = static_cast<DXGI_FORMAT>(viewFormat);
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.ArraySize = desc.ArraySize;
		HRESULT hr = device->CreateShaderResourceView(sourceResource, &srvDesc, &state.sourceView);
		if(FAILED(hr) || state.sourceView == nullptr){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				pendingLines.push_back(LogLine(
					"LocalDimmingCapture eye %d: CreateShaderResourceView on the %s failed, format %d, result %08x, so nothing can be read back",
					eye, path == LocalDimming::CaptureReadPath::ShaderResourceView ? "opened texture" : "copy this module created",
					viewFormat, static_cast<unsigned int>(hr)));
			}
			return false;
		}
	}

	if(shader == nullptr){
		// The whole input, from the pure core, so the source a run compiles is the source the tests hold.
		std::string source = LocalDimming::BlockReduceSource(config, path);

		ID3DBlob* bytecode = nullptr;
		ID3DBlob* compileError = nullptr;
		// A source name so the compiler's own message names this kernel instead of an anonymous shader.
		HRESULT hr = D3DCompile(source.c_str(), source.size(), "blockreduce", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &compileError);
		if(FAILED(hr) || bytecode == nullptr){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				std::string message = compileError != nullptr ? OneLine((const char*)compileError->GetBufferPointer()) : "no message";
				pendingLines.push_back(LogLine(
					"LocalDimmingCapture eye %d: D3DCompile of the block reduce failed, result %08x, message %s, so nothing can be read back",
					eye, static_cast<unsigned int>(hr), message.c_str()));
			}
		}else{
			HRESULT shaderResult = device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader);
			if(FAILED(shaderResult)){
				if(!state.loggedFailure){
					state.loggedFailure = true;
					pendingLines.push_back(LogLine(
						"LocalDimmingCapture eye %d: D3DCompile of the block reduce answered with bytecode and CreateComputeShader failed on it, result %08x, so nothing can be read back",
						eye, static_cast<unsigned int>(shaderResult)));
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

	int zonesPerEye = config.gridWidth * config.gridHeight;
	int gridSizeSlots = zonesPerEye * 2;   // the whole grid, in GridSlot order

	if(state.constants == nullptr){
		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = static_cast<UINT>(sizeof(SampleConstants));
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if(FAILED(device->CreateBuffer(&cbDesc, nullptr, &state.constants))){
			if(!state.loggedFailure){
				state.loggedFailure = true;
				pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: failed to create the constant buffer, so nothing can be read back", eye));
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
				pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: failed to create the grid buffer, so nothing can be read back", eye));
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
				pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: failed to create the grid view, so nothing can be read back", eye));
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
				pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: failed to create the staging buffer, so nothing can be read back", eye));
			}
			return false;
		}
	}

	return true;
}

void SharedTextureCapture::GridStats(const std::vector<uint8_t>& grid, int& minimum, int& maximum, int& nonZero){
	minimum = 255;
	maximum = 0;
	nonZero = 0;
	for(uint8_t value : grid){
		if(value < minimum){
			minimum = value;
		}
		if(value > maximum){
			maximum = value;
		}
		if(value != 0){
			nonZero++;
		}
	}
}

bool SharedTextureCapture::ReadGrid(int eye, double nowSeconds, std::vector<uint8_t>& gridValues){
	if(eye < 0 || eye > 1){
		return false;
	}

	std::vector<std::string> pendingLines;
	bool haveGrid = false;
	std::vector<uint8_t> storedGrid;

	{
		std::lock_guard<std::mutex> lock(stateLock);
		haveGrid = SampleEye(eye, nowSeconds, pendingLines);
		if(haveGrid){
			storedGrid = eyes[eye].grid;
		}
	}

	// outside the lock, which is the rule the log file has
	for(const std::string& line : pendingLines){
		LocalDimmingLog("%s", line.c_str());
	}

	if(!haveGrid){
		return false;
	}

	gridValues = storedGrid;
	return true;
}

bool SharedTextureCapture::SampleEye(int eye, double nowSeconds, std::vector<std::string>& pendingLines){
	EyeState& state = eyes[eye];

	// The throttle is the same decision the runs already read, unchanged. A tick that is not due for a sample
	// takes the grid that was stored and makes no device call at all, which is the whole point of moving the
	// capture off the render thread: there is no per frame work left on any thread.
	if(!LocalDimming::ShouldSample(nowSeconds, state.lastSampleSeconds, config.sendIntervalMs)){
		return !state.grid.empty();
	}

	// The throttle moves on whether the sample ran at all, so a failing sample is not retried on every tick.
	state.lastSampleSeconds = nowSeconds;

	if(!recorded.haveHandle){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture eye %d: read attempted with no shared handle recorded, so the hook has not grabbed a handle for the texture the compositor made",
				eye));
		}
		return false;
	}

	if(!EnsureDevice(pendingLines)){
		return false;
	}

	if(!OpenSharedTexture(pendingLines)){
		return false;
	}

	if(!EnsureResources(eye, pendingLines)){
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	// Only the part of the texture that belongs to this eye is reduced. On a combined side by side texture the
	// whole texture is both eyes, so the zone block size has to come from this eye's region and not from the
	// texture, otherwise half of this eye's zones measure the other eye's pixels. On a two slice array the region
	// is the whole subresource, and that subresource is the slice this eye reads.
	state.region = LocalDimming::CaptureRegionForEye(
		static_cast<int>(desc.Width), static_cast<int>(desc.Height), static_cast<int>(desc.ArraySize), eye);
	state.subresource = desc.ArraySize == 2 ? eye : 0;

	state.blockWidth = state.region.width / config.gridWidth;
	state.blockHeight = state.region.height / config.gridHeight;
	if(state.blockWidth <= 0 || state.blockHeight <= 0){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture eye %d: read failed, this eye's region %dx%d at %d,%d is smaller than one zone of the grid %dx%d",
				eye, state.region.width, state.region.height, state.region.x, state.region.y, config.gridWidth, config.gridHeight));
		}
		return false;
	}

	LocalDimming::CaptureReadPath path = LocalDimming::ResolveCaptureReadPath(desc.BindFlags);
	int zonesPerEye = config.gridWidth * config.gridHeight;
	int gridSizeSlots = zonesPerEye * 2;

	if(!state.loggedFirstDispatch){
		state.loggedFirstDispatch = true;
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture eye %d: first dispatch on this module's own context, %d threads for %d zones, path used %s, view format %d named for the resource format %d, grid buffer %d slots, this eye reads %dx%d at %d,%d from subresource %d",
			eye, zonesPerEye, zonesPerEye, LocalDimming::CaptureReadPathName(path).c_str(),
			LocalDimming::FamilyViewFormat(static_cast<int>(desc.Format)), static_cast<int>(desc.Format),
			gridSizeSlots,
			state.region.width, state.region.height, state.region.x, state.region.y, state.subresource));
	}

	// The constant data is built once for the sample and handed to both reads, so the pair differs only in the
	// moment it ran. The channel order is the tested decision, and it is the family's, so a typeless resource
	// gets the order its family stores bytes in. Nothing here decodes: the view gives the bytes as they are
	// stored and the swizzle only names which index is which channel, which is what the tested Luminance works on.
	LocalDimming::ChannelOrder order = LocalDimming::ChannelOrderForFormat(static_cast<int>(desc.Format));

	SampleConstants data = {};
	data.region[0] = state.region.x;
	data.region[1] = state.region.y;
	data.region[2] = state.region.width;
	data.region[3] = state.region.height;
	data.block[0] = state.blockWidth;
	data.block[1] = state.blockHeight;
	data.eyeInfo[0] = eye;
	data.eyeInfo[1] = state.subresource;
	// the channel index of R, G and B in the source's byte order, for an integer view. For a BGRA texture the
	// stored order is B, G, R; for an RGBA texture it is R, G, B.
	data.swizzle[0] = order.r;
	data.swizzle[1] = order.g;
	data.swizzle[2] = order.b;

	// Two reads of the same texture for one sample. A dispatch runs whenever the scheduler runs it, so a read
	// that lands while the compositor is still drawing sees part of the frame drawn and part of it not,
	// which is the partial frame the flicker looks like. Two reads that come out the same are a pair that ran
	// in a quiet window, so the grid they agree on is a whole frame, and that is the only grid this sample
	// keeps: a sample that cannot agree holds the values the panel was last driven with rather than sending a
	// frame that is part drawn.
	std::vector<uint8_t> first;
	if(!DispatchAndRead(eye, path, data, zonesPerEye, first, pendingLines)){
		return false;
	}
	std::vector<uint8_t> second;
	if(!DispatchAndRead(eye, path, data, zonesPerEye, second, pendingLines)){
		return false;
	}

	if(LocalDimming::GridsIdentical(first, second)){
		state.agreeCount++;
		state.grid = first;
	}else{
		state.disagreeCount++;
		if(!state.loggedDisagreement){
			state.loggedDisagreement = true;
			pendingLines.push_back(LogLine(
				"LocalDimmingCapture eye %d: two reads of the same texture disagreed, so at least one of them ran while the compositor was still drawing and read a frame only partly drawn; the grid accepted before this sample is kept (agreed %d, disagreed %d)",
				eye, state.agreeCount, state.disagreeCount));
		}
	}

	state.samples++;

	/*
	 The proof that this is the live image rather than a stale one, which is the only thing a run can show for a
	 read that has no synchronisation with the compositor's writes. The first three samples each write minimum,
	 maximum and the count of non-zero zones, and the staleness line is written once when three consecutive grids
	 are identical to each other. A grid that changes is the evidence; a flat or unchanging grid is an answer,
	 not a failure, and the two lines say which of the two the run got.
	*/
	int minimum = 0;
	int maximum = 0;
	int nonZero = 0;
	GridStats(state.grid, minimum, maximum, nonZero);

	if(state.samples <= 3){
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture eye %d: sample %d, minimum %d, maximum %d, non-zero zones %d of %d, uniform %s, path %s, pair %s",
			eye, state.samples, minimum, maximum, nonZero, zonesPerEye,
			(minimum == maximum) ? "yes, so this is not a read of real content" : "no",
			LocalDimming::CaptureReadPathName(path).c_str(),
			LocalDimming::GridsIdentical(first, second) ? "agreed" : "disagreed"));
	}

	if(LocalDimming::GridsIdentical(state.grid, state.previousGrid)){
		state.identicalRun++;
	}else{
		state.identicalRun = 1;
	}

	if(state.identicalRun >= 3 && !state.loggedStale){
		state.loggedStale = true;
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture eye %d: the grid has not changed across three samples, which a still scene gives just as readily as a read that is not the presented image, grid minimum %d, maximum %d, non-zero zones %d of %d",
			eye, minimum, maximum, nonZero, zonesPerEye));
	}

	state.previousGrid = state.grid;

	if(!state.loggedFirstRead){
		state.loggedFirstRead = true;
		pendingLines.push_back(LogLine(
			"LocalDimmingCapture eye %d: first read succeeded, grid %dx%d, region %dx%d at %d,%d, block %dx%d, subresource %d, stored sample %d",
			eye, config.gridWidth, config.gridHeight, state.region.width, state.region.height, state.region.x,
			state.region.y, state.blockWidth, state.blockHeight, state.subresource, state.samples));
	}

	// A sample that could not agree has nothing new to send, so the caller keeps the values the panel was last
	// driven with. The first sample has to agree before there is a grid at all.
	return !state.grid.empty();
}

bool SharedTextureCapture::DispatchAndRead(int eye, LocalDimming::CaptureReadPath path, const SampleConstants& data,
	int zonesPerEye, std::vector<uint8_t>& out, std::vector<std::string>& pendingLines){
	EyeState& state = eyes[eye];

	// The copy path needs the copy before the view is read, because the view the copy path uses is on the copy.
	if(path == LocalDimming::CaptureReadPath::Neither){
		context->CopyResource(state.copyTexture, openedTexture);
	}

	context->CSSetShader(shader, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &state.constants);
	context->CSSetUnorderedAccessViews(0, 1, &state.gridUav, nullptr);
	if(path == LocalDimming::CaptureReadPath::UnorderedAccessView){
		context->CSSetUnorderedAccessViews(1, 1, &state.sourceUav, nullptr);
	}else{
		context->CSSetShaderResources(0, 1, &state.sourceView);
	}
	context->UpdateSubresource(state.constants, 0, nullptr, &data, 0, 0);
	context->Dispatch(static_cast<UINT>(zonesPerEye), 1, 1);

	context->CopyResource(state.staging, state.gridBuffer);

	// Map on a staging buffer completes the work that was queued on this context before the read, so no separate
	// flush is needed, and this is the device this module created rather than the compositor's.
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if(FAILED(context->Map(state.staging, 0, D3D11_MAP_READ, 0, &mapped))){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			pendingLines.push_back(LogLine("LocalDimmingCapture eye %d: failed to map the readback buffer", eye));
		}
		return false;
	}

	const uint32_t* values = static_cast<const uint32_t*>(mapped.pData);

	// This eye's grid, row major, read from the slots GridSlot names. The grid buffer is the whole grid, so the
	// slots this dispatch did not write are the other eye's half, and they are not read here.
	out.assign(static_cast<size_t>(zonesPerEye), 0);
	for(int row = 0; row < config.gridHeight; row++){
		for(int col = 0; col < config.gridWidth; col++){
			int slot = LocalDimming::GridSlot(eye, row, col, config.gridHeight, config.gridWidth);
			if(slot < 0){
				continue;
			}
			out[static_cast<size_t>(row) * config.gridWidth + col] =
				static_cast<uint8_t>(values[slot] & 0xFF);
		}
	}

	context->Unmap(state.staging, 0);

	return true;
}

SharedTextureImageSource::SharedTextureImageSource(int eye, const LocalDimming::Config& config, SharedTextureCapture& capture) :
	eye(eye), config(config), capture(capture){
}

bool SharedTextureImageSource::Read(std::vector<uint8_t>& rgb, int& width, int& height){
	// Everything the capture does happens here, on the local dimming thread, on the device this module owns. The
	// interface is unchanged, so the runner still reads one grid per eye per tick.
	std::vector<uint8_t> grid;
	if(!capture.ReadGrid(eye, NowSeconds(), grid)){
		return false;
	}

	width = config.gridWidth;
	height = config.gridHeight;
	if(width <= 0 || height <= 0){
		return false;
	}

	// The stored grid is the luminance byte for the zone in all three channels, so re-luminance is the identity,
	// because the tested weights sum to the shift base. The runner's ComputeZoneValues is unchanged and reads
	// this as a one pixel per zone image.
	rgb.assign(grid.size() * 3, 0);
	for(size_t zone = 0; zone < grid.size(); zone++){
		rgb[zone * 3] = grid[zone];
		rgb[zone * 3 + 1] = grid[zone];
		rgb[zone * 3 + 2] = grid[zone];
	}

	return true;
}

}

#endif