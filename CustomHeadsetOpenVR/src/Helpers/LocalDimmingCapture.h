#pragma once
#include "LocalDimmingRunner.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/*
Every local dimming line goes through this instead of DriverLog.

DriverLog falls back to OutputDebugStringA when vr::VRDriverContext() is null, and that is exactly the case
in the injected process, which is why no ShaderReplacement line appears in any log file. Local dimming now
runs in that process, so its lines have to reach a file on their own: this appends to `localdimming.log` in
the config folder, once per event, behind its own mutex. Every local dimming call is made after the caller
has released any other lock, so the file lock is never held at the same time as the capture or the HID lock.

This is the local dimming path only. DriverLog is unchanged for the driver process.
*/
void LocalDimmingLog(const char* format, ...);

#ifdef _WIN32
// The two GetSharedHandle routes. IDXGIResource1 is the one for feature level 11_1 and above, IDXGIResource is
// the one for 11_0, and IDXGIAdapter is the adapter this module's own device is created on. The design note
// called the first route ID3D11Resource1; there is no such interface in the Windows SDK headers, and the
// interface a resource actually answers to for a shared handle is the DXGI one.
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

/*
The image the compositor actually displays, reduced to one value per zone on the GPU, read on the local
dimming thread through a shared handle.

Runs 14 and 15 closed the route the previous design used. The `CreatePixelShader` and `CreateTexture2D` detours
fired in the injected process, and the first plausible desc was `5760x2880, array size 1, bind flags 168,
usage 0, format 27, mips 1, misc flags 2`, but no draw detour and no `Flush` detour ever fired, and the per
interface version lookup showed all four versions answering to the same object and the same array, so there is
one cumulative array and nothing left to hook per version. The compositor's context is not the object that was
hooked, so a render-target read cannot be reached from a draw detour at all.

What the same run did give is the route this design uses: `misc flags 2` is `D3D11_RESOURCE_MISC_SHARED`, so the
composited side by side image is a shared texture, and a shared texture can be opened on a different device. So
the capture does not read the compositor's context any more. It takes the handle once per texture, on the hook
thread, and opens it on the local dimming module's own device, which nothing the hook uses, and everything after
that runs on the local dimming thread.

Three things follow from that and are the reason the shape is what it is:

  - the hook keeps only the desc observation and the handle grab. The draw, `Flush` and the two copy detours are
    gone, because the capture no longer depends on them and a hook that fires on every draw costs per frame even
    when it only decides and returns. The `CreatePixelShader` hook stays for the custom shader feature, and
    `CreateTexture2D` stays for the desc line and the grab, still gated on local dimming being enabled so a user
    with the feature off is not affected.
  - the capture's device is its own device, created on the adapter the hook recorded, because a shared handle
    opened on another device only refers to the same memory when both devices are on the same adapter. Nothing
    on this module's context is a hooked entry, so no stored original function pointers are needed and no
    re-entrancy guard is needed: a call the capture makes cannot come back into the hook, which is what made the
    previous design need both.
  - the read is not synchronised with the compositor's writes. There is no fence and no lock between a draw the
    compositor queues and a dispatch this module runs on the same memory, so the only way to tell a live read
    from a stale one is to look at the grid over several samples. That is what the staleness line is for: for the
    first three samples the log writes minimum, maximum and the count of non-zero zones, and if three consecutive
    grids are identical it writes one line saying the grid has not changed across three samples. A grid that
    changes is the evidence; a flat or unchanging grid is an answer, not a failure. The render resolution option
    can change during a run, which makes the compositor create a new composited texture at a different size, so
    the capture keeps the first texture it recorded and reads a new one when the desc the hook reports differs in
    width, height or array size, which is `LocalDimming::DescChangedForCapture`. This is what makes the staleness
    line readable: an unchanging grid after a resolution change is the texture being the old one, and the old
    resource can have been released, so the handle the capture holds can name a freed allocation. Without that
    decision the line could only be read as the compositor not presenting anything, because nothing in the run
    said the size had changed.

The reduce itself is unchanged in shape: one compute thread per zone, averaging the pixels of that zone's block
inside this eye's region, which is the region `LocalDimming::CaptureRegionForEye` gives, and one value per zone
in `LocalDimming::GridSlot` order. For `5760x2880` the regions are the halves and the blocks are 120x120 on a
24x24 grid. The weights, the shift and the grid size the shader uses are the defines the pure core generates
(`BlockAverageDefines`), so the GPU mirror cannot use numbers the tested function does not. The 3x3 neighbour
kernel is still not applied here: the kernel stays a pure-core decision on the grid, and the run has one
variable. Output goes to a `BIND_UNORDERED_ACCESS` buffer, is copied to a staging buffer that can be mapped, and
the mapped bytes are stored. The buffer is the whole grid, in `GridSlot` order, so the slots one eye's dispatch
does not write are the other eye's half.

Which view the opened texture is read through is the bind-flag decision, `LocalDimming::ResolveCaptureReadPath`,
on the desc the opened texture reports: the unordered access view when the unordered access bind is there, which
is what the observed desc has; the shader resource view when only that bind is there; and when neither view bit
is present the texture is copied once into a texture this module creates with `D3D11_BIND_SHADER_RESOURCE`. Which
path was used is logged, and it is logged as the name this decision gives rather than as a string the capture
kept to itself. Run 17 recorded format 27 on the texture it took, the typeless member of the R8G8B8A8 family, and
was rejected by the format decision before any view was created. The accepted set is `LocalDimming::AcceptedCaptureFormat`,
which is one decision: a format is readable when its family has an unsigned integer member, and the view names that
member through `LocalDimming::FamilyViewFormat`, because that is the class the HLSL body reads as uint4 and a view on
a typeless resource has to name a member of its own family with the same bits per component. For format 27 that gives
30, not 3, so the B8G8R8A8 formats are now rejected - that family has no unsigned integer member, so no legal view on
it can name the class this path reads. The format the view was created with is logged. The channel order is
`LocalDimming::ChannelOrderForFormat`, the family's order including its typeless member, and nothing decodes: the
view gives the bytes as they are stored and the swizzle only names which index is which channel, which is what the
tested `Luminance` works on; for the formats this path can read that order is the identity order.

Eye attribution is no longer a decision the capture makes. Each source reads for one eye, and the region that eye
reduces is fixed by its own eye and not by which view a draw was bound to, so there is no order of views to
assume and nothing to attribute. `LocalDimming::CaptureEyeIndex` is unchanged as the tested region-by-x decision;
the capture path simply no longer needs it, because there is no per draw call left to read an x from.

The throttle is `LocalDimming::ShouldSample` at `config.sendIntervalMs`, per eye, and `ZoneImageSource::Read`'s
interface is unchanged: the runner reads one grid per eye per tick, and this reads the bytes it stored when the
throttle says a sample is due. `LocalDimming::ComputeZoneValues` and the wire format, the flags and the block
limits are unchanged.

What is proven without a device is the decisions, not that the hook is reached. Whether the first plausible desc
is the composited eye image, whether the handle can be grabbed, and whether the texture opened from it is the
image the compositor is presenting, are what a run has to answer, and every one of those outcomes is logged once:
the desc and the grab, the open, the path chosen, the first dispatch, the first three grids, a grid that has not
changed across three samples, and the first read.

Two of those decisions changed after run 16, which recorded a texture that was not the composited image. The
plausible decision now also requires one mip level, because a composited per eye image has one level and the
texture run 16 recorded had 11, so a mipmapped resource is not read as an eye image. And a plausible texture that
arrives before any capture is registered is kept with a reference and replayed when the capture registers, so the
order between the hook being installed and the plugin object being created does not decide which texture the run
records; one line names the desc that was seen before registration and what was replayed for it.
*/
namespace LocalDimmingCapture{

/*
The texture the hook observed, as the numbers the capture needs, kept as plain ints and one 64 bit handle so the
hook thread and the local dimming thread do not pass a D3D11 type between them. The desc is recorded so the log
can say whether the desc the opened texture reports agrees with the desc the hook recorded, which is the check
that says the handle opened the same resource rather than a different one.
*/
struct RecordedTexture{
	uint64_t handle = 0;
	bool haveHandle = false;
	// width, height and array size are also the identity the change decision uses: the desc the hook reports for
	// a later texture is compared with the one recorded here, and a difference in any of the three is a new
	// texture rather than the same one reported twice.
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
class SharedTextureCapture{
public:
	explicit SharedTextureCapture(const LocalDimming::Config& config);
	~SharedTextureCapture();

