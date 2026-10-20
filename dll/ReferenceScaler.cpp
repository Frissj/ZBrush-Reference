// ReferenceScaler64.dll
// Helper for ReferenceScaler.txt. ZScript cannot resize images, so this DLL
// renders the reference image, scaled and positioned, into a document-sized
// PNG that the script imports as a texture and paints in with Color > FillLayer.
//
// All functions use the ZScript [FileExecute] calling convention and return
// 1 on success or a negative error code. Files live next to the DLL.
//
// Build (Linux, mingw-w64): see build.sh.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#define ZFUNC extern "C" __declspec(dllexport) float

static ULONG_PTR gGdiplusToken = 0;
static Gdiplus::Bitmap* gImage = nullptr;
static std::wstring gImagePath;

// Folder containing this DLL, with a trailing backslash.
static std::wstring DllFolder()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&DllFolder), &module);
	wchar_t path[MAX_PATH];
	DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
	std::wstring folder(path, len);
	size_t slash = folder.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"" : folder.substr(0, slash + 1);
}

static std::wstring Widen(const char* text)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (len <= 0)
		return L"";
	std::wstring out(len - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], len);
	return out;
}

static void StartGdiplus()
{
	if (gGdiplusToken)
		return;
	Gdiplus::GdiplusStartupInput input;
	Gdiplus::GdiplusStartup(&gGdiplusToken, &input, nullptr);
}

// The reference path is kept in reference_path.txt so it survives ZBrush
// unloading the DLL between calls.
static std::wstring ConfigPath()
{
	return DllFolder() + L"reference_path.txt";
}

// Loads (or reuses) the reference image. Returns false if it cannot be read.
static bool LoadImage(const std::wstring& path)
{
	if (gImage && path == gImagePath)
		return true;
	delete gImage;
	gImage = nullptr;
	gImagePath.clear();
	StartGdiplus();
	Gdiplus::Bitmap* image = Gdiplus::Bitmap::FromFile(path.c_str());
	if (!image || image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0) {
		delete image;
		return false;
	}
	gImage = image;
	gImagePath = path;
	return true;
}

static bool LoadSavedImage()
{
	FILE* f = _wfopen(ConfigPath().c_str(), L"rb");
	if (!f)
		return false;
	char buf[2048] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return LoadImage(Widen(buf));
}

// Saves the frame as PNG with GDI+'s built-in encoder.
static bool WritePng(const std::wstring& path, Gdiplus::Bitmap& frame)
{
	UINT count = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&count, &size);
	if (size == 0)
		return false;
	Gdiplus::ImageCodecInfo* codecs = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
	Gdiplus::GetImageEncoders(count, size, codecs);
	bool saved = false;
	for (UINT i = 0; i < count; ++i) {
		if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
			saved = frame.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
			break;
		}
	}
	free(codecs);
	return saved;
}

// text: a line for nav_log.txt (the script logs each Live check). Returns 1.
ZFUNC RSLog(char* text, double, char*, int, char*, int, char**)
{
	if (!text)
		return -1;
	if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu %s\n", GetTickCount(), text);
		fclose(f);
	}
	return 1;
}

// The anchor (model scale, x, y and rotation x, y, z recorded by Anchor) is
// kept in anchor.txt next to the DLL, so it survives script reloads and new
// builds, whose sliders start empty.
static std::wstring AnchorPath()
{
	return DllFolder() + L"anchor.txt";
}

// text: "scale x y rx ry rz". Returns 1, or -1 if it cannot be saved.
ZFUNC RSAnchorSave(char* text, double, char*, int, char*, int, char**)
{
	double v[6];
	if (!text || sscanf(text, "%lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	FILE* f = _wfopen(AnchorPath().c_str(), L"wb");
	if (!f)
		return -1;
	fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n", v[0], v[1], v[2], v[3], v[4], v[5]);
	fclose(f);
	return 1;
}

// text: "0" to "5", which saved anchor value. Returns it, or 0 if none.
ZFUNC RSAnchorGet(char* text, double, char*, int, char*, int, char**)
{
	const int i = text ? atoi(text) : -1;
	double v[6] = {0, 0, 0, 0, 0, 0};
	if (i < 0 || i > 5)
		return 0;
	if (FILE* f = _wfopen(AnchorPath().c_str(), L"rb")) {
		if (fscanf(f, "%lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
			v[0] = 0;
		fclose(f);
	}
	return v[0] > 0 ? static_cast<float>(v[i]) : 0;
}

// The DLL's build number, so the script can tell when ZBrush still has an
// older DLL loaded (the overlay keeps it loaded until ZBrush closes).
static const int kDllBuild = 63;

ZFUNC RSVersion(char*, double, char*, int, char*, int, char**)
{
	return kDllBuild;
}

// text: full path of the reference image. Remembers it for RSRender.
// Returns 1, or -1 if the image cannot be read, -2 if the path cannot be saved.
ZFUNC RSSetImage(char* text, double, char*, int, char*, int, char**)
{
	if (!text || !LoadImage(Widen(text)))
		return -1;
	FILE* f = _wfopen(ConfigPath().c_str(), L"wb");
	if (!f)
		return -2;
	fwrite(text, 1, strlen(text), f);
	fclose(f);
	return 1;
}

// Layout of the last rendered frame, "ratio px py anchorX anchorY".
static std::wstring LayoutPath()
{
	return DllFolder() + L"frame_layout.txt";
}

// text: "docW docH ratio px py anchorX anchorY frameName"
// The image's base layout is fitted to the document, centred, keeping its
// aspect ratio. That layout is scaled by ratio about the anchored model
// position and moved to the current model position (px, py), so the image
// tracks the model as if the camera moved. Outside the image is black.
// Writes frameName next to the DLL, or to frameName itself if it is an
// absolute path (no spaces). Returns 1, or:
// -1 bad arguments, -3 no reference image set, -4 could not write the frame.
ZFUNC RSRender(char* text, double, char*, int, char*, int, char**)
{
	double docW, docH, ratio, px, py, ax, ay;
	char frameName[256] = {0};
	if (!text || sscanf(text, "%lf %lf %lf %lf %lf %lf %lf %255s",
			&docW, &docH, &ratio, &px, &py, &ax, &ay, frameName) != 8)
		return -1;
	if (docW < 1 || docH < 1 || ratio <= 0 || docW > 16384 || docH > 16384)
		return -1;
	if (!LoadSavedImage())
		return -3;

	const double imgW = gImage->GetWidth(), imgH = gImage->GetHeight();
	const double fit = (docW / imgW < docH / imgH) ? docW / imgW : docH / imgH;
	const double baseW = imgW * fit, baseH = imgH * fit;
	const double baseX = (docW - baseW) * 0.5, baseY = (docH - baseH) * 0.5;

	const double x = px + ratio * (baseX - ax);
	const double y = py + ratio * (baseY - ay);

	Gdiplus::Bitmap frame(static_cast<INT>(docW), static_cast<INT>(docH), PixelFormat24bppRGB);
	{
		Gdiplus::Graphics g(&frame);
		g.Clear(Gdiplus::Color(255, 0, 0, 0));
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
		g.DrawImage(gImage, Gdiplus::RectF(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y),
			static_cast<Gdiplus::REAL>(baseW * ratio), static_cast<Gdiplus::REAL>(baseH * ratio)));
	}
	// frameName is relative to the DLL folder unless it is an absolute path.
	const std::wstring name = Widen(frameName);
	const bool absolute = name.size() > 2 && name[1] == L':';
	if (!WritePng(absolute ? name : DllFolder() + name, frame))
		return -4;
	// The overlay starts from this layout (see RSOverlayStart).
	if (FILE* f = _wfopen(LayoutPath().c_str(), L"wb")) {
		fprintf(f, "%.6f %.6f %.6f %.6f %.6f\n", ratio, px, py, ax, ay);
		fclose(f);
	}
	return 1;
}

// text: "docW docH frameName [r g b]". Writes a document-sized frame of one
// colour (default black), used to fill the canvas with the overlay's key
// colour. Returns 1, -1 for bad arguments, -4 if the frame cannot be written.
ZFUNC RSBlank(char* text, double, char*, int, char*, int, char**)
{
	double docW, docH;
	int r = 0, g = 0, b = 0;
	char frameName[256] = {0};
	if (!text || sscanf(text, "%lf %lf %255s %d %d %d", &docW, &docH, frameName, &r, &g, &b) < 3 || docW < 1 || docH < 1 || docW > 16384 || docH > 16384)
		return -1;
	StartGdiplus();
	Gdiplus::Bitmap frame(static_cast<INT>(docW), static_cast<INT>(docH), PixelFormat24bppRGB);
	{
		Gdiplus::Graphics gr(&frame);
		gr.Clear(Gdiplus::Color(255, static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b)));
	}
	return WritePng(DllFolder() + Widen(frameName), frame) ? 1 : -4;
}

// The calibration pattern: random black and white blocks at two sizes, the
// large ones aligned with Calibrate's sample grid so the coarse search locks
// on, the small ones so the fine search is exact.
static const int kBigBlocks = 18, kSmallBlocks = 144;

static unsigned PatternBit(unsigned x, unsigned y, unsigned salt)
{
	unsigned h = x * 73856093u ^ y * 19349663u ^ salt * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h & 1;
}

// text: "docW docH frameName". Writes the calibration pattern. Returns 1, -1
// for bad arguments, -4 if the frame cannot be written.
ZFUNC RSPattern(char* text, double, char*, int, char*, int, char**)
{
	double docW, docH;
	char frameName[256] = {0};
	if (!text || sscanf(text, "%lf %lf %255s", &docW, &docH, frameName) != 3 || docW < 1 || docH < 1 || docW > 16384 || docH > 16384)
		return -1;
	StartGdiplus();
	const int w = static_cast<int>(docW), h = static_cast<int>(docH);
	Gdiplus::Bitmap frame(w, h, PixelFormat24bppRGB);
	Gdiplus::Rect r(0, 0, w, h);
	Gdiplus::BitmapData data;
	if (frame.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat24bppRGB, &data) != Gdiplus::Ok)
		return -4;
	for (int y = 0; y < h; ++y) {
		BYTE* row = static_cast<BYTE*>(data.Scan0) + y * data.Stride;
		for (int x = 0; x < w; ++x) {
			const unsigned big = PatternBit(x * kBigBlocks / w, y * kBigBlocks / h, 1);
			const unsigned small = PatternBit(x * kSmallBlocks / w, y * kSmallBlocks / h, 2);
			const BYTE v = static_cast<BYTE>(big * 160 + small * 95);
			row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = v;
		}
	}
	frame.UnlockBits(&data);
	return WritePng(DllFolder() + Widen(frameName), frame) ? 1 : -4;
}

// Settings snapshot, for comparing ZBrush state before and after another
// plugin runs (which resets the script's variables). Values are kept in
// snapshot.txt next to the DLL as "id value" lines.
static std::wstring SnapshotPath()
{
	return DllFolder() + L"snapshot.txt";
}

// text: "clear" empties the snapshot; otherwise "id value" appends a value.
ZFUNC RSPut(char* text, double, char*, int, char*, int, char**)
{
	if (!text)
		return -1;
	const bool clear = strcmp(text, "clear") == 0;
	FILE* f = _wfopen(SnapshotPath().c_str(), clear ? L"wb" : L"ab");
	if (!f)
		return -2;
	if (!clear)
		fprintf(f, "%s\n", text);
	fclose(f);
	return 1;
}

// text: "id". Returns the last value stored for id, or -99999 if none.
ZFUNC RSGet(char* text, double, char*, int, char*, int, char**)
{
	if (!text)
		return -99999;
	const int want = atoi(text);
	FILE* f = _wfopen(SnapshotPath().c_str(), L"rb");
	if (!f)
		return -99999;
	float result = -99999;
	int id;
	double value;
	while (fscanf(f, "%d %lf", &id, &value) == 2) {
		if (id == want)
			result = static_cast<float>(value);
	}
	fclose(f);
	return result;
}

// text: "x y frameName". Returns the frame pixel at (x, y) as R*65536+G*256+B
// (exact in a float), or -1 if the frame cannot be read or (x, y) is outside.
ZFUNC RSPixel(char* text, double, char*, int, char*, int, char**)
{
	double x, y;
	char frameName[256] = {0};
	if (!text || sscanf(text, "%lf %lf %255s", &x, &y, frameName) != 3)
		return -1;
	StartGdiplus();
	const std::wstring name = Widen(frameName);
	const bool absolute = name.size() > 2 && name[1] == L':';
	Gdiplus::Bitmap* frame = Gdiplus::Bitmap::FromFile((absolute ? name : DllFolder() + name).c_str());
	float result = -1;
	if (frame && frame->GetLastStatus() == Gdiplus::Ok && x >= 0 && y >= 0 && x < frame->GetWidth() && y < frame->GetHeight()) {
		Gdiplus::Color c;
		if (frame->GetPixel(static_cast<INT>(x), static_cast<INT>(y), &c) == Gdiplus::Ok)
			result = static_cast<float>(c.GetR() * 65536 + c.GetG() * 256 + c.GetB());
	}
	delete frame;
	return result;
}

// ---------------------------------------------------------------------------
// Overlay window (stages 1 and 2 of the live reference)
// ---------------------------------------------------------------------------
// A click-through layered window owned by ZBrush's main window (so it stays
// above ZBrush but not above other programs), placed over the canvas and
// redrawn on its own thread. It shows the reference with the same layout as
// RSRender, keyed behind the model (see Composite). The DLL pins itself when the overlay
// starts, since ZBrush may unload it between [FileExecute] calls while the
// overlay thread is still running.

struct OverlayState {
	double screenX = 0, screenY = 0;  // screen position of document (0, 0)
	double pixelsPerDoc = 1;          // screen pixels per document pixel
	double docW = 0, docH = 0;
	double ratio = 1, px = 0, py = 0, ax = 0, ay = 0;
	// Rotation offset, in anchored document pixels: how far rotating has
	// moved the model away from the reference. Added to the anchor.
	double offX = 0, offY = 0;
	int alpha = 128;
	bool visible = false;
};

static CRITICAL_SECTION gOverlayLock;
static bool gOverlayLockReady = false;
static OverlayState gOverlay;
static HWND gOverlayWnd = nullptr;
static DWORD gOverlayThreadId = 0;
static const UINT WM_RS_REDRAW = WM_APP + 1;
static const UINT WM_RS_CLOSE = WM_APP + 2;

// Switches the calling thread to physical pixels while it lives. Under
// Windows display scaling ZBrush's own threads see scaled coordinates, which
// do not match a screen capture or the layered window's placement. Needs
// Windows 10 1607 or later; does nothing on older systems.
struct PhysicalPixels {
	typedef DPI_AWARENESS_CONTEXT (WINAPI *SetContext)(DPI_AWARENESS_CONTEXT);
	SetContext set = nullptr;
	DPI_AWARENESS_CONTEXT old = nullptr;
	PhysicalPixels()
	{
		set = reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
		if (set) {
			old = set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
			if (!old)
				old = set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
		}
	}
	~PhysicalPixels()
	{
		if (set && old)
			set(old);
	}
};

static BOOL CALLBACK FindZBrushWindow(HWND wnd, LPARAM param)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(wnd, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(wnd) || GetWindow(wnd, GW_OWNER))
		return TRUE;
	RECT r;
	GetWindowRect(wnd, &r);
	HWND* best = reinterpret_cast<HWND*>(param);
	RECT b = {0, 0, 0, 0};
	if (*best)
		GetWindowRect(*best, &b);
	if ((r.right - r.left) * (r.bottom - r.top) > (b.right - b.left) * (b.bottom - b.top))
		*best = wnd;
	return TRUE;
}

