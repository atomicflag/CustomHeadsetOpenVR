#include "LocalDimmingRunner.h"

#include <algorithm>

namespace LocalDimming{

Runner::Runner(const Config& config, Transport& transport, GridSource* gridSource) :
	config(config), transport(transport), gridSource(gridSource){
}

bool Runner::RunFrame(){
	if(!config.enable){
		return false;
	}

	std::array<uint8_t, ZonesPerEye> grid;
	for(int eye = 0; eye < EyeCount; eye++){
		if(gridSource == nullptr || !gridSource->ReadGrid(eye, grid)){
			return false;
		}

		for(int row = 0; row < GridRows; row++){
			for(int col = 0; col < GridCols; col++){
				size_t zone = static_cast<size_t>(row) * GridCols + col;
				int previous = firstForEye[eye] ? -1 : lastZones[eye][zone];
				uint8_t value = static_cast<uint8_t>(ZoneValue(grid[zone], previous));
				lastZones[eye][zone] = value;
				payload[ZoneIndex(eye, row, col)] = value;
			}
		}
		firstForEye[eye] = false;
	}

	int blockCount = static_cast<int>((payload.size() + DataChunk - 1) / DataChunk);
	lastBlockCount = blockCount;

	// the headset takes zone data only after the entitlement record and the level are written
	if(!entitlementSent){
		auto record = EntitlementPayload(EntitlementRecord);
		entitlementSent = transport.SendCommand(BuildCommandReport(CmdControl, OptWrite, record.data(), record.size()));
	}

	if(!levelSent){
		auto level = LevelPayload(DimmingLevel);
		levelSent = transport.SendCommand(BuildCommandReport(CmdSetProperty, OptWrite, level.data(), level.size()));
	}

	bool sent = false;
	for(int blockIndex = 0; blockIndex < blockCount; blockIndex++){
		size_t offset = static_cast<size_t>(blockIndex) * DataChunk;
		size_t length = std::min(DataChunk, payload.size() - offset);
		BuildDataReport(report, payload.data() + offset, length, blockIndex, blockCount);
		sent = transport.SendDataReport(report) || sent;
	}
	return sent;
}

}
