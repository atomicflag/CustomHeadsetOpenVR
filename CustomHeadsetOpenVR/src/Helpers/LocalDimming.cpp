#include "LocalDimming.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace LocalDimming{

// pi_server uses cvttss2si / cvttsd2si here, which truncate towards zero.
// value is already scaled to 0..255.
static int TruncateToByte(double value){
	return static_cast<int>(std::trunc(value));
}

// The simple form of a config name, so "Left/Right Half" and "left-right-half" name the same thing as
// "leftright". Both name parsers use it so a name with punctuation in it behaves the same way for both.
static std::string SimpleName(const std::string& name){
	std::string lower;
	for(char c : name){
		if((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')){
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
	}
	return lower;
}

TestPatternMode ParseTestPatternMode(const std::string& name, bool testPatternRamp){
	if(name.empty()){
		return testPatternRamp ? TestPatternMode::Ramp : TestPatternMode::Grey;
	}

	std::string lower = SimpleName(name);

	if(lower == "grey" || lower == "gray"){
		return TestPatternMode::Grey;
	}
	if(lower == "ramp"){
		return TestPatternMode::Ramp;
	}
	if(lower == "checkerboard"){
		return TestPatternMode::Checkerboard;
	}
	if(lower == "leftright" || lower == "leftrighthalf"){
		return TestPatternMode::LeftRightHalf;
	}
	if(lower == "quadrants" || lower == "quadrant"){
		return TestPatternMode::Quadrants;
	}
	if(lower == "diagonal"){
		return TestPatternMode::Diagonal;
	}
	if(lower == "eyesplit" || lower == "eye"){
		return TestPatternMode::EyeSplit;
	}
	// an unknown name is not a pattern, so it resolves to the one the config had before patterns existed
	// rather than to something the run would read as a result
	return TestPatternMode::Ramp;
}

std::string TestPatternModeName(TestPatternMode mode){
	switch(mode){
		case TestPatternMode::Grey: return "grey";
		case TestPatternMode::Ramp: return "ramp";
		case TestPatternMode::Checkerboard: return "checkerboard";
		case TestPatternMode::LeftRightHalf: return "leftright";
		case TestPatternMode::Quadrants: return "quadrants";
		case TestPatternMode::Diagonal: return "diagonal";
		case TestPatternMode::EyeSplit: return "eyesplit";
	}
	return "ramp";
}

ZoneOrder ParseZoneOrder(const std::string& name){
	std::string lower = SimpleName(name);

	if(lower == "columnmajor" || lower == "column"){
		return ZoneOrder::ColumnMajor;
	}
	// An unknown name is not a reading of the array, so it falls back to the identity, which is what every
	// run so far has been judged against, rather than reordering the array on a name nobody chose.
	return ZoneOrder::RowMajor;
}

std::string ZoneOrderName(ZoneOrder order){
	switch(order){
		case ZoneOrder::ColumnMajor: return "columnmajor";
		case ZoneOrder::RowMajor: return "rowmajor";
	}
	return "rowmajor";
}

size_t ApplyZoneTransform(const ZoneTransform& transform, int row, int col, int rows, int cols){
	// The position the transform puts the cell at, as an index in the same rows x cols grid the cell belongs
	// to. No clamp here: a rotation maps a rows x cols grid onto a cols x rows grid, so on a non square grid
	// the position it gives can be outside the grid, and that has to be visible to the caller rather than
	// hidden here. ZoneIndex is the place that clamps.
	int outRow = transform.swap ? col : row;
	int outCol = transform.swap ? row : col;

	// Each flip uses the size of the axis its output came from, which under a swap is the other axis. That is
	// what makes a flip a mirror of the view rather than a mirror of the array.
	int rowAxis = transform.swap ? cols : rows;
	int colAxis = transform.swap ? rows : cols;
	if(transform.flipRow){
		outRow = rowAxis - 1 - outRow;
	}
	if(transform.flipCol){
		outCol = colAxis - 1 - outCol;
	}

	return static_cast<size_t>(outRow) * static_cast<size_t>(cols) + static_cast<size_t>(outCol);
}

ZoneTransform InvertZoneTransform(const ZoneTransform& transform){
	// Without a swap each flag stands on its own axis, so each flip is its own inverse and the transform is
	// unchanged. With a swap the two outputs come from the other axis, so the two flips exchange: that is why
	// the counter-clockwise and clockwise elements are inverses of each other, and why antitranspose is its
	// own inverse.
	if(!transform.swap){
		return transform;
	}
	ZoneTransform inverse = transform;
	inverse.flipRow = transform.flipCol;
	inverse.flipCol = transform.flipRow;
	return inverse;
}

// The reading the pattern runs recorded, per eye: eye 0 is mirrored horizontally, eye 1 vertically, and the
// identity order reads the array in the order the values were computed. A pattern run can only measure this
// half, so it is kept as the two facts the runs gave, not as arithmetic.
ZoneTransform PanelReading(int eye, ZoneOrder order){
	ZoneTransform reading;
	if(order == ZoneOrder::ColumnMajor){
		reading.flipCol = eye == 0;   // mirrorhorizontal
		reading.flipRow = eye == 1;   // mirrorvertical
	}
	return reading;
}

// Run 20, and the same pair from the independent observation: counter-clockwise in the left eye, clockwise in
// the right. Both are rotations, so the swap is on, and the two flips are what separates the two directions.
// They are inverses of each other, which is why the composition gives the two eyes different positions even
// though both readings are the same kind of flip.
ZoneTransform ContentOrientation(int eye){
	// Hard-coded: no orientation. Both content runs reported the rotation that the applied orientation
	// produced, so the applied rotation is the whole error, and identity is the only mapping consistent with
	// both reports (run 22 with clockwise said the pattern needed counter-clockwise, run 23 with
	// counter-clockwise said it needed clockwise). The panel reading recorded by the pattern runs already
	// puts a cell's value at the zone that shows that cell's content, so the value for a cell goes to the
	// cell's own array position.
	return ZoneTransform{};
}

size_t ZoneIndex(int eye, int row, int col, int rows, int cols, ZoneOrder order){
	if(rows <= 0 || cols <= 0){
		return 0;
	}

	// Clamp so a coordinate outside the grid cannot index past the end of the array. The runner never
	// passes one. The composition is not a self map on a non square grid, because a rotation reads a
	// rows x cols grid as a cols x rows one, so the position the composition gives can be past the end of
	// the array even for a cell inside the grid. Both clamps are here: the coordinates as they always were,
	// and the position that comes out of the composition.
	row = std::max(0, std::min(row, rows - 1));
	col = std::max(0, std::min(col, cols - 1));

	// The whole decision in one composition: the cell's content belongs to the view position
	// ContentOrientation gives, and Invert(PanelReading) turns that view position back into the array
	// position the panel reads it at. So the value computed for a cell lands on the zone that shows that
	// cell's own content.
	size_t viewPosition = ApplyZoneTransform(ContentOrientation(eye), row, col, rows, cols);
	int viewRow = static_cast<int>(viewPosition / static_cast<size_t>(cols));
	int viewCol = static_cast<int>(viewPosition % static_cast<size_t>(cols));

	size_t index = ApplyZoneTransform(InvertZoneTransform(PanelReading(eye, order)), viewRow, viewCol, rows, cols);
	if(index >= static_cast<size_t>(rows) * static_cast<size_t>(cols)){
		// Only reachable on a non square grid, where the rotation leaves the grid it started on. The caller
		// indexes the payload at this value, so it has to stay inside the array.
		index = static_cast<size_t>(rows) * static_cast<size_t>(cols) - 1;
	}
	return index;
}

CaptureRegion CaptureRegionForEye(int width, int height, int arraySize, int eye){
	CaptureRegion region;
	if(width <= 0 || height <= 0){
		// nothing to sample. A degenerate texture gives an empty region rather than a negative one, so the
		// caller's block size check is the only place that has to reject it.
		return region;
	}

	// The two forms are the two ways vrcompositor holds the eye image, and they are not both true at once:
	// a two slice array already holds one eye in one subresource, so the region is the whole subresource.
	if(arraySize == 2){
		region.width = width;
		region.height = height;
		return region;
	}

	// Otherwise it is one combined texture with the eyes side by side, so this eye is one half of it at the
	// full height. eye is read as 0 or 1 because the caller only has two eyes.
	int half = width / 2;
	region.height = height;
	if(eye == 1){
		// The column an odd width leaves over goes to eye 1, which is the same side CaptureEyeIndex reads
		// as eye 1, so the halves do not overlap and together are the texture.
		region.x = half;
		region.width = width - half;
	}else{
		region.width = half;
	}
	return region;
}

int GridSlot(int eye, int row, int col, int rows, int cols){
	// A grid with no cells has no slots, and a position outside the grid has no slot either. Returning -1 for
	// both keeps the caller from indexing a buffer at a position that does not exist, which is the only thing
	// this function can get wrong.
	if(rows <= 0 || cols <= 0){
		return -1;
	}
	// The buffer is the payload, and the payload has exactly two halves, so an eye outside the two is a slot
	// that does not exist rather than a third block.
	if(eye < 0 || eye >= 2){
		return -1;
	}
	if(row < 0 || col < 0 || row >= rows || col >= cols){
		return -1;
	}
	return eye * rows * cols + row * cols + col;
}

std::string BlockAverageDefines(const Config& config){
	// The defines the compute shader is compiled with. Every number here is this module's own: the weights and
	// the shift are the constants `Luminance` uses, and the grid is the config's. The shader is compiled from
	// these lines plus the embedded source, so the mirror reads the same numbers the tests guard.
	std::string defines;
	defines += "#define LUM_R " + std::to_string(LuminanceWeightR) + "\n";
	defines += "#define LUM_G " + std::to_string(LuminanceWeightG) + "\n";
	defines += "#define LUM_B " + std::to_string(LuminanceWeightB) + "\n";
	defines += "#define LUM_SHIFT " + std::to_string(LuminanceShift) + "\n";
	defines += "#define GRID_ROWS " + std::to_string(config.gridHeight) + "\n";
	defines += "#define GRID_COLS " + std::to_string(config.gridWidth) + "\n";
	defines += "#define ZONES_PER_EYE (GRID_ROWS * GRID_COLS)\n";
	defines += "#define EYE_COUNT 2\n";
	return defines;
}

/*
 The body of the block reduce. Every number it uses is the defines above: the weights, the shift and the grid size
 come from the constants `Luminance` uses and from the config, so the mirror cannot drift from the tested function.

 The source is read as integer components, so the bytes the shader averages are the same bytes the CPU path would
 average: an sRGB view format would decode them first, and the tested function never does that.

 Which declaration the body gets is the bind-flag decision, so the same body serves both paths. The unordered access
 view needs an RWTexture2DArray, and the copy path and the shader resource path need a Texture2DArray. `Load` is
 valid on both, so the only thing the paths need separately is the coordinate form, because the two signatures take
 different types. On this host's shader compiler, against a texture of two slices with different values per slice,
 the read showed which component each signature honours as the slice: the int3 form takes the slice as its third
 component, and the int4 form takes it as its third as well with the fourth read as the mip, so the fourth has to be
 0. Putting the slice in the fourth component truncates the int3 form to slice 0 and reads out of bounds on the int4
 form, so the value written would be 0 for every zone.

 One thread per zone, one value per zone, written at the slot GridSlot gives. HLSL has no single byte store on a raw
 buffer (there is no Store8, which was checked on this host's shader compiler), so each zone gets a four byte slot
 and the CPU reads the low byte of it. The buffer is the whole grid, in GridSlot order, so the slots this dispatch
 does not write are the other eye's half.
*/
static const char* blockReduceBody = R"hlsl(

#ifdef READ_AS_UAV
RWTexture2DArray<uint4> source : register(u1);
	// This declaration's Load takes int3, so the array slice is the third component. Passing int4 here silently
	// truncates and drops the slice, which the compiler warns about, and the read lands on slice 0.
	#define LOAD_COORD(x, y, slice) int3(x, y, slice)
#else
Texture2DArray<uint4> source : register(t0);
	// This declaration's Load takes int4 as x, y, slice, mip, so the slice is the third component here too and the
	// fourth has to be 0 for the coordinate to be in bounds. Passing the slice as the fourth reads out of bounds.
	#define LOAD_COORD(x, y, slice) int4(x, y, slice, 0)
#endif

RWBuffer<uint> grid : register(u0);

cbuffer SampleData : register(b0){
	int4 region;   // x, y, width, height of this eye's region, as CaptureRegionForEye gives it
	int4 block;    // x, y = the block inside that region that belongs to this zone
	int4 eyeInfo;  // x = eye, y = the array slice this eye's region is
	int4 swizzle;  // channel index of R, G and B in the source's byte order
};

// the mirror of LocalDimming::GridSlot, fed by the defines the pure core generates
int GridSlot(int eye, int row, int col){
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
			uint4 p = source.Load(LOAD_COORD(region.x + col * block.x + x, region.y + row * block.y + y, eyeInfo.y));
			sum += (LUM_R * p[swizzle.x] + LUM_G * p[swizzle.y] + LUM_B * p[swizzle.z]) >> LUM_SHIFT;
		}
	}

	// the same average the tested function takes, in integer form: sum of the per pixel luminance divided by
	// the number of pixels. The CPU path keeps the average as a double, so a block whose average is not a whole
	// number can come out one lower here. That is the only place the mirror and the function differ.
	grid[GridSlot(eye, row, col)] = sum / (block.x * block.y);
}
)hlsl";