// Stage 2 (keying): the canvas is filled with the key colour, magenta, so
// the model is the only thing drawn on it that is not magenta. The overlay
// is excluded from screen capture (SetWindowDisplayAffinity), captures the
// screen under itself every time the desktop changes (DXGI desktop
// duplication, or BitBlt where that is unavailable), and shows an opaque copy
// of it with each pixel's share of magenta replaced by the reference: the
// model, its edges and X-ray come out right, and magenta never reaches the
// screen. The copy is at most a frame behind what ZBrush drew.
//
// If capture exclusion is unavailable (older Windows) or does not work (a
// test at start sees the overlay in its own capture), it falls back to
// drawing the reference at a constant opacity over everything, as in stage 1.

struct Buffer32 {
	HDC dc = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ old = nullptr;
	unsigned char* bits = nullptr;  // B, G, R, A, top-down
	int w = 0, h = 0;
	bool Resize(int nw, int nh)
	{
		if (nw == w && nh == h && dc)
			return true;
		Free();
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = nw;
		bi.bmiHeader.biHeight = -nh;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		HDC screen = GetDC(nullptr);
		dc = CreateCompatibleDC(screen);
		void* p = nullptr;
		bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
		ReleaseDC(nullptr, screen);
		if (!bmp) {
			DeleteDC(dc);
			dc = nullptr;
			return false;
		}
		old = SelectObject(dc, bmp);
		bits = static_cast<unsigned char*>(p);
		w = nw;
		h = nh;
		return true;
	}
	void Free()
	{
		if (dc) {
			SelectObject(dc, old);
			DeleteObject(bmp);
			DeleteDC(dc);
		}
		dc = nullptr;
		bits = nullptr;
		w = h = 0;
	}
};

static Buffer32 gRefLayer, gCapture, gOutput;
static int gKeyState = 0;     // 1 keying, 0 no capture exclusion, -1 exclusion not working

// Stage 3 (live navigation): ZScript cannot run while the mouse is dragging,
// so the DLL predicts ZBrush's camera from the mouse. A low-level mouse hook
// classifies each drag that starts over the canvas the way ZBrush does:
//   left drag on the model                  sculpting, ignored
//   left drag on the background, Alt held   move
//   ... Alt released during the drag        scale
//   right drag, Alt held                    move
//   right drag, Ctrl held                   scale
//   anything else                           rotate, ignored for now
// Move shifts the model by panGain document pixels per document pixel of
// mouse travel. Scale multiplies its size by exp(scaleY * dy + scaleX * dx)
// for mouse travel (dx, dy) in screen pixels, about a pivot. The gains and
// which pivot ZBrush uses are learned: after each drag, Live passes the exact
// values from TransformGet to RSOverlaySet, which compares them with the
// prediction (see Learn). What has been learned is kept in nav_model.txt.

enum Pivot { kPivotDocCentre, kPivotCursor, kPivotModel, kPivotLearned, kPivotCount };

struct NavModel {
	double panGain = 1;
	double scaleY = 0, scaleX = 0;
	// Least-squares sums for log(scale) = scaleY * dy + scaleX * dx.
	double syy = 0, sxx = 0, sxy = 0, sly = 0, slx = 0;
	double pivotError[kPivotCount] = {0, 0, 0, 0};
	int pivotSamples = 0;
	double learnedX = 0, learnedY = 0;  // mean observed pivot, document pixels
	int moves = 0, scales = 0;
	int pivot() const
	{
		int best = kPivotModel;
		for (int i = 0; i < kPivotCount; ++i)
			if (pivotSamples > 0 && pivotError[i] < pivotError[best])
				best = i;
		return best;
	}
};

enum DragMode { kDragNone, kDragMove, kDragScale, kDragIgnore };
// What a finished drag was, for Live (RSOverlayInfo 19) and the log.
enum DragKind { kKindNone, kKindMove, kKindScale, kKindRotate, kKindSculpt };

struct Gesture {
	bool active = false;
	bool done = false;         // released, waiting for Live's exact values
	bool classify = false;     // overlay thread checks if it started on the model
	bool right = false;
	bool altSeen = false;
	POINT down = {0, 0}, last = {0, 0};
	int mode = kDragNone;
	double ratio0 = 1, px0 = 0, py0 = 0;  // exact layout when the drag started
	// The current move or scale segment: layout when it began, mouse travel
	// since, and for scaling the document position where it began.
	double baseRatio = 1, basePx = 0, basePy = 0;
	double segX = 0, segY = 0;
	double cursorX = 0, cursorY = 0;
	// Totals for learning.
	double moveX = 0, moveY = 0;          // screen pixels moved while moving
	double scaleDx = 0, scaleDy = 0;      // screen pixels moved while scaling
	int scaleSegments = 0;
	bool movedAfterScale = false;
	bool rotated = false;
	int kind = 0;
	double scaleBasePx = 0, scaleBasePy = 0, scaleCursorX = 0, scaleCursorY = 0;  // first scale segment
};

static NavModel gNav;
static double gLastErrorPos = 0, gLastErrorScale = 1;
static int gLastKind = kKindNone;
static bool gMemLocked = false;  // ZBrush's values are read from memory (see MemReport)

// Stacks of this DLL's own threads hold copies of the values too.
static uintptr_t gOwnStacks[2][2] = {{0, 0}, {0, 0}};

static void NoteOwnStack(int slot)
{
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	gOwnStacks[slot][0] = reinterpret_cast<uintptr_t>(tib->StackLimit) & ~static_cast<uintptr_t>(0xfffff);
	gOwnStacks[slot][1] = reinterpret_cast<uintptr_t>(tib->StackBase);
}

static bool OwnThreadStack(uintptr_t lo, uintptr_t hi)
{
	for (auto& r : gOwnStacks)
		if (r[1] && r[0] < hi && lo < r[1])
			return true;
	return false;
}

static Gesture gPending;      // last finished navigation drag, until Live reports
static int gPendingCount = 0; // navigation drags since Live last reported
static int gStaleSets = 0;    // reports ignored as not including the drag yet
static Gesture gGesture;
static HHOOK gMouseHook = nullptr, gKeyHook = nullptr;
static int gHookEvents = 0;
static volatile bool gAltDown = false, gCtrlDown = false;
static std::vector<unsigned char> gKeyShare;  // per pixel, 0-255 share of magenta
static int gKeyW = 0, gKeyH = 0;

static std::wstring NavPath()
{
	return DllFolder() + L"nav_model.txt";
}

static void SaveNav()
{
	FILE* f = _wfopen(NavPath().c_str(), L"wb");
	if (!f)
		return;
	const NavModel& n = gNav;
	fprintf(f, "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %.9g %.9g %d %d\n",
		n.panGain, n.scaleY, n.scaleX, n.syy, n.sxx, n.sxy, n.sly, n.slx,
		n.pivotError[0], n.pivotError[1], n.pivotError[2], n.pivotError[3], n.pivotSamples,
		n.learnedX, n.learnedY, n.moves, n.scales);
	fclose(f);
}

static void LoadNav()
{
	FILE* f = _wfopen(NavPath().c_str(), L"rb");
	if (!f)
		return;
	NavModel n;
	if (fscanf(f, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %d %d",
			&n.panGain, &n.scaleY, &n.scaleX, &n.syy, &n.sxx, &n.sxy, &n.sly, &n.slx,
			&n.pivotError[0], &n.pivotError[1], &n.pivotError[2], &n.pivotError[3], &n.pivotSamples,
			&n.learnedX, &n.learnedY, &n.moves, &n.scales) == 17)
		gNav = n;
	fclose(f);
}

// The pivot for a scale segment, in document pixels, for a model at
// (px, py) when the segment began and a cursor at (cx, cy).
static void PivotFor(int pivot, const OverlayState& st, double px, double py, double cursorX, double cursorY,
	double& outX, double& outY)
{
	switch (pivot) {
	case kPivotDocCentre: outX = st.docW * 0.5; outY = st.docH * 0.5; break;
	case kPivotCursor: outX = cursorX; outY = cursorY; break;
	case kPivotLearned: outX = gNav.learnedX; outY = gNav.learnedY; break;
	default: outX = px; outY = py; break;
	}
}

