#pragma once
#include <cstdint>
#include <string>
#include <vector>

/*
Per zone backlight ("local dimming") for the Crystal family.

Wire format (observed in pi_server.exe, frame builder 0x1400b44f0 and the p3c wrapper 0x1400944c0,
matching the framing this repo already uses in PimaxCommon.cpp):

    [0] 0x00 report id, [1] 0x58 marker, [2] 3 + payload length, [3] cmd, [4] opt, [5..] payload

sent as a 65 byte HID output report. The packet builder FUN_1400b44f0 writes [len + 3][cmd][opt] and
then copies the caller's buffer verbatim, and the async sender FUN_1400c86f0 only takes a packet below
0x40, so one report carries at most 63 packet bytes, which is 60 payload bytes: the builder's zero fill
runs through index 0x3e, which is exactly the last byte of a 60 byte payload, so the packet fills the
report. The headset logs one line per frame it takes in, `cmd <cmd> opt <opt> datalen <payload length>`,
so the `datalen 54` in the firmware log is a 54 byte payload, which is 27 zones per eye over 2 eyes.
That is the block size Pimax happened to send, not a limit.

The zone grid is config driven: `cols_per_eye` and `rows_per_eye` are keys of the hmd config render
block (pi_server.exe 0x140112270, in the same key list as `local_dimming` and `disabled_backlight`).
No config file on disk has them; the only value recovered for this headset is `cols_per_eye` = 3 from a
memory snapshot of the running service (pvr_svr_25-12-05-05-40-39.dmp), so the default grid below is a
placeholder and the grid stays configurable.

The level is a named setting, not a zone frame: `SERVICE::HidServiceForP3C::SendSetLocalDimmingLevel`
(0x1402b8d20) sends `ld_level` + [0x01][value] as cmd 0x26 opt 5, clamped 0..100, and separately [0x01]
[value] as cmd 0x1c opt 5. The device reports the level state as `etp_ld ld_en:1 ld_s:161`, and 161 is
0xa0 + 1, which matches the entitlement gate `localDimmingStatus >= 0xa0`.

Value pipeline, per zone (observed in pi_server.exe, 0x140183481):

    q         = clamp(blockAverage / 255, 0, 1) * (1 - blackLevel) + blackLevel   // black level is a floor
    q         = q * 255
    quantised = truncate(q)
    out       = truncate(pow(quantised / 255, 1 / gamma) * 255)                    // gamma as an inverse exponent

pi_server additionally blends each zone with its 3x3 neighbours in the zone loop FUN_1401822a0, using
integer weights that sum to 16: the kernel is [1 2 1 / 2 4 2 / 1 2 1] (the 9 byte table at 0x141ca2478),
each neighbour is multiplied by its weight and the sum is shifted right by 4. The luminance step in the
same loop is (183 * sample[-1] + 54 * sample[-2] + 19 * sample[0]) >> 8, the same fixed point weights as
Luminance below. This module uses plain block averages and no neighbour blend, so it is not bit exact;
implementing the kernel is separate work and is not done here.

This module has no driver, VR or HID dependencies so it can be unit tested on its own.
*/
namespace LocalDimming{

// The test pattern a zone gets. Only the name matters on the device, since the pattern is what a run is
// judged by: Grey and Ramp cannot show local dimming at all over a 576 zone grid, because every zone gets
// a value close to its neighbours. Checkerboard and LeftRightHalf put a full value next to a dark one, so
// a block boundary is something the eye can actually see.
enum class TestPatternMode{
	Grey,
	Ramp,
	Checkerboard,
	LeftRightHalf,
	// Four levels, one per quadrant, and the quadrants are named in array space, which is the order this
	// core outputs (index = row * cols + col), not the order the panel is read in. The 2026-11-22 run put
	// a left/right split in array space on the panel as a top/bottom split, so the traversal is not
	// row major and the two eyes are traversed in opposite directions. Four different levels read the
	// permutation directly: where each level appears is the mapping, whereas a two level pattern only
	// shows that an edge exists and which side of it is which.
	Quadrants,
	// Bright where row < col in array space, dark otherwise, so the edge is a diagonal and not a split.
	// This is the pattern that settles what the Quadrants run left open: a half panel run says which side
	// of an edge is bright, which cannot separate a mirror from a flip, and four quadrant levels name the
	// permutation but say nothing about whether the diagonal is mirrored. An asymmetric triangle does,
	// because a horizontal mirror and a vertical flip put it on opposite sides, so one binary observation
	// per eye is enough: the same triangle side on both eyes means the mapping is symmetric, different
	// sides mean one eye is mirrored and the other is not.
	Diagonal,
	// Bright for eye 0 and dark for every other eye, so the pattern is not symmetric between the eyes.
	// Every pattern up to this one gives both eyes the same values, so the mirror assignment and the half
	// assignment are coupled: a run can read the traversal either way and the observation is the same. The
	// pattern is one half of the array, which is one eye's block in the payload, and a uniform field inside
	// it is insensitive to any permutation, so the only thing the run answers is which physical eye is
	// bright, which is what fixes which half of the payload the panel reads as the left eye.
	EyeSplit
};

// How the panel reads the zone array. The array is written in the order the image is computed, which is
// row major, and the 2026-11-22 runs showed that the panel does not read it that way: the same array was
// read transposed, with one eye mirrored horizontally and the other not, and the eye split run fixed which
// half of the array the panel reads as which eye. That is confirmed on the device, so this is the default.
// Row major is kept as the fallback for a name this code does not know, and because a run can still be
// judged against the identity.
enum class ZoneOrder{
	RowMajor,
	ColumnMajor
};

// A way a grid can be read, as three flags: whether the two axes are swapped and whether each one is flipped.
// The eight combinations are the eight ways a square grid can be read, so the panel reading and the content
// orientation become named facts instead of arithmetic inside one function. This is what run 20 made necessary:
// the code had one mapping, and it was standing in for two things that are measured separately.
struct ZoneTransform{
	bool swap = false;
	bool flipRow = false;
	bool flipCol = false;
};

// The position the transform puts grid cell (row, col) at.
size_t ApplyZoneTransform(const ZoneTransform& transform, int row, int col, int rows, int cols);

// The inverse. A transform that does not swap is its own inverse; a swap exchanges the two flip flags, which is
// why a clockwise transform and a counter-clockwise one are inverses of each other.
ZoneTransform InvertZoneTransform(const ZoneTransform& transform);

// How the panel reads the array: an array position, as the pattern runs recorded it. A pattern never reads the
// texture, so a pattern run can only measure this half of the mapping.
ZoneTransform PanelReading(int eye, ZoneOrder order);

// Which part of the view the content of a block belongs to. This half cannot come from a pattern run, only from a
// run with real content: run 20 saw the pattern counter-clockwise in the left eye and clockwise in the right, the
// same pair as the crash that showed ground pointing toward the nose in both lenses. It is the orientation the
// image in the texture is stored in, so it is a fact about the device rather than a setting.
ZoneTransform ContentOrientation(int eye);

// The four levels Quadrants gives out, in array space. They are fixed rather than a scale so that a run
// can be read against them: 255, 170, 85 and the configured dark value are four levels that can be ranked
// on a still panel, and each one belongs to one quadrant, so where the levels appear on the panel is the
// traversal order.
constexpr int QuadrantLevelBright = 255;
constexpr int QuadrantLevelUpperMid = 170;
constexpr int QuadrantLevelLowerMid = 85;

constexpr uint8_t ReportId = 0x00;
constexpr uint8_t FrameMarker = 0x58;
constexpr uint8_t CmdZoneData = 0x55;  // 85, the generic data transfer command (param blocks)
constexpr uint8_t CmdControl = 0x56;   // 86, entitlement / control
constexpr uint8_t CmdSetProperty = 0x26;  // 38, set a named setting, e.g. ld_level
constexpr uint8_t OptRead = 0x00;
constexpr uint8_t OptWrite = 0x05;

constexpr size_t ReportSize = 65;
// Payload maximum for one 65 byte command frame. The packet guard in FUN_1400c86f0 takes a packet below
// 0x40, and the packet is 3 header bytes plus the payload, so the payload is at most 60 and the packet
// fills the report exactly.
constexpr size_t MaxPayload = 60;
// The zone data that fits one param frame with the sub-header on: the 3 sub-header bytes come out of the
// payload allowance. With the sub-header off the payload is the data, so the allowance is MaxPayload.
// The `datalen 54` in the firmware log is the block Pimax happened to send (27 zones per eye over 2
// eyes), not a limit.
constexpr size_t MaxDataBlock = MaxPayload - 3;

// The per frame data report Pimax's own local dimming path uses. The block builder FUN_140094f60 builds
// a 1025 byte buffer: [0x00 report id][flags][len lo][len hi] then the data, and FUN_140099660 writes it
// with hid_write(handle, buffer, 0x401). The builder writes only its own 4 header bytes and copies the
// caller's buffer verbatim at offset 4, so there is no sub-header in Pimax's path and the 16 bit length
// is the data count, not 3 + data. This is the path that dims the panel, and it is on a handle Pimax
// opens itself ("open local dimming hid:", 0x14009b79f) chosen by the config keys
// localdimming_vid / localdimming_pid (0x14010fbd6), so it is a different device from the one the
// 65 byte command frames go to.
//
// The flags byte in the builder, read from the decompiled function:
//   base 3, and the block length is min(remaining, 0x3fd) so a block is capped at 1021 bytes
//   0x40 is added on the block where the block length equals the whole remaining array, which is the
//     first block of the transfer (cmp total, remaining / cmove)
//   0x80 is added on the block that was not capped, which is the last block (cmp chunk, remaining / cmove)
// So a single block transfer carries both bits, 0xC3, a multi block transfer is 0x43 on the first
// block, 3 on the middle blocks and 0x83 on the last one.
//
// The builder loops over the array, so a bigger array is more than one report in the same frame, which
// is only worth having when the real zone array is bigger than one chunk. It is: Pimax's published spec
// for the Crystal is 576 zones per eye, so the array is cols_per_eye * rows_per_eye * 2 eyes = 1152 bytes
// for a 24x24 grid, which is 1021 plus 131, two reports.
constexpr size_t BigReportSize = 1025;
// the whole chunk is data, because the builder copies the caller's buffer verbatim after its header
constexpr size_t MaxBigData = 1021;
// the zone data when the sub-header is on, which is our own form and not Pimax's. Its own length is one
// byte, so a block that uses the sub-header can describe at most 255 data bytes even though the chunk is
// 1021: the sub-header is 3 + 255 = 258 bytes and the rest of the report is zero padding. The 16 bit
// report length still describes the whole block, 3 + data. With the sub-header off the whole chunk is
// data, see DataBlockLimit below.
constexpr size_t MaxBigDataBlock = 255;
constexpr uint8_t DataFlagsBase = 3;
constexpr uint8_t DataFlagsFirstBlock = 0x40;
constexpr uint8_t DataFlagsLastBlock = 0x80;

// The entitlement record Pimax writes with cmd 0x56 opt 5 before the device honours zone data
// (pi_server.exe 0x1400b0df6, only when the stored entitlement type is above 0). The headset log
// shows it as `cmd 86 opt 5 datalen 4`. Every frame our sender wrote produced
// `ept_hid head_type[0] head check error`, so the values arrived and were rejected.
constexpr uint32_t DefaultEntitlementRecord = 0x100020b;

// Temporal response, hard-coded. A block average that moves from one accepted sample to the next is read
// noise plus real content change, and on a 24x24 panel the read noise is what looks like flicker, so a zone
// moves gradually. The deadband holds a zone on its previous value when the movement is smaller than the
// panel can show. The trade-off is response time on moving content, which is the part that reads as lag.
constexpr int ZoneSmoothWeight = 3;
constexpr int ZoneSmoothDeadband = 4;
struct Config{
	// zone grid per eye, row major
	int gridWidth = 9;
	int gridHeight = 3;
	// which order the panel reads the zone array in, as the name parsed by ParseZoneOrder: "rowmajor" or
	// "columnmajor", case insensitive. Row major is the order ComputeZoneValues outputs, so each zone value
	// stays where it was computed. Column major is what the panel was confirmed to read on 2026-11-22, so it
	// is the default now and a zone value goes to the zone it was computed for. A settings file that names
	// row major, or a name this code does not know, still gets the identity.
	std::string zoneOrder = "columnmajor";
	// Pimax applies gamma as an inverse exponent: pow(value, 1 / gamma)
	double gamma = 1.0;
	// black level floor, the range above it is compressed back over 0..1
	double blackLevel = 0.0;
	// our own cap on the output byte. pi_server clamps the input luminance to 0..255 and
	// stores the truncated result as a byte, so this is only used when set below 255.
	int maxZoneValue = 255;
	// feature flag. Off by default because it is not yet confirmed that the headset honours
	// cmd 0x55 from a sender other than Pimax software.
	bool enable = false;
	// minimum time between zone frames. Pimax software sends one frame per compositor frame;
	// this keeps the HID writes from stacking up on a 120 Hz panel.
	double sendIntervalMs = 20.0;
	// value for the named setting `ld_level`, sent as the key/value form Pimax uses (cmd 0x26 opt 5).
	// Crystal Light reports support_local_dimming_level 2, and Pimax clamps this to 0..100.
	int dimmingLevel = 2;
	// use the test pattern instead of the compositor image. Useful for confirming that the device
	// honours the zone frame at all, since a flat field makes the zone mapping easy to read.
	bool useTestPattern = false;
	// grey level for every test pattern zone
	int testPatternGrey = 128;
	// ramp each zone from black to white by its index. A flat field only proves the device answers;
	// a ramp also proves the zone order and the grid size. Kept because settings files written before
	// testPatternMode exist: an empty testPatternMode means the ramp when this is on, otherwise grey.
	bool testPatternRamp = false;
	// which pattern to use, as the name parsed by ParseTestPatternMode: "grey", "ramp", "checkerboard",
	// "leftright" or "quadrants". Case insensitive. Empty by default so that a settings file written
	// before this field exists keeps the meaning its testPatternRamp flag already had.
	std::string testPatternMode = "";
	// HID interface to write zone frames to. Pimax config has its own usb_intf for this, and this repo
	// uses interface 0 for commands and interface 2 for the info stream. Interface 0 is the default
	// because the Crystal Light config does not have a separate local dimming identity.
	int hidInterface = 0;
	// HID node to write zone frames to, as the label from the probe log, for example
	// "hid_34a4&pid_0016&mi_00". When this is set it wins over hidInterface, because two nodes can
	// report the same interface number and only the node decides where a frame lands.
	std::string hidNode = "";
	// the 4 byte entitlement record written before the level. See DefaultEntitlementRecord.
	uint32_t entitlementRecord = DefaultEntitlementRecord;
	// write the record once before the level. Turn this off to test whether the device honours zone
	// frames without it, which is what the head check error in the headset log says it does not.
	bool sendEntitlement = true;
	// cmd 0x55 is the generic data transfer command and Pimax's own sender puts a 3 byte sub-header in
	// front of the data: [kind][block index][data length]. The headset dispatches cmd 0x55 by that
	// first byte, and the error we get is `ept_hid head_type[0] head check error`, which is the check on
	// that byte. The head type the PC stores comes from the entitlement record, whose second byte is 2,
	// and the kind PiPlayService uses for its cmd 0x55 blocks is 2. So this is the one part of the zone
	// frame that Pimax's sender has and ours does not, and the flat test pattern made it look like a
	// zone value: with grey 0 the first byte of ours was 0.
	// No kind is bound to local dimming in either PC binary. Kind is the param component: 1 is the
	// permissions component, 2 the activation component, 7 is used with cmd 0x56. So a kind 2 frame is
	// accepted as an activation param block, and accepted is not the same as applied.
	bool useDataHeader = true;
	int dataKind = 2;
	// Send the zone data as the 1024 byte data report instead of the 65 byte param frame. Pimax's own
	// local dimming path does this, which is the difference between the native driver dimming the panel
	// and our param frames being accepted without ever being applied. A node that has a 1024 byte output
	// report rejects a 65 byte write, so the probe has to try the big report to find the device Pimax
	// opens for local dimming.
	bool useBigReport = false;
	// Flags byte of the data report. Pimax starts from 3 before the block flags, and the block flags are
	// 0x40 for the whole array in one block and 0x80 when more blocks follow.
	int dataFlags = DataFlagsBase;
	// write one complete group of frames (record, level, zones) to each Pimax HID node in turn, then
	// stop. The head check error is keyed to where the zone frame arrives, so this is how the right
	// node is found in one run: the headset log lists frames in arrival order, so the k-th group in
	// the log is the k-th node the driver enumerated, and the driver log names each node as it writes.
	bool probeNodes = false;
};

// Guard so a probe run does not spend a whole session on nodes that are not there. Nodes come from
// enumeration rather than a fixed list, because two nodes can report the same interface number.
constexpr int ProbeNodeLimit = 16;

// The largest zone data block for the current config. With the sub-header on the block is capped by the
// sub-header's own one byte length, so it is 255 data bytes for the data report and 57 for the 65 byte
// param frame; with the sub-header off the whole allowance is zone data, which is the full chunk of 1021
// for the data report and 60 for the param frame. A grid bigger than the allowance is not rejected, it is
// sent as several blocks, which is what the builder in FUN_140094f60 does.
inline size_t DataBlockLimit(const Config& config){
	if(config.useBigReport){
		return config.useDataHeader ? MaxBigDataBlock : MaxBigData;
	}
	return config.useDataHeader ? MaxDataBlock : MaxPayload;
}

// Rec.709 luminance, as named constants rather than literals. The values are pi_server's at 0x140182320
// (imul 0xb7, 0x36, 0x13 then shr 8) and they sum to 256, so pure grey maps to itself, which is what lets the
// capture store one luminance byte per zone and have re-luminance be the identity. They are constants because
// the GPU block reduce is generated from them: `BlockAverageDefines` emits these numbers as HLSL defines, so the
// shader mirror cannot use weights that differ from the function the tests guard.
constexpr int LuminanceWeightR = 183;
constexpr int LuminanceWeightG = 54;
constexpr int LuminanceWeightB = 19;
constexpr int LuminanceShift = 8;

inline uint8_t Luminance(uint8_t r, uint8_t g, uint8_t b){
	return static_cast<uint8_t>((LuminanceWeightR * r + LuminanceWeightG * g + LuminanceWeightB * b) >> LuminanceShift);
}

// The pattern name as the config writes it, case insensitive. An empty name keeps the behaviour the
// testPatternRamp flag had on its own, so settings files written before testPatternMode exist still mean
// the same thing. An unknown name falls back to Ramp rather than silently producing a different pattern,
// because a pattern is what a device run is judged by and a wrong one looks like a result.
TestPatternMode ParseTestPatternMode(const std::string& name, bool testPatternRamp);

// the pattern the current config resolves to
inline TestPatternMode ResolveTestPatternMode(const Config& config){
	return ParseTestPatternMode(config.testPatternMode, config.testPatternRamp);
}

// the canonical name, for the driver log, so a device run can be tied to the pattern that was live
std::string TestPatternModeName(TestPatternMode mode);

// The zone order name as the config writes it, case insensitive, in the same form ParseTestPatternMode
// uses. An unknown name falls back to RowMajor, which is the identity, so a settings file with a name this
// code does not know keeps the behaviour the runs already have rather than quietly reordering the array.
ZoneOrder ParseZoneOrder(const std::string& name);

// the order the current config resolves to
inline ZoneOrder ResolveZoneOrder(const Config& config){
	return ParseZoneOrder(config.zoneOrder);
}

// The canonical name, for the driver log, so a device run can be tied to the order that was live.
std::string ZoneOrderName(ZoneOrder order);

// The position in the zone array for the value computed from block cell (row, col) in a rows x cols grid, which
// is the composition of the two facts above: the cell's content belongs to the view position ContentOrientation
// gives, and the array position the panel reads as that zone is the inverse of PanelReading applied to it. The
// two halves are measured by different kinds of run, so they are kept apart - a pattern run measures PanelReading
// because a pattern never reads the texture, and only a run with real content can measure ContentOrientation. Run
// 19 showed what happens when a decision is split across two places and only one of them can be tested; this is
// the whole decision in one function.
//
// With the reading as the pattern runs recorded it and the orientation as run 20 observed it, the composition is
// the plain transpose for both eyes. The reading is the half that is not settled: the diagonal run's recorded
// corners do not match it, because a triangle holding a corner that lies on a diagonal can be bounded by either
// diagonal. The next look at the panel decides it, and the table in tasks/todo.md maps each answer to a value
// here.
size_t ZoneIndex(int eye, int row, int col, int rows, int cols, ZoneOrder order);

// the observed panel reading, without the config
inline size_t ZoneIndex(int eye, int row, int col, int rows, int cols){
	return ZoneIndex(eye, row, col, rows, cols, ZoneOrder::ColumnMajor);
}

// The part of a captured compositor texture that one eye's zone grid is laid over. vrcompositor keeps the
// eye image either as a two slice texture array, where each slice is one eye, or as one combined texture
// with the two eyes side by side, so the region is either a whole subresource or one half of the texture.
// Plain ints with no D3D11 types, so the decision can be checked without a device and the capture source
// can share it with the eye index decision.
struct CaptureRegion{
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// The region for one eye in a width x height texture with arraySize slices. A two slice array gives the
// whole subresource, because that subresource is the eye; anything else gives one half of the texture at
// the full height, eye 0 on the left and eye 1 on the right, with a column left over by an odd width going
// to eye 1 so the two halves cover the texture exactly once. The result is always a valid sub-rectangle of
// the texture, and the zone block size has to be taken from the region rather than from the whole texture.
CaptureRegion CaptureRegionForEye(int width, int height, int arraySize, int eye);

// The position one zone has in the grid buffer: eye major, then row major. This is the order the GPU writes
// and the order the CPU reads, so it is the layout both sides have to agree on, and the compute shader mirrors
// it as an HLSL function fed by the defines below. Returns -1 for a grid with no cells, an eye outside the two
// halves the payload has, or a position outside the grid, so nothing can be indexed at a slot that does not
// exist. GridSlot is where a value was computed; ZoneIndex is where the payload carries it, which is the
// composition, so the two are the same only where the composition is the identity.
int GridSlot(int eye, int row, int col, int rows, int cols);

// The HLSL defines for the GPU block reduce, generated from this module's own numbers: the luminance weights,
// the shift, and the grid size as the config names it (gridWidth is the columns, gridHeight the rows). Plain
// integers, one define per line, so the mirror cannot use weights or a grid that differ from the function the
// tests guard. The 3x3 neighbour kernel is deliberately not here: the kernel stays a pure-core decision on the
// grid, and it is not applied in this run so that the run has one variable.
std::string BlockAverageDefines(const Config& config);

// Which eye a copy into a captured texture belongs to, which is the decision the region-by-x rule gives and is
// unchanged by the region: the destination subresource when the texture is a two slice array, otherwise the half of
// the texture the copy writes to. The half decision is read off the region, so a copy at or past eye 0's
// width is eye 1, which is also where an odd width's middle column is, and a texture too narrow to split
// is eye 1. Height does not matter here, so any height gives the same answer.
int CaptureEyeIndex(int width, int arraySize, int subresource, int dstX);

// The size a texture has to be before it could be one eye's image. Plain ints and no D3D11 types, so the
// CreateTexture2D hook and the tests read the same decision, and the decision can be checked without a device.
// 960 is the smallest per eye panel this repo has: the fake headset's 1080p monitor is 960x1080 and every real
// headset section is 2544x2544 or larger. The compositor's eye image feeds that panel, and supersampling only
// makes it bigger, so nothing that is an eye image is below it. Everything that obviously is not an eye image is
// well below it: a readback is one pixel per zone, so tens of pixels, and a UI or overlay resource is hundreds.
// So 960 is the line between a texture that could be an eye image and one that is not, and it is low enough that
// a run does not miss a per eye texture rendered smaller than the panel.
constexpr int MinEyeTextureSide = 960;

// The slice counts an eye image can have, which are the two forms CaptureRegionForEye already handles: one
// combined texture with the eyes side by side, and a two slice array with one eye per slice. A cube map is 6 and
// a texture array is any other count, and neither is one eye's image, so a texture big enough on both sides but
// with a slice count outside these two is not reported as a plausible eye texture.
constexpr int MaxEyeTextureArraySize = 2;

// The mip levels a composited per eye image has, which is one. The compositor writes the eye image into a
// texture it creates with a single level, so a texture with more than one level is a mipmapped resource and not
// the composited image. Run 16 recorded a 1024x1024, array size 1 texture with 11 levels, which passed the size
// and the slice count, so the level count is what separates it from an eye image. A desc with no levels is not a
// texture, so it fails as well. This is the only addition to the decision; the size and slice rules are
// unchanged.
constexpr int EyeTextureMipLevels = 1;

// True when a width x height texture with arraySize slices could be one eye's image: at least MinEyeTextureSide
// on both sides, a slice count of 1 or 2, and exactly one mip level. A zero or negative size fails, so a desc
// that is not a texture is not mistaken for an eye image.
bool LooksLikeEyeTexture(int width, int height, int arraySize, int mips);

// Whether a desc the hook reports is a different texture from the one the capture already recorded: any
// difference in width, height or array size is a new texture, and the same desc is not one, so a call for a
// texture that is already recorded does nothing and this stays one event per texture rather than per frame work
// on the hook thread. The empty case is not a change: with nothing recorded, or with a desc that has no pixels,
// there is no texture to replace, and a desc with no pixels is not a small texture, it is not a texture. Plain
// ints and no D3D11 types, so the capture and the tests read the same decision.
// This is the decision the render resolution option goes through. That option can change during a run, which
// makes the compositor create a new composited texture at a different size, and the desc is the only thing that
// says so. It is also what makes the staleness line readable: before this decision, a grid that stopped
// changing after such a change was the capture reading the resource the first desc named, which is the texture
// being the old one, possibly a freed allocation, and this decision is what removes that.
bool DescChangedForCapture(int previousWidth, int previousHeight, int previousArraySize, int width, int height, int arraySize);

// How far a scan of one interface version's vtable array is allowed to read, which is the number of entries
// that interface itself declares: IUnknown's 3, then ID3D11DeviceChild's 4, then the base interface's 108, and
// each version's own additions, 19 for version 1, 10 for version 2, 3 for version 3 and 2 for version 4. This
// is a read bound, not an expectation about where an entry sits: the scan is not allowed to look past the array
// the object is known to have, and a version this core does not know gets no bound, so nothing is scanned for
// it. The base bound is the one run 14 confirmed against this runtime, because Flush resolved at 111, which is
// the 105th entry after IUnknown's 3 and ID3D11DeviceChild's 4.
int VersionArrayScanLimit(int version);

// The offset an interface version's array holds the base array's entry at, or -1 when nothing inside that
// version's bound equals it. The entry has to be the same function address the base array's resolved slot
// points to, because the entry is what a call dispatches through: a version whose array holds a different
// value at every offset is not the same function reached through that array, so nothing is installed for it and
// the run says so. Plain addresses in a plain list, so the decision can be checked without a device or a D3D11
// type, and the hook uses this same function to choose the offset rather than keeping its own scan.
int VersionArrayOffset(int version, uint64_t baseEntry, const std::vector<uint64_t>& versionEntries);

// The bind flags as the values d3d11.h gives them, written as plain integers so the read path decision can be
// checked without a device and without including d3d11.h here. The full list is vertex buffer 0x1, index buffer
// 0x2, constant buffer 0x4, shader resource 0x8, stream output 0x10, render target 0x20, depth stencil 0x40,
// unordered access 0x80. The two view bits were swapped in this file: 0x20 is the render target bind and 0x80 is
// the unordered access bind, which is what made a texture bound only as a render target read as an unordered
// access view, and 0x4 is the constant buffer bind, not a texture view bind at all.
//
// For the descs the runs reported: bind flags 168 is 0x20 + 0x80 + 0x8, render target plus unordered access plus
// shader resource, so the desc run 15 reported has both view bits and the decision below reads it as the
// unordered access view. The texture run 16 recorded has bind flags 8, which is the shader resource bit on its
// own, so the shader resource view the run named is the truth for that texture. Run 15's reading of 168 as 176
// was wrong for both reasons: it read the swapped pair as stream out plus render target, and a stream out bind
// does not allow a view.
constexpr unsigned int BindShaderResource = 0x8;
constexpr unsigned int BindStreamOut = 0x10;
constexpr unsigned int BindRenderTarget = 0x20;
constexpr unsigned int BindUnorderedAccess = 0x80;

// Which view the opened shared texture can be reduced through, decided from its bind flags alone. The unordered
// access view first, because a view on the resource itself needs no copy and the desc run 15 observed has the
// unordered access bind. The shader resource view second, for a texture that has only that bind. Neither for a
// texture with no view bind at all, which is the case that has to be copied into a texture this module creates
// with its own bind flags. A bind flag is what decides whether a view can be created for a resource, so nothing
// here uses a view the flags do not allow.
enum class CaptureReadPath{
	UnorderedAccessView,
	ShaderResourceView,
	Neither
};

CaptureReadPath ResolveCaptureReadPath(unsigned int bindFlags);

// The path as the log writes it, so the line that says which path was used names the decision this function made
// rather than a string the capture keeps to itself.
std::string CaptureReadPathName(CaptureReadPath path);

// The whole input the block reduce is compiled from: the defines, the line that picks the declaration for the read
// path, and the body that does the average. This is assembled here rather than in the capture because run 19 showed
// what happens when the parts are held in two places: the capture compiled the defines alone, so the source had no
// entry point and the run could only report that the compile failed. One input, one place, and the checks that the
// entry point and the per path coordinate form are in it can be written against this function.
std::string BlockReduceSource(const Config& config, CaptureReadPath path);

// The format values as dxgiformat.h gives them, written as plain integers so the format decision can be checked
// without a device and without including dxgiformat.h here, in the same form the bind flags above are. Run 17
// recorded format 27 on the composited texture, which is the typeless member of the R8G8B8A8 family: a typeless
// resource stores the same four bytes per pixel as the typed members of its family and keeps that family's
// channel order, but it names no component type, so a view on it has to name a typed format. The two typeless
// members join the formats this path already reads.
constexpr int FormatR8G8B8A8Typeless = 27;
constexpr int FormatR8G8B8A8Unorm = 28;
constexpr int FormatR8G8B8A8Uint = 30;
constexpr int FormatB8G8R8A8Unorm = 87;
constexpr int FormatB8G8R8A8Typeless = 90;
constexpr int FormatB8G8R8A8UnormSrgb = 91;

// The format a view names for one of these resources. A view on a typeless resource has to name a typed member,
// and it has to be a member of the resource's own family with the same number of bits per component: the rule for
// resources is that an R8G8B8A8_TYPELESS resource cannot be viewed as a R32_FLOAT even though the two are the same
// size in memory, so DXGI_FORMAT_R32G32B32A32_UINT (3) is not available to a format 27 resource. The reduce reads
// the source as uint4, so the family has to have an unsigned integer member: the R8G8B8A8 family has one at 30,
// and the B8G8R8A8 family has none at all (dxgiformat.h gives that family UNORM, X8 UNORM, TYPELESS and
// UNORM_SRGB), so a resource in that family cannot be read as raw bytes by this path. That makes the acceptance
// decision and the view format one decision: a format is readable when its family has the integer member, and the
// view names that member. Returns -1 when the family has none, so no view is asked for.
constexpr int ViewFormatForByteFamily = 30;   // DXGI_FORMAT_R8G8B8A8_UINT, the integer member of its own family
int FamilyViewFormat(int format);

// True when the capture can read a format at all, which is the same thing as the family having an unsigned
// integer member. A format outside the byte families has no four byte channel order for the reduce to read, so a
// float format such as DXGI_FORMAT_R16G16B16A16_FLOAT (10) and a depth format such as
// DXGI_FORMAT_D24_UNORM_S8_UINT (45) still fail.
bool AcceptedCaptureFormat(int format);

// Whether a format belongs to the B8G8R8A8 family, which stores its channels in the order B, G, R. The family
// includes the typeless member, because a typeless resource stores its bytes in its own family's order. This
// family has no unsigned integer member, so it is also the reason the reduce cannot read a format in it.
bool IsBgraFamily(int format);

// Whether the format is one of the two typeless members, which is the case where the view has to name a typed
// format because the resource itself names none. The log names this so a run can be read against it.
bool IsTypelessCaptureFormat(int format);

// The index of R, G and B in the source's byte order, which is what the constant buffer's swizzle carries: the
// R8G8B8A8 family is 0, 1, 2 and the B8G8R8A8 family is 2, 1, 0, including the typeless members of each. There is
// no decoding, because the tested Luminance works on the raw bytes, so the view gives the bytes as they are
// stored and the swizzle is the only thing that names the order. Every format this path can read is in the
// R8G8B8A8 family, so for a readable format the order is the identity order; the family decision is what rejects
// the other one.
struct ChannelOrder{
	int r = 0;
	int g = 1;
	int b = 2;
};

ChannelOrder ChannelOrderForFormat(int format);

// Two grids read as the same content: the same length and every zone equal. This is the staleness decision a run
// is read against, and it is the only thing that separates a read of the live per eye image from a read of
// something that is not being presented: three consecutive samples that are identical to each other say the
// texture opened from the shared handle is not the image the compositor is showing. A grid that is flat is still
// a read, so an unchanging flat grid is an answer and not a failure. An empty grid is not a read at all, so two
// empty grids are not identical to each other: a run that has never read anything must not be reported as an
// unchanging grid. Once the render resolution can change, an unchanging grid after such a change means the
// texture being read is the old one, which is what DescChangedForCapture is for.
bool GridsIdentical(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second);

// Whether the capture path should sample now. The hook fires many times per compositor frame, so sampling on
// every call reads the texture back tens of times per frame while the runner only sends every sendIntervalMs.
// A negative lastSampleSeconds means nothing has been sampled for this eye yet, so the first call always
// samples; from then on a sample is due when the time since the last one is at least the interval, so the
// boundary itself counts as a sample and a gap bigger than the interval is allowed rather than made up. An
// interval of 0 or less is the no-throttle case, which asks for every call. Times are seconds as the runner's
// clock gives them and the interval is milliseconds as the config names it; no clock and no D3D11 here, so
// the decision is checkable without a device and the capture code shares it.
bool ShouldSample(double nowSeconds, double lastSampleSeconds, double intervalMs);

// Grey for one zone, row major. 255 is the bright value and config.testPatternGrey is the dark one, so a
// pattern is the pair of them and not a scale, except Quadrants, which uses four fixed levels so the
// observed positions are four distinguishable ones.
// eye is which eye the zone belongs to, 0 for the first half of the zone array. Every pattern except
// EyeSplit gives the same value to both eyes, so the default 0 keeps every existing pattern byte for byte
// as it is; EyeSplit is the one that reads the pattern per eye.
uint8_t ZoneGrey(TestPatternMode mode, int zoneX, int zoneY, const Config& config, int eye = 0);

// One zone value from the average luminance of its block. blockLuminance is the block average
// as a double, 0..255, so it can be fractional. previous is the value the previous accepted sample sent for
// the same zone, or -1 when there is none, which is the case that has no response. Returns the byte to put in
// the payload.
int ZoneValue(double blockLuminance, const Config& config, int previous = -1);

// rgb is width * height * 3, row major. Returns gridWidth * gridHeight zone values, one per zone.
// The image is divided into gridWidth * gridHeight equal blocks and each zone is the average
// luminance of its block, with black level and gamma applied. previous is the last accepted sample's values
// (same length); an empty previous means there is nothing to respond to.
std::vector<uint8_t> ComputeZoneValues(const std::vector<uint8_t>& rgb, int width, int height,
	const Config& config, const std::vector<uint8_t>& previous = {});

// Builds one HID output report. Returns empty if the payload does not fit in a single report.
std::vector<uint8_t> BuildReport(uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload);

// The entitlement record as payload bytes, little endian. A report built from it has 3 + 4 = 7 in the
// length field, which is the `cmd 86 opt 5 datalen 4` line the headset logs.
std::vector<uint8_t> EntitlementPayload(uint32_t record);

// A named setting as payload: the name, then [0x01][value]. This is the shape
// SERVICE::HidServiceForP3C::SendSetLocalDimmingLevel (0x1402b8d20) builds for `ld_level`, which is the
// only local dimming write Pimax's own software has: name + type + value, sent with cmd 0x26 opt 5.
std::vector<uint8_t> PropertyPayload(const std::string& name, uint8_t value);

// The payload for a data transfer command: [kind][block index][data length] then the data, or the data
// alone when the header is off. Returns empty when the data is bigger than DataBlockLimit for this
// config, which is the whole allowance when the header is off.
std::vector<uint8_t> BuildDataPayload(const Config& config, const std::vector<uint8_t>& data, int blockIndex = 0);

// The whole 1025 byte data report for one block: [0x00][flags][len lo][len hi][payload], zero padded.
// `payload` is what the builder copies at offset 4, so its length is what the headset logs as datalen:
// the sub-headed payload when the sub-header is on, the zone data alone when it is off. Returns empty
// when the payload does not fit in one data report.
std::vector<uint8_t> BuildDataReport(const Config& config, const std::vector<uint8_t>& payload, int blockIndex, int blockCount);

// The part of a HID device path that identifies the node, which is the `hid#vid_...&pid_...&mi_..`
// segment. Only the node is in the path, and two nodes can report the same interface number, so this
// is what a frame's destination is identified by. Returned lower case, as Windows reports it.
std::string NodeLabel(const std::string& path);

// True when path is the node identified by label. Case insensitive, because paths come from Windows
// in mixed case and labels are copied out of the log by hand.
bool MatchesNodeLabel(const std::string& path, const std::string& label);

// The node that accepted the first write of each report size during a probe run. A node with a 65 byte
// output report rejects a 1025 byte write, so these are two different USB identities on the Crystal
// Light: 13476:24 takes the param frames and 13476:22, the device the config keys localdimming_vid and
// localdimming_pid select, takes the data reports. Empty when nothing accepted that size.
struct ProbeTargets{
	std::string commandNode = "";   // accepted a ReportSize write
	std::string dataNode = "";      // accepted a BigReportSize write
};

// The node one write of reportSize goes to after the probe. A configured node label wins when it names a
// node the probe wrote to; otherwise the node that accepted this report size is kept; otherwise the
// result is empty, which means the send falls back to the configured interface number. This is what
// makes a probe run keep the device it found instead of using it once and then writing every later
// frame to the interface. In the injected process this is the whole decision, because there is no command
// handle to fall back to: an empty result means the configured interface number and nothing else, and if
// that interface is not present the write has no destination at all rather than one inherited from the
// driver process.
std::string ResolveTarget(const std::string& configured, size_t reportSize, const ProbeTargets& found,
	const std::vector<std::string>& probed);

// Whether the DLL has to be injected into vrcompositor. Local dimming reads the image the compositor
// displays, and the hook that sees it is installed in the copy of the DLL that runs in that process, so the
// capture source and the hook have to be the same process. The gate is the shader feature or the local
// dimming feature, not the shader feature alone: a run with custom shaders off and local dimming on still
// needs the injection, which is the case the user is testing, and nothing is injected when both are off.
bool ShouldInjectCompositorPlugin(bool customShaderEnabled, bool localDimmingEnabled);

// The longest the local dimming thread waits in one go. The send interval is configurable and can be much
// longer than a useful tick, and a settings change has to be picked up on the next tick rather than after a
// long sleep, so the wait is capped.
constexpr double MaxThreadSleepMs = 100.0;

// How long the local dimming thread waits before its next tick, in milliseconds. IVRCompositorPluginProvider
// has no per frame callback, so the runner is driven by this thread rather than by a compositor frame, and
// HID writes have to be off the render thread because a write slower than 5 ms is what Pimax warns about.
// The wait is the remainder of the interval measured from the tick that just ran, so a tick that overran the
// interval is not made up by pushing the next one a whole interval out. A tick that is already due returns 0,
// so the thread does not sleep past a send, and the result is never negative: a clock that moves backwards
// returns 0 as well. A negative last tick means nothing has ticked yet, which is due immediately, and an
// interval of 0 or less is the no throttle case, which ticks on every pass.
double ThreadSleepMs(double nowSeconds, double lastTickSeconds, double sendIntervalMs,
	double maxSleepMs = MaxThreadSleepMs);

}