std::string BlockReduceSource(const Config& config, CaptureReadPath path){
	std::string source = BlockAverageDefines(config);
	if(path == CaptureReadPath::UnorderedAccessView){
		// The one line that picks the declaration, so the body stays the same for both paths.
		source += "#define READ_AS_UAV\n";
	}
	source += blockReduceBody;
	return source;
}

int CaptureEyeIndex(int width, int arraySize, int subresource, int dstX){
	if(arraySize == 2){
		return subresource;
	}

	// The half the copy writes to, taken from the region so the eye decision and the region cannot disagree.
	// A texture narrower than 2 has no left half, so every copy is read as eye 1, as it always was.
	CaptureRegion left = CaptureRegionForEye(width, 1, arraySize, 0);
	if(dstX >= left.width){
		return 1;
	}
	return 0;
}

bool LooksLikeEyeTexture(int width, int height, int arraySize, int mips){
	// A desc that is not a texture, or a texture with no pixels, is not an eye image. The hook only calls this
	// for a desc it has already read, so this is the guard rather than the interesting part of the decision.
	if(width <= 0 || height <= 0 || arraySize <= 0){
		return false;
	}

	if(width < MinEyeTextureSide || height < MinEyeTextureSide){
		return false;
	}

	if(arraySize > MaxEyeTextureArraySize){
		return false;
	}

	// A composited per eye image has one level. The 1024x1024, array size 1 texture run 16 recorded has 11
	// levels, so it is a mipmapped resource, not the image the compositor writes, and it has to fail here even
	// though it passes the size and the slice count. Exactly one, not at least one: a texture generated with
	// more levels is not the eye image.
	if(mips != EyeTextureMipLevels){
		return false;
	}

	return true;
}

