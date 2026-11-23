#include "PimaxLocalDimming.h"
#include "MiscHelper.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

// The same clock the capture samples on, so the send interval and the sample throttle share one base.
static double NowSeconds(){
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PimaxLocalDimming::PimaxLocalDimming(const LocalDimmingConfig& config) :
	config(config),
	transport(*this),
	testPatternLeft(config),
	// eye 1, so the pattern is evaluated for the second half of the zone array. The runner takes the
	// sources in payload order, so this is the source that reads as the right eye block.
	testPatternRight(config, 16, 1),
#ifdef _WIN32
	// The capture is the object the hook records the shared handle for, and both eyes read through it, so the
	// sources take it by reference. Order matters here as well: the capture is initialised before the sources
	// that reference it.
	capture(config),
	captureLeft(0, config, capture),
	captureRight(1, config, capture),
#endif
	runner(config, transport, &testPatternLeft, &testPatternRight){
}

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
		// The interval is measured from the start of the tick, so a write that takes time uses part of the
		// wait rather than pushing the next tick out by a whole interval.
		double tickStart = NowSeconds();
		RunFrame();
		lastTickSeconds = tickStart;

		// The wait is the remainder of the interval rather than the whole of it, so a tick that overran is
		// not paid back by pushing the next one out, and the cap is what keeps a settings change to
		// sendIntervalMs from being ignored for a long sleep. A tick that is already due runs straight on.
		double delayMs = LocalDimming::ThreadSleepMs(NowSeconds(), lastTickSeconds, config.sendIntervalMs);
		if(delayMs <= 0.0){
			continue;
		}
		SleepMs(static_cast<int>(delayMs));
	}
}

void PimaxLocalDimming::CloseHandlesLocked(){
	for(auto& target : targetHandles){
		if(target.second){
			hid_close(target.second);
		}
	}
	targetHandles.clear();
}

std::string PimaxLocalDimming::TargetName(const std::string& target){
	return target.empty() ? "configured interface" : target;
}

std::string PimaxLocalDimming::LogLine(const char* format, ...){
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	return buf;
}

std::string PimaxLocalDimming::TargetFor(size_t reportSize){
	if(!probeComplete){
		return LocalDimming::NodeLabel(probeNode);
	}
	return LocalDimming::ResolveTarget(config.hidNode, reportSize, targets, probeNodePaths);
}

std::vector<std::string> PimaxLocalDimming::EnumeratePimaxPaths(){
	std::vector<std::string> nodes;
	hid_device_info* devices = hid_enumerate(0x34A4, 0);
	if(!devices){
		return nodes;
	}
	for(auto devicesIter = devices; devicesIter; devicesIter = devicesIter->next){
		nodes.push_back(devicesIter->path);
	}
	hid_free_enumeration(devices);
	return nodes;
}

std::vector<std::string> PimaxLocalDimming::FindPimaxNodes(){
	std::lock_guard<std::mutex> lock(hidLock);
	return EnumeratePimaxPaths();
}

std::string PimaxLocalDimming::FindNodePathByLabel(const std::string& label){
	for(const std::string& path : EnumeratePimaxPaths()){
		if(LocalDimming::MatchesNodeLabel(path, label)){
			return path;
		}
	}
	return "";
}

std::string PimaxLocalDimming::FindInterfacePath(int interfaceNumber){
	hid_device_info* devices = hid_enumerate(0x34A4, 0);
	if(!devices){
		return "";
	}
	std::string path = "";
	for(auto devicesIter = devices; devicesIter; devicesIter = devicesIter->next){
		if(devicesIter->interface_number == interfaceNumber){
			path = devicesIter->path;
			break;
		}
	}
	hid_free_enumeration(devices);
	return path;
}

hid_device* PimaxLocalDimming::HandleForTargetLocked(const std::string& label, int interfaceNumber,
	const char* purpose, std::string& pendingLog){
	auto found = targetHandles.find(label);
	if(found != targetHandles.end()){
		return found->second;
	}
	double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	// do not enumerate HID on every frame when the node is not there
	if(now - lastDimmingAttempt < 5){
		return nullptr;
	}
	lastDimmingAttempt = now;

	std::string path = label.empty() ? FindInterfacePath(interfaceNumber) : FindNodePathByLabel(label);
	if(path.empty()){
		pendingLog = LogLine("LocalDimming: no Pimax HID node %s present, interface %d", label.c_str(), interfaceNumber);
		return nullptr;
	}
	hid_device* handle = hid_open_path(path.c_str());
	if(!handle){
		pendingLog = LogLine("LocalDimming: failed to open Pimax HID node %s, Pimax software may be holding it exclusively",
			LocalDimming::NodeLabel(path).c_str());
		return nullptr;
	}
	hid_set_nonblocking(handle, 1);
	pendingLog = LogLine("LocalDimming: opened HID node %s for %s", LocalDimming::NodeLabel(path).c_str(), purpose);
	targetHandles[label] = handle;
	return handle;
}

