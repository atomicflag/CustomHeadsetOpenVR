#include "LocalDimmingCapture.h"
#include "../Config/ConfigLoader.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

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
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")

namespace LocalDimmingCapture{

// read by the hook without a lock
static std::atomic<SharedTextureCapture*> registeredCapture{ nullptr };

// the first plausible texture seen before a capture registers, kept with a reference so it stays alive until the replay
static std::mutex pendingTextureLock;
static ID3D11Texture2D* pendingTexture = nullptr;

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

	std::lock_guard<std::mutex> lock(pendingTextureLock);
	if(pendingTexture == nullptr){
		pendingTexture = texture;
		pendingTexture->AddRef();
	}

	return false;
}

SharedTextureCapture::SharedTextureCapture(){
	RegisterCapture(this);
}

SharedTextureCapture::~SharedTextureCapture(){
	if(registeredCapture.load(std::memory_order_relaxed) == this){
		RegisterCapture(nullptr);
	}

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
	if(adapter != nullptr){
		adapter->Release();
	}
}

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
				LocalDimmingLog(
					"LocalDimmingCapture: shared handle grab failed, %s, handle 0, desc %dx%d, array size %d, bind flags %d, misc flags %d, so the capture has nothing to open on its own device and nothing can be read back",
					route.c_str(), recorded.width, recorded.height, recorded.arraySize, recorded.bindFlags, recorded.miscFlags);
			}
		}else{
			LocalDimmingLog(
				"LocalDimmingCapture: the texture the capture reads changed, desc %dx%d became %dx%d, handle %llu, %s, so the opened texture and the per-eye views are released and recreated for the new resource",
				oldWidth, oldHeight, width, height,
				static_cast<unsigned long long>(handle), route.c_str());
		}

		if(!firstTexture){
			ReleaseOpenedResources();
			if(recorded.haveHandle && device != nullptr){
				OpenSharedTexture();
			}
		}
	}
}

bool SharedTextureCapture::EnsureDevice(){
	if(device != nullptr){
		return true;
	}

	D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
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

bool SharedTextureCapture::EnsureResources(int eye){
	EyeState& state = eyes[eye];

	D3D11_TEXTURE2D_DESC desc = {};
	openedTexture->GetDesc(&desc);

	if(state.sourceUav == nullptr){
		// a view on a typeless resource has to name a member of its own family with the same bits per
		// component, which is the integer member the HLSL body reads as uint4
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
		std::string source = LocalDimming::BlockReduceSource();

		ID3DBlob* bytecode = nullptr;
		ID3DBlob* compileError = nullptr;
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
		// the grid buffer is DEFAULT usage and cannot be mapped, so this is the buffer the grid is read back from
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

	state.region = LocalDimming::CaptureRegionForEye(static_cast<int>(desc.Width), static_cast<int>(desc.Height), eye);

	// the filter requires one image wide enough for both eyes at one zone each, so a block is at least one pixel
	state.blockWidth = state.region.width / LocalDimming::GridCols;
	state.blockHeight = state.region.height / LocalDimming::GridRows;

	LocalDimming::ChannelOrder order = LocalDimming::StoredChannelOrder;

	SampleConstants data = {};
	data.region[0] = state.region.x;
	data.region[1] = state.region.y;
	data.region[2] = state.region.width;
	data.region[3] = state.region.height;
	data.block[0] = state.blockWidth;
	data.block[1] = state.blockHeight;
	data.eyeInfo[0] = eye;
	data.swizzle[0] = order.r;
	data.swizzle[1] = order.g;
	data.swizzle[2] = order.b;

	// two reads agree only when no draw ran between them, so the grid they agree on is a whole frame
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

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if(FAILED(context->Map(state.staging, 0, D3D11_MAP_READ, 0, &mapped))){
		if(!state.loggedFailure){
			state.loggedFailure = true;
			LocalDimmingLog("LocalDimmingCapture eye %d: failed to map the readback buffer", eye);
		}
		return false;
	}

	const uint32_t* values = static_cast<const uint32_t*>(mapped.pData);

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
