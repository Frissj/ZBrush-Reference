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
#include <tlhelp32.h>
#include <shlwapi.h>
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

// Where the DLL writes (frames, logs, this computer's settings): its own
// folder, or, when that cannot be written (installed in ZBrush's ZStartup
// folder without admin rights), C:\\Users\\Public\\Documents\\JRef\\. The
// script asks RSDataFolder which, to find frame0.png.
static std::wstring gDataFolder;

static std::wstring DataFolder()
{
	if (!gDataFolder.empty())
		return gDataFolder;
	const std::wstring own = DllFolder(), test = own + L"write_test.tmp";
	HANDLE h = CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
		gDataFolder = own;
	} else {
		CreateDirectoryW(L"C:\\Users\\Public\\Documents\\JRef", nullptr);
		gDataFolder = L"C:\\Users\\Public\\Documents\\JRef\\";
	}
	return gDataFolder;
}

// 1 when the DLL writes to C:\\Users\\Public\\Documents\\JRef\\, 0 when to its own
// folder.
extern "C" __declspec(dllexport) float RSDataFolder(char*, double, char*, int, char*, int, char**)
{
	return DataFolder() == DllFolder() ? 0.0f : 1.0f;
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

// A JRef project: the folder ("<images folder>\\JRef\\") where a project's
// views (project.jref), outlines, display settings and copies of its images
// are kept, so it can be copied to another computer with the images. The last
// one opened is reopened next time (last_project.txt next to the DLL).
// Without one, the DLL's folder holds them.
static std::wstring gProjectDir;
static bool gProjectTried = false;

static std::wstring ProjectFolder()
{
	if (!gProjectTried) {
		gProjectTried = true;
		if (FILE* f = _wfopen((DataFolder() + L"last_project.txt").c_str(), L"rb")) {
			char buf[1024] = {0};
			buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0;
			fclose(f);
			std::string dir = buf;
			while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r'))
				dir.pop_back();
			const std::wstring w = Widen(dir.c_str());
			const DWORD attr = w.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(w.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
				gProjectDir = w;
		}
	}
	return gProjectDir.empty() ? DataFolder() : gProjectDir;
}

// An image path as stored: relative ones are in the project folder.
static std::wstring ResolveImage(const std::string& path)
{
	const bool absolute = path.size() > 1 && (path[1] == ':' || (path[0] == '\\' && path[1] == '\\'));
	return absolute ? Widen(path.c_str()) : ProjectFolder() + Widen(path.c_str());
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
	return DataFolder() + L"reference_path.txt";
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
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu %s\n", GetTickCount(), text);
		fclose(f);
	}
	return 1;
}

// The anchor (model scale, x, y and rotation x, y, z recorded by Anchor) is
// kept in anchor.txt next to the DLL, so it survives script reloads and new
// builds, whose sliders start empty.
static void ViewsDefaultChanged(const double* anchor, const char* imagePath);

static std::wstring AnchorPath()
{
	return ProjectFolder() + L"anchor.txt";
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
	ViewsDefaultChanged(v, nullptr);
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
static const int kDllBuild = 88;

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
	ViewsDefaultChanged(nullptr, text);
	return 1;
}

// Layout of the last rendered frame, "ratio px py anchorX anchorY".
static std::wstring LayoutPath()
{
	return DataFolder() + L"frame_layout.txt";
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
	if (!WritePng(absolute ? name : DataFolder() + name, frame))
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
	return WritePng(DataFolder() + Widen(frameName), frame) ? 1 : -4;
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
	return WritePng(DataFolder() + Widen(frameName), frame) ? 1 : -4;
}

// Settings snapshot, for comparing ZBrush state before and after another
// plugin runs (which resets the script's variables). Values are kept in
// snapshot.txt next to the DLL as "id value" lines.
static std::wstring SnapshotPath()
{
	return DataFolder() + L"snapshot.txt";
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
	Gdiplus::Bitmap* frame = Gdiplus::Bitmap::FromFile((absolute ? name : DataFolder() + name).c_str());
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
	// The image's own adjustment (Adjust Image): width and height scale, and
	// its centre's shift in anchored document pixels.
	double imgSX = 1, imgSY = 1, imgX = 0, imgY = 0;
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
	return DataFolder() + L"nav_model.txt";
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

// Adjust Image: while on, mouse buttons on the canvas go to the image (ZBrush
// does not see them): drag inside it to move it, drag a corner (or drag with
// the right button) to scale it about its centre.
static volatile bool gAdjust = false;
// Mouse buttons held, and when one last went down or up (the camera only
// turns while one is held, or in the snap animation just after).
static volatile LONG gButtonsDown = 0;
static volatile DWORD gLastButtonAt = 0;
static volatile bool gAdjustEscape = false;  // Esc pressed: the overlay turns Adjust Image off
static bool AdjustMouse(WPARAM wp, POINT pt);

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
{
	if (code != HC_ACTION)
		return CallNextHookEx(gMouseHook, code, wp, lp);
	const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
	++gHookEvents;
	if (wp != WM_MOUSEMOVE && wp != WM_MOUSEWHEEL && wp != WM_MOUSEHWHEEL) {
		gLastButtonAt = GetTickCount();
		if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)
			++gButtonsDown;
		else if ((wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP) && gButtonsDown > 0)
			--gButtonsDown;
	}
	const bool alt = gAltDown, ctrl = gCtrlDown;
	EnterCriticalSection(&gOverlayLock);
	if (gAdjust) {
		const bool eaten = AdjustMouse(wp, m->pt);
		LeaveCriticalSection(&gOverlayLock);
		return eaten ? 1 : CallNextHookEx(gMouseHook, code, wp, lp);
	}
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
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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
		case VK_ESCAPE: if (down && gAdjust) gAdjustEscape = true; break;
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
	// 2 when the hook kept the event from ZBrush.
	return MouseHook(HC_ACTION, kinds[e], reinterpret_cast<LPARAM>(&m)) == 1 ? 2 : 1;
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

// The image the overlay shows: the showing view's (empty: none). Kept
// apart from the canvas-fill reference so the overlay shows only view images.
static std::string gOverlayPath;

// Mip levels of an image, cached for the few images on show (PureRef's
// multiview shows four at once), reloaded when the file changes (a project's
// "Front.png" replaced by another image). Used by the overlay thread only.
struct ImageCache {
	std::wstring path;
	std::vector<Level> levels;
	DWORD usedAt;
	FILETIME written;
};

static FILETIME WrittenAt(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA data = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
		return FILETIME{0, 0};
	return data.ftLastWriteTime;
}
static std::vector<ImageCache> gImages;

static bool LoadLevels(const std::wstring& path, std::vector<Level>& levels)
{
	levels.clear();
	StartGdiplus();
	Gdiplus::Bitmap* image = Gdiplus::Bitmap::FromFile(path.c_str());
	if (!image || image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0) {
		delete image;
		return false;
	}
	Level top;
	top.w = image->GetWidth();
	top.h = image->GetHeight();
	top.px.resize(static_cast<size_t>(top.w) * top.h);
	Gdiplus::Rect r(0, 0, top.w, top.h);
	Gdiplus::BitmapData data;
	if (image->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
		delete image;
		return false;
	}
	for (int y = 0; y < top.h; ++y)
		memcpy(&top.px[static_cast<size_t>(y) * top.w], static_cast<BYTE*>(data.Scan0) + y * data.Stride, top.w * 4);
	image->UnlockBits(&data);
	delete image;
	levels.push_back(std::move(top));
	while (levels.back().w > 16 && levels.back().h > 16) {
		const Level& a = levels.back();
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
		levels.push_back(std::move(b));
	}
	return true;
}

// The levels of the image at path (UTF-8), loading it if needed, or null.
static const std::vector<Level>* LevelsFor(const std::string& path)
{
	if (path.empty())
		return nullptr;
	const std::wstring w = ResolveImage(path);
	const FILETIME written = WrittenAt(w);
	for (size_t i = 0; i < gImages.size(); ++i) {
		ImageCache& c = gImages[i];
		if (c.path != w)
			continue;
		if (CompareFileTime(&c.written, &written) == 0) {
			c.usedAt = GetTickCount();
			return c.levels.empty() ? nullptr : &c.levels;
		}
		gImages.erase(gImages.begin() + i);
		break;
	}
	if (gImages.size() >= 4) {
		size_t oldest = 0;
		for (size_t i = 1; i < gImages.size(); ++i)
			if (static_cast<int>(gImages[i].usedAt - gImages[oldest].usedAt) < 0)
				oldest = i;
		gImages.erase(gImages.begin() + oldest);
	}
	gImages.push_back({w, {}, GetTickCount(), written});
	LoadLevels(w, gImages.back().levels);
	return gImages.back().levels.empty() ? nullptr : &gImages.back().levels;
}

static int gImgW = 0, gImgH = 0;  // the shown image's size (0: none)

// The image's rectangle on the overlay, in overlay pixels: fitted to the
// document and centred (as in RSRender), adjusted by Adjust Image, scaled by
// ratio about the anchored model position and moved with the model.
// Returns false if no image is shown.
static bool ImageRectFor(const OverlayState& st, int iw, int ih, double& x0, double& y0, double& w, double& h)
{
	if (iw <= 0 || ih <= 0 || st.docW < 1 || st.docH < 1)
		return false;
	const double imgW = iw, imgH = ih;
	const double fit = (st.docW / imgW < st.docH / imgH) ? st.docW / imgW : st.docH / imgH;
	const double baseW = imgW * fit * st.imgSX, baseH = imgH * fit * st.imgSY;
	const double cx = st.docW * 0.5 + st.imgX, cy = st.docH * 0.5 + st.imgY;
	const double k = st.pixelsPerDoc;
	x0 = (st.px + st.ratio * (cx - baseW * 0.5 - st.ax - st.offX)) * k;
	y0 = (st.py + st.ratio * (cy - baseH * 0.5 - st.ay - st.offY)) * k;
	w = baseW * st.ratio * k;
	h = baseH * st.ratio * k;
	return true;
}

// The same for the image the overlay shows (the showing view's).
static bool ImageRect(const OverlayState& st, double& x0, double& y0, double& w, double& h)
{
	return ImageRectFor(st, gImgW, gImgH, x0, y0, w, h);
}


static void SaveAdjustToView();

// 0 none, 1 moving, 2 a corner (keeps the aspect, the opposite corner
// stays), 3 right drag (scales about the centre), 4 a left or right edge,
// 5 the top or bottom edge (the opposite edge stays).
static int gAdjustDrag = 0;
static int gAdjustSideX = 0, gAdjustSideY = 0;
static POINT gAdjustDown;
static double gAdjustSX0 = 1, gAdjustSY0 = 1, gAdjustX0 = 0, gAdjustY0 = 0;

// The image fitted to the document, before Adjust Image, in document pixels.
static bool FitSize(const OverlayState& st, double& w, double& h)
{
	if (gImgW <= 0 || gImgH <= 0 || st.docW < 1 || st.docH < 1)
		return false;
	const double fit = (st.docW / gImgW < st.docH / gImgH) ? st.docW / gImgW : st.docH / gImgH;
	w = gImgW * fit;
	h = gImgH * fit;
	return true;
}

// Handles a mouse event in Adjust Image mode. Caller holds the lock.
// Returns true if ZBrush must not see it.
static bool AdjustMouse(WPARAM wp, POINT pt)
{
	OverlayState& st = gOverlay;
	const double k = st.pixelsPerDoc;
	switch (wp) {
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN: {
		double x0, y0, w, h;
		if (gAdjustDrag || !ZBrushInFront() || !CursorOverOverlay(pt, st) || !ImageRect(st, x0, y0, w, h))
			return false;
		x0 += st.screenX;
		y0 += st.screenY;
		// Only where the reference shows (the canvas background): clicks on
		// the model, or on ZBrush's popups and menus over the canvas, go to
		// ZBrush.
		const int kx = static_cast<int>(pt.x - st.screenX), ky = static_cast<int>(pt.y - st.screenY);
		if (kx < 0 || ky < 0 || kx >= gKeyW || ky >= gKeyH || gKeyShare.size() != static_cast<size_t>(gKeyW) * gKeyH ||
			gKeyShare[static_cast<size_t>(ky) * gKeyW + kx] < 128)
			return false;
		gAdjustSX0 = st.imgSX;
		gAdjustSY0 = st.imgSY;
		gAdjustX0 = st.imgX;
		gAdjustY0 = st.imgY;
		// Handles stay on the canvas when the image is larger.
		const double r = 12, right = st.screenX + st.docW * k - 1, bottom = st.screenY + st.docH * k - 1;
		const double hx0 = std::max(x0, st.screenX), hx1 = std::min(x0 + w, right);
		const double hy0 = std::max(y0, st.screenY), hy1 = std::min(y0 + h, bottom);
		const int sideX = fabs(pt.x - hx0) < r ? -1 : fabs(pt.x - hx1) < r ? 1 : 0;
		const int sideY = fabs(pt.y - hy0) < r ? -1 : fabs(pt.y - hy1) < r ? 1 : 0;
		const bool withinX = pt.x > hx0 - r && pt.x < hx1 + r, withinY = pt.y > hy0 - r && pt.y < hy1 + r;
		const bool inside = pt.x >= x0 && pt.x < x0 + w && pt.y >= y0 && pt.y < y0 + h;
		if (!(sideX && withinY) && !(sideY && withinX) && !inside)
			return false;  // off the image: ZBrush gets the click
		gAdjustDown = pt;
		gAdjustSideX = sideX;
		gAdjustSideY = sideY;
		if (wp == WM_RBUTTONDOWN)
			gAdjustDrag = 3;
		else if (sideX && sideY)
			gAdjustDrag = 2;
		else if (sideX && withinY)
			gAdjustDrag = 4;
		else if (sideY && withinX)
			gAdjustDrag = 5;
		else
			gAdjustDrag = 1;
		return true;
	}
	case WM_MOUSEMOVE: {
		double bw, bh;
		if (!gAdjustDrag || st.ratio <= 0 || k <= 0 || !FitSize(st, bw, bh))
			return false;
		// The drag in anchored document pixels.
		const double du = (pt.x - gAdjustDown.x) / (k * st.ratio), dv = (pt.y - gAdjustDown.y) / (k * st.ratio);
		const double w0 = bw * gAdjustSX0, h0 = bh * gAdjustSY0;
		double w = w0, h = h0, cx = gAdjustX0, cy = gAdjustY0;
		const double minSize = 4 / (k * st.ratio);
		if (gAdjustDrag == 1) {
			cx += du;
			cy += dv;
		} else if (gAdjustDrag == 2) {
			double f = 0.5 * ((w0 + gAdjustSideX * du) / w0 + (h0 + gAdjustSideY * dv) / h0);
			if (f * w0 < minSize || f * h0 < minSize)
				f = std::max(minSize / w0, minSize / h0);
			w = w0 * f;
			h = h0 * f;
			cx += gAdjustSideX * (w - w0) * 0.5;
			cy += gAdjustSideY * (h - h0) * 0.5;
		} else if (gAdjustDrag == 3) {
			const double f = exp(((pt.x - gAdjustDown.x) - (pt.y - gAdjustDown.y)) / 400.0);
			w = w0 * f;
			h = h0 * f;
		} else if (gAdjustDrag == 4) {
			w = std::max(minSize, w0 + gAdjustSideX * du);
			cx += gAdjustSideX * (w - w0) * 0.5;
		} else {
			h = std::max(minSize, h0 + gAdjustSideY * dv);
			cy += gAdjustSideY * (h - h0) * 0.5;
		}
		st.imgSX = std::min(100.0, w / bw);
		st.imgSY = std::min(100.0, h / bh);
		st.imgX = cx;
		st.imgY = cy;
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
		return false;  // the cursor still moves
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
		if (!gAdjustDrag)
			return false;
		gAdjustDrag = 0;
		SaveAdjustToView();
		return true;
	}
	return false;
}

static void FillBox(Buffer32& b, int x0, int y0, int x1, int y1, uint32_t bgr)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > b.w) x1 = b.w;
	if (y1 > b.h) y1 = b.h;
	for (int y = y0; y < y1; ++y) {
		uint32_t* row = reinterpret_cast<uint32_t*>(b.bits) + static_cast<size_t>(y) * b.w;
		for (int x = x0; x < x1; ++x)
			row[x] = 0xFF000000u | bgr;
	}
}

// Draws the Adjust Image handles: the image's outline, its corners and centre.
static void DrawGizmo(const OverlayState& st, Buffer32& b)
{
	double fx, fy, fw, fh;
	if (!ImageRect(st, fx, fy, fw, fh))
		return;
	const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	const int x1 = static_cast<int>(fx + fw), y1 = static_cast<int>(fy + fh);
	const uint32_t orange = 0xFF9020;  // R, G, B as 0xRRGGBB in the BGRA buffer
	FillBox(b, x0, y0, x1, y0 + 2, orange);
	FillBox(b, x0, y1 - 2, x1, y1, orange);
	FillBox(b, x0, y0, x0 + 2, y1, orange);
	FillBox(b, x1 - 2, y0, x1, y1, orange);
	const int c = 7;
	// Corner handles stay on the canvas when the image is larger.
	const int xs[2] = {std::max(x0, c), std::min(x1, b.w - c)}, ys[2] = {std::max(y0, c), std::min(y1, b.h - c)};
	for (int i = 0; i < 2; ++i)
		for (int j = 0; j < 2; ++j)
			FillBox(b, xs[i] - c, ys[j] - c, xs[i] + c, ys[j] + c, orange);
	// Edge handles (pull one side), at the middle of each visible edge.
	const int mx = (xs[0] + xs[1]) / 2, my = (ys[0] + ys[1]) / 2, e = 5;
	FillBox(b, xs[0] - e, my - 2 * e, xs[0] + e, my + 2 * e, orange);
	FillBox(b, xs[1] - e, my - 2 * e, xs[1] + e, my + 2 * e, orange);
	FillBox(b, mx - 2 * e, ys[0] - e, mx + 2 * e, ys[0] + e, orange);
	FillBox(b, mx - 2 * e, ys[1] - e, mx + 2 * e, ys[1] + e, orange);
	const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	FillBox(b, cx - 10, cy - 1, cx + 10, cy + 1, orange);
	FillBox(b, cx - 1, cy - 10, cx + 1, cy + 10, orange);
}

// Display: Classic shows the reference behind the model. PureRef shows it in
// a panel at one side of the canvas (right unless gPanelLeft), zoomed and
// panned with the camera as Classic would show it; with gMulti, two views
// (gMultiA above gMultiB) share the panel. Outside the panel the background
// shows gBackGrey.
static int gMode = 0, gPanelLeft = 0, gMulti = 0, gMultiA = 0, gMultiB = 3, gBackGrey = 48;
// Multiview's four cells (top left, top right, bottom right, bottom left):
// a view slot each, or -1 for none.
static int gCells[4] = {0, 3, 4, 1};
// The reference's gamma and saturation, in percent (100 leaves it as is),
// from the plugin's sliders (not saved: the sliders start at 100).
static int gGamma = 100, gSat = 100;
static const double kPanelShare = 0.35;  // of the canvas width

// The part of the document ZBrush shows (overlay pixels, x1 and y1 past the
// end): a document larger than ZBrush's viewport has its edges under ZBrush's
// interface. Learned from where the background (magenta) is seen; empty
// until then, which means the whole document.
static int gViewX0 = 0, gViewY0 = 0, gViewX1 = 0, gViewY1 = 0;
static bool gRedrawPending = false;
// ZBrush's view gizmo (the head and axes) in the viewport's top right
// corner, which outlines leave out: its bottom in overlay pixels, 0 until
// seen.
static int gGizmoBottom = 0;

static void ViewportRect(int w, int h, int& x0, int& y0, int& x1, int& y1)
{
	if (gViewX1 > gViewX0 && gViewY1 > gViewY0) {
		x0 = std::max(0, gViewX0);
		y0 = std::max(0, gViewY0);
		x1 = std::min(w, gViewX1);
		y1 = std::min(h, gViewY1);
	} else {
		x0 = y0 = 0;
		x1 = w;
		y1 = h;
	}
}

// The panel in overlay pixels (PureRef), at one side of the viewport.
static void PanelRect(int w, int h, int& x0, int& y0, int& pw, int& ph)
{
	int vx0, vy0, vx1, vy1;
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	pw = static_cast<int>((vx1 - vx0) * kPanelShare + 0.5);
	x0 = gPanelLeft ? vx0 : vx1 - pw;
	y0 = vy0;
	ph = vy1 - vy0;
}

// Layout and image of a view slot for the panel, from the current camera.
static bool ViewLayout(int slot, const OverlayState& st, OverlayState& out, std::string& path);
static bool MultiviewLayout(int slot, const OverlayState& st, OverlayState& out, std::string& path);
static int ActiveViewSlot();

// Draws an image (premultiplied, scaled by alpha) over dst, its rectangle at
// x0, y0 and dispW x dispH, only inside the clip rectangle.
static void DrawLevels(Buffer32& dst, const std::vector<Level>& levels, double x0, double y0, double dispW, double dispH,
	int cx0, int cy0, int cx1, int cy1, int alphaScale)
{
	if (dispW <= 0 || dispH <= 0 || levels.empty())
		return;
	// Smallest level still at least as large as the image on screen.
	size_t li = 0;
	while (li + 1 < levels.size() && levels[li + 1].w >= dispW && levels[li + 1].h >= dispH)
		++li;
	const Level& lv = levels[li];
	cx0 = std::max(cx0, std::max(0, static_cast<int>(floor(x0))));
	cy0 = std::max(cy0, std::max(0, static_cast<int>(floor(y0))));
	cx1 = std::min(cx1, std::min(dst.w, static_cast<int>(ceil(x0 + dispW))));
	cy1 = std::min(cy1, std::min(dst.h, static_cast<int>(ceil(y0 + dispH))));
	if (cx0 >= cx1 || cy0 >= cy1)
		return;
	// Gamma (on the unpremultiplied colour) and saturation.
	const bool adjust = gGamma != 100 || gSat != 100;
	unsigned char gammaLut[256];
	for (int i = 0; i < 256; ++i)
		gammaLut[i] = static_cast<unsigned char>(255 * pow(i / 255.0, 100.0 / gGamma) + 0.5);
	const int sat = gSat * 256 / 100;
	// Per column and row: the two texels to blend and the weight (0-256) of
	// the second, or -1 outside the image.
	std::vector<int> cx(cx1 - cx0), cw(cx1 - cx0);
	for (int x = cx0; x < cx1; ++x) {
		const double u = (x + 0.5 - x0) / dispW * lv.w - 0.5;
		int& ix = cx[x - cx0];
		if (u < -0.5 || u > lv.w - 0.5) { ix = -1; continue; }
		const double fu = u < 0 ? 0 : u > lv.w - 1 ? lv.w - 1 : u;
		ix = static_cast<int>(fu);
		cw[x - cx0] = static_cast<int>((fu - ix) * 256);
	}
	uint32_t* out = reinterpret_cast<uint32_t*>(dst.bits);
	for (int y = cy0; y < cy1; ++y) {
		const double v = (y + 0.5 - y0) / dispH * lv.h - 0.5;
		if (v < -0.5 || v > lv.h - 0.5)
			continue;
		const double fv = v < 0 ? 0 : v > lv.h - 1 ? lv.h - 1 : v;
		const int iy = static_cast<int>(fv), wy = static_cast<int>((fv - iy) * 256);
		const uint32_t* r0 = &lv.px[static_cast<size_t>(iy) * lv.w];
		const uint32_t* r1 = iy + 1 < lv.h ? r0 + lv.w : r0;
		uint32_t* row = out + static_cast<size_t>(y) * dst.w;
		for (int x = cx0; x < cx1; ++x) {
			const int ix = cx[x - cx0];
			if (ix < 0)
				continue;
			const int ix1 = ix + 1 < lv.w ? ix + 1 : ix;
			const int wx = cw[x - cx0];
			int ch[4];
			for (int k = 0, c = 24; c >= 0; c -= 8, ++k) {
				const int a = (r0[ix] >> c) & 255, b = (r0[ix1] >> c) & 255;
				const int d = (r1[ix] >> c) & 255, e = (r1[ix1] >> c) & 255;
				const int top = a * 256 + (b - a) * wx, bottom = d * 256 + (e - d) * wx;
				ch[k] = (top * 256 + (bottom - top) * wy) >> 16;
			}
			if (adjust && ch[0] > 0) {
				const int a = ch[0];
				const int luma = (ch[1] * 77 + ch[2] * 150 + ch[3] * 29) >> 8;
				for (int k = 1; k < 4; ++k) {
					int v = luma + (((ch[k] - luma) * sat) >> 8);
					v = v < 0 ? 0 : v > a ? a : v;
					ch[k] = a == 255 ? gammaLut[v] : gammaLut[v * 255 / a] * a / 255;
				}
			}
			const int alpha = ch[0] * alphaScale / 255;
			// Over what is there already (premultiplied).
			const uint32_t under = row[x];
			uint32_t result = 0;
			for (int k = 0, c = 24; c >= 0; c -= 8, ++k) {
				const int src = k == 0 ? alpha : ch[k] * alpha / 255;
				const int dv = (under >> c) & 255;
				const int v = src + dv * (255 - alpha) / 255;
				result |= static_cast<uint32_t>(v > 255 ? 255 : v) << c;
			}
			row[x] = result;
		}
	}
}

static void FillRect(Buffer32& b, int x0, int y0, int x1, int y1, uint32_t argb)
{
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, b.w);
	y1 = std::min(y1, b.h);
	for (int y = y0; y < y1; ++y) {
		uint32_t* row = reinterpret_cast<uint32_t*>(b.bits) + static_cast<size_t>(y) * b.w;
		for (int x = x0; x < x1; ++x)
			row[x] = argb;
	}
}

