#pragma once
#include "LocalDimmingRunner.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

/*
Every local dimming line goes through this instead of DriverLog.

DriverLog falls back to OutputDebugStringA when vr::VRDriverContext() is null, which is the case in the injected
process, so a line written there never reaches a log file. This appends to `localdimming.log` in the config
folder, once per event, behind its own mutex, and nothing inside it takes any other lock, so a caller can write
while it holds its own.
*/
void LocalDimmingLog(const char* format, ...);

#ifdef _WIN32
// IDXGIResource1 is the shared handle route for feature level 11_1 and above, IDXGIResource the one for 11_0,
// and IDXGIAdapter is the adapter this module's own device is created on.
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

/*
The image the compositor displays, reduced to one value per zone on the GPU, read on the local dimming thread
through a shared handle.

The confirmed composited image is one 5760x2880 texture, array size 1, with the eyes side by side, so the
subresource is always 0. Its desc reports misc flags 2, D3D11_RESOURCE_MISC_SHARED, so a shared texture can be
opened on a different device: the hook records the handle once per texture and this module reads it on its own
device, which has to be on the same adapter for the handle to name the same memory.

Nothing synchronises the read with the compositor's draws, so a sample reads the same texture twice and keeps the
grid only when the two agree: a pair that agrees ran while the compositor was not drawing, so it saw a whole
frame, and a pair that disagrees keeps the grid accepted before it.

The read path is the unordered access view on the opened texture, viewed as the integer member of the recorded
format because the body reads as uint4. Nothing decodes: the view gives the bytes as they are stored and the
swizzle only names which index is which channel.

The render resolution option can change during a run, which makes the compositor create a new texture at a
different size, so the capture keeps the first texture it recorded and reads a new one when the desc the hook
reports differs in size. Without that decision the sample path would keep reading a texture the compositor can
have released.

Every outcome is logged once: the first handle, the first device, the first open, one line per eye for the first
read that succeeded and for the first disagreement, and one failure line per eye. A texture that arrives before
any capture is registered is kept with a reference and replayed when the capture registers, so the order between
the hook being installed and the plugin object being created does not decide which texture the run records.
*/
namespace LocalDimmingCapture{

/*
The texture the hook observed, as the numbers the capture needs, kept as plain ints and one 64 bit handle so the
hook thread and the local dimming thread do not pass a D3D11 type between them. The desc is recorded so the log
can say whether the opened texture reports the desc the hook recorded, which is the check that says the handle
opened the same resource rather than a different one. The size is also the identity the change decision uses.
*/
struct RecordedTexture{
	uint64_t handle = 0;
	bool haveHandle = false;
	int width = 0;
	int height = 0;
	int arraySize = 0;
	int bindFlags = 0;
	int usage = 0;
	int format = 0;
	int mips = 0;
	int miscFlags = 0;
};

// Everything that touches a device lives here, and every call below `RecordTexture` is made on the local
// dimming thread. `RecordTexture` is the only call made on the hook thread, and it records, it does not sample.
class SharedTextureCapture : public LocalDimming::GridSource{
public:
	SharedTextureCapture();
	~SharedTextureCapture();

	/*
	The hook's entry point for one texture. A texture is created once, so this is once per texture. A later call
	for a desc that differs in size is a new texture, and the handle is grabbed again through the same route, the
	texture is opened again on this module's own device, and the old opened texture and the per-eye views are
	released so the sample path recreates them. The buffers are left as they are, because their size depends only
	on the grid. A call for the texture already recorded does nothing, and a grab that failed is not retried for
	the same texture.
	*/
	void RecordTexture(ID3D11Texture2D* texture);

	// One sample for one eye: create the device and the opened texture once, run the reduce for this eye's
	// region, read the grid back through the staging buffer, store it when the two reads agree, and give the
	// values in the order `LocalDimming::ZoneIndex` gives. Called once per tick, so every call samples.
	bool ReadGrid(int eye, std::array<uint8_t, LocalDimming::ZonesPerEye>& grid) override;

private:
	// the shape one compute thread has to be fed, in the layout the cbuffer declares
	struct SampleConstants{
		int region[4];
		int block[4];
		int eyeInfo[4];
		int swizzle[4];
	};

