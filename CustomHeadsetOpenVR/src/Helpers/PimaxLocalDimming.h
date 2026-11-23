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

class PimaxLocalDimming{
public:
	PimaxLocalDimming(const LocalDimmingConfig& config);
	~PimaxLocalDimming();

	void Start();

	// runs on the tick thread, never on the render thread
	bool RunFrame();

private:
	void ThreadLoop();

	// command frames go to the command node, zone data to the data node
	class HidTransport : public LocalDimming::Transport{
	public:
		HidTransport(PimaxLocalDimming& owner) : owner(owner) {}
		bool SendCommand(const std::array<uint8_t, LocalDimming::CommandReportSize>& report) override;
		bool SendDataReport(const std::array<uint8_t, LocalDimming::DataReportSize>& report) override;
	private:
		PimaxLocalDimming& owner;
		bool loggedRecord = false;
		bool loggedBigReport = false;
	};

	std::vector<std::string> EnumeratePimaxPaths();

	std::string FindNodePathByLabel(const std::string& label);

	hid_device* HandleForNodeLocked(const std::string& label, const char* purpose);
	hid_device* HandleForNode(const std::string& label, const char* purpose);

	bool SendToNode(const std::string& label, const char* purpose, const uint8_t* report, size_t length);

	const LocalDimmingConfig& config;
	HidTransport transport;
#ifdef _WIN32
	LocalDimmingCapture::SharedTextureCapture capture;
#endif
	LocalDimming::Runner runner;

	bool loggedFirstFrame = false;
	bool loggedFirstSend = false;

	std::map<std::string, hid_device*> nodeHandles;
	std::set<std::string> loggedNodeFailure;
	std::mutex hidLock;
	double lastOpenAttempt = 0;
	bool hidInitialised = false;

	std::thread tickThread;
	std::atomic<bool> threadRunning{ false };
	double lastTickSeconds = -1.0;

	void CloseHandlesLocked();
};