	/*
	The hook's entry point for one texture, called on the hook thread for the first plausible desc the
	`CreateTexture2D` detour reports, and once more for the texture that arrived before any capture was registered,
	which is replayed by `RegisterCapture` when the capture is there. A texture is created once, so this is once per
	texture, and the flag that records the handle is the guard against being called twice for the same one.

	The first plausible texture is recorded as it always was. A later call is compared with what was recorded, and
	a desc that differs in width, height or array size is a new texture, which is what happens when the render
	resolution option changes and the compositor makes a new composited texture at a different size. A call for
	the same texture still does nothing, so this is one event per texture, not per frame work on the hook thread,
	and a grab that failed is not retried for the same texture. On a change the handle is grabbed again through
	the same route, the texture is opened again on this module's own device, and the old opened texture and the
	per-eye views are released so the sample path recreates them. The buffers are left as they are, because their
	size depends only on the grid.

	The handle comes from `IDXGIResource1::GetSharedHandle`, which is the route for feature level 11_1 and
	above, with `IDXGIResource::GetSharedHandle` as the fallback for 11_0. The log line says which of the two
	answered, what it answered with, and whether the grab failed. This also records the adapter the resource's
	device is on, because this module's device has to be created on the same adapter for the handle to name the
	same memory. It does not create anything or sample, so the render thread is not charged for any of the
	capture's work beyond the one line for the change.
	*/
	void RecordTexture(ID3D11Texture2D* texture);