bool DescChangedForCapture(int previousWidth, int previousHeight, int previousArraySize, int width, int height, int arraySize){
	// The empty case is not a change. With nothing recorded there is no texture to replace, and a desc with no
	// pixels is not a small texture: the capture only records a desc that passed LooksLikeEyeTexture, so a zero
	// or negative size here is a desc that is not a texture rather than a resize.
	if(previousWidth <= 0 || previousHeight <= 0 || previousArraySize <= 0 ||
		width <= 0 || height <= 0 || arraySize <= 0){
		return false;
	}

	// The three fields the capture compares. Any other desc field is not part of this decision, because the
	// capture reads the desc of the opened texture and takes the region and the block size from it, so a change
	// that does not move the size does not need anything recreated.
	return width != previousWidth || height != previousHeight || arraySize != previousArraySize;
}

int VersionArrayScanLimit(int version){
	// The entry count each interface declares, read from the declaration order in the Windows SDK headers,
	// which is the vtable layout. A version this core does not know has no bound, so a scan for it reads
	// nothing, which is the same outcome as a version QueryInterface does not answer to.
	switch(version){
		case 0: return 115;   // IUnknown 3 + ID3D11DeviceChild 4 + the base interface's own 108
		case 1: return 134;   // + 19
		case 2: return 144;   // + 10
		case 3: return 147;   // + 3
		case 4: return 149;   // + 2
		default: return 0;
	}
}