hid_device* PimaxLocalDimming::HandleForTarget(const std::string& label, int interfaceNumber, const char* purpose){
	std::string pendingLog;
	hid_device* handle;
	{
		std::lock_guard<std::mutex> lock(hidLock);
		handle = HandleForTargetLocked(label, interfaceNumber, purpose, pendingLog);
	}
	// outside hidLock, so the file lock is never held at the same time as the HID lock
	if(!pendingLog.empty()){
		LocalDimmingLog("%s", pendingLog.c_str());
	}
	return handle;
}

bool PimaxLocalDimming::OpenLocalDimmingTarget(const std::string& nodeLabel, int interfaceNumber){
	// This is how the node from the probe log is pinned down for normal use, since two nodes can report the
	// same interface number. It is separate from the command path because Pimax software has to stay running
	// to keep the headset powered and it holds its own interfaces, so local dimming opens its own handle.
	return HandleForTarget(nodeLabel, interfaceNumber, "zone frames") != nullptr;
}

bool PimaxLocalDimming::SendReport(const std::string& label, int interfaceNumber, const char* purpose,
	const std::vector<uint8_t>& report){
	if(report.empty()){
		return false;
	}

	std::string pendingLog;
	bool sent = false;
	{
		std::lock_guard<std::mutex> lock(hidLock);
		// There is no command handle in this process, so the handle kept for the target is the only thing a
		// write can go to. When it is not there the write has no destination, and that is what a run reads.
		hid_device* target = HandleForTargetLocked(label, interfaceNumber, purpose, pendingLog);
		if(target == nullptr){
			// nothing to write to
		}else{
			int written = hid_write(target, report.data(), static_cast<int>(report.size()));
			sent = written == static_cast<int>(report.size());
		}
	}

	if(!pendingLog.empty()){
		LocalDimmingLog("%s", pendingLog.c_str());
	}

	return sent;
}

bool PimaxLocalDimming::SendFrame(const std::string& label, int interfaceNumber, uint8_t cmd, uint8_t opt,
	const std::vector<uint8_t>& payload){
	auto report = LocalDimming::BuildReport(cmd, opt, payload);
	if(report.empty()){
		return false;
	}
	return SendReport(label, interfaceNumber, "command frames", report);
}

bool PimaxLocalDimming::WriteToNode(const std::string& path, const std::vector<uint8_t>& report){
	std::string pendingLog;
	bool sent = false;
	{
		std::lock_guard<std::mutex> lock(hidLock);
		std::string label = LocalDimming::NodeLabel(path);
		hid_device* target = hid_open_path(path.c_str());
		if(!target){
			pendingLog = LogLine("LocalDimming: probe node %s could not be opened", label.c_str());
		}else{
			int written = hid_write(target, report.data(), static_cast<int>(report.size()));
			hid_close(target);
			sent = written == static_cast<int>(report.size());
			pendingLog = LogLine("LocalDimming: probe node %s %s", label.c_str(),
				sent ? "written" : "write failed");
		}
	}

	if(!pendingLog.empty()){
		LocalDimmingLog("%s", pendingLog.c_str());
	}

	return sent;
}

bool PimaxLocalDimming::HidTransport::Send(uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload){
	std::string target = owner.TargetFor(LocalDimming::ReportSize);
	bool sent;
	if(!owner.probeComplete){
		// The probe has to see the result of every write, so it opens a handle for the node under test
		// and closes it again. After the probe the handle for the target is kept open for the run.
		auto report = LocalDimming::BuildReport(cmd, opt, payload);
		if(report.empty()){
			return false;
		}
		sent = owner.WriteToNode(owner.probeNode, report);
	}else{
		sent = owner.SendFrame(target, owner.config.hidInterface, cmd, opt, payload);
	}

	// The first node that took a 65 byte report is the command device. It is the node the param frames
	// keep using after the probe, because the node that takes the data report rejects them.
	if(sent && !target.empty() && owner.targets.commandNode.empty()){
		owner.targets.commandNode = target;
	}

	// The 4 byte control frame is the entitlement record, and it is the one frame that decides whether
	// the device applies the zone values at all, so the first one is logged. This is what lets the
	// driver log be lined up with the headset log, so the line only claims a send when the write
	// actually returned success: on the 2026-11-22 run it reported the record as sent on a node whose
	// per-node write failed.
	if(sent && !loggedRecord && cmd == LocalDimming::CmdControl && payload.size() == 4){
		loggedRecord = true;
		LocalDimmingLog("LocalDimming: entitlement record %08x sent on node %s", static_cast<unsigned int>(payload[0]) |
			(static_cast<unsigned int>(payload[1]) << 8) | (static_cast<unsigned int>(payload[2]) << 16) |
			(static_cast<unsigned int>(payload[3]) << 24), owner.TargetName(target).c_str());
	}

	return sent;
}