// One view in a panel cell: the canvas as Classic would show it (that view's
// image placed by the camera), fitted into the cell.
static void DrawPanelCell(const OverlayState& st, int slot, int x0, int y0, int cw, int ch)
{
	FillRect(gRefLayer, x0, y0, x0 + cw, y0 + ch, 0xFF181818u);
	OverlayState vs;
	std::string path;
	if (slot < 0 || !MultiviewLayout(slot, st, vs, path))
		return;
	const std::vector<Level>* levels = LevelsFor(path);
	if (!levels)
		return;
	double ix, iy, iw, ih;
	if (!ImageRectFor(vs, (*levels)[0].w, (*levels)[0].h, ix, iy, iw, ih))
		return;
	// The viewport as Classic would show it, fitted into the cell.
	int vx0, vy0, vx1, vy1;
	ViewportRect(gRefLayer.w, gRefLayer.h, vx0, vy0, vx1, vy1);
	const double vw = vx1 - vx0, vh = vy1 - vy0;
	const double s = std::min(cw / vw, ch / vh);
	const double ox = x0 + (cw - vw * s) * 0.5 - vx0 * s, oy = y0 + (ch - vh * s) * 0.5 - vy0 * s;
	DrawLevels(gRefLayer, *levels, ox + ix * s, oy + iy * s, iw * s, ih * s, x0, y0, x0 + cw, y0 + ch, 255);
}

// Builds gRefLayer: what replaces the key colour (Classic: the reference,
// transparent black elsewhere; PureRef: the background colour, and the panel,
// which Composite shows over the model). Returns false if the overlay is off.
static bool BuildRefLayer(const OverlayState& st)
{
	if (!st.visible || st.docW < 1 || st.docH < 1)
		return false;
	const int w = static_cast<int>(st.docW * st.pixelsPerDoc + 0.5);
	const int h = static_cast<int>(st.docH * st.pixelsPerDoc + 0.5);
	if (!gRefLayer.Resize(w, h) || !gCapture.Resize(w, h) || !gOutput.Resize(w, h))
		return false;
	const std::vector<Level>* levels = LevelsFor(gOverlayPath);
	gImgW = levels ? (*levels)[0].w : 0;
	gImgH = levels ? (*levels)[0].h : 0;
	if (gMode == 1) {
		const uint32_t g = static_cast<uint32_t>(gBackGrey & 255);
		FillRect(gRefLayer, 0, 0, w, h, 0xFF000000u | g << 16 | g << 8 | g);
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		if (gMulti) {
			const int hw = pw / 2, hh = ph / 2;
			DrawPanelCell(st, gCells[0], px, py, hw, hh);
			DrawPanelCell(st, gCells[1], px + hw, py, pw - hw, hh);
			DrawPanelCell(st, gCells[2], px + hw, py + hh, pw - hw, ph - hh);
			DrawPanelCell(st, gCells[3], px, py + hh, hw, ph - hh);
			FillRect(gRefLayer, px, py + hh - 1, px + pw, py + hh + 1, 0xFF505050u);
			FillRect(gRefLayer, px + hw - 1, py, px + hw + 1, py + ph, 0xFF505050u);
		} else {
			DrawPanelCell(st, ActiveViewSlot(), px, py, pw, ph);
		}
		return true;
	}
	// Classic. No image (none for this view): the background shows black.
	memset(gRefLayer.bits, 0, static_cast<size_t>(w) * h * 4);
	double x0, y0, dispW, dispH;
	if (levels && ImageRect(st, x0, y0, dispW, dispH))
		DrawLevels(gRefLayer, *levels, x0, y0, dispW, dispH, 0, 0, w, h, st.alpha);
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

// ZBrush's other windows (popups, menus, notes) on screen, which the PureRef
// panel must not cover. Refreshed every 100 ms; each change is logged.
static std::vector<RECT> gUiRects;

struct UiScan {
	HWND main;
	std::vector<RECT> rects;
	std::string desc;
};

static BOOL CALLBACK CollectUiWindow(HWND wnd, LPARAM param)
{
	UiScan* scan = reinterpret_cast<UiScan*>(param);
	DWORD pid = 0;
	GetWindowThreadProcessId(wnd, &pid);
	if (pid != GetCurrentProcessId() || wnd == gOverlayWnd || wnd == scan->main || !IsWindowVisible(wnd))
		return TRUE;
	RECT r;
	if (!GetWindowRect(wnd, &r) || r.right <= r.left || r.bottom <= r.top)
		return TRUE;
	scan->rects.push_back(r);
	char cls[64] = {0}, line[160];
	GetClassNameA(wnd, cls, sizeof(cls));
	snprintf(line, sizeof(line), " [%s %ld %ld %ld %ld]", cls, r.left, r.top, r.right, r.bottom);
	scan->desc += line;
	return TRUE;
}

static void RefreshUiRects()
{
	static DWORD at = 0;
	static std::string lastDesc = "-";
	const DWORD now = GetTickCount();
	if (now - at < 100)
		return;
	at = now;
	UiScan scan;
	scan.main = nullptr;
	EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&scan.main));
	EnumWindows(CollectUiWindow, reinterpret_cast<LPARAM>(&scan));
	std::string children;
	if (scan.main) {
		UiScan kids;
		kids.main = nullptr;
		EnumChildWindows(scan.main, CollectUiWindow, reinterpret_cast<LPARAM>(&kids));
		children = kids.desc;
	}
	gUiRects = scan.rects;
	const std::string desc = scan.desc + " | children" + children;
	if (desc != lastDesc) {
		lastDesc = desc;
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu zbrush windows:%s\n", now, desc.c_str());
			fclose(f);
		}
	}
}