// Sets the overlay's layout to the prediction for the current segment.
// Caller holds the lock.
static void Predict()
{
	if (gMemLocked)
		return;  // ZBrush's own values are read every frame instead
	const Gesture& g = gGesture;
	OverlayState& st = gOverlay;
	if (g.mode == kDragMove) {
		st.ratio = g.baseRatio;
		st.px = g.basePx + gNav.panGain * g.segX / st.pixelsPerDoc;
		st.py = g.basePy + gNav.panGain * g.segY / st.pixelsPerDoc;
	} else if (g.mode == kDragScale) {
		// Until real zooms have been learned from, the reference waits for
		// Live instead of guessing (a wrong guess zooms the wrong way).
		if (gNav.scales < 2)
			return;
		const double s = exp(gNav.scaleY * g.segY + gNav.scaleX * g.segX);
		double cx, cy;
		PivotFor(gNav.pivot(), st, g.basePx, g.basePy, g.cursorX, g.cursorY, cx, cy);
		st.ratio = g.baseRatio * s;
		st.px = cx + s * (g.basePx - cx);
		st.py = cy + s * (g.basePy - cy);
	}
}

static bool CursorOverOverlay(POINT pt, const OverlayState& st)
{
	return st.visible && pt.x >= st.screenX && pt.y >= st.screenY &&
		pt.x < st.screenX + st.docW * st.pixelsPerDoc && pt.y < st.screenY + st.docH * st.pixelsPerDoc;
}

static bool ZBrushInFront()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
{
	if (code != HC_ACTION)
		return CallNextHookEx(gMouseHook, code, wp, lp);
	const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
	++gHookEvents;
	const bool alt = gAltDown, ctrl = gCtrlDown;
	EnterCriticalSection(&gOverlayLock);
	Gesture& g = gGesture;
	switch (wp) {
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
		if (!g.active && gKeyState == 1 && ZBrushInFront() && CursorOverOverlay(m->pt, gOverlay)) {
			g = Gesture();
			g.active = true;
			g.right = wp == WM_RBUTTONDOWN;
			g.classify = !g.right;
			g.down = g.last = m->pt;
			g.ratio0 = g.baseRatio = gOverlay.ratio;
			g.px0 = g.basePx = gOverlay.px;
			g.py0 = g.basePy = gOverlay.py;
		}
		break;
	case WM_MOUSEMOVE:
		if (g.active && g.mode != kDragIgnore) {
			const double dx = m->pt.x - g.last.x, dy = m->pt.y - g.last.y;
			g.last = m->pt;
			if (alt)
				g.altSeen = true;
			int mode;
			if (alt)
				mode = kDragMove;
			else if ((g.right && ctrl) || g.altSeen)
				mode = kDragScale;
			else
				mode = kDragNone;  // rotation
			if (mode != g.mode) {
				// A new segment starts from the layout predicted so far.
				g.mode = mode;
				g.baseRatio = gOverlay.ratio;
				g.basePx = gOverlay.px;
				g.basePy = gOverlay.py;
				g.segX = g.segY = 0;
				g.cursorX = (m->pt.x - gOverlay.screenX) / gOverlay.pixelsPerDoc;
				g.cursorY = (m->pt.y - gOverlay.screenY) / gOverlay.pixelsPerDoc;
				if (mode == kDragScale && ++g.scaleSegments == 1) {
					g.scaleBasePx = g.basePx;
					g.scaleBasePy = g.basePy;
					g.scaleCursorX = g.cursorX;
					g.scaleCursorY = g.cursorY;
				}
			}
			g.segX += dx;
			g.segY += dy;
			if (mode == kDragMove) {
				g.moveX += dx;
				g.moveY += dy;
				if (g.scaleSegments > 0 && (dx != 0 || dy != 0))
					g.movedAfterScale = true;
			} else if (mode == kDragScale) {
				g.scaleDx += dx;
				g.scaleDy += dy;
			} else if (dx != 0 || dy != 0) {
				g.rotated = true;
			}
			Predict();
		}
		break;
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
		if (g.active) {
			g.active = false;
			g.kind = g.mode == kDragIgnore ? kKindSculpt : g.scaleSegments > 0 ? kKindScale :
				(g.moveX != 0 || g.moveY != 0) ? kKindMove : g.rotated ? kKindRotate : kKindNone;
			gLastKind = g.kind;
			g.done = g.kind == kKindMove || g.kind == kKindScale || g.kind == kKindRotate;
			if (g.done) {
				// Waits for Live's exact values. Two navigation drags before
				// they come cannot be told apart, so that one is not learned.
				gPendingCount = gPending.done ? gPendingCount + 1 : 1;
				gPending = g;
				gStaleSets = 0;
			}
			if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu drag kind %d right %d alt %d scaleSegs %d move %.0f %.0f scale %.0f %.0f predicted %.5f %.2f %.2f\n", GetTickCount(),
					g.kind, g.right, g.altSeen, g.scaleSegments, g.moveX, g.moveY, g.scaleDx, g.scaleDy,
					gOverlay.ratio, gOverlay.px, gOverlay.py);
				fclose(f);
			}
		}
		break;
	}
	LeaveCriticalSection(&gOverlayLock);
	return CallNextHookEx(gMouseHook, code, wp, lp);
}

// Called by RSOverlaySet with Live's exact values after a drag: compares
// them with what the drag predicted and updates the navigation model.
// Caller holds the lock.
static void Learn(double ratio, double px, double py)
{
	Gesture& g = gPending;
	if (!g.done)
		return;
	g.done = false;
	const bool single = gPendingCount == 1;
	gPendingCount = 0;
	const double k = gOverlay.pixelsPerDoc;
	const double s = ratio / g.ratio0;
	// Every drag is logged, so ZBrush's behaviour can be checked.
	if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu exact kind %d right %d alt %d scaleSegs %d move %.0f %.0f scale %.0f %.0f afterScaleMove %d "
			"start %.5f %.2f %.2f predicted %.5f %.2f %.2f exact %.5f %.2f %.2f k %.4f\n", GetTickCount(),
			g.kind, g.right, g.altSeen, g.scaleSegments, g.moveX, g.moveY, g.scaleDx, g.scaleDy, g.movedAfterScale,
			g.ratio0, g.px0, g.py0, gOverlay.ratio, gOverlay.px, gOverlay.py, ratio, px, py, k);
		fclose(f);
	}
	if (g.kind == kKindRotate) {
		// Whatever rotating moved the model goes into the offset, so the
		// reference stays put (with ZBrush's memory read, OverlayStep does it).
		if (single && !gMemLocked) {
			gOverlay.offX += (px - g.px0) / ratio;
			gOverlay.offY += (py - g.py0) / ratio;
		}
		return;
	}
	if (!single)
		return;
	if (g.scaleSegments == 0) {
		// Move only: gain along the direction moved.
		const double mx = g.moveX / k, my = g.moveY / k, len2 = mx * mx + my * my;
		if (len2 * k * k > 400 && fabs(s - 1) < 0.01) {
			const double gain = ((px - g.px0) * mx + (py - g.py0) * my) / len2;
			if (fabs(gain) > 0.05 && fabs(gain) < 20) {
				++gNav.moves;
				const double w = gNav.moves < 5 ? 1.0 / gNav.moves : 0.2;
				gNav.panGain += w * (gain - gNav.panGain);
			}
		}
	} else if (g.scaleSegments == 1 && s > 0.01 && s < 100 && fabs(g.scaleDx) + fabs(g.scaleDy) > 20) {
		// Scale gain: least squares, with a weak prior towards no scaling.
		const double l = log(s);
		gNav.syy += g.scaleDy * g.scaleDy;
		gNav.sxx += g.scaleDx * g.scaleDx;
		gNav.sxy += g.scaleDx * g.scaleDy;
		gNav.sly += l * g.scaleDy;
		gNav.slx += l * g.scaleDx;
		const double ridge = 100;
		const double a = gNav.syy + ridge, b = gNav.sxy, d = gNav.sxx + ridge;
		const double det = a * d - b * b;
		if (det > 0) {
			gNav.scaleY = (gNav.sly * d - gNav.slx * b) / det;
			gNav.scaleX = (gNav.slx * a - gNav.sly * b) / det;
		}
		++gNav.scales;
		// Pivot: the fixed point of the observed scale, from where the model
		// was when scaling began. Only from drags with no move after it.
		if (fabs(s - 1) > 0.03 && !g.movedAfterScale) {
			const double bx = g.scaleBasePx, by = g.scaleBasePy;
			for (int i = 0; i < kPivotCount; ++i) {
				double hx, hy;
				PivotFor(i, gOverlay, bx, by, g.scaleCursorX, g.scaleCursorY, hx, hy);
				// Error of the position this pivot would have predicted (the
				// learned point as it stood before this drag).
				const double ex = hx + s * (bx - hx) - px, ey = hy + s * (by - hy) - py;
				gNav.pivotError[i] += sqrt(ex * ex + ey * ey);
			}
			const double cx = (px - s * bx) / (1 - s), cy = (py - s * by) / (1 - s);
			gNav.learnedX += (cx - gNav.learnedX) / (gNav.pivotSamples + 1);
			gNav.learnedY += (cy - gNav.learnedY) / (gNav.pivotSamples + 1);
			++gNav.pivotSamples;
		}
	}
	SaveNav();
}

// Tracks Alt and Ctrl as the mouse hook sees events, in order.
static LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
		const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
		switch (k->vkCode) {
		case VK_MENU: case VK_LMENU: case VK_RMENU: gAltDown = down; break;
		case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: gCtrlDown = down; break;
		}
	}
	return CallNextHookEx(gKeyHook, code, wp, lp);
}

#ifdef RS_TEST_HIDE_CAPTURE
// Test builds: text "event x y alt ctrl" feeds one input event to the hooks
// (event 0 move, 1 left down, 2 left up, 3 right down, 4 right up).
ZFUNC RSTestInput(char* text, double, char*, int, char*, int, char**)
{
	int e, x, y, alt, ctrl;
	if (!text || sscanf(text, "%d %d %d %d %d", &e, &x, &y, &alt, &ctrl) != 5)
		return -1;
	gAltDown = alt != 0;
	gCtrlDown = ctrl != 0;
	static const WPARAM kinds[] = {WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP};
	MSLLHOOKSTRUCT m = {};
	m.pt.x = x;
	m.pt.y = y;
	MouseHook(HC_ACTION, kinds[e], reinterpret_cast<LPARAM>(&m));
	return 1;
}
#endif

// The hooks run on their own thread so the overlay's redraws never delay the
// system's input.
static DWORD WINAPI HookThread(LPVOID)
{
	NoteOwnStack(1);
	PhysicalPixels physical;
	gKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
	gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
		DispatchMessageW(&msg);
	return 0;
}

// The reference as mip levels (each half the size of the one before), so it
// can be resampled for any zoom quickly and without aliasing.
struct Level {
	std::vector<uint32_t> px;  // B, G, R, A
	int w = 0, h = 0;
};
static std::vector<Level> gLevels;
static std::wstring gLevelsFor;