bool PimaxLocalDimming::HidTransport::SendReport(const std::vector<uint8_t>& report){
	std::string target = owner.TargetFor(LocalDimming::BigReportSize);
	bool sent;
	if(!owner.probeComplete){
		sent = owner.WriteToNode(owner.probeNode, report);
	}else{
		sent = owner.SendReport(target, owner.config.hidInterface, "zone frames", report);
	}

	// The first node that took a 1025 byte write is the local dimming device, which is a separate USB
	// identity from the command device, so it has to be kept for the rest of the run.
	if(sent && !target.empty() && owner.targets.dataNode.empty()){
		owner.targets.dataNode = target;
	}

	// A node with a 65 byte output report rejects a 1025 byte write, so this is what tells the command
	// path apart from the local dimming device in one run.
	if(!loggedBigReport && sent && report.size() == LocalDimming::BigReportSize){
		loggedBigReport = true;
		LocalDimmingLog("LocalDimming: first data report, %d bytes, flags %d, length field %d, node %s",
			static_cast<int>(report.size()), report[1], report[2] | (report[3] << 8),
			owner.TargetName(target).c_str());
	}

	return sent;
}

bool PimaxLocalDimming::RunFrame(){
	if(!config.enable){
		return false;
	}

	if(!loggedFirstFrame){
		loggedFirstFrame = true;
		// This is the only way to tell a headset that is not being presented from a transport that
		// cannot write: without it both cases look the same in the log. The pattern name is logged
		// because a device run is judged by what the pattern should look like, so the log has to say
		// which pattern the live driver resolved, not just that a pattern was on.
		LocalDimmingLog("LocalDimming: first frame, grid %dx%d, test pattern %d, pattern %s, dark %d",
			config.gridWidth, config.gridHeight, config.useTestPattern ? 1 : 0,
			LocalDimming::TestPatternModeName(LocalDimming::ResolveTestPatternMode(config)).c_str(),
			config.testPatternGrey);
	}

	// Pimax software stays running to keep the headset powered, so this feature has to open its own
	// handle instead of relying on the command path, which is only opened when there is no PVR session.
	if(config.probeNodes && !probeComplete){
		// The head check error is keyed to where the zone frame arrives, not to what it contains, so
		// the probe writes one complete group to each node in turn. Each group is treated as a fresh
		// connection so the record, the level and the zones are all sent again, the group is logged
		// before it is sent, and every write is logged with its result, so the k-th group in the
		// headset log is the k-th node here and failed writes are visible as gaps.
		if(probeNodePaths.empty()){
			probeNodePaths = FindPimaxNodes();
			if(probeNodePaths.empty()){
				LocalDimmingLog("LocalDimming: probe found no Pimax HID nodes");
				probeComplete = true;
				return false;
			}
			if(probeNodePaths.size() > LocalDimming::ProbeNodeLimit){
				probeNodePaths.resize(LocalDimming::ProbeNodeLimit);
			}
			LocalDimmingLog("LocalDimming: probing %d nodes", static_cast<int>(probeNodePaths.size()));
		}
		runner.Reset();
		probeNode = probeNodePaths[probeIndex];
		probeIndex++;
		LocalDimmingLog("LocalDimming: probe group %d, node %s", probeIndex, LocalDimming::NodeLabel(probeNode).c_str());
		if(probeIndex >= static_cast<int>(probeNodePaths.size())){
			probeComplete = true;
		}
	}else{
		// The probe is only worth running if the run keeps the node it found, so each report size keeps
		// the node that accepted it: the data report goes to the node that took a 1025 byte write and the
		// param frame to the node that took a 65 byte one. A node label in the config wins over the
		// discovery, and a size the probe found nothing for falls back to the configured interface number,
		// which is the only destination left now that there is no command handle.
		if(!loggedTargets){
			loggedTargets = true;
			if(config.probeNodes){
				LocalDimmingLog("LocalDimming: probe targets, command node %s, data node %s",
					TargetName(TargetFor(LocalDimming::ReportSize)).c_str(),
					TargetName(TargetFor(LocalDimming::BigReportSize)).c_str());
				// The record and the level go to the command node and the zone data to the data node, so
				// the group is sent again from the top now that the two targets are fixed.
				runner.Reset();
				transport.ResetLogged();
			}
		}
		probeNode = "";
		if(!OpenLocalDimmingTarget(TargetFor(LocalDimming::BigReportSize), config.hidInterface)){
			return false;
		}
	}

	// the test pattern when the config asks for it, otherwise whatever the compositor has written
	LocalDimming::ZoneImageSource* left = &testPatternLeft;
	LocalDimming::ZoneImageSource* right = &testPatternRight;
#ifdef _WIN32
	if(!config.useTestPattern){
		left = &captureLeft;
		right = &captureRight;
	}
#endif

	runner.SetImageSources(left, right);

	bool sent = runner.RunFrame(NowSeconds());

	if(sent && !loggedFirstSend){
		loggedFirstSend = true;
		// The number of blocks is what tells a device run whether the whole grid was sent: the headset
		// logs one line per block only for the param frames, and nothing at all for the data report, so
		// a 1152 byte grid (24x24 per eye, two 1025 byte reports) is only visible here.
		LocalDimmingLog("LocalDimming: first zone frame sent, %d zones per eye, %d blocks",
			config.gridWidth * config.gridHeight, runner.LastBlockCount());
	}

	return sent;
}