	/*
	One sample for one eye, on the local dimming thread. Throttled by `LocalDimming::ShouldSample` at the
	configured interval: a tick that is not due for a sample takes the stored grid and makes no device call. When
	a sample is due, this creates the device and the opened texture once, runs the compute reduce for this eye's
	region, reads the grid back through the staging buffer, stores it and returns the values. The grid values are
	this eye's grid, row major, taken from the slots `LocalDimming::GridSlot` names, so the layout the GPU writes
	and the layout the CPU reads are the same decision the tests guard. Returns false when this eye has nothing to
	return, which is the same outcome the interface reports when no image is available.
	*/
	bool ReadGrid(int eye, double nowSeconds, std::vector<uint8_t>& gridValues);

	// Recorded once for the process, so a run can read the interval and the path this capture is using.
	const LocalDimming::Config& Config() const { return config; }

private:
	// The same config object the runner and the source read, so the interval a run reads is the live one.
	const LocalDimming::Config& config;

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
		ID3D11ShaderResourceView* sourceView = nullptr;      // the shader resource view, for the copy path and the SRV path
		ID3D11UnorderedAccessView* sourceUav = nullptr;      // the typed unordered access view, for the UAV path
		ID3D11Texture2D* copyTexture = nullptr;              // only when neither view bit is present
		ID3D11Buffer* constants = nullptr;
		ID3D11Buffer* gridBuffer = nullptr;
		ID3D11UnorderedAccessView* gridUav = nullptr;
		ID3D11Buffer* staging = nullptr;
		// the last grid and the one before it, which is the pair the staleness decision is made from
		std::vector<uint8_t> grid;
		std::vector<uint8_t> previousGrid;
		// how many samples in a row have been identical to the one before them
		int identicalRun = 0;
		// the pair of reads a sample makes, and what came out of it. A read that agrees with the read made
		// straight after it is a read that ran while the compositor was not drawing, so it saw a whole frame.
		// A pair that disagrees is a read that caught the drawing in progress, which is the partial frame the
		// flicker looks like, and the grid that was accepted before is kept instead.
		int agreeCount = 0;
		int disagreeCount = 0;
		int samples = 0;
		int blockWidth = 0;
		int blockHeight = 0;
		int subresource = 0;
		LocalDimming::CaptureRegion region = {};
		double lastSampleSeconds = -1.0;
		// one line per eye, so a run can be read against them without repeating on every tick
		bool loggedFailure = false;
		bool loggedFirstDispatch = false;
		bool loggedFirstRead = false;
		bool loggedStale = false;
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
	EyeState eyes[2];
	std::mutex stateLock;