static void BuildLevels()
{
	if (!LoadSavedImage() || gLevelsFor == gImagePath)
		return;
	gLevels.clear();
	Level top;
	top.w = gImage->GetWidth();
	top.h = gImage->GetHeight();
	top.px.resize(static_cast<size_t>(top.w) * top.h);
	Gdiplus::Rect r(0, 0, top.w, top.h);
	Gdiplus::BitmapData data;
	if (gImage->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
		return;
	for (int y = 0; y < top.h; ++y)
		memcpy(&top.px[static_cast<size_t>(y) * top.w], static_cast<BYTE*>(data.Scan0) + y * data.Stride, top.w * 4);
	gImage->UnlockBits(&data);
	gLevels.push_back(std::move(top));
	while (gLevels.back().w > 16 && gLevels.back().h > 16) {
		const Level& a = gLevels.back();
		Level b;
		b.w = a.w / 2;
		b.h = a.h / 2;
		b.px.resize(static_cast<size_t>(b.w) * b.h);
		for (int y = 0; y < b.h; ++y)
			for (int x = 0; x < b.w; ++x) {
				const uint32_t* p = &a.px[static_cast<size_t>(y * 2) * a.w + x * 2];
				uint32_t out = 0;
				for (int c = 0; c < 32; c += 8) {
					const uint32_t sum = ((p[0] >> c) & 255) + ((p[1] >> c) & 255) + ((p[a.w] >> c) & 255) + ((p[a.w + 1] >> c) & 255);
					out |= ((sum + 2) / 4) << c;
				}
				b.px[static_cast<size_t>(y) * b.w + x] = out;
			}
		gLevels.push_back(std::move(b));
	}
	gLevelsFor = gImagePath;
}

// Resamples the reference for the current state into gRefLayer (bilinear,
// premultiplied, scaled by the opacity, transparent outside the image).
// Returns false if there is nothing to show.
static bool BuildRefLayer(const OverlayState& st)
{
	if (!st.visible || st.docW < 1 || st.docH < 1)
		return false;
	BuildLevels();
	if (gLevels.empty())
		return false;
	const int w = static_cast<int>(st.docW * st.pixelsPerDoc + 0.5);
	const int h = static_cast<int>(st.docH * st.pixelsPerDoc + 0.5);
	if (!gRefLayer.Resize(w, h) || !gCapture.Resize(w, h) || !gOutput.Resize(w, h))
		return false;

	// The image's rectangle on the overlay, as in RSRender.
	const double imgW = gLevels[0].w, imgH = gLevels[0].h;
	const double fit = (st.docW / imgW < st.docH / imgH) ? st.docW / imgW : st.docH / imgH;
	const double baseW = imgW * fit, baseH = imgH * fit;
	const double baseX = (st.docW - baseW) * 0.5, baseY = (st.docH - baseH) * 0.5;
	const double k = st.pixelsPerDoc;
	const double x0 = (st.px + st.ratio * (baseX - st.ax - st.offX)) * k, y0 = (st.py + st.ratio * (baseY - st.ay - st.offY)) * k;
	const double dispW = baseW * st.ratio * k, dispH = baseH * st.ratio * k;

	// Smallest level still at least as large as the image on screen.
	size_t li = 0;
	while (li + 1 < gLevels.size() && gLevels[li + 1].w >= dispW && gLevels[li + 1].h >= dispH)
		++li;
	const Level& lv = gLevels[li];

	// Per column and row: the two texels to blend and the weight (0-256) of
	// the second, or -1 outside the image.
	std::vector<int> cx(w), cw(w), ry(h), rw(h);
	for (int x = 0; x < w; ++x) {
		const double u = (x + 0.5 - x0) / dispW * lv.w - 0.5;
		if (u < -0.5 || u > lv.w - 0.5) { cx[x] = -1; continue; }
		const double fu = u < 0 ? 0 : u > lv.w - 1 ? lv.w - 1 : u;
		cx[x] = static_cast<int>(fu);
		cw[x] = static_cast<int>((fu - cx[x]) * 256);
	}
	for (int y = 0; y < h; ++y) {
		const double v = (y + 0.5 - y0) / dispH * lv.h - 0.5;
		if (v < -0.5 || v > lv.h - 0.5) { ry[y] = -1; continue; }
		const double fv = v < 0 ? 0 : v > lv.h - 1 ? lv.h - 1 : v;
		ry[y] = static_cast<int>(fv);
		rw[y] = static_cast<int>((fv - ry[y]) * 256);
	}
	const int op = st.alpha;
	uint32_t* out = reinterpret_cast<uint32_t*>(gRefLayer.bits);
	for (int y = 0; y < h; ++y) {
		uint32_t* row = out + static_cast<size_t>(y) * w;
		if (ry[y] < 0) {
			memset(row, 0, w * 4);
			continue;
		}
		const uint32_t* r0 = &lv.px[static_cast<size_t>(ry[y]) * lv.w];
		const uint32_t* r1 = ry[y] + 1 < lv.h ? r0 + lv.w : r0;
		const int wy = rw[y];
		for (int x = 0; x < w; ++x) {
			const int ix = cx[x];
			if (ix < 0) {
				row[x] = 0;
				continue;
			}
			const int ix1 = ix + 1 < lv.w ? ix + 1 : ix;
			const int wx = cw[x];
			uint32_t result = 0;
			int alpha = 255;
			for (int c = 24; c >= 0; c -= 8) {
				const int a = (r0[ix] >> c) & 255, b = (r0[ix1] >> c) & 255;
				const int d = (r1[ix] >> c) & 255, e = (r1[ix1] >> c) & 255;
				const int top = a * 256 + (b - a) * wx, bottom = d * 256 + (e - d) * wx;
				int v = (top * 256 + (bottom - top) * wy) >> 16;
				if (c == 24) {
					alpha = v * op / 255;
					v = alpha;
				} else {
					v = v * alpha / 255;  // premultiply
				}
				result |= static_cast<uint32_t>(v) << c;
			}
			row[x] = result;
		}
	}
	return true;
}

static void Present(HWND wnd, const OverlayState& st, Buffer32& buf, BYTE alpha)
{
	POINT dst = {static_cast<LONG>(st.screenX), static_cast<LONG>(st.screenY)};
	SIZE size = {buf.w, buf.h};
	POINT src = {0, 0};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
	UpdateLayeredWindow(wnd, nullptr, &dst, &size, buf.dc, &src, 0, &blend, ULW_ALPHA);
	if (!IsWindowVisible(wnd))
		ShowWindow(wnd, SW_SHOWNOACTIVATE);
}

// Screen capture with DXGI desktop duplication: frames arrive as soon as the
// desktop changes and copying them is cheap. Falls back to BitBlt (polled)
// when it cannot be set up.
struct Duplicator {
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	IDXGIOutputDuplication* dup = nullptr;
	ID3D11Texture2D* staging = nullptr;
	RECT output = {0, 0, 0, 0};   // the duplicated monitor, screen pixels
	RECT region = {0, 0, 0, 0};   // what the staging texture holds
	bool failed = false;

	void Release()
	{
		if (staging) staging->Release();
		if (dup) dup->Release();
		if (context) context->Release();
		if (device) device->Release();
		staging = nullptr;
		dup = nullptr;
		context = nullptr;
		device = nullptr;
	}

	// Sets up duplication of the monitor containing (x, y).
	bool Init(int x, int y)
	{
		Release();
		IDXGIFactory1* factory = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			return false;
		bool ok = false;
		IDXGIAdapter1* adapter = nullptr;
		for (UINT a = 0; !ok && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
			IDXGIOutput* out = nullptr;
			for (UINT o = 0; !ok && adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
				DXGI_OUTPUT_DESC desc;
				out->GetDesc(&desc);
				const RECT& r = desc.DesktopCoordinates;
				if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
					IDXGIOutput1* out1 = nullptr;
					if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
							D3D11_SDK_VERSION, &device, nullptr, &context)) &&
						SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1))) &&
						SUCCEEDED(out1->DuplicateOutput(device, &dup))) {
						output = r;
						ok = true;
					}
					if (out1)
						out1->Release();
				}
				out->Release();
			}
			adapter->Release();
		}
		factory->Release();
		if (!ok)
			Release();
		return ok;
	}

	// Waits up to timeout ms for a new desktop frame and copies the screen
	// rectangle (sx, sy, w, h) into dst. Returns 1 for a new frame, 0 if none
	// came, -1 if duplication failed.
	int Grab(int sx, int sy, int w, int h, Buffer32& dst, UINT timeout)
	{
		if (!dup)
			return -1;
		DXGI_OUTDUPL_FRAME_INFO info;
		IDXGIResource* res = nullptr;
		HRESULT hr = dup->AcquireNextFrame(timeout, &info, &res);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
			return 0;
		if (FAILED(hr)) {
			// Access lost (mode change, secure desktop): set up again next time.
			Release();
			return -1;
		}
		ID3D11Texture2D* frame = nullptr;
		res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frame));
		res->Release();
		// The part of the rectangle on this monitor, in the monitor's pixels.
		const int left = sx > output.left ? sx : output.left, top = sy > output.top ? sy : output.top;
		const int right = sx + w < output.right ? sx + w : output.right, bottom = sy + h < output.bottom ? sy + h : output.bottom;
		int result = 0;
		if (frame && right > left && bottom > top) {
			const int cw = right - left, ch = bottom - top;
			if (!staging || region.right - region.left != cw || region.bottom - region.top != ch) {
				if (staging)
					staging->Release();
				staging = nullptr;
				D3D11_TEXTURE2D_DESC fd;
				frame->GetDesc(&fd);
				D3D11_TEXTURE2D_DESC td = {};
				td.Width = cw;
				td.Height = ch;
				td.MipLevels = 1;
				td.ArraySize = 1;
				td.Format = fd.Format;
				td.SampleDesc.Count = 1;
				td.Usage = D3D11_USAGE_STAGING;
				td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				device->CreateTexture2D(&td, nullptr, &staging);
			}
			region = {left, top, right, bottom};
			if (staging) {
				D3D11_BOX box = {static_cast<UINT>(left - output.left), static_cast<UINT>(top - output.top), 0,
					static_cast<UINT>(right - output.left), static_cast<UINT>(bottom - output.top), 1};
				context->CopySubresourceRegion(staging, 0, 0, 0, 0, frame, 0, &box);
				D3D11_MAPPED_SUBRESOURCE map;
				if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
					if (cw != w || ch != h)
						memset(dst.bits, 0, static_cast<size_t>(w) * h * 4);
					for (int y = 0; y < ch; ++y)
						memcpy(dst.bits + (static_cast<size_t>(top - sy + y) * w + (left - sx)) * 4,
							static_cast<BYTE*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, cw * 4);
					context->Unmap(staging, 0);
					result = 1;
				}
			}
		}
		if (frame)
			frame->Release();
		dup->ReleaseFrame();
		return result;
	}
};

static Duplicator gDup;
static double gMagentaShare = 0;  // percent of the overlay that was key colour
static double gModelPixels = 0;   // pixels of the overlay that were not
static int gCaptureMethod = 0;  // 1 desktop duplication, 0 BitBlt
static double gFps = 0;

// Fills gCapture with the screen under the overlay. Returns true if it may
// have changed since the last call.
static bool Capture(const OverlayState& st)
{
	const int w = gCapture.w, h = gCapture.h;
	const int sx = static_cast<int>(st.screenX), sy = static_cast<int>(st.screenY);
	if (!gDup.dup && !gDup.failed) {
		if (!gDup.Init(sx + w / 2, sy + h / 2))
			gDup.failed = true;
	}
	if (gDup.dup) {
		const int got = gDup.Grab(sx, sy, w, h, gCapture, 8);
		if (got >= 0) {
			gCaptureMethod = 1;
			return got == 1;
		}
		// Lost: try to set it up again on the next call.
		return false;
	}
	gCaptureMethod = 0;
#ifdef RS_TEST_HIDE_CAPTURE
	// Test builds (Wine has no capture exclusion): hide while capturing, and
	// stay shown long enough between captures for tests to read the screen.
	Sleep(200);
	ShowWindow(gOverlayWnd, SW_HIDE);
	Sleep(60);
#else
	Sleep(4);
#endif
	HDC screen = GetDC(nullptr);
	BitBlt(gCapture.dc, 0, 0, w, h, screen, sx, sy, SRCCOPY);
	ReleaseDC(nullptr, screen);
	GdiFlush();
	return true;
}

// Below this share of magenta (out of 255) a pixel is left as it is. Above
// it the magenta is replaced, which also removes any magenta tint ZBrush puts
// on the model.
static const int kModelBelow = 12;