// While the cursor is over the panel, the panel steps aside and ZBrush
// shows under it (its view gizmo, menus drawn inside its window). Cursor
// changes are logged (the canvas cursor is the one most seen over the
// canvas outside the panel).
static bool gPanelAside = false;

static bool UpdatePanelAside(const OverlayState& st)
{
	const bool before = gPanelAside;
	gPanelAside = false;
	if (gMode != 1 || !st.visible)
		return before != gPanelAside;
	CURSORINFO ci = {sizeof(ci)};
	if (!GetCursorInfo(&ci))
		return before != gPanelAside;
	const HCURSOR cur = (ci.flags & CURSOR_SHOWING) ? ci.hCursor : nullptr;
	const int w = static_cast<int>(st.docW * st.pixelsPerDoc + 0.5), h = static_cast<int>(st.docH * st.pixelsPerDoc + 0.5);
	int px, py, pw, ph;
	PanelRect(w, h, px, py, pw, ph);
	const int x = static_cast<int>(ci.ptScreenPos.x - st.screenX), y = static_cast<int>(ci.ptScreenPos.y - st.screenY);
	const bool onCanvas = x >= 0 && y >= 0 && x < w && y < h;
	const bool onPanel = onCanvas && x >= px && x < px + pw && y >= py && y < py + ph;
	static std::vector<std::pair<HCURSOR, int>> seen;
	static HCURSOR canvas = nullptr, logged = reinterpret_cast<HCURSOR>(-1);
	if (onCanvas && !onPanel && ZBrushInFront()) {
		bool found = false;
		for (auto& c : seen)
			if (c.first == cur) { ++c.second; found = true; }
		if (!found)
			seen.push_back({cur, 1});
		int best = 0;
		for (const auto& c : seen)
			if (c.second > best) { best = c.second; canvas = c.first; }
	}
	// Over the panel, ZBrush shows (the view gizmo, menus): the panel is
	// see-through until the cursor leaves it.
	if (onPanel)
		gPanelAside = true;
	if (cur != logged && onCanvas) {
		logged = cur;
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu cursor %p (arrow %p, canvas %p) at %d %d, on panel %d, panel aside %d\n", GetTickCount(),
				static_cast<void*>(cur), static_cast<void*>(LoadCursor(nullptr, IDC_ARROW)), static_cast<void*>(canvas), x, y,
				onPanel ? 1 : 0, gPanelAside ? 1 : 0);
			fclose(f);
		}
	}
	return before != gPanelAside;
}

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
	static std::vector<int> rowMagenta, colMagenta;
	rowMagenta.assign(h, 0);
	colMagenta.assign(w, 0);
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
		if (t > 128) {
			++rowMagenta[i / w];
			++colMagenta[i % w];
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
	if (gViewX1 > gViewX0 && gViewY1 > gViewY0) {
		// The gizmo: what is not background in the viewport's top right
		// corner (a square of 30% of its height), when the background shows
		// in most of that corner (the model is not across it). Its lowest
		// row so far, plus a margin.
		const int q = static_cast<int>(std::min(gViewX1 - gViewX0, gViewY1 - gViewY0) * 0.3);
		const int cx0 = std::max(0, gViewX1 - q), cy0 = std::max(0, gViewY0), cx1 = std::min(w, gViewX1), cy1 = std::min(h, gViewY0 + q);
		int lowest = -1;
		size_t other = 0, all = 0;
		for (int y = cy0; y < cy1; ++y)
			for (int x = cx0; x < cx1; ++x) {
				++all;
				// A few pixels in from the viewport's edge: its border is not the gizmo.
				if (gKeyShare[static_cast<size_t>(y) * w + x] < 64 && x < cx1 - 4 && y > cy0 + 4) {
					++other;
					lowest = y;
				}
			}
		if (all && other * 2 < all && lowest >= 0) {
			// Raised at once; lowered (to the most seen meanwhile) once it
			// has been lower for 3 seconds, as after a popup there closes.
			const int bottom = lowest + 6;
			static DWORD lowerSince = 0;
			static int lowerMax = 0;
			const DWORD now = GetTickCount();
			int next = gGizmoBottom;
			if (bottom >= gGizmoBottom) {
				next = bottom;
				lowerSince = 0;
			} else {
				if (!lowerSince) {
					lowerSince = now;
					lowerMax = bottom;
				}
				lowerMax = std::max(lowerMax, bottom);
				if (now - lowerSince >= 3000) {
					next = lowerMax;
					lowerSince = 0;
				}
			}
			if (next != gGizmoBottom) {
				gGizmoBottom = next;
				if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
					fprintf(f, "%lu view gizmo down to %d (corner %d %d to %d %d)\n", GetTickCount(), gGizmoBottom, cx0, cy0, cx1, cy1);
					fclose(f);
				}
			}
		}
	}
	{
		// The viewport grows to every row and column the background shows
		// in (at least 8 pixels of it, so stray magenta in the interface
		// does not count), from frames where the background spans at least
		// a third of the document each way (a popup or the model can hide
		// most of it).
		int x0 = w, x1 = -1, y0 = h, y1 = -1;
		for (int x = 0; x < w; ++x)
			if (colMagenta[x] >= 8) { x0 = std::min(x0, x); x1 = x; }
		for (int y = 0; y < h; ++y)
			if (rowMagenta[y] >= 8) { y0 = std::min(y0, y); y1 = y; }
		static DWORD statsAt = 0;
		if (GetTickCount() - statsAt >= 2000) {
			statsAt = GetTickCount();
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu capture method %d, %.1f fps, magenta %.1f%%, background spans %d %d to %d %d, viewport %d %d to %d %d, mode %d\n",
					statsAt, gCaptureMethod, gFps, gMagentaShare, x0, y0, x1, y1, gViewX0, gViewY0, gViewX1, gViewY1, gMode);
				fclose(f);
			}
		}
		if (x1 - x0 >= w / 3 && y1 - y0 >= h / 3) {
			const bool had = gViewX1 > gViewX0;
			const int nx0 = had ? std::min(gViewX0, x0) : x0, ny0 = had ? std::min(gViewY0, y0) : y0;
			const int nx1 = had ? std::max(gViewX1, x1 + 1) : x1 + 1, ny1 = had ? std::max(gViewY1, y1 + 1) : y1 + 1;
			if (nx0 != gViewX0 || ny0 != gViewY0 || nx1 != gViewX1 || ny1 != gViewY1) {
				gViewX0 = nx0;
				gViewY0 = ny0;
				gViewX1 = nx1;
				gViewY1 = ny1;
				EnterCriticalSection(&gOverlayLock);
				gRedrawPending = true;
				LeaveCriticalSection(&gOverlayLock);
				if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
					fprintf(f, "%lu viewport %d %d to %d %d of %d x %d\n", GetTickCount(), gViewX0, gViewY0, gViewX1, gViewY1, w, h);
					fclose(f);
				}
			}
		}
	}
	gModelPixels = static_cast<double>(model);
	if (gMode == 1 && !gPanelAside) {
		// PureRef: the panel is shown over the model, but not over ZBrush's
		// popups and menus.
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		const int sx = static_cast<int>(st.screenX), sy = static_cast<int>(st.screenY);
		for (int y = py; y < py + ph && y < h; ++y) {
			const uint32_t* src = reinterpret_cast<const uint32_t*>(gRefLayer.bits) + static_cast<size_t>(y) * w;
			uint32_t* dst = reinterpret_cast<uint32_t*>(gOutput.bits) + static_cast<size_t>(y) * w;
			for (int x = px; x < px + pw && x < w; ++x) {
				bool ui = false;
				for (const RECT& r : gUiRects)
					if (x + sx >= r.left && x + sx < r.right && y + sy >= r.top && y + sy < r.bottom) {
						ui = true;
						break;
					}
				if (!ui)
					dst[x] = src[x] | 0xFF000000u;
			}
		}
	}
	if (gAdjust && gMode == 0)
		DrawGizmo(st, gOutput);
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
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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
	// Frames of a rotation drag in which its position changed at the same
	// scale. Rotating never changes ZBrush's position (nav_log: rotation
	// drags end at the x, y they started at), so such a copy is something
	// else that happens to equal it in the front view.
	int rotFrames;
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
					gTriplets.push_back({sc, x, y, 0, 0, 0, 0});
		}
}

// Reads the live triplet: of the copies that stay put while rotating, the
// one that has changed in the most frames of pan and zoom drags (copies that
// only update now and then never win), the most recently changed one until a
// drag has shown which. Only pans and zooms count: a rotation changes other
// values, which must not win the next pan. Switches and sudden jumps are
// logged. mode: the drag's DragMode, or -1 when the mouse is not dragging.
static bool TripletValues(double& sx, double& px, double& py, int mode)
{
	const bool dragging = mode == kDragMove || mode == kDragScale;
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
			if (mode == kDragNone && a == t.s.last && (b != t.x.last || c != t.y.last) && t.rotFrames++ == 0) {
				if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
					fprintf(f, "%lu triplet %zu moves when rotating: not the camera (x %.3f y %.3f, was %.3f %.3f)\n",
						now, i, b, c, t.x.last, t.y.last);
					fclose(f);
				}
			}
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
		if ((t.rotFrames == 0) != (o.rotFrames == 0) ? t.rotFrames == 0 :
				t.posFrames != o.posFrames ? t.posFrames > o.posFrames :
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
		if (curReadable && !(cur.rotFrames > 0 && cand.rotFrames == 0) &&
				cand.posFrames <= cur.posFrames * 3 / 2 + 10 && !(cur.dragFrames == 0 && cand.dragFrames > 0))
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
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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
	const bool read = !gTriplets.empty() ? TripletValues(sx, px, py, gGesture.active ? gGesture.mode : -1) :
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

// ---------------------------------------------------------------------------
// Views: a reference image per snapped view (slots 0-5, in the Image Plane
// plugin's order Front, Back, Right, Left, Top, Bottom). Slot 6 is the plain
// anchor (Load Image + Anchor), used while no view has an image.
//
// ZBrush rotates the model about a pivot that is not its TransformGet
// position: rotating leaves that position unchanged (nav_log: rotation drags
// start and end at the same x, y) but the model moves on screen. Snapped back
// to a view, the model is shifted while ZBrush's position says it is not, so
// the shift cannot be read, only seen. Each view keeps the model's
// silhouette as it was lined up with the image (its mask, in image space);
// when the view is snapped to again, the current silhouette is matched
// against it and the difference becomes the offset.
// ---------------------------------------------------------------------------
static const int kViewSlots = 7, kDefaultView = 6;

// Each view's rotation as ZBrush keeps it (RotCode of the matrix). Front
// (identity), Left and Right were read from ZBrush (nav_log: Left
// [0 0 -1; 0 1 0; 1 0 0]); Back, Top and Bottom follow from them.
static const int kViewCodes[6] = {16484, 3360, 9202, 10642, 11786, 7898};  // Front Back Right Left Top Bottom

struct View {
	bool used = false;
	double rx = 0, ry = 0, rz = 0, scale = 0, ax = 0, ay = 0;
	double imgSX = 1, imgSY = 1, imgX = 0, imgY = 0;  // Adjust Image
	// The view's rotation matrices as read from ZBrush's memory (it keeps a
	// few copies, not all laid out alike), each packed by RotCode.
	std::vector<int> codes;
	std::string path;
	// Silhouette in image space: 0 not seen, 1 background, 2 model.
	std::vector<unsigned char> mask;
	int gw = 0, gh = 0;
	double cell = 0, u0 = 0, v0 = 0;
};
static View gViews[kViewSlots];
static bool gViewsLoaded = false;
static int gActiveView = -1;
static bool gViewAway = true;
// Pending silhouette work for the overlay thread: 1 record, 2 match.
static int gViewJob = 0, gViewJobSlot = -1;
static DWORD gViewJobAt = 0, gViewSettledAt = 0;
static double gViewLastCount = -1;
static double gViewScore = -1, gViewShiftX = 0, gViewShiftY = 0;
static DWORD gLastFrameAt = 0;  // when the overlay last composited a new capture

static std::wstring ViewsPath()
{
	const std::wstring folder = ProjectFolder();
	return folder + (gProjectDir.empty() ? L"views.txt" : L"project.jref");
}

static std::wstring MaskPath(int i)
{
	wchar_t name[32];
	swprintf(name, 32, L"view%d_outline.bin", i);  // (view%d_mask.bin: before build 83, with the interface in it)
	return ProjectFolder() + name;
}

static void SaveViews()
{
	FILE* f = _wfopen(ViewsPath().c_str(), L"wb");
	if (!f)
		return;
	for (int i = 0; i < kViewSlots; ++i) {
		const View& v = gViews[i];
		std::string codes;
		for (int c : v.codes)
			codes += (codes.empty() ? "" : ",") + std::to_string(c);
		fprintf(f, "%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s|%s\n", v.used ? 1 : 0, v.rx, v.ry, v.rz, v.scale,
			v.ax, v.ay, v.imgSX, v.imgSY, v.imgX, v.imgY, codes.empty() ? "-1" : codes.c_str(), v.path.c_str());
	}
	fclose(f);
}

static void SaveMask(int i)
{
	const View& v = gViews[i];
	if (v.mask.empty()) {
		_wremove(MaskPath(i).c_str());
		return;
	}
	if (FILE* f = _wfopen(MaskPath(i).c_str(), L"wb")) {
		fprintf(f, "%d %d %.6f %.6f %.6f\n", v.gw, v.gh, v.cell, v.u0, v.v0);
		fwrite(v.mask.data(), 1, v.mask.size(), f);
		fclose(f);
	}
}

static void LoadMask(int i)
{
	View& v = gViews[i];
	v.mask.clear();
	FILE* f = _wfopen(MaskPath(i).c_str(), L"rb");
	if (!f)
		return;
	if (fscanf(f, "%d %d %lf %lf %lf", &v.gw, &v.gh, &v.cell, &v.u0, &v.v0) == 5 && fgetc(f) == '\n' &&
			v.gw > 0 && v.gh > 0 && v.gw < 4096 && v.gh < 4096) {
		v.mask.resize(static_cast<size_t>(v.gw) * v.gh);
		if (fread(v.mask.data(), 1, v.mask.size(), f) != v.mask.size())
			v.mask.clear();
	}
	fclose(f);
}

static void LoadViews()
{
	if (gViewsLoaded)
		return;
	gViewsLoaded = true;
	if (FILE* f = _wfopen(ViewsPath().c_str(), L"rb")) {
		char line[2400];
		for (int i = 0; i < kViewSlots && fgets(line, sizeof(line), f); ++i) {
			View& v = gViews[i];
			int used = 0;
			char codes[200] = {0};
			const int got = sscanf(line, "%d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %199[-0-9,]", &used, &v.rx, &v.ry, &v.rz, &v.scale,
				&v.ax, &v.ay, &v.imgSX, &v.imgSY, &v.imgX, &v.imgY, codes);
			if (got < 7)
				continue;
			if (got < 12 || v.imgSX <= 0 || v.imgSY <= 0) {
				v.imgSX = v.imgSY = 1;
				v.imgX = v.imgY = 0;
				codes[0] = 0;
			}
			v.codes.clear();
			for (char* t = strtok(codes, ","); t; t = strtok(nullptr, ","))
				if (atoi(t) >= 0)
					v.codes.push_back(atoi(t));
			v.used = used != 0;
			const char* bar = strchr(line, '|');
			v.path = bar ? bar + 1 : "";
			while (!v.path.empty() && (v.path.back() == '\n' || v.path.back() == '\r'))
				v.path.pop_back();
			LoadMask(i);
		}
		fclose(f);
	} else {
		// First run with views: the plain anchor from anchor.txt and the
		// image from reference_path.txt.
		View& d = gViews[kDefaultView];
		if (FILE* a = _wfopen(AnchorPath().c_str(), L"rb")) {
			d.used = fscanf(a, "%lf %lf %lf %lf %lf %lf", &d.scale, &d.ax, &d.ay, &d.rx, &d.ry, &d.rz) == 6 && d.scale > 0;
			fclose(a);
		}
		if (FILE* c = _wfopen(ConfigPath().c_str(), L"rb")) {
			char buf[2048] = {0};
			buf[fread(buf, 1, sizeof(buf) - 1, c)] = 0;
			fclose(c);
			d.path = buf;
		}
	}
}

// Anchor or Load Image changed the plain anchor (slot 6).
static void ViewsDefaultChanged(const double* anchor, const char* imagePath)
{
	LoadViews();
	View& d = gViews[kDefaultView];
	if (anchor) {
		d.used = anchor[0] > 0;
		d.scale = anchor[0];
		d.ax = anchor[1];
		d.ay = anchor[2];
		d.rx = anchor[3];
		d.ry = anchor[4];
		d.rz = anchor[5];
		d.mask.clear();
		SaveMask(kDefaultView);
	}
	if (imagePath)
		d.path = imagePath;
	SaveViews();
}

static double AngleDiff(double a, double b)
{
	double d = fmod(fabs(a - b), 360.0);
	return d > 180 ? 360 - d : d;
}

static bool SameRotation(const View& v, double rx, double ry, double rz)
{
	return AngleDiff(v.rx, rx) + AngleDiff(v.ry, ry) + AngleDiff(v.rz, rz) < 1;
}

static bool AnyViewImage()
{
	for (int i = 0; i < kDefaultView; ++i)
		if (gViews[i].used)
			return true;
	return false;
}

// The slot whose rotation this is, or -1.
static int MatchView(double rx, double ry, double rz)
{
	LoadViews();
	for (int i = 0; i < kDefaultView; ++i)
		if (gViews[i].used && SameRotation(gViews[i], rx, ry, rz))
			return i;
	return -1;
}

static void WriteReferencePath(const std::string& path)
{
	if (FILE* f = _wfopen(ConfigPath().c_str(), L"wb")) {
		fwrite(path.data(), 1, path.size(), f);
		fclose(f);
	}
}

static void ViewLog(const char* what, int slot)
{
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu view %s slot %d score %.3f shift %.2f %.2f offset %.2f %.2f\n", GetTickCount(), what, slot,
			gViewScore, gViewShiftX, gViewShiftY, gOverlay.offX, gOverlay.offY);
		fclose(f);
	}
}

