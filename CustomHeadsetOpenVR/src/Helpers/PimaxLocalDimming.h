#pragma once
#include "../Config/Config.h"
#include "LocalDimmingCapture.h"
#include "LocalDimmingRunner.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <hidapi/hidapi.h>

/*
The device side of local dimming for the Crystal family: the HID transport, the grid source, the runner, and the
thread that ticks it. The zone math has no driver dependencies, so this is the only part that needs a headset,
and the capture is the only part that cannot be checked without one.

The grid source is the capture. On Linux there is no capture, so the runner has no source and nothing is ever
sent there.

Everything here runs in the process the DLL is injected into, vrcompositor, because the image only exists there:
the CreateTexture2D hook is installed in the copy of the DLL loaded in that process, so the capture and the sender
have to be the same copy. Nothing is created here when the plugin is not injected.

It runs on its own thread. IVRCompositorPluginProvider has no per frame callback, so there is no frame to drive
the runner from, and the tick is the whole `LocalDimming::SendIntervalMs`, which is what
`LocalDimming::ThreadSleepMs` gives. HID writes do not happen on the render thread, because Pimax warns on writes
slower than 5 ms and a slow write on that thread is a stutter in the compositor. The capture runs on this thread
as well, on the device the capture owns: the hook only records the shared handle, so the reduce, the readback and
the Map are all on the thread that owns its own context.

Nothing here depends on PimaxDirectUSB. The command handle that class keeps is not open in the injected process,
so there is no fallback for a write: it goes to the node found for its label, and a node that cannot be opened is
a write with no destination. hidapi is not initialised by anything else in this process, so hid_init is called
once here, on first use.

The two nodes are the confirmed ones, so a frame's destination is its node label: the command frame goes to the
command node and the zone data to the data node. The path is still read from enumeration, because paths change
between runs and Pimax software holds its own interfaces.

Every line this writes goes through LocalDimmingLog, which takes only its own file lock and no other, so a line
can be written while hidLock is held.
*/
class PimaxLocalDimming{
public:
	PimaxLocalDimming(const LocalDimmingConfig& config);
	// Stops the tick thread and closes the handles kept for the nodes.
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

	// Sends one command frame to the command node and one data report to the data node. It does not wait for
	// the headset reply, because the per frame path cannot block on the 0.5 second command timeout that
	// SendCommandWithResult uses, and because there is no command handle in this process.
	class HidTransport : public LocalDimming::Transport{
	public:
		HidTransport(PimaxLocalDimming& owner) : owner(owner) {}
		bool SendCommand(const std::array<uint8_t, LocalDimming::CommandReportSize>& report) override;
		bool SendDataReport(const std::array<uint8_t, LocalDimming::DataReportSize>& report) override;
	private:
		PimaxLocalDimming& owner;
		// one line each, so a run can read the record and the first data report without repeating
		bool loggedRecord = false;
		bool loggedBigReport = false;
	};

	// Every Pimax HID node in enumeration order, as HID path strings. No lock, the caller holds hidLock.
	std::vector<std::string> EnumeratePimaxPaths();

	// Path for one Pimax node by label, empty when that node is not present. No lock, the caller holds
	// hidLock. Two nodes can report the same interface number, so the label is what a destination is found by.
	std::string FindNodePathByLabel(const std::string& label);

	// Handle one node's frames go to, opened on first use and kept for the rest of the run. Only successful
	// opens are kept, so a node that is not there is tried again, and the attempt is throttled so a run does
	// not enumerate HID on every frame. The failure line is written once per node, so a run reads the first one
	// rather than one per attempt. No lock, the caller holds hidLock.
	hid_device* HandleForNodeLocked(const std::string& label, const char* purpose);
	// The same, taking hidLock.
	hid_device* HandleForNode(const std::string& label, const char* purpose);

	// Writes one whole report buffer to the handle kept for one node, opening it on first use. The length is the
	// fixed size of the report this process writes for that node: 0x401 for the data node, 65 for the command
	// node. There is no command handle to fall back to, so a node that cannot be opened is a write with no
	// destination rather than a write to another node.
	bool SendToNode(const std::string& label, const char* purpose, const uint8_t* report, size_t length);

	const LocalDimmingConfig& config;
	HidTransport transport;
#ifdef _WIN32
	// The capture owns the device and the opened shared texture, and it is the grid source the runner reads for
	// both eyes. It is declared first because the runner holds a reference to it through the interface and members
	// are initialised in declaration order.
	LocalDimmingCapture::SharedTextureCapture capture;
#endif
	LocalDimming::Runner runner;

	bool loggedFirstFrame = false;
	bool loggedFirstSend = false;

	// hidapi state, owned here rather than by PimaxDirectUSB, because this runs in the injected process.
	// Handles kept per node label, so the command node and the data node each get their own, and Pimax
	// software can still hold its own interfaces at the same time.
	std::map<std::string, hid_device*> nodeHandles;
	std::set<std::string> loggedNodeFailure;
	std::mutex hidLock;
	double lastOpenAttempt = 0;
	bool hidInitialised = false;

	// The tick thread. It sleeps the send interval after each tick rather than spinning, so every tick is due and
	// the runner has no throttle of its own.
	std::thread tickThread;
	std::atomic<bool> threadRunning{ false };
	double lastTickSeconds = -1.0;

	// Closes every handle kept for a node. No lock, the caller holds hidLock.
	void CloseHandlesLocked();
};