// Builds the opaque output from gCapture: each pixel's share of magenta is
// replaced by the reference (gRefLayer, black where there is none).
static void Composite(HWND wnd, const OverlayState& st)
{
	const int w = gOutput.w, h = gOutput.h;
	const size_t n = static_cast<size_t>(w) * h;
	gKeyShare.resize(n);
	gKeyW = w;
	gKeyH = h;
	const unsigned char* cap = gCapture.bits;
	const unsigned char* ref = gRefLayer.bits;
	unsigned char* out = gOutput.bits;
	size_t magenta = 0, model = 0;
	for (size_t i = 0; i < n; ++i, cap += 4, ref += 4, out += 4) {
		const int b = cap[0], g = cap[1], r = cap[2];
		// Share of magenta: blending any grey with magenta (255, 0, 255) by t
		// raises R and B above G by t * 255.
		const int mn = r < b ? r : b;
		int t = mn > g ? mn - g : 0;
		gKeyShare[i] = static_cast<unsigned char>(t);
		out[3] = 255;
		if (t < kModelBelow) {
			++model;
			out[0] = static_cast<unsigned char>(b);
			out[1] = static_cast<unsigned char>(g);
			out[2] = static_cast<unsigned char>(r);
			continue;
		}
		if (t > 200)
			++magenta;
		t = t > 230 ? 256 : (t << 8) / 255;
		const int ob = b + (t * (ref[0] - 255) >> 8), og = g + (t * ref[1] >> 8), orr = r + (t * (ref[2] - 255) >> 8);
		out[0] = static_cast<unsigned char>(ob < 0 ? 0 : ob > 255 ? 255 : ob);
		out[1] = static_cast<unsigned char>(og < 0 ? 0 : og > 255 ? 255 : og);
		out[2] = static_cast<unsigned char>(orr < 0 ? 0 : orr > 255 ? 255 : orr);
	}
	gMagentaShare = n ? 100.0 * magenta / n : 0;
	gModelPixels = static_cast<double>(model);
	Present(wnd, st, gOutput, 255);
}

// Checks that the overlay really is left out of screen captures: shows it
// solid green and looks for green in a BitBlt of the screen under it.
static bool ExclusionWorks(HWND wnd, const OverlayState& st)
{
#ifdef RS_TEST_HIDE_CAPTURE
	return true;
#endif
	const int w = gOutput.w, h = gOutput.h;
	uint32_t* out = reinterpret_cast<uint32_t*>(gOutput.bits);
	for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
		out[i] = 0xff00ff00;
	Present(wnd, st, gOutput, 255);
	Sleep(100);
	HDC screen = GetDC(nullptr);
	BitBlt(gCapture.dc, 0, 0, w, h, screen, static_cast<int>(st.screenX), static_cast<int>(st.screenY), SRCCOPY);
	ReleaseDC(nullptr, screen);
	GdiFlush();
	const uint32_t* cap = reinterpret_cast<const uint32_t*>(gCapture.bits);
	size_t green = 0, n = static_cast<size_t>(w) * h;
	for (size_t i = 0; i < n; i += 7)
		if ((cap[i] & 0xffffff) == 0x00ff00)
			++green;
	return green * 7 < n / 2;
}

// A left drag that starts on the model is sculpting, not navigation. Decided
// from the last capture, on the overlay thread that owns it.
static void ClassifyGesture()
{
	EnterCriticalSection(&gOverlayLock);
	Gesture& g = gGesture;
	if (g.classify) {
		g.classify = false;
		const int x = static_cast<int>(g.down.x - gOverlay.screenX), y = static_cast<int>(g.down.y - gOverlay.screenY);
		if (x >= 0 && y >= 0 && x < gKeyW && y < gKeyH && gKeyShare[static_cast<size_t>(y) * gKeyW + x] < kModelBelow) {
			// Undo anything predicted before the drag was classified.
			g.mode = kDragIgnore;
			g.done = false;
			gOverlay.ratio = g.ratio0;
			gOverlay.px = g.px0;
			gOverlay.py = g.py0;
		}
	}
	LeaveCriticalSection(&gOverlayLock);
}

// Reading ZBrush's camera from its memory. The DLL runs inside ZBrush, so
// once it knows where ZBrush keeps the model's scale and position it can
// read them every frame, exactly, during drags. To find them: the first
// report of exact values from Live (RSOverlaySet) scans ZBrush's writable
// memory for floats and doubles equal to each value; each later report with
// changed values keeps only the locations that changed to match. When few
// are left for all three values, they are read every frame. Nothing is
// written. If the locations stop matching a report, the search starts over.

struct MemCand {
	uintptr_t addr;
	bool isDouble;
	double last;      // value when last read
	DWORD changedAt;  // when it last changed
};

struct MemTrack {
	std::vector<MemCand> cands;
	double target = 0;
	int narrowings = 0;   // reports with a changed value that it survived
	bool tooMany = false;
};

static MemTrack gMemScale, gMemX, gMemY;
static bool gMemScanned = false;
static double gAnchorScale = 0;  // TransformGet scale at ratio 1
// For a second after each report, a location whose value changes while no
// mouse button is down is not ZBrush's camera (a temporary or stack copy).
static DWORD gMemWatchUntil = 0;
static int gMemDisagree = 0;


static bool MemRead(const MemCand& c, double& v)
{
	SIZE_T got = 0;
	if (c.isDouble) {
		double d;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.addr), &d, 8, &got) || got != 8)
			return false;
		v = d;
	} else {
		float f;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.addr), &f, 4, &got) || got != 4)
			return false;
		v = f;
	}
	return v == v;  // not NaN
}

// The script passes values rounded to two or three decimals.
static bool MemMatch(double v, double target)
{
	return fabs(v - target) <= 0.0051 + fabs(target) * 1e-5;
}

// Last scan: MB scanned, x matches, x-y pairs, scales near pairs, ms.
static double gScanStats[5] = {0, 0, 0, 0, 0};

// Scans for x with y right after it (as float or double pairs), then for the
// scale within 1 KB of each pair. Scanning for the scale on its own finds too
// many matches when it is a common number such as 1.
static void MemScan(double sx, double px, double py)
{
	const DWORD t0 = GetTickCount();
	MemTrack* tracks[3] = {&gMemScale, &gMemX, &gMemY};
	const double targets[3] = {sx, px, py};
	for (int t = 0; t < 3; ++t) {
		tracks[t]->cands.clear();
		tracks[t]->target = targets[t];
		tracks[t]->narrowings = 0;
		tracks[t]->tooMany = false;
	}
	const size_t kMaxPairs = 200000;
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&MemScan), &self);
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uintptr_t stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit) & ~static_cast<uintptr_t>(0xfffff);
	const uintptr_t stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
	std::vector<unsigned char> buf(1 << 22);
	const uintptr_t bufLo = reinterpret_cast<uintptr_t>(buf.data()), bufHi = bufLo + buf.size();
	size_t scanned = 0, rawX = 0, rawY = 0;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
		const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		const uintptr_t next = base + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xff;
		const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
		if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD) &&
			(mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
			// Not this DLL's own copies of the values, nor the calling thread's
			// stack (it holds the values being reported right now).
			mbi.AllocationBase != self && !(stackLow < next && base < stackHigh) &&
			!OwnThreadStack(base, next)) {
			// Chunks overlap by 8 bytes so a pair across a boundary is found.
			for (uintptr_t chunk = base; chunk < next; chunk += buf.size() - 8) {
				const size_t len = next - chunk < buf.size() ? next - chunk : buf.size();
				SIZE_T got = 0;
				if (chunk < bufHi && bufLo < chunk + len)
					continue;  // the scan's own copy of memory
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(chunk), buf.data(), len, &got))
					continue;
				scanned += got;
				for (size_t i = 0; i + 8 <= got; i += 4) {
					float f, g;
					memcpy(&f, &buf[i], 4);
					memcpy(&g, &buf[i + 4], 4);
					if (MemMatch(f, px)) {
						++rawX;
						if (MemMatch(g, py) && gMemX.cands.size() < kMaxPairs) {
							gMemX.cands.push_back({chunk + i, false, f, 0});
							gMemY.cands.push_back({chunk + i + 4, false, g, 0});
						}
					}
					if (MemMatch(f, py))
						++rawY;
					if (i % 8 == 0 && i + 16 <= got) {
						double d, e;
						memcpy(&d, &buf[i], 8);
						memcpy(&e, &buf[i + 8], 8);
						if (MemMatch(d, px)) {
							++rawX;
							if (MemMatch(e, py) && gMemX.cands.size() < kMaxPairs) {
								gMemX.cands.push_back({chunk + i, true, d, 0});
								gMemY.cands.push_back({chunk + i + 8, true, e, 0});
							}
						}
						if (MemMatch(d, py))
							++rawY;
					}
				}
				if (len < buf.size())
					break;
			}
		}
		if (next <= addr)
			break;
		addr = next;
	}
	// The scale near each pair, of the same type.
	std::vector<unsigned char> win(2048 + 16);
	for (const MemCand& x : gMemX.cands) {
		const uintptr_t lo = x.addr > 1024 ? x.addr - 1024 : 0;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(lo), win.data(), win.size(), &got))
			continue;
		const size_t step = x.isDouble ? 8 : 4;
		for (size_t i = (x.addr - lo) % step; i + step <= got; i += step) {
			double v;
			if (x.isDouble) {
				memcpy(&v, &win[i], 8);
			} else {
				float f;
				memcpy(&f, &win[i], 4);
				v = f;
			}
			if (MemMatch(v, sx) && lo + i != x.addr) {
				gMemScale.cands.push_back({lo + i, x.isDouble, v, 0});
				if (gMemScale.cands.size() > kMaxPairs * 4)
					break;
			}
		}
	}
	// One scale location can be near several pairs.
	std::sort(gMemScale.cands.begin(), gMemScale.cands.end(),
		[](const MemCand& a, const MemCand& b) { return a.addr < b.addr || (a.addr == b.addr && a.isDouble < b.isDouble); });
	gMemScale.cands.erase(std::unique(gMemScale.cands.begin(), gMemScale.cands.end(),
		[](const MemCand& a, const MemCand& b) { return a.addr == b.addr && a.isDouble == b.isDouble; }), gMemScale.cands.end());
	gMemScanned = true;
	gScanStats[0] = static_cast<double>(scanned >> 20);
	gScanStats[1] = static_cast<double>(rawX);
	gScanStats[2] = static_cast<double>(gMemX.cands.size());
	gScanStats[3] = static_cast<double>(gMemScale.cands.size());
	gScanStats[4] = static_cast<double>(GetTickCount() - t0);
	if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu scan for scale %.4f x %.4f y %.4f: %zu MB in %lu ms, x matches %zu, y matches %zu, pairs %zu, scales near %zu\n",
			GetTickCount(), sx, px, py, scanned >> 20, GetTickCount() - t0, rawX, rawY, gMemX.cands.size(), gMemScale.cands.size());
		fclose(f);
	}
}

// Keeps the candidates that now hold target. Returns false if none are left.
static bool MemNarrow(MemTrack& tr, double target)
{
	const bool changed = !MemMatch(tr.target, target);
	std::vector<MemCand> keep;
	for (const MemCand& c : tr.cands) {
		double v;
		if (MemRead(c, v) && MemMatch(v, target)) {
			keep.push_back(c);
			keep.back().last = v;
		}
	}
	tr.cands.swap(keep);
	if (changed && !tr.cands.empty())
		++tr.narrowings;
	tr.target = target;
	return !tr.cands.empty() && !tr.tooMany;
}

static bool MemTrackReady(const MemTrack& tr)
{
	return tr.narrowings >= 2 && !tr.cands.empty() && tr.cands.size() <= 64;
}

// Reads one value. Of several candidates the one that changed most recently
// is the live one (others are copies made at other times, such as the
// script's own variables).
static bool MemValue(MemTrack& tr, double& out)
{
	const DWORD now = GetTickCount();
	const MemCand* best = nullptr;
	for (MemCand& c : tr.cands) {
		double v;
		if (!MemRead(c, v))
			continue;
		if (v != c.last) {
			c.last = v;
			c.changedAt = now;
		}
		if (!best || static_cast<int>(c.changedAt - best->changedAt) > 0)
			best = &c;
	}
	if (best)
		out = best->last;
	return best != nullptr;
}