int VersionArrayOffset(int version, uint64_t baseEntry, const std::vector<uint64_t>& versionEntries){
	int limit = VersionArrayScanLimit(version);
	if(limit <= 0 || baseEntry == 0){
		return -1;
	}

	// The bound is the size the interface is known to have. A list shorter than the bound is only read as far
	// as it goes, so a scan is not turned into a read past the end of an array.
	if(static_cast<int>(versionEntries.size()) < limit){
		limit = static_cast<int>(versionEntries.size());
	}

	for(int offset = 0; offset < limit; offset++){
		if(versionEntries[static_cast<size_t>(offset)] == baseEntry){
			// The first match, because an address can appear twice in an array: a wrapper that forwards two
			// slots to the same function is still the entry a call through that slot dispatches through.
			return offset;
		}
	}

	return -1;
}

CaptureReadPath ResolveCaptureReadPath(unsigned int bindFlags){
	// The order is the point: a resource that can be viewed through its own unordered access bind is read
	// through that bind, with no copy, so the flag that is present decides the path before the one that would
	// need a copy. A texture with neither view bit cannot be viewed at all, and that is the case the capture
	// falls back to a copy for.
	if((bindFlags & BindUnorderedAccess) != 0){
		return CaptureReadPath::UnorderedAccessView;
	}
	if((bindFlags & BindShaderResource) != 0){
		return CaptureReadPath::ShaderResourceView;
	}
	return CaptureReadPath::Neither;
}

