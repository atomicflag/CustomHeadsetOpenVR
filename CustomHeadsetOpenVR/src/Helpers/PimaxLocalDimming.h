#pragma once
#include "../Config/Config.h"
#include "LocalDimmingCapture.h"
#include "LocalDimmingRunner.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <hidapi/hidapi.h>

/*
The device side of local dimming for the Crystal family: the HID transport, the image sources, the runner,
and the thread that ticks it. The zone math has no driver dependencies, so this is the only part that needs
a headset, and the render target capture is the only part that cannot be checked without one.

The image source is the captured render target, or the test pattern when the config asks for it.

Everything here runs in the process the DLL is injected into, vrcompositor, because the image only exists
there: the CopySubresourceRegion hook is installed in the copy of the DLL loaded in that process, so the
capture source and the sender have to be the same copy. Nothing is created here when the plugin is not
injected, which makes the feature Windows only in practice: InjectCompositorPlugin returns false on Linux,
so on Linux this class is never constructed and nothing is ever sent.

It runs on its own thread. IVRCompositorPluginProvider has no per frame callback, so there is no frame to
drive the runner from, and the tick is config.sendIntervalMs, which is what LocalDimming::ThreadSleepMs
gives. HID writes do not happen on the render thread, because Pimax warns on writes slower than 5 ms and a
slow write on that thread is a stutter in the compositor. The capture now runs on this thread as well, on the
device the capture owns: the hook only records the shared handle, so the reduce, the readback and the Map are
all on the thread that owns its own context and nothing on it is shared with the hook.

Nothing here depends on PimaxDirectUSB. The command handle that class keeps is not open in the injected
process, so there is no fallback for a write: it goes to the node resolved for its report size, or to the
configured interface number, and a target that cannot be opened is a write with no destination. hidapi is not
initialised by anything else in this process, so hid_init is called once here, on first use.

Every line this writes goes through LocalDimmingLog, which appends to localdimming.log in the config folder,
and every call happens after hidLock is released, so the file lock is never held at the same time as the HID
lock.
*/
class PimaxLocalDimming{
public:
	PimaxLocalDimming(const LocalDimmingConfig& config);
	// Stops the tick thread and closes the handles kept for the targets.
	~PimaxLocalDimming();

	// Starts the tick thread, and hidapi with it. Called once from CompositorPlugin::Initialize for the
	// injected process, only when the connected headset has local dimming on.
	void Start();

	// One tick of the runner. Returns true when a zone frame was sent. Runs on the tick thread, never on the
	// render thread.
	bool RunFrame();

private:
	// What the tick thread runs: a tick, then the remainder of the send interval.
	void ThreadLoop();

	// Sends one frame to the node resolved for its report size, or to the configured interface number. It
	// does not wait for the headset reply, because the per frame path cannot block on the 0.5 second command
	// timeout that SendCommandWithResult uses, and because there is no command handle in this process.
	class HidTransport : public LocalDimming::Transport{
	public:
		HidTransport(PimaxLocalDimming& owner) : owner(owner) {}
		bool Send(uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload) override;
		// the 1024 byte data report path. The node that takes a 1025 byte write is the device Pimax
		// opens for local dimming, so the first one is logged with its length.
		bool SendReport(const std::vector<uint8_t>& report) override;
		// Re-arms the two logs so the first record and the first data report after the probe name the
		// node they actually went to, which is the node the probe found for that report size.
		void ResetLogged(){
			loggedRecord = false;
			loggedBigReport = false;
		}
	private:
		PimaxLocalDimming& owner;
		bool loggedRecord = false;
		bool loggedBigReport = false;
	};

	// Node label a write of one report size goes to: the node being probed while the probe runs,
	// otherwise the target resolved from what the probe found for that size. Empty means the configured
	// interface.
	std::string TargetFor(size_t reportSize);

	// How a target is written in the log, so a run records which node each report size went to.
	static std::string TargetName(const std::string& target);

	// A line written by a helper that holds hidLock, kept so the call happens after the lock is released.
	// The wording is the wording the driver process already used, so a run reads against the same lines.
	static std::string LogLine(const char* format, ...);