// Queues recording (1) or matching (2) the silhouette for a slot. Only
// while the overlay is showing (it is what sees the model).
static void QueueViewJob(int job, int slot)
{
	if (!gOverlayWnd || !gOverlay.visible)
		return;
	gViewJob = job;
	gViewJobSlot = slot;
	gViewJobAt = GetTickCount();
	gViewSettledAt = gViewJobAt;
	gViewLastCount = -1;
}

// Makes a slot the shown one: its image and anchor, offset cleared, then
// its silhouette matched (or recorded, the first time). Call with the lock.
static void ActivateView(int slot)
{
	View& v = gViews[slot];
	if (slot != gActiveView || gOverlayPath != v.path) {
		gOverlayPath = v.path;
		WriteReferencePath(v.path);  // for the canvas fill (Overlay Off)
		gRedrawPending = true;
	}
	gActiveView = slot;
	gViewAway = false;
	gAnchorScale = v.scale;
	gOverlay.ax = v.ax;
	gOverlay.ay = v.ay;
	gOverlay.imgSX = v.imgSX;
	gOverlay.imgSY = v.imgSY;
	gOverlay.imgX = v.imgX;
	gOverlay.imgY = v.imgY;
	gOverlay.offX = gOverlay.offY = 0;
	QueueViewJob(v.mask.empty() ? 1 : 2, slot);
	ViewLog(v.mask.empty() ? "activate (record)" : "activate (match)", slot);
}

// Adjust Image finished a drag: the showing view keeps the image's new size
// and place. Caller holds the lock.
static void SaveAdjustToView()
{
	if (gActiveView < 0 || gActiveView >= kViewSlots)
		return;
	View& v = gViews[gActiveView];
	v.imgSX = gOverlay.imgSX;
	v.imgSY = gOverlay.imgSY;
	v.imgX = gOverlay.imgX;
	v.imgY = gOverlay.imgY;
	SaveViews();
	ViewLog("adjusted", gActiveView);
}

// Snapped to a view that has no image: nothing is shown (black) until a view
// with one is snapped to. Caller holds the lock.
static void BlankView()
{
	gActiveView = -2;
	gViewAway = true;
	gViewJob = 0;
	gOverlayPath.clear();
	gOverlay.imgSX = gOverlay.imgSY = 1;
	gOverlay.imgX = gOverlay.imgY = 0;
	gOverlay.offX = gOverlay.offY = 0;
	gRedrawPending = true;
	ViewLog("snapped to a view without an image: cleared", -2);
}

static int ActiveViewSlot()
{
	return gActiveView;
}

static bool ViewLayout(int slot, const OverlayState& st, OverlayState& out, std::string& path)
{
	if (slot < 0 || slot >= kDefaultView || !gViews[slot].used || gViews[slot].scale <= 0)
		return false;
	const View& v = gViews[slot];
	out = st;
	if (gAnchorScale > 0)
		out.ratio = st.ratio * gAnchorScale / v.scale;
	out.ax = v.ax;
	out.ay = v.ay;
	out.imgSX = v.imgSX;
	out.imgSY = v.imgSY;
	out.imgX = v.imgX;
	out.imgY = v.imgY;
	if (slot != gActiveView)
		out.offX = out.offY = 0;
	path = v.path;
	return true;
}

// The model's extent in a view's silhouette (image space): its bounds, and
// per grid row and column the first and last model cell (-1 if none).
struct MaskExtent {
	const unsigned char* data = nullptr;
	size_t size = 0;
	unsigned sum = 0;
	bool ok = false;
	int i0 = 0, i1 = 0, j0 = 0, j1 = 0;
	std::vector<int> rowMin, rowMax, colMin, colMax;
};
static MaskExtent gExtents[kViewSlots];

static const MaskExtent& ExtentFor(int slot)
{
	const View& v = gViews[slot];
	MaskExtent& e = gExtents[slot];
	unsigned sum = 0;
	for (size_t i = 0; i < v.mask.size(); i += 97)
		sum = sum * 31 + v.mask[i];
	if (e.data == v.mask.data() && e.size == v.mask.size() && e.sum == sum)
		return e;
	e = MaskExtent();
	e.data = v.mask.data();
	e.size = v.mask.size();
	e.sum = sum;
	if (v.gw <= 0 || v.gh <= 0 || v.mask.size() != static_cast<size_t>(v.gw) * v.gh || v.cell <= 0)
		return e;
	e.rowMin.assign(v.gh, -1);
	e.rowMax.assign(v.gh, -1);
	e.colMin.assign(v.gw, -1);
	e.colMax.assign(v.gw, -1);
	e.i0 = v.gw;
	e.j0 = v.gh;
	e.i1 = e.j1 = -1;
	for (int j = 0; j < v.gh; ++j)
		for (int i = 0; i < v.gw; ++i) {
			if (v.mask[static_cast<size_t>(j) * v.gw + i] != 2)
				continue;
			if (e.rowMin[j] < 0) e.rowMin[j] = i;
			e.rowMax[j] = i;
			if (e.colMin[i] < 0) e.colMin[i] = j;
			e.colMax[i] = j;
			e.i0 = std::min(e.i0, i);
			e.i1 = std::max(e.i1, i);
			e.j0 = std::min(e.j0, j);
			e.j1 = std::max(e.j1, j);
		}
	e.ok = e.i1 > e.i0 && e.j1 > e.j0;
	return e;
}

// A view's screen axes in the model's frame, from its rotation (kViewCodes):
// d[0] screen right, d[1] screen down, d[2] towards the camera.
static void ViewAxes(int slot, double d[3][3])
{
	int code = kViewCodes[slot];
	double m[9];
	for (int i = 0; i < 9; ++i, code /= 3)
		m[i] = code % 3 - 1;
	for (int c = 0; c < 3; ++c) {
		d[0][c] = m[c];
		d[1][c] = -m[3 + c];
		d[2][c] = m[6 + c];
	}
}

// Multiview: a view other than the one being looked at (C, the showing
// view) shows the same part of the model. Both views' silhouettes (recorded
// when their images were lined up with the model) give the model's extent
// in each image, so an axis the two views share (height, between Front and
// Left) maps across: the same place along the model, zoomed alike. Along the
// axis that is depth for C (across, in Left seen from Front), the part being
// looked at is centred: the model's extent there within the band shown, so
// zoomed into the face, the side view shows the head. Without both
// silhouettes the view is placed by the camera as Classic would show it.
static bool MultiviewLayout(int slot, const OverlayState& st, OverlayState& out, std::string& path)
{
	if (!ViewLayout(slot, st, out, path))
		return false;
	const int c = gActiveView;
	if (c < 0 || c >= kDefaultView || c == slot || !gViews[c].used)
		return true;
	OverlayState cs;
	std::string cpath;
	if (!ViewLayout(c, st, cs, cpath) || cs.ratio <= 0)
		return true;
	const MaskExtent& ce = ExtentFor(c);
	const MaskExtent& ve = ExtentFor(slot);
	if (!ce.ok || !ve.ok)
		return true;
	const View& cv = gViews[c];
	const View& vv = gViews[slot];
	// C's window (the viewport) in its image space: centre and half size per
	// screen axis.
	int vx0, vy0, vx1, vy1;
	const double kp = st.pixelsPerDoc > 0 ? st.pixelsPerDoc : 1;
	ViewportRect(static_cast<int>(st.docW * kp + 0.5), static_cast<int>(st.docH * kp + 0.5), vx0, vy0, vx1, vy1);
	const double midX = (vx0 + vx1) * 0.5 / kp, midY = (vy0 + vy1) * 0.5 / kp;
	const double centreC[2] = {(midX - cs.px) / cs.ratio + cs.ax + cs.offX, (midY - cs.py) / cs.ratio + cs.ay + cs.offY};
	const double halfC[2] = {(vx1 - vx0) * 0.5 / kp / cs.ratio, (vy1 - vy0) * 0.5 / kp / cs.ratio};
	// The model's bounds in image space, per screen axis.
	const double lowC[2] = {cv.u0 + ce.i0 * cv.cell, cv.v0 + ce.j0 * cv.cell};
	const double highC[2] = {cv.u0 + (ce.i1 + 1) * cv.cell, cv.v0 + (ce.j1 + 1) * cv.cell};
	const double lowV[2] = {vv.u0 + ve.i0 * vv.cell, vv.v0 + ve.j0 * vv.cell};
	const double highV[2] = {vv.u0 + (ve.i1 + 1) * vv.cell, vv.v0 + (ve.j1 + 1) * vv.cell};
	double dc[3][3], dv[3][3];
	ViewAxes(c, dc);
	ViewAxes(slot, dv);
	double centreV[2] = {0, 0}, halfV[2] = {0, 0};
	int depthAxis = -1;
	double k = 0;
	for (int a = 0; a < 2; ++a) {
		int b = -1;
		double sign = 0;
		for (int t = 0; t < 3; ++t) {
			const double dot = dv[a][0] * dc[t][0] + dv[a][1] * dc[t][1] + dv[a][2] * dc[t][2];
			if (fabs(dot) > 0.5) { b = t; sign = dot; }
		}
		if (b == 2 || b < 0) {
			depthAxis = a;
			continue;
		}
		double f = (centreC[b] - lowC[b]) / (highC[b] - lowC[b]);
		if (sign < 0)
			f = 1 - f;
		centreV[a] = lowV[a] + f * (highV[a] - lowV[a]);
		const double ka = (highV[a] - lowV[a]) / (highC[b] - lowC[b]);
		if (k <= 0)
			k = ka;
		halfV[a] = halfC[b];
	}
	if (k <= 0)
		return true;
	for (int a = 0; a < 2; ++a)
		halfV[a] *= k;
	if (depthAxis >= 0) {
		// The band shown along the shared axis, and the model's extent
		// across it within the band.
		const int o = 1 - depthAxis;
		const double origin = o == 0 ? vv.u0 : vv.v0;
		const int n = o == 0 ? vv.gw : vv.gh;
		const int from = std::max(0, static_cast<int>(floor((centreV[o] - halfV[o] - origin) / vv.cell)));
		const int to = std::min(n - 1, static_cast<int>(floor((centreV[o] + halfV[o] - origin) / vv.cell)));
		const std::vector<int>& mins = o == 0 ? ve.colMin : ve.rowMin;
		const std::vector<int>& maxs = o == 0 ? ve.colMax : ve.rowMax;
		int lo = -1, hi = -1;
		for (int i = from; i <= to; ++i)
			if (mins[i] >= 0) {
				lo = lo < 0 ? mins[i] : std::min(lo, mins[i]);
				hi = std::max(hi, maxs[i]);
			}
		const double across = depthAxis == 0 ? vv.u0 : vv.v0;
		centreV[depthAxis] = lo >= 0 ? across + (lo + hi + 1) * 0.5 * vv.cell
			: (lowV[depthAxis] + highV[depthAxis]) * 0.5;
	}
	out.ratio = cs.ratio / k;
	out.offX = out.offY = 0;
	out.px = midX - out.ratio * (centreV[0] - out.ax);
	out.py = midY - out.ratio * (centreV[1] - out.ay);
	return true;
}

