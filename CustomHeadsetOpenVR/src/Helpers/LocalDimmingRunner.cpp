#include "LocalDimmingRunner.h"

#include <algorithm>

namespace LocalDimming{

TestPatternImageSource::TestPatternImageSource(const Config& config, int blockSize, int eye) :
	config(config), blockSize(blockSize), eye(eye){
}

void TestPatternImageSource::SetZone(int zone, uint8_t grey){
	if(zone >= 0){
		zoneGreys.resize(static_cast<size_t>(zone) + 1, 0);
		zoneGreys[static_cast<size_t>(zone)] = grey;
	}
}

bool TestPatternImageSource::Read(std::vector<uint8_t>& rgb, int& width, int& height){
	if(config.gridWidth <= 0 || config.gridHeight <= 0 || blockSize <= 0){
		return false;
	}

	width = config.gridWidth * blockSize;
	height = config.gridHeight * blockSize;
	rgb.assign(static_cast<size_t>(width) * height * 3, 0);

	// the pattern lives in ZoneGrey, so the same mapping is what the tests check and what the device sees
	TestPatternMode mode = ResolveTestPatternMode(config);

	for(int zoneY = 0; zoneY < config.gridHeight; zoneY++){
		for(int zoneX = 0; zoneX < config.gridWidth; zoneX++){
			size_t zone = static_cast<size_t>(zoneY) * config.gridWidth + zoneX;
			uint8_t grey = zone < zoneGreys.size() ? zoneGreys[zone] : ZoneGrey(mode, zoneX, zoneY, config, eye);
			for(int y = 0; y < blockSize; y++){
				for(int x = 0; x < blockSize; x++){
					size_t px = ((static_cast<size_t>(zoneY) * blockSize + y) * width +
						(static_cast<size_t>(zoneX) * blockSize + x)) * 3;
					rgb[px] = rgb[px + 1] = rgb[px + 2] = grey;
				}
			}
		}
	}

	return true;
}

Runner::Runner(const Config& config, Transport& transport, ZoneImageSource* leftEye, ZoneImageSource* rightEye) :
	config(config), transport(transport), leftEye(leftEye), rightEye(rightEye){
}

void Runner::SetImageSources(ZoneImageSource* leftEye, ZoneImageSource* rightEye){
	this->leftEye = leftEye;
	this->rightEye = rightEye;
}

void Runner::Reset(){
	entitlementSent = false;
	levelSent = false;
	lastZones[0].clear();
	lastZones[1].clear();
	lastBlockCount = 0;
	lastSendTime = -1.0;
}

bool Runner::RunFrame(double nowSeconds){
	if(!config.enable){
		return false;
	}

	if(lastSendTime >= 0.0 && nowSeconds - lastSendTime < config.sendIntervalMs / 1000.0){
		return false;
	}

	std::vector<uint8_t> payload;
	for(int eye = 0; eye < 2; eye++){
		ZoneImageSource* source = eye == 0 ? leftEye : rightEye;
		if(source == nullptr){
			return false;
		}

		std::vector<uint8_t> rgb;
		int width = 0, height = 0;
		if(!source->Read(rgb, width, height)){
			return false;
		}

		// The zone responds to the value this eye's last frame sent for it, which is the temporal response.
		std::vector<uint8_t> zones = ComputeZoneValues(rgb, width, height, config, lastZones[eye]);
		if(zones.empty()){
			return false;
		}

		lastZones[eye] = zones;

		// The values are put at the array position the panel reads as this zone's own physical position, not at
		// the position they were computed at, which is what ZoneIndex decides: the panel reading, as the runs
		// recorded it, read back through its inverse, applied to the view position the cell's content belongs
		// to. On a square grid that covers the array once, so every position carries one value; on a grid that
		// is not square a rotation is not a self map, so some positions are written by several zones and some
		// by none, and the ones nobody writes keep the zero the payload was resized to.
		ZoneOrder order = ResolveZoneOrder(config);
		size_t base = payload.size();
		size_t zoneCount = zones.size();
		payload.resize(base + zoneCount, 0);
		for(size_t i = 0; i < zoneCount; i++){
			int row = static_cast<int>(i) / config.gridWidth;
			int col = static_cast<int>(i) % config.gridWidth;
			payload[base + ZoneIndex(eye, row, col, config.gridHeight, config.gridWidth, order)] = zones[i];
		}
	}

	// The zone data goes out in blocks of DataBlockLimit, which is 57 data bytes for the 65 byte param
	// frame with the sub-header and 60 without it, and 255 for the data report with the sub-header (its
	// own length is one byte) and the whole 1021 byte chunk without it. Pimax's builder FUN_140094f60
	// loops, so a grid bigger than one block is sent as several reports in the same frame: a 576 zone
	// per eye panel is 1152 bytes, which is 1021 plus 131, so two reports with the first block flag on
	// the first and the last block flag on the last.
	size_t blockLimit = DataBlockLimit(config);
	int blockCount = static_cast<int>((payload.size() + blockLimit - 1) / blockLimit);
	if(blockCount == 0){
		return false;
	}
	lastBlockCount = blockCount;

	lastSendTime = nowSeconds;

	if(config.sendEntitlement && !entitlementSent){
		// The headset log prints `ept_hid head_type[0] head check error` for every zone frame that
		// arrives without this record written first, so the values are received but never applied.
		// Pimax's own sender writes it before it starts sending zones, and its frames are accepted.
		// Retried until the write succeeds, like the level.
		entitlementSent = transport.Send(CmdControl, OptWrite, EntitlementPayload(config.entitlementRecord));
	}

	if(!levelSent){
		// The level is a named setting (`ld_level`), which is the only local dimming write Pimax's own
		// software has. The device logs its state as `etp_ld ld_en:1 ld_s:161`, so the gate is open on
		// our runs; the level is sent anyway because Pimax sends it before any zone data. Retried until
		// the write succeeds, like the record.
		levelSent = transport.Send(CmdSetProperty, OptWrite,
			PropertyPayload("ld_level", static_cast<uint8_t>(config.dimmingLevel)));
	}

	bool sent = false;
	for(int blockIndex = 0; blockIndex < blockCount; blockIndex++){
		size_t offset = static_cast<size_t>(blockIndex) * blockLimit;
		size_t length = std::min(blockLimit, payload.size() - offset);
		std::vector<uint8_t> block(payload.begin() + offset, payload.begin() + offset + length);
		std::vector<uint8_t> frame = BuildDataPayload(config, block, blockIndex);
		if(frame.empty()){
			return sent;
		}
		if(config.useBigReport){
			// the data report has no cmd and opt, the flags byte and the 16 bit length replace them
			std::vector<uint8_t> report = BuildDataReport(config, frame, blockIndex, blockCount);
			if(report.empty()){
				return sent;
			}
			sent = transport.SendReport(report) || sent;
		}else{
			sent = transport.Send(CmdZoneData, OptWrite, frame) || sent;
		}
	}
	return sent;
}

}