	// Every Pimax HID node in enumeration order, as HID path strings. No lock, the caller holds hidLock.
	std::vector<std::string> EnumeratePimaxPaths();
	// Every Pimax HID node in enumeration order, taking hidLock.
	std::vector<std::string> FindPimaxNodes();
	// Path for one Pimax node by label, empty when that node is not present. No lock, the caller holds
	// hidLock.
	std::string FindNodePathByLabel(const std::string& label);
	// Path for one Pimax HID interface, empty when that interface is not present. No lock, the caller holds
	// hidLock.
	std::string FindInterfacePath(int interfaceNumber);

	// Handle one target's frames go to, opened on first use and kept for the rest of the run. `label` is a
	// node label and an empty label is the configured interface number. This is what makes a probe run keep
	// the device it found instead of naming the node once and then writing every later frame to the
	// interface. Only successful opens are kept, so a node that is not there is tried again. No lock, the
	// caller holds hidLock.
	hid_device* HandleForTargetLocked(const std::string& label, int interfaceNumber, const char* purpose,
		std::string& pendingLog);
	// The same, taking hidLock, and writing the line it collected after the lock is released.
	hid_device* HandleForTarget(const std::string& label, int interfaceNumber, const char* purpose);

	// Opens and keeps the handle the zone data reports go to, by node label when one names a node and
	// otherwise by interface number. hidapi does not claim a device exclusively, so this can hold the same
	// node as Pimax software, and their frames interleave.
	bool OpenLocalDimmingTarget(const std::string& nodeLabel, int interfaceNumber);

	// Writes one whole report buffer to the handle kept for one target, opening it on first use, which is the
	// 1024 byte data report path. There is no command handle to fall back to, so a target that cannot be
	// opened is a write with no destination rather than a write to the command device.
	bool SendReport(const std::string& label, int interfaceNumber, const char* purpose, const std::vector<uint8_t>& report);
	bool SendFrame(const std::string& label, int interfaceNumber, uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload);

	// Writes one report to one node, opening and closing a handle for it, which is the probe path and the one
	// that reports the result of every write. Without the result per write, a write that failed and a write
	// that landed on a node the firmware does not read commands from look the same in the headset log.
	bool WriteToNode(const std::string& path, const std::vector<uint8_t>& report);

	const LocalDimmingConfig& config;
	HidTransport transport;
	// The two sources are read for the two eyes, and the runner takes them in payload order, so the left
	// source is the first half of the zone array and the right source the second. A pattern that gives both
	// eyes the same values cannot tell the two halves apart; EyeSplit can, because it makes only the half
	// eye 0 is read for bright, so which physical eye lights up is the observation.
	LocalDimming::TestPatternImageSource testPatternLeft;
	LocalDimming::TestPatternImageSource testPatternRight;
#ifdef _WIN32
	// The capture owns the device and the opened shared texture, and the two sources are one eye's read handle
	// through it. The capture is declared first because the sources hold a reference to it and members are
	// initialised in declaration order, so the reference is to a constructed object.
	LocalDimmingCapture::SharedTextureCapture capture;
	LocalDimmingCapture::SharedTextureImageSource captureLeft;
	LocalDimmingCapture::SharedTextureImageSource captureRight;
#endif
	LocalDimming::Runner runner;

	bool loggedFirstFrame = false;
	bool loggedFirstSend = false;

	// Node the next send goes to, empty when sends use the configured interface. Set by the probe.
	std::string probeNode;
	std::vector<std::string> probeNodePaths;
	int probeIndex = 0;
	bool probeComplete = false;

	// Node that accepted the first successful write of each report size, as a node label. The param
	// frame and the data report land on different devices on the Crystal Light, so the probe has to keep
	// both: the run that finds the local dimming device has to write to it for the rest of the run.
	LocalDimming::ProbeTargets targets;
	bool loggedTargets = false;

	// hidapi state, owned here rather than by PimaxDirectUSB, because this runs in the injected process.
	// Handles kept for the local dimming targets, by node label. The param frames and the data report can
	// name different nodes, so each target gets its own handle, and two targets that name the same node
	// share one.
	std::map<std::string, hid_device*> targetHandles;
	std::mutex hidLock;
	double lastDimmingAttempt = 0;
	bool hidInitialised = false;

	// The tick thread. The runner is throttled by config.sendIntervalMs, so the thread sleeps the remainder
	// of that interval after each tick rather than spinning, and the sleep is capped so a settings change is
	// picked up on the next tick.
	std::thread tickThread;
	std::atomic<bool> threadRunning{ false };
	double lastTickSeconds = -1.0;

	// Closes every handle kept for a target. No lock, the caller holds hidLock.
	void CloseHandlesLocked();
};
