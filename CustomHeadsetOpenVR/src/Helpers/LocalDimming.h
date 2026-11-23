#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace LocalDimming{

constexpr int GridRows = 24;
constexpr int GridCols = 24;
constexpr int ZonesPerEye = GridRows * GridCols;
constexpr int EyeCount = 2;

// zones are in plain array order, half 0 is the physical left eye
size_t ZoneIndex(int eye, int row, int col);

constexpr int LuminanceWeightR = 183;
constexpr int LuminanceWeightG = 54;
constexpr int LuminanceWeightB = 19;
constexpr int LuminanceShift = 8;

constexpr int ZoneSmoothWeight = 1;
constexpr int ZoneSmoothDeadband = 4;
int ZoneValue(uint8_t blockLuminance, int previous);

struct Config{
	bool enable = true;
};

constexpr double SendIntervalMs = 20.0;
constexpr uint8_t DimmingLevel = 100;           // ld_level, sent once before any zone data
constexpr uint32_t EntitlementRecord = 0x100020b;

// a data report is the report id, flags, a 16 bit block length, then the block data zero padded to 1025 bytes
constexpr size_t DataReportSize = 1025;
constexpr size_t DataChunk = 1021;            // the largest block one data report carries
constexpr uint8_t DataFlagsBase = 3;
constexpr uint8_t DataFlagsFirstBlock = 0x40;
constexpr uint8_t DataFlagsLastBlock = 0x80;

// a command packet is 3 header bytes plus at most 60 payload bytes
constexpr size_t CommandReportSize = 65;
constexpr size_t EntitlementPayloadSize = 4;
constexpr char LevelName[] = "ld_level";
constexpr size_t LevelPayloadSize = sizeof(LevelName) - 1 + 2;   // the name, the type byte, the value
constexpr uint8_t FrameMarker = 0x58;
constexpr uint8_t CmdControl = 0x56;
constexpr uint8_t CmdSetProperty = 0x26;
constexpr uint8_t OptWrite = 0x05;

// 27 is R8G8B8A8_TYPELESS, 30 is the R8G8B8A8_UINT view a typeless resource needs
constexpr int TextureFormat = 27;
constexpr int ViewFormat = 30;
struct ChannelOrder{
	int r = 0;
	int g = 1;
	int b = 2;
};
constexpr ChannelOrder StoredChannelOrder{0, 1, 2};

constexpr int HidVendorId = 0x34A4;
constexpr const char* DataNodeLabel = "hid#vid_34a4&pid_0016";
constexpr const char* CommandNodeLabel = "hid#vid_34a4&pid_0018&mi_00";

struct CaptureRegion{
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// the eye texture holds both eyes side by side, so one eye's region is half the width at the full height
CaptureRegion CaptureRegionForEye(int width, int height, int eye);

std::string BlockReduceSource();

void BuildDataReport(std::array<uint8_t, DataReportSize>& report, const uint8_t* data, size_t length, int blockIndex, int blockCount);

std::array<uint8_t, CommandReportSize> BuildCommandReport(uint8_t cmd, uint8_t opt, const uint8_t* payload, size_t payloadSize);
std::array<uint8_t, EntitlementPayloadSize> EntitlementPayload(uint32_t record);
std::array<uint8_t, LevelPayloadSize> LevelPayload(uint8_t value);

// hidapi reports Windows device paths in upper case, so the label is matched case insensitively
bool MatchesNodeLabel(const std::string& path, const std::string& label);

constexpr int MinEyeImageWidth = 2 * GridCols;
constexpr int MinEyeImageHeight = GridRows;
constexpr int EyeTextureMipLevels = 1;
constexpr int MiscFlagsShared = 2;
// a desc is the eye texture when it is the shared one image that holds both eyes side by side
bool LooksLikeEyeTexture(int width, int height, int mips, int format, int miscFlags);

bool ShouldInjectCompositorPlugin(bool customShaderEnabled, bool localDimmingEnabled);

// the wait is only the time left in the send interval
double ThreadSleepMs(double nowSeconds, double lastTickSeconds);

}