std::string CaptureReadPathName(CaptureReadPath path){
	switch(path){
		case CaptureReadPath::UnorderedAccessView: return "unordered access view on the opened texture";
		case CaptureReadPath::ShaderResourceView: return "shader resource view on the opened texture";
		case CaptureReadPath::Neither: return "copied into a texture this module created with the shader resource bind";
	}
	return "copied into a texture this module created with the shader resource bind";
}

bool IsBgraFamily(int format){
	// The family, not one member of it. The typeless member has the same byte order as the typed members,
	// because the family name is the storage order, so it belongs here.
	return format == FormatB8G8R8A8Unorm || format == FormatB8G8R8A8UnormSrgb || format == FormatB8G8R8A8Typeless;
}

bool IsTypelessCaptureFormat(int format){
	return format == FormatR8G8B8A8Typeless || format == FormatB8G8R8A8Typeless;
}

int FamilyViewFormat(int format){
	// The unsigned integer member of the format's own family, because the HLSL body reads the source as uint4 and
	// a view on a typeless resource has to name a typed member of that family with the same bits per component.
	// The R8G8B8A8 family has one at 30, so format 27 - the desc run 17 recorded - is read through it. The
	// B8G8R8A8 family has no unsigned integer member at all, so a resource in that family has no format this path
	// can name, and -1 says that plainly instead of asking for a view the runtime would refuse. For a typed
	// member of the first family this is the format the capture already named, so nothing that worked changes.
	if(IsBgraFamily(format)){
		return -1;
	}
	if(format == FormatR8G8B8A8Typeless || format == FormatR8G8B8A8Unorm || format == FormatR8G8B8A8Uint){
		return ViewFormatForByteFamily;
	}
	return -1;
}

bool AcceptedCaptureFormat(int format){
	// Run 17 rejected the right texture on this decision alone: the desc was format 27, the typeless member of
	// the R8G8B8A8 family, and the acceptance test only took the UNORM variants. Acceptance is now the same
	// decision as the view format, because a format is readable exactly when its family has the unsigned integer
	// member the reduce reads as uint4. Formats outside the byte families still fail, so this did not widen the
	// decision; the B8G8R8A8 formats that used to pass are the ones that now fail, and the reason is that a legal
	// view on them cannot name the class this path reads.
	return FamilyViewFormat(format) >= 0;
}

ChannelOrder ChannelOrderForFormat(int format){
	// The channel order is a property of the family, so the typeless members get the order their family stores
	// bytes in. The green index is 1 in both families.
	ChannelOrder order;
	if(IsBgraFamily(format)){
		order.r = 2;
		order.b = 0;
	}
	return order;
}

bool GridsIdentical(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second){
	// An empty grid is not a read, so two empty grids are not the same content. Without this the run that has
	// never read anything would look like the run that read the same thing three times.
	if(first.empty() || second.empty()){
		return false;
	}
	if(first.size() != second.size()){
		return false;
	}
	for(size_t i = 0; i < first.size(); i++){
		if(first[i] != second[i]){
			return false;
		}
	}
	return true;
}

bool ShouldSample(double nowSeconds, double lastSampleSeconds, double intervalMs){
	// Nothing sampled for this eye yet, so the first hook call that records a texture also samples. Without
	// this the capture would have to be read once before it could be sampled at all.
	if(lastSampleSeconds < 0.0){
		return true;
	}

	// The no throttle case. An interval of 0 or less is a config that wants every call.
	if(intervalMs <= 0.0){
		return true;
	}

	// The boundary counts as a sample, so a hook call at exactly the interval is due. A time earlier than the
	// last sample is not a sample, so the throttle only moves forward even if the clock is not monotonic.
	return nowSeconds - lastSampleSeconds >= intervalMs / 1000.0;
}

