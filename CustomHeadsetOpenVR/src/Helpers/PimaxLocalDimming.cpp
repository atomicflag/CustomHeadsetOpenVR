#include "PimaxLocalDimming.h"
#include "MiscHelper.h"

#include <chrono>
#include <cstdio>

static double NowSeconds(){
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
PimaxLocalDimming::PimaxLocalDimming(const LocalDimmingConfig& config) :
	config(config),
	transport(*this),
	capture(),
	runner(config, transport, &capture){
}
#else
PimaxLocalDimming::PimaxLocalDimming(const LocalDimmingConfig& config) :
	config(config),
	transport(*this),
	runner(config, transport, nullptr){
}
#endif

PimaxLocalDimming::~PimaxLocalDimming(){
	// the thread has to stop before the handles are closed, because it is the thread that writes
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
		double tickStart = NowSeconds();
		RunFrame();
		lastTickSeconds = tickStart;

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
	hid_device* node = HandleForNodeLocked(label, purpose);
	if(node == nullptr){
		return false;
	}

	int written = hid_write(node, report, static_cast<int>(length));
	return written == static_cast<int>(length);
}

bool PimaxLocalDimming::HidTransport::SendCommand(const std::array<uint8_t, LocalDimming::CommandReportSize>& report){
	bool sent = owner.SendToNode(LocalDimming::CommandNodeLabel, "command frames", report.data(), LocalDimming::CommandReportSize);

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
		LocalDimmingLog("LocalDimming: first frame, grid %dx%d, data node %s, command node %s",
			LocalDimming::GridRows, LocalDimming::GridCols, LocalDimming::DataNodeLabel, LocalDimming::CommandNodeLabel);
	}

	bool sent = runner.RunFrame();

	if(sent && !loggedFirstSend){
		loggedFirstSend = true;
		LocalDimmingLog("LocalDimming: first zone frame sent, %d zones per eye, %d blocks",
			LocalDimming::ZonesPerEye, runner.LastBlockCount());
	}

	return sent;
}
