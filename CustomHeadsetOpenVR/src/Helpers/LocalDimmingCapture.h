#pragma once
#include "LocalDimmingRunner.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

void LocalDimmingLog(const char* format, ...);

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

namespace LocalDimmingCapture{

// plain ints and one 64 bit handle, so no D3D11 type crosses between the hook thread and the local dimming thread
struct RecordedTexture{
	uint64_t handle = 0;
	int width = 0;
	int height = 0;
	int arraySize = 0;
	int bindFlags = 0;
	int usage = 0;
	int format = 0;
	int mips = 0;
	int miscFlags = 0;
};

// everything that touches a device lives here, on the local dimming thread; RecordTexture is the only call made on the hook thread
class SharedTextureCapture : public LocalDimming::GridSource{
public:
	SharedTextureCapture();
	~SharedTextureCapture();

	void RecordTexture(ID3D11Texture2D* texture);

	bool ReadGrid(int eye, std::array<uint8_t, LocalDimming::ZonesPerEye>& grid) override;

private:
	// mirrors the cbuffer layout in the block reduce source
	struct SampleConstants{
		int region[4];
		int block[4];
		int eyeInfo[4];
		int swizzle[4];
	};

	struct EyeState{
		ID3D11UnorderedAccessView* sourceUav = nullptr;
		ID3D11Buffer* constants = nullptr;
		ID3D11Buffer* gridBuffer = nullptr;
		ID3D11UnorderedAccessView* gridUav = nullptr;
		ID3D11Buffer* staging = nullptr;
		std::array<uint8_t, LocalDimming::ZonesPerEye> grid = {};
		bool accepted = false;
		int blockWidth = 0;
		int blockHeight = 0;
		LocalDimming::CaptureRegion region = {};
		bool loggedFailure = false;
		bool loggedFirstRead = false;
		bool loggedDisagreement = false;
	};

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	IDXGIAdapter* adapter = nullptr;
	ID3D11Texture2D* openedTexture = nullptr;
	ID3D11ComputeShader* shader = nullptr;

	RecordedTexture recorded;
	EyeState eyes[LocalDimming::EyeCount];
	std::mutex stateLock;

	bool grabAttempted = false;
	bool loggedDevice = false;
	bool loggedOpenFailed = false;
	bool loggedOpenDesc = false;

	uint64_t GrabSharedHandle(ID3D11Texture2D* texture, std::string& route);

	void ReleaseOpenedResources();

	bool SampleEye(int eye);

	bool DispatchAndRead(int eye, const SampleConstants& data, std::array<uint8_t, LocalDimming::ZonesPerEye>& out);
	bool EnsureDevice();
	bool OpenSharedTexture();
	bool EnsureResources(int eye);

};

void RegisterCapture(SharedTextureCapture* capture);

bool RecordSharedTextureFromHook(ID3D11Texture2D* texture);

}

#endif