// zoneX is the column and zoneY is the row of the zone in array space, which is the order this core
// computes and outputs. Every pattern below is named in array space, so a run reads the panel against the
// array and not against itself: the value a zone gets does not depend on how the panel reads the array,
// only on which physical zone ends up showing it, which is what ZoneIndex decides.
// eye is the eye the zone belongs to. Every pattern except EyeSplit is the same for both eyes, so the
// default of 0 is enough for them and none of them changes.
uint8_t ZoneGrey(TestPatternMode mode, int zoneX, int zoneY, const Config& config, int eye){
	int dark = std::max(0, std::min(255, config.testPatternGrey));
	if(zoneX < 0 || zoneY < 0){
		return static_cast<uint8_t>(dark);
	}

	switch(mode){
		case TestPatternMode::Grey:
			return static_cast<uint8_t>(dark);

		case TestPatternMode::Ramp:{
			int zoneCount = config.gridWidth * config.gridHeight;
			if(zoneCount <= 1){
				// a single zone grid has nowhere to ramp to, which is what the old inline code did
				return static_cast<uint8_t>(dark);
			}
			int zone = zoneY * config.gridWidth + zoneX;
			return static_cast<uint8_t>(std::max(0, std::min(255, zone * 255 / (zoneCount - 1))));
		}

		case TestPatternMode::Checkerboard:
			return ((zoneX + zoneY) % 2) == 0 ? 255 : static_cast<uint8_t>(dark);

		case TestPatternMode::LeftRightHalf:
			// an odd width keeps the middle column on the dark side, so the two halves are not even
			return zoneX < config.gridWidth / 2 ? 255 : static_cast<uint8_t>(dark);

		case TestPatternMode::Quadrants:{
			// The halves are decided by the same integer division LeftRightHalf uses, so on an odd grid the
			// middle column belongs to the right half and the middle row to the bottom half, which is the
			// dark side of each split. That keeps the middle line on one side rather than making it its own
			// band, so every zone resolves to one of the four levels and the run can be read zone by zone.
			bool leftHalf = zoneX < config.gridWidth / 2;
			bool topHalf = zoneY < config.gridHeight / 2;
			if(topHalf && leftHalf){
				return static_cast<uint8_t>(QuadrantLevelBright);
			}
			if(topHalf){
				return static_cast<uint8_t>(QuadrantLevelUpperMid);
			}
			if(leftHalf){
				return static_cast<uint8_t>(QuadrantLevelLowerMid);
			}
			return static_cast<uint8_t>(dark);
		}

		case TestPatternMode::Diagonal:
			// row < col in array space, so the bright side is the triangle that holds the top right corner.
			// The diagonal itself is on the dark side, which keeps it on one side like the other patterns do
			// rather than making it its own band. Under row major both eyes show this same triangle, because
			// there is no mirror in the identity mapping; under column major eye 0 and eye 1 show opposite
			// triangle sides, which is the single observation that separates a mirror from a flip.
			return zoneY < zoneX ? 255 : static_cast<uint8_t>(dark);

		case TestPatternMode::EyeSplit:
			// The first half of the zone array, which is one eye's block in the payload, is bright and the
			// second half is dark. Every other pattern gives both eyes the same values, so the value here
			// depends on the eye, and that is the whole point of the pattern: the field inside each half is
			// uniform, so no permutation of the array can change what a half looks like, and the run answers
			// only which physical eye the panel reads as eye 0. It cannot say anything about the mirror or
			// the flip, because a uniform field is insensitive to both.
			return eye == 0 ? 255 : static_cast<uint8_t>(dark);
	}

	return static_cast<uint8_t>(dark);
}

