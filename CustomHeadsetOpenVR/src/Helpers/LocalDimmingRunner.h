#pragma once
#include "LocalDimming.h"
#include <array>

/*
The parts that sit around the zone math. Nothing here depends on SteamVR, hidapi or D3D11.

  GridSource  where the runner gets one eye's grid
  Transport   where the runner sends the frame
  Runner      grid -> the temporal response per zone -> data reports -> send

The payload is the left eye then the right eye, each half in array order, so a zone value is written at the
position ZoneIndex gives for the cell it was computed from. 1152 bytes is two blocks of the data report, so the
grid is block 0 of 1021 and block 1 of 131. Every buffer is a fixed size, so a tick allocates nothing.
*/
namespace LocalDimming{

class GridSource{
public:
	virtual ~GridSource() = default;
	// One eye's grid, one byte per zone in array order, which is the order ZoneIndex gives. The implementation
	// fills exactly ZonesPerEye bytes. Returns false when this eye has nothing accepted for this frame.
	virtual bool ReadGrid(int eye, std::array<uint8_t, ZonesPerEye>& grid) = 0;
};

class Transport{
public:
	virtual ~Transport() = default;
	// Sends one command frame to the command node. The per frame path must not wait for the headset reply.
	virtual bool SendCommand(const std::array<uint8_t, CommandReportSize>& report) = 0;
	// Sends one whole data report to the data node.
	virtual bool SendDataReport(const std::array<uint8_t, DataReportSize>& report) = 0;
};

class Runner{
public:
	Runner(const Config& config, Transport& transport, GridSource* gridSource);

	// The values the last frame accepted for this eye, which is what the next frame responds to.
	const std::array<uint8_t, ZonesPerEye>& LastZones(int eye) const { return lastZones[eye]; }

	// Called once per tick, once per send interval; the transport thread sleeps the interval, so every call is due.
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
	// A zone with nothing accepted yet has no value to respond to, so the first accepted grid byte is the value.
	bool firstForEye[EyeCount] = { true, true };
	bool entitlementSent = false;
	bool levelSent = false;
};

}