static std::wstring DisplayPath()
{
	return ProjectFolder() + L"display.txt";
}

static void SaveDisplay()
{
	if (FILE* f = _wfopen(DisplayPath().c_str(), L"wb")) {
		fprintf(f, "%d %d %d %d %d %d %d %d %d %d\n", gMode, gPanelLeft, gMulti, gMultiA, gMultiB, gBackGrey,
			gCells[0], gCells[1], gCells[2], gCells[3]);
		fclose(f);
	}
}

static bool gDisplayLoaded = false;

static void LoadDisplay()
{
	if (gDisplayLoaded)
		return;
	gDisplayLoaded = true;
	if (FILE* f = _wfopen(DisplayPath().c_str(), L"rb")) {
		int v[10] = {0, 0, 0, 0, 3, 48, 0, 3, 4, 1};
		const int n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
		if (n == 10)
			for (int i = 0; i < 4; ++i)
				gCells[i] = v[6 + i] >= -1 && v[6 + i] < 6 ? v[6 + i] : -1;
		if (n >= 6) {
			gMode = v[0] == 1;
			gPanelLeft = v[1] != 0;
			gMulti = v[2] != 0;
			gMultiA = v[3] >= 0 && v[3] < 6 ? v[3] : 0;
			gMultiB = v[4] >= 0 && v[4] < 6 ? v[4] : 3;
			gBackGrey = v[5] >= 0 && v[5] < 256 ? v[5] : 48;
		}
		fclose(f);
	}
}

// The silhouette as currently on the canvas, in image space (doc x =
// px + ratio * (u - ax - offX), as BuildRefLayer places the image), sampled
// on the grid of g (whose size is set here if empty).
static void SampleSilhouette(const OverlayState& st, double offX, double offY, View& g, std::vector<unsigned char>& out)
{
	if (g.gw == 0) {
		g.cell = st.docW / 640.0;
		g.gw = 1280;
		g.gh = static_cast<int>(st.docH * 2 / g.cell + 0.5);
		g.u0 = -st.docW * 0.5;
		g.v0 = -st.docH * 0.5;
	}
	out.assign(static_cast<size_t>(g.gw) * g.gh, 0);
	const double k = st.pixelsPerDoc;
	// Only the viewport: ZBrush's interface over the document's edges is not
	// the model.
	int vx0, vy0, vx1, vy1;
	ViewportRect(gKeyW, gKeyH, vx0, vy0, vx1, vy1);
	// Nor is the view gizmo in the top right corner.
	const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3);
	for (int j = 0; j < g.gh; ++j) {
		const double v = g.v0 + (j + 0.5) * g.cell;
		const int yp = static_cast<int>(floor((st.py + st.ratio * (v - st.ay - offY)) * k));
		if (yp < vy0 || yp >= vy1)
			continue;
		for (int i = 0; i < g.gw; ++i) {
			const double u = g.u0 + (i + 0.5) * g.cell;
			const int xp = static_cast<int>(floor((st.px + st.ratio * (u - st.ax - offX)) * k));
			if (xp < vx0 || xp >= vx1 || (xp >= gizmoX && yp < gGizmoBottom))
				continue;
			out[static_cast<size_t>(j) * g.gw + i] = gKeyShare[static_cast<size_t>(yp) * gKeyW + xp] < 128 ? 2 : 1;
		}
	}
}

// How well cur, shifted by (ox, oy) cells, matches saved: model cells in
// both over model cells in either, where both were seen. -1 if too little.
static double MaskScore(const View& g, const std::vector<unsigned char>& cur, int ox, int oy, int step)
{
	int both = 0, either = 0;
	for (int j = 0; j < g.gh; j += step) {
		const int cj = j - oy;
		if (cj < 0 || cj >= g.gh)
			continue;
		const unsigned char* a = &g.mask[static_cast<size_t>(j) * g.gw];
		const unsigned char* b = &cur[static_cast<size_t>(cj) * g.gw];
		for (int i = 0; i < g.gw; i += step) {
			const int ci = i - ox;
			if (ci < 0 || ci >= g.gw)
				continue;
			const int x = a[i], y = b[ci];
			if (!x || !y)
				continue;
			both += x == 2 && y == 2;
			either += x == 2 || y == 2;
		}
	}
	const int minEither = 40 / (step * step) + 4;
	return either >= minEither ? static_cast<double>(both) / either : -1;
}

static void RunViewJob(const OverlayState& st)
{
	const int slot = gViewJobSlot, job = gViewJob;
	if (slot < 0 || slot >= kViewSlots || gKeyShare.empty())
		return;
	View& v = gViews[slot];
	if (job == 1) {
		v.gw = 0;
		SampleSilhouette(st, st.offX, st.offY, v, v.mask);
		size_t model = 0;
		for (unsigned char m : v.mask)
			model += m == 2;
		if (model < 50)
			v.mask.clear();
		SaveMask(slot);
		gViewScore = static_cast<double>(model);
		gViewShiftX = gViewShiftY = 0;
		EnterCriticalSection(&gOverlayLock);
		ViewLog(v.mask.empty() ? "record failed (model not seen)" : "recorded", slot);
		LeaveCriticalSection(&gOverlayLock);
		return;
	}
	if (v.mask.empty())
		return;
	std::vector<unsigned char> cur;
	SampleSilhouette(st, 0, 0, v, cur);
	// Coarse to fine: every 8th cell over half the document each way, then
	// narrower and denser around the best.
	int bx = 0, by = 0;
	double best = -1;
	const int rx = v.gw / 4, ry = v.gh / 4;
	for (int oy = -ry; oy <= ry; oy += 8)
		for (int ox = -rx; ox <= rx; ox += 8) {
			const double s = MaskScore(v, cur, ox, oy, 8);
			if (s > best) { best = s; bx = ox; by = oy; }
		}
	static const int kPasses[3][2] = {{8, 4}, {3, 2}, {1, 1}};  // reach, sampling
	for (int pass = 0; pass < 3 && best >= 0; ++pass) {
		const int r = kPasses[pass][0], step = kPasses[pass][1];
		int nx = bx, ny = by;
		double nb = -1;
		for (int oy = by - r; oy <= by + r; ++oy)
			for (int ox = bx - r; ox <= bx + r; ++ox) {
				const double s = MaskScore(v, cur, ox, oy, step);
				if (s > nb) { nb = s; nx = ox; ny = oy; }
			}
		bx = nx;
		by = ny;
		best = nb;
	}
	double fx = bx, fy = by;
	if (best > 0) {
		// Sub-cell: a parabola through the scores either side.
		const double l = MaskScore(v, cur, bx - 1, by, 1), r = MaskScore(v, cur, bx + 1, by, 1);
		const double u = MaskScore(v, cur, bx, by - 1, 1), d = MaskScore(v, cur, bx, by + 1, 1);
		if (l >= 0 && r >= 0 && l + r - 2 * best < 0)
			fx += 0.5 * (l - r) / (l + r - 2 * best);
		if (u >= 0 && d >= 0 && u + d - 2 * best < 0)
			fy += 0.5 * (u - d) / (u + d - 2 * best);
	}
	gViewScore = best;
	gViewShiftX = fx * v.cell;
	gViewShiftY = fy * v.cell;
	EnterCriticalSection(&gOverlayLock);
	if (abs(bx) >= rx - 2 || abs(by) >= ry - 2) {
		// At the end of the search: no real match.
		ViewLog("match at the edge of the search, not moved", slot);
	} else if (best >= 0.6) {
		gOverlay.offX = gViewShiftX;
		gOverlay.offY = gViewShiftY;
		ViewLog("matched", slot);
	} else {
		ViewLog("match too weak, not moved", slot);
	}
	LeaveCriticalSection(&gOverlayLock);
}

// Runs the queued job and marks it done (a button may be waiting on it),
// unless another was queued meanwhile.
static void FinishViewJob(const OverlayState& st)
{
	const int job = gViewJob, slot = gViewJobSlot;
	const DWORD at = gViewJobAt;
	RunViewJob(st);
	EnterCriticalSection(&gOverlayLock);
	if (gViewJob == job && gViewJobSlot == slot && gViewJobAt == at)
		gViewJob = 0;
	LeaveCriticalSection(&gOverlayLock);
}

// Each overlay step: runs a queued job. Recording uses the first capture
// taken after it was asked for (the button that asked waits, so nothing it
// shows afterwards gets in). Matching waits until the view has settled (no
// drag, the model's pixel count steady for 300 ms, after the snap
// animation).
static void ViewJobStep(const OverlayState& st)
{
	if (!gViewJob)
		return;
	const DWORD now = GetTickCount();
	if (gViewJob == 1) {
		const bool fresh = static_cast<int>(gLastFrameAt - gViewJobAt) > 100 || now - gLastFrameAt > 250;
		if ((now - gViewJobAt >= 150 && fresh) || now - gViewJobAt > 1500)
			FinishViewJob(st);
		return;
	}
	if (gGesture.active || fabs(gModelPixels - gViewLastCount) > 0.002 * (gModelPixels + 100)) {
		gViewLastCount = gModelPixels;
		gViewSettledAt = now;
		if (now - gViewJobAt < 5000)
			return;
	}
	if (now - gViewSettledAt < 300 || now - gViewJobAt < 400)
		return;
	FinishViewJob(st);
}

// Waits (up to 2 s) for a queued recording to be done.
static void WaitViewRecord()
{
	for (int i = 0; i < 200 && gViewJob == 1; ++i)
		Sleep(10);
}


// ---------------------------------------------------------------------------
// ZBrush's rotation, read from memory like the zoom and pan, so the DLL
// sees snaps to a view itself (ZScript's Live loop does not keep running).
// The angles TransformGet reports are not stored as such (build 70's search
// for them found nothing that followed), so this looks for the rotation
// matrix instead, which needs no knowledge of ZBrush's angle convention: at
// the Overlay press's first rotated test view, every 3 x 3 block of floats
// or doubles (rows 3 or 4 values apart) that is a rotation (orthonormal) and
// not axis-aligned; at the next test views, those still rotations that
// changed. A snapped view's matrix has only -1, 0 and 1 in it, which also
// identifies the view (RotCode).
// ---------------------------------------------------------------------------
struct RotCand {
	uintptr_t addr;
	bool isDouble;
	int stride;  // values from one row to the next
	double m[9];
	DWORD changedAt;
	double settled[9];  // m when the view was last looked up
	bool unit;          // stored as a pure rotation (not scaled by the zoom)
	double m1[9], m2[9];  // at the first and second test views
	// How it relates to the reference copy (see RotRelate): -1 not known,
	// else bit 0 transposed, bits 1-3 and 4-6 the signs of the diagonal
	// matrices on its left and right.
	int rel;
};
static std::vector<RotCand> gRotCands;
static int gRotNarrowings = 0;
static bool gRotLocked = false;
static DWORD gRotChangedAt = 0;
static bool gRotSettled = true;

static bool IsRotation(const double* m)
{
	for (int i = 0; i < 3; ++i)
		for (int j = i; j < 3; ++j) {
			const double d = m[i * 3] * m[j * 3] + m[i * 3 + 1] * m[j * 3 + 1] + m[i * 3 + 2] * m[j * 3 + 2];
			if (fabs(d - (i == j ? 1 : 0)) > 2e-3)
				return false;
		}
	return true;
}

// The view's code: each entry rounded to -1, 0 or 1, packed base 3, or -1
// if the matrix is not axis-aligned (not a snapped view).
static int RotCode(const double* m)
{
	int code = 0;
	for (int i = 8; i >= 0; --i) {
		const double r = floor(m[i] + 0.5);
		if (fabs(m[i] - r) > 0.01 || r < -1 || r > 1)
			return -1;
		code = code * 3 + static_cast<int>(r) + 1;
	}
	return code;
}

static void RotFromBytes(const unsigned char* b, bool isDouble, int stride, double* m)
{
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			const size_t at = static_cast<size_t>(r * stride + c);
			if (isDouble) {
				memcpy(&m[r * 3 + c], b + at * 8, 8);
			} else {
				float f;
				memcpy(&f, b + at * 4, 4);
				m[r * 3 + c] = f;
			}
		}
}

// Divides out a uniform scale: ZBrush's drawing matrix is the rotation times
// the zoom (build 76's search only took unit rows and found a copy that is
// not updated when the view goes back to front). False unless the three
// rows have the same length.
static bool Unscale(double* m)
{
	double n[3];
	for (int r = 0; r < 3; ++r) {
		n[r] = sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
		if (!(n[r] > 1e-6 && n[r] < 1e9))
			return false;
	}
	if (fabs(n[1] / n[0] - 1) > 2e-3 || fabs(n[2] / n[0] - 1) > 2e-3)
		return false;
	for (int i = 0; i < 9; ++i)
		m[i] /= n[i / 3];
	return true;
}

static bool RotRead(const RotCand& c, double* m)
{
	unsigned char b[11 * 8];
	const size_t n = static_cast<size_t>(2 * c.stride + 3) * (c.isDouble ? 8 : 4);
	SIZE_T got = 0;
	if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.addr), b, n, &got) || got != n)
		return false;
	RotFromBytes(b, c.isDouble, c.stride, m);
	for (int i = 0; i < 9; ++i)
		if (m[i] != m[i])
			return false;
	return Unscale(m) && IsRotation(m);
}

// The traces (sum of the diagonal) a rotation by these Euler angles can have,
// for every order of applying them and every sign convention: the trace
// depends only on the angle turned, so it needs no knowledge of ZBrush's
// convention, and lets the scan keep only blocks that are this rotation
// (build 74's scan filled up with other rotations before reaching it).
static std::vector<double> EulerTraces(const double* deg)
{
	std::vector<double> traces;
	const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
	for (const auto& o : orders)
		for (int signs = 0; signs < 8; ++signs) {
			double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
			for (int k = 0; k < 3; ++k) {
				const int axis = o[k];
				const double a = deg[axis] * 3.14159265358979323846 / 180 * ((signs >> axis) & 1 ? -1 : 1);
				const double c = cos(a), sn = sin(a);
				double r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
				const int i = (axis + 1) % 3, j = (axis + 2) % 3;
				r[i * 3 + i] = c;
				r[i * 3 + j] = -sn;
				r[j * 3 + i] = sn;
				r[j * 3 + j] = c;
				double t[9];
				for (int y = 0; y < 3; ++y)
					for (int x = 0; x < 3; ++x)
						t[y * 3 + x] = r[y * 3] * m[x] + r[y * 3 + 1] * m[3 + x] + r[y * 3 + 2] * m[6 + x];
				memcpy(m, t, sizeof(m));
			}
			traces.push_back(m[0] + m[4] + m[8]);
		}
	return traces;
}