int ZoneValue(double blockLuminance, const Config& config, int previous){
	blockLuminance = std::max(0.0, std::min(255.0, blockLuminance));

	// black level as a floor, with the range above it compressed back over 0..1. pi_server does
	// this as value01 * (1 - bl) + bl in float, then scales back by 255; keeping the value in the
	// 0..255 scale gives the same result without a divide then multiply round trip.
	double blackLevel = std::max(0.0, std::min(1.0, config.blackLevel));
	double value = blockLuminance * (1.0 - blackLevel) + blackLevel * 255.0;

	int quantised = TruncateToByte(value);
	quantised = std::max(0, std::min(255, quantised));

	// gamma as an inverse exponent on the quantised value
	double out01 = quantised / 255.0;
	if(config.gamma > 0.0 && config.gamma != 1.0){
		out01 = std::pow(out01, 1.0 / config.gamma);
	}

	int out = TruncateToByte(out01 * 255.0);
	out = std::max(0, std::min(config.maxZoneValue, out));

	// The response, in the same domain as previous, which is the value the panel was last driven with for this
	// zone. A movement the panel cannot show leaves the zone where it is; a bigger one moves gradually rather
	// than in one jump.
	if(previous >= 0){
		int delta = out - previous;
		if(std::abs(delta) <= ZoneSmoothDeadband){
			return previous;
		}
		out = (ZoneSmoothWeight * previous + out) / (ZoneSmoothWeight + 1);
	}

	return std::max(0, std::min(config.maxZoneValue, out));
}

std::vector<uint8_t> ComputeZoneValues(const std::vector<uint8_t>& rgb, int width, int height,
	const Config& config, const std::vector<uint8_t>& previous){
	std::vector<uint8_t> zones;
	if(width <= 0 || height <= 0 || config.gridWidth <= 0 || config.gridHeight <= 0){
		return zones;
	}
	if(rgb.size() < static_cast<size_t>(width) * height * 3){
		return zones;
	}

	int blockWidth = width / config.gridWidth;
	int blockHeight = height / config.gridHeight;
	if(blockWidth <= 0 || blockHeight <= 0){
		// the image is smaller than one block per zone
		return zones;
	}

	size_t zoneCount = static_cast<size_t>(config.gridWidth) * config.gridHeight;
	zones.reserve(zoneCount);
	bool canSmooth = previous.size() == zoneCount;

	for(int zoneY = 0; zoneY < config.gridHeight; zoneY++){
		for(int zoneX = 0; zoneX < config.gridWidth; zoneX++){
			uint64_t sum = 0;
			int count = 0;
			for(int y = zoneY * blockHeight; y < (zoneY + 1) * blockHeight; y++){
				for(int x = zoneX * blockWidth; x < (zoneX + 1) * blockWidth; x++){
					size_t i = (static_cast<size_t>(y) * width + x) * 3;
					sum += Luminance(rgb[i], rgb[i + 1], rgb[i + 2]);
					count++;
				}
			}

			// pixels left over by an image size that is not a multiple of the grid are ignored
			size_t zone = static_cast<size_t>(zoneY) * config.gridWidth + zoneX;
			int previousValue = canSmooth ? previous[zone] : -1;
			zones.push_back(static_cast<uint8_t>(ZoneValue(static_cast<double>(sum) / count, config, previousValue)));
		}
	}

	return zones;
}

std::vector<uint8_t> BuildReport(uint8_t cmd, uint8_t opt, const std::vector<uint8_t>& payload){
	if(payload.size() > MaxPayload){
		return {};
	}

	std::vector<uint8_t> report(ReportSize, 0);
	report[0] = ReportId;
	report[1] = FrameMarker;
	report[2] = static_cast<uint8_t>(3 + payload.size());
	report[3] = cmd;
	report[4] = opt;
	for(size_t i = 0; i < payload.size(); i++){
		report[5 + i] = payload[i];
	}
	return report;
}

std::vector<uint8_t> EntitlementPayload(uint32_t record){
	return {
		static_cast<uint8_t>(record & 0xFF),
		static_cast<uint8_t>((record >> 8) & 0xFF),
		static_cast<uint8_t>((record >> 16) & 0xFF),
		static_cast<uint8_t>((record >> 24) & 0xFF)
	};
}

std::vector<uint8_t> PropertyPayload(const std::string& name, uint8_t value){
	std::vector<uint8_t> payload(name.begin(), name.end());
	// the type byte Pimax puts between the name and the value (0x1402b935b, then the value at 0x1402b9372)
	payload.push_back(1);
	payload.push_back(value);
	return payload;
}

