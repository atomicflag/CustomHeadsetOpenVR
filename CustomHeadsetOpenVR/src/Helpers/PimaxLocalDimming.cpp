#include "PimaxLocalDimming.h"
#include "MiscHelper.h"

#include <chrono>
#include <cstdio>

// The clock the tick is measured on. The capture has no clock of its own, so this is the only one.
static double NowSeconds(){
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
PimaxLocalDimming::PimaxLocalDimming(const LocalDimmingConfig& config) :
	config(config),
	transport(*this),
	// The capture is the object the hook records the shared handle for and the grid source both eyes read
	// through, so the runner takes it through the interface. Order matters here as well: the capture is
	// initialised before the runner that references it.
	capture(),
	runner(config, transport, &capture){
}
#else
// On Linux the plugin is never injected, so this class is never constructed and nothing is ever sent. The
// runner has no source, which is the same outcome as an eye with nothing to read.
PimaxLocalDimming::PimaxLocalDimming(const LocalDimmingConfig& config) :
	config(config),
	transport(*this),
	runner(config, transport, nullptr){
}
#endif

PimaxLocalDimming::~PimaxLocalDimming(){
	// The thread has to stop before the handles are closed, because it is the thread that writes.
	threadRunning = false;
	if(tickThread.joinable()){
		tickThread.join();
	}
	std::lock_guard<std::mutex> lock(hidLock);
	CloseHandlesLocked();
}

void PimaxLocalDimming::Start(){
	if(threadRunning){
		return;
	}

	// hidapi is not initialised by anything else in the injected process, so this is its first use here and
	// the one time it has to be called.
	if(!hidInitialised){
		hidInitialised = true;
		if(hid_init() != 0){
			LocalDimmingLog("LocalDimming: hid_init failed");
		}
	}

	threadRunning = true;
	tickThread = std::thread(&PimaxLocalDimming::ThreadLoop, this);
}

void PimaxLocalDimming::ThreadLoop(){
	while(threadRunning){
		// The interval is measured from the start of the tick, so a write that takes time uses part of the wait
		// rather than pushing the next tick out by a whole interval.
		double tickStart = NowSeconds();
		RunFrame();
		lastTickSeconds = tickStart;

		// The wait is the time left in the interval, so a tick that overran is not paid back by pushing the next
		// one out.
		double delayMs = LocalDimming::ThreadSleepMs(NowSeconds(), lastTickSeconds);
		if(delayMs <= 0.0){
			continue;
		}
		SleepMs(static_cast<int>(delayMs));
	}
}

void PimaxLocalDimming::CloseHandlesLocked(){
	for(auto& node : nodeHandles){
		if(node.second){
			hid_close(node.second);
		}
	}
	nodeHandles.clear();
}

std::vector<std::string> PimaxLocalDimming::EnumeratePimaxPaths(){
	std::vector<std::string> nodes;
	hid_device_info* devices = hid_enumerate(static_cast<unsigned short>(LocalDimming::HidVendorId), 0);
	if(!devices){
		return nodes;
	}
	for(auto devicesIter = devices; devicesIter; devicesIter = devicesIter->next){
		nodes.push_back(devicesIter->path);
	}
	hid_free_enumeration(devices);
	return nodes;
}

std::string PimaxLocalDimming::FindNodePathByLabel(const std::string& label){
	for(const std::string& path : EnumeratePimaxPaths()){
		if(LocalDimming::MatchesNodeLabel(path, label)){
			return path;
		}
	}
	return "";
}

hid_device* PimaxLocalDimming::HandleForNodeLocked(const std::string& label, const char* purpose){
	auto found = nodeHandles.find(label);
	if(found != nodeHandles.end()){
		return found->second;
	}
	double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	// do not enumerate HID on every frame when the node is not there
	if(now - lastOpenAttempt < 5){
		return nullptr;
	}
	lastOpenAttempt = now;

	std::string path = FindNodePathByLabel(label);
	if(path.empty()){
		if(loggedNodeFailure.insert(label).second){
			LocalDimmingLog("LocalDimming: no Pimax HID node %s present", label.c_str());
		}
		return nullptr;
	}
	hid_device* handle = hid_open_path(path.c_str());
	if(!handle){
		if(loggedNodeFailure.insert(label).second){
			LocalDimmingLog("LocalDimming: failed to open Pimax HID node %s, Pimax software may be holding it exclusively",
				label.c_str());
		}
		return nullptr;
	}
	hid_set_nonblocking(handle, 1);
	LocalDimmingLog("LocalDimming: opened HID node %s for %s", label.c_str(), purpose);
	nodeHandles[label] = handle;
	return handle;
}

hid_device* PimaxLocalDimming::HandleForNode(const std::string& label, const char* purpose){
	std::lock_guard<std::mutex> lock(hidLock);
	return HandleForNodeLocked(label, purpose);
}

bool PimaxLocalDimming::SendToNode(const std::string& label, const char* purpose, const uint8_t* report, size_t length){
	std::lock_guard<std::mutex> lock(hidLock);
	// There is no command handle in this process, so the handle kept for the node is the only thing a
	// write can go to. When it is not there the write has no destination, and that is what a run reads.
	hid_device* node = HandleForNodeLocked(label, purpose);
	if(node == nullptr){
		return false;
	}

	int written = hid_write(node, report, static_cast<int>(length));
	return written == static_cast<int>(length);
}

bool PimaxLocalDimming::HidTransport::SendCommand(const std::array<uint8_t, LocalDimming::CommandReportSize>& report){
	bool sent = owner.SendToNode(LocalDimming::CommandNodeLabel, "command frames", report.data(), LocalDimming::CommandReportSize);

	// The entitlement record is the one frame that decides whether the device applies the zone values at all,
	// so the first one is logged. This is what lets the driver log be lined up with the headset log, so the
	// line only claims a send when the write actually returned success. The record is the payload of the frame
	// whose cmd is CmdControl, at the position the fixed shape puts it.
	if(sent && !loggedRecord && report[3] == LocalDimming::CmdControl){
		loggedRecord = true;
		LocalDimmingLog("LocalDimming: entitlement record %08x sent on node %s", static_cast<unsigned int>(report[5]) |
			(static_cast<unsigned int>(report[6]) << 8) | (static_cast<unsigned int>(report[7]) << 16) |
			(static_cast<unsigned int>(report[8]) << 24), LocalDimming::CommandNodeLabel);
	}

	return sent;
}

bool PimaxLocalDimming::HidTransport::SendDataReport(const std::array<uint8_t, LocalDimming::DataReportSize>& report){
	bool sent = owner.SendToNode(LocalDimming::DataNodeLabel, "zone frames", report.data(), LocalDimming::DataReportSize);

	// The first data report is the one that shows the confirmed report form on the wire: the node that takes a
	// 1025 byte write is the device Pimax opens for local dimming, and it is a separate USB identity from the
	// node that takes the 65 byte command frames.
	if(sent && !loggedBigReport){
		loggedBigReport = true;
		LocalDimmingLog("LocalDimming: first data report, %d bytes, flags %d, length field %d, node %s",
			static_cast<int>(LocalDimming::DataReportSize), report[1], report[2] | (report[3] << 8), LocalDimming::DataNodeLabel);
	}

	return sent;
}

bool PimaxLocalDimming::RunFrame(){
	if(!config.enable){
		return false;
	}

	if(!loggedFirstFrame){
		loggedFirstFrame = true;
		// This is the only way to tell a headset that is not being presented from a transport that cannot
		// write: without it both cases look the same in the log.
		LocalDimmingLog("LocalDimming: first frame, grid %dx%d, data node %s, command node %s",
			LocalDimming::GridRows, LocalDimming::GridCols, LocalDimming::DataNodeLabel, LocalDimming::CommandNodeLabel);
	}

	bool sent = runner.RunFrame();

	if(sent && !loggedFirstSend){
		loggedFirstSend = true;
		// The number of blocks is what tells a device run whether the whole grid was sent: the headset logs one
		// line per block only for the param frames, and nothing at all for the data report, so a 1152 byte grid
		// is only visible here.
		LocalDimmingLog("LocalDimming: first zone frame sent, %d zones per eye, %d blocks",
			LocalDimming::ZonesPerEye, runner.LastBlockCount());
	}

	return sent;
}