static void MemWeedOut(MemTrack& tr)
{
	std::vector<MemCand> keep;
	for (const MemCand& c : tr.cands) {
		double v;
		if (MemRead(c, v) && MemMatch(v, tr.target))
			keep.push_back(c);
	}
	if (!keep.empty())
		tr.cands.swap(keep);
}

// ZBrush keeps a view's values together: y right after x, the scale close
// by. Locations that fit that pattern are the real ones; stray copies (freed
// memory, temporaries) almost never do.
struct MemTriplet {
	MemCand s, x, y;
	DWORD changedAt;
	int dragFrames;  // frames in which it changed while the mouse was dragging
	int posFrames;   // ... of which its position changed
};
static int gTripletInUse = -1;
static std::vector<MemTriplet> gTriplets;

static void BuildTriplets()
{
	gTriplets.clear();
	gTripletInUse = -1;
	for (const MemCand& x : gMemX.cands)
		for (const MemCand& y : gMemY.cands) {
			if (y.isDouble != x.isDouble || y.addr != x.addr + (x.isDouble ? 8 : 4))
				continue;
			for (const MemCand& sc : gMemScale.cands)
				if (sc.isDouble == x.isDouble && (sc.addr > x.addr ? sc.addr - x.addr : x.addr - sc.addr) <= 1024)
					gTriplets.push_back({sc, x, y, 0, 0, 0});
		}
}

// Reads the live triplet: the one that has changed in the most frames
// while the mouse was dragging (copies that only update now and then never
// win), the most recently changed one until a drag has shown which. Switches
// and sudden jumps are logged.
static bool TripletValues(double& sx, double& px, double& py, bool dragging)
{
	const DWORD now = GetTickCount();
	int best = -1;
	for (size_t i = 0; i < gTriplets.size(); ++i) {
		MemTriplet& t = gTriplets[i];
		double a, b, c;
		if (!MemRead(t.s, a) || !MemRead(t.x, b) || !MemRead(t.y, c))
			continue;
		// ZBrush borrows this memory to draw the navigation head: whole-number
		// scale and position (scale 64). Such a read is skipped.
		if (a == floor(a) && b == floor(b) && c == floor(c))
			continue;
		if (a != t.s.last || b != t.x.last || c != t.y.last) {
			if (dragging) {
				++t.dragFrames;
				if (b != t.x.last || c != t.y.last)
					++t.posFrames;
			}
			t.s.last = a;
			t.x.last = b;
			t.y.last = c;
			t.changedAt = now;
		}
		if (best < 0) {
			best = static_cast<int>(i);
			continue;
		}
		// Prefer the copy whose position follows drags (some copies only
		// ever update the scale), then the busiest, then the latest.
		const MemTriplet& o = gTriplets[best];
		if (t.posFrames != o.posFrames ? t.posFrames > o.posFrames :
				t.dragFrames != o.dragFrames ? t.dragFrames > o.dragFrames : static_cast<int>(t.changedAt - o.changedAt) > 0)
			best = static_cast<int>(i);
	}
	// Stay with the copy in use unless another is clearly busier: near-equal
	// copies (a pixel or two apart) would otherwise make the image jitter.
	if (best >= 0 && gTripletInUse >= 0 && gTripletInUse < static_cast<int>(gTriplets.size()) && best != gTripletInUse) {
		const MemTriplet& cur = gTriplets[gTripletInUse];
		const MemTriplet& cand = gTriplets[best];
		double a, b, c;
		const bool curReadable = MemRead(cur.s, a) && MemRead(cur.x, b) && MemRead(cur.y, c);
		if (curReadable && cand.posFrames <= cur.posFrames * 3 / 2 + 10 && !(cur.dragFrames == 0 && cand.dragFrames > 0))
			best = gTripletInUse;
	}
	if (best < 0)
		return false;
	const MemTriplet& t = gTriplets[best];
	const bool jump = sx > 0 && (fabs(t.s.last / sx - 1) > 0.3 || fabs(t.x.last - px) > 200 || fabs(t.y.last - py) > 200);
	// ZBrush briefly reuses the view's memory, for example to draw the
	// navigation head. A big jump is only taken once three reads in a row
	// show it.
	static int pending = 0;
	static double pendS = 0, pendX = 0, pendY = 0;
	if (jump) {
		const bool same = pending > 0 && fabs(t.s.last / pendS - 1) < 0.02 && fabs(t.x.last - pendX) < 5 && fabs(t.y.last - pendY) < 5;
		pending = same ? pending + 1 : 1;
		pendS = t.s.last;
		pendX = t.x.last;
		pendY = t.y.last;
		if (pending < 3)
			return true;  // keep the values passed in
	}
	pending = 0;
	if (best != gTripletInUse || jump) {
		if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu read triplet %d of %zu (drag frames %d)%s: scale %.4f x %.3f y %.3f, was %.4f %.3f %.3f\n",
				now, best, gTriplets.size(), t.dragFrames, jump ? " JUMP" : "", t.s.last, t.x.last, t.y.last, sx, px, py);
			fclose(f);
		}
		gTripletInUse = best;
	}
	sx = t.s.last;
	px = t.x.last;
	py = t.y.last;
	return true;
}

// Feeds a report of exact values to the search. Caller holds the lock.
static void MemReport(double sx, double px, double py)
{
	if (sx <= 0)
		return;
	if (gMemLocked) {
		// ZBrush can report old values for a while after a drag; while the
		// live locations disagree with a report, it is not used to narrow
		// them, unless that keeps happening.
		double ms = sx, mx = px, my = py;
		const bool agree = MemValue(gMemScale, ms) && MemValue(gMemX, mx) && MemValue(gMemY, my) &&
			MemMatch(ms, sx) && MemMatch(mx, px) && MemMatch(my, py);
		if (!agree && ++gMemDisagree < 4)
			return;
	}
	gMemDisagree = 0;
	if (!gMemScanned) {
		MemScan(sx, px, py);
	} else {
		const bool ok = MemNarrow(gMemScale, sx) & MemNarrow(gMemX, px) & MemNarrow(gMemY, py);
		if (!ok)
			MemScan(sx, px, py);  // lost them (or too many): start over
	}
	gMemLocked = MemTrackReady(gMemScale) && MemTrackReady(gMemX) && MemTrackReady(gMemY);
	if (gMemLocked)
		BuildTriplets();
	gMemWatchUntil = GetTickCount() + 1000;
	if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu memory %d candidates %zu %zu %zu narrowed %d %d %d locked %d\n", GetTickCount(),
			gMemScanned, gMemScale.cands.size(), gMemX.cands.size(), gMemY.cands.size(),
			gMemScale.narrowings, gMemX.narrowings, gMemY.narrowings, gMemLocked);
		fclose(f);
	}
}

// Each frame while locked: the layout from ZBrush's own values. A rotation
// drag must not move the reference, so whatever the model's position changes
// by during frames of a rotation goes into the offset (the image stays put).
static void MemFollow()
{
	if (!gMemLocked || gAnchorScale <= 0)
		return;
	EnterCriticalSection(&gOverlayLock);
	if (!gGesture.active && static_cast<int>(gMemWatchUntil - GetTickCount()) > 0) {
		const size_t before = gMemScale.cands.size() + gMemX.cands.size() + gMemY.cands.size();
		MemWeedOut(gMemScale);
		MemWeedOut(gMemX);
		MemWeedOut(gMemY);
		if (gMemScale.cands.size() + gMemX.cands.size() + gMemY.cands.size() != before)
			BuildTriplets();
	}
	const bool rotating = gGesture.active && gGesture.mode == kDragNone;
	OverlayState& st = gOverlay;
	double sx = st.ratio * gAnchorScale, px = st.px, py = st.py;
	const bool read = !gTriplets.empty() ? TripletValues(sx, px, py, gGesture.active) :
		MemValue(gMemScale, sx) && MemValue(gMemX, px) && MemValue(gMemY, py);
	if (read && sx > 0 && sx < 1e7 && fabs(px) < 1e6 && fabs(py) < 1e6) {
		const double r = sx / gAnchorScale;
		if (rotating) {
			st.offX += (px - st.px) / r;
			st.offY += (py - st.py) / r;
		}
		st.ratio = r;
		st.px = px;
		st.py = py;
	}
	LeaveCriticalSection(&gOverlayLock);
}

static bool gRedrawPending = false;

// One pass of the overlay loop: rebuilds the reference if its layout
// changed, captures, composites and presents. Returns false when idle.
static bool OverlayStep(HWND wnd)
{
	static OverlayState shown;
	static bool tested = false;
	static DWORD fpsStart = 0;
	static int fpsFrames = 0;
	ClassifyGesture();
	MemFollow();
	EnterCriticalSection(&gOverlayLock);
	const OverlayState st = gOverlay;
	const bool redraw = gRedrawPending;
	gRedrawPending = false;
	LeaveCriticalSection(&gOverlayLock);
	if (!st.visible) {
		if (IsWindowVisible(wnd))
			ShowWindow(wnd, SW_HIDE);
		tested = false;
		return false;
	}
	const bool layoutChanged = redraw || st.ratio != shown.ratio || st.px != shown.px || st.py != shown.py ||
		st.ax != shown.ax || st.ay != shown.ay || st.offX != shown.offX || st.offY != shown.offY || st.alpha != shown.alpha || st.screenX != shown.screenX ||
		st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc;
	if (layoutChanged) {
		if (st.screenX != shown.screenX || st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc) {
			// The overlay moved (recalibrated): it may be on another monitor.
			gDup.Release();
			gDup.failed = false;
		}
		if (!BuildRefLayer(st)) {
			ShowWindow(wnd, SW_HIDE);
			return false;
		}
		shown = st;
	}
	if (gKeyState == 1 && !tested) {
		tested = true;
		if (!ExclusionWorks(wnd, st))
			gKeyState = -1;
	}
	if (gKeyState != 1) {
		if (layoutChanged)
			Present(wnd, st, gRefLayer, static_cast<BYTE>(255));
		return false;
	}
	if (Capture(st) || layoutChanged) {
		Composite(wnd, st);
		++fpsFrames;
		const DWORD now = GetTickCount();
		if (now - fpsStart >= 1000) {
			gFps = fpsFrames * 1000.0 / (now - fpsStart);
			fpsStart = now;
			fpsFrames = 0;
		}
	}
	return true;
}

static LRESULT CALLBACK OverlayProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_RS_REDRAW:
		EnterCriticalSection(&gOverlayLock);
		gRedrawPending = true;
		LeaveCriticalSection(&gOverlayLock);
		if (!gOverlay.visible || gKeyState != 1)
			OverlayStep(wnd);
		return 0;
	case WM_RS_CLOSE:
		DestroyWindow(wnd);
		return 0;
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_DESTROY:
		gOverlayWnd = nullptr;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

static DWORD WINAPI OverlayThread(LPVOID ready)
{
	// The overlay window is placed in physical pixels, like the calibration.
	NoteOwnStack(0);
	PhysicalPixels physical;
	StartGdiplus();
	WNDCLASSW wc = {};
	wc.lpfnWndProc = OverlayProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"ReferenceScalerOverlay";
	RegisterClassW(&wc);
	HWND owner = nullptr;
	EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&owner));
	gOverlayWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		wc.lpszClassName, L"Reference Scaler", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, nullptr);
	// WDA_EXCLUDEFROMCAPTURE (Windows 10 2004 and later): the overlay's own
	// captures of the screen see what is underneath it.
#ifdef RS_TEST_HIDE_CAPTURE
	gKeyState = 1;
#else
	gKeyState = gOverlayWnd && SetWindowDisplayAffinity(gOverlayWnd, 0x11) ? 1 : 0;
#endif
	if (gOverlayWnd) {
		LoadNav();
		CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr));
	}
	SetEvent(static_cast<HANDLE>(ready));
	// Messages first, then a frame; frames pace themselves on the desktop
	// (Capture waits for the next one), and an idle overlay just waits.
	HWND wnd = gOverlayWnd;
	MSG msg;
	for (;;) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				gOverlayThreadId = 0;
				return 0;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!gOverlayWnd || !OverlayStep(wnd))
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
	}
}