	// One line for the events that belong to the capture rather than to one eye: the grab, the device, the open,
	// and one for a desc that names a different texture. A failed grab is not tried again for the same texture,
	// so a run sees one line for it rather than one per call for that texture, while a desc that is a different
	// texture is a different event and its grab is tried. The open is tried again on the next tick, so the
	// failure line and the line that reports what was opened are guarded separately: a run that fails to open and
	// then opens still has both.
	bool grabAttempted = false;
	bool loggedDevice = false;
	bool loggedOpenFailed = false;
	bool loggedOpenDesc = false;

	// Called with stateLock held. The two routes for a shared handle, in the same words the first grab line
	// uses, so a re-grab for a new texture answers through the same route and the run reads the same thing. The
	// adapter is recorded on the first grab and kept, because a texture made later by the same device is on the
	// same adapter.
	uint64_t GrabSharedHandle(ID3D11Texture2D* texture, std::string& route);

	// Called with stateLock held, or by the destructor after the capture is no longer registered. Release the
	// opened texture and each eye's view set, so the sample path recreates them for the new resource when they
	// are null. The buffers are not released, because their size depends only on the grid and not on the texture.
	void ReleaseOpenedResources();

	// Called with stateLock held. Returns false when the eye cannot be read, and collects the lines it decided
	// so they are written after the lock is gone. One entry per line, because every line is one event.
	bool SampleEye(int eye, double nowSeconds, std::vector<std::string>& pendingLines);

	// One dispatch and its readback, into the grid the caller asks for. Called twice for one sample:
	// the two reads run at different moments, so a pair that agrees is a pair that ran while the compositor was
	// not drawing, and a pair that disagrees is a pair where at least one read saw a frame only partly drawn.
	bool DispatchAndRead(int eye, LocalDimming::CaptureReadPath path, const SampleConstants& data, int zonesPerEye,
		std::vector<uint8_t>& out, std::vector<std::string>& pendingLines);
	bool EnsureDevice(std::vector<std::string>& pendingLines);
	bool OpenSharedTexture(std::vector<std::string>& pendingLines);
	bool EnsureResources(int eye, std::vector<std::string>& pendingLines);

	// The numbers the staleness line and the first three sample lines read, from one grid.
	static void GridStats(const std::vector<uint8_t>& grid, int& minimum, int& maximum, int& nonZero);
};

// One eye's read handle. The whole capture is shared between the two eyes because the observed texture is one
// texture read at two regions or two slices; what is per eye is the region, the view set, the grid and the
// throttle time.
class SharedTextureImageSource : public LocalDimming::ZoneImageSource{
public:
	SharedTextureImageSource(int eye, const LocalDimming::Config& config, SharedTextureCapture& capture);

	// reads the current image for one eye as rgb, width * height * 3, row major. The stored grid is the
	// luminance byte for the zone in all three channels, so re-luminance is the identity, which is what the
	// tested path's weights give.
	bool Read(std::vector<uint8_t>& rgb, int& width, int& height) override;

private:
	int eye = 0;
	const LocalDimming::Config& config;
	SharedTextureCapture& capture;
};

/*
The capture the hook records the handle for, registered once by the object that owns the runner for the life of
the process. The hook reads it without a lock. Nothing is recorded when nothing is registered, but the texture is
kept for the capture that registers later: run 16 is the case where the hook was installed before this object
existed, so the first plausible desc was logged for a capture that was not there and the run recorded the next
plausible texture instead. Keeping the first one, with a reference so it stays alive, is what makes the replay
record the texture the hook actually saw. The replay is one line, and it names the desc that was seen before
registration and what was replayed for it.
*/
void RegisterCapture(SharedTextureCapture* capture);

// The hook's entry point for a texture it created. Returns true when a registered capture recorded it, false
// when nothing was registered, in which case the texture is kept for the replay and the hook keeps its own desc
// line.
bool RecordSharedTextureFromHook(ID3D11Texture2D* texture);

}

#endif