static void RotScan(const double* deg)
{
	const std::vector<double> traces = EulerTraces(deg);
	gRotCands.clear();
	gRotNarrowings = 0;
	const DWORD t0 = GetTickCount();
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&RotScan), &self);
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uintptr_t stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit) & ~static_cast<uintptr_t>(0xfffff);
	const uintptr_t stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
	std::vector<unsigned char> buf(1 << 22);
	const uintptr_t bufLo = reinterpret_cast<uintptr_t>(buf.data()), bufHi = bufLo + buf.size();
	const size_t overlap = 11 * 8;
	size_t scanned = 0, rows = 0;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
		const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		const uintptr_t next = base + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xff;
		const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
		if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD) &&
			(mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) && mbi.AllocationBase != self &&
			!(stackLow < next && base < stackHigh) && !OwnThreadStack(base, next)) {
			for (uintptr_t chunk = base; chunk < next; chunk += buf.size() - overlap) {
				const size_t len = next - chunk < buf.size() ? next - chunk : buf.size();
				SIZE_T got = 0;
				if (chunk < bufHi && bufLo < chunk + len)
					continue;
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(chunk), buf.data(), len, &got))
					continue;
				scanned += got;
				for (size_t i = 0; i + 11 * 4 <= got; i += 4) {
					for (int isDouble = 0; isDouble < 2; ++isDouble) {
						const size_t sz = isDouble ? 8 : 4;
						if (isDouble && (i % 8 != 0 || i + 11 * 8 > got))
							continue;
						// Quick test first: the first row has a sensible length.
						double r0[3];
						for (int c = 0; c < 3; ++c) {
							if (isDouble) {
								memcpy(&r0[c], &buf[i + c * sz], 8);
							} else {
								float f;
								memcpy(&f, &buf[i + c * sz], 4);
								r0[c] = f;
							}
						}
						const double n0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
						if (!(n0 > 1e-12 && n0 < 1e18))
							continue;
						++rows;
						for (int stride = 3; stride <= 4; ++stride) {
							if (i + static_cast<size_t>(2 * stride + 3) * sz > got)
								continue;
							double m[9];
							RotFromBytes(&buf[i], isDouble != 0, stride, m);
							if (!Unscale(m) || !IsRotation(m) || RotCode(m) >= 0 || gRotCands.size() >= 200000)
								continue;
							// With any axis flipped (a copy may be in screen
							// coordinates), its diagonal entries change sign.
							bool angle = false;
							for (int flips = 0; flips < 8 && !angle; ++flips) {
								const double tr = (flips & 1 ? -m[0] : m[0]) + (flips & 2 ? -m[4] : m[4]) + (flips & 4 ? -m[8] : m[8]);
								for (double t : traces)
									if (fabs(tr - t) < 3e-3)
										angle = true;
							}
							if (!angle)
								continue;
							double raw[9];
							RotFromBytes(&buf[i], isDouble != 0, stride, raw);
							RotCand c = {chunk + i, isDouble != 0, stride, {}, 0, {}, fabs(sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]) - 1) < 2e-3, {}, {}, -1};
							memcpy(c.m, m, sizeof(m));
							memcpy(c.m1, m, sizeof(m));
							gRotCands.push_back(c);
						}
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
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu rotation matrix scan: %zu MB in %lu ms, %zu rows tried, %zu rotations by this angle found%s\n", GetTickCount(),
			scanned >> 20, GetTickCount() - t0, rows, gRotCands.size(), gRotCands.size() >= 200000 ? " (FULL)" : "");
		fclose(f);
	}
}

// Keeps the candidates that are still rotations and changed with the view.
static void RotNarrow()
{
	std::vector<RotCand> keep;
	for (const RotCand& c : gRotCands) {
		double m[9];
		if (!RotRead(c, m))
			continue;
		double change = 0;
		for (int i = 0; i < 9; ++i)
			change = std::max(change, fabs(m[i] - c.m[i]));
		if (change < 0.02)
			continue;
		keep.push_back(c);
		memcpy(keep.back().m, m, sizeof(m));
		if (gRotNarrowings == 0)
			memcpy(keep.back().m2, m, sizeof(m));
	}
	gRotCands.swap(keep);
	++gRotNarrowings;
}

// Applies a relation: D1 * (transposed?) m * D2, the signs from rel.
static void RotApply(int rel, const double* m, double* out)
{
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			const double v = rel & 1 ? m[c * 3 + r] : m[r * 3 + c];
			const double s1 = (rel >> (1 + r)) & 1 ? -1 : 1, s2 = (rel >> (4 + c)) & 1 ? -1 : 1;
			out[r * 3 + c] = s1 * v * s2;
		}
}

// Every copy is put in the reference copy's layout before its view is
// looked up: the views' codes (kViewCodes) were read from a pure rotation
// copy, and ZBrush's other copies (such as its drawing matrix) may be
// transposed or have axes flipped. The relation is the one that turns the
// copy into the reference at both test views. Copies with none are not used.
static void RotRelate()
{
	int ref = -1;
	for (size_t i = 0; i < gRotCands.size(); ++i)
		if (gRotCands[i].unit && ref < 0)
			ref = static_cast<int>(i);
	if (ref < 0 && !gRotCands.empty())
		ref = 0;
	for (RotCand& c : gRotCands) {
		c.rel = -1;
		if (ref < 0)
			continue;
		const RotCand& r = gRotCands[ref];
		for (int rel = 0; rel < 128 && c.rel < 0; ++rel) {
			double a[9], b[9];
			RotApply(rel, c.m1, a);
			RotApply(rel, c.m2, b);
			bool same = true;
			for (int i = 0; i < 9; ++i)
				if (fabs(a[i] - r.m1[i]) > 3e-3 || fabs(b[i] - r.m2[i]) > 3e-3)
					same = false;
			if (same)
				c.rel = rel;
		}
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu rotation copies:", GetTickCount());
		for (const RotCand& c : gRotCands)
			fprintf(f, " [%s %s stride %d relation %d]", c.unit ? "pure" : "scaled", c.isDouble ? "double" : "float", c.stride, c.rel);
		fprintf(f, "\n");
		fclose(f);
	}
}

// Reads every candidate. Returns the codes of the ones that are snapped
// (axis-aligned), without repeats, and whether any changed since the last
// read (more than rounding). With sinceSettled, only the copies that changed
// since the view was last looked up count: a copy ZBrush stopped updating
// would otherwise keep reporting its old view.
static std::vector<int> RotCodes(bool* changed = nullptr, bool sinceSettled = false)
{
	std::vector<int> codes;
	bool any = false;
	// The camera turns only while a mouse button is held or in the snap
	// animation just after: a copy that changes with the buttons up and idle
	// follows something else (the brush under the cursor) and is dropped.
	const bool idle = gButtonsDown == 0 && !gGesture.active && GetTickCount() - gLastButtonAt > 2000;
	for (size_t ci = 0; ci < gRotCands.size(); ++ci) {
		RotCand& c = gRotCands[ci];
		double m[9];
		if (!RotRead(c, m))
			continue;
		bool moved = false, step = false;
		for (int i = 0; i < 9; ++i) {
			// Changes this small are ZBrush easing the end of a snap: the
			// view counts as still.
			if (fabs(m[i] - c.m[i]) > 5e-4)
				step = true;
			if (fabs(m[i] - c.settled[i]) > 1e-4)
				moved = true;
		}
		if (step && idle) {
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu rotation copy %zu changed with the mouse buttons up: not the camera, dropped (%zu left)\n",
					GetTickCount(), ci, gRotCands.size() - 1);
				fclose(f);
			}
			gRotCands.erase(gRotCands.begin() + ci);
			--ci;
			continue;
		}
		any = any || step;
		memcpy(c.m, m, sizeof(m));
		if (sinceSettled && !moved)
			continue;
		if (c.rel < 0)
			continue;
		double canon[9];
		RotApply(c.rel, m, canon);
		const int code = RotCode(canon);
		if (code >= 0 && std::find(codes.begin(), codes.end(), code) == codes.end())
			codes.push_back(code);
	}
	if (changed)
		*changed = any;
	return codes;
}

static std::vector<int> CurrentRotCodes()
{
	return gRotLocked ? RotCodes() : std::vector<int>();
}

static int ViewTickCodes(const std::vector<int>& codes);

// Each overlay frame: when the rotation has changed and then held still for
// 200 ms (a snap's animation has finished), the view is looked up. Every
// candidate's matrix is logged then, and every 250 ms while it moves, to
// show what ZBrush does.
static void RotFollow()
{
	if (!gRotLocked)
		return;
	EnterCriticalSection(&gOverlayLock);
	bool changed = false;
	const std::vector<int> codes = RotCodes(&changed, true);
	const DWORD now = GetTickCount();
	static DWORD loggedAt = 0;
	if (changed && now - loggedAt >= 250) {
		loggedAt = now;
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu rotation moving, dragging %d:", now, gGesture.active ? 1 : 0);
			for (const RotCand& c : gRotCands) {
				fprintf(f, " [%d", RotCode(c.m));
				for (int i = 0; i < 9; ++i)
					fprintf(f, " %.4f", c.m[i]);
				fprintf(f, "]");
			}
			fprintf(f, "\n");
			fclose(f);
		}
	}
	static std::vector<int> lastCodes;
	// After settling, an easing that creeps on (too slowly to count as
	// moving) can still land on a view: that is looked up too.
	const bool landed = gRotSettled && !gGesture.active && !codes.empty() && codes != lastCodes;
	if (changed) {
		gRotChangedAt = now;
		gRotSettled = false;
	} else if (landed || (!gRotSettled && !gGesture.active && now - gRotChangedAt >= 200)) {
		gRotSettled = true;
		lastCodes = codes;
		const int result = ViewTickCodes(codes);
		for (RotCand& c : gRotCands)
			memcpy(c.settled, c.m, sizeof(c.m));
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu rotation settled, %zu snapped copies, result %d:", now, codes.size(), result);
			for (const RotCand& c : gRotCands) {
				fprintf(f, " [%d", RotCode(c.m));
				for (int i = 0; i < 9; ++i)
					fprintf(f, " %.4f", c.m[i]);
				fprintf(f, "]");
			}
			fprintf(f, "\n");
			fclose(f);
		}
	}
	LeaveCriticalSection(&gOverlayLock);
}

static bool AtView(const std::vector<int>& codes, int slot)
{
	return slot >= 0 && slot < 6 && std::find(codes.begin(), codes.end(), kViewCodes[slot]) != codes.end();
}

static void AddCodes(View& v, const std::vector<int>& codes)
{
	for (int c : codes)
		if (std::find(v.codes.begin(), v.codes.end(), c) == v.codes.end())
			v.codes.push_back(c);
}

