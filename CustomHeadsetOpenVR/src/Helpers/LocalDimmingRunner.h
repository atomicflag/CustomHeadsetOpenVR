#pragma once
#include "LocalDimming.h"

/*
The parts that sit around the pure zone math. Nothing here depends on SteamVR, hidapi or D3D11, so
the runner and the test pattern can be exercised without a headset.

  ZoneImageSource  where the runner gets the image for one eye
  Transport        where the runner sends the frame
  Runner           image -> zone values -> report -> send, throttled by config.sendIntervalMs

The zone payload is the left eye then the right eye, which is the order the Pimax software uses for
the 54 byte per frame payload (27 zones per eye). That order is the array half order, so source 0 is the
first block of the payload, which is the half an eye dependent pattern such as EyeSplit makes bright.
Each eye's block is written through ZoneIndex, so a zone value lands at the array position the panel
reads as that zone, not at the position it was computed at;
with the default row major order that is the same position, so the payload is unchanged. The payload is
sent as blocks of DataBlockLimit, so a grid bigger than one block is several sends in one frame, which is
what Pimax's chunked builder does.
*/
namespace LocalDimming{

class ZoneImageSource{
public:
	virtual ~ZoneImageSource() = default;
	// reads the current image for one eye as rgb, width * height * 3, row major.
	// Returns false when no image is available for this frame.
	virtual bool Read(std::vector<uint8_t>& rgb, int& width, int& height) = 0;
};

class Transport{
public:
	virtual ~Transport() = default;
	// Sends one frame. The per frame path must not wait for the headset reply.
	virtual bool Send(uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload) = 0;
	// Sends one whole report buffer, which is the 1024 byte data report path.
	virtual bool SendReport(const std::vector<uint8_t>& report) = 0;
};

// One colour per zone, from the configured pattern. Deterministic, so it drives the tests and can be used
// as a config test pattern on the device. A single zone can be overridden with SetZone.
// eye is the eye this source is read for, and it is what an eye dependent pattern such as EyeSplit
// evaluates against. Every other pattern gives the same value for every eye, so the default of 0 leaves
// them as they are.
class TestPatternImageSource : public ZoneImageSource{
public:
	TestPatternImageSource(const Config& config, int blockSize = 16, int eye = 0);
	// grey level for one zone, row major. Zones that have not been set use the pattern from ZoneGrey.
	void SetZone(int zone, uint8_t grey);
	bool Read(std::vector<uint8_t>& rgb, int& width, int& height) override;
private:
	const Config& config;
	int blockSize = 0;
	int eye = 0;
	std::vector<uint8_t> zoneGreys;
};

class Runner{
public:
	Runner(const Config& config, Transport& transport, ZoneImageSource* leftEye, ZoneImageSource* rightEye);

	// the image sources can be swapped when the config changes
	void SetImageSources(ZoneImageSource* leftEye, ZoneImageSource* rightEye);

	// Clears the sent flags and the smoothing state so the next frame sends the record, the level and
	// the zones again. Used by the interface probe, which treats each interface as a fresh connection.
	void Reset();

	// The values the last frame sent for this eye, which is what the next frame responds to.
	const std::vector<uint8_t>& LastZones(int eye) const { return lastZones[eye]; }

	// Called once per frame. Sends at most once every config.sendIntervalMs and returns true when a
	// zone frame was sent.
	bool RunFrame(double nowSeconds);

	// how many blocks the last frame was sent as, which is the part of the run that a device test can
	// only be read from the driver log
	int LastBlockCount() const { return lastBlockCount; }

private:
	const Config& config;
	Transport& transport;
	ZoneImageSource* leftEye;
	ZoneImageSource* rightEye;
	std::vector<uint8_t> lastZones[2];
	int lastBlockCount = 0;
	bool entitlementSent = false;
	bool levelSent = false;
	double lastSendTime = -1.0;
};

}