static void EnsureLock()
{
	if (!gOverlayLockReady) {
		InitializeCriticalSection(&gOverlayLock);
		gOverlayLockReady = true;
	}
}

// Starts the overlay thread once and keeps the DLL loaded for it.
static bool EnsureOverlay()
{
	EnsureLock();
	if (gOverlayWnd)
		return true;
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&EnsureOverlay), &self);
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE thread = CreateThread(nullptr, 0, OverlayThread, ready, 0, &gOverlayThreadId);
	if (!thread) {
		CloseHandle(ready);
		return false;
	}
	WaitForSingleObject(ready, 5000);
	CloseHandle(ready);
	CloseHandle(thread);
	return gOverlayWnd != nullptr;
}

// ---------------------------------------------------------------------------
// Canvas calibration: finds where the document is on screen by matching the
// last rendered frame (frame0.png, which is what the canvas shows behind the
// model) against a capture of ZBrush's window. This needs no knowledge of
// ZBrush's layout, canvas zoom or Windows display scaling.
// ---------------------------------------------------------------------------

struct Rgb {
	std::vector<unsigned char> px;  // B, G, R per pixel
	int w = 0, h = 0;
	const unsigned char* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
};

static bool LoadRgb(const std::wstring& path, Rgb& out)
{
	StartGdiplus();
	Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
	bool ok = false;
	if (bmp && bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) {
		Gdiplus::Rect r(0, 0, bmp->GetWidth(), bmp->GetHeight());
		Gdiplus::BitmapData data;
		if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &data) == Gdiplus::Ok) {
			out.w = r.Width;
			out.h = r.Height;
			out.px.resize(static_cast<size_t>(out.w) * out.h * 3);
			for (int y = 0; y < out.h; ++y)
				memcpy(&out.px[static_cast<size_t>(y) * out.w * 3], static_cast<BYTE*>(data.Scan0) + y * data.Stride, out.w * 3);
			bmp->UnlockBits(&data);
			ok = true;
		}
	}
	delete bmp;
	return ok;
}

static bool CaptureScreen(const RECT& r, Rgb& out)
{
	const int w = r.right - r.left, h = r.bottom - r.top;
	if (w < 16 || h < 16)
		return false;
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, dib);
	const bool ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY) != 0;
	GdiFlush();
	if (ok) {
		out.w = w;
		out.h = h;
		out.px.resize(static_cast<size_t>(w) * h * 3);
		const unsigned char* src = static_cast<const unsigned char*>(bits);
		for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
			out.px[i * 3] = src[i * 4];
			out.px[i * 3 + 1] = src[i * 4 + 1];
			out.px[i * 3 + 2] = src[i * 4 + 2];
		}
	}
	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return ok;
}

// Box-averages img into blocks of size d.
static Rgb Shrink(const Rgb& img, int d)
{
	Rgb out;
	out.w = img.w / d;
	out.h = img.h / d;
	out.px.resize(static_cast<size_t>(out.w) * out.h * 3);
	for (int y = 0; y < out.h; ++y)
		for (int x = 0; x < out.w; ++x)
			for (int c = 0; c < 3; ++c) {
				int sum = 0;
				for (int j = 0; j < d; ++j)
					for (int i = 0; i < d; ++i)
						sum += img.at(x * d + i, y * d + j)[c];
				out.px[(static_cast<size_t>(y) * out.w + x) * 3 + c] = static_cast<unsigned char>(sum / (d * d));
			}
	return out;
}

// Average colour of the frame over a square of the given radius around (x, y).
static void FrameColor(const Rgb& f, double x, double y, double radius, int out[3])
{
	const int r = static_cast<int>(radius);
	const int cx = static_cast<int>(x), cy = static_cast<int>(y);
	int sum[3] = {0, 0, 0}, n = 0;
	for (int j = cy - r; j <= cy + r; ++j) {
		if (j < 0 || j >= f.h)
			continue;
		for (int i = cx - r; i <= cx + r; ++i) {
			if (i < 0 || i >= f.w)
				continue;
			const unsigned char* p = f.at(i, j);
			sum[0] += p[0];
			sum[1] += p[1];
			sum[2] += p[2];
			++n;
		}
	}
	for (int c = 0; c < 3; ++c)
		out[c] = n ? sum[c] / n : 0;
}

struct Sample {
	double fx, fy;  // frame position (pixel centre)
};

// Luminance of a B, G, R pixel.
static int Lum(const unsigned char* p)
{
	return (p[0] + p[1] * 5 + p[2] * 2) / 8;
}

// Correlation between the frame's sample luminances and the screen image at
// the same points, for the document placed at (ox, oy) in img pixels with k
// img pixels per document pixel. Correlation ignores brightness and contrast
// changes in how ZBrush displays the canvas. Returns -1 if under 15% of the
// samples land on the image (the canvas can extend past the window).
static double Score(const Rgb& img, const std::vector<Sample>& samples, const std::vector<int>& lums,
	double ox, double oy, double k, int* inBounds = nullptr)
{
	double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
	int n = 0;
	for (size_t i = 0; i < samples.size(); ++i) {
		const int x = static_cast<int>(floor(ox + k * samples[i].fx));
		const int y = static_cast<int>(floor(oy + k * samples[i].fy));
		if (x < 0 || y < 0 || x >= img.w || y >= img.h)
			continue;
		const double va = lums[i], vb = Lum(img.at(x, y));
		sa += va;
		sb += vb;
		saa += va * va;
		sbb += vb * vb;
		sab += va * vb;
		++n;
	}
	if (n < 40 || n < samples.size() * 0.15)
		return -1;
	const double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
	if (va <= 0 || vb <= 0)
		return -1;
	if (inBounds)
		*inBounds = n;
	return cov / sqrt(va * vb);
}

// Ranks placements by how unlikely their correlation is by chance, so a
// placement with few samples on screen (canvas zoomed past the window) only
// wins with a strong correlation.
static double Significance(double corr, int n)
{
	return corr * sqrt(static_cast<double>(n));
}

static void SampleLums(const Rgb& frame, const std::vector<Sample>& samples, double radius, std::vector<int>& lums)
{
	lums.resize(samples.size());
	for (size_t i = 0; i < samples.size(); ++i) {
		int c[3];
		FrameColor(frame, samples[i].fx, samples[i].fy, radius, c);
		const unsigned char p[3] = {static_cast<unsigned char>(c[0]), static_cast<unsigned char>(c[1]), static_cast<unsigned char>(c[2])};
		lums[i] = Lum(p);
	}
}

struct Calibration {
	double screenX = 0, screenY = 0, pixelsPerDoc = 0;
	double percent = 0;  // correlation of the best placement, x100
};
static Calibration gCalibration;

// Finds the calibration pattern in the screen capture. Returns false if the
// best placement correlates under 0.4.
static bool Calibrate(const Rgb& frame, const Rgb& screen, Calibration& out)
{
	// Sample grids: one point in the middle of each large pattern block for
	// the coarse search, four per block for the next pass, then a dense grid
	// over the small blocks for the exact fit.
	auto grid = [&](int n, double at) {
		std::vector<Sample> s;
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
				s.push_back({(i + at) * frame.w / n, (j + at) * frame.h / n});
		return s;
	};
	const std::vector<Sample> coarse = grid(kBigBlocks, 0.5), medium = grid(4 * kBigBlocks, 0.5), fine = grid(110, 0.37);

	// Coarse search on the capture shrunk by d, over scales 0.25 to 4.
	const int d = 8;
	const Rgb small = Shrink(screen, d);
	std::vector<int> lums;
	double best = -1e9, bestCorr = -1, bestK = 1, bestX = 0, bestY = 0;
	for (double k = 0.25; k <= 4.0; k *= 1.04) {
		const double ks = k / d;  // small-image pixels per document pixel
		SampleLums(frame, coarse, 0.5 / ks, lums);
		const double tw = ks * frame.w, th = ks * frame.h;
		// Step a quarter of a large block, at least one small-image pixel.
		const double step = fmax(1.0, tw / kBigBlocks / 4);
		for (double oy = -th * 0.85; oy <= small.h - th * 0.15; oy += step)
			for (double ox = -tw * 0.85; ox <= small.w - tw * 0.15; ox += step) {
				int n = 0;
				const double c = Score(small, coarse, lums, ox, oy, ks, &n);
				const double s = c > -1 ? Significance(c, n) : -1e9;
				if (s > best) {
					best = s;
					bestCorr = c;
					bestK = k;
					bestX = ox * d;
					bestY = oy * d;
				}
			}
	}

	// Refine at full resolution. The coarse fit is good to about half a large
	// block, the medium one to about a small block.
	struct Pass { const std::vector<Sample>* samples; double scaleRange, scaleStep; int offRange, offStep; };
	const int halfBlock = static_cast<int>(0.6 * bestK * frame.w / kBigBlocks) + 2 * d;
	const Pass passes[] = {
		{&medium, 0.05, 0.01, halfBlock, 2},
		{&fine, 0.012, 0.002, 2 + static_cast<int>(1.2 * bestK * frame.w / kSmallBlocks), 1},
		{&fine, 0.004, 0.001, 2, 1}};
	for (const Pass& pass : passes) {
		const double k0 = bestK, x0 = bestX, y0 = bestY;
		best = -1e9;
		for (double f = 1 - pass.scaleRange; f <= 1 + pass.scaleRange + 1e-9; f += pass.scaleStep) {
			const double k = k0 * f;
			SampleLums(frame, *pass.samples, 0.5 / k, lums);
			// Keep the document centre fixed while trying scales.
			const double cx = floor(x0 + (k0 - k) * frame.w * 0.5), cy = floor(y0 + (k0 - k) * frame.h * 0.5);
			for (int dy = -pass.offRange; dy <= pass.offRange; dy += pass.offStep)
				for (int dx = -pass.offRange; dx <= pass.offRange; dx += pass.offStep) {
					int n = 0;
					const double c = Score(screen, *pass.samples, lums, cx + dx, cy + dy, k, &n);
					const double s = c > -1 ? Significance(c, n) : -1e9;
					if (s > best) {
						best = s;
						bestCorr = c;
						bestK = k;
						bestX = cx + dx;
						bestY = cy + dy;
					}
				}
		}
	}
	out.pixelsPerDoc = bestK;
	out.screenX = bestX;
	out.screenY = bestY;
	out.percent = 100.0 * bestCorr;
	return bestCorr >= 0.4;
}

// Saves an Rgb image as PNG, for diagnosing calibration.
static void SaveRgb(const std::wstring& path, const Rgb& img)
{
	Gdiplus::Bitmap bmp(img.w, img.h, PixelFormat24bppRGB);
	Gdiplus::Rect r(0, 0, img.w, img.h);
	Gdiplus::BitmapData data;
	if (bmp.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat24bppRGB, &data) != Gdiplus::Ok)
		return;
	for (int y = 0; y < img.h; ++y)
		memcpy(static_cast<BYTE*>(data.Scan0) + y * data.Stride, &img.px[static_cast<size_t>(y) * img.w * 3], img.w * 3);
	bmp.UnlockBits(&data);
	WritePng(path, bmp);
}