// The view's rotation, from Live or the rotation read from memory. When the
// view is snapped to a slot's rotation after being away from it (or to
// another slot), that slot is shown and the model's silhouette matched.
// Snapped to a view without an image (while some view has one), the
// reference is cleared. Returns 10 + slot when it activated one, the slot if
// it was already showing, -2 when it cleared, or -1. Caller holds the lock.
static int ViewTickLocked(double rx, double ry, double rz)
{
	const int slot = MatchView(rx, ry, rz);
	int result = slot;
	if (slot < 0) {
		gViewAway = true;
		// Turning freely leaves the reference as it is.
		bool snapped = true;
		for (double a : {rx, ry, rz})
			if (AngleDiff(a, 90 * floor(a / 90 + 0.5)) > 0.5)
				snapped = false;
		if (snapped && AnyViewImage() && gActiveView != -2) {
			BlankView();
			result = -2;
		}
	} else {
		// A view loaded before its matrix codes were known learns them here.
		const std::vector<int> codes = CurrentRotCodes();
		const size_t before = gViews[slot].codes.size();
		AddCodes(gViews[slot], codes);
		if (gViews[slot].codes.size() != before)
			SaveViews();
		if (slot != gActiveView || gViewAway) {
			ActivateView(slot);
			result = 10 + slot;
		}
	}
	if ((result >= 10 || result == -2) && gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return result;
}

// The same from the rotation matrices read from memory: codes are the
// snapped copies' RotCodes (none when turned freely). The view is the slot
// sharing the most codes with them. Caller holds the lock.
static int ViewTickCodes(const std::vector<int>& codes)
{
	LoadViews();
	if (codes.empty()) {
		gViewAway = true;  // turning freely leaves the reference as it is
		return -1;
	}
	int slot = -1;
	for (int i = 0; i < kDefaultView; ++i)
		if (gViews[i].used && AtView(codes, i))
			slot = i;
	int result = slot;
	if (slot < 0) {
		gViewAway = true;
		if (gActiveView != -2) {
			BlankView();
			result = -2;
		}
	} else if (slot != gActiveView || gViewAway) {
		ActivateView(slot);
		result = 10 + slot;
	}
	if ((result >= 10 || result == -2) && gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return result;
}

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
	RotFollow();
	RefreshUiRects();
	if (gAdjustEscape) {
		EnterCriticalSection(&gOverlayLock);
		gAdjustEscape = false;
		if (gAdjust) {
			SaveAdjustToView();
			gAdjust = false;
			gAdjustDrag = 0;
			gRedrawPending = true;
		}
		LeaveCriticalSection(&gOverlayLock);
	}
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
		st.ax != shown.ax || st.ay != shown.ay || st.offX != shown.offX || st.offY != shown.offY ||
		st.imgSX != shown.imgSX || st.imgSY != shown.imgSY || st.imgX != shown.imgX || st.imgY != shown.imgY || st.alpha != shown.alpha || st.screenX != shown.screenX ||
		st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc;
	if (layoutChanged) {
		if (st.screenX != shown.screenX || st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc) {
			// The overlay moved (recalibrated): it may be on another monitor.
			gDup.Release();
			gDup.failed = false;
			gViewX0 = gViewY0 = gViewX1 = gViewY1 = 0;
			gGizmoBottom = 0;
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
	const bool aside = UpdatePanelAside(st);
	if (Capture(st) || layoutChanged || aside) {
		Composite(wnd, st);
		gLastFrameAt = GetTickCount();
		++fpsFrames;
		const DWORD now = GetTickCount();
		if (now - fpsStart >= 1000) {
			gFps = fpsFrames * 1000.0 / (now - fpsStart);
			fpsStart = now;
			fpsFrames = 0;
		}
	}
	ViewJobStep(st);
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
		wc.lpszClassName, L"JRef", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, nullptr);
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
	if (!LoadRgb(DataFolder() + L"frame0.png", frame))
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
	SaveRgb(DataFolder() + L"capture.png", screen);
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
	// Only a view's image is shown, once its view is snapped to.
	LoadViews();
	LoadDisplay();
	BlankView();
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
	case 34: return static_cast<float>(gViewScore);
	case 35: return static_cast<float>(gViewShiftX);
	case 36: return static_cast<float>(gViewShiftY);
	case 37: return static_cast<float>(gActiveView);
	case 38: return gAdjust ? 1 : 0;
	case 39: return gRotLocked ? static_cast<float>(gRotCands.size()) : 0;
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
	// The DLL switches views on its own, so the script's anchor sliders can
	// be another view's: the showing view's anchor is the one that counts.
	if (gActiveView >= 0 && gActiveView < kViewSlots && gViews[gActiveView].used && gViews[gActiveView].scale > 0) {
		if (sx > 0)
			r = sx / gViews[gActiveView].scale;
		ax = gViews[gActiveView].ax;
		ay = gViews[gActiveView].ay;
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
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

// text: "start|next|last rx ry rz", the rotation TransformGet reports at
// each of the Overlay press's test views. "start" searches memory for it,
// the others keep the locations that changed to match. Returns how many
// locations are left; after "last", 1 or more means the rotation is read.
ZFUNC RSRotProbe(char* text, double, char*, int, char*, int, char**)
{
	char mode[16] = {0};
	double r[3];
	if (!text || sscanf(text, "%15s %lf %lf %lf", mode, &r[0], &r[1], &r[2]) != 4)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	if (strcmp(mode, "start") == 0) {
		gRotLocked = false;
		RotScan(r);
	} else {
		RotNarrow();
	}
	if (strcmp(mode, "last") == 0) {
		gRotLocked = !gRotCands.empty() && gRotNarrowings >= 2;
		RotRelate();
		// The Overlay press looks the current view up itself once its
		// popups are closed (they would get into the outline match).
		RotCodes();
		for (RotCand& c : gRotCands)
			memcpy(c.settled, c.m, sizeof(c.m));
		gRotSettled = true;
	}
	const size_t n = gRotCands.size();
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu rotation %s %.3f %.3f %.3f: %zu left, locked %d\n", GetTickCount(), mode, r[0], r[1], r[2], n, gRotLocked);
		fclose(f);
	}
	LeaveCriticalSection(&gOverlayLock);
	return static_cast<float>(n);
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
	SaveRgb(DataFolder() + L"snap_capture.png", cap);
	SaveRgb(DataFolder() + L"snap_overlay.png", out);
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

// text: "name value" sets a display setting and returns it: "mode" (0
// Classic, 1 PureRef), "left" (PureRef panel on the left), "multi" (two
// views in the panel), "a" and "b" (the two views, slots 0-5), "back" (the
// grey shown around the panel), "gamma" and "sat" (the reference's gamma
// and saturation in percent). The value "-1" toggles mode, left and multi
// and steps a and b to the next view. "name" alone returns the setting.
ZFUNC RSDisplay(char* text, double, char*, int, char*, int, char**)
{
	char name[16] = {0};
	int value = -2;
	if (!text || sscanf(text, "%15s %d", name, &value) < 1)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadDisplay();
	if (strcmp(name, "cell") == 0) {
		// "cell i" returns cell i's view (-1 none); "cell i -1" steps it to
		// the next view (Front ... Bottom, then none).
		int cell = -1, step = 0;
		const int n = sscanf(text, "%15s %d %d", name, &cell, &step);
		float result = -2;
		if (n >= 2 && cell >= 0 && cell < 4) {
			if (n == 3 && step == -1) {
				gCells[cell] = (gCells[cell] + 2) % 7 - 1;
				SaveDisplay();
				gRedrawPending = true;
			}
			result = static_cast<float>(gCells[cell]);
		}
		LeaveCriticalSection(&gOverlayLock);
		if (gOverlayWnd)
			PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
		return result;
	}
	int* field = strcmp(name, "mode") == 0 ? &gMode : strcmp(name, "left") == 0 ? &gPanelLeft :
		strcmp(name, "multi") == 0 ? &gMulti : strcmp(name, "a") == 0 ? &gMultiA : strcmp(name, "b") == 0 ? &gMultiB :
		strcmp(name, "back") == 0 ? &gBackGrey : strcmp(name, "gamma") == 0 ? &gGamma : strcmp(name, "sat") == 0 ? &gSat : nullptr;
	float result = -1;
	if (field) {
		const bool view = field == &gMultiA || field == &gMultiB;
		const bool level = field == &gBackGrey || field == &gGamma || field == &gSat;
		if (value == -1 && !level)
			*field = view ? (*field + 1) % 6 : !*field;
		else if (value >= 0)
			*field = view ? value % 6 : field == &gBackGrey ? std::min(value, 255) :
				field == &gGamma ? std::max(20, std::min(value, 300)) : field == &gSat ? std::min(value, 200) : value != 0;
		if (value != -2) {
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu display %s\n", GetTickCount(), text);
				fclose(f);
			}
			SaveDisplay();
			gRedrawPending = true;
			if (gMode == 1)
				gAdjust = false;  // Adjust Image works on the Classic view
		}
		result = static_cast<float>(*field);
	}
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return result;
}

// text: "1" turns Adjust Image on, "0" off (keeping the image's size and
// place for the showing view), "2" only checks whether it could go on.
// Returns 1, 0 if no image is showing, -1 if the overlay is not on, -2 in
// PureRef.
ZFUNC RSAdjust(char* text, double, char*, int, char*, int, char**)
{
	const int req = text ? atoi(text) : 0;
	const bool on = req == 1 || req == 2;
	if (!gOverlayWnd || !gOverlay.visible) {
		gAdjust = false;
		return -1;
	}
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	float result = 1;
	if (on && gMode == 1) {
		result = -2;
	} else if (on && (gActiveView < 0 || gImgW <= 0)) {
		result = 0;
	} else if (req == 2) {
		result = 1;
	} else {
		if (!on && gAdjust)
			SaveAdjustToView();
		gAdjust = on;
		gAdjustDrag = 0;
		gRedrawPending = true;
	}
	LeaveCriticalSection(&gOverlayLock);
	PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return result;
}

// text: "rx ry rz", the view's rotation, from Live each tick. When the view
// is snapped to a slot's rotation after being away from it (or to another
// slot), that slot is shown and the model's silhouette matched. Returns
// 10 + slot when it activated one, the slot if it was already showing, or
// -1 when the rotation is no slot's.
ZFUNC RSViewTick(char* text, double, char*, int, char*, int, char**)
{
	double rx, ry, rz;
	if (!text || sscanf(text, "%lf %lf %lf", &rx, &ry, &rz) != 3)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	const int result = ViewTickLocked(rx, ry, rz);
	LeaveCriticalSection(&gOverlayLock);
	return static_cast<float>(result);
}

// text: "peek" or "go". The view snapped to now, from the rotation read
// from memory: "peek" returns its slot if it has an image (else -1), "go"
// shows it as RSViewTick does. -3 if the rotation was not found.
ZFUNC RSViewNow(char* text, double, char*, int, char*, int, char**)
{
	if (!gRotLocked)
		return -3;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadViews();
	const std::vector<int> codes = RotCodes();
	int result = -1;
	if (text && strcmp(text, "go") == 0) {
		gViewAway = true;
		result = ViewTickCodes(codes);
		for (RotCand& c : gRotCands)
			memcpy(c.settled, c.m, sizeof(c.m));
	} else {
		for (int i = 0; i < kDefaultView; ++i)
			if (gViews[i].used && AtView(codes, i))
				result = i;
	}
	LeaveCriticalSection(&gOverlayLock);
	return static_cast<float>(result);
}

// text: "rx ry rz". 1 if every angle is a multiple of 90 degrees (a
// snapped view), else 0.
ZFUNC RSViewSnapped(char* text, double, char*, int, char*, int, char**)
{
	double r[3];
	if (!text || sscanf(text, "%lf %lf %lf", &r[0], &r[1], &r[2]) != 3)
		return 0;
	for (double a : r)
		if (AngleDiff(a, 90 * floor(a / 90 + 0.5)) > 0.5)
			return 0;
	return 1;
}

// text: "slot rx ry rz". The other slot (0-5) already using this rotation,
// or -1.
ZFUNC RSViewOwner(char* text, double, char*, int, char*, int, char**)
{
	int slot;
	double rx, ry, rz;
	if (!text || sscanf(text, "%d %lf %lf %lf", &slot, &rx, &ry, &rz) != 4)
		return -1;
	LoadViews();
	for (int i = 0; i < kDefaultView; ++i)
		if (i != slot && gViews[i].used && SameRotation(gViews[i], rx, ry, rz))
			return static_cast<float>(i);
	return -1;
}

static int gPrepSlot = -1;
static double gPrep[6];

// text: "slot scale x y rx ry rz", the view an image is about to be loaded
// for (RSViewImage takes the path). Returns 1, or -1.
ZFUNC RSViewPrep(char* text, double, char*, int, char*, int, char**)
{
	int slot;
	double v[6];
	if (!text || sscanf(text, "%d %lf %lf %lf %lf %lf %lf", &slot, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7 ||
			slot < 0 || slot >= kDefaultView || v[0] <= 0)
		return -1;
	gPrepSlot = slot;
	memcpy(gPrep, v, sizeof(gPrep));
	return 1;
}

// Sets a slot's anchor from RSViewPrep's values and shows it; the
// silhouette is recorded as it is now. Call with the lock.
static void AnchorSlot(int slot, const double* v)
{
	View& w = gViews[slot];
	w.used = true;
	w.scale = v[0];
	w.ax = v[1];
	w.ay = v[2];
	w.rx = v[3];
	w.ry = v[4];
	w.rz = v[5];
	const std::vector<int> codes = CurrentRotCodes();
	if (!codes.empty())
		w.codes = codes;
	w.mask.clear();
	w.gw = 0;
	SaveMask(slot);
	SaveViews();
	gActiveView = -1;
	ActivateView(slot);
}

static void SwitchProject(const std::wstring& dir);
static void WriteLink(const std::wstring& zpr);
static std::wstring gCurrentZpr;  // the ZBrush project last opened or saved

// Without a project, the first image loaded starts one in a JRef folder next
// to it. The image is copied into the project ("Front.png" ...) and stored
// by that name, so the project keeps working when moved. Returns the stored
// path (the original if it cannot be copied).
static std::string ProjectImage(int slot, const char* source)
{
	static const wchar_t* kNames[6] = {L"Front", L"Back", L"Right", L"Left", L"Top", L"Bottom"};
	const std::wstring src = Widen(source);
	if (gProjectDir.empty()) {
		const size_t slash = src.find_last_of(L"\\/");
		if (slash != std::wstring::npos) {
			// Views loaded before there was a project move into it, unless
			// that folder already holds a project.
			std::vector<View> before(gViews, gViews + kViewSlots);
			std::vector<std::wstring> paths;
			for (const View& v : before)
				paths.push_back(v.path.empty() ? L"" : ResolveImage(v.path));
			const std::wstring dir = src.substr(0, slash + 1) + L"JRef\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			SwitchProject(dir);
			bool empty = true;
			for (int i = 0; i < kViewSlots; ++i)
				empty = empty && !gViews[i].used;
			if (empty) {
				for (int i = 0; i < kViewSlots; ++i) {
					if (!before[i].used || i == slot)
						continue;
					gViews[i] = before[i];
					if (i < 6 && !paths[i].empty()) {
						char utf8[MAX_PATH * 3] = {0};
						WideCharToMultiByte(CP_UTF8, 0, paths[i].c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
						gViews[i].path = ProjectImage(i, utf8);
					}
					SaveMask(i);
				}
				SaveViews();
			}
		}
	}
	if (gProjectDir.empty() || slot < 0 || slot >= 6)
		return source;
	const size_t dot = src.find_last_of(L'.');
	const std::wstring ext = dot != std::wstring::npos && src.find_last_of(L"\\/") < dot ? src.substr(dot) : L".png";
	const std::wstring name = std::wstring(kNames[slot]) + ext;
	const std::wstring dest = gProjectDir + name;
	if (_wcsicmp(dest.c_str(), src.c_str()) != 0 && !CopyFileW(src.c_str(), dest.c_str(), FALSE))
		return source;
	char utf8[MAX_PATH * 3] = {0};
	WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
	return utf8;
}

// Makes dir (with a trailing backslash) the project: remembered for next
// time, and its views and settings loaded. Caller holds the lock.
static void SwitchProject(const std::wstring& dir)
{
	gProjectTried = true;
	gProjectDir = dir;
	if (FILE* f = _wfopen((DataFolder() + L"last_project.txt").c_str(), L"wb")) {
		char utf8[MAX_PATH * 3] = {0};
		WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
		fputs(utf8, f);
		fclose(f);
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		char utf8[MAX_PATH * 3] = {0};
		WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
		fprintf(f, "%lu project %s\n", GetTickCount(), utf8);
		fclose(f);
	}
	for (int i = 0; i < kViewSlots; ++i)
		gViews[i] = View();
	// No project (a ZBrush project without one): no views until an image is
	// loaded, which starts one.
	gViewsLoaded = dir.empty();
	gDisplayLoaded = false;
	LoadViews();
	LoadDisplay();
	BlankView();
	// The ZBrush project open now belongs to this one.
	if (!dir.empty() && !gCurrentZpr.empty())
		WriteLink(gCurrentZpr);
}

// ---------------------------------------------------------------------------
// ZBrush projects. ZBrush's own opens and saves of .zpr files are watched (its
// imports of CreateFileW/A and CloseHandle are pointed here), so each ZBrush
// project gets "<project>.jref" next to it naming its JRef folder (relative to
// the project, and absolute): opening the project again, on this computer or
// another, reopens its references. A .zpr read only in part (Lightbox
// thumbnails) does not count; a project without a .jref has none.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION gZprLock;
static bool gZprReady = false;
struct ZprFile {
	HANDLE handle;
	std::wstring path;
	bool write;
};
static std::vector<ZprFile> gZprFiles;
static volatile LONG gZprCount = 0;  // gZprFiles.size(), read without the lock
typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* CloseHandleFn)(HANDLE);
static CreateFileWFn gRealCreateFileW = nullptr;
static CreateFileAFn gRealCreateFileA = nullptr;
static CloseHandleFn gRealCloseHandle = nullptr;
typedef BOOL(WINAPI* MoveFileExWFn)(LPCWSTR, LPCWSTR, DWORD);
static MoveFileExWFn gRealMoveFileExW = nullptr;

static void ProjectLog(const char* what, const std::wstring& path)
{
	char utf8[MAX_PATH * 3] = {0};
	WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu %s %s\n", GetTickCount(), what, utf8);
		fclose(f);
	}
}

static bool IsZpr(const std::wstring& path)
{
	return path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".zpr") == 0;
}

static std::wstring LinkFor(const std::wstring& zpr)
{
	return zpr.substr(0, zpr.size() - 4) + L".jref";
}

static std::wstring FolderOf(const std::wstring& path)
{
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

static bool IsFolder(const std::wstring& path)
{
	const DWORD attr = GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string Utf8(const std::wstring& w)
{
	char utf8[MAX_PATH * 3] = {0};
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
	return utf8;
}

// The JRef folder a link file names: relative to the link first (the
// project moved with its references), then absolute. Empty if neither.
static std::wstring ReadLink(const std::wstring& link)
{
	FILE* f = _wfopen(link.c_str(), L"rb");
	if (!f)
		return L"";
	char lines[3][1024] = {{0}};
	for (int i = 0; i < 3 && fgets(lines[i], sizeof(lines[i]), f); ++i) {
		size_t n = strlen(lines[i]);
		while (n && (lines[i][n - 1] == '\n' || lines[i][n - 1] == '\r'))
			lines[i][--n] = 0;
	}
	fclose(f);
	if (strcmp(lines[0], "JRef link") != 0)
		return L"";
	std::wstring dir = FolderOf(link) + Widen(lines[1]);
	if (lines[1][0] && IsFolder(dir))
		return dir.back() == L'\\' ? dir : dir + L'\\';
	dir = Widen(lines[2]);
	if (lines[2][0] && IsFolder(dir))
		return dir.back() == L'\\' ? dir : dir + L'\\';
	return L"";
}

static void WriteLink(const std::wstring& zpr)
{
	if (gProjectDir.empty())
		return;
	wchar_t rel[MAX_PATH] = {0};
	const std::wstring from = FolderOf(zpr);
	if (!PathRelativePathToW(rel, from.c_str(), FILE_ATTRIBUTE_DIRECTORY, gProjectDir.c_str(), FILE_ATTRIBUTE_DIRECTORY))
		rel[0] = 0;
	std::wstring r = rel;
	if (r.size() > 1 && r[0] == L'.' && (r[1] == L'\\' || r[1] == L'/'))
		r = r.substr(2);
	while (!r.empty() && (r[0] == L'\\' || r[0] == L'/'))
		r = r.substr(1);
	if (FILE* f = _wfopen(LinkFor(zpr).c_str(), L"wb")) {
		fprintf(f, "JRef link\n%s\n%s\n", Utf8(r).c_str(), Utf8(gProjectDir).c_str());
		fclose(f);
		ProjectLog("project linked to", zpr);
	}
}

// ZBrush loaded (read in full) or saved a project.
static void ZprDone(const std::wstring& zpr, bool write)
{
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	gCurrentZpr = zpr;
	if (write) {
		WriteLink(zpr);
	} else {
		const std::wstring dir = ReadLink(LinkFor(zpr));
		ProjectLog(dir.empty() ? "zbrush project opened, no JRef project:" : "zbrush project opened:", zpr);
		if (!dir.empty() && _wcsicmp(dir.c_str(), gProjectDir.c_str()) != 0) {
			SwitchProject(dir);
		} else if (dir.empty() && !gProjectDir.empty()) {
			// Another model: no references until one is loaded for it.
			SwitchProject(L"");
		}
	}
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
}

static void TrackZpr(HANDLE h, const std::wstring& path, DWORD access, DWORD disposition)
{
	if (h == INVALID_HANDLE_VALUE || !IsZpr(path))
		return;
	const bool write = (access & (GENERIC_WRITE | FILE_WRITE_DATA)) != 0 || disposition == CREATE_ALWAYS || disposition == CREATE_NEW;
	wchar_t full[MAX_PATH] = {0};
	const std::wstring p = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) ? std::wstring(full) : path;
	EnterCriticalSection(&gZprLock);
	gZprFiles.push_back({h, p, write});
	gZprCount = static_cast<LONG>(gZprFiles.size());
	LeaveCriticalSection(&gZprLock);
	ProjectLog(write ? "zpr opened for writing" : "zpr opened for reading", p);
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
	DWORD flags, HANDLE templ)
{
	const HANDLE h = gRealCreateFileW(name, access, share, sa, disposition, flags, templ);
	if (name)
		TrackZpr(h, name, access, disposition);
	return h;
}

static HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
	DWORD flags, HANDLE templ)
{
	const HANDLE h = gRealCreateFileA(name, access, share, sa, disposition, flags, templ);
	if (name) {
		wchar_t w[MAX_PATH] = {0};
		MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
		TrackZpr(h, w, access, disposition);
	}
	return h;
}

static BOOL WINAPI HookCloseHandle(HANDLE h)
{
	if (gZprCount == 0)
		return gRealCloseHandle(h);
	ZprFile done = {nullptr, L"", false};
	bool found = false, full = false;
	EnterCriticalSection(&gZprLock);
	for (size_t i = 0; i < gZprFiles.size(); ++i)
		if (gZprFiles[i].handle == h) {
			done = gZprFiles[i];
			gZprFiles.erase(gZprFiles.begin() + i);
			gZprCount = static_cast<LONG>(gZprFiles.size());
			found = true;
			break;
		}
	LeaveCriticalSection(&gZprLock);
	if (found && !done.write) {
		// Read in full: the file position has reached its end.
		LARGE_INTEGER size = {}, pos = {}, zero = {};
		full = GetFileSizeEx(h, &size) && SetFilePointerEx(h, zero, &pos, FILE_CURRENT) && size.QuadPart > 0 &&
			pos.QuadPart >= size.QuadPart * 9 / 10;
		char what[96];
		snprintf(what, sizeof(what), "zpr closed after reading %lld of %lld bytes:", static_cast<long long>(pos.QuadPart),
			static_cast<long long>(size.QuadPart));
		ProjectLog(what, done.path);
	}
	const BOOL result = gRealCloseHandle(h);
	if (found && (done.write || full))
		ZprDone(done.path, done.write);
	return result;
}

// A save written to a temporary file and then renamed to the .zpr.
static BOOL WINAPI HookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
	const BOOL ok = gRealMoveFileExW(from, to, flags);
	if (ok && to && IsZpr(to)) {
		wchar_t full[MAX_PATH] = {0};
		ProjectLog("zpr written by renaming to", to);
		ZprDone(GetFullPathNameW(to, MAX_PATH, full, nullptr) ? std::wstring(full) : std::wstring(to), true);
	}
	return ok;
}

static BOOL WINAPI HookMoveFileW(LPCWSTR from, LPCWSTR to)
{
	return HookMoveFileExW(from, to, MOVEFILE_COPY_ALLOWED);
}

// Points every module's imports of CreateFileW/A and CloseHandle (from
// kernel32 or kernelbase) at the hooks, except system modules other than
// the C runtimes ZBrush may read files through.
static void PatchImports(HMODULE module, const void* const* from, const void* const* to, int count)
{
	BYTE* base = reinterpret_cast<BYTE*>(module);
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress || !dir.Size)
		return;
	for (const IMAGE_IMPORT_DESCRIPTOR* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
		for (IMAGE_THUNK_DATA* t = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk); t->u1.Function; ++t) {
			for (int i = 0; i < count; ++i) {
				if (!from[i] || reinterpret_cast<const void*>(t->u1.Function) != from[i])
					continue;
				DWORD old;
				if (VirtualProtect(&t->u1.Function, sizeof(t->u1.Function), PAGE_READWRITE, &old)) {
					t->u1.Function = reinterpret_cast<ULONG_PTR>(to[i]);
					VirtualProtect(&t->u1.Function, sizeof(t->u1.Function), old, &old);
				}
			}
		}
	}
}