	// what one eye reads through. One source view, one grid buffer, one grid view and one staging buffer per
	// eye, all on this module's own device.
	struct EyeState{
		ID3D11UnorderedAccessView* sourceUav = nullptr;      // the typed unordered access view on the opened texture
		ID3D11Buffer* constants = nullptr;
		ID3D11Buffer* gridBuffer = nullptr;
		ID3D11UnorderedAccessView* gridUav = nullptr;
		ID3D11Buffer* staging = nullptr;
		// the grid the last accepted sample stored. A pair that agrees is a pair that ran while the compositor
		// was not drawing, so it saw a whole frame; a pair that disagrees keeps the grid accepted before it.
		// `accepted` says whether any pair has agreed yet, which a fixed grid cannot show by being empty.
		std::array<uint8_t, LocalDimming::ZonesPerEye> grid = {};
		bool accepted = false;
		int blockWidth = 0;
		int blockHeight = 0;
		LocalDimming::CaptureRegion region = {};
		// one line per eye, so a run can be read against them without repeating on every tick
		bool loggedFailure = false;
		bool loggedFirstRead = false;
		bool loggedDisagreement = false;
	};

	// the device this module owns, and the context that goes with it. Created on the local dimming thread, on
	// the adapter the hook recorded. Nothing here is a hooked entry.
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	IDXGIAdapter* adapter = nullptr;
	ID3D11Texture2D* openedTexture = nullptr;
	ID3D11ComputeShader* shader = nullptr;

	RecordedTexture recorded;
	EyeState eyes[LocalDimming::EyeCount];
	std::mutex stateLock;

	// One line for the events that belong to the capture rather than to one eye: the grab, the device, the open,
	// and one for a desc that names a different texture. The open is tried again on the next tick, so the failure
	// line and the line that reports what was opened are guarded separately.
	bool grabAttempted = false;
	bool loggedDevice = false;
	bool loggedOpenFailed = false;
	bool loggedOpenDesc = false;

	// Called with stateLock held. The two routes for a shared handle, in the same words the first grab line uses,
	// so a re-grab for a new texture answers through the same route. The adapter is recorded on the first grab and
	// kept, because a texture made later by the same device is on the same adapter.
	uint64_t GrabSharedHandle(ID3D11Texture2D* texture, std::string& route);

	// Called with stateLock held, or by the destructor after the capture is no longer registered. Release the
	// opened texture and each eye's view set, so the sample path recreates them for the new resource when they
	// are null. The buffers are not released, because their size depends only on the grid and not on the texture.
	void ReleaseOpenedResources();

	// Called with stateLock held. Returns false when the eye cannot be read.
	bool SampleEye(int eye);

	// One dispatch and its readback, written into the caller's grid. Called twice for one sample, at two
	// moments, so a pair that agrees is a pair that ran while the compositor was not drawing, and the sample keeps
	// the pair only when the two fixed grids compare equal.
	bool DispatchAndRead(int eye, const SampleConstants& data, std::array<uint8_t, LocalDimming::ZonesPerEye>& out);
	bool EnsureDevice();
	bool OpenSharedTexture();
	bool EnsureResources(int eye);

};

/*
The capture the hook records the handle for, registered once by the object that owns the runner for the life of
the process. The hook reads it without a lock. Nothing is recorded when nothing is registered, but the texture is
kept for the capture that registers later, with a reference so it stays alive.
*/
void RegisterCapture(SharedTextureCapture* capture);

// The hook's entry point for a texture it created. Returns true when a registered capture recorded it, false
// when nothing was registered, in which case the texture is kept for the replay and the hook keeps its own desc
// line.
bool RecordSharedTextureFromHook(ID3D11Texture2D* texture);

}

#endif
