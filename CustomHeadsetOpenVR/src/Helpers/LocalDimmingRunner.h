#pragma once
#include "LocalDimming.h"
#include <array>

namespace LocalDimming{

class GridSource{
public:
	virtual ~GridSource() = default;
	// false when this eye has nothing accepted for this frame
	virtual bool ReadGrid(int eye, std::array<uint8_t, ZonesPerEye>& grid) = 0;
};

class Transport{
public:
	virtual ~Transport() = default;
	virtual bool SendCommand(const std::array<uint8_t, CommandReportSize>& report) = 0;
	virtual bool SendDataReport(const std::array<uint8_t, DataReportSize>& report) = 0;
};

class Runner{
public:
	Runner(const Config& config, Transport& transport, GridSource* gridSource);

	const std::array<uint8_t, ZonesPerEye>& LastZones(int eye) const { return lastZones[eye]; }

	// called once per send interval
	bool RunFrame();

	int LastBlockCount() const { return lastBlockCount; }

private:
	const Config& config;
	Transport& transport;
	GridSource* gridSource;
	std::array<uint8_t, ZonesPerEye> lastZones[EyeCount];
	std::array<uint8_t, ZonesPerEye * EyeCount> payload;   // the whole grid, in ZoneIndex order
	std::array<uint8_t, DataReportSize> report;
	int lastBlockCount = 0;
	// -1 means a zone has nothing accepted yet
	bool firstForEye[EyeCount] = { true, true };
	bool entitlementSent = false;
	bool levelSent = false;
};

}
