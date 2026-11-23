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

uint64_t SharedTextureCapture::GrabSharedHandle(ID3D11Texture2D* texture){
	uint64_t handle = 0;

	IDXGIResource1* resource1 = nullptr;
	if(SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource1)))){
		HANDLE sharedHandle = nullptr;
		if(SUCCEEDED(resource1->GetSharedHandle(&sharedHandle))){
			handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
		}
		resource1->Release();
	}else{
		IDXGIResource* dxgiResource = nullptr;
		if(SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiResource)))){
			HANDLE sharedHandle = nullptr;
			if(SUCCEEDED(dxgiResource->GetSharedHandle(&sharedHandle))){
				handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
			}
			dxgiResource->Release();
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

		if(grabAttempted && width == recorded.width && height == recorded.height){
			return;
		}

		bool firstTexture = !grabAttempted;
		grabAttempted = true;

		recorded.width = width;
		recorded.height = height;
		recorded.handle = GrabSharedHandle(texture);

		if(!firstTexture){
			ReleaseOpenedResources();
			if(device != nullptr){
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
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);

	if(FAILED(hr) || device == nullptr || context == nullptr){
		if(!loggedDevice){
			loggedDevice = true;
			LocalDimmingLog(
				"LocalDimmingCapture: could not create this module's own device, adapter %s, result %08x",
				adapter != nullptr ? "the recorded adapter" : "the default",
				static_cast<unsigned int>(hr));
		}
		return false;
	}

	return true;
}

bool SharedTextureCapture::OpenSharedTexture(){
	if(openedTexture != nullptr){
		return true;
	}

	ID3D11Texture2D* opened = nullptr;
	HRESULT hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(recorded.handle), __uuidof(ID3D11Texture2D),
		reinterpret_cast<void**>(&opened));

	if(FAILED(hr) || opened == nullptr){
		if(!loggedOpenFailed){
			loggedOpenFailed = true;
			LocalDimmingLog(
				"LocalDimmingCapture: OpenSharedResource failed for handle %llu, result %08x",
				static_cast<unsigned long long>(recorded.handle), static_cast<unsigned int>(hr));
		}
		return false;
	}

	openedTexture = opened;

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
					"LocalDimmingCapture eye %d: CreateUnorderedAccessView failed, format %d, result %08x",
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
					"LocalDimmingCapture eye %d: D3DCompile failed, result %08x, message %s",
					eye, static_cast<unsigned int>(hr), message.c_str());
			}
		}else{
			HRESULT shaderResult = device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader);
			if(FAILED(shaderResult)){
				if(!state.loggedFailure){
					state.loggedFailure = true;
					LocalDimmingLog(
						"LocalDimmingCapture eye %d: CreateComputeShader failed on the compiled bytecode, result %08x",
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
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the constant buffer", eye);
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
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the grid buffer", eye);
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
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the grid view", eye);
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
				LocalDimmingLog("LocalDimmingCapture eye %d: failed to create the staging buffer", eye);
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
			"LocalDimmingCapture eye %d: two reads disagreed, the last accepted grid is kept",
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
