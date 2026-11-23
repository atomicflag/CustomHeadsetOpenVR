#pragma once
#include <array>
#include <cstdint>
#include <string>

/*
Per zone backlight for the Pimax Crystal Light. Every number here is a confirmed fact, so it is a constant.

Data report, the form Pimax's own path writes (block builder FUN_140094f60, write FUN_140099660):

    [0] report id 0, [1] flags, [2..3] block length as 16 bit, [4..] the zone data, zero padded to 1025

written with hid_write(handle, buffer, 0x401). Flags: base 3, 0x40 on the block that carries the whole
remaining array, which is the first block, and 0x80 on the block that was not capped, which is the last one.
A block is capped at 1021, so the 1152 byte grid is two reports, 1021 plus 131, and a single block transfer
carries both bits.

The zone data is what dims the panel. The entitlement record and the level are the two named writes the
device needs before it honours it, and they go as command frames to the command node.

The grid size is a constant, so every buffer is a fixed array and the per tick path allocates nothing.
*/
namespace LocalDimming{

constexpr int GridRows = 24;
constexpr int GridCols = 24;
constexpr int ZonesPerEye = GridRows * GridCols;   // 576
constexpr int EyeCount = 2;

// The confirmed mapping: plain array order, half 0 is the physical left eye.
size_t ZoneIndex(int eye, int row, int col);

// The luminance weights as pi_server has them at 0x140182320. They sum to 256, so pure grey maps to itself.
// The block reduce on the GPU uses them; the CPU does not, so here they only generate the HLSL defines.
constexpr int LuminanceWeightR = 183;
constexpr int LuminanceWeightG = 54;
constexpr int LuminanceWeightB = 19;
constexpr int LuminanceShift = 8;

// The temporal response. A movement inside the deadband holds the zone on its last accepted value; a bigger
// one moves gradually. Black level and gamma are identity on the confirmed values, so nothing is left of them.
// The input is a grid byte, so there is no clamp on this side: the clamp that matters is pi_server's, on its own
// input, on its side. A negative previous is the first frame for that zone, which has nothing to respond to.
constexpr int ZoneSmoothWeight = 3;
constexpr int ZoneSmoothDeadband = 4;
int ZoneValue(uint8_t blockLuminance, int previous);

struct Config{
	bool enable = true;
};

constexpr double SendIntervalMs = 20.0;
constexpr uint8_t DimmingLevel = 100;           // ld_level, sent once before any zone data
constexpr uint32_t EntitlementRecord = 0x100020b;

// The data report.
constexpr size_t DataReportSize = 1025;
constexpr size_t DataChunk = 1021;            // the builder's block cap, 0x3fd
constexpr uint8_t DataFlagsBase = 3;
constexpr uint8_t DataFlagsFirstBlock = 0x40;
constexpr uint8_t DataFlagsLastBlock = 0x80;

// The command frame the record and the level go as. The packet guard in FUN_1400c86f0 takes a packet below
// 0x40, and the packet is 3 header bytes plus the payload, so the payload is at most 60.
constexpr size_t CommandReportSize = 65;
constexpr size_t EntitlementPayloadSize = 4;   // the `cmd 86 opt 5 datalen 4` line the headset logs
constexpr char LevelName[] = "ld_level";
constexpr size_t LevelPayloadSize = sizeof(LevelName) - 1 + 2;   // the name, the type byte 1, the value
constexpr uint8_t FrameMarker = 0x58;
constexpr uint8_t CmdControl = 0x56;
constexpr uint8_t CmdSetProperty = 0x26;
constexpr uint8_t OptWrite = 0x05;

// The two observed numbers: 27 is the format the desc the hook recorded, 30 is the view format the read that
// worked used for it. Against dxgiformat.h they are R8G8B8A8_TYPELESS and R8G8B8A8_UINT, which is the family a
// view on a typeless resource has to name, and the class the body reads as uint4.
constexpr int TextureFormat = 27;
constexpr int ViewFormat = 30;
struct ChannelOrder{
	int r = 0;
	int g = 1;
	int b = 2;
};
// The order the confirmed format stores its bytes in. Nothing decodes: the view gives the bytes as stored.
constexpr ChannelOrder StoredChannelOrder{0, 1, 2};

// The HID nodes. The path is still read from enumeration, because paths change between runs and Pimax
// software holds its own interfaces.
constexpr int HidVendorId = 0x34A4;
constexpr const char* DataNodeLabel = "hid#vid_34a4&pid_0016";
constexpr const char* CommandNodeLabel = "hid#vid_34a4&pid_0018&mi_00";

struct CaptureRegion{
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// The confirmed texture is one combined texture with the eyes side by side, so the region is one half of it
// at the full height. The size comes from the desc, so the region and the block size stay dynamic. The caller
// only reaches this for a desc that passed the layout filter, so the sizes are positive by construction.
CaptureRegion CaptureRegionForEye(int width, int height, int eye);

// The whole compute source, generated from this module's own numbers so the GPU mirror cannot drift from the
// tested weights. The read path is the unordered access view on the shared texture, so the body needs no copy.
std::string BlockReduceSource();

// The data report, filled in place from one block of the grid: report id 0, flags, the block length as 16 bit,
// then the block, zero padded to 1025.
void BuildDataReport(std::array<uint8_t, DataReportSize>& report, const uint8_t* data, size_t length, int blockIndex, int blockCount);

// The command frame: report id 0, the marker, the packet length, cmd, opt, then the payload.
std::array<uint8_t, CommandReportSize> BuildCommandReport(uint8_t cmd, uint8_t opt, const uint8_t* payload, size_t payloadSize);
std::array<uint8_t, EntitlementPayloadSize> EntitlementPayload(uint32_t record);
std::array<uint8_t, LevelPayloadSize> LevelPayload(uint8_t value);

// A frame's destination is its node label. hidapi reports Windows device paths in upper case, so the known label
// is searched for in the lowercased path. Two nodes can report the same interface number, so the node, not the
// interface number, is what identifies a destination.
bool MatchesNodeLabel(const std::string& path, const std::string& label);

// What decides that a desc is the eye texture: the format the runs observed, one mip level, and the bounds the
// layout requires, not a guess about the render size: one image with the eyes side by side, at least one pixel
// per zone, so it is at least two grids wide and one grid tall.
constexpr int MinEyeImageWidth = 2 * GridCols;   // 48
constexpr int MinEyeImageHeight = GridRows;      // 24
constexpr int EyeTextureMipLevels = 1;
bool LooksLikeEyeTexture(int width, int height, int mips, int format);

bool ShouldInjectCompositorPlugin(bool customShaderEnabled, bool localDimmingEnabled);

// What the local dimming thread waits after a tick. The tick is the whole interval, so the wait is only the time
// left; this is the only place the send interval is measured, the capture has no clock of its own.
double ThreadSleepMs(double nowSeconds, double lastTickSeconds);

}