std::vector<uint8_t> BuildDataPayload(const Config& config, const std::vector<uint8_t>& data, int blockIndex){
	if(data.size() > DataBlockLimit(config)){
		return {};
	}

	if(!config.useDataHeader){
		// Pimax's builder copies the caller's buffer verbatim after its own header, so with the
		// sub-header off the payload is the data and the length field is the data count.
		return data;
	}

	std::vector<uint8_t> payload;
	payload.push_back(static_cast<uint8_t>(config.dataKind & 0xFF));
	payload.push_back(static_cast<uint8_t>(blockIndex & 0xFF));
	payload.push_back(static_cast<uint8_t>(data.size()));
	payload.insert(payload.end(), data.begin(), data.end());
	return payload;
}

std::vector<uint8_t> BuildDataReport(const Config& config, const std::vector<uint8_t>& payload, int blockIndex, int blockCount){
	if(payload.size() > MaxBigData){
		return {};
	}

	std::vector<uint8_t> report(BigReportSize, 0);
	report[0] = ReportId;

	uint8_t flags = static_cast<uint8_t>(config.dataFlags & 0xFF);
	if(blockIndex == 0){
		flags |= DataFlagsFirstBlock;
	}
	if(blockIndex == blockCount - 1){
		flags |= DataFlagsLastBlock;
	}
	report[1] = flags;
	report[2] = static_cast<uint8_t>(payload.size() & 0xFF);
	report[3] = static_cast<uint8_t>((payload.size() >> 8) & 0xFF);
	for(size_t i = 0; i < payload.size(); i++){
		report[4 + i] = payload[i];
	}
	return report;
}

static std::string LowerCase(const std::string& value){
	std::string out;
	out.reserve(value.size());
	for(char c : value){
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

std::string NodeLabel(const std::string& path){
	// hidapi reports Windows device paths in upper case, as `\\?\HID#VID_34A4&PID_0016#7&...`, so the label
	// has to be cut out of the lowercased path instead of searched for in the original. Taking it from the
	// original path returns the whole path, which then matches nothing.
	std::string lower = LowerCase(path);
	size_t start = lower.find("hid#");
	if(start == std::string::npos){
		return lower;
	}
	size_t end = lower.find('#', start + 4);
	return end == std::string::npos ? lower.substr(start) : lower.substr(start, end - start);
}

bool MatchesNodeLabel(const std::string& path, const std::string& label){
	if(label.empty()){
		return false;
	}

	std::string needle = LowerCase(label);
	if(needle.rfind("hid#", 0) != 0){
		// labels are copied out of the log, which prints them with the hid# prefix. Accept either form
		needle = "hid#" + needle;
	}
	return LowerCase(path).find(needle) != std::string::npos;
}

std::string ResolveTarget(const std::string& configured, size_t reportSize, const ProbeTargets& found,
	const std::vector<std::string>& probed){
	const std::string& accepted = reportSize == BigReportSize ? found.dataNode : found.commandNode;

	if(!configured.empty()){
		for(const std::string& path : probed){
			if(MatchesNodeLabel(path, configured)){
				return NodeLabel(path);
			}
		}
	}

	if(!accepted.empty()){
		return accepted;
	}

	// Nothing names a node for this size, so the send uses the configured interface. A label that was
	// never probed still names the node the user pinned, so it is kept rather than dropped. This empty
	// result is not a fallback to a handle: the injected process has no command handle, so the configured
	// interface number is the only destination left, and when that interface is not present the write has
	// nowhere to go.
	return LowerCase(configured);
}

bool ShouldInjectCompositorPlugin(bool customShaderEnabled, bool localDimmingEnabled){
	// Either feature needs the process the hook runs in, so the gate is the two of them together.
	return customShaderEnabled || localDimmingEnabled;
}

double ThreadSleepMs(double nowSeconds, double lastTickSeconds, double sendIntervalMs, double maxSleepMs){
	// Nothing has ticked yet, so the first tick runs at once. Same convention as ShouldSample.
	if(lastTickSeconds < 0.0){
		return 0.0;
	}

	// The no throttle case: an interval of 0 or less asks for every pass.
	if(sendIntervalMs <= 0.0){
		return 0.0;
	}

	double elapsedMs = (nowSeconds - lastTickSeconds) * 1000.0;
	if(elapsedMs < 0.0){
		// The clock moved backwards, so tick again rather than sleeping a negative time.
		return 0.0;
	}
	if(elapsedMs >= sendIntervalMs){
		// The boundary counts as due, so a tick that ran past the interval is not paid back by sleeping.
		return 0.0;
	}

	double remaining = sendIntervalMs - elapsedMs;
	return remaining > maxSleepMs ? maxSleepMs : remaining;
}

}
