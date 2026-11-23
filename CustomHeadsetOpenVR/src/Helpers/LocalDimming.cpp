#include "LocalDimming.h"

#include <cctype>
#include <cstdlib>

namespace LocalDimming{

size_t ZoneIndex(int eye, int row, int col){
	return static_cast<size_t>(eye) * ZonesPerEye + static_cast<size_t>(row) * GridCols + static_cast<size_t>(col);
}

// Only the response. The input is a grid byte, so there is nothing to clamp here: the clamp is pi_server's, on
// its own input, on its side. A negative previous is a zone with nothing accepted yet, so the byte is the value.
int ZoneValue(uint8_t blockLuminance, int previous){
	int value = static_cast<int>(blockLuminance);

	if(previous < 0){
		return value;
	}
	if(std::abs(value - previous) <= ZoneSmoothDeadband){
		return previous;
	}
	return (ZoneSmoothWeight * previous + value) / (ZoneSmoothWeight + 1);
}

CaptureRegion CaptureRegionForEye(int width, int height, int eye){
	CaptureRegion region;

	int half = width / 2;
	region.height = height;
	if(eye == 1){
		// the column an odd width leaves over goes to eye 1, so the two halves cover the texture exactly once
		region.x = half;
		region.width = width - half;
	}else{
		region.width = half;
	}
	return region;
}

static std::string BlockReduceDefines(){
	std::string defines;
	defines += "#define LUM_R " + std::to_string(LuminanceWeightR) + "\n";
	defines += "#define LUM_G " + std::to_string(LuminanceWeightG) + "\n";
	defines += "#define LUM_B " + std::to_string(LuminanceWeightB) + "\n";
	defines += "#define LUM_SHIFT " + std::to_string(LuminanceShift) + "\n";
	defines += "#define GRID_ROWS " + std::to_string(GridRows) + "\n";
	defines += "#define GRID_COLS " + std::to_string(GridCols) + "\n";
	defines += "#define ZONES_PER_EYE " + std::to_string(ZonesPerEye) + "\n";
	return defines;
}

/*
 One thread per zone, one value per zone, written at the position ZoneIndex gives. HLSL has no single byte
 store on a raw buffer, so each zone gets a four byte slot and the CPU reads the low byte of it. The buffer is
 the whole grid, so the slots one eye's dispatch does not write are the other eye's half.

 On this host's shader compiler, against a texture of two slices with different values per slice, Load takes the
 array slice as its third component, so the coordinate is an int3 and the fourth component has to be left out.
 The confirmed texture has one slice, so the slice is 0.
*/
static const char* blockReduceBody = R"hlsl(

RWTexture2DArray<uint4> source : register(u1);

RWBuffer<uint> grid : register(u0);

cbuffer SampleData : register(b0){
	int4 region;   // x, y, width, height of this eye's region, as CaptureRegionForEye gives it
	int4 block;    // x, y = the block inside that region that belongs to this zone
	int4 eyeInfo;  // x = eye
	int4 swizzle;  // channel index of R, G and B in the source's byte order
};

// the mirror of LocalDimming::ZoneIndex, fed by the defines the pure core generates
int ZoneIndex(int eye, int row, int col){
	return eye * ZONES_PER_EYE + row * GRID_COLS + col;
}

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID){
	int zone = id.x;
	if(zone >= ZONES_PER_EYE){
		return;
	}

	int row = zone / GRID_COLS;
	int col = zone % GRID_COLS;
	int eye = eyeInfo.x;

	uint sum = 0;
	for(int y = 0; y < block.y; y++){
		for(int x = 0; x < block.x; x++){
			uint4 p = source.Load(int3(region.x + col * block.x + x, region.y + row * block.y + y, 0));
			sum += (LUM_R * p[swizzle.x] + LUM_G * p[swizzle.y] + LUM_B * p[swizzle.z]) >> LUM_SHIFT;
		}
	}

	// the average the tested luminance weights give, in integer form
	grid[ZoneIndex(eye, row, col)] = sum / (block.x * block.y);
}
)hlsl";

std::string BlockReduceSource(){
	return BlockReduceDefines() + blockReduceBody;
}

// The report is always 1025 bytes, so the tail of a block shorter than the cap is the zero padding. The flags
// are the two bits the confirmed path sets: 0x40 on the block that carries the whole remaining array, which is
// the first, and 0x80 on the block that was not capped, which for the grid is the second.
void BuildDataReport(std::array<uint8_t, DataReportSize>& report, const uint8_t* data, size_t length, int blockIndex, int blockCount){
	report.fill(0);

	uint8_t flags = DataFlagsBase;
	if(blockIndex == 0){
		flags |= DataFlagsFirstBlock;
	}
	if(blockIndex == blockCount - 1){
		flags |= DataFlagsLastBlock;
	}
	report[1] = flags;
	report[2] = static_cast<uint8_t>(length & 0xFF);
	report[3] = static_cast<uint8_t>((length >> 8) & 0xFF);
	for(size_t i = 0; i < length; i++){
		report[4 + i] = data[i];
	}
}

std::array<uint8_t, CommandReportSize> BuildCommandReport(uint8_t cmd, uint8_t opt, const uint8_t* payload, size_t payloadSize){
	std::array<uint8_t, CommandReportSize> report = {};
	report[1] = FrameMarker;
	report[2] = static_cast<uint8_t>(3 + payloadSize);
	report[3] = cmd;
	report[4] = opt;
	for(size_t i = 0; i < payloadSize; i++){
		report[5 + i] = payload[i];
	}
	return report;
}

// little endian, which is the `cmd 86 opt 5 datalen 4` line the headset logs
std::array<uint8_t, EntitlementPayloadSize> EntitlementPayload(uint32_t record){
	return {
		static_cast<uint8_t>(record & 0xFF),
		static_cast<uint8_t>((record >> 8) & 0xFF),
		static_cast<uint8_t>((record >> 16) & 0xFF),
		static_cast<uint8_t>((record >> 24) & 0xFF)
	};
}

// the name, then the type byte 1 and the value, as 0x1402b8d20 builds it for ld_level
std::array<uint8_t, LevelPayloadSize> LevelPayload(uint8_t value){
	std::array<uint8_t, LevelPayloadSize> payload = {};
	for(size_t i = 0; i < LevelPayloadSize - 2; i++){
		payload[i] = static_cast<uint8_t>(LevelName[i]);
	}
	payload[LevelPayloadSize - 2] = 1;
	payload[LevelPayloadSize - 1] = value;
	return payload;
}

bool MatchesNodeLabel(const std::string& path, const std::string& label){
	std::string lower;
	lower.reserve(path.size());
	for(char c : path){
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return lower.find(label) != std::string::npos;
}

bool LooksLikeEyeTexture(int width, int height, int mips, int format){
	if(format != TextureFormat || mips != EyeTextureMipLevels){
		return false;
	}
	return width >= MinEyeImageWidth && height >= MinEyeImageHeight;
}

bool ShouldInjectCompositorPlugin(bool customShaderEnabled, bool localDimmingEnabled){
	return customShaderEnabled || localDimmingEnabled;
}

double ThreadSleepMs(double nowSeconds, double lastTickSeconds){
	if(lastTickSeconds < 0.0){
		return 0.0;
	}

	double remaining = SendIntervalMs - (nowSeconds - lastTickSeconds) * 1000.0;
	return remaining > 0.0 ? remaining : 0.0;
}

}