// text: "docW docH alpha" — the document size and opacity 0-255. Hides the
// overlay, captures ZBrush's window, finds the canvas by matching
// frame0.png, which must hold the calibration pattern the canvas is showing
// (see RSPattern), against it,
// then shows the overlay there with the last rendered frame's layout. The
// capture is saved as capture.png. Returns the match strength (correlation
// x100, 40-100), or -1 bad arguments, -2 the window
// could not be created, -3 frame0.png missing, -4 the screen could not be
// captured, -5 the pattern was not found on screen
// (RSOverlayInfo 3 gives the best match).
ZFUNC RSOverlayStart(char* text, double, char*, int, char*, int, char**)
{
	double dw, dh, a;
	if (!text || sscanf(text, "%lf %lf %lf", &dw, &dh, &a) != 3 || dw < 1 || dh < 1)
		return -1;
	if (!EnsureOverlay())
		return -2;
	PhysicalPixels physical;
	Rgb frame;
	if (!LoadRgb(DllFolder() + L"frame0.png", frame))
		return -3;

	EnterCriticalSection(&gOverlayLock);
	gOverlay.visible = false;
	LeaveCriticalSection(&gOverlayLock);
	SendMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	Sleep(150);

	HWND zbrush = nullptr;
	EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&zbrush));
	RECT r;
	if (!zbrush || !GetWindowRect(zbrush, &r))
		GetWindowRect(GetDesktopWindow(), &r);
	Rgb screen;
	if (!CaptureScreen(r, screen))
		return -4;
	SaveRgb(DllFolder() + L"capture.png", screen);
	Calibration cal;
	const bool found = Calibrate(frame, screen, cal);
	cal.screenX += r.left;
	cal.screenY += r.top;
	// The frame's size in document pixels can differ from docW if ZBrush
	// reports the document size differently; scale to the frame.
	cal.pixelsPerDoc *= frame.w / dw;
	gCalibration = cal;
	if (!found)
		return -5;

	EnterCriticalSection(&gOverlayLock);
	gOverlay.screenX = cal.screenX;
	gOverlay.screenY = cal.screenY;
	gOverlay.pixelsPerDoc = cal.pixelsPerDoc;
	gOverlay.docW = dw;
	gOverlay.docH = dh;
	gOverlay.alpha = a < 0 ? 0 : a > 255 ? 255 : static_cast<int>(a);
	if (FILE* f = _wfopen(LayoutPath().c_str(), L"rb")) {
		double r, px, py, ax, ay;
		if (fscanf(f, "%lf %lf %lf %lf %lf", &r, &px, &py, &ax, &ay) == 5 && r > 0) {
			gOverlay.ratio = r;
			gOverlay.px = px;
			gOverlay.py = py;
			gOverlay.ax = ax;
			gOverlay.ay = ay;
		}
		fclose(f);
	}
	gOverlay.visible = true;
	LeaveCriticalSection(&gOverlayLock);
	PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return static_cast<float>(floor(cal.percent));
}

// text: "0" screen X of the document's top-left corner, "1" screen Y, "2"
// screen pixels per document pixel (x1000), "3" match strength, from the
// last RSOverlayStart; "4" 1 if the overlay is showing, else 0; "5" 1 if the
// model is keyed in front (capture exclusion works), 0 if Windows has no
// capture exclusion, -1 if it did not work. Navigation model: "6" pan gain,
// "7" and "8" scale per 1000 px of vertical and horizontal travel, "9" pivot
// (0 document centre, 1 cursor, 2 model centre, 3 learned point), "10" and
// "11" moves and scales learned from, "12" the last drag's predicted position
// error in document pixels, "13" its predicted / exact scale. "14" to "16"
// the overlay's current ratio, x and y (including any drag prediction). "17"
// mouse events seen, "18" the last drag's mode (0 rotate, 1 move, 2 scale,
// 3 sculpt) + 10 x scale segments + 100 while dragging. "19" what the last
// finished drag was (0 nothing, 1 move, 2 scale, 3 rotate, 4 sculpt), then
// clears it; "20" capture method (1 desktop duplication, 0 BitBlt); "21"
// overlay frames per second; "22" 1 if ZBrush's values are read from
// memory, "23" to "25" memory candidates left for scale, x and y, "26" how
// many of them sit together the way ZBrush stores a view; "27" to "31" the
// last scan's MB scanned, x matches, x-y pairs, scales near pairs and ms;
// "32" percent of the overlay showing the key colour in the last frame, "33"
// how many of its pixels were not key colour (the model).
ZFUNC RSOverlayInfo(char* text, double, char*, int, char*, int, char**)
{
	const int which = text ? atoi(text) : -1;
	switch (which) {
	case 0: return static_cast<float>(gCalibration.screenX);
	case 1: return static_cast<float>(gCalibration.screenY);
	case 2: return static_cast<float>(floor(gCalibration.pixelsPerDoc * 1000 + 0.5));
	case 3: return static_cast<float>(floor(gCalibration.percent));
	case 4: return gOverlayWnd && gOverlay.visible ? 1 : 0;
	case 5: return static_cast<float>(gKeyState);
	case 6: return static_cast<float>(gNav.panGain);
	case 7: return static_cast<float>(gNav.scaleY * 1000);
	case 8: return static_cast<float>(gNav.scaleX * 1000);
	case 9: return static_cast<float>(gNav.pivot());
	case 10: return static_cast<float>(gNav.moves);
	case 11: return static_cast<float>(gNav.scales);
	case 12: return static_cast<float>(gLastErrorPos);
	case 13: return static_cast<float>(gLastErrorScale);
	case 14: return static_cast<float>(gOverlay.ratio);
	case 15: return static_cast<float>(gOverlay.px);
	case 16: return static_cast<float>(gOverlay.py);
	case 17: return static_cast<float>(gHookEvents);
	case 19: {
		const int kind = gLastKind;
		gLastKind = kKindNone;
		return static_cast<float>(kind);
	}
	case 20: return static_cast<float>(gCaptureMethod);
	case 21: return static_cast<float>(floor(gFps + 0.5));
	case 22: return gMemLocked ? 1 : 0;
	case 23: return static_cast<float>(gMemScale.cands.size());
	case 24: return static_cast<float>(gMemX.cands.size());
	case 25: return static_cast<float>(gMemY.cands.size());
	case 26: return static_cast<float>(gTriplets.size());
	case 32: return static_cast<float>(floor(gMagentaShare + 0.5));
	case 33: return static_cast<float>(gModelPixels);
	case 27: case 28: case 29: case 30: case 31: return static_cast<float>(gScanStats[which - 27]);
	case 18: return static_cast<float>(gGesture.mode + 10 * gGesture.scaleSegments + 100 * gGesture.active);
	}
	return -1;
}

// text: "ratio px py anchorX anchorY [scale]", as for RSRender, plus the
// model scale from TransformGet, which finds ZBrush's values in memory (see
// MemReport). Also how the
// navigation model learns: values set right after a drag are the exact
// result of that drag. Returns 1, -1 for bad arguments, -2 if the overlay is
// not running, 3 if not applied because a drag is in progress or the values
// do not include the last drag yet (call again).
ZFUNC RSOverlaySet(char* text, double, char*, int, char*, int, char**)
{
	double r, px, py, ax, ay, sx = 0;
	if (!text || sscanf(text, "%lf %lf %lf %lf %lf %lf", &r, &px, &py, &ax, &ay, &sx) < 5 || r <= 0)
		return -1;
	if (!gOverlayWnd)
		return -2;
	EnterCriticalSection(&gOverlayLock);
	if (FILE* f = _wfopen((DllFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu set %.5f %.2f %.2f dragging %d unreported %d shown %.5f %.2f %.2f\n", GetTickCount(),
			r, px, py, gGesture.active, gPending.done ? gPendingCount : 0, gOverlay.ratio, gOverlay.px, gOverlay.py);
		fclose(f);
	}
	// ZBrush can report the values from before a drag for a while after it.
	// Values still equal to the drag's start, while the prediction moved,
	// are not applied (the script retries); after a few tries they are.
	if (gGesture.active || (!gMemLocked && gPending.done && gStaleSets < 4 &&
			fabs(px - gPending.px0) < 0.3 && fabs(py - gPending.py0) < 0.3 && fabs(r / gPending.ratio0 - 1) < 0.001 &&
			(fabs(gOverlay.px - px) > 2 || fabs(gOverlay.py - py) > 2 || fabs(gOverlay.ratio / r - 1) > 0.01))) {
		if (!gGesture.active)
			++gStaleSets;
		LeaveCriticalSection(&gOverlayLock);
		return 3;
	}
	if (gPending.done) {
		// How far the drag prediction was from the exact values.
		gLastErrorPos = sqrt((gOverlay.px - px) * (gOverlay.px - px) + (gOverlay.py - py) * (gOverlay.py - py));
		gLastErrorScale = gOverlay.ratio / r;
	}
	Learn(r, px, py);
	if (sx > 0) {
		gAnchorScale = sx / r;
		MemReport(sx, px, py);
	}
	// While ZBrush's values are read from memory those are the truth (a
	// report may be older than them).
	if (!gMemLocked || sx <= 0) {
		gOverlay.ratio = r;
		gOverlay.px = px;
		gOverlay.py = py;
	}
	gOverlay.ax = ax;
	gOverlay.ay = ay;
	LeaveCriticalSection(&gOverlayLock);
	PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return 1;
}

// text: "start|next|last scale x y anchorScale": the
// Overlay button sets a few test views with TransformSet and reports each
// one's exact values. "start" scans ZBrush's memory for them, "next" keeps
// the locations that changed to match, so the camera is found within one
// button press. Returns 1 when locked, 0 if not (yet), -1 bad arguments.
ZFUNC RSMemProbe(char* text, double, char*, int, char*, int, char**)
{
	char mode[16] = {0};
	double sx, px, py, anchor;
	if (!text || sscanf(text, "%15s %lf %lf %lf %lf", mode, &sx, &px, &py, &anchor) != 5 || sx <= 0 || anchor <= 0)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	gAnchorScale = anchor;
	if (strcmp(mode, "start") == 0) {
		gMemLocked = false;
		gMemScanned = false;
	}
	gMemDisagree = 0;
	MemReport(sx, px, py);
	// The test views change the values without a mouse drag; that must not
	// count against the right locations. After the last report ("last"),
	// locations that still change on their own are weeded out as usual.
	gMemWatchUntil = strcmp(mode, "last") == 0 ? GetTickCount() + 1500 : GetTickCount();
	const bool locked = gMemLocked;
	LeaveCriticalSection(&gOverlayLock);
	return locked ? 1 : 0;
}

// Saves what the overlay last captured (snap_capture.png, the canvas as
// ZBrush drew it) and what it showed (snap_overlay.png) next to the DLL.
// Returns 1, or 0 if the overlay has not drawn anything yet.
ZFUNC RSSnapshot(char*, double, char*, int, char*, int, char**)
{
	if (!gOverlayLockReady || !gCapture.bits || !gOutput.bits)
		return 0;
	StartGdiplus();
	EnterCriticalSection(&gOverlayLock);
	Rgb cap, out;
	cap.w = out.w = gCapture.w;
	cap.h = out.h = gCapture.h;
	cap.px.resize(static_cast<size_t>(cap.w) * cap.h * 3);
	out.px.resize(cap.px.size());
	for (size_t i = 0, n = static_cast<size_t>(cap.w) * cap.h; i < n; ++i)
		for (int c = 0; c < 3; ++c) {
			cap.px[i * 3 + c] = gCapture.bits[i * 4 + c];
			out.px[i * 3 + c] = gOutput.bits[i * 4 + c];
		}
	LeaveCriticalSection(&gOverlayLock);
	SaveRgb(DllFolder() + L"snap_capture.png", cap);
	SaveRgb(DllFolder() + L"snap_overlay.png", out);
	return 1;
}

// Clears the rotation offset (the view is back at the anchored rotation).
// Returns 1 if there was one, else 0.
ZFUNC RSOverlayResetOffset(char*, double, char*, int, char*, int, char**)
{
	if (!gOverlayLockReady)
		return 0;
	EnterCriticalSection(&gOverlayLock);
	const bool had = gOverlay.offX != 0 || gOverlay.offY != 0;
	gOverlay.offX = gOverlay.offY = 0;
	LeaveCriticalSection(&gOverlayLock);
	return had ? 1 : 0;
}

// Hides the overlay. Returns 1.
ZFUNC RSOverlayStop(char*, double, char*, int, char*, int, char**)
{
	if (gOverlayWnd) {
		EnterCriticalSection(&gOverlayLock);
		gOverlay.visible = false;
		LeaveCriticalSection(&gOverlayLock);
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	}
	return 1;
}