static void InstallZprHooks()
{
	if (gZprReady)
		return;
	gZprReady = true;
	InitializeCriticalSection(&gZprLock);
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll");
	gRealCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(k32, "CreateFileW"));
	gRealCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(k32, "CreateFileA"));
	gRealCloseHandle = reinterpret_cast<CloseHandleFn>(GetProcAddress(k32, "CloseHandle"));
	gRealMoveFileExW = reinterpret_cast<MoveFileExWFn>(GetProcAddress(k32, "MoveFileExW"));
	if (!gRealCreateFileW || !gRealCreateFileA || !gRealCloseHandle || !gRealMoveFileExW)
		return;
	auto at = [](HMODULE m, const char* name) { return m ? reinterpret_cast<const void*>(GetProcAddress(m, name)) : nullptr; };
	const void* from[10] = {at(k32, "CreateFileW"), at(k32, "CreateFileA"), at(k32, "CloseHandle"), at(k32, "MoveFileExW"),
		at(k32, "MoveFileW"), at(kb, "CreateFileW"), at(kb, "CreateFileA"), at(kb, "CloseHandle"), at(kb, "MoveFileExW"),
		at(kb, "MoveFileW")};
	const void* to[10] = {reinterpret_cast<const void*>(&HookCreateFileW), reinterpret_cast<const void*>(&HookCreateFileA),
		reinterpret_cast<const void*>(&HookCloseHandle), reinterpret_cast<const void*>(&HookMoveFileExW),
		reinterpret_cast<const void*>(&HookMoveFileW), reinterpret_cast<const void*>(&HookCreateFileW),
		reinterpret_cast<const void*>(&HookCreateFileA), reinterpret_cast<const void*>(&HookCloseHandle),
		reinterpret_cast<const void*>(&HookMoveFileExW), reinterpret_cast<const void*>(&HookMoveFileW)};
	wchar_t sys[MAX_PATH] = {0};
	GetWindowsDirectoryW(sys, MAX_PATH);
	const size_t sysLen = wcslen(sys);
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&InstallZprHooks), &self);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;
	MODULEENTRY32W me = {sizeof(me)};
	int patched = 0;
	for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
		if (me.hModule == self)
			continue;
		const bool system = sysLen && _wcsnicmp(me.szExePath, sys, sysLen) == 0;
		const bool runtime = _wcsicmp(me.szModule, L"ucrtbase.dll") == 0 || _wcsicmp(me.szModule, L"msvcrt.dll") == 0 ||
			_wcsnicmp(me.szModule, L"msvcp", 5) == 0;
		if (system && !runtime)
			continue;
		PatchImports(me.hModule, from, to, 10);
		++patched;
	}
	CloseHandle(snap);
	char what[64];
	snprintf(what, sizeof(what), "watching ZBrush's project files (%d modules), window:", patched);
	HWND zb = nullptr;
	EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&zb));
	wchar_t title[512] = {0};
	if (zb)
		GetWindowTextW(zb, title, 512);
	ProjectLog(what, title);
}

// text: "open path", path being a project's project.jref (or any file in its
// folder, or a ZBrush project's .jref link): makes it the project. "has"
// returns 1 if a project is open. Returns 1, or -1 if the folder does not
// exist.
ZFUNC RSProject(char* text, double, char*, int, char*, int, char**)
{
	if (!text)
		return -1;
	EnsureLock();
	InstallZprHooks();
	if (strncmp(text, "has", 3) == 0) {
		ProjectFolder();
		return gProjectDir.empty() ? 0 : 1;
	}
	if (strncmp(text, "open ", 5) != 0)
		return -1;
	std::wstring path = Widen(text + 5);
	if (path.size() > 2 && path[0] == L'!' && path[1] == L':')
		path = path.substr(2);
	for (wchar_t& c : path)
		if (c == L'/')
			c = L'\\';
	const DWORD attr = GetFileAttributesW(path.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES)
		return -1;
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		const std::wstring linked = ReadLink(path);
		const size_t slash = path.find_last_of(L'\\');
		path = !linked.empty() ? linked : slash == std::wstring::npos ? L"" : path.substr(0, slash);
	}
	if (path.empty())
		return -1;
	if (path.back() != L'\\')
		path += L'\\';
	EnterCriticalSection(&gOverlayLock);
	SwitchProject(path);
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return 1;
}

// text: "name" returns a stored number (0 if none), "name value" stores it.
// Kept in values.txt next to the DLL (this computer's settings: the saved
// document background, the depth fix).
ZFUNC RSValue(char* text, double, char*, int, char*, int, char**)
{
	char name[32] = {0};
	double value = 0;
	if (!text)
		return 0;
	const int n = sscanf(text, "%31s %lf", name, &value);
	if (n < 1)
		return 0;
	std::vector<std::pair<std::string, double>> values;
	const std::wstring path = DataFolder() + L"values.txt";
	if (FILE* f = _wfopen(path.c_str(), L"rb")) {
		char k[32];
		double v;
		while (fscanf(f, "%31s %lf", k, &v) == 2)
			values.push_back({k, v});
		fclose(f);
	}
	double found = 0;
	bool had = false;
	for (auto& kv : values)
		if (kv.first == name) {
			if (n == 2)
				kv.second = value;
			found = kv.second;
			had = true;
		}
	if (n == 2) {
		if (!had)
			values.push_back({name, value});
		if (FILE* f = _wfopen(path.c_str(), L"wb")) {
			for (const auto& kv : values)
				fprintf(f, "%s %.6f\n", kv.first.c_str(), kv.second);
			fclose(f);
		}
		return static_cast<float>(value);
	}
	return static_cast<float>(found);
}

// text: the image's full path, for the slot given to RSViewPrep. The view
// is anchored at the current zoom and position and shown, fitted to the
// document. Returns 1 (shown), 2 (kept: not snapped to that view now), -1 if
// the image cannot be read, -2 without RSViewPrep.
ZFUNC RSViewImage(char* text, double, char*, int, char*, int, char**)
{
	if (gPrepSlot < 0)
		return -2;
	if (!text || !LoadImage(Widen(text)))
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadViews();
	View& w = gViews[gPrepSlot];
	w.path = ProjectImage(gPrepSlot, text);
	w.imgSX = w.imgSY = 1;
	w.imgX = w.imgY = 0;
	// Shown (and the model's outline recorded) now only when snapped to that
	// view; otherwise kept until it is snapped to.
	const bool here = gRotLocked && AtView(CurrentRotCodes(), gPrepSlot);
	float result = 1;
	if (here) {
		AnchorSlot(gPrepSlot, gPrep);
	} else {
		w.used = true;
		w.scale = gPrep[0];
		w.ax = gPrep[1];
		w.ay = gPrep[2];
		w.mask.clear();
		w.gw = 0;
		SaveMask(gPrepSlot);
		SaveViews();
		ViewLog("image stored for later", gPrepSlot);
		result = 2;
	}
	gPrepSlot = -1;
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	WaitViewRecord();
	return result;
}

// text: "slot scale x y rx ry rz": Anchor pressed while a view is showing.
// Returns 1, or -1.
ZFUNC RSViewAnchor(char* text, double, char*, int, char*, int, char**)
{
	int slot;
	double v[6];
	if (!text || sscanf(text, "%d %lf %lf %lf %lf %lf %lf", &slot, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7 ||
			slot < 0 || slot >= kViewSlots || v[0] <= 0)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadViews();
	AnchorSlot(slot, v);
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	WaitViewRecord();
	return 1;
}

// text: "slot". Removes a view's image. If it was showing, the background
// goes black until another view is snapped to. Returns 1.
ZFUNC RSViewClear(char* text, double, char*, int, char*, int, char**)
{
	const int slot = text ? atoi(text) : -1;
	if (slot < 0 || slot >= kDefaultView)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadViews();
	gViews[slot] = View();
	SaveMask(slot);
	SaveViews();
	ViewLog("image removed", slot);
	if (gActiveView == slot)
		BlankView();
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return 1;
}

// text: "slot". 1 if the view has an image, else 0.
ZFUNC RSViewUsed(char* text, double, char*, int, char*, int, char**)
{
	const int slot = text ? atoi(text) : -1;
	if (slot < 0 || slot >= kViewSlots)
		return 0;
	LoadViews();
	return gViews[slot].used ? 1 : 0;
}

// text: "slot field", field 0-5: scale, x, y, rx, ry, rz of its anchor.
ZFUNC RSViewGet(char* text, double, char*, int, char*, int, char**)
{
	int slot = -1, field = -1;
	if (!text || sscanf(text, "%d %d", &slot, &field) != 2 || slot < 0 || slot >= kViewSlots || field < 0 || field > 5)
		return 0;
	LoadViews();
	const View& v = gViews[slot];
	const double f[6] = {v.scale, v.ax, v.ay, v.rx, v.ry, v.rz};
	return static_cast<float>(f[field]);
}

// Hides the overlay. Returns 1.
ZFUNC RSOverlayStop(char*, double, char*, int, char*, int, char**)
{
	if (gOverlayWnd) {
		EnterCriticalSection(&gOverlayLock);
		gOverlay.visible = false;
		gViewAway = true;
		gViewJob = 0;
		gAdjust = false;
		gAdjustDrag = 0;
		LeaveCriticalSection(&gOverlayLock);
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	}
	return 1;
}
