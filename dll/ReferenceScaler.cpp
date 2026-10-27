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
#include <winternl.h>
#include <shlwapi.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <unordered_map>
#include <map>
#include <tuple>
#include <algorithm>
#include <cmath>
#include <emmintrin.h>
#include <thread>
#include <atomic>
#include <ppl.h>
#include <memory>
#include <dwmapi.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

// Windows' headers define these as macros (rpcndr.h), which MSVC sees.
#undef small
#undef near

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
static const int kDllBuild = 234;

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
	// its centre's shift in anchored document pixels, and its turn about its
	// centre in degrees (clockwise on screen).
	double imgSX = 1, imgSY = 1, imgX = 0, imgY = 0, imgRot = 0;
	// The image's zoom over ratio: turning and panning hold the image's size
	// (MemFollow), while ZBrush's perspective changes its scale. Only for
	// showing the image; ratio stays ZBrush's, for the silhouettes and depth.
	double fix = 1;
	int alpha = 128;
	bool visible = false;
};

// The test suite (dll\test\TrackTest.cpp, which compiles this file) drives
// the tracking without ZBrush: the camera's axes and pivot depth come from
// gTestCam instead of ZBrush's memory, nothing is eased over time, and no
// frames are saved.
static bool gTesting = false;
static struct { double d[3][3]; double pivot; } gTestCam = {};

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

// Stage 2 (keying): the background is the document's background colour,
// learned per row from what ZBrush shows (LearnRowBackground), inside the
// viewport; the model is everything else. The overlay is excluded from screen capture (SetWindowDisplayAffinity), captures the
// screen under itself every time the desktop changes (DXGI desktop
// duplication, or BitBlt where that is unavailable), and shows an opaque copy
// of it with the background replaced by the reference. The copy is at most a frame behind what ZBrush drew.
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
static volatile DWORD gDragEndAt = 0;  // when the last drag on the canvas ended
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
// the right button) to scale it about its centre, click off it to finish.
static volatile bool gAdjust = false;
// Mouse buttons held, and when one last went down or up (the camera only
// turns while one is held, or in the snap animation just after).
static volatile LONG gButtonsDown = 0;
static volatile DWORD gLastButtonAt = 0;
static volatile bool gAdjustEscape = false;  // Esc pressed or clicked off the image: the overlay turns Adjust Image off
static bool gAdjustEatUp = false;  // the click that ended Adjust Image: ZBrush sees neither its down nor its up
static volatile LONG gAdjustUndoKey = 0;  // Ctrl+Z 1 (undo), 2 (redo): the key hook asks, the overlay does it
static bool AdjustMouse(WPARAM wp, POINT pt);
static bool CanvasCursor(HCURSOR cur);

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
	if (gAdjustEatUp && (wp == WM_LBUTTONUP || wp == WM_RBUTTONUP)) {
		gAdjustEatUp = false;
		LeaveCriticalSection(&gOverlayLock);
		return 1;
	}
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
			gDragEndAt = GetTickCount();
			g.kind =g.mode == kDragIgnore ? kKindSculpt : g.scaleSegments > 0 ? kKindScale :
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
		case 'Z':
		case 'Y':
			// Adjust Image's own undo and redo, which ZBrush does not see (its
			// undo would take back sculpting instead).
			if (gAdjust && gCtrlDown && ZBrushInFront()) {
				if (down)
					gAdjustUndoKey = k->vkCode == 'Y' || (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2 : 1;
				return 1;
			}
			break;
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
// The levels live on the heap so their address stays put while the cache's
// entries move (the GPU's textures go by serial, one per image loaded).
struct ImageCache {
	std::wstring path;
	std::shared_ptr<std::vector<Level>> levels;
	DWORD usedAt;
	FILETIME written;
	unsigned serial;
};
static unsigned gImageSerial = 0;

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
			return c.levels->empty() ? nullptr : c.levels.get();
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
	gImages.push_back({w, std::make_shared<std::vector<Level>>(), GetTickCount(), written, ++gImageSerial});
	LoadLevels(w, *gImages.back().levels);
	return gImages.back().levels->empty() ? nullptr : gImages.back().levels.get();
}

// The cache entry holding these levels (null if gone), for the GPU's copy.
static const ImageCache* ImageEntry(const std::vector<Level>* levels)
{
	for (const ImageCache& c : gImages)
		if (c.levels.get() == levels)
			return &c;
	return nullptr;
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
	const double k = st.pixelsPerDoc, r = st.ratio * st.fix;
	x0 = (st.px + r * (cx - baseW * 0.5 - st.ax - st.offX)) * k;
	y0 = (st.py + r * (cy - baseH * 0.5 - st.ay - st.offY)) * k;
	w = baseW * r * k;
	h = baseH * r * k;
	return true;
}

static const double kPi = 3.14159265358979;

// Overlay point (x, y) in the rectangle's own frame (unturned), relative to
// its centre, for an image turned by deg about its centre.
static void Unturn(double x0, double y0, double w, double h, double deg, double x, double y, double& lx, double& ly)
{
	const double a = deg * kPi / 180, c = cos(a), s = sin(a);
	const double dx = x - (x0 + w * 0.5), dy = y - (y0 + h * 0.5);
	lx = dx * c + dy * s;
	ly = -dx * s + dy * c;
}

// A point in the rectangle's own frame (relative to its centre) on the overlay.
static void Turn(double x0, double y0, double w, double h, double deg, double lx, double ly, double& x, double& y)
{
	const double a = deg * kPi / 180, c = cos(a), s = sin(a);
	x = x0 + w * 0.5 + lx * c - ly * s;
	y = y0 + h * 0.5 + lx * s + ly * c;
}

// The same for the image the overlay shows (the showing view's).
static bool ImageRect(const OverlayState& st, double& x0, double& y0, double& w, double& h)
{
	return ImageRectFor(st, gImgW, gImgH, x0, y0, w, h);
}


static void SaveAdjustToView();

// The handles sit round the image's centre (kept on the canvas when the
// centre is off it), so they are in reach however far the image is zoomed:
// the middle square moves, the handle along the image's width scales it
// across, the one along its height up and down, the one between them both
// ways at once (Shift keeps the aspect), and the ring turns it (Shift in
// steps of 15 degrees). Dragging elsewhere on the image moves it, a right
// drag scales it about its centre, a click off it ends Adjust Image.
// 0 none, 1 moving, 2 the two-way handle, 3 right drag, 4 width, 5 height,
// 6 the ring.
static int gAdjustDrag = 0;
static POINT gAdjustDown;
static double gAdjustGX = 0, gAdjustGY = 0;  // the handles' centre on the overlay, as the drag began
static const double kArm = 70, kRing = 105;  // handle and ring distances from it, overlay pixels

// The image's adjustment, for Ctrl+Z (and Ctrl+Shift+Z) while Adjust Image is on.
struct AdjustState {
	double sx, sy, x, y, rot;
	bool operator==(const AdjustState& o) const { return sx == o.sx && sy == o.sy && x == o.x && y == o.y && rot == o.rot; }
};
static AdjustState gAdjust0;
static std::vector<AdjustState> gAdjustUndo, gAdjustRedo;
static int gAdjustUndoView = -1;  // the view the steps are for

static AdjustState AdjustNow()
{
	const OverlayState& s = gOverlay;
	return {s.imgSX, s.imgSY, s.imgX, s.imgY, s.imgRot};
}

static void AdjustApply(const AdjustState& a)
{
	OverlayState& s = gOverlay;
	s.imgSX = a.sx;
	s.imgSY = a.sy;
	s.imgX = a.x;
	s.imgY = a.y;
	s.imgRot = a.rot;
}

// Keeps a step to undo (before a change). Caller holds the lock.
static int ActiveViewSlot();

static void AdjustRemember(const AdjustState& before)
{
	if (gAdjustUndoView != ActiveViewSlot()) {
		gAdjustUndo.clear();
		gAdjustRedo.clear();
		gAdjustUndoView = ActiveViewSlot();
	}
	gAdjustUndo.push_back(before);
	if (gAdjustUndo.size() > 100)
		gAdjustUndo.erase(gAdjustUndo.begin());
	gAdjustRedo.clear();
}

// The handles' centre on the overlay, for the image's rectangle x0, y0, w,
// h (overlay pixels): the image's centre, kept far enough inside the
// viewport for the ring.
static void ViewportRect(int w, int h, int& x0, int& y0, int& x1, int& y1);

static void GizmoCentre(const OverlayState& st, double x0, double y0, double w, double h, double& gx, double& gy)
{
	const int ow = static_cast<int>(st.docW * st.pixelsPerDoc + 0.5), oh = static_cast<int>(st.docH * st.pixelsPerDoc + 0.5);
	int vx0, vy0, vx1, vy1;
	ViewportRect(ow, oh, vx0, vy0, vx1, vy1);
	const double m = kRing + 12;
	gx = x0 + w * 0.5;
	gy = y0 + h * 0.5;
	if (vx1 - vx0 > 2 * m)
		gx = std::max(vx0 + m, std::min(vx1 - m, gx));
	if (vy1 - vy0 > 2 * m)
		gy = std::max(vy0 + m, std::min(vy1 - m, gy));
}

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
		// ZBrush's menus and popups over the canvas show an interface cursor:
		// those clicks go to ZBrush. Every other click on the canvas is
		// Adjust Image's, so ZBrush never turns the camera (or sculpts) while
		// it is on, the model included.
		CURSORINFO ci = {sizeof(ci)};
		if (GetCursorInfo(&ci) && !CanvasCursor((ci.flags & CURSOR_SHOWING) ? ci.hCursor : nullptr))
			return false;
		// Overlay pixels from here on.
		const double mx = pt.x - st.screenX, my = pt.y - st.screenY;
		double gx, gy;
		GizmoCentre(st, x0, y0, w, h, gx, gy);
		double lx, ly;
		Unturn(x0, y0, w, h, st.imgRot, mx, my, lx, ly);
		const bool inside = fabs(lx) <= w * 0.5 && fabs(ly) <= h * 0.5;
		// Distances from the handles, in the image's own frame about the
		// handles' centre (turned with the image).
		double hx, hy;
		Unturn(gx - w * 0.5, gy - h * 0.5, w, h, st.imgRot, mx, my, hx, hy);
		auto onHandle = [&](double ax, double ay) { return hypot(hx - ax, hy - ay) < 12; };
		const double fromG = hypot(hx, hy);
		int drag = 0;
		if (wp == WM_RBUTTONDOWN)
			drag = inside || fromG < kRing + 10 ? 3 : 0;
		else if (fabs(hx) < 11 && fabs(hy) < 11)
			drag = 1;
		else if (onHandle(kArm, 0))
			drag = 4;
		else if (onHandle(0, kArm))
			drag = 5;
		else if (onHandle(kArm * 0.7, kArm * 0.7))
			drag = 2;
		else if (fabs(fromG - kRing) < 9)
			drag = 6;
		else if (inside)
			drag = 1;
		if (!drag) {
			// Off the image: Adjust Image ends, as with Esc.
			gAdjustEscape = true;
			gAdjustEatUp = true;
			return true;
		}
		gAdjust0 = AdjustNow();
		gAdjustDown = pt;
		gAdjustGX = gx;
		gAdjustGY = gy;
		gAdjustDrag = drag;
		return true;
	}
	case WM_MOUSEMOVE: {
		double bw, bh;
		const double r = st.ratio * st.fix;
		if (!gAdjustDrag || r <= 0 || k <= 0 || !FitSize(st, bw, bh))
			return false;
		const AdjustState& a = gAdjust0;
		AdjustState n = a;
		const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		// The mouse, as the drag began and now, from the handles' centre in
		// the image's own frame (overlay pixels).
		const double gx = gAdjustGX + st.screenX, gy = gAdjustGY + st.screenY;
		const double turn = a.rot * kPi / 180, c = cos(turn), s = sin(turn);
		const double dx0 = gAdjustDown.x - gx, dy0 = gAdjustDown.y - gy, dx1 = pt.x - gx, dy1 = pt.y - gy;
		const double lx0 = dx0 * c + dy0 * s, ly0 = -dx0 * s + dy0 * c;
		const double lx1 = dx1 * c + dy1 * s, ly1 = -dx1 * s + dy1 * c;
		const double minSX = 4 / (k * r * bw), minSY = 4 / (k * r * bh);
		auto ratioOf = [](double now, double was) { return fabs(was) > 2 ? now / was : 1.0; };
		if (gAdjustDrag == 1) {
			// The drag in anchored document pixels.
			n.x += (pt.x - gAdjustDown.x) / (k * r);
			n.y += (pt.y - gAdjustDown.y) / (k * r);
		} else if (gAdjustDrag == 2) {
			double fx = ratioOf(lx1, lx0), fy = ratioOf(ly1, ly0);
			if (shift)
				fx = fy = 0.5 * (fx + fy);
			n.sx = a.sx * fx;
			n.sy = a.sy * fy;
		} else if (gAdjustDrag == 3) {
			const double f = exp(((pt.x - gAdjustDown.x) - (pt.y - gAdjustDown.y)) / 400.0);
			n.sx = a.sx * f;
			n.sy = a.sy * f;
		} else if (gAdjustDrag == 4) {
			n.sx = a.sx * ratioOf(lx1, lx0);
		} else if (gAdjustDrag == 5) {
			n.sy = a.sy * ratioOf(ly1, ly0);
		} else {
			double deg = a.rot + (atan2(dy1, dx1) - atan2(dy0, dx0)) * 180 / kPi;
			if (shift)
				deg = 15 * floor(deg / 15 + 0.5);
			n.rot = deg - 360 * floor((deg + 180) / 360);
		}
		n.sx = std::max(minSX, std::min(100.0, n.sx));
		n.sy = std::max(minSY, std::min(100.0, n.sy));
		AdjustApply(n);
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
		return false;  // the cursor still moves
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
		if (!gAdjustDrag)
			return false;
		gAdjustDrag = 0;
		if (!(AdjustNow() == gAdjust0))
			AdjustRemember(gAdjust0);
		SaveAdjustToView();
		return true;
	}
	return false;
}

// Ctrl+Z (undo) or Ctrl+Shift+Z (redo) of Adjust Image. Caller holds the lock.
static bool AdjustUndo(bool redo)
{
	if (gAdjustUndoView != ActiveViewSlot() || gAdjustDrag)
		return false;
	std::vector<AdjustState>& from = redo ? gAdjustRedo : gAdjustUndo;
	std::vector<AdjustState>& to = redo ? gAdjustUndo : gAdjustRedo;
	if (from.empty())
		return false;
	to.push_back(AdjustNow());
	AdjustApply(from.back());
	from.pop_back();
	SaveAdjustToView();
	return true;
}

// One step of drawing a layer: a rectangle filled (replacing what is there),
// or an image drawn over what is there, clipped to the rectangle. The CPU
// runs the steps on a Buffer32 (RunOps), the GPU draws the same list
// (GpuOverlay::DrawOps), so both show the same.
struct DrawOp {
	bool image = false;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	uint32_t argb = 0;                       // a fill's colour, premultiplied
	const std::vector<Level>* levels = nullptr;
	double ix = 0, iy = 0, iw = 0, ih = 0;  // where an image is drawn
	double rot = 0;                          // ... turned about its centre, degrees
	int alpha = 255;                         // an image's opacity
};

// The overlay pixels a turned rectangle covers.
static void TurnedBounds(double x0, double y0, double w, double h, double deg, double& bx0, double& by0, double& bx1, double& by1)
{
	bx0 = by0 = 1e30;
	bx1 = by1 = -1e30;
	for (int i = 0; i < 4; ++i) {
		double x, y;
		Turn(x0, y0, w, h, deg, (i & 1 ? 0.5 : -0.5) * w, (i & 2 ? 0.5 : -0.5) * h, x, y);
		bx0 = std::min(bx0, x);
		by0 = std::min(by0, y);
		bx1 = std::max(bx1, x);
		by1 = std::max(by1, y);
	}
}

static void FillOp(std::vector<DrawOp>& ops, int x0, int y0, int x1, int y1, uint32_t argb)
{
	DrawOp o;
	o.x0 = x0;
	o.y0 = y0;
	o.x1 = x1;
	o.y1 = y1;
	o.argb = argb;
	ops.push_back(o);
}

// The Adjust Image handles on a w x h overlay: the image's outline (dotted,
// turned with it), and round its centre the move square, the width, height
// and two-way handles on arms along the image's axes, and the turning ring.
static void DrawGizmo(const OverlayState& st, int w, int h, std::vector<DrawOp>& ops)
{
	double fx, fy, fw, fh;
	if (!ImageRect(st, fx, fy, fw, fh))
		return;
	const uint32_t orange = 0xFFFF9020u, light = 0xFFFFD0A0u;  // A, R, G, B as 0xAARRGGBB in the BGRA buffer
	auto dot = [&](double x, double y, int r, uint32_t col) {
		if (x < -r || y < -r || x > w + r || y > h + r)
			return;
		const int ix = static_cast<int>(floor(x)), iy = static_cast<int>(floor(y));
		FillOp(ops, ix - r, iy - r, ix + r + 1, iy + r + 1, col);
	};
	// The outline: a dot every 5 pixels, only where it is on the overlay.
	for (int e = 0; e < 4; ++e) {
		const double s0 = e & 1 ? 0.5 : -0.5;
		const double len = e < 2 ? fw : fh;
		for (double t = 0; t <= len; t += 5) {
			const double u = t / len - 0.5;
			double x, y;
			if (e < 2)
				Turn(fx, fy, fw, fh, st.imgRot, u * fw, s0 * fh, x, y);
			else
				Turn(fx, fy, fw, fh, st.imgRot, s0 * fw, u * fh, x, y);
			dot(x, y, 1, orange);
		}
	}
	double gx, gy;
	GizmoCentre(st, fx, fy, fw, fh, gx, gy);
	auto at = [&](double lx, double ly, double& x, double& y) { Turn(gx - fw * 0.5, gy - fh * 0.5, fw, fh, st.imgRot, lx, ly, x, y); };
	double x, y;
	for (double t = 14; t < kArm - 6; t += 4) {
		at(t, 0, x, y);
		dot(x, y, 1, orange);
		at(0, t, x, y);
		dot(x, y, 1, orange);
	}
	at(kArm, 0, x, y);
	dot(x, y, 6, orange);
	at(0, kArm, x, y);
	dot(x, y, 6, orange);
	at(kArm * 0.7, kArm * 0.7, x, y);
	dot(x, y, 6, light);
	for (int i = 0; i < 90; ++i) {
		const double a = i * 2 * kPi / 90;
		dot(gx + kRing * cos(a), gy + kRing * sin(a), 1, light);
	}
	FillOp(ops, static_cast<int>(gx) - 8, static_cast<int>(gy) - 8, static_cast<int>(gx) + 9, static_cast<int>(gy) + 9, orange);
	FillOp(ops, static_cast<int>(gx) - 5, static_cast<int>(gy) - 5, static_cast<int>(gx) + 6, static_cast<int>(gy) + 6, 0xFF202020u);
}

// Display: Classic shows the reference behind the model. PureRef shows it in
// a panel at one side of the canvas (right unless gPanelLeft), zoomed and
// panned with the camera as Classic would show it; with gMulti, two views
// (gMultiA above gMultiB) share the panel. Outside the panel the background
// shows gBackGrey.
static int gMode = 0, gPanelLeft = 0, gMulti = 0, gMultiA = 0, gMultiB = 3, gBackGrey = 48;
// 3D Auto Track: Multiview goes to the surface being looked at (SurfaceDepth),
// not only the band; on unless switched off (build 233).
static int gAuto3D = 1;
// Auto Persp: the script turns ZBrush's Persp on with PureRef Mode and off
// with Classic when this is on (only then: Persp set by hand stays).
static int gAutoPersp = 0;
// Multiview's four cells (top left, top right, bottom right, bottom left):
// a view slot each, or -1 for none.
static int gCells[4] = {0, 3, 4, 1};
// The reference's gamma and saturation, in percent (100 leaves it as is),
// from the plugin's sliders, saved in display.txt (the sliders start there).
static int gGamma = 100, gSat = 100;
// Model Opacity, in percent: under 100, Classic shows the reference through
// the model as Image Plane does (from the plugin's slider, saved likewise).
static int gModelOpacity = 75;
// Zoom Out, in percent: the panel's cells show the images this many times
// smaller than the canvas shows the model (Multiview slider, saved likewise).
static int gCellZoomOut = 200;
// Auto Persp: the Persp each mode last had (Classic, PureRef), restored when
// the mode changes. Build 229 set it on in PureRef and off in Classic each
// time, undoing Persp turned off by hand in PureRef.
static bool gPerspFor[2] = {false, true};
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
// While held, CameraAxes gives every caller its first reading.
static bool gAxesHold = false, gAxesHeldSet = false;
static double gAxesHeld[3][3];
// A Persp turn's pan, zoom and eye from ZBrush's orbit (OrbitPan); the
// surface point the cells last showed, where the orbit is first looked for.
static void OrbitPan(OverlayState& st);
static bool gOrbitOn = false, gFocusOk = false;
static double gOrbitEye = 0, gFocusG[3] = {};

template <typename Fn>
static void ParallelRows(int y0, int y1, const Fn& fn);

// v / 255, rounded, for v up to 255 * 255.
static inline int Div255(int v)
{
	v += 128;
	return (v + (v >> 8)) >> 8;
}

// Draws an image (premultiplied, scaled by alpha) over dst, its rectangle at
// x0, y0 and dispW x dispH, only inside the clip rectangle. Rows are drawn in
// bands on every core, and opaque pixels or ones over nothing skip the blend:
// a zoom redraws the whole reference, which took 71 ms a step on one core with
// the image filling the canvas (bench, build 143), the lag seen zooming.
static void DrawLevels(Buffer32& dst, const std::vector<Level>& levels, double x0, double y0, double dispW, double dispH,
	int cx0, int cy0, int cx1, int cy1, int alphaScale, double rot = 0)
{
	if (dispW <= 0 || dispH <= 0 || levels.empty())
		return;
	// Smallest level still at least as large as the image on screen.
	size_t li = 0;
	while (li + 1 < levels.size() && levels[li + 1].w >= dispW && levels[li + 1].h >= dispH)
		++li;
	const Level& lv = levels[li];
	double bx0 = x0, by0 = y0, bx1 = x0 + dispW, by1 = y0 + dispH;
	if (rot != 0)
		TurnedBounds(x0, y0, dispW, dispH, rot, bx0, by0, bx1, by1);
	cx0 = std::max(cx0, std::max(0, static_cast<int>(floor(bx0))));
	cy0 = std::max(cy0, std::max(0, static_cast<int>(floor(by0))));
	cx1 = std::min(cx1, std::min(dst.w, static_cast<int>(ceil(bx1))));
	cy1 = std::min(cy1, std::min(dst.h, static_cast<int>(ceil(by1))));
	if (cx0 >= cx1 || cy0 >= cy1)
		return;
	// Gamma (on the unpremultiplied colour) and saturation.
	const bool adjust = gGamma != 100 || gSat != 100;
	unsigned char gammaLut[256];
	for (int i = 0; i < 256; ++i)
		gammaLut[i] = static_cast<unsigned char>(255 * pow(i / 255.0, 100.0 / gGamma) + 0.5);
	const int sat = gSat * 256 / 100;
	if (rot != 0) {
		// Turned: each pixel's place in the image, turned back about its centre.
		const double a = rot * kPi / 180, c = cos(a), s = sin(a), mx = x0 + dispW * 0.5, my = y0 + dispH * 0.5;
		uint32_t* out = reinterpret_cast<uint32_t*>(dst.bits);
		ParallelRows(cy0, cy1, [&](int ya, int yb) {
		for (int y = ya; y < yb; ++y) {
			uint32_t* row = out + static_cast<size_t>(y) * dst.w;
			for (int x = cx0; x < cx1; ++x) {
				const double dx = x + 0.5 - mx, dy = y + 0.5 - my;
				const double u = (dx * c + dy * s + dispW * 0.5) / dispW * lv.w - 0.5;
				const double v = (-dx * s + dy * c + dispH * 0.5) / dispH * lv.h - 0.5;
				if (u < -0.5 || u > lv.w - 0.5 || v < -0.5 || v > lv.h - 0.5)
					continue;
				const double fu = u < 0 ? 0 : u > lv.w - 1 ? lv.w - 1 : u, fv = v < 0 ? 0 : v > lv.h - 1 ? lv.h - 1 : v;
				const int ix = static_cast<int>(fu), iy = static_cast<int>(fv);
				const int wx = static_cast<int>((fu - ix) * 256), wy = static_cast<int>((fv - iy) * 256);
				const uint32_t* r0 = &lv.px[static_cast<size_t>(iy) * lv.w];
				const uint32_t* r1 = iy + 1 < lv.h ? r0 + lv.w : r0;
				const int ix1 = ix + 1 < lv.w ? ix + 1 : ix;
				int ch[4];
				for (int k = 0, sh = 24; sh >= 0; sh -= 8, ++k) {
					const int p = (r0[ix] >> sh) & 255, q = (r0[ix1] >> sh) & 255;
					const int d = (r1[ix] >> sh) & 255, e = (r1[ix1] >> sh) & 255;
					const int top = p * 256 + (q - p) * wx, bottom = d * 256 + (e - d) * wx;
					ch[k] = (top * 256 + (bottom - top) * wy) >> 16;
				}
				if (adjust && ch[0] > 0) {
					const int al = ch[0];
					const int luma = (ch[1] * 77 + ch[2] * 150 + ch[3] * 29) >> 8;
					for (int k = 1; k < 4; ++k) {
						int t = luma + (((ch[k] - luma) * sat) >> 8);
						t = t < 0 ? 0 : t > al ? al : t;
						ch[k] = al == 255 ? gammaLut[t] : gammaLut[t * 255 / al] * al / 255;
					}
				}
				const int alpha = alphaScale == 255 ? ch[0] : Div255(ch[0] * alphaScale);
				if (alpha == 0)
					continue;
				const int src[4] = {alpha, Div255(ch[1] * alpha), Div255(ch[2] * alpha), Div255(ch[3] * alpha)};
				const uint32_t under = row[x];
				uint32_t result = 0;
				for (int k = 0, sh = 24; sh >= 0; sh -= 8, ++k) {
					const int t = src[k] + Div255(static_cast<int>((under >> sh) & 255) * (255 - alpha));
					result |= static_cast<uint32_t>(t > 255 ? 255 : t) << sh;
				}
				row[x] = result;
			}
		}
		});
		return;
	}
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
	ParallelRows(cy0, cy1, [&](int ya, int yb) {
	for (int y = ya; y < yb; ++y) {
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
			const int alpha = alphaScale == 255 ? ch[0] : Div255(ch[0] * alphaScale);
			if (alpha == 0)
				continue;
			if (alpha == 255) {
				row[x] = 0xFF000000u | static_cast<uint32_t>(ch[1]) << 16 | static_cast<uint32_t>(ch[2]) << 8 | static_cast<uint32_t>(ch[3]);
				continue;
			}
			const int sr = Div255(ch[1] * alpha), sg = Div255(ch[2] * alpha), sb = Div255(ch[3] * alpha);
			// Over what is there already (premultiplied).
			const uint32_t under = row[x];
			if (!under) {
				row[x] = static_cast<uint32_t>(alpha) << 24 | static_cast<uint32_t>(sr) << 16 | static_cast<uint32_t>(sg) << 8 | static_cast<uint32_t>(sb);
				continue;
			}
			const int src[4] = {alpha, sr, sg, sb};
			uint32_t result = 0;
			for (int k = 0, c = 24; c >= 0; c -= 8, ++k) {
				const int v = src[k] + Div255(static_cast<int>((under >> c) & 255) * (255 - alpha));
				result |= static_cast<uint32_t>(v > 255 ? 255 : v) << c;
			}
			row[x] = result;
		}
	}
	});
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

static void ImageOp(std::vector<DrawOp>& ops, const std::vector<Level>& levels, double ix, double iy, double iw, double ih,
	int cx0, int cy0, int cx1, int cy1, int alpha, double rot = 0)
{
	DrawOp o;
	o.image = true;
	o.levels = &levels;
	o.ix = ix;
	o.iy = iy;
	o.iw = iw;
	o.ih = ih;
	o.rot = rot;
	o.x0 = cx0;
	o.y0 = cy0;
	o.x1 = cx1;
	o.y1 = cy1;
	o.alpha = alpha;
	ops.push_back(o);
}

// Runs drawing steps on the CPU.
static void RunOps(Buffer32& b, const std::vector<DrawOp>& ops)
{
	for (const DrawOp& o : ops)
		if (o.image)
			DrawLevels(b, *o.levels, o.ix, o.iy, o.iw, o.ih, o.x0, o.y0, o.x1, o.y1, o.alpha, o.rot);
		else
			FillRect(b, o.x0, o.y0, o.x1, o.y1, o.argb);
}

// The GPU draws the reference layer when it composites (defined with it).
// Returns false when the CPU has to.
static bool GpuDrawRef(int w, int h, const std::vector<DrawOp>& ops);

// One view for a panel cell: the canvas as Classic would show it (that
// view's image placed by the camera) on a w x h overlay, and how many screen
// pixels it shows per pixel of the image.
struct PanelCell {
	const std::vector<Level>* levels = nullptr;
	double ix = 0, iy = 0, iw = 0, ih = 0, rot = 0, mag = 0;
};

static PanelCell PlanPanelCell(const OverlayState& st, int slot)
{
	PanelCell c;
	OverlayState vs;
	std::string path;
	if (slot < 0 || !MultiviewLayout(slot, st, vs, path))
		return c;
	const std::vector<Level>* levels = LevelsFor(path);
	if (!levels || !ImageRectFor(vs, (*levels)[0].w, (*levels)[0].h, c.ix, c.iy, c.iw, c.ih))
		return c;
	c.levels = levels;
	c.rot = vs.imgRot;
	c.mag = c.iw / (*levels)[0].w;
	return c;
}

// The panel's cells show the images at Classic's scale (build 191: fitted
// into the cell they showed smaller), less Zoom Out (build 222: a fixed
// share of the canvas's zoom, so they still follow it). Build 193 zoomed them
// out past 2.5 image pixels a screen pixel, which lost the zoom: wrong,
// taken out.
static double PanelScale(const PanelCell*, int)
{
	return 100.0 / gCellZoomOut;
}

// Draws a planned view into a cell at scale s about the middle of the part
// of the viewport beside the panel (where the model is worked on: the panel
// covers the rest), which goes to the cell's middle, so the model centred
// there has its reference centred in the cell.
static void DrawPanelCell(const PanelCell& c, double s, int w, int h, int x0, int y0, int cw, int ch, std::vector<DrawOp>& ops)
{
	FillOp(ops, x0, y0, x0 + cw, y0 + ch, 0xFF181818u);
	if (!c.levels)
		return;
	int vx0, vy0, vx1, vy1, px, py, pw, ph;
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	PanelRect(w, h, px, py, pw, ph);
	if (gPanelLeft)
		vx0 = px + pw;
	else
		vx1 = px;
	if (vx1 - vx0 < 1 || vy1 - vy0 < 1)
		return;
	const double ox = x0 + cw * 0.5 - (vx0 + vx1) * 0.5 * s, oy = y0 + ch * 0.5 - (vy0 + vy1) * 0.5 * s;
	ImageOp(ops, *c.levels, ox + c.ix * s, oy + c.iy * s, c.iw * s, c.ih * s, x0, y0, x0 + cw, y0 + ch, 255, c.rot);
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
	std::vector<DrawOp> ops;
	if (gMode == 1) {
		const uint32_t g = static_cast<uint32_t>(gBackGrey & 255);
		FillOp(ops, 0, 0, w, h, 0xFF000000u | g << 16 | g << 8 | g);
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		if (gMulti) {
			// The upper image's view above the lower's (ZPlugin > JRef >
			// Upper Image and Lower Image).
			const int hh = ph / 2;
			// Both cells from one reading of the rotation: each read its own,
			// and a turn's step between them put the two at different angles
			// (build 217; one reading from build 218).
			// Through a Persp turn ZBrush's pan and zoom from that rotation too
			// (OrbitPan).
			gAxesHold = true;
			gAxesHeldSet = false;
			OverlayState so = st;
			OrbitPan(so);
			const PanelCell cells[2] = {PlanPanelCell(so, gMultiA), PlanPanelCell(so, gMultiB)};
			gAxesHold = gOrbitOn = false;
			const double s = PanelScale(cells, 2);
			DrawPanelCell(cells[0], s, w, h, px, py, pw, hh, ops);
			DrawPanelCell(cells[1], s, w, h, px, py + hh, pw, ph - hh, ops);
			FillOp(ops, px, py + hh - 1, px + pw, py + hh + 1, 0xFF505050u);
		} else {
			gAxesHold = true;
			gAxesHeldSet = false;
			OverlayState so = st;
			OrbitPan(so);
			const PanelCell cell = PlanPanelCell(so, ActiveViewSlot());
			gAxesHold = gOrbitOn = false;
			DrawPanelCell(cell, PanelScale(&cell, 1), w, h, px, py, pw, ph, ops);
		}
	} else {
		// Classic. No image (none for this view): the document's background shows.
		FillOp(ops, 0, 0, w, h, 0);
		double x0, y0, dispW, dispH;
		if (levels && ImageRect(st, x0, y0, dispW, dispH))
			ImageOp(ops, *levels, x0, y0, dispW, dispH, 0, 0, w, h, st.alpha, st.imgRot);
	}
	if (!GpuDrawRef(w, h, ops))
		RunOps(gRefLayer, ops);
	return true;
}

// Shows a CPU-drawn overlay through the GPU's window (defined with it).
// Returns false when the overlay is a layered window drawn by the CPU.
static bool GpuPresentBuffer(const OverlayState& st, Buffer32& buf);

static void Present(HWND wnd, const OverlayState& st, Buffer32& buf, BYTE alpha)
{
	if (GpuPresentBuffer(st, buf))
		return;
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
	std::vector<RECT> dirty;      // the last frame's changed rectangles
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
		// The part of the rectangle on this monitor, in the monitor's pixels.
		const int left = sx > output.left ? sx : output.left, top = sy > output.top ? sy : output.top;
		const int right = sx + w < output.right ? sx + w : output.right, bottom = sy + h < output.bottom ? sy + h : output.bottom;
		// Nothing to do when only the mouse pointer moved (no new image) or
		// nothing under the overlay changed: hovering ZBrush's menus or
		// anything else on screen then costs no copy and no composite. Once
		// a frame of this rectangle has been taken; the first always is.
		const bool fresh = !staging || region.left != left || region.top != top || region.right != right || region.bottom != bottom;
		bool under = info.LastPresentTime.QuadPart != 0 || fresh;
		if (under && !fresh) {
			UINT need = 0;
			dirty.resize(64);
			HRESULT dr = dup->GetFrameDirtyRects(static_cast<UINT>(dirty.size() * sizeof(RECT)), dirty.data(), &need);
			if (dr == DXGI_ERROR_MORE_DATA) {
				dirty.resize(need / sizeof(RECT) + 1);
				dr = dup->GetFrameDirtyRects(static_cast<UINT>(dirty.size() * sizeof(RECT)), dirty.data(), &need);
			}
			UINT moveNeed = 0;
			DXGI_OUTDUPL_MOVE_RECT mv[1];
			const HRESULT mr = dup->GetFrameMoveRects(sizeof(mv), mv, &moveNeed);
			if (SUCCEEDED(dr) && SUCCEEDED(mr) && moveNeed == 0) {
				under = false;
				for (UINT k = 0; k < need / sizeof(RECT); ++k) {
					const RECT& d = dirty[k];
					if (d.left + output.left < right && d.right + output.left > left && d.top + output.top < bottom && d.bottom + output.top > top) {
						under = true;
						break;
					}
				}
			}
		}
		if (!under) {
			res->Release();
			dup->ReleaseFrame();
			return 0;
		}
		ID3D11Texture2D* frame = nullptr;
		res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frame));
		res->Release();
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

// Capture of ZBrush's own window (Windows Graphics Capture): it shows ZBrush
// without the overlay, so the overlay need not be hidden from screen capture
// (screenshots show it). Frames come when ZBrush draws.
struct WindowCapture {
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	ID3D11Texture2D* staging = nullptr;
	int stagingW = 0, stagingH = 0;
	HWND target = nullptr;
	HANDLE arrived = nullptr;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rtDevice{nullptr};
	winrt::Windows::Graphics::SizeInt32 size{0, 0};

	void Release()
	{
		try {
			if (session)
				session.Close();
			if (pool)
				pool.Close();
		} catch (...) {
		}
		session = nullptr;
		pool = nullptr;
		item = nullptr;
		rtDevice = nullptr;
		if (staging) staging->Release();
		if (context) context->Release();
		if (device) device->Release();
		staging = nullptr;
		context = nullptr;
		device = nullptr;
		stagingW = stagingH = 0;
	}

	bool Init(HWND wnd)
	{
		using namespace winrt::Windows::Graphics::Capture;
		using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
		Release();
		target = wnd;
		if (!arrived)
			arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (...) {
		}
		try {
			if (!GraphicsCaptureSession::IsSupported())
				return false;
			if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
					D3D11_SDK_VERSION, &device, nullptr, &context))) {
				Release();
				return false;
			}
			IDXGIDevice* dxgi = nullptr;
			device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
			winrt::com_ptr<::IInspectable> insp;
			const HRESULT hr = dxgi ? CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put()) : E_FAIL;
			if (dxgi)
				dxgi->Release();
			if (FAILED(hr)) {
				Release();
				return false;
			}
			rtDevice = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
			auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
			if (FAILED(interop->CreateForWindow(wnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item))) || !item) {
				Release();
				return false;
			}
			size = item.Size();
			pool = Direct3D11CaptureFramePool::CreateFreeThreaded(rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
			HANDLE ev = arrived;
			pool.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
			session = pool.CreateCaptureSession(item);
			try {
				session.IsCursorCaptureEnabled(false);
			} catch (...) {
			}
			// Every frame ZBrush draws (Windows 11 24H2 and later limit the
			// rate otherwise; build 112 saw at most 32 a second).
			try {
				session.MinUpdateInterval(std::chrono::milliseconds(1));
			} catch (...) {
			}
			// No yellow border around ZBrush while it is captured.
			try {
				GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
				session.IsBorderRequired(false);
			} catch (...) {
			}
			session.StartCapture();
			return true;
		} catch (...) {
			Release();
			return false;
		}
	}

	// As Duplicator::Grab: waits up to timeout ms for a new frame of ZBrush's
	// window and copies the screen rectangle (sx, sy, w, h) from it; into
	// gpuDst too (a w x h texture on this device) when the window covers it
	// all, setting *gpuCopied.
	int Grab(int sx, int sy, int w, int h, Buffer32& dst, DWORD timeout, ID3D11Texture2D* gpuDst = nullptr, bool* gpuCopied = nullptr)
	{
		using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
		if (!session)
			return -1;
		if (!IsWindow(target)) {
			Release();
			return -1;
		}
		try {
			winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
			for (int pass = 0; pass < 2 && !frame; ++pass) {
				// The latest frame: older ones waiting are dropped.
				for (auto next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame()) {
					if (frame)
						frame.Close();
					frame = next;
				}
				if (!frame && pass == 0)
					WaitForSingleObject(arrived, timeout);
			}
			if (!frame)
				return 0;
			const auto cs = frame.ContentSize();
			if (cs.Width != size.Width || cs.Height != size.Height) {
				size = cs;
				pool.Recreate(rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
			}
			auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
			winrt::com_ptr<ID3D11Texture2D> tex;
			access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
			// The frame is the window's visible bounds.
			RECT wr;
			if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr))))
				GetWindowRect(target, &wr);
			D3D11_TEXTURE2D_DESC td;
			tex->GetDesc(&td);
			const int fw = std::min(static_cast<int>(td.Width), cs.Width), fh = std::min(static_cast<int>(td.Height), cs.Height);
			const int left = std::max(sx, static_cast<int>(wr.left)), top = std::max(sy, static_cast<int>(wr.top));
			const int right = std::min(sx + w, static_cast<int>(wr.left) + fw), bottom = std::min(sy + h, static_cast<int>(wr.top) + fh);
			int result = 0;
			if (right > left && bottom > top) {
				const int cw = right - left, ch = bottom - top;
				if (!staging || stagingW != cw || stagingH != ch) {
					if (staging)
						staging->Release();
					staging = nullptr;
					D3D11_TEXTURE2D_DESC sd = {};
					sd.Width = cw;
					sd.Height = ch;
					sd.MipLevels = 1;
					sd.ArraySize = 1;
					sd.Format = td.Format;
					sd.SampleDesc.Count = 1;
					sd.Usage = D3D11_USAGE_STAGING;
					sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					device->CreateTexture2D(&sd, nullptr, &staging);
					stagingW = cw;
					stagingH = ch;
				}
				if (staging) {
					D3D11_BOX box = {static_cast<UINT>(left - wr.left), static_cast<UINT>(top - wr.top), 0,
						static_cast<UINT>(right - wr.left), static_cast<UINT>(bottom - wr.top), 1};
					context->CopySubresourceRegion(staging, 0, 0, 0, 0, tex.get(), 0, &box);
					if (gpuDst && gpuCopied && cw == w && ch == h) {
						context->CopySubresourceRegion(gpuDst, 0, 0, 0, 0, tex.get(), 0, &box);
						*gpuCopied = true;
					}
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
			frame.Close();
			return result;
		} catch (...) {
			Release();
			return -1;
		}
	}
};

static WindowCapture gWgc;
static bool gUseWgc = false;  // the overlay captures ZBrush's window, not the screen
// The GPU compositor's frame texture (GpuOverlay keeps these): on the window
// capture's device, the capture copies each frame into it there, so the
// frame never goes up from the CPU.
static ID3D11Device* gGpuDev = nullptr;
static ID3D11Texture2D* gGpuCapTex = nullptr;
static int gGpuCapW = 0, gGpuCapH = 0;
static bool gGpuCapFresh = false;
static double gMagentaShare = 0;  // percent of the overlay that was background
// The document's background colour on screen, per overlay row (ZBrush's
// background is flat or a vertical gradient), learned from the frames
// themselves (LearnRowBackground): a pixel of that colour inside the
// viewport is background, and the reference replaces it. ZBrush's own
// background is left as it is, so nothing of the overlay is saved with a
// project. 0 for a row not learned yet; the colour has 0xFF000000 set.
static std::vector<uint32_t> gRowBg;
static DWORD gRowBgAt = 0;
// The viewport as the calibration capture showed it (where the pattern was
// seen), applied when the overlay is placed; the viewport is not grown
// from the background then.
static int gCalVX0 = 0, gCalVY0 = 0, gCalVX1 = 0, gCalVY1 = 0;
static bool gViewFixed = false;
static double gModelPixels = 0;   // pixels of the overlay that were not
static int gCaptureMethod = 0;  // 2 ZBrush's window, 1 desktop duplication, 0 BitBlt
static double gFps = 0;

// Fills gCapture with the screen under the overlay. Returns true if it may
// have changed since the last call.
static bool Capture(const OverlayState& st)
{
	const int w = gCapture.w, h = gCapture.h;
	const int sx = static_cast<int>(st.screenX), sy = static_cast<int>(st.screenY);
	if (gUseWgc) {
		const bool onGpu = gGpuCapTex && gGpuDev && gGpuDev == gWgc.device && gGpuCapW == w && gGpuCapH == h;
		bool copied = false;
		const int got = gWgc.Grab(sx, sy, w, h, gCapture, 8, onGpu ? gGpuCapTex : nullptr, &copied);
		if (got == 1)
			gGpuCapFresh = copied;  // the GPU's frame is this one too, or must be sent up
		if (got >= 0) {
			gCaptureMethod = 2;
			return got == 1;
		}
		// The window capture stopped: the screen is captured instead, which
		// needs the overlay hidden from it.
		gUseWgc = false;
		SetWindowDisplayAffinity(gOverlayWnd, 0x11);
		return false;
	}
	if (!gDup.dup && !gDup.failed) {
		if (!gDup.Init(sx + w / 2, sy + h / 2))
			gDup.failed = true;
	}
	if (gDup.dup) {
		const int got = gDup.Grab(sx, sy, w, h, gCapture, 8);
		if (got == 1)
			gGpuCapFresh = false;
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
	gGpuCapFresh = false;
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

struct Rgb {
	std::vector<unsigned char> px;  // B, G, R per pixel
	int w = 0, h = 0;
	const unsigned char* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
};

static bool OnMenu(int x, int y);
static int MenuAt(int x, int y);

// Whether ZBrush shows its canvas cursor (it draws the brush circle with it)
// rather than an interface one (over its palettes and menus, also where they
// lie over the canvas). The handles change every session (nav_log, build
// 128: canvas 600BFD and EC0467 alternating; over the JRef palette on the
// canvas 1BD0181, 2F0A51, 10013), so each is judged by its hotspot: a
// pointer's is near its tip, the canvas cursor's in the middle. No
// cursor (hidden while sculpting) counts as the canvas. Logged once each.
static bool CanvasCursor(HCURSOR cur)
{
	if (!cur)
		return true;
	// Also asked from the mouse hook (Adjust Image).
	static SRWLOCK lock = SRWLOCK_INIT;
	AcquireSRWLockExclusive(&lock);
	struct Release { ~Release() { ReleaseSRWLockExclusive(&lock); } } release;
	static std::vector<std::pair<HCURSOR, bool>> known;
	for (const auto& k : known)
		if (k.first == cur)
			return k.second;
	bool canvas = false;
	int hx = -1, hy = -1, cw = 0, ch = 0;
	ICONINFO ii = {};
	if (GetIconInfo(cur, &ii)) {
		hx = static_cast<int>(ii.xHotspot);
		hy = static_cast<int>(ii.yHotspot);
		BITMAP bm = {};
		if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm)) {
			cw = bm.bmWidth;
			ch = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
		}
		// In the middle half both ways (10013, used over the palette, has
		// its hotspot at 5 11 of 32 x 32).
		canvas = cw > 0 && ch > 0 && hx >= cw / 4 && hx <= cw * 3 / 4 && hy >= ch / 4 && hy <= ch * 3 / 4;
		if (ii.hbmMask)
			DeleteObject(ii.hbmMask);
		if (ii.hbmColor)
			DeleteObject(ii.hbmColor);
	}
	known.push_back({cur, canvas});
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu cursor %p: hotspot %d %d of %d x %d, %s\n", GetTickCount(), static_cast<void*>(cur), hx, hy, cw, ch,
			canvas ? "ZBrush's canvas cursor" : "an interface cursor (palettes, menus)");
		fclose(f);
	}
	return canvas;
}
static std::vector<RECT> gMenuBoxes;  // ZBrush's menus on the canvas (UpdateMenus)
// An open flyout (ZPlugin and the other menus): ZBrush lightens a rectangle
// behind it over its interface (56 to 65, 58 to 74: capture.png), seen round
// the canvas: the strip left of it and the band above. That rectangle is the
// flyout; ZBrush shows there, through the panel, until it goes. Opening one
// redraws the whole canvas at once (nav_log, build 129: "redraw ... 362179
// pixels"), so the menu's own pixels on the canvas cannot be told from it.
static RECT gFlyout = {0, 0, 0, 0};
// Where a flyout just closed: the canvas coming back there is no menu for a
// second (build 132: closed from the menu bar, cursor on the interface, the
// canvas reappearing under it became a menu, a hole in the panel).
static RECT gFlyoutGone = {0, 0, 0, 0};
static DWORD gFlyoutGoneAt = 0;

static bool InFlyout(int x, int y)
{
	return gFlyout.right > gFlyout.left && x >= gFlyout.left && x < gFlyout.right && y >= gFlyout.top && y < gFlyout.bottom;
}

// While the cursor is over the panel (not over a ZBrush menu on it), the
// panel steps aside: see-through, and ZBrush's view gizmo in front of it.
// Cursor changes are logged (the canvas cursor is the one most seen over the
// viewport outside the panel).
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
	int vx0, vy0, vx1, vy1;
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	const bool onCanvas = x >= vx0 && y >= vy0 && x < vx1 && y < vy1;
	const bool inPanel = onCanvas && x >= px && x < px + pw && y >= py && y < py + ph;
	const int menu = MenuAt(x, y);
	const bool canvasCursor = CanvasCursor(cur);
	// With an interface cursor over the panel, ZBrush has a menu there
	// (build 128 went see-through with the cursor on the JRef palette).
	const bool flyout = InFlyout(x, y);
	const bool onPanel = inPanel && menu < 0 && canvasCursor && !flyout;
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
	if (gPanelAside != before)
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			if (gPanelAside)
				fprintf(f, "%lu panel see-through: cursor at %d %d on the panel (%d %d to %d %d), no menu there\n", GetTickCount(), x, y, px, py, px + pw, py + ph);
			else if (inPanel && flyout)
				fprintf(f, "%lu panel solid: cursor at %d %d in the open flyout [%ld %ld to %ld %ld]\n", GetTickCount(), x, y,
					gFlyout.left, gFlyout.top, gFlyout.right, gFlyout.bottom);
			else if (inPanel && menu >= 0)
				fprintf(f, "%lu panel solid: cursor at %d %d on menu %d of %zu [%ld %ld to %ld %ld] over the panel\n", GetTickCount(), x, y, menu,
					gMenuBoxes.size(), gMenuBoxes[menu].left, gMenuBoxes[menu].top, gMenuBoxes[menu].right, gMenuBoxes[menu].bottom);
			else if (inPanel)
				fprintf(f, "%lu panel solid: cursor at %d %d over the panel shows an interface cursor (%p): a ZBrush menu there\n", GetTickCount(), x, y,
					static_cast<void*>(cur));
			else
				fprintf(f, "%lu panel solid: cursor at %d %d off the panel (%d %d to %d %d)\n", GetTickCount(), x, y, px, py, px + pw, py + ph);
			fclose(f);
		}
	// Only that: guessing when a ZBrush menu was open (by the cursor's shape,
	// build 115; by where the cursor left the canvas, builds 116 and 117) hid
	// the panel when it should have shown.
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

// ZBrush's menus over the panel's part of the canvas. A flyout list has no
// background over the canvas (its text is drawn straight onto the model;
// capture.png of a ZPlugin menu: the checker's 81 and 105 unchanged between
// the item names), only sub-palettes are filled, so no colour finds it (build
// 119 looked for flat runs and found strips). Instead: the whole viewport
// is kept, and what changes in it while the cursor is on ZBrush's interface
// (a menu opening; then items lighting, a sub-palette unfolding) is the menu,
// pixel by pixel, until it goes again or the canvas is clicked (which closes
// menus and is how the model changes). The panel draws under those pixels, so
// a menu's text shows over the reference. Build 120 kept only the panel's
// part, and moving the panel (Panel Left, pressed in the open menu) took the
// menu as the canvas. A model changed from the interface (a material) shows
// over the panel the same way until that click.
// A menu already open when the overlay starts (the one JRef was pressed in)
// is in the first copy: those pixels are found from the calibration capture,
// where they covered the pattern, and are still there after it ("baked").
// The boxes round clusters of menu pixels are the menus: a click in one is
// not a canvas click (build 121 took pressing Panel Left in the ZPlugin menu,
// over the canvas, for one and kept the menu as canvas), and the cursor in
// one is not on the panel (the panel stays solid behind it).
// Build 124 froze the copy while a menu was open and called every change
// since then the menu: ZBrush's brush circle following the cursor over the
// panel, the model and gizmo redrawing. The box grew to wherever the cursor
// had been (so the panel never went see-through there, and the gizmo could
// not be reached), model showed through as menu, and past 40% the lot was
// dropped, the real menu with it (nav_log: "too big for a menu, dropped"
// again and again). Now each pixel is decided as it changes: menu if the
// cursor is on ZBrush's interface or on a menu then, else the canvas (taken
// into the copy at once); a menu pixel back to the copy's value is gone.
static std::vector<uint32_t> gMenuBase;
static std::vector<unsigned char> gMenuBaked;
static int gMenuBakedCount = 0;
// The start menu's own pixels as found (where, and their colour): fewer than
// half still alike and it has closed. Build 135 compared them with the copy
// of the canvas, which other steps overwrite; a menu closed while Overlay
// settled stayed a hole in the panel.
static std::vector<std::pair<size_t, uint32_t>> gMenuBakedPixels;
static std::vector<unsigned char> gMenuMask;  // 1: a menu's pixel
static std::vector<int> gMenuRowCount;
static int gMenuMaskCount = 0;
static bool gMenuOpen = false;  // menu pixels (or the start menu) showing
static RECT gMenuBox = {0, 0, 0, 0};  // the start menu's box
// After the first copy, ZBrush redraws the canvas for a while (the pattern
// going, the model back), with the cursor on the interface (Overlay was just
// pressed): all canvas until it has been still 20 frames (at most 8 s).
// Build 127 took those redraws for a menu over the whole viewport, kept all
// session: holes in the model, the gizmo never reachable.
static bool gMenuSettling = false;
static int gMenuQuiet = 0;
static DWORD gMenuSettleAt = 0;
static Rgb gCalScreen;           // the calibration capture (pattern showing)
static Rgb gCalFrame;            // the pattern (frame0.png)
static int gCalScreenX = 0, gCalScreenY = 0;  // its top left on screen
static bool gCalPending = false;  // a new calibration not yet compared

static inline bool MenuPixel(size_t i, const uint32_t*)
{
	if (gMenuBakedCount && gMenuBaked[i])
		return true;
	return gMenuMaskCount && gMenuMask[i];
}

// Which menu box the point is in (16 pixels round each), or -1.
static int MenuAt(int x, int y)
{
	for (size_t k = 0; k < gMenuBoxes.size(); ++k) {
		const RECT& r = gMenuBoxes[k];
		if (x >= r.left - 16 && x < r.right + 16 && y >= r.top - 16 && y < r.bottom + 16)
			return static_cast<int>(k);
	}
	return -1;
}

static bool OnMenu(int x, int y)
{
	return MenuAt(x, y) >= 0;
}

// How long UpdateMenus's parts took last frame, in ms (for the log): the
// flyout and cursor checks, the changes against the copy, the boxes.
static double gMenuMs[3] = {0};

static bool MenuOverEmptyCanvas(const OverlayState& st, const unsigned char* key, int w, int h, int vx0, int vy0, int vx1, int vy1,
	std::vector<unsigned char>& cand);

static void UpdateMenus(const OverlayState& st, int w, int h)
{
	LARGE_INTEGER partStart, partFreq;
	QueryPerformanceCounter(&partStart);
	QueryPerformanceFrequency(&partFreq);
	gMenuMs[0] = gMenuMs[1] = gMenuMs[2] = 0;
	auto partAt = [&](int k) {
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		gMenuMs[k] = 1000.0 * (t.QuadPart - partStart.QuadPart) / partFreq.QuadPart;
		partStart = t;
	};
	if (!gCapture.bits)
		return;
	const uint32_t* cap = reinterpret_cast<const uint32_t*>(gCapture.bits);
	const size_t all = static_cast<size_t>(w) * h;
	auto log = [&](const char* what, int n) {
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu menu %s (%d pixels; box %ld %ld to %ld %ld)\n", GetTickCount(), what, n, gMenuBox.left, gMenuBox.top, gMenuBox.right, gMenuBox.bottom);
			fclose(f);
		}
	};
	int vx0, vy0, vx1, vy1;
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	vx0 = std::max(0, vx0);
	vy0 = std::max(0, vy0);
	vx1 = std::min(w, vx1);
	vy1 = std::min(h, vy1);
	if (vx1 <= vx0 || vy1 <= vy0)
		return;
	// A pixel of the calibration capture showing the pattern (its own or a
	// neighbouring pattern pixel, for the scaled edges).
	const int sox = static_cast<int>(st.screenX) - gCalScreenX, soy = static_cast<int>(st.screenY) - gCalScreenY;
	auto calAt = [&](int x, int y) -> const unsigned char* {
		const int sx = x + sox, sy = y + soy;
		if (sx < 0 || sy < 0 || sx >= gCalScreen.w || sy >= gCalScreen.h)
			return nullptr;
		return gCalScreen.at(sx, sy);
	};
	auto showsPattern = [&](const unsigned char* c, int x, int y) {
		const int fx = static_cast<int>(static_cast<long long>(x) * gCalFrame.w / w), fy = static_cast<int>(static_cast<long long>(y) * gCalFrame.h / h);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx) {
				const int ax = fx + dx, ay = fy + dy;
				if (ax < 0 || ay < 0 || ax >= gCalFrame.w || ay >= gCalFrame.h)
					continue;
				const unsigned char* p = gCalFrame.at(ax, ay);
				if (abs(p[0] - c[0]) <= 8 && abs(p[1] - c[1]) <= 8 && abs(p[2] - c[2]) <= 8)
					return true;
			}
		return false;
	};
	if (gMenuBase.size() != all || gCalPending) {
		// The first copy, once the canvas is back (the pattern no longer
		// shows in most of a sample).
		if (gCalPending && gCalScreen.w && gCalFrame.w) {
			int pattern = 0, samples = 0;
			for (int y = vy0; y < vy1; y += 16)
				for (int x = vx0; x < vx1; x += 16) {
					const uint32_t v = cap[static_cast<size_t>(y) * w + x];
					const unsigned char c[3] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16)};
					++samples;
					pattern += showsPattern(c, x, y);
				}
			if (samples && pattern * 5 > samples)
				return;
		}
		gMenuBase.assign(cap, cap + all);
		gMenuBaked.assign(all, 0);
		gMenuBakedCount = 0;
		gMenuBakedPixels.clear();
		gMenuMask.assign(all, 0);
		gMenuRowCount.assign(h, 0);
		gMenuMaskCount = 0;
		gMenuBoxes.clear();
		gMenuOpen = false;
		gMenuBox = {0, 0, 0, 0};
		gMenuSettling = true;
		gMenuQuiet = 0;
		gMenuSettleAt = GetTickCount();
		if (gCalPending && gCalScreen.w && gCalFrame.w) {
			// Baked: covering the pattern in the calibration capture, the same
			// now, and so is a neighbour (a stray match is alone). Not the
			// view gizmo's corner (it is drawn over the canvas too).
			const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3), gizmoY = vy0 + (vx1 - gizmoX);
			std::vector<unsigned char> same(all, 0);
			for (int y = vy0; y < vy1; ++y)
				for (int x = vx0; x < vx1; ++x) {
					if (x >= gizmoX && y < gizmoY)
						continue;
					const unsigned char* c = calAt(x, y);
					if (!c || showsPattern(c, x, y))
						continue;
					const uint32_t v = cap[static_cast<size_t>(y) * w + x];
					same[static_cast<size_t>(y) * w + x] = abs(static_cast<int>(v & 255) - c[0]) <= 2 && abs(static_cast<int>(v >> 8 & 255) - c[1]) <= 2 &&
						abs(static_cast<int>(v >> 16 & 255) - c[2]) <= 2;
				}
			for (int y = vy0 + 1; y < vy1 - 1; ++y)
				for (int x = vx0 + 1; x < vx1 - 1; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (same[i] && (same[i - 1] || same[i + 1] || same[i - w] || same[i + w]))
						gMenuBaked[i] = 1;
				}
			// Only the menu: the largest cluster of 16-pixel blocks holding 8+
			// such pixels (a menu's text is dense). Build 122 kept matches
			// scattered over the whole viewport, its box was the viewport, and
			// every canvas click then counted as one on the menu.
			const int bw = (w + 15) / 16, bh = (h + 15) / 16;
			std::vector<int> blocks(static_cast<size_t>(bw) * bh, 0);
			for (int y = vy0; y < vy1; ++y)
				for (int x = vx0; x < vx1; ++x)
					blocks[static_cast<size_t>(y / 16) * bw + x / 16] += gMenuBaked[static_cast<size_t>(y) * w + x];
			std::vector<int> label(blocks.size(), 0);
			int best = 0, bestSize = 0, next = 0;
			for (size_t s = 0; s < blocks.size(); ++s) {
				if (blocks[s] < 8 || label[s])
					continue;
				++next;
				int size = 0;
				std::vector<size_t> stack = {s};
				label[s] = next;
				while (!stack.empty()) {
					const size_t b = stack.back();
					stack.pop_back();
					++size;
					const int bx = static_cast<int>(b % bw), by = static_cast<int>(b / bw);
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx) {
							const int nx = bx + dx, ny = by + dy;
							if (nx < 0 || ny < 0 || nx >= bw || ny >= bh)
								continue;
							const size_t nb = static_cast<size_t>(ny) * bw + nx;
							if (!label[nb] && blocks[nb] >= 8) {
								label[nb] = next;
								stack.push_back(nb);
							}
						}
				}
				if (size > bestSize) {
					bestSize = size;
					best = next;
				}
			}
			long bx0 = w, by0 = h, bx1 = -1, by1 = -1;
			for (int y = vy0; y < vy1; ++y)
				for (int x = vx0; x < vx1; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!gMenuBaked[i])
						continue;
					if (!best || label[static_cast<size_t>(y / 16) * bw + x / 16] != best) {
						gMenuBaked[i] = 0;
						continue;
					}
					++gMenuBakedCount;
					gMenuBakedPixels.push_back({i, cap[i] & 0xFFFFFFu});
					bx0 = std::min<long>(bx0, x);
					bx1 = std::max<long>(bx1, x + 1);
					by0 = std::min<long>(by0, y);
					by1 = y + 1;
				}
			if (gMenuBakedCount < 30 || static_cast<double>(bx1 - bx0) * (by1 - by0) > 0.4 * (vx1 - vx0) * (vy1 - vy0)) {
				gMenuBaked.clear();
				gMenuBakedCount = 0;
				gMenuBakedPixels.clear();
			} else {
				gMenuBox = {bx0, by0, bx1, by1};
				gMenuBoxes.assign(1, gMenuBox);
				gMenuOpen = true;
			}
			log("already open at the start", gMenuBakedCount);
		}
		gCalPending = false;
		return;
	}
	// A menu open when PureRef Mode comes on (pressed in the ZPlugin menu) is
	// already in the copy: it never changes, so it never became a menu, and
	// the panel drew over it (build 233: the start menu was not found at
	// Overlay, 0 pixels). It is found from what the screen shows: not the
	// background, yet no model in ZBrush's canvas depth, in blocks it fills
	// (a menu's backdrop is solid; the model's antialiased outline is a thin
	// ring). Taken as the start menu, it goes as that does.
	{
		static int seenMode = -1;
		static bool want = false;
		if (gMode == 1 && seenMode != 1)
			want = true;
		seenMode = gMode;
		const unsigned char* key = gKeyShare.size() == all ? gKeyShare.data() : nullptr;
		if (want && !gMenuSettling && gMode == 1) {
			want = false;
			int found = 0;
			std::vector<unsigned char> cand;
			if (!gMenuBakedCount && key && MenuOverEmptyCanvas(st, key, w, h, vx0, vy0, vx1, vy1, cand)) {
				const int bs = 16, bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
				std::vector<int> blocks(static_cast<size_t>(bw) * bh, 0);
				for (int y = vy0; y < vy1; ++y)
					for (int x = vx0; x < vx1; ++x)
						blocks[static_cast<size_t>(y / bs) * bw + x / bs] += cand[static_cast<size_t>(y) * w + x];
				// The largest connected area of blocks at least half filled.
				std::vector<int> label(blocks.size(), 0);
				int best = 0, bestSize = 0, next = 0;
				for (size_t s = 0; s < blocks.size(); ++s) {
					if (blocks[s] < bs * bs / 2 || label[s])
						continue;
					++next;
					int size = 0;
					std::vector<size_t> stack = {s};
					label[s] = next;
					while (!stack.empty()) {
						const size_t b = stack.back();
						stack.pop_back();
						++size;
						const int bx = static_cast<int>(b % bw), by = static_cast<int>(b / bw);
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx) {
								const int nx = bx + dx, ny = by + dy;
								if (nx < 0 || ny < 0 || nx >= bw || ny >= bh)
									continue;
								const size_t nb = static_cast<size_t>(ny) * bw + nx;
								if (!label[nb] && blocks[nb] >= bs * bs / 2) {
									label[nb] = next;
									stack.push_back(nb);
								}
							}
					}
					if (size > bestSize) {
						bestSize = size;
						best = next;
					}
				}
				// Its box, a block wider each way for the edges the blocks miss.
				long bx0 = w, by0 = h, bx1 = -1, by1 = -1;
				for (size_t s = 0; best && s < blocks.size(); ++s)
					if (label[s] == best) {
						const long bx = static_cast<long>(s % bw) * bs, by = static_cast<long>(s / bw) * bs;
						bx0 = std::min(bx0, bx - bs);
						by0 = std::min(by0, by - bs);
						bx1 = std::max(bx1, bx + 2 * bs);
						by1 = std::max(by1, by + 2 * bs);
					}
				bx0 = std::max<long>(bx0, vx0);
				by0 = std::max<long>(by0, vy0);
				bx1 = std::min<long>(bx1, vx1);
				by1 = std::min<long>(by1, vy1);
				if (best && bestSize >= 6 && static_cast<double>(bx1 - bx0) * (by1 - by0) <= 0.4 * (vx1 - vx0) * (vy1 - vy0)) {
					gMenuBaked.assign(all, 0);
					gMenuBakedPixels.clear();
					for (long y = by0; y < by1; ++y)
						for (long x = bx0; x < bx1; ++x) {
							const size_t i = static_cast<size_t>(y) * w + x;
							if (!cand[i])
								continue;
							gMenuBaked[i] = 1;
							gMenuBakedPixels.push_back({i, cap[i] & 0xFFFFFFu});
							++found;
						}
					gMenuBakedCount = found;
					gMenuBox = {bx0, by0, bx1, by1};
					gMenuBoxes.push_back(gMenuBox);
					gMenuOpen = true;
				}
			}
			log(cand.empty() ? "open when PureRef came on not looked for (no canvas depth)" : "open when PureRef came on", found);
		}
	}
	CURSORINFO ci = {sizeof(ci)};
	GetCursorInfo(&ci);
	const int cx = static_cast<int>(ci.ptScreenPos.x - st.screenX), cy = static_cast<int>(ci.ptScreenPos.y - st.screenY);
	bool onPanel = false;
	if (gMode == 1) {
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		onPanel = cx >= px && cx < px + pw && cy >= py && cy < py + ph;
	}
	const int menuAt = MenuAt(cx, cy);
	const bool inView = cx >= vx0 && cy >= vy0 && cx < vx1 && cy < vy1;
	const bool canvasCursor = CanvasCursor((ci.flags & CURSOR_SHOWING) ? ci.hCursor : nullptr);
	// On ZBrush's interface: outside the viewport, or over it with an
	// interface cursor (a palette or menu lying on the canvas).
	const bool onInterface = !inView || !canvasCursor;
	// A click with the canvas cursor anywhere on the viewport (over the panel
	// too: it is the canvas there) closes ZBrush's menus. Build 131 left out
	// the panel, and a closed ZPlugin menu stayed a hole in it.
	const bool onCanvas = inView && canvasCursor;
	const bool down = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
	const char* where = !inView ? "on the interface" : !canvasCursor ? "on a ZBrush menu (interface cursor)" : menuAt >= 0 ? "in a menu box" :
		onPanel ? "on the panel" : "on the canvas";
	// A flyout closed this frame: the start menu (JRef's palette, in the
	// ZPlugin flyout) went with it. Build 133 waited for half its pixels to
	// change, but taking a too-big menu as canvas had copied them over, and
	// its box stayed a hole in the panel until the panel moved.
	bool flyoutClosed = false;
	{
		// The flyout's backdrop round the canvas: pixels of the interface
		// lighter than the copy by 4 to 40, alike in all three channels.
		// Opened (3000+ such pixels, with the cursor on the interface, at
		// least 48 each way), it holds until under a quarter of the most seen
		// are left; meanwhile the copy of the interface is kept as it was.
		static std::vector<uint32_t> outBase;
		static bool open = false;
		static int peak = 0;
		auto outside = [&](auto&& fn) {
			for (int y = 0; y < h; ++y) {
				const size_t i0 = static_cast<size_t>(y) * w;
				if (y < vy0 || y >= vy1) {
					fn(i0, 0, w, y);
				} else {
					fn(i0, 0, vx0, y);
					fn(i0, vx1, w, y);
				}
			}
		};
		auto keep = [&]() {
			outside([&](size_t i0, int a, int b, int) {
				if (b > a)
					memcpy(&outBase[i0 + a], cap + i0 + a, static_cast<size_t>(b - a) * 4);
			});
		};
		if (outBase.size() != all) {
			outBase.assign(cap, cap + all);
		} else {
			int light = 0, dark = 0;
			// Lighter pixels per 8-pixel block, for the largest connected area.
			const int bs = 8, bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
			static std::vector<int> blocks;
			blocks.assign(static_cast<size_t>(bw) * bh, 0);
			outside([&](size_t i0, int a, int b, int y) {
				// Most of the interface does not change: skipped whole.
				if (b <= a || !memcmp(cap + i0 + a, &outBase[i0 + a], static_cast<size_t>(b - a) * 4))
					return;
				for (int x = a; x < b; ++x) {
					const uint32_t c = cap[i0 + x], o = outBase[i0 + x];
					const int d0 = static_cast<int>(c & 255) - static_cast<int>(o & 255);
					const int d1 = static_cast<int>(c >> 8 & 255) - static_cast<int>(o >> 8 & 255);
					const int d2 = static_cast<int>(c >> 16 & 255) - static_cast<int>(o >> 16 & 255);
					const int lo = std::min(d0, std::min(d1, d2)), hi = std::max(d0, std::max(d1, d2));
					if (lo >= 4 && hi <= 40 && hi - lo <= 6) {
						++light;
						++blocks[static_cast<size_t>(y / bs) * bw + x / bs];
					} else if (hi <= -4 && lo >= -40 && hi - lo <= 6) {
						++dark;
					}
				}
			});
			// The largest connected area of lighter blocks (touching blocks
			// join): the backdrop. Build 132 boxed every lighter pixel: a
			// button lighting far off stretched the ZPlugin flyout across the
			// window, and toolbar buttons lit under the cursor made one.
			long fx0 = w, fy0 = h, fx1 = -1, fy1 = -1;
			int areaLight = 0;
			if (light >= 3000) {
				static std::vector<char> seen;
				seen.assign(blocks.size(), 0);
				std::vector<size_t> stack;
				for (size_t s = 0; s < blocks.size(); ++s) {
					if (!blocks[s] || seen[s])
						continue;
					seen[s] = 1;
					stack.assign(1, s);
					int count = 0, b0 = bw, b1 = -1, c0 = bh, c1 = -1;
					while (!stack.empty()) {
						const size_t b = stack.back();
						stack.pop_back();
						count += blocks[b];
						const int bx = static_cast<int>(b % bw), by = static_cast<int>(b / bw);
						b0 = std::min(b0, bx);
						b1 = std::max(b1, bx);
						c0 = std::min(c0, by);
						c1 = std::max(c1, by);
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx) {
								const int nx = bx + dx, ny = by + dy;
								if (nx < 0 || ny < 0 || nx >= bw || ny >= bh)
									continue;
								const size_t nb = static_cast<size_t>(ny) * bw + nx;
								if (blocks[nb] && !seen[nb]) {
									seen[nb] = 1;
									stack.push_back(nb);
								}
							}
					}
					if (count > areaLight) {
						areaLight = count;
						fx0 = std::min<long>(w, static_cast<long>(b0) * bs);
						fx1 = std::min<long>(w, static_cast<long>(b1 + 1) * bs);
						fy0 = std::min<long>(h, static_cast<long>(c0) * bs);
						fy1 = std::min<long>(h, static_cast<long>(c1 + 1) * bs);
					}
				}
			}
			auto logFly = [&](const char* what) {
				if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
					fprintf(f, "%lu flyout %s (%d lighter pixels round the canvas, most %d; box %ld %ld to %ld %ld; cursor %s at %d %d)\n", GetTickCount(), what,
						light, peak, gFlyout.left, gFlyout.top, gFlyout.right, gFlyout.bottom, where, cx, cy);
					fclose(f);
				}
			};
			// Not while the canvas settles after Overlay (build 131 took the
			// interface changing then for a flyout over the whole window: the
			// panel see-through everywhere), nor one over half the window.
			const bool sane = static_cast<double>(fx1 - fx0) * (fy1 - fy0) <= 0.5 * w * h;
			if (!open) {
				// 8000+ lighter pixels in one area, 150 wide and 40 high (a lit
				// button is far smaller; the ZPlugin backdrop is 62000).
				if (!gMenuSettling && onInterface && areaLight >= 8000 && fx1 - fx0 >= 150 && fy1 - fy0 >= 40 && sane) {
					open = true;
					peak = areaLight;
				} else {
					// One open since before Overlay (its backdrop already in the
					// copy) closing: the interface round the canvas darkens.
					if (dark >= 8000 && gMenuBakedCount) {
						flyoutClosed = true;
						logFly("open since the start closed (the interface darkened)");
					}
					keep();
				}
			} else {
				peak = std::max(peak, areaLight);
				if (areaLight * 4 < peak) {
					// Its pixels on the canvas go with it: they never match the
					// copy again exactly (the canvas redrew under the menu).
					int cleared = 0;
					const int y0 = std::max(vy0, static_cast<int>(gFlyout.top)), y1 = std::min(vy1, static_cast<int>(gFlyout.bottom));
					const int x0 = std::max(vx0, static_cast<int>(gFlyout.left)), x1 = std::min(vx1, static_cast<int>(gFlyout.right));
					for (int y = y0; y < y1 && x1 > x0; ++y) {
						if (!gMenuRowCount[y])
							continue;
						const size_t i0 = static_cast<size_t>(y) * w;
						for (int x = x0; x < x1; ++x)
							if (gMenuMask[i0 + x]) {
								gMenuMask[i0 + x] = 0;
								--gMenuRowCount[y];
								--gMenuMaskCount;
								gMenuBase[i0 + x] = cap[i0 + x];
								++cleared;
							}
					}
					open = false;
					flyoutClosed = true;
					logFly("closed");
					gFlyoutGone = gFlyout;
					gFlyoutGoneAt = GetTickCount();
					gFlyout = {0, 0, 0, 0};
					if (cleared) {
						gMenuOpen = gMenuMaskCount > 0 || gMenuBakedCount > 0;
						gMenuBoxes.erase(std::remove_if(gMenuBoxes.begin(), gMenuBoxes.end(), [&](const RECT& r) {
							return r.left >= x0 - 24 && r.right <= x1 + 24 && r.top >= y0 - 24 && r.bottom <= y1 + 24;
						}), gMenuBoxes.end());
						log("pixels of the closed flyout taken as canvas", cleared);
					}
					keep();
				}
			}
			if (open) {
				// Seen only in the band above the canvas (a menu over the
				// middle of it): it reaches down the canvas, how far unknown,
				// so to its bottom.
				RECT r = {fx0, fy0, fx1, fy1};
				if (fy1 <= vy0 + 2)
					r.bottom = vy1;
				const bool was = gFlyout.right > gFlyout.left;
				const bool moved = abs(r.left - gFlyout.left) > 8 || abs(r.right - gFlyout.right) > 8 || abs(r.bottom - gFlyout.bottom) > 8;
				gFlyout = r;
				if (!was)
					logFly("opened");
				else if (moved)
					logFly("changed");
			}
		}
	}
	auto sync = [&]() {
		// The copy takes the capture wherever they differ.
		for (int y = vy0; y < vy1; ++y) {
			const size_t i0 = static_cast<size_t>(y) * w + vx0;
			if (memcmp(cap + i0, &gMenuBase[i0], static_cast<size_t>(vx1 - vx0) * 4))
				memcpy(&gMenuBase[i0], cap + i0, static_cast<size_t>(vx1 - vx0) * 4);
		}
	};
	auto clearMask = [&]() {
		if (gMenuMaskCount)
			for (int y = vy0; y < vy1; ++y)
				if (gMenuRowCount[y]) {
					memset(&gMenuMask[static_cast<size_t>(y) * w], 0, w);
					gMenuRowCount[y] = 0;
				}
		gMenuMaskCount = 0;
	};
	bool unbaked = false;  // the boxes are worked out again
	auto unbake = [&](const char* why, int gone) {
		if (!gMenuBakedCount)
			return;
		unbaked = true;
		// What showed there now is the canvas.
		for (int y = vy0; y < vy1; ++y)
			for (int x = vx0; x < vx1; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (gMenuBaked[i])
					gMenuBase[i] = cap[i];
			}
		gMenuBaked.clear();
		gMenuBakedCount = 0;
		gMenuBakedPixels.clear();
		gMenuBox = {0, 0, 0, 0};
		log(why, gone);
	};
	static size_t boxesLogged = 0;
	// ZBrush shows its canvas cursor inside a menu box: there is no menu
	// there (over its menus it shows an interface cursor), and the box goes
	// (its pixels taken as canvas, the start menu with it if it is that one).
	// Not in an open flyout, where ZBrush shows the canvas cursor over the
	// list's empty part. Build 136 kept a start menu whose pixels never
	// changed (2859 of 2877 still as found long after): something on the
	// canvas, alike in the calibration capture, not JRef's palette.
	if (inView && canvasCursor && !InFlyout(cx, cy))
		for (size_t k = 0; k < gMenuBoxes.size(); ++k) {
			const RECT r = gMenuBoxes[k];
			if (cx < r.left || cx >= r.right || cy < r.top || cy >= r.bottom)
				continue;
			int cleared = 0;
			for (int y = std::max(vy0, static_cast<int>(r.top)); y < std::min(vy1, static_cast<int>(r.bottom)); ++y) {
				if (!gMenuRowCount[y])
					continue;
				const size_t i0 = static_cast<size_t>(y) * w;
				for (int x = std::max(vx0, static_cast<int>(r.left)); x < std::min(vx1, static_cast<int>(r.right)); ++x)
					if (gMenuMask[i0 + x]) {
						gMenuMask[i0 + x] = 0;
						--gMenuRowCount[y];
						--gMenuMaskCount;
						gMenuBase[i0 + x] = cap[i0 + x];
						++cleared;
					}
			}
			if (gMenuBakedCount && gMenuBox.left < r.right && gMenuBox.right > r.left && gMenuBox.top < r.bottom && gMenuBox.bottom > r.top)
				unbake("start menu dropped: ZBrush shows its canvas cursor there", 0);
			gMenuBoxes.erase(gMenuBoxes.begin() + k);
			boxesLogged = gMenuBoxes.size();
			gMenuOpen = gMenuMaskCount > 0 || gMenuBakedCount > 0;
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu menu box [%ld %ld to %ld %ld] dropped: ZBrush shows its canvas cursor in it at %d %d (%d pixels taken as canvas)\n",
					GetTickCount(), r.left, r.top, r.right, r.bottom, cx, cy, cleared);
				fclose(f);
			}
			break;
		}
	if (onCanvas && down) {
		if (gMenuMaskCount || gMenuBakedCount)
			log("closed (canvas clicked)", gMenuMaskCount);
		unbake("start menu gone (canvas clicked)", 0);
		clearMask();
		sync();
		gMenuBoxes.clear();
		boxesLogged = 0;
		gMenuOpen = false;
		return;
	}
	// Menu pixels grow only while the cursor is on ZBrush's interface
	// (outside the viewport, or showing an interface cursor over it: a menu
	// opening, a tooltip, items lighting, a sub-palette unfolding). Not:
	// - a pixel taken as canvas in the last 45 frames changing back (build 125
	//   left rings: the brush circle went into the copy while the cursor was
	//   on the panel, and its going, with the cursor then on the interface,
	//   became a menu that never matched the copy again);
	// - a change of 12 levels or less on the model (ZBrush redrawing it after
	//   a button press; on the background it counts: build 127 lost a
	//   tooltip's dark body that way, only its outline showed);
	// - anything while the canvas settles after the first copy;
	// - the view gizmo's corner (build 125 put the head in front of the
	//   panel after PureRef was pressed).
	// More than half the viewport added in one frame is a redraw: those are
	// taken as canvas, the menus found before are kept (build 125 threw them
	// all away past a quarter, and the panel covered the submenu).
	static std::vector<uint16_t> absorbedAt;
	static uint16_t frame = 0;
	++frame;
	if (absorbedAt.size() != all)
		absorbedAt.assign(all, static_cast<uint16_t>(frame - 1000));
	// Build 128 also grew menus with the canvas cursor inside a menu box,
	// and ZBrush draws its brush circle over a list's empty part: rings.
	// Only ZBrush's interface cursor grows them now. Not during a drag that
	// started on the canvas, nor for 300 ms after it (ZBrush's last redraws
	// of it): ZBrush holds the mouse while it pans, turns or sculpts, so no
	// menu can open, though the cursor goes onto the interface or off the
	// window. Builds 125-185 took the model moving under it then for menus,
	// blocks over the panel until the cursor passed over them.
	const bool dragging = gGesture.active || GetTickCount() - gDragEndAt < 300;
	const bool rootGrow = onInterface && !dragging;
	const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3);
	const int gizmoY = std::max(gGizmoBottom, vy0 + (vx1 - gizmoX));
	int added = 0, gone = 0, absorbed = 0, bakedGone = 0, recent = 0, faint = 0;
	const unsigned char* keyShare = gKeyShare.size() == all ? gKeyShare.data() : nullptr;
	// A flyout closed in the last second: the canvas coming back where it was.
	const bool goneRecent = gFlyoutGone.right > gFlyoutGone.left && GetTickCount() - gFlyoutGoneAt < 1000;
	// Where ZBrush's canvas cursor was last, and when: for half a second after
	// it leaves, changes within 200 pixels are its brush circle going (build
	// 134: leaving the panel over its top edge left a gap there, a menu box
	// round what was left of the circle, until the cursor came back).
	static int lastCanvasX = 0, lastCanvasY = 0;
	static DWORD lastCanvasAt = 0;
	if (inView && canvasCursor) {
		lastCanvasX = cx;
		lastCanvasY = cy;
		lastCanvasAt = GetTickCount();
	}
	const bool justLeft = !(inView && canvasCursor) && GetTickCount() - lastCanvasAt < 500;
	partAt(0);
	// Rows are independent, so they are done in bands on every core, each with
	// its own counts (the whole canvas changes when ZBrush redraws, and on one
	// core that took 5 to 11 ms a frame: nav_log, build 145).
	std::atomic<int> sumAdded(0), sumGone(0), sumAbsorbed(0), sumBakedGone(0), sumRecent(0), sumFaint(0), sumMask(0);
	// Nothing can become a menu this frame (the cursor on the canvas, or the
	// canvas settling): a row without menu pixels takes every change as
	// canvas in one tight pass and a row copy, the usual case while turning
	// or zooming the model.
	const bool mayAny = !gMenuSettling && rootGrow;
	ParallelRows(vy0, vy1, [&](int ya, int yb) {
	int bAdded = 0, bGone = 0, bAbsorbed = 0, bBakedGone = 0, bRecent = 0, bFaint = 0, bMask = 0;
	for (int y = ya; y < yb; ++y) {
		const size_t i0 = static_cast<size_t>(y) * w;
		if (!memcmp(cap + i0 + vx0, &gMenuBase[i0 + vx0], static_cast<size_t>(vx1 - vx0) * 4)) {
			if (gMenuRowCount[y]) {
				bGone += gMenuRowCount[y];
				bMask -= gMenuRowCount[y];
				gMenuRowCount[y] = 0;
				memset(&gMenuMask[i0], 0, w);
			}
			continue;
		}
		if (!mayAny && !gMenuRowCount[y] && !gMenuBakedCount) {
			const uint32_t* c = cap + i0;
			const uint32_t* b = &gMenuBase[i0];
			uint16_t* at = &absorbedAt[i0];
			int n = 0;
			for (int x = vx0; x < vx1; ++x)
				if ((c[x] ^ b[x]) & 0xFFFFFFu) {
					at[x] = frame;
					++n;
				}
			memcpy(&gMenuBase[i0 + vx0], c + vx0, static_cast<size_t>(vx1 - vx0) * 4);
			bAbsorbed += n;
			continue;
		}
		for (int x = vx0; x < vx1; ++x) {
			const size_t i = i0 + x;
			const uint32_t c = cap[i], b = gMenuBase[i];
			const bool diff = ((c ^ b) & 0xFFFFFFu) != 0;
			if (gMenuBakedCount && gMenuBaked[i]) {
				bBakedGone += diff;
				continue;
			}
			if (diff) {
				if (gMenuMask[i])
					continue;
				const bool may = !gMenuSettling && rootGrow &&
					!(goneRecent && x >= gFlyoutGone.left && x < gFlyoutGone.right && y >= gFlyoutGone.top && y < gFlyoutGone.bottom) &&
					!(justLeft && abs(x - lastCanvasX) < 200 && abs(y - lastCanvasY) < 200);
				const bool onModel = keyShare && keyShare[i] < 128;
				const bool big = !onModel || abs(static_cast<int>(c & 255) - static_cast<int>(b & 255)) > 12 ||
					abs(static_cast<int>(c >> 8 & 255) - static_cast<int>(b >> 8 & 255)) > 12 ||
					abs(static_cast<int>(c >> 16 & 255) - static_cast<int>(b >> 16 & 255)) > 12;
				const bool wasCanvas = static_cast<uint16_t>(frame - absorbedAt[i]) < 45;
				if (may && big && !wasCanvas && !(x >= gizmoX && y < gizmoY)) {
					gMenuMask[i] = 3;  // added this frame
					++gMenuRowCount[y];
					++bMask;
					++bAdded;
				} else {
					gMenuBase[i] = c;
					absorbedAt[i] = frame;
					++bAbsorbed;
					bRecent += may && wasCanvas;
					bFaint += may && !big;
				}
			} else if (gMenuMask[i]) {
				gMenuMask[i] = 0;
				--gMenuRowCount[y];
				--bMask;
				++bGone;
			}
		}
	}
	sumAdded += bAdded;
	sumGone += bGone;
	sumAbsorbed += bAbsorbed;
	sumBakedGone += bBakedGone;
	sumRecent += bRecent;
	sumFaint += bFaint;
	sumMask += bMask;
	});
	added += sumAdded;
	gone += sumGone;
	absorbed += sumAbsorbed;
	bakedGone += sumBakedGone;
	recent += sumRecent;
	faint += sumFaint;
	gMenuMaskCount += sumMask;
	partAt(1);
	// This frame's additions: kept, or a redraw taken as canvas.
	// Half the viewport: a flyout's see-through backdrop over the canvas is
	// a quarter or so (build 129 called the ZPlugin menu, 253277 pixels, a
	// redraw at a tenth and covered it).
	const bool redraw = added * 2.0 > static_cast<double>(vx1 - vx0) * (vy1 - vy0);
	if (added)
		for (int y = vy0; y < vy1; ++y) {
			if (!gMenuRowCount[y])
				continue;
			const size_t i0 = static_cast<size_t>(y) * w;
			for (int x = vx0; x < vx1; ++x)
				if (gMenuMask[i0 + x] == 3) {
					if (redraw) {
						gMenuMask[i0 + x] = 0;
						--gMenuRowCount[y];
						--gMenuMaskCount;
						gMenuBase[i0 + x] = cap[i0 + x];
						absorbedAt[i0 + x] = frame;
					} else {
						gMenuMask[i0 + x] = 1;
					}
				}
		}
	if (redraw) {
		log("redraw (half the viewport changed at once) taken as canvas", added);
		added = 0;
	}
	if (gMenuSettling) {
		gMenuQuiet = absorbed < 100 ? gMenuQuiet + 1 : 0;
		// At most 8 s (build 136 stopped at 3 with ZBrush still redrawing, and
		// took some of it for menus).
		if (gMenuQuiet >= 20 || GetTickCount() - gMenuSettleAt > 8000) {
			gMenuSettling = false;
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu menus: canvas settled after %lu ms (%s)\n", GetTickCount(), GetTickCount() - gMenuSettleAt,
					gMenuQuiet >= 20 ? "still for 20 frames" : "8 s passed");
				fclose(f);
			}
		}
	}
	// The start menu closed: most of its pixels changed.
	if (gMenuBakedCount && bakedGone * 2 > gMenuBakedCount)
		unbake("start menu closed", bakedGone);
	else if (gMenuBakedCount && flyoutClosed)
		unbake("start menu closed with its flyout", bakedGone);
	static int bakedAlike = 0;
	if (gMenuBakedCount) {
		int& alike = bakedAlike;
		alike = 0;
		for (const auto& p : gMenuBakedPixels)
			alike += (cap[p.first] & 0xFFFFFFu) == p.second;
		if (alike * 2 < static_cast<int>(gMenuBakedPixels.size()))
			unbake("start menu gone (under half its pixels as found)", static_cast<int>(gMenuBakedPixels.size()) - alike);
	}
	// The start menu is the one Overlay was pressed in, and ZBrush closes it
	// when the cursor leaves: the cursor 48+ pixels outside its box means it
	// is gone. Build 137 kept one made of the model (alike in the calibration
	// capture, never changing) while the cursor sat below the viewport, a
	// hole in the panel until the cursor came back to it.
	if (gMenuBakedCount && (cx < gMenuBox.left - 48 || cx >= gMenuBox.right + 48 || cy < gMenuBox.top - 48 || cy >= gMenuBox.bottom + 48))
		unbake("start menu gone (the cursor left it)", 0);
	// A quarter of the viewport is not a menu but the model changing from
	// the interface (a material): it is the canvas.
	if (gMenuMaskCount * 4.0 > static_cast<double>(vx1 - vx0) * (vy1 - vy0)) {
		log("too many pixels for a menu: taken as the canvas", gMenuMaskCount);
		clearMask();
		sync();
	}
	gMenuOpen = gMenuMaskCount > 0 || gMenuBakedCount > 0;
	{
		// Now and then: what was taken as canvas, and why.
		static DWORD at = 0;
		static int sumAbsorbed = 0, sumRecent = 0, sumFaint = 0;
		sumAbsorbed += absorbed;
		sumRecent += recent;
		sumFaint += faint;
		if (GetTickCount() - at >= 2000 && (sumAbsorbed || gMenuOpen)) {
			at = GetTickCount();
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu menus: %zu showing, %d menu pixels, start menu %d (%d still as found); last 2 s %d taken as canvas (%d changing back from canvas, %d faint), cursor %s\n",
					at, gMenuBoxes.size(), gMenuMaskCount, gMenuBakedCount, gMenuBakedCount ? bakedAlike : 0, sumAbsorbed, sumRecent, sumFaint, where);
				fclose(f);
			}
			sumAbsorbed = sumRecent = sumFaint = 0;
		}
	}
	if (!added && !gone && !unbaked)
		return;
	// The boxes: clusters of 24-pixel blocks holding menu pixels (touching
	// blocks join, so a list's items make one box with its empty right part),
	// those with 30+ pixels and something that appeared from the interface
	// (or the start menu). The rest (grown on a menu that has gone) are
	// taken as canvas.
	const int bs = 24, bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
	std::vector<int> blocks(static_cast<size_t>(bw) * bh, 0), rootBlocks(blocks.size(), 0);
	if (gMenuMaskCount || gMenuBakedCount)
		for (int y = vy0; y < vy1; ++y) {
			if (!gMenuRowCount[y] && !gMenuBakedCount)
				continue;
			const size_t i0 = static_cast<size_t>(y) * w;
			for (int x = vx0; x < vx1; ++x) {
				const bool baked = gMenuBakedCount && gMenuBaked[i0 + x];
				if (gMenuMask[i0 + x] || baked) {
					const size_t s = static_cast<size_t>(y / bs) * bw + x / bs;
					++blocks[s];
					rootBlocks[s] += baked || gMenuMask[i0 + x] == 1;
				}
			}
		}
	std::vector<int> label(blocks.size(), 0);
	std::vector<RECT> boxes;
	std::vector<int> counts;
	std::vector<char> keep(1, 0);  // per label
	for (size_t s = 0; s < blocks.size(); ++s) {
		if (!blocks[s] || label[s])
			continue;
		const int id = static_cast<int>(keep.size());
		label[s] = id;
		std::vector<size_t> stack = {s};
		int count = 0, roots = 0, b0 = bw, b1 = -1, c0 = bh, c1 = -1;
		while (!stack.empty()) {
			const size_t b = stack.back();
			stack.pop_back();
			count += blocks[b];
			roots += rootBlocks[b];
			const int bx = static_cast<int>(b % bw), by = static_cast<int>(b / bw);
			b0 = std::min(b0, bx);
			b1 = std::max(b1, bx);
			c0 = std::min(c0, by);
			c1 = std::max(c1, by);
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx) {
					const int nx = bx + dx, ny = by + dy;
					if (nx < 0 || ny < 0 || nx >= bw || ny >= bh)
						continue;
					const size_t nb = static_cast<size_t>(ny) * bw + nx;
					if (blocks[nb] && !label[nb]) {
						label[nb] = id;
						stack.push_back(nb);
					}
				}
		}
		// Pixels over a quarter of the viewport are no menu (the canvas
		// redrawing). By its pixels, not its box: ZPlugin's palette popped up
		// over the canvas is a column the canvas's height whose box (joined to
		// what changed beside it) passed 40% of the viewport, and the panel was
		// drawn over it (build 192: 194351 and 206230 pixels, an eighth).
		const RECT box = {std::max(vx0, b0 * bs), std::max(vy0, c0 * bs), std::min(vx1, (b1 + 1) * bs), std::min(vy1, (c1 + 1) * bs)};
		const bool huge = count > 0.25 * (vx1 - vx0) * (vy1 - vy0);
		if (huge)
			log("box over a quarter of the viewport taken as canvas", count);
		keep.push_back(roots > 0 && !huge);
		if (count >= 30 && roots > 0 && !huge) {
			boxes.push_back(box);
			counts.push_back(count);
		}
	}
	int dropped = 0;
	for (int y = vy0; y < vy1; ++y) {
		if (!gMenuRowCount[y])
			continue;
		const size_t i0 = static_cast<size_t>(y) * w;
		for (int x = vx0; x < vx1; ++x)
			if (gMenuMask[i0 + x] && !keep[label[static_cast<size_t>(y / bs) * bw + x / bs]]) {
				gMenuMask[i0 + x] = 0;
				--gMenuRowCount[y];
				--gMenuMaskCount;
				gMenuBase[i0 + x] = cap[i0 + x];
				absorbedAt[i0 + x] = frame;
				++dropped;
			}
	}
	if (dropped)
		log("pixels left from a closed menu taken as canvas", dropped);
	gMenuOpen = gMenuMaskCount > 0 || gMenuBakedCount > 0;
	gMenuBoxes = boxes;
	partAt(2);
	// Logged when the boxes change in number: each box, its pixels, and what
	// this frame did (with the cursor where).
	if (boxes.size() != boxesLogged) {
		boxesLogged = boxes.size();
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu menus now %zu (menu pixels %d, start menu %d; this frame %d added, %d gone, %d taken as canvas; cursor %s at %d %d):",
				GetTickCount(), boxes.size(), gMenuMaskCount, gMenuBakedCount, added, gone, absorbed, where, cx, cy);
			for (size_t k = 0; k < boxes.size(); ++k)
				fprintf(f, " [%ld %ld to %ld %ld, %d]", boxes[k].left, boxes[k].top, boxes[k].right, boxes[k].bottom, counts[k]);
			fprintf(f, "\n");
			fclose(f);
		}
	}
}

static inline bool SameColour(uint32_t a, uint32_t b, int tol = 3)
{
	return abs(static_cast<int>(a & 255) - static_cast<int>(b & 255)) <= tol &&
		abs(static_cast<int>(a >> 8 & 255) - static_cast<int>(b >> 8 & 255)) <= tol &&
		abs(static_cast<int>(a >> 16 & 255) - static_cast<int>(b >> 16 & 255)) <= tol;
}

// Learns gRowBg from gCapture, twice a second, going down the viewport from
// the colour seen most over it (see below). A shaded model never fills a row
// with one colour, so it cannot become the background, and the background is
// right however ZBrush tints or graduates it, without ever capturing the
// canvas empty.
static void LearnRowBackground()
{
	const int w = gCapture.w, h = gCapture.h;
	const DWORD now = GetTickCount();
	if (gRowBg.size() == static_cast<size_t>(h) && now - gRowBgAt < 500)
		return;
	gRowBgAt = now;
	gRowBg.assign(h, 0);
	int vx0, vy0, vx1, vy1;
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	if (vx1 - vx0 < 8 || vy1 - vy0 < 8)
		return;
	const uint32_t* c = reinterpret_cast<const uint32_t*>(gCapture.bits);
	// Counts colours, the first 128 different ones seen (the background is
	// among the first in any row: it borders the viewport).
	std::vector<std::pair<uint32_t, int>> counts;
	size_t last = 0;  // the entry counted last: runs of one colour are quick
	auto count = [&](uint32_t p) {
		if (last < counts.size() && counts[last].first == p) {
			++counts[last].second;
			return;
		}
		for (size_t i = 0; i < counts.size(); ++i)
			if (counts[i].first == p) {
				++counts[i].second;
				last = i;
				return;
			}
		if (counts.size() < 128) {
			last = counts.size();
			counts.push_back({p, 1});
		}
	};
	auto most = [&](uint32_t& col) {
		int best = 0;
		for (const auto& e : counts)
			if (e.second > best) { best = e.second; col = e.first; }
		return best;
	};
	// The start: the colour seen most over the whole viewport (every 16th
	// pixel each way). ([IGet, Document:Back] does not give the colour: starting from
	// it, the rows crept through the model's greys and the top of the
	// viewport, depending on what the cursor and menus put there, kept no
	// background.)
	uint32_t ref = 0xFF303030u;
	for (int y = vy0; y < vy1; y += 16)
		for (int x = vx0; x < vx1; x += 16)
			count(c[static_cast<size_t>(y) * w + x] | 0xFF000000u);
	most(ref);
	// Every 4th pixel of a row is counted, so 4% of the row is a quarter as
	// many.
	const int need = std::max(2, (vx1 - vx0) / 100);
	// Counts by offset from the row above's colour, -12 to 12 per channel, so
	// a pixel costs one increment (searching a list of the model's shades
	// near the background took 9 ms, a dropped frame twice a second).
	static std::vector<int> hist(25 * 25 * 25, 0);
	static std::vector<int> touched;
	for (int y = vy0; y < vy1; ++y) {
		const uint32_t* row = c + static_cast<size_t>(y) * w;
		// The colour seen most within 12 of the row above's, if at least 4%
		// of the row has it; else the row above's carries on. (Not simply a
		// colour filling the row: ZBrush's frame line and tooltips do.)
		const int r0 = ref & 255, g0 = ref >> 8 & 255, b0 = ref >> 16 & 255;
		touched.clear();
		for (int x = vx0; x < vx1; x += 4) {
			const uint32_t p = row[x];
			const int dr = static_cast<int>(p & 255) - r0 + 12;
			const int dg = static_cast<int>(p >> 8 & 255) - g0 + 12;
			const int db = static_cast<int>(p >> 16 & 255) - b0 + 12;
			if (static_cast<unsigned>(dr) > 24 || static_cast<unsigned>(dg) > 24 || static_cast<unsigned>(db) > 24)
				continue;
			const int i = (db * 25 + dg) * 25 + dr;
			if (hist[i]++ == 0)
				touched.push_back(i);
		}
		int best = 0, bi = 0;
		for (int i : touched) {
			if (hist[i] > best) { best = hist[i]; bi = i; }
			hist[i] = 0;
		}
		if (best >= need)
			ref = 0xFF000000u | static_cast<uint32_t>(r0 + bi % 25 - 12) |
				static_cast<uint32_t>(g0 + bi / 25 % 25 - 12) << 8 |
				static_cast<uint32_t>(b0 + bi / 625 - 12) << 16;
		gRowBg[y] = ref;
	}
}

// Builds the opaque output from gCapture: the background is replaced by the
// reference (gRefLayer, where it has one).
// How long each stage of the last Composite took, in ms (for the log).
static double gStageMs[8] = {0};

// Runs fn(y0, y1) over rows y0 to y1 in bands, one per core (up to 8), and
// waits: the overlay's per-pixel work, so a frame takes a fraction of the
// time on one core. The runtime's worker threads stay between frames.
template <typename Fn>
static void ParallelRows(int y0, int y1, const Fn& fn)
{
	static const int cores = std::max(1, std::min(8, static_cast<int>(std::thread::hardware_concurrency())));
	const int bands = std::min(cores, std::max(1, (y1 - y0) / 64));
	if (bands <= 1) {
		fn(y0, y1);
		return;
	}
	concurrency::parallel_for(0, bands, [&](int b) { fn(y0 + (y1 - y0) * b / bands, y0 + (y1 - y0) * (b + 1) / bands); });
}

// A row's run of background-coloured pixels, x0 to x1 (past the end), and
// its region (union-find: parent; area and edge gathered in the root).
struct BgRun {
	int y, x0, x1, parent;
	size_t area;
	bool edge, keep;
};

// The GPU composites (defined with it): the CPU only finds the background
// and the menus, and GpuCompose draws the frame from them.
static bool GpuComposes();
static void GpuCompose(const OverlayState& st, const std::vector<DrawOp>& on);

static void Composite(HWND wnd, const OverlayState& st)
{
	const bool gpu = GpuComposes();
	LARGE_INTEGER compStart;  // for the log: how long a frame takes
	QueryPerformanceCounter(&compStart);
	LARGE_INTEGER stageAt = compStart, qpf;
	QueryPerformanceFrequency(&qpf);
	auto stage = [&](int k) {
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		gStageMs[k] = 1000.0 * (t.QuadPart - stageAt.QuadPart) / qpf.QuadPart;
		stageAt = t;
	};
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
	// Background: pixels of their row's background colour (within 1 per
	// channel) inside the viewport, with their four neighbours too, that
	// belong to a region of such pixels reaching the viewport's edge or
	// covering 1% of it. The model's shading can have the background's grey
	// (its darker parts often do); such patches are enclosed by the model's
	// other shades and stay model, where before the reference showed through
	// them as gaps.
	LearnRowBackground();
	stage(0);
	static std::vector<unsigned char> same, bg;
	static std::vector<BgRun> runs;
	// Not cleared: only the viewport is read, and every byte of it is
	// written first.
	same.resize(n);
	bg.resize(n);
	{
		int vx0, vy0, vx1, vy1;
		ViewportRect(w, h, vx0, vy0, vx1, vy1);
		const uint32_t* c = reinterpret_cast<const uint32_t*>(gCapture.bits);
		ParallelRows(vy0, vy1, [&](int ya, int yb) {
		for (int y = ya; y < yb; ++y) {
			const uint32_t b = gRowBg.size() == static_cast<size_t>(h) ? gRowBg[y] : 0;
			const uint32_t* row = c + static_cast<size_t>(y) * w;
			unsigned char* s = &same[static_cast<size_t>(y) * w];
			if (!b) {
				memset(s + vx0, 0, vx1 - vx0);
				continue;
			}
			// Within 1: ZBrush draws its background exactly, and its menu
			// panels are only 3 off it (51 on a 48 background). Four pixels
			// at a time: per byte |p - b| <= 1, for blue, green and red.
			const __m128i bv = _mm_set1_epi32(static_cast<int>(b)), one = _mm_set1_epi8(1), zero = _mm_setzero_si128();
			int x = vx0;
			for (; x + 4 <= vx1; x += 4) {
				const __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x));
				const __m128i d = _mm_or_si128(_mm_subs_epu8(p, bv), _mm_subs_epu8(bv, p));
				const int m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(d, one), zero));
				s[x] = (m & 0x7) == 0x7;
				s[x + 1] = (m & 0x70) == 0x70;
				s[x + 2] = (m & 0x700) == 0x700;
				s[x + 3] = (m & 0x7000) == 0x7000;
			}
			for (; x < vx1; ++x)
				s[x] = SameColour(row[x], b, 1);
		}
		});
		// With all four neighbours (those outside the viewport count): thin
		// bridges of the background's colour, as along the model's
		// antialiased outline, are cut, so a region cannot leak into the
		// model through them. Plain byte loops, which the compiler vectorises.
		static std::vector<unsigned char> ones;
		ones.assign(w, 1);
		ParallelRows(vy0, vy1, [&](int ya, int yb) {
		for (int y = ya; y < yb; ++y) {
			const size_t r = static_cast<size_t>(y) * w;
			const unsigned char* s = &same[r];
			const unsigned char* up = y == vy0 ? ones.data() : s - w;
			const unsigned char* dn = y == vy1 - 1 ? ones.data() : s + w;
			unsigned char* o = &bg[r];
			for (int x = vx0 + 1; x < vx1 - 1; ++x)
				o[x] = s[x] & s[x - 1] & s[x + 1] & up[x] & dn[x];
			o[vx0] = s[vx0] & (vx0 + 1 < vx1 ? s[vx0 + 1] : 1) & up[vx0] & dn[vx0];
			o[vx1 - 1] = s[vx1 - 1] & (vx1 - 2 >= vx0 ? s[vx1 - 2] : 1) & up[vx1 - 1] & dn[vx1 - 1];
		}
		});
		stage(1);
		// Regions (4-connected), as runs: each row's background is a few
		// runs, and runs touching across rows are joined (union-find), so a
		// region costs its runs, not its pixels. A region is background if it
		// reaches the viewport's edge or covers 1% of it (ZBrush's frame line
		// encloses most of the canvas, so most of it is the second kind).
		runs.clear();
		auto root = [&](int r) {
			while (runs[r].parent != r) {
				runs[r].parent = runs[runs[r].parent].parent;
				r = runs[r].parent;
			}
			return r;
		};
		size_t prevStart = 0, prevEnd = 0;
		for (int y = vy0; y < vy1; ++y) {
			const unsigned char* row = &bg[static_cast<size_t>(y) * w];
			const size_t curStart = runs.size();
			size_t p = prevStart;
			// Eight bytes at a time through long stretches of 0s or 1s.
			const uint64_t ones8 = 0x0101010101010101ull;
			for (int x = vx0; x < vx1;) {
				if (!row[x]) {
					uint64_t v;
					while (x + 8 <= vx1 && (memcpy(&v, row + x, 8), v == 0))
						x += 8;
					while (x < vx1 && !row[x])
						++x;
					continue;
				}
				const int x0 = x;
				uint64_t v;
				while (x + 8 <= vx1 && (memcpy(&v, row + x, 8), v == ones8))
					x += 8;
				while (x < vx1 && row[x])
					++x;
				const int id = static_cast<int>(runs.size());
				runs.push_back({y, x0, x, id, static_cast<size_t>(x - x0), x0 == vx0 || x == vx1 || y == vy0 || y == vy1 - 1, false});
				while (p < prevEnd && runs[p].x1 <= x0)
					++p;
				for (size_t q = p; q < prevEnd && runs[q].x0 < x; ++q) {
					const int a = root(static_cast<int>(q)), b = root(id);
					if (a != b)
						runs[std::max(a, b)].parent = std::min(a, b);
				}
			}
			prevStart = curStart;
			prevEnd = runs.size();
		}
		// Roots have the smallest index of their region, so one pass in
		// order gathers each region's area and edge into its root.
		for (size_t i = 0; i < runs.size(); ++i) {
			const int r = root(static_cast<int>(i));
			if (r != static_cast<int>(i)) {
				runs[r].area += runs[i].area;
				runs[r].edge = runs[r].edge || runs[i].edge;
			}
		}
		const size_t big = std::max<size_t>(2000, static_cast<size_t>(vx1 - vx0) * (vy1 - vy0) / 100);
		for (size_t i = 0; i < runs.size(); ++i) {
			const BgRun& r = runs[root(static_cast<int>(i))];
			runs[i].keep = r.edge || r.area >= big;
		}
	}
	stage(3);
	// The model everywhere first (a straight copy), then the background runs:
	// the reference (premultiplied) over the background, opaque or absent
	// almost everywhere, which needs no blending. In PureRef the background
	// colour (the panel is drawn below, over the model too).
	const uint32_t* cap32 = reinterpret_cast<const uint32_t*>(cap);
	const uint32_t* ref32 = reinterpret_cast<const uint32_t*>(ref);
	uint32_t* out32 = reinterpret_cast<uint32_t*>(out);
	// Runs are in row order, so each band of rows has its own runs and rows.
	std::atomic<size_t> keyed(0);
	ParallelRows(0, h, [&](int ya, int yb) {
	const size_t a0 = static_cast<size_t>(ya) * w, a1 = static_cast<size_t>(yb) * w;
	if (!gpu)
		for (size_t i = a0; i < a1; ++i)
			out32[i] = cap32[i] | 0xFF000000u;
	memset(&gKeyShare[a0], 0, a1 - a0);
	size_t mine = 0;
	auto first = std::lower_bound(runs.begin(), runs.end(), ya, [](const BgRun& r, int y) { return r.y < y; });
	for (auto it = first; it != runs.end() && it->y < yb; ++it) {
		const BgRun& run = *it;
		if (!run.keep)
			continue;
		const size_t i0 = static_cast<size_t>(run.y) * w;
		const int len = run.x1 - run.x0;
		memset(&gKeyShare[i0 + run.x0], 255, len);
		rowMagenta[run.y] += len;
		mine += len;
		for (int x = run.x0; x < run.x1 && !gpu; ++x) {
			const size_t i = i0 + x;
			const uint32_t rf = ref32[i];
			const unsigned a = rf >> 24;
			if (a == 255) {
				out32[i] = rf;
			} else if (a != 0) {
				const uint32_t c = cap32[i];
				const unsigned k = 255 - a;
				uint32_t o = 0xFF000000u;
				for (int s = 0; s < 24; s += 8) {
					const unsigned v = (rf >> s & 255) + ((c >> s & 255) * k + 127) / 255;
					o |= (v > 255 ? 255 : v) << s;
				}
				out32[i] = o;
			}
		}
	}
	keyed += mine;
	});
	magenta = keyed;
	// Per column, only for growing the viewport (one with a fixed viewport
	// never grows) and the log's spans.
	if (!gViewFixed)
		for (const BgRun& run : runs)
			if (run.keep)
				for (int x = run.x0; x < run.x1; ++x)
					++colMagenta[x];
	model = n - magenta;
	stage(4);
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
		// The slowest frame since the last stats line (to here, plus the
		// present before it), and how many went over 8 ms (a dropped frame at
		// 120 Hz).
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		// With the reference's redraw before it, when there was one.
		const double ms = 1000.0 * (now.QuadPart - compStart.QuadPart) / qpf.QuadPart + gStageMs[2];
		static double worstMs = 0, worstStage[8];
		static int frames = 0, slow = 0;
		++frames;
		if (ms + gStageMs[6] > 8.0)
			++slow;
		if (ms + gStageMs[6] > worstMs) {
			worstMs = ms + gStageMs[6];
			memcpy(worstStage, gStageMs, sizeof worstStage);
		}
		static DWORD statsAt = 0;
		if (GetTickCount() - statsAt >= 2000) {
			statsAt = GetTickCount();
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				// "reference" is redrawing the reference (a zoom, pan or setting
				// change), "menus" the last frame's menus, panel and Model Opacity
				// (of it: flyout and cursor checks, changes against the copy,
				// boxes), "present" handing the frame over (of it: the swap
				// chain's Present, on the GPU).
				fprintf(f, "%lu capture method %d, %.1f fps, magenta %.1f%%, background spans %d %d to %d %d, viewport %d %d to %d %d, mode %d, frame %.1f ms to here "
					"(learn %.1f, match %.1f, regions %.1f, combine %.1f; last reference %.1f, menus %.1f (flyout %.1f, changes %.1f, boxes %.1f), present %.1f (swap %.1f)); "
					"worst of %d frames %.1f ms (reference %.1f, learn %.1f, match %.1f, regions %.1f, combine %.1f, menus %.1f, present %.1f), %d over 8 ms\n",
					statsAt, gCaptureMethod, gFps, gMagentaShare, x0, y0, x1, y1, gViewX0, gViewY0, gViewX1, gViewY1, gMode, ms,
					gStageMs[0], gStageMs[1], gStageMs[3], gStageMs[4], gStageMs[2], gStageMs[5], gMenuMs[0], gMenuMs[1], gMenuMs[2], gStageMs[6],
					gStageMs[7], frames, worstMs, worstStage[2], worstStage[0], worstStage[1], worstStage[3], worstStage[4], worstStage[5], worstStage[6], slow);
				fclose(f);
			}
			worstMs = 0;
			frames = slow = 0;
		}
		if (!gViewFixed && x1 - x0 >= w / 3 && y1 - y0 >= h / 3) {
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
	// Followed in every mode, so a menu open when the panel appears or moves
	// is known.
	UpdateMenus(st, w, h);
	if (gpu) {
		// The rest, per pixel, is the GPU's (the same rules, in its shader).
		std::vector<DrawOp> on;
		if (gAdjust && gMode == 0)
			DrawGizmo(st, w, h, on);
		stage(5);
		GpuCompose(st, on);
		stage(6);
		return;
	}
	// Classic with Model Opacity under 100: the reference shows through the
	// model too, as with Image Plane. ZBrush can only blend its model with
	// the canvas, and the reference is not on it, so the overlay does it. Not
	// over ZBrush's menu pixels, an open flyout (found from its backdrop, so
	// always a real one; build 142 blended over the submenu) or the view
	// gizmo. Build 141 also left out whole menu boxes: with the JRef palette
	// open, strips of the model redrawn as the palette grew were taken as
	// menus and stayed solid until the cursor reached the canvas.
	if (gMode == 0 && gModelOpacity < 100) {
		int vx0, vy0, vx1, vy1;
		ViewportRect(w, h, vx0, vy0, vx1, vy1);
		const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3);
		const int m = (100 - gModelOpacity) * 256 / 100;
		const bool menus = gMenuOpen || gMenuBakedCount;
		ParallelRows(vy0, vy1, [&](int ya, int yb) {
		for (int y = ya; y < yb; ++y) {
			const size_t i0 = static_cast<size_t>(y) * w;
			const bool gizmoRow = y < gGizmoBottom;
			const bool flyoutRow = gFlyout.right > gFlyout.left && y >= gFlyout.top && y < gFlyout.bottom;
			for (int x = vx0; x < vx1; ++x) {
				const size_t i = i0 + x;
				const uint32_t rf = ref32[i];
				const int a = static_cast<int>(rf >> 24);
				if (!a || gKeyShare[i] || (gizmoRow && x >= gizmoX) || (menus && MenuPixel(i, cap32)) ||
					(flyoutRow && x >= gFlyout.left && x < gFlyout.right))
					continue;
				// ref is premultiplied: rf - c * a is a * (reference - model).
				const uint32_t c = cap32[i];
				uint32_t o = 0xFF000000u;
				for (int s = 0; s < 24; s += 8) {
					const int cv = static_cast<int>(c >> s & 255), rv = static_cast<int>(rf >> s & 255);
					const int v = cv + (rv - cv * a / 255) * m / 256;
					o |= static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v) << s;
				}
				out32[i] = o;
			}
		}
		});
	}
	if (gMode == 1) {
		// PureRef: the panel is solid over the model and the view gizmo (the
		// head in the viewport's top right). With the cursor over it, it is
		// see-through (40%) and the gizmo shows in front; it never goes
		// (builds 118 to 120 hid it). ZBrush's menus are always in front:
		// their pixels are left to ZBrush, the panel solid around them.
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		const int ex0 = std::max(0, px), ex1 = std::min(w, px + pw);
		int gvx0, gvy0, gvx1, gvy1;
		ViewportRect(w, h, gvx0, gvy0, gvx1, gvy1);
		const int gizmoX = gvx1 - static_cast<int>(std::min(gvx1 - gvx0, gvy1 - gvy0) * 0.3);
		const uint32_t* cap32 = reinterpret_cast<const uint32_t*>(gCapture.bits);
		const bool menus = gMenuOpen || gMenuBakedCount;
		const bool hover = gPanelAside;
		const unsigned a = 102;
		// A menu's whole box is ZBrush's (a flyout's see-through backdrop
		// over the model changes it too little to be found pixel by pixel).
		std::vector<std::pair<int, int>> spans;
		for (int y = std::max(0, py); y < py + ph && y < h; ++y) {
			const size_t i0 = static_cast<size_t>(y) * w;
			const uint32_t* src = reinterpret_cast<const uint32_t*>(gRefLayer.bits) + i0;
			uint32_t* dst = reinterpret_cast<uint32_t*>(gOutput.bits) + i0;
			const bool gizmoRow = hover && y < gGizmoBottom && gKeyShare.size() == static_cast<size_t>(w) * h;
			spans.clear();
			for (const RECT& r : gMenuBoxes)
				if (y >= r.top && y < r.bottom)
					spans.push_back({static_cast<int>(r.left), static_cast<int>(r.right)});
			// And an open flyout's, found from its backdrop round the canvas.
			if (gFlyout.right > gFlyout.left && y >= gFlyout.top && y < gFlyout.bottom)
				spans.push_back({static_cast<int>(gFlyout.left), static_cast<int>(gFlyout.right)});
			for (int x = ex0; x < ex1; ++x) {
				bool inBox = false;
				for (const auto& s : spans)
					inBox |= x >= s.first && x < s.second;
				if (inBox || (menus && MenuPixel(i0 + x, cap32))) {
					dst[x] = 0;
				} else if (!hover) {
					dst[x] = src[x] | 0xFF000000u;
				} else if (gizmoRow && x >= gizmoX && gKeyShare[i0 + x] < 64) {
					dst[x] = 0;
				} else {
					const uint32_t s = src[x];
					dst[x] = a << 24 | ((s >> 16 & 255) * a / 255) << 16 | ((s >> 8 & 255) * a / 255) << 8 | (s & 255) * a / 255;
				}
			}
		}
	}
	{
		// Outside the viewport (ZBrush's interface over the document's edges)
		// and over ZBrush's other windows the overlay is see-through, so
		// ZBrush's menus and popups show as they are, never covered.
		int vx0, vy0, vx1, vy1;
		ViewportRect(w, h, vx0, vy0, vx1, vy1);
		uint32_t* o = reinterpret_cast<uint32_t*>(gOutput.bits);
		for (int y = 0; y < h; ++y) {
			uint32_t* row = o + static_cast<size_t>(y) * w;
			if (y < vy0 || y >= vy1) {
				memset(row, 0, static_cast<size_t>(w) * 4);
				continue;
			}
			memset(row, 0, static_cast<size_t>(vx0) * 4);
			memset(row + vx1, 0, static_cast<size_t>(w - vx1) * 4);
		}
		const int sx = static_cast<int>(st.screenX), sy = static_cast<int>(st.screenY);
		for (const RECT& r : gUiRects) {
			const int x0 = std::max(0, static_cast<int>(r.left) - sx), x1 = std::min(w, static_cast<int>(r.right) - sx);
			const int y0 = std::max(0, static_cast<int>(r.top) - sy), y1 = std::min(h, static_cast<int>(r.bottom) - sy);
			for (int y = y0; y < y1; ++y)
				if (x1 > x0)
					memset(o + static_cast<size_t>(y) * w + x0, 0, static_cast<size_t>(x1 - x0) * 4);
		}
	}
	if (gAdjust && gMode == 0) {
		std::vector<DrawOp> on;
		DrawGizmo(st, w, h, on);
		RunOps(gOutput, on);
	}
	stage(5);
	Present(wnd, st, gOutput, 255);
	stage(6);
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

// The GPU compositor (build 145). The CPU still finds the background and
// ZBrush's menus (gKeyShare, gMenuMask, gMenuBaked); the GPU draws the
// reference layer from the same drawing steps as the CPU (DrawOp), and one
// shader makes each frame from ZBrush's frame, that layer and those masks by
// the rules Composite's CPU path follows. The overlay window has no bitmap of
// its own: a DirectComposition swap chain shows the frames (premultiplied
// alpha). On the CPU, redrawing the reference for a zoom step took 6 ms
// (71 ms before build 144) and the per-pixel combining 1.5 to 2.5 ms a frame.
// A self-test at start checks that the window shows what it draws and lets
// clicks through; otherwise the layered window drawn by the CPU is used.
static const char kGpuShaders[] = R"(
cbuffer C : register(b0) {
	float4 rect;
	float4 color;
	float2 target; float alphaScale; float gammaExp;
	float sat; int mode; int hover; int menus;
	int4 viewport;
	int4 panel;
	int4 flyout;
	int gizmoX; int gizmoBottom; float blendM; int boxCount;
	int uiCount; float rot; float back; int pad;
	int4 boxes[16];
	int4 ui[16];
};
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D t3 : register(t3);
Texture2D t4 : register(t4);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
	float2 c = float2(id & 1, id >> 1);
	float2 p = lerp(rect.xy, rect.zw, c);
	// An image turned about its centre (clockwise on screen, y down).
	float2 m = (rect.xy + rect.zw) * 0.5, d = p - m;
	float cs = cos(rot), sn = sin(rot);
	p = m + float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
	VO o;
	o.pos = float4(p.x / target.x * 2 - 1, 1 - p.y / target.y * 2, 0, 1);
	o.uv = c;
	return o;
}
float4 fill(VO i) : SV_Target { return color; }
// An image (straight alpha): saturation, gamma, then premultiplied.
float4 image(VO i) : SV_Target {
	float4 c = t0.Sample(smp, i.uv);
	float l = dot(c.rgb, float3(0.3008, 0.5859, 0.1133));
	float3 rgb = pow(saturate(l + (c.rgb - l) * sat), gammaExp);
	float a = c.a * alphaScale;
	return float4(rgb * a, a);
}
bool In(int4 r, int2 p) { return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w; }
// t0 ZBrush's frame, t1 the reference layer (premultiplied), t2 background
// (gKeyShare), t3 and t4 menu pixels (gMenuMask, gMenuBaked).
float4 compose(VO i) : SV_Target {
	int2 p = int2(i.pos.xy);
	int3 l = int3(p, 0);
	float4 cap = float4(t0.Load(l).rgb, 1);
	float4 rf = t1.Load(l);
	bool bg = t2.Load(l).r > 0.5;
	float4 o = cap;
	if (bg)
		o = rf + cap * (1 - rf.a);
	bool menuPix = ((menus & 1) != 0 && t3.Load(l).r > 0) || ((menus & 2) != 0 && t4.Load(l).r > 0);
	bool inFly = In(flyout, p);
	bool inView = In(viewport, p);
	bool gizmo = p.y < gizmoBottom && p.x >= gizmoX;
	bool inBox = inFly;
	for (int k = 0; k < boxCount; ++k)
		inBox = inBox || In(boxes[k], p);
	if (mode == 0 && blendM > 0 && !bg && inView && rf.a > 0 && !gizmo && !menuPix && !inBox)
		o = float4(cap.rgb + (rf.rgb - cap.rgb * rf.a) * blendM, 1);
	// Classic: a menu's whole box is ZBrush's, the canvas behind it plain
	// grey (build 206 showed the reference through the ZPlugin palette's
	// see-through backdrop, only PureRef's panel kept out of the boxes).
	if (mode == 0 && (inBox || menuPix))
		o = bg ? float4(back, back, back, 1) : cap;
	if (mode == 1 && In(panel, p)) {
		if (inBox || menuPix)
			o = 0;
		else if (hover == 0)
			o = float4(rf.rgb, 1);
		else if (gizmo && !bg)
			o = 0;
		else
			o = float4(rf.rgb * 0.4, 0.4);
	}
	if (!inView)
		o = 0;
	for (int k = 0; k < uiCount; ++k)
		if (In(ui[k], p))
			o = 0;
	return o;
}
)";

struct GpuConsts {
	float rect[4];
	float color[4];
	float target[2], alphaScale, gammaExp;
	float sat;
	int mode, hover, menus;
	int viewport[4];
	int panel[4];
	int flyout[4];
	int gizmoX, gizmoBottom;
	float blendM;
	int boxCount;
	int uiCount;
	float rot;  // radians, for a turned image
	float back;  // gBackGrey, 0-1
	int pad;
	int boxes[16][4];
	int ui[16][4];
};

template <typename T>
static void SafeRelease(T*& p)
{
	if (p)
		p->Release();
	p = nullptr;
}

struct GpuTex {
	ID3D11Texture2D* t = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	void Release()
	{
		SafeRelease(rtv);
		SafeRelease(srv);
		SafeRelease(t);
	}
	bool Create(ID3D11Device* dev, int w, int h, DXGI_FORMAT format, bool target)
	{
		Release();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0);
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &t)) || FAILED(dev->CreateShaderResourceView(t, nullptr, &srv)) ||
			(target && FAILED(dev->CreateRenderTargetView(t, nullptr, &rtv)))) {
			Release();
			return false;
		}
		return true;
	}
};

struct GpuOverlay {
	bool ready = false;
	HWND wnd = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	IDCompositionDevice* dcomp = nullptr;
	IDCompositionTarget* target = nullptr;
	IDCompositionVisual* visual = nullptr;
	IDXGISwapChain1* chain = nullptr;
	ID3D11RenderTargetView* backRtv = nullptr;
	int chainW = 0, chainH = 0;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* psFill = nullptr;
	ID3D11PixelShader* psImage = nullptr;
	ID3D11PixelShader* psCompose = nullptr;
	ID3D11Buffer* cb = nullptr;
	ID3D11SamplerState* sampler = nullptr;
	ID3D11BlendState* over = nullptr;
	ID3D11RasterizerState* raster = nullptr;
	GpuTex cap, ref, key, menuMask, menuBaked;
	std::vector<unsigned char> keyCopy, maskCopy, bakedCopy;  // what those textures hold (UploadChanged)
	ID3D11Texture2D* readback = nullptr;
	int w = 0, h = 0;
	LONG winX = 0, winY = 0;
	int winW = 0, winH = 0;
	struct ImageTex {
		unsigned serial;
		ID3D11Texture2D* t;
		ID3D11ShaderResourceView* srv;
		DWORD usedAt;
	};
	std::vector<ImageTex> images;
	volatile bool snapWanted = false;
	volatile bool snapDone = false;
	std::string why;  // for the log: what failed

	void Release()
	{
		for (ImageTex& im : images) {
			SafeRelease(im.srv);
			SafeRelease(im.t);
		}
		images.clear();
		cap.Release();
		ref.Release();
		key.Release();
		menuMask.Release();
		menuBaked.Release();
		SafeRelease(readback);
		SafeRelease(backRtv);
		SafeRelease(chain);
		SafeRelease(visual);
		SafeRelease(target);
		SafeRelease(dcomp);
		SafeRelease(raster);
		SafeRelease(over);
		SafeRelease(sampler);
		SafeRelease(cb);
		SafeRelease(psCompose);
		SafeRelease(psImage);
		SafeRelease(psFill);
		SafeRelease(vs);
		SafeRelease(ctx);
		SafeRelease(dev);
		gGpuDev = nullptr;
		gGpuCapTex = nullptr;
		gGpuCapW = gGpuCapH = 0;
		chainW = chainH = w = h = 0;
		ready = false;
	}

	bool Fail(const char* what)
	{
		why = what;
		Release();
		return false;
	}

	// On the window capture's device when there is one (shared), so its
	// frames are copied on the GPU.
	bool Init(HWND window, ID3D11Device* shared)
	{
		Release();
		wnd = window;
		if (shared) {
			dev = shared;
			dev->AddRef();
			dev->GetImmediateContext(&ctx);
		} else if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
					   D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
			return Fail("no Direct3D 11 device");
		}
		auto compile = [&](const char* entry, const char* profile, ID3DBlob** out) {
			ID3DBlob* errors = nullptr;
			const HRESULT hr = D3DCompile(kGpuShaders, sizeof(kGpuShaders) - 1, "jref", nullptr, nullptr, entry, profile,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
			if (errors) {
				if (FAILED(hr))
					why = static_cast<const char*>(errors->GetBufferPointer());
				errors->Release();
			}
			return SUCCEEDED(hr);
		};
		ID3DBlob* blob = nullptr;
		if (!compile("vs", "vs_5_0", &blob))
			return Fail(("vertex shader: " + why).c_str());
		const HRESULT hv = dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
		blob->Release();
		if (FAILED(hv))
			return Fail("vertex shader not created");
		const char* entries[3] = {"fill", "image", "compose"};
		ID3D11PixelShader** shaders[3] = {&psFill, &psImage, &psCompose};
		for (int k = 0; k < 3; ++k) {
			if (!compile(entries[k], "ps_5_0", &blob))
				return Fail(("pixel shader: " + why).c_str());
			const HRESULT hp = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shaders[k]);
			blob->Release();
			if (FAILED(hp))
				return Fail("pixel shader not created");
		}
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(GpuConsts);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		// Premultiplied "over", as DrawLevels blends.
		D3D11_BLEND_DESC bl = {};
		bl.RenderTarget[0].BlendEnable = TRUE;
		bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.ScissorEnable = TRUE;
		if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb)) || FAILED(dev->CreateSamplerState(&sd, &sampler)) ||
			FAILED(dev->CreateBlendState(&bl, &over)) || FAILED(dev->CreateRasterizerState(&rd, &raster)))
			return Fail("pipeline states not created");
		IDXGIDevice* dxgi = nullptr;
		dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
		const HRESULT hc = dxgi ? DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&dcomp)) : E_FAIL;
		if (dxgi)
			dxgi->Release();
		if (FAILED(hc) || FAILED(dcomp->CreateTargetForHwnd(wnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual)) ||
			FAILED(target->SetRoot(visual)))
			return Fail("DirectComposition not set up");
		ready = true;
		gGpuDev = dev;
		return true;
	}

	bool EnsureChain(int cw, int ch)
	{
		if (chain && cw == chainW && ch == chainH)
			return true;
		SafeRelease(backRtv);
		if (chain) {
			ctx->OMSetRenderTargets(0, nullptr, nullptr);
			if (FAILED(chain->ResizeBuffers(2, cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
				return false;
		} else {
			IDXGIDevice* dxgi = nullptr;
			IDXGIAdapter* adapter = nullptr;
			IDXGIFactory2* factory = nullptr;
			dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
			if (dxgi)
				dxgi->GetAdapter(&adapter);
			if (adapter)
				adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
			DXGI_SWAP_CHAIN_DESC1 d = {};
			d.Width = cw;
			d.Height = ch;
			d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			d.BufferCount = 2;
			d.Scaling = DXGI_SCALING_STRETCH;
			d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
			d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
			const HRESULT hr = factory ? factory->CreateSwapChainForComposition(dev, &d, nullptr, &chain) : E_FAIL;
			SafeRelease(factory);
			SafeRelease(adapter);
			SafeRelease(dxgi);
			if (FAILED(hr) || FAILED(visual->SetContent(chain)) || FAILED(dcomp->Commit()))
				return false;
		}
		ID3D11Texture2D* back = nullptr;
		if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
			return false;
		const HRESULT hr = dev->CreateRenderTargetView(back, nullptr, &backRtv);
		back->Release();
		if (FAILED(hr))
			return false;
		chainW = cw;
		chainH = ch;
		return true;
	}

	// The w x h textures the frames are made from.
	bool EnsureSize(int nw, int nh)
	{
		if (nw == w && nh == h && cap.t)
			return true;
		gGpuCapTex = nullptr;
		gGpuCapW = gGpuCapH = 0;
		gGpuCapFresh = false;
		SafeRelease(readback);
		keyCopy.clear();
		maskCopy.clear();
		bakedCopy.clear();
		if (!cap.Create(dev, nw, nh, DXGI_FORMAT_B8G8R8A8_UNORM, false) || !ref.Create(dev, nw, nh, DXGI_FORMAT_B8G8R8A8_UNORM, true) ||
			!key.Create(dev, nw, nh, DXGI_FORMAT_R8_UNORM, false) || !menuMask.Create(dev, nw, nh, DXGI_FORMAT_R8_UNORM, false) ||
			!menuBaked.Create(dev, nw, nh, DXGI_FORMAT_R8_UNORM, false)) {
			w = h = 0;
			return false;
		}
		const float clear[4] = {0, 0, 0, 0};
		ctx->ClearRenderTargetView(ref.rtv, clear);
		w = nw;
		h = nh;
		gGpuCapTex = cap.t;
		gGpuCapW = w;
		gGpuCapH = h;
		return true;
	}

	// A one-byte-per-pixel map sent to its texture: only the band of rows
	// that changed since the last send (copy holds what the texture has), and
	// nothing when none did. Sending the whole background map (2 MB) and both
	// menu maps every frame made most of the 1.5 to 2.5 ms hand-off (build 145).
	void UploadChanged(GpuTex& t, const unsigned char* src, std::vector<unsigned char>& copy, int tw, int th)
	{
		const size_t n = static_cast<size_t>(tw) * th;
		int y0 = 0, y1 = th;
		if (copy.size() == n) {
			while (y0 < th && !memcmp(src + static_cast<size_t>(y0) * tw, &copy[static_cast<size_t>(y0) * tw], tw))
				++y0;
			if (y0 == th)
				return;
			while (y1 > y0 && !memcmp(src + static_cast<size_t>(y1 - 1) * tw, &copy[static_cast<size_t>(y1 - 1) * tw], tw))
				--y1;
		} else {
			copy.resize(n);
		}
		const D3D11_BOX box = {0, static_cast<UINT>(y0), 0, static_cast<UINT>(tw), static_cast<UINT>(y1), 1};
		ctx->UpdateSubresource(t.t, 0, &box, src + static_cast<size_t>(y0) * tw, tw, 0);
		memcpy(&copy[static_cast<size_t>(y0) * tw], src + static_cast<size_t>(y0) * tw, static_cast<size_t>(y1 - y0) * tw);
	}

	void SetConsts(const GpuConsts& c)
	{
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			memcpy(m.pData, &c, sizeof(c));
			ctx->Unmap(cb, 0);
		}
	}

	// A texture of the image (its levels as mips), made once per image.
	ID3D11ShaderResourceView* ImageSrv(const std::vector<Level>* levels)
	{
		const ImageCache* entry = ImageEntry(levels);
		if (!entry || levels->empty())
			return nullptr;
		for (ImageTex& im : images)
			if (im.serial == entry->serial) {
				im.usedAt = GetTickCount();
				return im.srv;
			}
		const Level& top = (*levels)[0];
		if (top.w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || top.h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
			return nullptr;
		if (images.size() >= 6) {
			size_t oldest = 0;
			for (size_t i = 1; i < images.size(); ++i)
				if (static_cast<int>(images[i].usedAt - images[oldest].usedAt) < 0)
					oldest = i;
			SafeRelease(images[oldest].srv);
			SafeRelease(images[oldest].t);
			images.erase(images.begin() + oldest);
		}
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = top.w;
		d.Height = top.h;
		d.MipLevels = static_cast<UINT>(levels->size());
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_IMMUTABLE;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> init(levels->size());
		for (size_t i = 0; i < levels->size(); ++i) {
			init[i].pSysMem = (*levels)[i].px.data();
			init[i].SysMemPitch = (*levels)[i].w * 4;
		}
		ImageTex im = {entry->serial, nullptr, nullptr, GetTickCount()};
		if (FAILED(dev->CreateTexture2D(&d, init.data(), &im.t)) || FAILED(dev->CreateShaderResourceView(im.t, nullptr, &im.srv))) {
			SafeRelease(im.t);
			return nullptr;
		}
		images.push_back(im);
		return im.srv;
	}

	// The drawing steps on a tw x th target, as RunOps does them on the CPU.
	void DrawOps(ID3D11RenderTargetView* rtv, int tw, int th, const std::vector<DrawOp>& ops)
	{
		if (ops.empty())
			return;
		D3D11_VIEWPORT vp = {0, 0, static_cast<float>(tw), static_cast<float>(th), 0, 1};
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(raster);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ctx->IASetInputLayout(nullptr);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetSamplers(0, 1, &sampler);
		GpuConsts c = {};
		c.target[0] = static_cast<float>(tw);
		c.target[1] = static_cast<float>(th);
		c.gammaExp = 100.0f / gGamma;
		c.sat = gSat / 100.0f;
		for (const DrawOp& o : ops) {
			D3D11_RECT clip = {std::max(0, o.x0), std::max(0, o.y0), std::min(tw, o.x1), std::min(th, o.y1)};
			c.rot = 0;
			if (o.image) {
				double bx0 = o.ix, by0 = o.iy, bx1 = o.ix + o.iw, by1 = o.iy + o.ih;
				if (o.rot != 0) {
					TurnedBounds(o.ix, o.iy, o.iw, o.ih, o.rot, bx0, by0, bx1, by1);
					c.rot = static_cast<float>(o.rot * kPi / 180);
				}
				clip.left = std::max<LONG>(clip.left, static_cast<LONG>(floor(bx0)));
				clip.top = std::max<LONG>(clip.top, static_cast<LONG>(floor(by0)));
				clip.right = std::min<LONG>(clip.right, static_cast<LONG>(ceil(bx1)));
				clip.bottom = std::min<LONG>(clip.bottom, static_cast<LONG>(ceil(by1)));
			}
			if (clip.right <= clip.left || clip.bottom <= clip.top)
				continue;
			ID3D11ShaderResourceView* srv = nullptr;
			if (o.image) {
				srv = ImageSrv(o.levels);
				if (!srv || o.iw <= 0 || o.ih <= 0)
					continue;
				c.rect[0] = static_cast<float>(o.ix);
				c.rect[1] = static_cast<float>(o.iy);
				c.rect[2] = static_cast<float>(o.ix + o.iw);
				c.rect[3] = static_cast<float>(o.iy + o.ih);
				c.alphaScale = o.alpha / 255.0f;
			} else {
				c.rect[0] = static_cast<float>(clip.left);
				c.rect[1] = static_cast<float>(clip.top);
				c.rect[2] = static_cast<float>(clip.right);
				c.rect[3] = static_cast<float>(clip.bottom);
				c.color[0] = (o.argb >> 16 & 255) / 255.0f;
				c.color[1] = (o.argb >> 8 & 255) / 255.0f;
				c.color[2] = (o.argb & 255) / 255.0f;
				c.color[3] = (o.argb >> 24 & 255) / 255.0f;
			}
			SetConsts(c);
			ctx->RSSetScissorRects(1, &clip);
			ctx->OMSetBlendState(o.image ? over : nullptr, nullptr, 0xFFFFFFFFu);
			ctx->PSSetShader(o.image ? psImage : psFill, nullptr, 0);
			ctx->PSSetShaderResources(0, 1, &srv);
			ctx->Draw(4, 0);
		}
		ID3D11ShaderResourceView* none = nullptr;
		ctx->PSSetShaderResources(0, 1, &none);
	}

	bool DrawRef(int nw, int nh, const std::vector<DrawOp>& ops)
	{
		if (!EnsureSize(nw, nh))
			return false;
		DrawOps(ref.rtv, w, h, ops);
		return true;
	}

	// Puts the window where the overlay is and shows the frame.
	void Show(const OverlayState& st, int fw, int fh)
	{
		const LONG x = static_cast<LONG>(st.screenX), y = static_cast<LONG>(st.screenY);
		if (x != winX || y != winY || fw != winW || fh != winH) {
			SetWindowPos(wnd, nullptr, x, y, fw, fh, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
			winX = x;
			winY = y;
			winW = fw;
			winH = fh;
		}
		LARGE_INTEGER p0, p1, pf;
		QueryPerformanceCounter(&p0);
		chain->Present(0, 0);
		QueryPerformanceCounter(&p1);
		QueryPerformanceFrequency(&pf);
		gStageMs[7] = 1000.0 * (p1.QuadPart - p0.QuadPart) / pf.QuadPart;
		if (!IsWindowVisible(wnd))
			ShowWindow(wnd, SW_SHOWNOACTIVATE);
	}

	// A frame drawn by the CPU (premultiplied BGRA), straight into the swap chain.
	bool PresentBuffer(const OverlayState& st, Buffer32& buf)
	{
		if (!EnsureChain(buf.w, buf.h))
			return false;
		ID3D11Texture2D* back = nullptr;
		if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
			return false;
		ctx->UpdateSubresource(back, 0, nullptr, buf.bits, buf.w * 4, 0);
		back->Release();
		Show(st, buf.w, buf.h);
		return true;
	}

	// Copies the frame about to be shown into gOutput (RSSnapshot).
	void Snapshot()
	{
		ID3D11Texture2D* back = nullptr;
		if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
			return;
		if (!readback) {
			D3D11_TEXTURE2D_DESC d;
			back->GetDesc(&d);
			d.Usage = D3D11_USAGE_STAGING;
			d.BindFlags = 0;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			d.MiscFlags = 0;
			dev->CreateTexture2D(&d, nullptr, &readback);
		}
		if (readback && gOutput.w == chainW && gOutput.h == chainH) {
			ctx->CopyResource(readback, back);
			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m))) {
				for (int y = 0; y < chainH; ++y)
					memcpy(gOutput.bits + static_cast<size_t>(y) * chainW * 4, static_cast<BYTE*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
						static_cast<size_t>(chainW) * 4);
				ctx->Unmap(readback, 0);
			}
		}
		back->Release();
	}

	// One frame, by Composite's rules (see the shader), with the drawing
	// steps "on" (the Adjust Image handles) over it.
	void Compose(const OverlayState& st, const std::vector<DrawOp>& on)
	{
		const int fw = gCapture.w, fh = gCapture.h;
		if (!EnsureSize(fw, fh) || !EnsureChain(fw, fh))
			return;
		const size_t n = static_cast<size_t>(fw) * fh;
		if (!gGpuCapFresh) {
			ctx->UpdateSubresource(cap.t, 0, nullptr, gCapture.bits, fw * 4, 0);
			gGpuCapFresh = true;
		}
		if (gKeyShare.size() == n)
			UploadChanged(key, gKeyShare.data(), keyCopy, fw, fh);
		GpuConsts c = {};
		c.rect[2] = static_cast<float>(fw);
		c.rect[3] = static_cast<float>(fh);
		c.target[0] = static_cast<float>(fw);
		c.target[1] = static_cast<float>(fh);
		c.mode = gMode;
		c.hover = gPanelAside ? 1 : 0;
		if (gMenuMaskCount && gMenuMask.size() == n) {
			UploadChanged(menuMask, gMenuMask.data(), maskCopy, fw, fh);
			c.menus |= 1;
		}
		if (gMenuBakedCount && gMenuBaked.size() == n) {
			UploadChanged(menuBaked, gMenuBaked.data(), bakedCopy, fw, fh);
			c.menus |= 2;
		}
		int vx0, vy0, vx1, vy1, px, py, pw, ph;
		ViewportRect(fw, fh, vx0, vy0, vx1, vy1);
		PanelRect(fw, fh, px, py, pw, ph);
		const int vp[4] = {vx0, vy0, vx1, vy1}, pn[4] = {px, py, px + pw, py + ph};
		memcpy(c.viewport, vp, sizeof(vp));
		memcpy(c.panel, pn, sizeof(pn));
		if (gFlyout.right > gFlyout.left) {
			const int fl[4] = {static_cast<int>(gFlyout.left), static_cast<int>(gFlyout.top), static_cast<int>(gFlyout.right),
				static_cast<int>(gFlyout.bottom)};
			memcpy(c.flyout, fl, sizeof(fl));
		}
		c.gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3);
		c.gizmoBottom = gGizmoBottom;
		c.blendM = gModelOpacity < 100 ? (100 - gModelOpacity) / 100.0f : 0;
		c.back = (gBackGrey & 255) / 255.0f;
		for (const RECT& r : gMenuBoxes) {
			if (c.boxCount == 16)
				break;
			const int b[4] = {static_cast<int>(r.left), static_cast<int>(r.top), static_cast<int>(r.right), static_cast<int>(r.bottom)};
			memcpy(c.boxes[c.boxCount++], b, sizeof(b));
		}
		const int sx = static_cast<int>(st.screenX), sy = static_cast<int>(st.screenY);
		for (const RECT& r : gUiRects) {
			if (c.uiCount == 16)
				break;
			const int u[4] = {static_cast<int>(r.left) - sx, static_cast<int>(r.top) - sy, static_cast<int>(r.right) - sx, static_cast<int>(r.bottom) - sy};
			memcpy(c.ui[c.uiCount++], u, sizeof(u));
		}
		SetConsts(c);
		D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(fw), static_cast<float>(fh), 0, 1};
		const D3D11_RECT all = {0, 0, fw, fh};
		ctx->OMSetRenderTargets(1, &backRtv, nullptr);
		ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
		ctx->RSSetViewports(1, &viewport);
		ctx->RSSetState(raster);
		ctx->RSSetScissorRects(1, &all);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ctx->IASetInputLayout(nullptr);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetShader(psCompose, nullptr, 0);
		ID3D11ShaderResourceView* srvs[5] = {cap.srv, ref.srv, key.srv, menuMask.srv, menuBaked.srv};
		ctx->PSSetShaderResources(0, 5, srvs);
		ctx->Draw(4, 0);
		ID3D11ShaderResourceView* none[5] = {};
		ctx->PSSetShaderResources(0, 5, none);
		DrawOps(backRtv, fw, fh, on);
		if (snapWanted) {
			Snapshot();
			snapWanted = false;
			snapDone = true;
		}
		Show(st, fw, fh);
	}

	// Shows a test square and reads the screen back: the window must show
	// what it draws, and clicks must go through it to ZBrush.
	bool SelfTest(HWND zbrush)
	{
		RECT zr;
		if (!zbrush || !GetWindowRect(zbrush, &zr))
			return Fail("no ZBrush window for the self-test");
		const int s = 16;
		OverlayState st;
		st.screenX = (zr.left + zr.right) / 2 - s / 2;
		st.screenY = (zr.top + zr.bottom) / 2 - s / 2;
		Buffer32 b;
		if (!b.Resize(s, s))
			return Fail("self-test buffer");
		const uint32_t test = 0xFF13E78Cu;
		for (int i = 0; i < s * s; ++i)
			reinterpret_cast<uint32_t*>(b.bits)[i] = test;
		if (!PresentBuffer(st, b))
			return Fail("swap chain not created");
		DwmFlush();
		Sleep(50);
		DwmFlush();
		HDC screen = GetDC(nullptr);
		const COLORREF seen = GetPixel(screen, static_cast<int>(st.screenX) + s / 2, static_cast<int>(st.screenY) + s / 2);
		ReleaseDC(nullptr, screen);
		const POINT mid = {static_cast<LONG>(st.screenX) + s / 2, static_cast<LONG>(st.screenY) + s / 2};
		const HWND hit = WindowFromPoint(mid);
		memset(b.bits, 0, static_cast<size_t>(s) * s * 4);
		PresentBuffer(st, b);
		ShowWindow(wnd, SW_HIDE);
		const bool shows = abs(static_cast<int>(GetRValue(seen)) - 0x13) <= 3 && abs(static_cast<int>(GetGValue(seen)) - 0xE7) <= 3 &&
			abs(static_cast<int>(GetBValue(seen)) - 0x8C) <= 3;
		if (!shows) {
			char line[96];
			snprintf(line, sizeof(line), "self-test: the window did not show its frame (saw %06lX)", static_cast<unsigned long>(seen));
			return Fail(line);
		}
		if (hit == wnd)
			return Fail("self-test: clicks did not go through the window");
		return true;
	}
};

static GpuOverlay gGpu;

static bool GpuComposes()
{
	return gGpu.ready && gKeyState == 1;
}

static void GpuCompose(const OverlayState& st, const std::vector<DrawOp>& on)
{
	gGpu.Compose(st, on);
}

static bool GpuDrawRef(int w, int h, const std::vector<DrawOp>& ops)
{
	return GpuComposes() && gGpu.DrawRef(w, h, ops);
}

static bool GpuPresentBuffer(const OverlayState& st, Buffer32& buf)
{
	return gGpu.ready && gGpu.PresentBuffer(st, buf);
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

static int ScanWorkers()
{
	static const int n = std::max(1, std::min(32, static_cast<int>(std::thread::hardware_concurrency())));
	return n;
}

// Runs fn(worker, at, bytes, starts, got) over every 4 MB piece of the
// committed memory keep(region) accepts, on every core: the scans for the
// camera and the depth images read 3-4 GB, 1.4-11 s on one core (build 182).
// A piece is read `overlap` bytes past its end (within its region) so a value
// across the boundary is whole; fn takes the values starting before `starts`,
// so none is seen twice. The scan's own buffers are left out. Returns the
// bytes scanned.
template <typename Keep, typename Fn>
static size_t ScanMemory(size_t overlap, const Keep& keep, const Fn& fn)
{
	const size_t piece = 1 << 22;
	const int workers = ScanWorkers();
	std::vector<std::vector<unsigned char>> bufs(workers, std::vector<unsigned char>(piece + overlap));
	std::vector<std::pair<uintptr_t, size_t>> pieces;
	MEMORY_BASIC_INFORMATION mbi;
	for (uintptr_t addr = 0; VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) == sizeof mbi;) {
		const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), next = base + mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && keep(mbi))
			for (uintptr_t at = base; at < next; at += piece) {
				const size_t len = std::min<size_t>(piece + overlap, next - at);
				bool own = false;
				for (const auto& b : bufs) {
					const uintptr_t lo = reinterpret_cast<uintptr_t>(b.data());
					own = own || (at < lo + b.size() && lo < at + len);
				}
				if (!own)
					pieces.push_back({at, len});
			}
		if (next <= addr)
			break;
		addr = next;
	}
	std::atomic<size_t> nextPiece{0}, scanned{0};
	concurrency::parallel_for(0, workers, [&](int w) {
		unsigned char* buf = bufs[w].data();
		for (size_t k; (k = nextPiece++) < pieces.size();) {
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pieces[k].first), buf, pieces[k].second, &got))
				continue;
			const size_t starts = std::min<size_t>(piece, got);
			scanned += starts;
			fn(w, pieces[k].first, buf, starts, static_cast<size_t>(got));
		}
	});
	return scanned;
}

// Where ZBrush's camera can be: its writable memory, not this DLL's own
// copies of the values, nor the calling thread's stack (it holds the values
// being reported right now) or JRef's threads'.
static bool CameraRegion(const MEMORY_BASIC_INFORMATION& m)
{
	static HMODULE self = nullptr;
	if (!self)
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&CameraRegion), &self);
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uintptr_t stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit) & ~static_cast<uintptr_t>(0xfffff);
	const uintptr_t stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
	const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), next = base + m.RegionSize;
	const DWORD prot = m.Protect & 0xff;
	return (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE) &&
		(m.Type == MEM_PRIVATE || m.Type == MEM_IMAGE) && m.AllocationBase != self &&
		!(stackLow < next && base < stackHigh) && !OwnThreadStack(base, next);
}

// Scans for x with y right after it (as float or double pairs), then for the
// scale within 1 KB of each pair. Scanning for the scale on its own finds too
// many matches when it is a common number such as 1.
// One search, in parts, so the rotation's can share its pass over memory
// (CameraScan): set up, each piece of memory, then the results.
struct MemScanJob {
	static const size_t kMaxPairs = 200000;
	double sx, px, py;
	int workers;
	std::vector<std::vector<MemCand>> xs, ys;
	std::vector<size_t> rawXs, rawYs;
	// MemMatch's ranges, for the quick test sixteen bytes at a time.
	__m128d dxLo, dxHi, dyLo, dyHi;
	__m128 fxLo, fxHi, fyLo, fyHi;
	MemScanJob(double sx_, double px_, double py_);
	void Piece(int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got);
	void Finish(size_t scanned, DWORD t0);
};

MemScanJob::MemScanJob(double sx_, double px_, double py_) : sx(sx_), px(px_), py(py_), workers(ScanWorkers()),
	xs(workers), ys(workers), rawXs(workers), rawYs(workers)
{
	MemTrack* tracks[3] = {&gMemScale, &gMemX, &gMemY};
	const double targets[3] = {sx, px, py};
	for (int t = 0; t < 3; ++t) {
		tracks[t]->cands.clear();
		tracks[t]->target = targets[t];
		tracks[t]->narrowings = 0;
		tracks[t]->tooMany = false;
	}
	const double xTol = 0.0051 + fabs(px) * 1e-5, yTol = 0.0051 + fabs(py) * 1e-5;
	dxLo = _mm_set1_pd(px - xTol);
	dxHi = _mm_set1_pd(px + xTol);
	dyLo = _mm_set1_pd(py - yTol);
	dyHi = _mm_set1_pd(py + yTol);
	fxLo = _mm_set1_ps(nextafterf(static_cast<float>(px - xTol), -INFINITY));
	fxHi = _mm_set1_ps(nextafterf(static_cast<float>(px + xTol), INFINITY));
	fyLo = _mm_set1_ps(nextafterf(static_cast<float>(py - yTol), -INFINITY));
	fyHi = _mm_set1_ps(nextafterf(static_cast<float>(py + yTol), INFINITY));
}

void MemScanJob::Piece(int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got)
{
	{
		std::vector<MemCand>& wx = xs[w];
		std::vector<MemCand>& wy = ys[w];
		size_t rawX = 0, rawY = 0;
		for (size_t blk = 0; blk < starts; blk += 16) {
			// Sixteen bytes at a time first, as four floats and two doubles
			// against x's and y's ranges (widened a step, so nothing that
			// matches is skipped): nearly all of memory is neither.
			if (blk + 16 <= got) {
				const __m128 f4 = _mm_loadu_ps(reinterpret_cast<const float*>(buf + blk));
				const __m128d d2 = _mm_loadu_pd(reinterpret_cast<const double*>(buf + blk));
				const __m128 inF = _mm_or_ps(_mm_and_ps(_mm_cmpge_ps(f4, fxLo), _mm_cmple_ps(f4, fxHi)),
					_mm_and_ps(_mm_cmpge_ps(f4, fyLo), _mm_cmple_ps(f4, fyHi)));
				const __m128d inD = _mm_or_pd(_mm_and_pd(_mm_cmpge_pd(d2, dxLo), _mm_cmple_pd(d2, dxHi)),
					_mm_and_pd(_mm_cmpge_pd(d2, dyLo), _mm_cmple_pd(d2, dyHi)));
				if (!_mm_movemask_ps(inF) && !_mm_movemask_pd(inD))
					continue;
			}
			for (size_t i = blk; i < blk + 16 && i < starts && i + 8 <= got; i += 4) {
				float f, g;
				memcpy(&f, &buf[i], 4);
				memcpy(&g, &buf[i + 4], 4);
				if (MemMatch(f, px)) {
					++rawX;
					if (MemMatch(g, py) && wx.size() < kMaxPairs) {
						wx.push_back({at + i, false, f, 0});
						wy.push_back({at + i + 4, false, g, 0});
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
						if (MemMatch(e, py) && wx.size() < kMaxPairs) {
							wx.push_back({at + i, true, d, 0});
							wy.push_back({at + i + 8, true, e, 0});
						}
					}
					if (MemMatch(d, py))
						++rawY;
				}
			}
		}
		rawXs[w] += rawX;
		rawYs[w] += rawY;
	}
}

void MemScanJob::Finish(size_t scanned, DWORD t0)
{
	size_t rawX = 0, rawY = 0;
	for (int w = 0; w < workers; ++w) {
		rawX += rawXs[w];
		rawY += rawYs[w];
		for (size_t k = 0; k < xs[w].size() && gMemX.cands.size() < kMaxPairs; ++k) {
			gMemX.cands.push_back(xs[w][k]);
			gMemY.cands.push_back(ys[w][k]);
		}
	}
	// In address order, as one pass over memory found them.
	for (MemTrack* tr : {&gMemX, &gMemY})
		std::sort(tr->cands.begin(), tr->cands.end(), [](const MemCand& a, const MemCand& b) { return a.addr < b.addr; });
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

static void MemScan(double sx, double px, double py)
{
	const DWORD t0 = GetTickCount();
	MemScanJob job(sx, px, py);
	const size_t scanned = ScanMemory(16, CameraRegion, [&](int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got) {
		job.Piece(w, at, buf, starts, got);
	});
	job.Finish(scanned, t0);
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
	// Frames of a zoom drag in which its scale changed. Some copies move
	// with the drag but keep the scale until the button is let go (build
	// 193, triplet 9: x and y followed a zoom, scale 40.83 until release,
	// 117.44 after), and the cells went to the forehead mid-zoom.
	int scaleFrames;
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
					gTriplets.push_back({sc, x, y, 0, 0, 0, 0, 0});
		}
}

static bool Turning();

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
				if (mode == kDragScale && a != t.s.last)
					++t.scaleFrames;
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
		// ever update the scale, or only after the button is let go), then
		// the busiest, then the latest. Moving while rotating does not count
		// against a copy: ZBrush's live position does move when it turns
		// about the model (nav_log: x 560.958 to 565.552 in a turn's first
		// frame), and passing over those copies picked one that only updated
		// after each drag, so pans lagged a drag behind. MemFollow keeps a
		// rotation's position change out of the image anyway.
		// A copy whose scale follows zooms counts well ahead of one whose
		// position alone does (see scaleFrames).
		const MemTriplet& o = gTriplets[best];
		const int ts = t.posFrames + 4 * t.scaleFrames, os = o.posFrames + 4 * o.scaleFrames;
		if (ts != os ? ts > os :
				t.dragFrames != o.dragFrames ? t.dragFrames > o.dragFrames : static_cast<int>(t.changedAt - o.changedAt) > 0)
			best = static_cast<int>(i);
	}
	// Stay with the copy in use unless another is clearly busier: near-equal
	// copies (a pixel or two apart) would otherwise make the image jitter.
	// Never with one whose scale has stayed put through zooms that another's
	// followed.
	if (best >= 0 && gTripletInUse >= 0 && gTripletInUse < static_cast<int>(gTriplets.size()) && best != gTripletInUse) {
		const MemTriplet& cur = gTriplets[gTripletInUse];
		const MemTriplet& cand = gTriplets[best];
		double a, b, c;
		const bool curReadable = MemRead(cur.s, a) && MemRead(cur.x, b) && MemRead(cur.y, c);
		const int curScore = cur.posFrames + 4 * cur.scaleFrames, candScore = cand.posFrames + 4 * cand.scaleFrames;
		const bool staleScale = cand.scaleFrames >= 3 && cur.scaleFrames * 4 < cand.scaleFrames;
		if (curReadable && !staleScale && candScore <= curScore * 3 / 2 + 10 && !(cur.dragFrames == 0 && cand.dragFrames > 0))
			best = gTripletInUse;
	}
	if (best < 0)
		return false;
	const MemTriplet& t = gTriplets[best];
	// A jump: well beyond the last step taken (40 document pixels more than
	// twice it), so a pan already moving fast is not one. 200 pixels from any
	// step missed a read 170 pixels off between two alike, the camera still
	// before it (build 173: y -85, -28, -79, the cell 82 px and back).
	static double stepX = 0, stepY = 0;
	const bool jump = sx > 0 && (fabs(t.s.last / sx - 1) > 0.3 || fabs(t.x.last - px) > 40 + 2 * fabs(stepX) || fabs(t.y.last - py) > 40 + 2 * fabs(stepY));
	// ZBrush briefly reuses the view's memory, for example to draw the
	// navigation head. A big jump is only taken once the next read carries
	// on from it (close to it, not equal: a fast zoom or pan keeps changing,
	// and waiting for equal reads held the image still until the drag
	// stopped). Not while turning: with Persp on ZBrush's pan and zoom move
	// 300-600 pixels a frame as it turns, and holding them back paired the
	// turned rotation with the pan and zoom from before (build 219: the
	// cells flicked half a window off and back every 150-400 ms of a turn).
	static int pending = 0;
	static double pendS = 0, pendX = 0, pendY = 0;
	if (jump && !Turning()) {
		const bool follows = pending > 0 && fabs(t.s.last / pendS - 1) < 0.3 && fabs(t.x.last - pendX) < 200 && fabs(t.y.last - pendY) < 200;
		pending = follows ? pending + 1 : 1;
		pendS = t.s.last;
		pendX = t.x.last;
		pendY = t.y.last;
		if (pending < 2)
			return true;  // keep the values passed in
	}
	pending = 0;
	if (best != gTripletInUse || jump) {
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			fprintf(f, "%lu read triplet %d of %zu (drag frames %d, scale followed zooms %d)%s: scale %.4f x %.3f y %.3f, was %.4f %.3f %.3f\n",
				now, best, gTriplets.size(), t.dragFrames, t.scaleFrames, jump ? " JUMP" : "", t.s.last, t.x.last, t.y.last, sx, px, py);
			fclose(f);
		}
		gTripletInUse = best;
	}
	stepX = t.x.last - px;
	stepY = t.y.last - py;
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
// Nor may it, or a pan, scale it: in perspective ZBrush's scale changes as
// the model turns or moves (nav_log, build 190: 0.91 to 0.60 in one turn,
// 2.48 to 2.52 in a pan), so the image keeps its size through those frames
// (OverlayState::fix) and pans move it as they are, flat. Zooms scale it
// from there; snapping to a view puts it back on ZBrush's scale.
// ZBrush's rotation is read from memory, and is changing (RotFollow).
static bool gTurnKnown = false, gTurning = false;

// The image's size held through a turn or pan (hold), against ZBrush's
// zoom moving r: within 5% of the zoom the hold began at, the size stays
// (fix makes up the difference, ZBrush's jitter); past it, or ending over
// 0.5% from it, it is a real zoom and the image follows it from where the
// hold began. Builds 192-201 made
// up every change during a pan, and ZBrush's Alt + right drag zoom reads as
// a pan for its first frames (and pans and zooms in one drag): each zoom
// left the image a little behind, and a few zooms in and out shrank it to a
// twelfth of the model's size.
struct ScaleHold {
	bool on = false;
	double r0 = 0, fix0 = 1;
};

static void HoldScale(ScaleHold& h, bool hold, double rOld, double r, double& fix)
{
	if (!hold || rOld <= 0 || r <= 0) {
		// Ended away from the zoom it began at (by over 0.5%: a pan's jitter
		// comes back): a small zoom within the drag, followed too; else the
		// size it began with, at the zoom it ends at.
		if (h.on && r > 0)
			fix = fabs(r / h.r0 - 1) > 0.005 ? h.fix0 : h.fix0 * h.r0 / r;
		h.on = false;
		return;
	}
	if (!h.on) {
		h.on = true;
		h.r0 = rOld;
		h.fix0 = fix;
	}
	fix = fabs(r / h.r0 - 1) < 0.05 ? std::max(0.01, std::min(100.0, h.fix0 * h.r0 / r)) : h.fix0;
}

// Turning: the rotation read from memory is changing, or without it, a drag
// that is neither a pan nor a zoom. Never in a pan or a zoom: ZBrush does not
// turn then, whatever a copy of the rotation reads, and a pan taken for a
// turn goes into the offset (the image stops following the model).
static bool Turning()
{
	if (gGesture.active && (gGesture.mode == kDragMove || gGesture.mode == kDragScale))
		return false;
	return gTurnKnown ? gTurning : gGesture.active && gGesture.mode == kDragNone;
}

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
	const bool rotating = Turning();
	const bool panning = gGesture.active && gGesture.mode == kDragMove;
	OverlayState& st = gOverlay;
	double sx = st.ratio * gAnchorScale, px = st.px, py = st.py;
	const bool read = !gTriplets.empty() ? TripletValues(sx, px, py, gGesture.active ? gGesture.mode : -1) :
		MemValue(gMemScale, sx) && MemValue(gMemX, px) && MemValue(gMemY, py);
	// A zoom whose scale has not moved while its position has: the copy's
	// scale is not live (see MemTriplet::scaleFrames). The layout holds until
	// it moves or the drag ends, rather than pair the new position with the
	// old scale (the cells went to the forehead mid-zoom, build 193).
	static bool zooming = false;
	static double zoomScale = 0;
	const bool zoomNow = gGesture.active && gGesture.mode == kDragScale;
	if (zoomNow && !zooming)
		zoomScale = st.ratio * gAnchorScale;
	zooming = zoomNow;
	const bool stale = zooming && read && sx == zoomScale && (px != st.px || py != st.py);
	if (read && !stale && sx > 0 && sx < 1e7 && fabs(px) < 1e6 && fabs(py) < 1e6) {
		const double r = sx / gAnchorScale;
		static ScaleHold hold;
		HoldScale(hold, rotating || panning, st.ratio, r, st.fix);
		if (rotating) {
			st.offX += (px - st.px) / (r * st.fix);
			st.offY += (py - st.py) / (r * st.fix);
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
	double imgSX = 1, imgSY = 1, imgX = 0, imgY = 0, imgRot = 0;  // Adjust Image
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
	// (view%d_mask.bin: before build 83, with the interface in it;
	// view%d_outline.bin: before build 204, with palettes and tooltips
	// refreshed into it)
	swprintf(name, 32, L"view%d_shape.bin", i);
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
		// The image's turn after the codes (build 191; older builds stop
		// reading the codes there and still find the path after the bar).
		fprintf(f, "%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s %.6f|%s\n", v.used ? 1 : 0, v.rx, v.ry, v.rz, v.scale,
			v.ax, v.ay, v.imgSX, v.imgSY, v.imgX, v.imgY, codes.empty() ? "-1" : codes.c_str(), v.imgRot, v.path.c_str());
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
			v.imgRot = 0;
			const int got = sscanf(line, "%d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %199[-0-9,] %lf", &used, &v.rx, &v.ry, &v.rz, &v.scale,
				&v.ax, &v.ay, &v.imgSX, &v.imgSY, &v.imgX, &v.imgY, codes, &v.imgRot);
			if (got < 7)
				continue;
			if (got < 12 || v.imgSX <= 0 || v.imgSY <= 0) {
				v.imgSX = v.imgSY = 1;
				v.imgX = v.imgY = v.imgRot = 0;
				codes[0] = 0;
			}
			if (!(fabs(v.imgRot) <= 360))
				v.imgRot = 0;
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
	gOverlay.imgRot = v.imgRot;
	gOverlay.offX = gOverlay.offY = 0;
	gOverlay.fix = 1;
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
	v.imgRot = gOverlay.imgRot;
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
	gOverlay.imgX = gOverlay.imgY = gOverlay.imgRot = 0;
	gOverlay.offX = gOverlay.offY = 0;
	gOverlay.fix = 1;
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
	out.imgRot = v.imgRot;
	if (slot != gActiveView) {
		out.offX = out.offY = 0;
		out.fix = 1;
	}
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
	std::vector<unsigned char> clean;  // the silhouette without straight structures
	int removed = 0;
};
static MaskExtent gExtents[kViewSlots];

// A silhouette as recorded holds everything ZBrush drew that is not the
// background: image planes (a frame round the model, edge on; a filled block
// face on) and the floor line, all straight and spanning the whole extent
// (nav_log dumps, build 123: the Left view framed on four sides, every 3D
// track hitting the frame 3% in). Removed: a run of columns (then rows), each
// one unbroken over 60% of the extent with the same ends, with background
// beside the run on one side and at most 60% model on the other. A
// creature's columns end at different rows from their neighbours and have
// model beside them.
static int RemoveStraight(std::vector<unsigned char>& m, int gw, int gh, bool cols)
{
	const int n = cols ? gw : gh, len = cols ? gh : gw;
	auto at = [&](int line, int k) -> unsigned char& {
		return cols ? m[static_cast<size_t>(k) * gw + line] : m[static_cast<size_t>(line) * gw + k];
	};
	// The extent across the lines.
	int lo = len, hi = -1;
	for (int line = 0; line < n; ++line)
		for (int k = 0; k < len; ++k)
			if (at(line, k) == 2) { lo = std::min(lo, k); hi = std::max(hi, k); }
	if (hi <= lo)
		return 0;
	// 60%: the side plane's edge in the dumps runs 70% of the height.
	const int need = static_cast<int>((hi - lo + 1) * 0.6);
	// Each line's longest unbroken model run.
	std::vector<int> a(n, -1), b(n, -1);
	for (int line = 0; line < n; ++line) {
		int start = -1;
		for (int k = 0; k <= len; ++k) {
			const bool on = k < len && at(line, k) == 2;
			if (on && start < 0)
				start = k;
			if (!on && start >= 0) {
				if (k - start > b[line] - a[line] + 1 || a[line] < 0) { a[line] = start; b[line] = k - 1; }
				start = -1;
			}
		}
	}
	auto straight = [&](int line) { return line >= 0 && line < n && a[line] >= 0 && b[line] - a[line] + 1 >= need; };
	int removed = 0;
	for (int line = 0; line < n;) {
		if (!straight(line)) {
			++line;
			continue;
		}
		int end = line;
		while (straight(end + 1) && abs(a[end + 1] - a[line]) <= 2 && abs(b[end + 1] - b[line]) <= 2)
			++end;
		const int from = a[line], to = b[line];
		// (Not limited by width: build 155 kept wide runs as the model, and
		// kept a ZBrush palette strip recorded beside the DeathClaw's arm, the
		// same image columns in Front and Left, cut off at the canvas's edge.)
		// Clear on one side, at most 60% model on the other (the frame's top
		// edge with the head touching it, 52% in the Left dump; a creature's
		// own edge has its body right beside it). Where model is on both sides
		// something crosses the line (the blockout's leg through the frame's
		// bars) and counts on neither; its cells stay.
		auto model = [&](int l, int k) { return l >= 0 && l < n && at(l, k) == 2; };
		auto crossing = [&](int k) { return model(line - 2, k) && model(end + 2, k); };
		int onlyBefore = 0, onlyAfter = 0;
		for (int k = from; k <= to; ++k) {
			const bool b0 = model(line - 2, k), b1 = model(end + 2, k);
			onlyBefore += b0 && !b1;
			onlyAfter += b1 && !b0;
		}
		const double before = static_cast<double>(onlyBefore) / (to - from + 1), after = static_cast<double>(onlyAfter) / (to - from + 1);
		if (std::min(before, after) <= 0.25 && std::max(before, after) <= 0.6) {
			// The line and its own edge (a cell either side of it).
			for (int l = line - 1; l <= end + 1; ++l)
				if (l >= 0 && l < n)
					for (int k = from; k <= to; ++k)
						if (at(l, k) == 2 && !crossing(k)) {
							at(l, k) = 1;
							++removed;
						}
		}
		line = end + 1;
	}
	return removed;
}

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
	// Columns, rows, then both again (the extent shrinks as the frame goes).
	// A model that is itself a box (an early blockout) would go too: under a
	// quarter of it left keeps the silhouette as it was. (Build 124 kept it
	// at over half removed, and the Front view's face-on image plane is
	// about half: nav_log, view 0, 0 cells removed.)
	e.clean = v.mask;
	for (int pass = 0; pass < 2; ++pass) {
		e.removed += RemoveStraight(e.clean, v.gw, v.gh, true);
		e.removed += RemoveStraight(e.clean, v.gw, v.gh, false);
	}
	if (e.removed * 4 > std::count(v.mask.begin(), v.mask.end(), 2) * 3) {
		e.clean = v.mask;
		e.removed = 0;
	}
	e.rowMin.assign(v.gh, -1);
	e.rowMax.assign(v.gh, -1);
	e.colMin.assign(v.gw, -1);
	e.colMax.assign(v.gw, -1);
	e.i0 = v.gw;
	e.j0 = v.gh;
	e.i1 = e.j1 = -1;
	for (int j = 0; j < v.gh; ++j)
		for (int i = 0; i < v.gw; ++i) {
			if (e.clean[static_cast<size_t>(j) * v.gw + i] != 2)
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
// d[0] screen right, d[1] screen down, d[2] towards the camera. (Build 151
// turned the camera to the far side, reading the rows as right, down and
// into the screen; the Left view then showed the tail for the hand. A
// creature facing left in the Left image faces the Front camera this way.)
// ZBrush's rotation copies (and the view codes read from them) hold the
// camera's rotation transposed, with x's couplings the other way: the
// camera's own is S m^T S, S = diag(-1, 1, 1). Snapped Front, Back, Right and
// Left read the same either way, and so did turning about the vertical axis
// alone; tilted, frames from different angles disagreed about where the
// model was (builds 175-177: 34-62% of points impossible between angles;
// build 178's saved frames: 4.5% this way, the outline's noise, against
// 30% as read). Top and Bottom swap.
static void RotFix(const double m[9], double out[9])
{
	static const double s[3] = {-1, 1, 1};
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = s[r] * m[c * 3 + r] * s[c];
}

static void ViewAxes(int slot, double d[3][3])
{
	int code = kViewCodes[slot];
	double raw[9], m[9];
	for (int i = 0; i < 9; ++i, code /= 3)
		raw[i] = code % 3 - 1;
	RotFix(raw, m);
	for (int c = 0; c < 3; ++c) {
		d[0][c] = m[c];
		d[1][c] = -m[3 + c];
		d[2][c] = m[6 + c];
	}
}

// A view's screen axis a as a model axis: which one (0-2) and its sign.
static void ScreenAxis(const double d[3][3], int a, int& m, double& s)
{
	m = 0;
	s = 0;
	for (int t = 0; t < 3; ++t)
		if (fabs(d[a][t]) > 0.5) { m = t; s = d[a][t]; }
}

// The visible surface at a point of C's view, by the silhouettes: a point is
// the model's only if every other view with a silhouette sees model there
// (the views' visual hull), and what C shows is the first such point from its
// camera. ZBrush's own depth (PixolPick) is documented as broken and is not
// used.
//
// Points are in model units (G): along each model axis, the distance from the
// model's pivot as the canvas shows it at ratio 1. Every view's image is
// placed by its anchor (the pivot's image point) and scale, as Classic places
// it (ViewLayout), so a view's image point is G = s * (img - anchor) * f per
// screen axis, f = gAnchorScale / scale. Builds 137-154 measured points as a
// share of each silhouette's bounds instead, taking a silhouette for the
// whole model; the DeathClaw's were recorded zoomed in, cut off at the
// canvas's edge (the leg, and the head and hands beyond it), so the views
// disagreed about where things were and every ray met the leg's cut edge.
struct ViewFrame {
	const View* v = nullptr;
	int m[2] = {0, 0};  // each screen axis as a model axis, and its sign
	double s[2] = {0, 0};
	double f = 1;
	double anchor[2] = {0, 0};  // with the offset, for the showing view
};

static ViewFrame FrameFor(int slot, double offX, double offY)
{
	ViewFrame fr;
	const View& v = gViews[slot];
	fr.v = &v;
	double d[3][3];
	ViewAxes(slot, d);
	for (int a = 0; a < 2; ++a)
		ScreenAxis(d, a, fr.m[a], fr.s[a]);
	fr.f = gAnchorScale > 0 && v.scale > 0 ? gAnchorScale / v.scale : 1;
	fr.anchor[0] = v.ax + offX;
	fr.anchor[1] = v.ay + offY;
	return fr;
}

// Image point along screen axis a to model units, and back.
static double ToModel(const ViewFrame& fr, int a, double img)
{
	return fr.s[a] * (img - fr.anchor[a]) * fr.f;
}

static double ToImage(const ViewFrame& fr, int a, double g)
{
	return fr.anchor[a] + fr.s[a] * g / fr.f;
}

// A run of cells along screen axis a (lo to hi) in model units, low first.
static void CellsToModel(const ViewFrame& fr, int a, int lo, int hi, double& g0, double& g1)
{
	const double origin = a == 0 ? fr.v->u0 : fr.v->v0;
	const double x = ToModel(fr, a, origin + lo * fr.v->cell), y = ToModel(fr, a, origin + (hi + 1) * fr.v->cell);
	g0 = std::min(x, y);
	g1 = std::max(x, y);
}

static unsigned HashDouble(unsigned h, double x)
{
	unsigned long long b;
	memcpy(&b, &x, sizeof b);
	return h * 31 + static_cast<unsigned>(b ^ (b >> 32));
}

// The views C's rays are cast against, set up once per layout.
struct HullSet {
	struct Hull { ViewFrame fr; const unsigned char* mask; };
	Hull hulls[kViewSlots];
	int nh = 0;
	int md = 0;  // C's camera axis as a model axis, and its sign
	double sd = 0;
	double dLo = 0, dHi = 0;  // the depths the hull views saw model at
	unsigned sum = 0;  // the silhouettes and anchors it was made from
};

static void BuildHulls(int c, HullSet& hs)
{
	double dc[3][3];
	ViewAxes(c, dc);
	ScreenAxis(dc, 2, hs.md, hs.sd);
	const int md = hs.md;
	int nh = 0;
	hs.sum = HashDouble(static_cast<unsigned>(c), gAnchorScale);
	bool any = false;
	for (int w = 0; w < kDefaultView; ++w) {
		if (w == c || !gViews[w].used)
			continue;
		const MaskExtent& e = ExtentFor(w);
		hs.sum = hs.sum * 31 + e.sum + static_cast<unsigned>(e.size) * 7 + w;
		hs.sum = HashDouble(HashDouble(HashDouble(hs.sum, gViews[w].ax), gViews[w].ay), gViews[w].scale);
		if (!e.ok)
			continue;
		HullSet::Hull h;
		h.fr = FrameFor(w, 0, 0);
		h.mask = e.clean.data();
		const int ad = h.fr.m[0] == md ? 0 : h.fr.m[1] == md ? 1 : -1;
		if (ad < 0)
			continue;
		double g0, g1;
		CellsToModel(h.fr, ad, ad == 0 ? e.i0 : e.j0, ad == 0 ? e.i1 : e.j1, g0, g1);
		hs.dLo = any ? std::min(hs.dLo, g0) : g0;
		hs.dHi = any ? std::max(hs.dHi, g1) : g1;
		any = true;
		hs.hulls[nh++] = h;
	}
	hs.nh = nh;
}

// The run of model through cell (i, j) of a silhouette along one image axis
// (along 0 across, 1 down), and how far the model under that run reaches the
// other way (lo to hi, in cells): from a leg, down to the foot; from the
// hand, down to the claw tips.
static void RunReach(const unsigned char* mask, int gw, int gh, int i, int j, int along, int& lo, int& hi)
{
	auto at = [&](int p, int q) {
		const int x = along == 0 ? p : q, y = along == 0 ? q : p;
		return x >= 0 && y >= 0 && x < gw && y < gh && mask[static_cast<size_t>(y) * gw + x] == 2;
	};
	const int n = along == 0 ? gw : gh, nq = along == 0 ? gh : gw;
	const int p0 = along == 0 ? i : j, q0 = along == 0 ? j : i;
	int a = p0, b = p0;
	while (a > 0 && at(a - 1, q0))
		--a;
	while (b < n - 1 && at(b + 1, q0))
		++b;
	const int stride = std::max(1, (b - a) / 16);
	auto any = [&](int q) {
		for (int p = a; p <= b; p += stride)
			if (at(p, q))
				return true;
		return at(b, q);
	};
	lo = hi = q0;
	while (lo > 0 && any(lo - 1))
		--lo;
	while (hi < nq - 1 && any(hi + 1))
		++hi;
}

// A hull view's screen axis that is not C's depth: the model axis it shares
// with C's screen.
static int SharedAxis(const HullSet& hs, const HullSet::Hull& h)
{
	return h.fr.m[0] == hs.md ? 1 : 0;
}

// The ray at g[] (model units; C's two screen axes come in, the depth along
// its camera axis goes out, and back is where the ray leaves the model
// again: the far side of the part it hit).
// Two silhouettes make phantoms: looking at the crotch from the front, the
// Left silhouette's row there holds the hand, in front of the legs, so the
// ray down the leg is inside both silhouettes at the hand's depth (build
// 152 showed the hand for the crotch). With want (per hull view, the reach
// of the part C shows along the axis the two share, from RunReach in C), each
// solid stretch of the ray is checked against the part it is in in the hull
// view: the leg reaches the foot in both, the hand stops at the claws. A
// later stretch is taken when it matches clearly better (by a tenth of the
// depth seen). False when no view sees across C's depth or the ray misses.
static bool CastHull(const HullSet& hs, double g[3], double& back, const double (*want)[2] = nullptr)
{
	const int md = hs.md, nh = hs.nh;
	const double sd = hs.sd, range = hs.dHi - hs.dLo;
	if (!nh || range <= 0)
		return false;
	auto at = [&](int k) { return sd > 0 ? hs.dHi - k * range / 200 : hs.dLo + k * range / 200; };
	auto cellOf = [&](const HullSet::Hull& h, int cell[2]) {
		for (int a = 0; a < 2; ++a) {
			const double img = ToImage(h.fr, a, g[h.fr.m[a]]);
			cell[a] = static_cast<int>(floor((img - (a == 0 ? h.fr.v->u0 : h.fr.v->v0)) / h.fr.v->cell));
		}
	};
	// How far the stretch entered at k is from the part C shows.
	auto mismatch = [&](int k) {
		g[md] = at(k);
		double off = 0;
		for (int t = 0; t < nh; ++t) {
			const HullSet::Hull& h = hs.hulls[t];
			const int ao = SharedAxis(hs, h);
			int cell[2], lo, hi;
			cellOf(h, cell);
			RunReach(h.mask, h.fr.v->gw, h.fr.v->gh, cell[0], cell[1], 1 - ao, lo, hi);
			double gl, gh;
			CellsToModel(h.fr, ao, lo, hi, gl, gh);
			off += fabs(gl - want[t][0]) + fabs(gh - want[t][1]);
		}
		return off;
	};
	// From the camera's side inwards, in steps of half a percent.
	int entered = -1, bestIn = -1, bestOut = -1;
	double bestOff = 0;
	for (int k = 0; k <= 200; ++k) {
		g[md] = at(k);
		bool inside = true;
		for (int t = 0; t < nh && inside; ++t) {
			const HullSet::Hull& h = hs.hulls[t];
			int cell[2];
			cellOf(h, cell);
			inside = cell[0] >= 0 && cell[1] >= 0 && cell[0] < h.fr.v->gw && cell[1] < h.fr.v->gh &&
				h.mask[static_cast<size_t>(cell[1]) * h.fr.v->gw + cell[0]] == 2;
		}
		if (inside && entered < 0)
			entered = k;
		if (entered >= 0 && (!inside || k == 200)) {
			const int out = inside ? k : k - 1;
			if (!want) {
				bestIn = entered;
				bestOut = out;
				break;
			}
			const double off = mismatch(entered);
			if (bestIn < 0 || off < bestOff - 0.1 * range) {
				bestIn = entered;
				bestOut = out;
				bestOff = off;
			}
			entered = -1;
		}
	}
	if (bestIn < 0)
		return false;
	back = at(bestOut);
	g[md] = at(bestIn);
	return true;
}

// The depth of the surface round the middle of C's viewport, in model units
// along C's camera axis: the surface behind each silhouette cell within reach of
// the middle, averaged with weights falling smoothly to nothing at the reach.
// One ray at the middle (builds 137-148) flipped between the surface and the
// band's middle whenever the middle crossed a gap between fingers or the
// outline: half the model in the side view, several times a second panning
// across the hand. The cells are fixed on the model (every step-th cell of
// C's grid), so moving the camera only shifts their weights and the depth
// moves smoothly. Each cell's depth is kept until a silhouette changes.
// back is the far side of the parts hit, averaged the same way. False when
// no surface is within reach; samples is how many were.
static bool SurfaceDepth(int c, const ViewFrame& cf, const double centreC[2], double reach, double& depth, double& back, int& samples)
{
	samples = 0;
	const View& cv = gViews[c];
	const MaskExtent& ce = ExtentFor(c);
	if (!ce.ok || reach <= 0 || cv.cell <= 0)
		return false;
	static HullSet hs;
	BuildHulls(c, hs);
	if (!hs.nh)
		return false;
	// Per cell of C's grid, the near and far side; NaN not cast yet, -1e30
	// missed. Kept while the silhouettes and C's placement stay.
	static std::vector<float> cache;
	static unsigned cacheSum = 0;
	const unsigned key = HashDouble(HashDouble(HashDouble(hs.sum, cf.anchor[0]), cf.anchor[1]), cf.f);
	const size_t n = static_cast<size_t>(cv.gw) * cv.gh;
	if (cacheSum != key || cache.size() != n * 2) {
		cache.assign(n * 2, NAN);
		cacheSum = key;
	}
	const int* m = cf.m;
	// Reach in cells; about 25 cells across it whatever the zoom.
	const double r = reach / cv.cell;
	int step = 1;
	while (r / step > 12)
		step *= 2;
	const double ci = (centreC[0] - cv.u0) / cv.cell - 0.5, cj = (centreC[1] - cv.v0) / cv.cell - 0.5;
	const int i0 = std::max(0, static_cast<int>(ceil((ci - r) / step)) * step), i1 = std::min(cv.gw - 1, static_cast<int>(floor(ci + r)));
	const int j0 = std::max(0, static_cast<int>(ceil((cj - r) / step)) * step), j1 = std::min(cv.gh - 1, static_cast<int>(floor(cj + r)));
	double sum = 0, sumBack = 0, weights = 0;
	for (int j = j0; j <= j1; j += step)
		for (int i = i0; i <= i1; i += step) {
			const double d2 = ((i - ci) * (i - ci) + (j - cj) * (j - cj)) / (r * r);
			const size_t at = static_cast<size_t>(j) * cv.gw + i;
			if (d2 >= 1 || ce.clean[at] != 2)
				continue;
			float& d = cache[at * 2];
			float& rearAt = cache[at * 2 + 1];
			if (std::isnan(d)) {
				double g[3] = {0, 0, 0};
				const double img[2] = {cv.u0 + (i + 0.5) * cv.cell, cv.v0 + (j + 0.5) * cv.cell};
				for (int b = 0; b < 2; ++b)
					g[m[b]] = ToModel(cf, b, img[b]);
				// The reach of the part this cell is in, in C, along each
				// hull view's shared axis (CastHull).
				double want[kViewSlots][2];
				for (int t = 0; t < hs.nh; ++t) {
					const HullSet::Hull& h = hs.hulls[t];
					const int ms = h.fr.m[SharedAxis(hs, h)];
					const int bs = m[0] == ms ? 0 : 1;
					int lo, hi;
					RunReach(ce.clean.data(), cv.gw, cv.gh, i, j, 1 - bs, lo, hi);
					CellsToModel(cf, bs, lo, hi, want[t][0], want[t][1]);
				}
				double b = 0;
				const bool hit = CastHull(hs, g, b, want);
				d = hit ? static_cast<float>(g[hs.md]) : -1e30f;
				rearAt = hit ? static_cast<float>(b) : -1e30f;
			}
			if (d < -1e29f)
				continue;
			const double w = (1 - d2) * (1 - d2);
			sum += w * d;
			sumBack += w * rearAt;
			weights += w;
			++samples;
		}
	if (weights <= 0)
		return false;
	depth = sum / weights;
	back = sumBack / weights;
	return true;
}

// ZBrush's canvas depth (defined with the scan for it, DepthScan).
static bool DepthFound(double& empty);
static bool DepthAtDoc(double x, double y, double& v);
static bool DepthSame(double a, double b);
static bool CanvasDepth(double x, double y, double reach, double kp, double& z, double& width, int& samples);
// The camera as the depth planes need it: its axes (as ViewAxes gives them),
// the pivot's document point, the zoom, and the document's middle, which
// perspective is taken about.
// A point's depth Z towards the camera is (pivot - value) / zoom model units
// (DepthZ), pivot the model's pivot depth ZBrush holds (PivotDepth); across,
// a document point is the pivot's offset / zoom scaled by 1 - Z * invD
// (perspective: one over the eye's distance from the pivot; 0, flat, as
// ZBrush draws with Persp off).
// Checked against both silhouettes (the visual hull's front along each row
// of a frame): turned 13-16 degrees off Front at zoom 2.65 to 6.21, off by
// 5-7 with the pivot and 280-366 without (build 196 left it out, and the side
// cell showed the cheek and ear for the nose); snapped to Front, where ZBrush
// puts the pivot to 0, off by 3. And frames up to 70 degrees apart agree:
// 0-2% of points impossible. Builds 177-197 fitted a depth scale, offset and
// perspective to the frames and silhouettes instead (to make up for a pivot
// held through snaps), and what they fitted (x1.114 -38, eye at 1800) put
// 40-63% of points impossible: the front cell on the hair, from the side.
// invD: perspective (ProjInvD), 0 flat.
struct Cam {
	double d[3][3];
	double px, py, r, pivot, cx, cy;
	double invD = 0;
};

// ZBrush's projection (Draw > Persp), from its own record in ZBrush.exe's
// data (x64dbg, ZBrush 2023, build 207): floats docW/2 and docH/2 (the
// perspective's centre), 0, the eye's distance in document pixels, an int of
// switches (bit 0 Persp; 0x559 with it on, 0x558 off, in Horizontal: bit 6 is
// not Vertical), the field of view in degrees and the crop factor. The
// distance holds the field of view, focal length and crop: half the
// document's width / tan(half the angle) * crop, 1415.28 for 50 mm on 1019 x
// 776 up to zoom 1.38, then 1026.9 x the zoom (0.7256 of it: 1.568 to 3.088,
// build 213's log); it moves during drags (to 1524) and settles back. Two
// copies; only one followed the Draw palette, so the one that changed last
// is read. ZBrush draws about the document's middle with the eye at that
// distance (E, document pixels): a point Z = zoom * depth in front of the
// pivot's plane at E / (E - Z) times its flat place, and its depth value
// pivot - 2E - Z * E / (E - Z) (DepthZ: ZBrush's depth dumped at zooms 0.91,
// 1 and 2.33, within 0.08-0.18 of 650). Builds 207-213 took the eye as 1415.28
// x the square root of the zoom, fitted to those dumps' outlines, which near
// zoom 1 tell the two apart by under a pixel; at 2.33 their depth fit it 8
// off, so 208-213 fitted the depth's constant to the points per frame. The
// zoom is the overlay's ratio (ZBrush's scale over the anchor's, 40.83 in
// that project).
static bool ReadOwn(uintptr_t at, void* out, size_t n);
struct ProjRec { uintptr_t at; unsigned char last[0x1c]; DWORD changedAt; };
static std::vector<ProjRec> gProj;
static double gProjDocW = 0, gProjDocH = 0;
static DWORD gProjFoundAt = 0;
static double gTestProjD0 = 0;  // tests: Persp with this eye distance (0 flat)

// ZBrush.exe is packed: its code and data are one section of 433 MB, its
// pages execute-read-write (build 207 looked only in read-write and
// write-copy pages, and found none). The two places the records were found
// (ZBrush 2023) are looked at first; the whole image only when they are not
// records, at most every 30 s, in 1 MB pieces. counts: candidates dropped by
// each check, for the log.
static const uintptr_t kProjKnown[2] = {0xEC476C8, 0xEC482C8};

static std::string gProjNotes;  // each record with sane values, for the log

static bool ProjIs(const unsigned char* b, double docW, double docH, int counts[4])
{
	float c[2], d0, fov, crop;
	uint32_t zero, flags;
	memcpy(c, b, 8);
	if (c[0] != static_cast<float>(docW * 0.5) || c[1] != static_cast<float>(docH * 0.5))
		return false;
	++counts[0];
	memcpy(&zero, b + 8, 4);
	memcpy(&d0, b + 12, 4);
	memcpy(&flags, b + 16, 4);
	memcpy(&fov, b + 20, 4);
	memcpy(&crop, b + 24, 4);
	if (zero || !(d0 > 10 && d0 < 1e7) || flags > 0xffff || !(fov > 0.5 && fov < 179.5) || !(crop > 0.01 && crop < 100))
		return false;
	++counts[1];
	// The eye from the field of view across the width or the height, with
	// the crop or without, or farther: past zoom 1.38 ZBrush keeps it at
	// 0.7256 x the zoom x that (build 214 asked for it exactly, and a session
	// started at zoom 2.35, eye 2555, found no record: Persp unknown, its
	// frames read as flat).
	const double t = tan(fov * kPi / 360);
	const double wants[4] = {docW * 0.5 / t * crop, docH * 0.5 / t * crop, docW * 0.5 / t, docH * 0.5 / t};
	bool fits = false;
	for (double want : wants)
		fits = fits || (d0 / want >= 0.99 && d0 / want <= 1000);
	if (gProjNotes.size() < 600) {
		char note[160];
		snprintf(note, sizeof note, " [eye %.3f, switches %x, field of view %.4f, crop %.4f; width gives %.3f, height %.3f%s]", d0, flags, fov, crop, wants[0], wants[1], fits ? "" : ": not taken");
		gProjNotes += note;
	}
	if (!fits)
		return false;
	++counts[2];
	return true;
}

static void ProjFind(double docW, double docH)
{
	gProj.clear();
	gProjDocW = docW;
	gProjDocH = docH;
	gProjFoundAt = GetTickCount();
	const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	MODULEINFO mi = {};
	GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof mi);
	int counts[4] = {};
	gProjNotes.clear();
	auto take = [&](uintptr_t at, const unsigned char* b) {
		ProjRec r = {at, {}, 0};
		memcpy(r.last, b, sizeof r.last);
		gProj.push_back(r);
	};
	for (uintptr_t off : kProjKnown) {
		unsigned char b[0x1c];
		if (off + sizeof b <= mi.SizeOfImage && ReadOwn(exe + off, b, sizeof b) && ProjIs(b, docW, docH, counts))
			take(exe + off, b);
	}
	const bool known = !gProj.empty();
	size_t scanned = 0;
	if (!known) {
		memset(counts, 0, sizeof counts);
		const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		std::vector<unsigned char> buf;
		for (uintptr_t a = exe; a < exe + mi.SizeOfImage;) {
			MEMORY_BASIC_INFORMATION m;
			if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof m) != sizeof m)
				break;
			const uintptr_t end = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize, exe + mi.SizeOfImage);
			if (m.State == MEM_COMMIT && (m.Protect & writable) && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				for (uintptr_t at = a; at < end; at += 1 << 20) {
					// Each piece read 0x1c past its end, for a record across two.
					const size_t n = static_cast<size_t>(std::min<uintptr_t>(end - at, (1 << 20) + 0x1c));
					buf.resize(n);
					if (!ReadOwn(at, buf.data(), n))
						continue;
					scanned += n;
					for (size_t o = 0; o + 0x1c <= n && o < (1u << 20); o += 4)
						if (ProjIs(&buf[o], docW, docH, counts))
							take(at + o, &buf[o]);
				}
			a = end;
		}
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu projection records for %.0f x %.0f: %zu%s", GetTickCount(), docW, docH, gProj.size(), known ? " where found before" : "");
		for (const ProjRec& r : gProj)
			fprintf(f, " [+%llx]", static_cast<unsigned long long>(r.at - exe));
		if (!known)
			fprintf(f, " (%.0f MB of %.0f looked at; %d with the document's middle, %d of them with sane values, %d with the eye where the field of view puts it)",
				scanned / 1048576.0, mi.SizeOfImage / 1048576.0, counts[0], counts[1], counts[2]);
		fprintf(f, "%s", gProjNotes.c_str());
		fprintf(f, "\n");
		fclose(f);
	}
}

// Persp on and the eye's distance (document pixels, at the reference zoom),
// from the record that changed last. False if no record is found.
static bool ProjRead(double docW, double docH, bool& persp, double& d0)
{
	if (docW != gProjDocW || docH != gProjDocH || (gProj.empty() && GetTickCount() - gProjFoundAt > 30000))
		ProjFind(docW, docH);
	const ProjRec* use = nullptr;
	for (ProjRec& r : gProj) {
		unsigned char now[sizeof r.last];
		if (!ReadOwn(r.at, now, sizeof now))
			continue;
		if (memcmp(now, r.last, sizeof now)) {
			memcpy(r.last, now, sizeof now);
			r.changedAt = GetTickCount();
		}
		if (!use || static_cast<int>(r.changedAt - use->changedAt) > 0)
			use = &r;
	}
	if (!use)
		return false;
	float f;
	uint32_t flags;
	memcpy(&f, use->last + 12, 4);
	memcpy(&flags, use->last + 16, 4);
	persp = (flags & 1) != 0;
	d0 = f;
	// Logged at each switch of Persp, and for the eye once it has held a
	// second: it moves through drags and settles back (build 211: 1415 to 1524
	// and back, 30 lines a second).
	static int lastPersp = -1;
	static double lastD0 = 0, heldD0 = 0;
	static DWORD heldSince = 0;
	if (d0 != heldD0) {
		heldD0 = d0;
		heldSince = GetTickCount();
	}
	// Persp switched (by hand or by Auto Persp): what the mode showing wants
	// from now on (RSIsOn "perspwanted").
	if (lastPersp >= 0 && persp != (lastPersp == 1))
		gPerspFor[gMode == 1] = persp;
	if (persp != (lastPersp == 1) || lastPersp < 0 || (fabs(d0 - lastD0) > 0.01 && GetTickCount() - heldSince >= 1000)) {
		lastPersp = persp;
		lastD0 = d0;
		if (FILE* fl = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			float fov, crop;
			memcpy(&fov, use->last + 20, 4);
			memcpy(&crop, use->last + 24, 4);
			fprintf(fl, "%lu projection: Persp %s, eye at %.2f (field of view %.4f, crop %.4f, switches %x)\n", GetTickCount(), persp ? "on" : "off", d0, fov, crop, flags);
			fclose(fl);
		}
	}
	return true;
}

// The camera's perspective at zoom r: one over the eye's distance from the
// pivot's plane in model units (Cam::invD), 0 with Persp off: the zoom over
// ZBrush's eye in document pixels. The tests give the eye below zoom 1.38 and
// follow ZBrush's record above it.
static double ProjInvD(double docW, double docH, double r)
{
	if (r <= 0)
		return 0;
	if (gTesting)
		return gTestProjD0 > 0 ? r / std::max(gTestProjD0, 0.72562 * r * gTestProjD0) : 0;
	if (gOrbitOn)
		return r / gOrbitEye;
	bool persp;
	double eye;
	return ProjRead(docW, docH, persp, eye) && persp && eye > 0 ? r / eye : 0;
}

// A depth plane's value as depth towards the camera, model units. pivot: the
// value at depth 0. With Persp on, ZBrush's value is that less zoom * z / (1
// - z * invD), and that is ZBrush's pivot less twice the eye's distance
// (TakeFrame).
static double DepthZ(const Cam& c, double value)
{
	const double w = (c.pivot - value) / c.r;
	return c.invD > 0 ? w / (1 + w * c.invD) : w;
}

static void CamUnproject(const Cam& c, double x, double y, double z, double invD, double g[3])
{
	const double s = 1 - z * invD;
	const double ax = ((x - c.cx) * s - (c.px - c.cx)) / c.r, ay = ((y - c.cy) * s - (c.py - c.cy)) / c.r;
	for (int t = 0; t < 3; ++t)
		g[t] = ax * c.d[0][t] + ay * c.d[1][t] + z * c.d[2][t];
}

static bool CamProject(const Cam& c, const float g[3], double invD, double& x, double& y, double& z)
{
	double s[3];
	for (int a = 0; a < 3; ++a)
		s[a] = g[0] * c.d[a][0] + g[1] * c.d[a][1] + g[2] * c.d[a][2];
	z = s[2];
	const double q = 1 - z * invD;
	if (q <= 0.05)
		return false;
	x = c.cx + (s[0] * c.r + (c.px - c.cx)) / q;
	y = c.cy + (s[1] * c.r + (c.py - c.cy)) / q;
	return true;
}

static bool CloudDepth(const Cam& c, double qx, double qy, double reach, double& front, double& thick, int& samples);
static bool CloudHas();
static bool LiveCam(const OverlayState& st, const double d[3][3], Cam& c);
static void CamPoint(const Cam& c, double x, double y, double z, double g[3]);
static bool CameraAxes(double d[3][3]);
static void RotDump(FILE* f);
// Canvas readings since the last multiview log line: used, not covering the
// screen's model, still being drawn.
static int gCanvasUsed = 0, gCanvasUncovered = 0, gCanvasDrawing = 0;

// Multiview: a view other than the one being looked at (C, the showing
// view) shows the same part of the model. Both views' anchors (ViewFrame)
// place their images on the model, so an axis the two views share (height,
// between Front and Left) maps across: the same model point, zoomed alike.
// Along the
// axis that is depth for C (across, in Left seen from Front), the part being
// looked at is centred: the model's extent there within the band shown, so
// zoomed into the face, the side view shows the head. Zoomed in closer than
// the head's depth, it goes to the surface C shows (SurfaceDepth: the nose, not
// the middle of the skull). Without both silhouettes the view is placed by
// the camera as Classic would show it.
static bool MultiviewLayout(int slot, const OverlayState& st, OverlayState& out, std::string& path)
{
	if (!ViewLayout(slot, st, out, path))
		return false;
	// The camera: at view c when snapped to one (its image, if it has one,
	// only for the silhouettes); its axes from ZBrush's rotation at any angle
	// (turned freely, in perspective, in a view without an image: builds
	// 137-174 tracked only snapped to a view with an image), a snapped view's
	// when the rotation copies are not found.
	const int c = gActiveView >= 0 && gActiveView < kDefaultView && !gViewAway ? gActiveView : -1;
	// The view snapped to shows its image as Classic does, on the pivot's
	// plane; in perspective the part looked at is nearer or farther and drawn
	// larger or smaller, so that view is laid out like the others then.
	const bool persp = ProjInvD(st.docW, st.docH, st.ratio) > 0;
	if ((c == slot && !persp) || st.ratio <= 0)
		return true;
	const bool haveC = c >= 0 && gViews[c].used;
	double dc[3][3];
	if (!CameraAxes(dc)) {
		if (c < 0)
			return true;
		ViewAxes(c, dc);
	}
	// Whether the camera has turned since this slot's last layout: neither
	// easing below applies then (see the depth's).
	bool turning = false;
	if (slot >= 0 && slot < kViewSlots) {
		static double lastDc[kViewSlots][3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int k = 0; k < 3; ++k)
				turning = turning || fabs(dc[i][k] - lastDc[slot][i][k]) > 1e-4;
		memcpy(lastDc[slot], dc, sizeof lastDc[slot]);
	}
	const MaskExtent& ve = ExtentFor(slot);
	const View& vv = gViews[slot];
	const ViewFrame vf = FrameFor(slot, 0, 0);
	// C's window in its image space: centre and half size per screen axis.
	// The window is the part of the viewport beside the panel, where the
	// model is worked on and which the cell shows (DrawPanelCell). Builds
	// 137-151 took the whole viewport, whose middle is under the panel: the
	// surface was looked for right of what was being looked at, and the
	// part found sat a quarter of the cell right of its middle (the hand off
	// the cell's edge).
	int vx0, vy0, vx1, vy1;
	const double kp = st.pixelsPerDoc > 0 ? st.pixelsPerDoc : 1;
	{
		const int w = static_cast<int>(st.docW * kp + 0.5), h = static_cast<int>(st.docH * kp + 0.5);
		ViewportRect(w, h, vx0, vy0, vx1, vy1);
		int px, py, pw, ph;
		PanelRect(w, h, px, py, pw, ph);
		if (gPanelLeft)
			vx0 = px + pw;
		else
			vx1 = px;
	}
	const double midX = (vx0 + vx1) * 0.5 / kp, midY = (vy0 + vy1) * 0.5 / kp;
	// The window's middle and half size in model units: from the pivot along
	// the camera's right and down, at zoom 1 (a view's image point is G = s *
	// (img - anchor) * f, and C's image placed by the camera reduces to this,
	// its anchor and scale cancelling: no image of C is needed).
	const double gx = (midX - st.px) / st.ratio, gy = (midY - st.py) / st.ratio;
	const double halfM[2] = {(vx1 - vx0) * 0.5 / kp / st.ratio, (vy1 - vy0) * 0.5 / kp / st.ratio};
	double centreV[2] = {0, 0}, halfV[2] = {0, 0};
	int depthAxis = -1;
	// The middle of the window in model units, and its depth: the surface the
	// camera shows there, along its axis towards the camera.
	double g[3];
	for (int t = 0; t < 3; ++t)
		g[t] = gx * dc[0][t] + gy * dc[1][t];
	Cam cam;
	const bool camOk = LiveCam(st, dc, cam);
	int samples = 0;
	bool focus = false, held = false, fromCanvas = false;
	double front = 0, thick = 0;  // the surface towards the camera, the part's thickness behind it
	{
		// From the points of ZBrush's finished frames (CloudDepth), never the
		// plane as it is: ZBrush draws it part by part nearly all the time
		// the view moves, and builds 164-171 read it mid-draw (the body behind
		// the face, a few pixels, an empty plane) however they checked it.
		// Within a quarter of the window's smaller side. With too little in
		// reach (a part no finished frame has shown yet, or the middle off the
		// model) the last reading stays: the silhouettes put the far side
		// elsewhere (build 172: -120 for the map's 268, the cell 60 px off
		// and back within 5 layouts), and are used only before any frame.
		static double heldFront = 0, heldThick = 0;
		static bool haveHeld = false;
		const double reach = 0.25 * std::min(vx1 - vx0, vy1 - vy0) / kp / st.ratio;
		// A surface on V's silhouette, or within 15% of its size of it: the
		// points read with a wrong pivot depth or depth scale sit off the model.
		auto onModel = [&](double depth) {
			if (!ve.ok)
				return true;
			double p[3];
			CamPoint(cam, midX, midY, depth, p);
			for (int a = 0; a < 2; ++a) {
				double g0, g1;
				CellsToModel(vf, a, a == 0 ? ve.i0 : ve.j0, a == 0 ? ve.i1 : ve.j1, g0, g1);
				const double margin = 0.15 * (g1 - g0);
				if (p[vf.m[a]] < g0 - margin || p[vf.m[a]] > g1 + margin)
					return false;
			}
			return true;
		};
		// The points first; snapped to a view with a silhouette, the
		// silhouettes when the points have nothing there. Builds 195-198 took
		// the silhouettes first when snapped, but two silhouettes only bound the
		// model (the visual hull): from the side the mouth's, chin's and
		// forehead's surface came out at the cheek's depth, 150-180 units off,
		// and the front cell off its window (dll\test). (Build 194's black side
		// cell, which they were for, was the points' pivot depth held from
		// before a snap, PivotDepth.)
		if (gAuto3D && camOk && CloudDepth(cam, midX, midY, reach, front, thick, samples) && onModel(front))
			focus = fromCanvas = true;
		if (!focus && gAuto3D && haveC) {
			OverlayState cs;
			std::string cpath;
			if (ViewLayout(c, st, cs, cpath) && cs.ratio > 0) {
				const ViewFrame cf = FrameFor(c, cs.offX, cs.offY);
				const double centreC[2] = {(midX - cs.px) / cs.ratio + cs.ax + cs.offX, (midY - cs.py) / cs.ratio + cs.ay + cs.offY};
				const double halfC[2] = {(vx1 - vx0) * 0.5 / kp / cs.ratio, (vy1 - vy0) * 0.5 / kp / cs.ratio};
				double cd[3][3], depth = 0, back = 0;
				ViewAxes(c, cd);
				int md;
				double sd;
				ScreenAxis(cd, 2, md, sd);
				if (SurfaceDepth(c, cf, centreC, 0.5 * std::min(halfC[0], halfC[1]), depth, back, samples)) {
					front = sd * depth;
					thick = front - sd * back;
					focus = true;
				}
			}
		}
		if (!focus && gAuto3D && haveHeld) {
			front = heldFront;
			thick = heldThick;
			focus = held = true;
		}
		if (focus && !held) {
			heldFront = front;
			heldThick = thick;
			haveHeld = true;
		}
		// A jump in the depth (the middle moving onto a part nearer or
		// farther: build 173, 140 to 297 in a frame, the cell 190 px) is eased
		// over about 150 ms, 50 ms a step of 1/e, so the cell glides there;
		// smaller changes, as the middle moves over one part, are followed as
		// they are. The layout is asked for again until it has arrived.
		// Not while the camera turns: the depth is along the camera's axis, so
		// the eased depth from before put on the turned ray is off the surface
		// (builds up to 216: a turn put the point 430 units behind the face,
		// the cell far off until 150 ms after letting go).
		if (focus) {
			static double easedFront = 0, easedThick = 0;
			static LARGE_INTEGER easedAt = {};
			static bool started = false, easing = false;
			LARGE_INTEGER t, qf;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&qf);
			if (started && !gTesting && !turning && (easing || fabs(front - easedFront) > 15 || fabs(thick - easedThick) > 15)) {
				// At most a frame's time a step (the layout is not asked for
				// while the camera is still: the first step after a pause
				// would otherwise be the whole jump).
				const double ms = std::min(33.0, 1000.0 * (t.QuadPart - easedAt.QuadPart) / qf.QuadPart);
				const double a = 1 - exp(-ms / 50.0);
				easedFront += (front - easedFront) * a;
				easedThick += (thick - easedThick) * a;
				easing = fabs(front - easedFront) > 0.5 || fabs(thick - easedThick) > 0.5;
			} else {
				easing = false;
			}
			if (!easing) {
				easedFront = front;
				easedThick = thick;
			}
			started = true;
			easedAt = t;
			front = easedFront;
			thick = easedThick;
			if (easing) {
				EnterCriticalSection(&gOverlayLock);
				gRedrawPending = true;
				LeaveCriticalSection(&gOverlayLock);
			}
			// The point on the ray through the window's middle at that depth
			// (in perspective the ray is not along the camera's axis).
			CamPoint(cam, midX, midY, front, g);
			if (!Turning()) {
				memcpy(gFocusG, g, sizeof gFocusG);
				gFocusOk = true;
			}
		}
	}
	{
		// Each silhouette once a session in the log, as text (# model, .
		// background, blank not seen; at most 100 columns), with its bounds
		// and axes: build 122 found every surface 3% in from the near edge,
		// at every height.
		static bool dumped[kViewSlots] = {};
		for (int w = 0; w < kDefaultView; ++w) {
			const View& v = gViews[w];
			if (dumped[w] || !v.used || v.mask.size() != static_cast<size_t>(v.gw) * v.gh || v.gw <= 0)
				continue;
			dumped[w] = true;
			const MaskExtent& e = ExtentFor(w);
			double d[3][3];
			ViewAxes(w, d);
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu silhouette of view %d (straight structures removed: %d cells): %d x %d cells of %.2f from %.1f %.1f, model cells %d..%d by %d..%d, right %+.0f %+.0f %+.0f down %+.0f %+.0f %+.0f towards %+.0f %+.0f %+.0f\n",
					GetTickCount(), w, e.removed, v.gw, v.gh, v.cell, v.u0, v.v0, e.i0, e.i1, e.j0, e.j1,
					d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2], d[2][0], d[2][1], d[2][2]);
				const int step = (v.gw + 99) / 100;
				for (int j = 0; j < v.gh; j += step) {
					std::string line;
					for (int i = 0; i < v.gw; i += step) {
						unsigned char best = 0;
						for (int dj = 0; dj < step && j + dj < v.gh; ++dj)
							for (int di = 0; di < step && i + di < v.gw; ++di)
								best = std::max(best, e.clean[static_cast<size_t>(j + dj) * v.gw + i + di]);
						line += best == 2 ? '#' : best == 1 ? '.' : ' ';
					}
					fprintf(f, "  |%s|\n", line.c_str());
				}
				fclose(f);
			}
		}
	}
	// The cell shows a window the size of the camera's (DrawPanelCell fits the
	// same screen area), so its half size per screen axis is the camera's, in
	// V's units. Its middle: the surface the camera shows at the window's
	// middle, so the cell shows in its middle what the viewport does. Builds
	// 150-198 moved it into the part by half its thickness (at most a quarter
	// of the window), for a creature's hand; on a head the front cell then
	// sat on the middle of the head for the side of the nose or an eye, and
	// the test suite (dll\test) found every cell a quarter to a third of the
	// window off the point looked at, at every angle and zoom.
	// In perspective the surface looked at is drawn larger by 1 / (1 - depth *
	// invD) than on the pivot's plane (CamProject): the cell shows its image
	// that much larger, and so a window that much smaller, to match it.
	double grow = 1;
	if (focus && cam.invD > 0)
		grow = 1 / std::max(0.05, 1 - front * cam.invD);
	double along = 0;
	int da = 0;
	for (int a = 0; a < 2; ++a) {
		halfV[a] = halfM[a] / vf.f / grow;
		if (fabs(dc[2][vf.m[a]]) > along) {
			along = fabs(dc[2][vf.m[a]]);
			da = a;
		}
	}
	for (int a = 0; a < 2; ++a)
		centreV[a] = ToImage(vf, a, g[vf.m[a]]);
	// The camera snapped, V seeing straight across its depth, and no surface
	// found: the band of V's silhouette the window spans along the axis they
	// share.
	if (along > 0.999 && !focus) {
		depthAxis = da;
		centreV[da] = vf.anchor[da];
	}
	if (depthAxis >= 0 && ve.ok) {
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
		if (lo >= 0)
			centreV[depthAxis] = across + (lo + hi + 1) * 0.5 * vv.cell;
	}
	{
		// Logged when what it went by changes: the views, whether the surface
		// was found, and where (model units; moves of a 25th of the window), at
		// most every 100 ms for moves. Builds 147-183 also read ZBrush's depth
		// plane over the window for the line, 25,000 reads of a few bytes:
		// 25-40 ms a layout, nearly every frame while the model moved (the
		// overlay at 21 fps, its reference stage 44 ms of a 51 ms frame).
		static int lastC = -1, lastSlot = -1, lastFocus = -1;
		static double lastG[3] = {0, 0, 0};
		static DWORD loggedAt = 0;
		const double tol = std::max(halfM[0], halfM[1]) * 2 / 25;
		const int how = held ? 2 : focus ? 1 : 0;
		const bool moved = fabs(g[0] - lastG[0]) > tol || fabs(g[1] - lastG[1]) > tol || fabs(g[2] - lastG[2]) > tol;
		if (c != lastC || slot != lastSlot || how != lastFocus || (moved && GetTickCount() - loggedAt >= 100)) {
			lastC = c;
			lastSlot = slot;
			lastFocus = how;
			loggedAt = GetTickCount();
			memcpy(lastG, g, sizeof lastG);
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu multiview %d from %d: %s (%d cells), model point %.0f %.0f %.0f (front %.0f, thick %.0f), towards the camera %+.2f %+.2f %+.2f, V centre %.0f %.0f half %.0f %.0f, depth axis %d\n",
					GetTickCount(), slot, c, held ? "surface held (none in reach)" : fromCanvas ? "surface from the canvas depth" : focus ? "surface from the silhouettes" : "band only",
					samples, g[0], g[1], g[2], front, thick, dc[2][0], dc[2][1], dc[2][2],
					centreV[0], centreV[1], halfV[0], halfV[1], depthAxis);
				fclose(f);
			}
		}
	}
	// Document pixels per V image pixel: the camera's per model unit, by V's
	// model units per image pixel.
	out.ratio = st.ratio * vf.f * grow;
	out.offX = out.offY = 0;
	// ZBrush's camera can jump in a frame (a zoom step: build 175, 595 to 382,
	// the cell 217 px; the model framed): a jump of the cell's middle by over
	// half its window, or of its zoom by over 15%, is eased over about 150 ms
	// like the depth's; moves smaller than that, as the view is panned, are
	// followed as they are. Not while the camera turns: the cell then
	// follows the turn as it is, not behind it.
	if (slot >= 0 && slot < kViewSlots) {
		struct Eased { double x, y, logR; LARGE_INTEGER at; bool started, easing; };
		static Eased eased[kViewSlots] = {};
		Eased& e = eased[slot];
		LARGE_INTEGER t, qf;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&qf);
		const double logR = log(out.ratio);
		const bool jump = fabs(centreV[0] - e.x) > halfV[0] * 0.5 || fabs(centreV[1] - e.y) > halfV[1] * 0.5 || fabs(logR - e.logR) > log(1.15);
		if (e.started && !gTesting && !turning && (e.easing || jump)) {
			const double ms = std::min(33.0, 1000.0 * (t.QuadPart - e.at.QuadPart) / qf.QuadPart);
			const double a = 1 - exp(-ms / 50.0);
			e.x += (centreV[0] - e.x) * a;
			e.y += (centreV[1] - e.y) * a;
			e.logR += (logR - e.logR) * a;
			e.easing = fabs(centreV[0] - e.x) > halfV[0] * 0.01 || fabs(centreV[1] - e.y) > halfV[1] * 0.01 || fabs(logR - e.logR) > 0.002;
		} else {
			e.easing = false;
		}
		if (!e.easing) {
			e.x = centreV[0];
			e.y = centreV[1];
			e.logR = logR;
		}
		e.started = true;
		e.at = t;
		centreV[0] = e.x;
		centreV[1] = e.y;
		out.ratio = exp(e.logR);
		if (e.easing) {
			EnterCriticalSection(&gOverlayLock);
			gRedrawPending = true;
			LeaveCriticalSection(&gOverlayLock);
		}
	}
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
		fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", gMode, gPanelLeft, gMulti, gMultiA, gMultiB, gBackGrey,
			gCells[0], gCells[1], gCells[2], gCells[3], gAuto3D, gGamma, gSat, gModelOpacity, gAutoPersp, gCellZoomOut);
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
		int v[16] = {0, 0, 0, 0, 3, 48, 0, 3, 4, 1, 1, 100, 100, 75, 0, 200};
		const int n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
			&v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
		gAuto3D = n < 11 || v[10] != 0;
		gAutoPersp = n >= 15 && v[14] != 0;  // build 207
		gCellZoomOut = n >= 16 ? std::max(100, std::min(v[15], 1000)) : 200;  // build 222
		// Gamma, Saturation and Model Opacity (build 142; older files keep the defaults).
		if (n >= 14) {
			gGamma = std::max(20, std::min(v[11], 300));
			gSat = std::max(0, std::min(v[12], 200));
			gModelOpacity = std::max(0, std::min(v[13], 100));
		}
		if (n >= 10)
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
// ZBrush's canvas depth at document points, read a row at a time: 1 the
// model, 0 the empty canvas, -1 unknown (no buffer found, off it, unread).
struct DepthRows {
	int row = -1;
	bool ok = false;
	std::vector<unsigned char> data;
	int At(double x, double y);
};
static bool DepthFound(double& empty);

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
	// Nor is anything else drawn over the canvas: a cell is only the model
	// where ZBrush's depth has the model too, and only the canvas where it
	// is empty (build 202: a floating palette and a slider's tooltip were
	// saved into the Front outline as model, and matching it then slid the
	// image 14 and 33 document pixels off the model). Without a depth buffer
	// the capture alone, as before.
	DepthRows depth;
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
			const unsigned char seen = gKeyShare[static_cast<size_t>(yp) * gKeyW + xp] < 128 ? 2 : 1;
			const int d = k > 0 ? depth.At((xp + 0.5) / k, (yp + 0.5) / k) : -1;
			out[static_cast<size_t>(j) * g.gw + i] = d < 0 || d == (seen == 2) ? seen : 0;
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

// The share of saved's model cells that cur sees (on the canvas, model or
// not) with cur shifted by (ox, oy) cells: how much of the model a match
// could be judged on.
static double MaskCoverage(const View& g, const std::vector<unsigned char>& cur, int ox, int oy)
{
	int model = 0, seen = 0;
	for (int j = 0; j < g.gh; ++j) {
		const int cj = j - oy;
		const unsigned char* a = &g.mask[static_cast<size_t>(j) * g.gw];
		for (int i = 0; i < g.gw; ++i) {
			if (a[i] != 2)
				continue;
			++model;
			const int ci = i - ox;
			seen += cj >= 0 && cj < g.gh && ci >= 0 && ci < g.gw && cur[static_cast<size_t>(cj) * g.gw + ci] != 0;
		}
	}
	return model ? static_cast<double>(seen) / model : 0;
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
	// Zoomed in on a part, the canvas sees a little of the model, and sliding
	// the saved outline until all-model cells cover that window scores near
	// 1 anywhere (build 146: zoomed in on the hand, "matched ... score 0.969
	// shift -405.13 271.44", and the PureRef panel jumped off the model). A
	// match is judged only with most of the saved model in view.
	const double coverage = best >= 0 ? MaskCoverage(v, cur, bx, by) : 0;
	EnterCriticalSection(&gOverlayLock);
	if (gMemLocked) {
		// ZBrush's camera is read from its memory, and it puts the model where
		// ZBrush draws it (the depth frames of a session fit it to under a
		// unit at zooms 0.87 to 4.07, also after turning and snapping back).
		// A saved outline only adds its own errors: build 204 matched the
		// Front view 0.6 s after the overlay started, "shift 6.45 -22.05",
		// and moved a calibrated image off the model. Logged, not applied.
		ViewLog("match not used, the camera is read from memory", slot);
	} else if (abs(bx) >= rx - 2 || abs(by) >= ry - 2) {
		// At the end of the search: no real match.
		ViewLog("match at the edge of the search, not moved", slot);
	} else if (best >= 0.6 && coverage < 0.6) {
		char what[96];
		snprintf(what, sizeof(what), "match with %.0f%% of the model in view, not moved", 100 * coverage);
		ViewLog(what, slot);
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

// Each overlay step: keeps the showing view's silhouette current. It was
// recorded once, when the view was lined up, and the model grows after
// that: the DeathClaw's Front and Left silhouettes held only the arm when
// the legs were there (build 153 looked at the hand for the crotch, the
// only part 3D Auto Track knew of). With the camera on the view and still,
// no menu open and no job queued, every cell the canvas and ZBrush's depth
// agree on is written into the saved silhouette, except round the cursor (the brush circle is
// not the model). Only when the saved model lines up with the canvas as it
// is (85% of its cells seen on the canvas are model there), so a view whose
// image has slid off the model is left alone. At most every 2 s.
static void ViewRefreshStep(const OverlayState& st)
{
	static DWORD lastAt = 0, settledAt = 0;
	static double lastCount = -1;
	const DWORD now = GetTickCount();
	if (gGesture.active || fabs(gModelPixels - lastCount) > 0.002 * (gModelPixels + 100)) {
		lastCount = gModelPixels;
		settledAt = now;
		return;
	}
	// Only snapped to the view (gActiveView stays its slot while turning away
	// freely), and only with ZBrush's depth to tell the model from what is
	// drawn over it (see SampleSilhouette).
	const int c = gActiveView;
	double empty;
	if (now - settledAt < 500 || now - lastAt < 2000 || gViewJob || c < 0 || c >= kDefaultView || gViewAway || !DepthFound(empty) ||
			gKeyShare.empty() || st.ratio <= 0)
		return;
	View& v = gViews[c];
	if (!v.used || v.gw <= 0 || v.mask.size() != static_cast<size_t>(v.gw) * v.gh || !gMenuBoxes.empty() || gFlyout.right > gFlyout.left ||
		gMenuBakedCount)
		return;
	lastAt = now;
	std::vector<unsigned char> cur;
	SampleSilhouette(st, st.offX, st.offY, v, cur);
	// The cursor's surroundings in image space, left out.
	POINT cp;
	double cu = 0, cv = 0, cr = -1;
	if (GetCursorPos(&cp) && st.pixelsPerDoc > 0) {
		const double k = st.pixelsPerDoc;
		cu = ((cp.x - st.screenX) / k - st.px) / st.ratio + st.ax + st.offX;
		cv = ((cp.y - st.screenY) / k - st.py) / st.ratio + st.ay + st.offY;
		cr = 160 / k / st.ratio;
	}
	auto nearCursor = [&](int i, int j) {
		const double du = v.u0 + (i + 0.5) * v.cell - cu, dv = v.v0 + (j + 0.5) * v.cell - cv;
		return cr > 0 && du * du + dv * dv < cr * cr;
	};
	int model = 0, kept = 0;
	for (int j = 0; j < v.gh; ++j)
		for (int i = 0; i < v.gw; ++i) {
			const size_t at = static_cast<size_t>(j) * v.gw + i;
			if (v.mask[at] == 2 && cur[at] && !nearCursor(i, j)) {
				++model;
				kept += cur[at] == 2;
			}
		}
	if (model < 200 || kept < model * 0.85)
		return;
	int changed = 0;
	for (int j = 0; j < v.gh; ++j)
		for (int i = 0; i < v.gw; ++i) {
			const size_t at = static_cast<size_t>(j) * v.gw + i;
			if (cur[at] && cur[at] != v.mask[at] && !nearCursor(i, j)) {
				v.mask[at] = cur[at];
				++changed;
			}
		}
	if (changed < 20)
		return;
	gExtents[c].data = nullptr;  // worked out again
	SaveMask(c);
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu silhouette of view %d refreshed from the canvas: %d cells changed (%.0f%% of the saved model still model)\n", now, c,
			changed, 100.0 * kept / model);
		fclose(f);
	}
}

// ---------------------------------------------------------------------------
// ZBrush's canvas depth, read from its memory. The canvas is 2.5D: every
// document pixel (pixol) keeps the depth of the surface drawn there, updated
// with every stroke, which is what 3D Auto Track needs at the middle of the
// screen, from any view and with no other view's silhouette. ZScript's
// PixolPick returns one value everywhere (0.366211 on the model and off it,
// nav_log), so the buffer is found instead: one allocation the document's
// size (width x height x bytes per pixel, the pixel maybe in a larger
// record, the image maybe at twice the size for antialiasing), where the
// empty canvas holds one value and the model, by the overlay's own mask,
// holds others that change smoothly.
// ---------------------------------------------------------------------------
struct DepthBuffer {
	bool ok = false;
	uintptr_t base = 0;  // the value of buffer pixel (0, 0)
	int scale = 1;       // buffer pixels per document pixel
	size_t pitch = 0, stride = 0;  // bytes per row and per pixel
	int type = 0;        // 0 float, 1 uint16, 2 int32, 3 double
	int w = 0, h = 0;
	double bg = 0;       // the empty canvas's value
	double score = 0;    // pixels it agreed with the mask on
	uintptr_t image = 0; // ZBrush's image object holding it, when found by it
};
static DepthBuffer gDepth;
static std::vector<uintptr_t> gDepthDropped;  // buffers that stopped fitting
// Every image object taken for depth, with its empty value. ZBrush keeps
// more than one, and the one with the model in it changes (build 164: the
// plane found with the model held only the empty value 18 s later, over the
// whole window); DepthStep takes whichever agrees with the mask.
static std::vector<std::pair<uintptr_t, double>> gDepthImages;

static const int kDepthTypeSize[4] = {4, 2, 4, 8};

static double DepthValue(const unsigned char* p, int type)
{
	switch (type) {
	case 0: { float f; memcpy(&f, p, 4); return f; }
	case 1: { uint16_t u; memcpy(&u, p, 2); return u; }
	case 2: { int32_t i; memcpy(&i, p, 4); return i; }
	default: { double d; memcpy(&d, p, 8); return d; }
	}
}

static bool DepthSame(double a, double b)
{
	return fabs(a - b) <= 1e-6 * std::max(1.0, fabs(a));
}

int DepthRows::At(double x, double y)
{
	if (!gDepth.ok)
		return -1;
	const int bx = static_cast<int>(x * gDepth.scale), by = static_cast<int>(y * gDepth.scale);
	if (bx < 0 || by < 0 || bx >= gDepth.w || by >= gDepth.h)
		return -1;
	if (by != row) {
		row = by;
		const size_t n = static_cast<size_t>(gDepth.w - 1) * gDepth.stride + kDepthTypeSize[gDepth.type];
		data.resize(n);
		SIZE_T got = 0;
		ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(gDepth.base + by * gDepth.pitch), data.data(), n, &got) && got == n;
	}
	if (!ok)
		return -1;
	const double v = DepthValue(&data[static_cast<size_t>(bx) * gDepth.stride], gDepth.type);
	if (v != v)
		return -1;
	return !DepthSame(v, gDepth.bg) && fabs(v) < 1e6 ? 1 : 0;
}

static bool DepthFound(double& empty)
{
	empty = gDepth.bg;
	return gDepth.ok;
}

// The buffer's value at a document point; false off the buffer or unreadable.
static bool DepthAtDoc(double x, double y, double& v)
{
	if (!gDepth.ok)
		return false;
	const int bx = static_cast<int>(x * gDepth.scale), by = static_cast<int>(y * gDepth.scale);
	if (bx < 0 || by < 0 || bx >= gDepth.w || by >= gDepth.h)
		return false;
	unsigned char raw[8];
	SIZE_T got = 0;
	const uintptr_t at = gDepth.base + by * gDepth.pitch + bx * gDepth.stride;
	const int n = kDepthTypeSize[gDepth.type];
	if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), raw, n, &got) || got != static_cast<SIZE_T>(n))
		return false;
	v = DepthValue(raw, gDepth.type);
	return v == v;
}

// The surface round a document point from ZBrush's canvas depth, as
// SurfaceDepth works it out from silhouettes: the model's pixels within
// reach, averaged with weights falling smoothly to nothing at the reach. In
// document pixels, like x and y: 0 at the plane through the tool's middle,
// negative towards the camera (x64dbg, a model drawn in Front: -9 at its
// outline to -167 at its middle, the empty canvas 35840). width is the
// model's width across the rows round the point (their median), standing in
// for the part's thickness, which the canvas does not hold.
//
// Only a plane that shows what the screen shows is read: ZBrush redraws the
// plane in passes, and a layout that catches a pass before it reaches the
// middle read a handful of pixels (2 to 53 of about 430, build 169) whose
// average was another depth, frame after frame. So each point in reach is
// read only where the overlay's capture (gKeyShare, the frame ZBrush shows)
// is sure it is the model, and the plane must have the model at most of
// them; the depth is the average over those it has. A plane mid-pass, empty
// (ZBrush's copy plane is cleared while the view moves) or from another pose
// fails, and the caller keeps the last reading. kp: overlay pixels per
// document pixel.
static uintptr_t ImagePixels(uintptr_t image, int dw, int dh);

// UpdateMenus: 1 where the screen shows neither the background (key) nor
// the model (ZBrush's canvas depth) inside the viewport, outside the view
// gizmo's corner: ZBrush's interface drawn over the canvas. False without a
// canvas depth (cand left empty).
static bool MenuOverEmptyCanvas(const OverlayState& st, const unsigned char* key, int w, int h, int vx0, int vy0, int vx1, int vy1,
	std::vector<unsigned char>& cand)
{
	cand.clear();
	if (!gDepth.ok || st.pixelsPerDoc <= 0)
		return false;
	cand.assign(static_cast<size_t>(w) * h, 0);
	const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3), gizmoY = vy0 + (vx1 - gizmoX);
	DepthRows depth;
	for (int y = vy0; y < vy1; ++y)
		for (int x = vx0; x < vx1; ++x) {
			const size_t i = static_cast<size_t>(y) * w + x;
			if ((x >= gizmoX && y < gizmoY) || key[i] >= 240)
				continue;
			cand[i] = depth.At(x / st.pixelsPerDoc, y / st.pixelsPerDoc) == 0;
		}
	return true;
}

static bool CanvasDepthIn(double x, double y, double reach, double kp, double& z, double& width, int& samples)
{
	samples = 0;
	width = 0;
	auto model = [&](double px, double py, double& v) {
		return DepthAtDoc(px, py, v) && !DepthSame(v, gDepth.bg) && fabs(v) < 1e6;
	};
	// The capture's verdict at a document point: 1 model, 0 canvas, -1 not sure.
	auto screen = [&](double px, double py) {
		const int sx = static_cast<int>(px * kp), sy = static_cast<int>(py * kp);
		if (sx < 0 || sy < 0 || sx >= gKeyW || sy >= gKeyH || gKeyShare.size() < static_cast<size_t>(gKeyW) * gKeyH)
			return -1;
		const int k = gKeyShare[static_cast<size_t>(sy) * gKeyW + sx];
		return k <= 15 ? 1 : k >= 240 ? 0 : -1;
	};
	const double step = std::max(1.0, reach / 12);
	double sum = 0, weights = 0;
	int shown = 0;
	std::vector<double> seen;  // every point's value, for the second read
	for (double dy = -reach; dy <= reach; dy += step)
		for (double dx = -reach; dx <= reach; dx += step) {
			const double d2 = (dx * dx + dy * dy) / (reach * reach);
			if (d2 >= 1 || screen(x + dx, y + dy) != 1)
				continue;
			++shown;
			double v;
			const bool m = model(x + dx, y + dy, v);
			seen.push_back(m ? v : 1e300);
			if (!m)
				continue;
			const double w = (1 - d2) * (1 - d2);
			sum += w * v;
			weights += w;
			++samples;
		}
	if (shown < 10 || samples < shown * 0.8 || weights <= 0) {
		++gCanvasUncovered;
		return false;
	}
	// Read only while ZBrush is not writing it. It draws a frame part by
	// part (build 170: the reading went 250, 367, 251, 369 frame after
	// frame with every point round the middle covered, the middle -1930 or
	// -2420: the body behind the head, then the head drawn over it), so a
	// plane that covers what the screen shows may still be unfinished. The
	// points read again 3 ms on must all be the same.
	LARGE_INTEGER qf, t0, t;
	QueryPerformanceFrequency(&qf);
	QueryPerformanceCounter(&t0);
	do {
		YieldProcessor();
		QueryPerformanceCounter(&t);
	} while (t.QuadPart - t0.QuadPart < qf.QuadPart * 3 / 1000);
	size_t i = 0;
	for (double dy = -reach; dy <= reach; dy += step)
		for (double dx = -reach; dx <= reach; dx += step) {
			const double d2 = (dx * dx + dy * dy) / (reach * reach);
			if (d2 >= 1 || screen(x + dx, y + dy) != 1)
				continue;
			double v;
			const bool m = model(x + dx, y + dy, v);
			if (i >= seen.size() || (m ? v : 1e300) != seen[i]) {
				++gCanvasDrawing;
				return false;
			}
			++i;
		}
	++gCanvasUsed;
	z = sum / weights;
	// The model's run across each row within half the reach, through the
	// model pixel nearest the point's column; the median of them.
	std::vector<double> runs;
	double v;
	for (double dy = -reach / 2; dy <= reach / 2; dy += std::max(1.0, reach / 6))
		for (int r = 0; r <= reach; ++r) {
			bool found = false;
			for (int side : {-1, 1}) {
				const double at = x + side * r, row = y + dy;
				if (!model(at, row, v))
					continue;
				double lo = at, hi = at;
				while (hi - lo < 8 * reach && model(lo - 1, row, v))
					lo -= 1;
				while (hi - lo < 8 * reach && model(hi + 1, row, v))
					hi += 1;
				runs.push_back(hi - lo + 1);
				found = true;
				break;
			}
			if (found)
				break;
		}
	if (!runs.empty()) {
		std::sort(runs.begin(), runs.end());
		width = runs[runs.size() / 2];
	}
	return true;
}

// ZBrush keeps two planes: one redrawn as the view moves, one a copy it
// clears when a move starts and fills when it ends (watching from outside).
// The one in use first, then the other; one that fits is kept.
static bool CanvasDepth(double x, double y, double reach, double kp, double& z, double& width, int& samples)
{
	samples = 0;
	width = 0;
	if (!gDepth.ok || reach <= 0 || kp <= 0)
		return false;
	if (CanvasDepthIn(x, y, reach, kp, z, width, samples))
		return true;
	const DepthBuffer was = gDepth;
	for (const auto& im : gDepthImages) {
		if (im.first == was.image)
			continue;
		const uintptr_t first = ImagePixels(im.first, was.w, was.h);
		if (!first)
			continue;
		gDepth.base = first;
		gDepth.image = im.first;
		gDepth.bg = im.second;
		if (CanvasDepthIn(x, y, reach, kp, z, width, samples))
			return true;
	}
	gDepth = was;
	return false;
}

// Document points the overlay is sure about: off the model (background) and
// on it, spread over the viewport.
struct DepthSample { int x, y; bool model; };

static std::vector<DepthSample> DepthSamples(const OverlayState& st, int want)
{
	std::vector<DepthSample> out;
	if (gKeyShare.empty() || st.pixelsPerDoc <= 0)
		return out;
	int vx0, vy0, vx1, vy1;
	ViewportRect(gKeyW, gKeyH, vx0, vy0, vx1, vy1);
	const int gizmoX = vx1 - static_cast<int>(std::min(vx1 - vx0, vy1 - vy0) * 0.3);
	const int step = std::max(2, static_cast<int>(sqrt(static_cast<double>(vx1 - vx0) * (vy1 - vy0) / std::max(1, want))));
	for (int y = vy0 + step / 2; y < vy1; y += step)
		for (int x = vx0 + step / 2; x < vx1; x += step) {
			if (x >= gizmoX && y < gGizmoBottom)
				continue;
			// Sure: the pixel and its neighbours two away all alike.
			bool sure = true;
			const int k0 = gKeyShare[static_cast<size_t>(y) * gKeyW + x];
			if (k0 > 15 && k0 < 240)
				continue;
			for (int d = -2; d <= 2 && sure; d += 4)
				for (int e = 0; e < 2 && sure; ++e) {
					const int xx = x + (e ? d : 0), yy = y + (e ? 0 : d);
					if (xx < 0 || yy < 0 || xx >= gKeyW || yy >= gKeyH)
						continue;
					const int k = gKeyShare[static_cast<size_t>(yy) * gKeyW + xx];
					sure = (k0 <= 15) == (k <= 15) && (k <= 15 || k >= 240);
				}
			if (sure)
				out.push_back({static_cast<int>(x / st.pixelsPerDoc), static_cast<int>(y / st.pixelsPerDoc), k0 <= 15});
		}
	return out;
}

// How well a layout fits the samples: the share whose class (the empty
// canvas's value or not) agrees with the mask, with the model's values
// changing smoothly. Negative when it plainly does not fit.
static double DepthFit(const unsigned char* mem, size_t size, size_t off, size_t pitch, size_t stride, int type, int scale,
	const std::vector<DepthSample>& s, double& bg)
{
	const int n = kDepthTypeSize[type];
	auto value = [&](int x, int y, double& v) {
		const size_t at = off + static_cast<size_t>(y) * scale * pitch + static_cast<size_t>(x) * scale * stride;
		if (at + n > size)
			return false;
		v = DepthValue(mem + at, type);
		return v == v && fabs(v) < 1e30;
	};
	// The empty canvas: one value.
	bool haveBg = false;
	int bgSeen = 0, modelSeen = 0;
	for (const DepthSample& p : s) {
		double v;
		if (p.model || !value(p.x, p.y, v))
			continue;
		if (!haveBg) {
			bg = v;
			haveBg = true;
		} else if (!DepthSame(v, bg) && ++bgSeen > 3) {
			return -1;
		}
	}
	if (!haveBg)
		return -1;
	int agree = 0, total = 0;
	double lo = 1e300, hi = -1e300;
	for (const DepthSample& p : s) {
		double v;
		if (!value(p.x, p.y, v))
			continue;
		++total;
		const bool model = !DepthSame(v, bg);
		agree += model == p.model;
		if (p.model && model) {
			++modelSeen;
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
	}
	if (total < 50 || modelSeen < 20 || hi <= lo)
		return -1;
	// Depth's empty value is far beyond the model's (35840 for the empty
	// canvas, the model -9 to -167, x64dbg). Build 161 took a mask plane,
	// empty 1 and the model 0 to 0.998, and read depth 0 everywhere.
	if (fabs(bg) < 4 * std::max(fabs(lo), fabs(hi)))
		return -1;
	// Neighbouring model pixels within a quarter of the model's range. (A
	// twentieth, build 158, turned the real buffer down: the side of a model
	// falls 10 document pixels a pixel in a range of 170, x64dbg.)
	int smooth = 0, pairs = 0;
	for (const DepthSample& p : s) {
		double v, w;
		if (!p.model || !value(p.x, p.y, v) || DepthSame(v, bg) || !value(p.x + 1, p.y, w) || DepthSame(w, bg))
			continue;
		++pairs;
		smooth += fabs(w - v) <= (hi - lo) * 0.25;
	}
	if (pairs < 10 || smooth < pairs * 0.7)
		return -1;
	return static_cast<double>(agree) / total;
}

static void DepthLog(const char* what)
{
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu canvas depth %s\n", GetTickCount(), what);
		fclose(f);
	}
}

// ZBrush's own objects for the canvas: an image object with the document's
// width and height as floats and a pointer to a pixels object, which holds the
// plane's size in bytes and the address of its first pixel. The document's own
// images are the ones its size, 32 bits a pixel; the depth among them is the
// float plane whose empty value is far beyond the model's. Found by these
// objects, not by guessing at memory's contents (builds 158-162: the depth
// missed in some sessions, a mask taken for it in another).
//
// Their layout is learned from memory each time (ImageLayoutLearn), not taken
// from one ZBrush build: builds 163-188 had ZBrush 2023's vtables
// (ZBrush.exe + 0xCFD2DE8 and + 0xCFFEF90) and field offsets written in, and
// ZBrush.exe is packed and has no type names, so another build would have
// moved them with nothing to find them by. Tried on the running ZBrush 2023
// before build 189, learning gave those values back, and a second image class
// (+ 0xCFD5568) the fixed vtable had left out.
struct ImageKind {
	uintptr_t vt;  // the image class's vtable
	int pixels;    // offset of the pointer to the pixels object
	int wh;        // offset of the width and height (floats)
};
struct ImageLayout {
	uintptr_t pixVt = 0;            // the pixels class's vtable
	int sizeOff = 0, firstOff = 0;  // offsets of the size and the first pixel's address
	std::vector<ImageKind> kinds;
};
static ImageLayout gImgLayout;

static bool ReadOwn(uintptr_t at, void* out, size_t n)
{
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), out, n, &got) && got == n;
}

// The first pixel of an image object's plane, checked against the learned
// layout and the document's size; 0 if it is not one.
static uintptr_t ImagePixels(uintptr_t image, int dw, int dh)
{
	uintptr_t vt;
	if (!gImgLayout.pixVt || !ReadOwn(image, &vt, 8))
		return 0;
	for (const ImageKind& k : gImgLayout.kinds) {
		if (k.vt != vt)
			continue;
		float wh[2];
		uintptr_t pixels, pvt, first;
		int64_t size;
		if (!ReadOwn(image + k.wh, wh, sizeof wh) || wh[0] != dw || wh[1] != dh || !ReadOwn(image + k.pixels, &pixels, 8) ||
			!ReadOwn(pixels, &pvt, 8) || pvt != gImgLayout.pixVt || !ReadOwn(pixels + gImgLayout.sizeOff, &size, 8) ||
			size != static_cast<int64_t>(dw) * dh * 4 || !ReadOwn(pixels + gImgLayout.firstOff, &first, 8))
			return 0;
		return first;
	}
	return 0;
}

// Learns the layout and finds the document's images, in two passes over
// ZBrush's private memory. First the pixels objects: the plane's size in
// bytes (w * h * 4), a pointer within 32 bytes after it to memory that holds
// a plane that size, and the nearest pointer into ZBrush.exe within 256 bytes
// before it, the object's vtable; the most common class so placed is the
// pixels class. Then the image objects: a pointer to one of those, the
// nearest pointer into ZBrush.exe within 512 bytes before it (the vtable),
// and the width and height as floats between them (or just after). Returns
// the image objects.
static std::vector<uintptr_t> ImageLayoutLearn(int dw, int dh, size_t& scanned)
{
	const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	MODULEINFO mi = {};
	GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof mi);
	const uintptr_t exeEnd = exe + mi.SizeOfImage;
	const uint64_t size = static_cast<uint64_t>(dw) * dh * 4;
	auto privateRw = [](const MEMORY_BASIC_INFORMATION& m) { return m.Type == MEM_PRIVATE && (m.Protect & 0xff) == PAGE_READWRITE; };
	auto planeAt = [&](uintptr_t q) {
		MEMORY_BASIC_INFORMATION r;
		return q && VirtualQuery(reinterpret_cast<LPCVOID>(q), &r, sizeof r) == sizeof r && r.State == MEM_COMMIT &&
			reinterpret_cast<uintptr_t>(r.BaseAddress) + r.RegionSize >= q + size;
	};
	struct PixHit { uintptr_t obj, vt; int sizeOff, firstOff; };
	const int workers = ScanWorkers();
	std::vector<std::vector<PixHit>> pixHits(workers);
	scanned = ScanMemory(0, privateRw, [&](int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t) {
		for (size_t i = 0; i + 8 <= starts; i += 8) {
			uint64_t v;
			memcpy(&v, &buf[i], 8);
			if (v != size)
				continue;
			unsigned char near[0x100 + 0x28];
			const uintptr_t field = at + i, from = field - 0x100;
			if (!ReadOwn(from, near, sizeof near))
				continue;
			int firstOff = -1;
			for (int d = 8; d <= 0x20 && firstOff < 0; d += 8) {
				uintptr_t q;
				memcpy(&q, near + 0x100 + d, 8);
				if (planeAt(q))
					firstOff = d;
			}
			if (firstOff < 0)
				continue;
			for (int d = 8; d <= 0x100; d += 8) {
				uintptr_t q;
				memcpy(&q, near + 0x100 - d, 8);
				if (q >= exe && q < exeEnd) {
					pixHits[w].push_back({field - d, q, d, d + firstOff});
					break;
				}
			}
		}
	});
	// The pixels class: the most common (vtable, offsets).
	std::map<std::tuple<uintptr_t, int, int>, int> counts;
	for (const auto& hs : pixHits)
		for (const PixHit& h : hs)
			++counts[{h.vt, h.sizeOff, h.firstOff}];
	gImgLayout = ImageLayout();
	int most = 0;
	for (const auto& c : counts)
		if (c.second > most) {
			most = c.second;
			gImgLayout.pixVt = std::get<0>(c.first);
			gImgLayout.sizeOff = std::get<1>(c.first);
			gImgLayout.firstOff = std::get<2>(c.first);
		}
	std::vector<uintptr_t> pixObjs;
	for (const auto& hs : pixHits)
		for (const PixHit& h : hs)
			if (h.vt == gImgLayout.pixVt && h.sizeOff == gImgLayout.sizeOff && h.firstOff == gImgLayout.firstOff)
				pixObjs.push_back(h.obj);
	std::sort(pixObjs.begin(), pixObjs.end());
	if (pixObjs.empty())
		return {};
	const uintptr_t lo = pixObjs.front(), hi = pixObjs.back();
	struct ImgHit { uintptr_t obj; ImageKind kind; };
	std::vector<std::vector<ImgHit>> imgHits(workers);
	scanned += ScanMemory(0, privateRw, [&](int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t) {
		for (size_t i = 0; i + 8 <= starts; i += 8) {
			uintptr_t v;
			memcpy(&v, &buf[i], 8);
			if (v < lo || v > hi || !std::binary_search(pixObjs.begin(), pixObjs.end(), v))
				continue;
			unsigned char near[0x200 + 0x40];
			const uintptr_t field = at + i, from = field - 0x200;
			if (!ReadOwn(from, near, sizeof near))
				continue;
			for (int d = 8; d <= 0x200; d += 8) {
				uintptr_t q;
				memcpy(&q, near + 0x200 - d, 8);
				if (q < exe || q >= exeEnd)
					continue;
				// The width and height from the vtable to 64 bytes past the pointer.
				const int o = 0x200 - d;
				for (int k = o + 8; k + 8 <= static_cast<int>(sizeof near); k += 4) {
					float a, b;
					memcpy(&a, near + k, 4);
					memcpy(&b, near + k + 4, 4);
					if (a == dw && b == dh) {
						imgHits[w].push_back({field - d, {q, d, k - o}});
						break;
					}
				}
				break;
			}
		}
	});
	std::vector<uintptr_t> images;
	for (const auto& hs : imgHits)
		for (const ImgHit& h : hs) {
			bool known = false;
			for (const ImageKind& k : gImgLayout.kinds)
				known = known || (k.vt == h.kind.vt && k.pixels == h.kind.pixels && k.wh == h.kind.wh);
			if (!known)
				gImgLayout.kinds.push_back(h.kind);
			images.push_back(h.obj);
		}
	std::sort(images.begin(), images.end());
	images.erase(std::unique(images.begin(), images.end()), images.end());
	return images;
}

// The document's images, found by their layout learned from memory, each
// tried as depth (DepthFit, and its empty value far beyond the model's).
static bool DepthFromObjects(const OverlayState& st, const std::vector<DepthSample>& samples, int dw, int dh)
{
	const DWORD t0 = GetTickCount();
	size_t scanned = 0;
	const std::vector<uintptr_t> images = ImageLayoutLearn(dw, dh, scanned);
	gDepthImages.clear();
	char line[600];
	{
		const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		std::string kinds;
		for (const ImageKind& k : gImgLayout.kinds) {
			char one[96];
			snprintf(one, sizeof one, " [vtable +%llx, pixels at +%x, width and height at +%x]", static_cast<unsigned long long>(k.vt - exe), k.pixels, k.wh);
			kinds += one;
		}
		snprintf(line, sizeof line, "by ZBrush's image objects: %zu of the document's size in %zu MB, %lu ms; learned: pixels class +%llx (size at +%x, first pixel at +%x), image classes%s",
			images.size(), scanned >> 20, GetTickCount() - t0, static_cast<unsigned long long>(gImgLayout.pixVt ? gImgLayout.pixVt - exe : 0),
			gImgLayout.sizeOff, gImgLayout.firstOff, kinds.empty() ? " none" : kinds.c_str());
	}
	DepthLog(line);
	DepthBuffer best;
	for (uintptr_t image : images) {
		const uintptr_t first = ImagePixels(image, dw, dh);
		const size_t size = static_cast<size_t>(dw) * dh * 4;
		std::vector<unsigned char> mem(size);
		if (!first || !ReadOwn(first, mem.data(), size))
			continue;
		double bg = 0;
		double fit = DepthFit(mem.data(), size, 0, static_cast<size_t>(dw) * 4, 4, 0, 1, samples, bg);
		// By content alone, when the mask does not settle it: as floats, one
		// value over a fifth of the plane, and every other value a quarter of
		// it or less (depth: 35840 empty, the model in the hundreds).
		const float* f = reinterpret_cast<const float*>(mem.data());
		const size_t n = static_cast<size_t>(dw) * dh;
		std::vector<float> s;
		for (size_t i = 0; i < n; i += n / 4096 + 1)
			s.push_back(f[i]);
		std::vector<float> sorted = s;
		std::sort(sorted.begin(), sorted.end());
		float mode = sorted.empty() ? 0 : sorted[0];
		size_t modeCount = 0;
		for (size_t i = 0, run = 0; i < sorted.size(); ++i) {
			run = i && sorted[i] == sorted[i - 1] ? run + 1 : 1;
			if (run > modeCount) {
				modeCount = run;
				mode = sorted[i];
			}
		}
		bool byContent = modeCount * 5 >= s.size() && fabs(mode) >= 100;
		for (float v : s)
			if (v != mode && !(fabs(v) * 4 <= fabs(mode)))
				byContent = false;
		if (fit < 0 && byContent) {
			fit = 0.5;
			bg = mode;
		}
		snprintf(line, sizeof line, "  image %p, pixels %p: %s, empty %.6g", reinterpret_cast<void*>(image), reinterpret_cast<void*>(first),
			fit < 0 ? "not depth" : fit == 0.5 ? "depth by its values" : "depth", bg);
		DepthLog(line);
		if (fit >= 0)
			gDepthImages.push_back({image, bg});
		if (fit > best.score) {
			best.ok = true;
			best.base = first;
			best.scale = 1;
			best.pitch = static_cast<size_t>(dw) * 4;
			best.stride = 4;
			best.type = 0;
			best.w = dw;
			best.h = dh;
			best.bg = bg;
			best.score = fit;
			best.image = image;
		}
	}
	if (!best.ok)
		return false;
	gDepth = best;
	return true;
}

// Looks for the buffer: by ZBrush's image objects, else every private
// allocation the document's size, every layout. Logs the best few.
static void DepthScan(const OverlayState& st)
{
	const DWORD t0 = GetTickCount();
	const std::vector<DepthSample> samples = DepthSamples(st, 3000);
	const int dw = static_cast<int>(st.docW + 0.5), dh = static_cast<int>(st.docH + 0.5);
	if (samples.size() < 200 || dw < 16 || dh < 16)
		return;
	if (DepthFromObjects(st, samples, dw, dh))
		return;
	// Allocations: base and committed size (regions of one allocation summed);
	// and every committed region on its own, for the wider search.
	std::vector<std::pair<uintptr_t, size_t>> allocs, regions;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
		const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), next = base + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xff;
		if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (prot == PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
			const uintptr_t ab = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
			regions.push_back({base, mbi.RegionSize});
			if (!allocs.empty() && allocs.back().first == ab && allocs.back().first + allocs.back().second == base)
				allocs.back().second += mbi.RegionSize;
			else if (ab == base)
				allocs.push_back({base, mbi.RegionSize});
		}
		if (next <= addr)
			break;
		addr = next;
	}
	std::vector<DepthBuffer> found;
	std::vector<unsigned char> mem;
	size_t tried = 0, copied = 0;
	static const int kStrides[] = {2, 4, 8, 12, 16, 20, 24, 32};
	for (const auto& al : allocs) {
		bool loaded = false;
		for (int scale = 1; scale <= 2; ++scale) {
			const size_t w = static_cast<size_t>(dw) * scale, h = static_cast<size_t>(dh) * scale;
			for (int stride : kStrides) {
				size_t lastPitch = 0;
				for (size_t align : {1, 16, 32, 64}) {
					const size_t pw = (w + align - 1) / align * align, pitch = pw * stride;
					if (pitch == lastPitch)
						continue;
					lastPitch = pitch;
					const size_t need = pitch * h;
					if (al.second < need || al.second > need + need / 50 + (1 << 16))
						continue;
					if (!loaded) {
						if (al.second > (size_t(512) << 20))
							break;
						mem.resize(al.second);
						SIZE_T got = 0;
						if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(al.first), mem.data(), mem.size(), &got) || got != mem.size())
							break;
						loaded = true;
						copied += got;
					}
					for (size_t head = 0; head + need <= al.second && head <= 4096; head += 16)
						for (int type = 0; type < 4; ++type)
							for (int field = 0; field + kDepthTypeSize[type] <= stride; field += kDepthTypeSize[type]) {
								++tried;
								double bg = 0;
								const double fit = DepthFit(mem.data(), mem.size(), head + field, pitch, stride, type, scale, samples, bg);
								if (fit < 0.9)
									continue;
								DepthBuffer b;
								b.ok = true;
								b.base = al.first + head + field;
								b.scale = scale;
								b.pitch = pitch;
								b.stride = stride;
								b.type = type;
								b.w = static_cast<int>(w);
								b.h = static_cast<int>(h);
								b.bg = bg;
								b.score = fit;
								found.push_back(b);
							}
				}
			}
		}
	}
	// Not a block of its own (build 160 found no allocation the right size,
	// where 159 found the buffer 0x70 into one): the start of a long run of
	// one value (the empty canvas along the top rows) in any committed region
	// big enough, taken as the image's first pixel, a float plane of the
	// document's width.
	if (found.empty()) {
		const size_t need = static_cast<size_t>(dw) * dh * 4;
		for (const auto& al : regions) {
			if (al.second < need || al.second > (size_t(512) << 20))
				continue;
			mem.resize(al.second);
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(al.first), mem.data(), mem.size(), &got) || got < need)
				continue;
			copied += got;
			const uint32_t* d = reinterpret_cast<const uint32_t*>(mem.data());
			const size_t n = got / 4;
			for (size_t i = 0; i + 512 < n; i += 256) {
				const uint32_t v = d[i];
				if (!v || d[i + 64] != v || d[i + 255] != v || d[i + 511] != v)
					continue;
				float fv;
				memcpy(&fv, &v, 4);
				size_t s = i, e = i;
				while (s > 0 && d[s - 1] == v)
					--s;
				while (e < n && d[e] == v)
					++e;
				if (fabs(fv) > 1e-3 && fabs(fv) < 1e7)
					for (int scale = 1; scale <= 2; ++scale) {
						const size_t pitch = static_cast<size_t>(dw) * scale * 4;
						if (s * 4 + pitch * dh * scale > got)
							continue;
						++tried;
						double bg = 0;
						const double fit = DepthFit(mem.data(), got, s * 4, pitch, 4, 0, scale, samples, bg);
						if (fit < 0.9)
							continue;
						DepthBuffer b;
						b.ok = true;
						b.base = al.first + s * 4;
						b.scale = scale;
						b.pitch = pitch;
						b.stride = 4;
						b.type = 0;
						b.w = dw * scale;
						b.h = dh * scale;
						b.bg = bg;
						b.score = fit;
						found.push_back(b);
					}
				i = e / 256 * 256;  // past the run
			}
		}
	}
	// Best first; a float before the other readings of the same bytes; a
	// buffer dropped before (ZBrush keeps two planes alike, and one may be a
	// copy left behind) after the rest.
	auto dropped = [](const DepthBuffer& b) {
		return std::find(gDepthDropped.begin(), gDepthDropped.end(), b.base) != gDepthDropped.end();
	};
	std::stable_sort(found.begin(), found.end(), [&](const DepthBuffer& a, const DepthBuffer& b) {
		if (dropped(a) != dropped(b))
			return !dropped(a);
		if (fabs(a.score - b.score) > 0.002)
			return a.score > b.score;
		return (a.type == 0) > (b.type == 0);
	});
	char line[400];
	snprintf(line, sizeof line, "scan: %zu allocations, %zu MB copied, %zu layouts tried in %lu ms, %zu fit (document %d x %d, %zu samples)",
		allocs.size(), copied >> 20, tried, GetTickCount() - t0, found.size(), dw, dh, samples.size());
	DepthLog(line);
	static const char* kTypes[4] = {"float", "uint16", "int32", "double"};
	for (size_t i = 0; i < found.size() && i < 8; ++i) {
		const DepthBuffer& b = found[i];
		snprintf(line, sizeof line, "  candidate %zu: at %p, %s every %zu bytes, rows %zu bytes, %d x %d (x%d), empty canvas %.6g, agrees %.1f%%", i,
			reinterpret_cast<void*>(b.base), kTypes[b.type], b.stride, b.pitch, b.w, b.h, b.scale, b.bg, 100 * b.score);
		DepthLog(line);
	}
	if (!found.empty())
		gDepth = found[0];
}

// ---------------------------------------------------------------------------
// The model's surface, from ZBrush's finished frames only, kept as points in
// model units (G: from the model's pivot along the model's axes, as the
// canvas shows them at zoom 1), so the camera's pan, zoom and rotation only
// change how it is looked up: from any angle, with or without an image for
// the view the camera is in (builds 172-174 kept one view's image space, and
// tracked only in a snapped view with an image).
// ZBrush keeps two depth planes: it draws into one, part by part, almost all
// the time the view moves, and copies a finished frame into the other, which
// it clears as soon as it draws again (watching both from outside, 100 ms
// apart). The two alike, with the model in them, is a finished frame. Each
// pixel of it is a point of the surface: (x - pivot x) / zoom along the
// camera's right, (y - pivot y) / zoom along its down, DepthZ towards it.
// One point is kept a cell of kCloudCell model units,
// the latest frame's; a point a finished frame shows in front of what it
// sees, or where it sees no model, is gone (an edit, a part moved).
// ---------------------------------------------------------------------------
struct CloudPoint {
	uint64_t key;
	float p[3];
};
static std::vector<CloudPoint> gCloud;
static std::unordered_map<uint64_t, uint32_t> gCloudAt;
static const double kCloudCell = 2;
static int gCloudVersion = 0;  // changes with every frame taken in

static bool CloudHas()
{
	return !gCloud.empty();
}

static uint64_t CloudKey(const double p[3])
{
	uint64_t k = 0;
	for (int t = 0; t < 3; ++t)
		k = k << 21 | (static_cast<uint64_t>(static_cast<int64_t>(floor(p[t] / kCloudCell)) + (1 << 20)) & 0x1FFFFF);
	return k;
}

static void CloudErase(uint32_t i)
{
	gCloudAt.erase(gCloud[i].key);
	if (i + 1 != gCloud.size()) {
		gCloud[i] = gCloud.back();
		gCloudAt[gCloud[i].key] = i;
	}
	gCloud.pop_back();
}

// The model's pivot depth as ZBrush holds it now: the float after x and y in
// the triplet MemFollow follows. It moves as the model turns (373 to 725
// over one turn, build 195) and goes to 0 when a view is snapped to. False,
// with why in the log, when the triplet's x and y are not the overlay's (it
// runs a frame ahead during moves) or it has not followed a drag yet (copies
// of the start's position have 0 after them and never move).
static bool PivotDepth(const OverlayState& st, double& z)
{
	if (gTesting) {
		z = gTestCam.pivot;
		return true;
	}
	const char* why = nullptr;
	double xyz[3] = {0, 0, 0};
	if (gTripletInUse < 0 || gTripletInUse >= static_cast<int>(gTriplets.size())) {
		why = "no camera triplet";
	} else {
		const MemTriplet& t = gTriplets[gTripletInUse];
		bool read;
		if (t.x.isDouble) {
			read = ReadOwn(t.x.addr, xyz, sizeof xyz);
		} else {
			float f[3];
			read = ReadOwn(t.x.addr, f, sizeof f);
			for (int i = 0; i < 3; ++i)
				xyz[i] = f[i];
		}
		if (!read)
			why = "unreadable";
		else if (t.posFrames <= 0)
			why = "triplet has not followed a drag";
		else if (fabs(xyz[0] - st.px) >= 0.5 || fabs(xyz[1] - st.py) >= 0.5)
			why = "triplet x and y not the overlay's";
		else if (!(fabs(xyz[2]) < 1e6))
			why = "not a depth";
	}
	static const char* lastWhy = "";
	static double lastZ = NAN;
	const char* now = why ? why : "read";
	if (strcmp(now, lastWhy) || (!why && fabs(xyz[2] - lastZ) > 50)) {
		char line[240];
		snprintf(line, sizeof line, "model pivot depth %s: x %.3f y %.3f depth %.3f (overlay x %.3f y %.3f)", now, xyz[0], xyz[1], xyz[2], st.px, st.py);
		DepthLog(line);
		lastWhy = now;
		if (!why)
			lastZ = xyz[2];
	}
	if (why)
		return false;
	z = xyz[2];
	return true;
}

// For the layout: the pivot read now, else the last read (during a move, a
// frame behind at most). Frames take only a fresh read (DepthMapStep): build
// 195 kept 729 through a snap to Front that put it to 0, and the points from
// those frames sat 275 in front of the face.
static bool LiveCam(const OverlayState& st, const double d[3][3], Cam& c)
{
	memcpy(c.d, d, sizeof c.d);
	c.px = st.px;
	c.py = st.py;
	c.r = st.ratio;
	c.cx = st.docW * 0.5;
	c.cy = st.docH * 0.5;
	c.invD = ProjInvD(st.docW, st.docH, st.ratio);
	static double kept = 0;
	static bool have = false;
	double p;
	if (PivotDepth(st, p)) {
		kept = p;
		have = true;
	}
	c.pivot = kept;
	return have && c.r > 0;
}

static void CamPoint(const Cam& c, double x, double y, double z, double g[3])
{
	CamUnproject(c, x, y, z, c.invD, g);
}

// A finished frame kept for measuring and rebuilding: its camera and its
// depth plane at half size.
struct FrameRec {
	Cam cam;
	float bg;
	int w, h;  // the half-size plane's
	std::vector<float> plane;
	bool Model(int x, int y, float& v) const
	{
		if (x < 0 || y < 0 || x >= w || y >= h)
			return false;
		v = plane[static_cast<size_t>(y) * w + x];
		return v != bg && fabs(v) < 1e6;
	}
	bool Inside(double x, double y) const
	{
		return x >= 0 && y >= 0 && x < 2 * w && y < 2 * h;
	}
};
static std::vector<FrameRec> gFrames;  // at most kFrames, from different angles
static const size_t kFrames = 6;

// A frame into the points: those it shows to be gone (where it sees no
// model, or in front of the surface it sees, by more than a cell) out, then
// its own in. Counts for the log. A frame that finds over a quarter of the
// points it sees gone (at least 2000 seen) is out of step with the camera
// read with it and is not taken: false. (Build 179 took a frame of two
// cleared planes, alike and empty, during a turn, and lost 27025 points.)
static bool CloudIngest(const FrameRec& f, size_t& added, size_t& gone, size_t& modelPx)
{
	added = gone = modelPx = 0;
	const Cam& c = f.cam;
	auto goneAt = [&](uint32_t i, bool& seen) {
		double x, y, z;
		seen = CamProject(c, gCloud[i].p, c.invD, x, y, z) && f.Inside(x, y);
		if (!seen)
			return false;
		float v;
		const bool model = f.Model(static_cast<int>(x / 2), static_cast<int>(y / 2), v);
		return !model || z > DepthZ(c, v) + 2 * kCloudCell;
	};
	size_t seenCount = 0, goneCount = 0;
	for (uint32_t i = 0; i < gCloud.size(); ++i) {
		bool seen;
		goneCount += goneAt(i, seen);
		seenCount += seen;
	}
	if (seenCount >= 2000 && goneCount * 4 > seenCount) {
		gone = goneCount;
		return false;
	}
	for (uint32_t i = 0; i < gCloud.size();) {
		bool seen;
		if (goneAt(i, seen)) {
			CloudErase(i);
			++gone;
		} else {
			++i;
		}
	}
	// Every half-size pixel, or every few when a cell spans several.
	const int step = std::max(1, static_cast<int>(kCloudCell * c.r / 4));
	for (int y = 0; y < f.h; y += step)
		for (int x = 0; x < f.w; x += step) {
			float v;
			if (!f.Model(x, y, v))
				continue;
			++modelPx;
			double p[3];
			CamUnproject(c, 2 * x + 1, 2 * y + 1, DepthZ(c, v), c.invD, p);
			const uint64_t k = CloudKey(p);
			const auto it = gCloudAt.find(k);
			CloudPoint* cp;
			if (it != gCloudAt.end()) {
				cp = &gCloud[it->second];
			} else {
				gCloudAt[k] = static_cast<uint32_t>(gCloud.size());
				gCloud.push_back({k, {0, 0, 0}});
				cp = &gCloud.back();
				++added;
			}
			for (int t = 0; t < 3; ++t)
				cp->p[t] = static_cast<float>(p[t]);
		}
	modelPx *= static_cast<size_t>(step) * step * 4;
	return true;
}

// A frame with Persp on, its depth from ZBrush's eye (TakeFrame), checked
// against the points it shows: each half-size pixel's nearest point, value +
// zoom * z / (1 - z * invD) there, and the band two points' cells wide that
// holds most of them (where the points have a gap or spill past an outline a
// pixel pairs one surface's value with another's point). False when over 30%
// agree on a value over two cells from the eye's: ZBrush's depth then follows
// something else (a pivot not 0 is not yet seen in Persp), and the frame is
// not taken. Logged at most every 2 s, and whenever it is not taken.
static bool PerspCheck(const FrameRec& f, double pivotRead)
{
	const Cam& c = f.cam;
	const double invD = c.invD;
	// Each point over the half-size pixels its cell covers drawn at its depth:
	// zoomed in, the points sit pixels apart, and a pixel between them would
	// get a point from the far side.
	std::vector<float> nearest(f.plane.size(), -1e30f);
	for (const CloudPoint& p : gCloud) {
		double x, y, z;
		if (!CamProject(c, p.p, invD, x, y, z) || !f.Inside(x, y))
			continue;
		const int r = static_cast<int>(kCloudCell * c.r / (1 - z * invD) / 4);
		const int i = static_cast<int>(x / 2), j = static_cast<int>(y / 2);
		for (int jj = std::max(0, j - r); jj <= std::min(f.h - 1, j + r); ++jj)
			for (int ii = std::max(0, i - r); ii <= std::min(f.w - 1, i + r); ++ii) {
				float& n = nearest[static_cast<size_t>(jj) * f.w + ii];
				n = std::max(n, static_cast<float>(z));
			}
	}
	std::vector<double> c0;
	for (int y = 0; y < f.h; ++y)
		for (int x = 0; x < f.w; ++x) {
			const double z = nearest[static_cast<size_t>(y) * f.w + x];
			float v;
			if (z > -1e29 && f.Model(x, y, v))
				c0.push_back(v + c.r * z / (1 - z * invD));
		}
	double C = 0, share = 0;
	size_t in = 0;
	if (c0.size() >= 500) {
		std::sort(c0.begin(), c0.end());
		const double band = 2 * kCloudCell * c.r;
		size_t from = 0;
		for (size_t i = 0, j = 0; j < c0.size(); ++j) {
			while (c0[j] - c0[i] > band)
				++i;
			if (j - i + 1 > in) {
				in = j - i + 1;
				from = i;
			}
		}
		C = c0[from + in / 2];
		share = static_cast<double>(in) / c0.size();
	}
	const bool against = in >= 500 && share >= 0.3 && fabs(C - c.pivot) > 2 * kCloudCell * c.r;
	static DWORD loggedAt = 0;
	if (against || GetTickCount() - loggedAt > 2000) {
		loggedAt = GetTickCount();
		char line[300];
		if (in >= 500)
			snprintf(line, sizeof line, "Persp frame %s: value at depth 0 %.2f from the eye (%.2f, pivot %.3f), the points' %.2f (%.0f%% of %zu pixels within 2 cells), zoom %.5f",
				against ? "not taken, the points put its depth elsewhere" : "taken", c.pivot, c.r / invD, pivotRead, C, 100 * share, c0.size(), c.r);
		else
			snprintf(line, sizeof line, "Persp frame taken: value at depth 0 %.2f from the eye (%.2f, pivot %.3f), too few points to check it (%zu pixels), zoom %.5f",
				c.pivot, c.r / invD, pivotRead, c0.size(), c.r);
		DepthLog(line);
	}
	return !against;
}

// Each overlay step: a finished frame, when there is a new one, into the
// points; and the cells laid out again when the camera turns (the layout is
// otherwise asked for only when the pan or zoom changes).
static void TakeFrame(const OverlayState& st, const double d[3][3], const std::vector<float>& planeA, int w, int h, DWORD now);

static void DepthMapStep(const OverlayState& st)
{
	// ZBrush's projection each step, whatever else happens: its records found
	// and each Persp switch logged as it is made (build 209 read it only for a
	// frame or a PureRef layout, and a session in Classic with no pan, so no
	// pivot and no frame, never looked).
	ProjInvD(st.docW, st.docH, st.ratio);
	double d[3][3];
	if (!CameraAxes(d)) {
		const int c = gActiveView;
		if (c < 0 || c >= kDefaultView || gViewAway)
			return;
		ViewAxes(c, d);
	}
	// The camera still for a quarter second before a frame is taken: ZBrush
	// moves its camera before it starts drawing, and a frame read in that gap
	// is the old view under the new camera (build 181: the first frame, read
	// 360 ms into a pan, went in where nothing could check it, and every
	// right frame after it was turned down as out of step).
	static DWORD cameraMovedAt = 0;
	{
		static double lastD[3][3] = {}, lastCam[3] = {};
		bool turned = false;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				turned = turned || fabs(d[i][j] - lastD[i][j]) > 1e-4;
		const bool moved = st.px != lastCam[0] || st.py != lastCam[1] || st.ratio != lastCam[2];
		if (turned || moved) {
			memcpy(lastD, d, sizeof lastD);
			lastCam[0] = st.px;
			lastCam[1] = st.py;
			lastCam[2] = st.ratio;
			cameraMovedAt = GetTickCount();
		}
		if (turned) {
			EnterCriticalSection(&gOverlayLock);
			gRedrawPending = true;
			LeaveCriticalSection(&gOverlayLock);
		}
	}
	if (!gDepth.image || gDepthImages.size() < 2 || st.ratio <= 0 || GetTickCount() - cameraMovedAt < 250)
		return;
	const int w = gDepth.w, h = gDepth.h;
	const uintptr_t a = ImagePixels(gDepthImages[0].first, w, h), b = ImagePixels(gDepthImages[1].first, w, h);
	if (!a || !b)
		return;
	// First 16 rows of each: alike, and the model in them (or a full look
	// at most every quarter second, for a model the rows miss).
	static std::vector<float> rowA, rowB, planeA, planeB;
	static DWORD lastFull = 0;
	rowA.resize(w);
	rowB.resize(w);
	bool model = false;
	for (int i = 0; i < 16; ++i) {
		const size_t at = static_cast<size_t>((2 * i + 1) * h / 32) * w * 4;
		if (!ReadOwn(a + at, rowA.data(), w * 4) || !ReadOwn(b + at, rowB.data(), w * 4) || memcmp(rowA.data(), rowB.data(), w * 4))
			return;
		for (int x = 0; x < w && !model; x += 4)
			model = rowA[x] != rowA[0] && fabs(rowA[x]) < 1e6;
	}
	const DWORD now = GetTickCount();
	if (!model && now - lastFull < 250)
		return;
	lastFull = now;
	const size_t n = static_cast<size_t>(w) * h;
	planeA.resize(n);
	planeB.resize(n);
	if (!ReadOwn(a, planeA.data(), n * 4) || !ReadOwn(b, planeB.data(), n * 4) || memcmp(planeA.data(), planeB.data(), n * 4))
		return;
	TakeFrame(st, d, planeA, w, h, now);
}

// A finished frame (ZBrush's depth plane, w x h, read with the camera st and
// axes d) into the points. The test suite calls it with planes rendered from
// a model.
static void TakeFrame(const OverlayState& st, const double d[3][3], const std::vector<float>& planeA, int w, int h, DWORD now)
{
	const size_t n = static_cast<size_t>(w) * h;
	// The empty value: the most common of a sparse sample (ZBrush changes it,
	// 24000 then 35840).
	std::vector<float> s;
	for (size_t i = 0; i < n; i += n / 2048 + 1)
		s.push_back(planeA[i]);
	std::sort(s.begin(), s.end());
	float bg = s[0];
	size_t most = 0;
	for (size_t i = 0, run = 0; i < s.size(); ++i) {
		run = i && s[i] == s[i - 1] ? run + 1 : 1;
		if (run > most) {
			most = run;
			bg = s[i];
		}
	}
	// The pivot read with the frame, never one held from before (PivotDepth).
	double pivot;
	if (!PivotDepth(st, pivot))
		return;
	const double invD = ProjInvD(st.docW, st.docH, st.ratio);
	// Not without ZBrush's projection record: whether Persp is on is then
	// unknown, and a Persp frame read as flat puts its points wrong (build
	// 214, its record turned down at zoom 2.35: frame after frame refused as
	// out of step, the points started again twice).
	if (!gTesting && gProj.empty()) {
		static DWORD loggedAt = 0;
		if (GetTickCount() - loggedAt > 10000) {
			loggedAt = GetTickCount();
			DepthLog("frame not taken: ZBrush's projection record is not found, so whether Persp is on is not known");
		}
		return;
	}
	// The same frame as last time, seen again: nothing new.
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](const void* p, size_t len) {
		for (size_t i = 0; i < len; ++i)
			key = (key ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
	};
	for (size_t i = 0; i < n; i += 61)
		mix(&planeA[i], 4);
	const double cam[4] = {st.px, st.py, st.ratio, pivot};
	mix(cam, sizeof cam);
	mix(d, sizeof(double) * 9);
	static uint64_t lastKey = 0;
	if (key == lastKey)
		return;
	lastKey = key;
	// Kept at half size: the frame from this angle (within 20 degrees) it
	// replaces, else a new angle, the oldest dropped past kFrames.
	FrameRec f;
	memcpy(f.cam.d, d, sizeof f.cam.d);
	f.cam.px = st.px;
	f.cam.py = st.py;
	f.cam.r = st.ratio;
	f.cam.pivot = pivot;
	f.cam.cx = w * 0.5;
	f.cam.cy = h * 0.5;
	f.bg = bg;
	f.w = w / 2;
	f.h = h / 2;
	f.plane.resize(static_cast<size_t>(f.w) * f.h);
	for (int y = 0; y < f.h; ++y)
		for (int x = 0; x < f.w; ++x)
			f.plane[static_cast<size_t>(y) * f.w + x] = planeA[static_cast<size_t>(2 * y + 1) * w + 2 * x + 1];
	// No model in it (ZBrush clears both planes when a turn starts, and two
	// cleared planes are alike too): not a finished frame.
	size_t modelHalf = 0;
	for (float v : f.plane)
		modelHalf += v != bg && fabs(v) < 1e6;
	if (modelHalf < 200)
		return;
	// Persp: ZBrush's value at depth 0 is its pivot less twice the eye's
	// distance, E = zoom / invD (dumps at zooms 0.91, 1 and 2.33).
	if (invD > 0) {
		f.cam.invD = invD;
		f.cam.pivot = pivot - 2 * st.ratio / invD;
		if (!PerspCheck(f, pivot))
			return;
	}
	size_t added = 0, gone = 0, modelPx = 0;
	static int rejected = 0;
	if (!CloudIngest(f, added, gone, modelPx)) {
		// Three in a row: the points are what is out of step (the model
		// replaced, say), and start again from this frame.
		const bool restart = ++rejected >= 3;
		char line[200];
		snprintf(line, sizeof line, "finished frame %s: it finds %zu of the points it sees gone", restart ? "taken alone, the points started again" : "not taken (out of step with the camera)", gone);
		DepthLog(line);
		if (!restart)
			return;
		gCloud.clear();
		gCloudAt.clear();
		gFrames.clear();
		CloudIngest(f, added, gone, modelPx);
	}
	rejected = 0;
	for (size_t i = 0; i < gFrames.size(); ++i) {
		const double* a = gFrames[i].cam.d[2];
		if (a[0] * d[2][0] + a[1] * d[2][1] + a[2] * d[2][2] >= cos(20 * 3.14159265358979 / 180)) {
			gFrames.erase(gFrames.begin() + i);
			break;
		}
	}
	// Each kept frame saved for working out offline how the planes, the
	// pivot and the rotation fit (build 177: the best depth scale and
	// perspective still left 34-62% of points impossible between angles):
	// JRef_Data\frames\frame<n>.txt, the camera and every value it was taken
	// from, and frame<n>.bin, the half-size plane.
	if (!gTesting) {
		static int saved = 0;
		const std::wstring dir = DataFolder() + L"frames\\";
		CreateDirectoryW(dir.c_str(), nullptr);
		const int n = saved++ % 16;
		if (FILE* t = _wfopen((dir + L"frame" + std::to_wstring(n) + L".txt").c_str(), L"wb")) {
			fprintf(t, "tick %lu\nsize %d %d\nempty %.9g\npivot %.9g\nzoom %.9g\npos %.9g %.9g\ncentre %.9g %.9g\ndoc %.9g %.9g\noff %.9g %.9g\nanchorscale %.9g\n",
				now, f.w, f.h, f.bg, pivot, st.ratio, st.px, st.py, f.cam.cx, f.cam.cy, st.docW, st.docH, st.offX, st.offY, gAnchorScale);
			if (invD > 0)
				fprintf(t, "persp invD %.9g eye %.9g value at depth 0 %.9g\n", invD, st.ratio / invD, f.cam.pivot);
			fprintf(t, "axes %.9g %.9g %.9g  %.9g %.9g %.9g  %.9g %.9g %.9g\n", d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2], d[2][0], d[2][1], d[2][2]);
			if (gTripletInUse >= 0 && gTripletInUse < static_cast<int>(gTriplets.size())) {
				const MemTriplet& tr = gTriplets[gTripletInUse];
				float v[8] = {};
				ReadOwn(tr.x.addr - 16, v, sizeof v);
				fprintf(t, "triplet x at %p, scale at %+lld: around x", reinterpret_cast<void*>(tr.x.addr), static_cast<long long>(tr.s.addr) - static_cast<long long>(tr.x.addr));
				for (float a : v)
					fprintf(t, " %.9g", a);
				fprintf(t, "\n");
			}
			RotDump(t);
			fclose(t);
		}
		if (FILE* b = _wfopen((dir + L"frame" + std::to_wstring(n) + L".bin").c_str(), L"wb")) {
			fwrite(f.plane.data(), 4, f.plane.size(), b);
			fclose(b);
		}
	}
	gFrames.push_back(std::move(f));
	if (gFrames.size() > kFrames)
		gFrames.erase(gFrames.begin());
	++gCloudVersion;
	static DWORD lastLog = 0;
	if (now - lastLog > 5000) {
		lastLog = now;
		char line[300];
		snprintf(line, sizeof line, "finished frame into the model's points: %zu model pixels, %zu points new, %zu gone, %zu in all, %zu angles kept; empty %g, pivot %.3f, zoom %.5f, towards the camera %+.3f %+.3f %+.3f",
			modelPx, added, gone, gCloud.size(), gFrames.size(), bg, pivot, st.ratio, d[2][0], d[2][1], d[2][2]);
		DepthLog(line);
	}
	// The cells are laid out again with it.
	EnterCriticalSection(&gOverlayLock);
	gRedrawPending = true;
	LeaveCriticalSection(&gOverlayLock);
}

// The surface round the camera's ray through a document point (x, y), from
// the points: each point seen from the camera (Cam, with the measured depth
// scale and perspective), the nearest in each bin of a grid round the point
// (bins in model units at each point's depth), then the bins within reach
// averaged with weights falling smoothly to nothing at the reach; thick, the
// width of the model's runs across the rows within reach, weighted the same
// way (a row whose run changes part, an arm crossing it, moves it by its
// small share: build 172's median of 7 rows moved the cell 50 px for 20 px
// of panning), standing in for the part's thickness. In model units,
// front towards the camera. False with fewer than 10 bins of the model in
// reach. The last answer is kept for the same question (each cell asks).
static bool CloudDepth(const Cam& c, double qx, double qy, double reach, double& front, double& thick, int& samples)
{
	samples = 0;
	thick = 0;
	if (gCloud.empty() || reach <= 0 || c.r <= 0)
		return false;
	static double lastQ[18] = {};
	static int lastVersion = -1, lastSamples = 0;
	static double lastFront = 0, lastThick = 0;
	static bool lastOk = false;
	const double q[18] = {qx, qy, reach, c.px, c.py, c.r, c.cx, c.cy, c.invD, c.d[0][0], c.d[0][1], c.d[0][2], c.d[1][0], c.d[1][1], c.d[1][2], c.d[2][0], c.d[2][1], c.d[2][2]};
	if (lastVersion == gCloudVersion && !memcmp(q, lastQ, sizeof q)) {
		front = lastFront;
		thick = lastThick;
		samples = lastSamples;
		return lastOk;
	}
	memcpy(lastQ, q, sizeof q);
	lastVersion = gCloudVersion;
	// Bins a fortieth of the reach (builds 172-198: a twelfth, 16 units at
	// zoom 1, which put the window's middle on the profile seen side on
	// 70-90 units off the surface), at least two and a half of the points'
	// cells: at one cell the near surface has gaps, and a bin in one shows the
	// far side of the head (400 units off zoomed in, dll\test).
	const double b = std::max(2.5 * kCloudCell, reach / 40);
	const int nu = static_cast<int>(ceil(3 * reach / b)), nv = static_cast<int>(ceil(reach / b));
	const int gw = 2 * nu + 1, gh = 2 * nv + 1;
	// The points in bands, one per core, each into its own grid (up to 180,000
	// points with six angles kept, every layout the camera moves). Each bin
	// keeps its nearest point where it is, not at the bin's middle: where the
	// surface turns away (the forehead seen side on) the nearest of a bin
	// sits half a bin to the camera's side, and taken at the middle put the
	// surface 10-60 units in front of the ray's (dll\test).
	struct Bin { float z, u, v; };
	static std::vector<Bin> grid;
	static std::vector<std::vector<Bin>> bandGrids;
	const size_t cells = static_cast<size_t>(gw) * gh;
	grid.assign(cells, Bin{-1e30f, 0, 0});
	const int n = static_cast<int>(gCloud.size());
	const int bands = std::max(1, std::min(ScanWorkers(), n / 8192));
	bandGrids.resize(bands);
	concurrency::parallel_for(0, bands, [&](int band) {
		std::vector<Bin>& bg = bandGrids[band];
		bg.assign(cells, Bin{-1e30f, 0, 0});
		for (int k = n * band / bands, end = n * (band + 1) / bands; k < end; ++k) {
			double x, y, z;
			if (!CamProject(c, gCloud[k].p, c.invD, x, y, z))
				continue;
			// Off the ray in model units at the point's own depth: in
			// perspective a part near the eye is drawn larger, and at the
			// pivot's depth its points would sit further apart than a bin (the
			// brow over the eye at x2.9, the eye found through the gaps:
			// dll\test).
			const double q = 1 - z * c.invD;
			const double su = (x - qx) * q / c.r, sv = (y - qy) * q / c.r;
			if (fabs(su) > 3 * reach || fabs(sv) > reach)
				continue;
			const long i = lround(su / b) + nu, j = lround(sv / b) + nv;
			if (i < 0 || j < 0 || i >= gw || j >= gh)
				continue;
			Bin& cell = bg[static_cast<size_t>(j) * gw + i];
			if (z > cell.z)
				cell = Bin{static_cast<float>(z), static_cast<float>(su), static_cast<float>(sv)};
		}
	});
	for (const auto& bg : bandGrids)
		for (size_t k = 0; k < cells; ++k)
			if (bg[k].z > grid[k].z)
				grid[k] = bg[k];
	auto at = [&](int i, int j) { return i >= 0 && j >= 0 && i < gw && j < gh && grid[static_cast<size_t>(j) * gw + i].z > -1e29f; };
	// The surface found last time, where the camera now sees it: while it is
	// still at the ray, the ray stays on it (see the seed below).
	static double prevG[3] = {};
	static bool havePrev = false;
	bool usePrev = false;
	double prevZ = 0;
	if (havePrev && !gTesting) {
		const float pg[3] = {static_cast<float>(prevG[0]), static_cast<float>(prevG[1]), static_cast<float>(prevG[2])};
		double px, py, pz;
		if (CamProject(c, pg, c.invD, px, py, pz)) {
			const double q = 1 - pz * c.invD;
			usePrev = hypot((px - qx) * q / c.r, (py - qy) * q / c.r) < 6 * b;
			prevZ = pz;
		}
	}
	// The surface where the ray meets it: a plane (depth = a + b du + c dv)
	// fitted to the bins round the ray, weighted falling smoothly to nothing
	// at the radius, the bins off it by over three cells (another part,
	// behind or in front) left out and the plane fitted again; its depth on
	// the ray. Within two and a half bins of the ray when 6 are there, else
	// six bins, else a third of the reach, else the whole reach (10 bins at
	// least). Builds
	// 172-198 averaged the bins over the reach, which leaned to the side with
	// more model: seen side on, the forehead's and chin's surface came out
	// 70-120 units off the ray's, and the front cell half its window off; a
	// third of the reach still took in the cheek for the lips seen side on,
	// 20-30 off (dll\test).
	auto plane = [&](double radius, double& depth) {
		// Only the surface the point nearest the ray is on: bins in front of
		// it or behind by more than 6 bins, and 3 units a unit away (steeper
		// than 72 degrees), are another part (the nose's edge over the eye
		// beyond it, seen at 60 degrees: 60-80 units off, dll\test).
		// Where the ray grazes a part's edge (the nose's, the cheek beyond
		// it), the bin nearest the ray is on one or the other as the camera
		// moves a pixel: build 218 found the nose and the cheek 60 units apart
		// in turn, frame by frame through a turn, and both cells jumped by a
		// fifth of their window. Of the bins within a bin and a half of the
		// ray, the one nearest the surface found last time is the seed.
		std::vector<char> out(cells, 0);
		const Bin* seed = nullptr;
		if (usePrev)
			for (size_t k = 0; k < cells; ++k)
				if (grid[k].z > -1e29f && grid[k].u * grid[k].u + grid[k].v * grid[k].v <= 2.25 * b * b && (!seed || fabs(grid[k].z - prevZ) < fabs(seed->z - prevZ)))
					seed = &grid[k];
		if (!seed)
			for (size_t k = 0; k < cells; ++k)
				if (grid[k].z > -1e29f && (!seed || grid[k].u * grid[k].u + grid[k].v * grid[k].v < seed->u * seed->u + seed->v * seed->v))
					seed = &grid[k];
		if (!seed)
			return 0;
		for (size_t k = 0; k < cells; ++k)
			if (grid[k].z > -1e29f && fabs(grid[k].z - seed->z) > 6 * b + 3 * hypot(grid[k].u - seed->u, grid[k].v - seed->v))
				out[k] = 1;
		int used = 0;
		for (int pass = 0; pass < 3; ++pass) {
			double m[3][4] = {};
			used = 0;
			for (int j = 0; j < gh; ++j)
				for (int i = 0; i < gw; ++i) {
					if (!at(i, j) || out[static_cast<size_t>(j) * gw + i])
						continue;
					const Bin& p = grid[static_cast<size_t>(j) * gw + i];
					const double du = p.u, dv = p.v, r2 = (du * du + dv * dv) / (radius * radius);
					if (r2 >= 1)
						continue;
					const double wgt = (1 - r2) * (1 - r2), x[3] = {1, du, dv}, z = p.z;
					for (int r = 0; r < 3; ++r) {
						for (int k = 0; k < 3; ++k)
							m[r][k] += wgt * x[r] * x[k];
						m[r][3] += wgt * x[r] * z;
					}
					++used;
				}
			if (used < 6)
				return used;
			// Gaussian elimination with partial pivoting.
			for (int col = 0; col < 3; ++col) {
				int piv = col;
				for (int r = col + 1; r < 3; ++r)
					if (fabs(m[r][col]) > fabs(m[piv][col]))
						piv = r;
				for (int k = 0; k < 4; ++k)
					std::swap(m[col][k], m[piv][k]);
				if (fabs(m[col][col]) < 1e-12)
					return 0;
				for (int r = 0; r < 3; ++r)
					if (r != col) {
						const double f = m[r][col] / m[col][col];
						for (int k = col; k < 4; ++k)
							m[r][k] -= f * m[col][k];
					}
			}
			const double a0 = m[0][3] / m[0][0], a1 = m[1][3] / m[1][1], a2 = m[2][3] / m[2][2];
			depth = a0;
			if (pass == 2)
				break;
			bool dropped = false;
			for (int j = 0; j < gh; ++j)
				for (int i = 0; i < gw; ++i) {
					if (!at(i, j) || out[static_cast<size_t>(j) * gw + i])
						continue;
					const Bin& p = grid[static_cast<size_t>(j) * gw + i];
					if (p.u * p.u + p.v * p.v >= radius * radius)
						continue;
					if (fabs(p.z - (a0 + a1 * p.u + a2 * p.v)) > 3 * b) {
						out[static_cast<size_t>(j) * gw + i] = 1;
						dropped = true;
					}
				}
			if (!dropped)
				break;
		}
		return used;
	};
	double depth = 0;
	samples = plane(2.5 * b, depth);
	if (samples < 6)
		samples = plane(6 * b, depth);
	if (samples < 6)
		samples = plane(reach / 3, depth);
	if (samples < 10)
		samples = plane(reach, depth);
	lastOk = samples >= 10 || (samples >= 6 && reach > 0);
	lastSamples = samples;
	if (!lastOk)
		return false;
	// No nearer or farther than the points at the ray (within a bin and a
	// half): at an outline the plane through the part's near side runs on
	// past its edge, in front of anything there (the nose's edge seen at 60
	// degrees, 80 units in front of it, dll\test).
	{
		float zLo = 1e30f, zHi = -1e30f;
		for (size_t k = 0; k < cells; ++k) {
			const Bin& p = grid[k];
			if (p.z > -1e29f && p.u * p.u + p.v * p.v <= 2.25 * b * b) {
				zLo = std::min(zLo, p.z);
				zHi = std::max(zHi, p.z);
			}
		}
		if (zHi >= zLo)
			depth = std::min<double>(zHi, std::max<double>(zLo, depth));
	}
	front = depth;
	CamPoint(c, qx, qy, front, prevG);
	havePrev = true;
	double runSum = 0, runWeights = 0;
	for (int j = 0; j < gh; ++j) {
		const double dv = (j - nv) * b, wgt = (1 - dv * dv / (reach * reach)) * (1 - dv * dv / (reach * reach));
		if (wgt <= 0 || fabs(dv) >= reach)
			continue;
		// The run through the model bin nearest the ray's column.
		for (int r = 0; r <= nv; ++r) {
			int hit = -1;
			if (at(nu - r, j))
				hit = nu - r;
			else if (at(nu + r, j))
				hit = nu + r;
			if (hit < 0)
				continue;
			int lo = hit, hi = hit;
			while (at(lo - 1, j))
				--lo;
			while (at(hi + 1, j))
				++hi;
			runSum += wgt * (hi - lo + 1) * b;
			runWeights += wgt;
			break;
		}
	}
	if (runWeights > 0)
		thick = runSum / runWeights;
	lastFront = front;
	lastThick = thick;
	return true;
}

// Each overlay step: finds the buffer and checks every 2 s that it still
// fits (a new document moves it), both only with the canvas steady for half
// a second: the capture lags ZBrush's redraw, and a check during a camera
// move dropped the real buffer 3 s after it was found (build 159: "lost:
// agrees with 120 of 360 pixels"), and the next scan waited 15 s. Two
// failed checks in a row drop it; a scan follows at once, then every 5 s.
static void DepthStep(const OverlayState& st)
{
	static DWORD lastScan = 0, lastCheck = 0, settledAt = 0;
	static double lastCount = -1;
	static int failed = 0;
	const DWORD now = GetTickCount();
	if (gGesture.active || gKeyShare.empty() || fabs(gModelPixels - lastCount) > 0.002 * (gModelPixels + 100)) {
		lastCount = gModelPixels;
		settledAt = now;
		return;
	}
	if (now - settledAt < 500)
		return;
	if (gDepth.image && !gDepthImages.empty()) {
		// Found by ZBrush's objects: every 2 s, each followed again (it may
		// have moved the pixels) and the one that agrees with the mask taken,
		// on the model and off it apart (a plane holding only the empty value
		// agrees with nine samples in ten where the model fills a tenth).
		if (now - lastCheck < 2000)
			return;
		lastCheck = now;
		const std::vector<DepthSample> s = DepthSamples(st, 400);
		const DepthBuffer was = gDepth;
		double bestScore = -1;
		DepthBuffer best = gDepth;
		int bestModel = 0, bestModelTotal = 0;
		static DWORD lastDiag = 0;
		const bool diag = now - lastDiag > 6000;
		if (diag)
			lastDiag = now;
		for (auto& im : gDepthImages) {
			const uintptr_t first = ImagePixels(im.first, gDepth.w, gDepth.h);
			if (!first)
				continue;
			gDepth.ok = true;
			gDepth.base = first;
			gDepth.image = im.first;
			// The empty value, again from the canvas's samples: ZBrush changes
			// it (24000, then 35840 a minute later, watching from outside).
			{
				std::vector<double> empties;
				for (const DepthSample& p : s) {
					double v;
					if (!p.model && DepthAtDoc(p.x, p.y, v))
						empties.push_back(v);
				}
				std::sort(empties.begin(), empties.end());
				size_t run = 0, most = 0;
				double mode = im.second;
				for (size_t i = 0; i < empties.size(); ++i) {
					run = i && empties[i] == empties[i - 1] ? run + 1 : 1;
					if (run > most) {
						most = run;
						mode = empties[i];
					}
				}
				if (most * 2 > empties.size() && fabs(mode) >= 100)
					im.second = mode;
			}
			gDepth.bg = im.second;
			int on = 0, onTotal = 0, off = 0, offTotal = 0;
			for (const DepthSample& p : s) {
				double v;
				if (!DepthAtDoc(p.x, p.y, v))
					continue;
				const bool model = !DepthSame(v, gDepth.bg);
				(p.model ? onTotal : offTotal)++;
				(p.model ? on : off) += model == p.model;
			}
			if (onTotal < 10 || offTotal < 10)
				continue;
			const double score = std::min(static_cast<double>(on) / onTotal, static_cast<double>(off) / offTotal);
			// Where the plane's model is against the mask's, to tell a stale
			// plane (the model where it was) from an empty one or one off by a
			// scale or shift (build 165: the model at 4 of the mask's 65).
			if (score < 0.85 && diag) {
				std::vector<float> plane(static_cast<size_t>(gDepth.w) * gDepth.h);
				if (ReadOwn(first, plane.data(), plane.size() * 4)) {
					size_t count = 0;
					int px0 = gDepth.w, py0 = gDepth.h, px1 = -1, py1 = -1;
					for (int y = 0; y < gDepth.h; ++y)
						for (int x = 0; x < gDepth.w; ++x) {
							const double v = plane[static_cast<size_t>(y) * gDepth.w + x];
							if (DepthSame(v, gDepth.bg) || !(fabs(v) < 1e6))
								continue;
							++count;
							px0 = std::min(px0, x);
							py0 = std::min(py0, y);
							px1 = std::max(px1, x);
							py1 = std::max(py1, y);
						}
					int mx0 = 1 << 30, my0 = 1 << 30, mx1 = -1, my1 = -1;
					for (const DepthSample& p : s)
						if (p.model) {
							mx0 = std::min(mx0, p.x);
							my0 = std::min(my0, p.y);
							mx1 = std::max(mx1, p.x);
							my1 = std::max(my1, p.y);
						}
					char line[300];
					snprintf(line, sizeof line, "  image %p: model %d/%d, canvas %d/%d; plane's model %zu px in %d %d to %d %d, the mask's in %d %d to %d %d", reinterpret_cast<void*>(im.first),
						on, onTotal, off, offTotal, count, px0, py0, px1, py1, mx0, my0, mx1, my1);
					DepthLog(line);
				}
			}
			// The plane in use stays while it fits (CanvasDepth moves to the
			// plane ZBrush redraws as the view moves; both fit while it is still).
			if (score > bestScore + (im.first == was.image ? -0.05 : best.image == was.image && bestScore >= 0.85 ? 0.05 : 0)) {
				bestScore = score;
				best = gDepth;
				bestModel = on;
				bestModelTotal = onTotal;
			}
		}
		if (bestScore < 0 && ImagePixels(was.image, was.w, was.h)) {
			// Too little model or canvas in view to judge: as it was.
			gDepth = was;
			gDepth.base = ImagePixels(was.image, was.w, was.h);
			return;
		}
		gDepth = best;
		gDepth.ok = bestScore >= 0.85;
		if (gDepth.ok != was.ok || gDepth.image != was.image) {
			char line[200];
			snprintf(line, sizeof line, "%s image %p: the model at %d of the mask's %d model pixels", gDepth.ok ? "using" : "no plane fits, best",
				reinterpret_cast<void*>(gDepth.image), bestModel, bestModelTotal);
			DepthLog(line);
		}
		if (bestScore < 0 && !ImagePixels(gDepth.image, gDepth.w, gDepth.h)) {
			DepthLog("image objects gone, looking again");
			gDepth = DepthBuffer();
			gDepthImages.clear();
			lastScan = 0;
		}
		return;
	}
	if (!gDepth.ok) {
		// A scan copies a few hundred MB (0.9 s, build 161): at most every
		// 30 s, at once after a drop.
		if (now - lastScan < 30000 && lastScan)
			return;
		lastScan = now;
		DepthScan(st);
		failed = 0;
		return;
	}
	if (now - lastCheck < 2000)
		return;
	lastCheck = now;
	if (gDepth.image) {
		// Found by ZBrush's objects: followed again (it may have moved the
		// pixels), not judged by the mask.
		const uintptr_t first = ImagePixels(gDepth.image, gDepth.w, gDepth.h);
		if (!first) {
			DepthLog("image object gone, looking again");
			gDepth.ok = false;
			lastScan = 0;
		} else {
			gDepth.base = first;
		}
		return;
	}
	const std::vector<DepthSample> s = DepthSamples(st, 400);
	int agree = 0, total = 0;
	for (const DepthSample& p : s) {
		double v;
		if (!DepthAtDoc(p.x, p.y, v))
			continue;
		++total;
		agree += !DepthSame(v, gDepth.bg) == p.model;
	}
	if (total >= 50 && agree < total * 0.85) {
		char line[160];
		snprintf(line, sizeof line, "check failed: agrees with %d of %d pixels%s", agree, total, failed ? ", dropped" : "");
		DepthLog(line);
		if (++failed >= 2) {
			gDepth.ok = false;
			gDepthDropped.push_back(gDepth.base);
			lastScan = 0;
		}
	} else {
		failed = 0;
	}
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
	// Changed in a drag taken for a pan or zoom: judged once the drag is over
	// (RotCodes).
	bool panStep = false;
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

// The rotation's search in parts, like MemScanJob.
struct RotScanJob {
	static const size_t kMax = 200000;
	std::vector<double> traces;
	int workers;
	std::vector<std::vector<RotCand>> found;
	std::vector<size_t> rowsBy;
	std::vector<std::vector<float>> normsF;
	std::vector<std::vector<double>> normsD;
	explicit RotScanJob(const double* deg);
	void Piece(int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got);
	void Finish(size_t scanned, DWORD t0);
};

RotScanJob::RotScanJob(const double* deg) : traces(EulerTraces(deg)), workers(ScanWorkers()), found(workers), rowsBy(workers),
	normsF(workers), normsD(workers)
{
	gRotCands.clear();
	gRotNarrowings = 0;
}

void RotScanJob::Piece(int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got)
{
	{
		std::vector<RotCand>& out = found[w];
		size_t rows = 0;
		// The squared length of the three values from every position, as
		// floats and as doubles, in plain loops the compiler does several at
		// a time; the quick tests below then only compare (build 183 worked
		// each out per position and type: most of the scan's time). A 4 KB
		// block at a time, so the lengths stay in the core's cache (build
		// 184 did the whole 4 MB piece first, and wrote and read back 8 MB
		// more per piece than it scanned: 1.5 s against 183's 1.0 s).
		const size_t cf = got / 4, cd = got / 8;
		std::vector<float>& nf = normsF[w];
		std::vector<double>& nd = normsD[w];
		nf.resize(cf);
		nd.resize(cd);
		const float* fv = reinterpret_cast<const float*>(buf);
		const double* dv = reinterpret_cast<const double*>(buf);
		const size_t kBlock = 4096;
		for (size_t i = 0; i < starts && i + 11 * 4 <= got; i += 4) {
			if (i % kBlock == 0) {
				// Lengths for this block's positions and the 4 rows past it.
				const size_t fEnd = std::min(cf >= 2 ? cf - 2 : 0, (i + kBlock) / 4 + 4);
				const size_t dEnd = std::min(cd >= 2 ? cd - 2 : 0, (i + kBlock) / 8 + 4);
				for (size_t k = i / 4; k < fEnd; ++k)
					nf[k] = fv[k] * fv[k] + fv[k + 1] * fv[k + 1] + fv[k + 2] * fv[k + 2];
				for (size_t k = i / 8; k < dEnd; ++k)
					nd[k] = dv[k] * dv[k] + dv[k + 1] * dv[k + 1] + dv[k + 2] * dv[k + 2];
			}
			for (int isDouble = 0; isDouble < 2; ++isDouble) {
				const size_t sz = isDouble ? 8 : 4;
				if (isDouble && (i % 8 != 0 || i + 11 * 8 > got))
					continue;
				// Quick tests first: the first row has a sensible length, and
				// the second row the same length (Unscale's test without the
				// square roots; most memory fails it, and the full test on
				// every sensible row was most of the scan's time).
				const size_t k = i / sz;
				const double n0 = isDouble ? nd[k] : nf[k];
				if (!(n0 > 1e-12 && n0 < 1e18))
					continue;
				++rows;
				for (int stride = 3; stride <= 4; ++stride) {
					if (i + static_cast<size_t>(2 * stride + 3) * sz > got || out.size() >= kMax)
						continue;
					const double n1 = isDouble ? nd[k + stride] : nf[k + stride];
					if (!(fabs(n1 / n0 - 1) <= 4.1e-3))
						continue;
					double m[9];
					RotFromBytes(&buf[i], isDouble != 0, stride, m);
					if (!Unscale(m) || !IsRotation(m) || RotCode(m) >= 0)
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
					RotCand c = {at + i, isDouble != 0, stride, {}, 0, {}, fabs(sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]) - 1) < 2e-3, {}, {}, -1};
					memcpy(c.m, m, sizeof(m));
					memcpy(c.m1, m, sizeof(m));
					out.push_back(c);
				}
			}
		}
		rowsBy[w] += rows;
	}
}

void RotScanJob::Finish(size_t scanned, DWORD t0)
{
	size_t rows = 0;
	for (int w = 0; w < workers; ++w) {
		rows += rowsBy[w];
		for (size_t k = 0; k < found[w].size() && gRotCands.size() < kMax; ++k)
			gRotCands.push_back(found[w][k]);
	}
	// In address order, as one pass over memory found them.
	std::sort(gRotCands.begin(), gRotCands.end(), [](const RotCand& a, const RotCand& b) { return a.addr < b.addr; });
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu rotation matrix scan: %zu MB in %lu ms on %d cores, %zu rows tried, %zu rotations by this angle found%s\n", GetTickCount(),
			scanned >> 20, GetTickCount() - t0, workers, rows, gRotCands.size(), gRotCands.size() >= kMax ? " (FULL)" : "");
		fclose(f);
	}
}

static void RotScan(const double* deg)
{
	const DWORD t0 = GetTickCount();
	RotScanJob job(deg);
	const size_t scanned = ScanMemory(11 * 8, CameraRegion, [&](int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got) {
		job.Piece(w, at, buf, starts, got);
	});
	job.Finish(scanned, t0);
}

// Both searches in one pass over memory: reading ZBrush's 3.5-4 GB is most of
// each (build 188: 1.4 s for the camera, then 1.3 s more for the rotation).
static void CameraScan(double sx, double px, double py, const double* deg)
{
	const DWORD t0 = GetTickCount();
	MemScanJob mem(sx, px, py);
	RotScanJob rot(deg);
	const size_t scanned = ScanMemory(11 * 8, CameraRegion, [&](int w, uintptr_t at, const unsigned char* buf, size_t starts, size_t got) {
		mem.Piece(w, at, buf, starts, got);
		rot.Piece(w, at, buf, starts, got);
	});
	mem.Finish(scanned, t0);
	rot.Finish(scanned, t0);
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

// Every rotation copy as read now, raw and in the reference layout.
static void RotDump(FILE* f)
{
	for (const RotCand& c : gRotCands) {
		double m[9], canon[9];
		if (!RotRead(c, m))
			continue;
		fprintf(f, "rotation %s %s stride %d relation %d raw", c.unit ? "pure" : "scaled", c.isDouble ? "double" : "float", c.stride, c.rel);
		for (double v : m)
			fprintf(f, " %.6f", v);
		if (c.rel >= 0) {
			RotApply(c.rel, m, canon);
			fprintf(f, " canon");
			for (double v : canon)
				fprintf(f, " %.6f", v);
		}
		fprintf(f, "\n");
	}
}

// The camera's axes in the model's frame at any angle, as ViewAxes gives a
// snapped view's (d[0] screen right, d[1] screen down, d[2] towards the
// camera), from the rotation copies: each put in the reference layout (the
// one kViewCodes was read from) and its rows made unit length (some copies
// carry the zoom); of the copies two or more agree on, those that changed
// last (RotCodes), the most of them on a tie. Through a turn only three of
// ZBrush's nine follow it, the rest catching up when the button is let go:
// builds to 215 took the one most agreed with, the rotation from before the
// turn, all through it, and with Persp on (ZBrush's pan and zoom move as it
// turns) the cells ran half a window off and snapped back on release.
// False until the copies are found.
static bool CameraAxes(double d[3][3])
{
	if (gTesting) {
		memcpy(d, gTestCam.d, sizeof gTestCam.d);
		return true;
	}
	if (!gRotLocked)
		return false;
	if (gAxesHold && gAxesHeldSet) {
		memcpy(d, gAxesHeld, sizeof gAxesHeld);
		return true;
	}
	std::vector<std::vector<double>> got;
	std::vector<DWORD> changed;
	const DWORD now = GetTickCount();
	for (const RotCand& c : gRotCands) {
		if (c.rel < 0)
			continue;
		double m[9], canon[9];
		if (!RotRead(c, m))
			continue;
		// Changed since RotCodes last read it: changed now.
		bool fresh = false;
		for (int k = 0; k < 9; ++k)
			fresh = fresh || fabs(m[k] - c.m[k]) > 5e-4;
		RotApply(c.rel, m, canon);
		bool ok = true;
		for (int r = 0; r < 3 && ok; ++r) {
			const double len = sqrt(canon[r * 3] * canon[r * 3] + canon[r * 3 + 1] * canon[r * 3 + 1] + canon[r * 3 + 2] * canon[r * 3 + 2]);
			ok = len > 1e-6;
			for (int k = 0; k < 3 && ok; ++k)
				canon[r * 3 + k] /= len;
		}
		if (ok) {
			got.push_back(std::vector<double>(canon, canon + 9));
			changed.push_back(fresh ? now : c.changedAt);
		}
	}
	if (got.empty())
		return false;
	// Turning, or turned under a second ago: ZBrush copies the turned rotation
	// into the six copies that wait for the button some 80 ms after it is let
	// go, and a read between its writes took the six from before the turn for
	// a frame (build 221, the cells off and back on release).
	static DWORD turnedAt = 0;
	if (Turning())
		turnedAt = now;
	const bool turning = now - turnedAt < 1000;
	size_t best = 0;
	int bestAgree = -1;
	DWORD bestAt = 0;
	for (size_t i = 0; i < got.size(); ++i) {
		int agree = 0;
		DWORD at = changed[i];
		for (size_t j = 0; j < got.size(); ++j) {
			double diff = 0;
			for (int k = 0; k < 9; ++k)
				diff = std::max(diff, fabs(got[i][k] - got[j][k]));
			if (diff < 1e-3) {
				++agree;
				if (static_cast<int>(changed[j] - at) > 0)
					at = changed[j];
			}
		}
		// A group of two or more over a lone copy (not while turning); then
		// the one that changed last; then the larger. While turning a lone copy
		// is a live one caught mid-step: ZBrush writes its three live copies
		// one after another, a read between the writes finds all three apart,
		// and build 216 then took the six from before the turn for that frame
		// (the cells flicked back and forth through the turn).
		bool better;
		if (bestAgree < 0)
			better = true;
		else if (!turning && (agree >= 2) != (bestAgree >= 2))
			better = agree >= 2;
		else if (at != bestAt)
			better = static_cast<int>(at - bestAt) > 0;
		else
			better = agree > bestAgree;
		if (better) {
			bestAgree = agree;
			bestAt = at;
			best = i;
		}
	}
	double m[9];
	RotFix(got[best].data(), m);
	for (int c = 0; c < 3; ++c) {
		d[0][c] = m[c];
		d[1][c] = -m[3 + c];
		d[2][c] = m[6 + c];
	}
	if (gAxesHold) {
		memcpy(gAxesHeld, d, sizeof gAxesHeld);
		gAxesHeldSet = true;
	}
	return true;
}

// ZBrush's camera through a turn with Persp on. ZBrush turns about a point
// of the model (Q, a point of the surface, its own for each turn) that stays
// where it is on screen at the same size: three still states either side of
// two turns (frames, build 220) put one point at the same place and size to
// 0.00 px, and every pan and zoom ZBrush wrote through an 8 s turn kept that
// point at its distance from the origin to 0.5 units, at zooms 1.9 to 5.1.
// ZBrush's pan and zoom (the origin's place and size) swing 300-600 px a
// frame then, and read a frame apart from the rotation they put the cells
// anywhere (builds 216-220); with Q, the rotation alone gives them, as it
// gives them to ZBrush. Q is solved from the pan and zoom ZBrush writes in
// the turn, each a point at Q's distance from the origin whatever rotation it
// went with, so the reads need not be paired with the rotation at all. The
// eye is ZBrush's record as read with each (it moves through a drag). Builds
// 221-226 took it as max(E0, 0.7257 zoom E0), as in build 220's frames
// (zooms 2.3-5.1); still at zoom 1.51 the record read 1415, not 1551, and no
// turn's Q fitted (build 226: none in a session).
struct Orbit {
	bool restOk = false;  // the state the turn started from, held still
	double restR = 0, restX = 0, restY = 0, restEye = 0, restD[3][3] = {}, cx = 0, cy = 0;
	bool turning = false, solved = false, logged = false, haveQ = false;
	double q[3] = {}, sx = 0, sy = 0, m = 0, rms = 0;
	std::vector<std::array<double, 4>> samples;  // ZBrush's (zoom, x, y, eye) this turn
};
static Orbit gOrbit;

// Q's place and size on screen in the still state.
static bool OrbitAnchor(Orbit& o, const double q[3], double& sx, double& sy, double& m)
{
	double s[3];
	for (int a = 0; a < 3; ++a)
		s[a] = q[0] * o.restD[a][0] + q[1] * o.restD[a][1] + q[2] * o.restD[a][2];
	const double f = 1 - s[2] * o.restR / o.restEye;
	if (f <= 0.05)
		return false;
	sx = o.cx + (s[0] * o.restR + o.restX - o.cx) / f;
	sy = o.cy + (s[1] * o.restR + o.restY - o.cy) / f;
	m = o.restR / f;
	return true;
}

// Each written pan and zoom's distance of Q from the origin, less Q's own.
static bool OrbitResiduals(Orbit& o, const double q[3], std::vector<double>& res)
{
	double sx, sy, m;
	if (!OrbitAnchor(o, q, sx, sy, m))
		return false;
	const double len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
	res.clear();
	for (const auto& p : o.samples) {
		const double r = p[0], f = r / m, e = p[3];
		const double z = (1 - f) * e / r, a = (o.cx + (sx - o.cx) * f - p[1]) / r, b = (o.cy + (sy - o.cy) * f - p[2]) / r;
		res.push_back(sqrt(a * a + b * b + z * z) - len);
	}
	return true;
}

// Gauss-Newton from a starting point; false if it fails. rms in model units.
static bool OrbitFit(Orbit& o, double q[3], double& rms)
{
	std::vector<double> res, step;
	for (int it = 0; it < 30; ++it) {
		if (!OrbitResiduals(o, q, res))
			return false;
		const size_t n = res.size();
		std::vector<std::array<double, 3>> jac(n);
		for (int t = 0; t < 3; ++t) {
			double qq[3] = {q[0], q[1], q[2]};
			qq[t] += 0.5;
			if (!OrbitResiduals(o, qq, step))
				return false;
			for (size_t i = 0; i < n; ++i)
				jac[i][t] = (step[i] - res[i]) / 0.5;
		}
		double m[3][4] = {};
		for (size_t i = 0; i < n; ++i)
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c)
					m[r][c] += jac[i][r] * jac[i][c];
				m[r][3] -= jac[i][r] * res[i];
			}
		const double scale = m[0][0] + m[1][1] + m[2][2];
		for (int col = 0; col < 3; ++col) {
			int piv = col;
			for (int r = col + 1; r < 3; ++r)
				if (fabs(m[r][col]) > fabs(m[piv][col]))
					piv = r;
			for (int c = 0; c < 4; ++c)
				std::swap(m[col][c], m[piv][c]);
			// Samples too alike to fix all three of Q's coordinates.
			if (fabs(m[col][col]) < 1e-6 * scale)
				return false;
			for (int r = 0; r < 3; ++r)
				if (r != col) {
					const double f = m[r][col] / m[col][col];
					for (int c = col; c < 4; ++c)
						m[r][c] -= f * m[col][c];
				}
		}
		double moved = 0;
		for (int t = 0; t < 3; ++t) {
			const double dq = std::max(-200.0, std::min(200.0, m[t][3] / m[t][t]));
			q[t] += dq;
			moved = std::max(moved, fabs(dq));
		}
		if (moved < 0.01)
			break;
	}
	if (!OrbitResiduals(o, q, res) || res.empty())
		return false;
	double sum = 0;
	for (double v : res)
		sum += v * v;
	rms = sqrt(sum / res.size());
	return true;
}

static void OrbitLog(const char* line)
{
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu persp turn: %s\n", GetTickCount(), line);
		fclose(f);
	}
}

// ZBrush's pan and zoom for rotation d from the orbit, with the eye at eye.
static bool OrbitCam(const Orbit& o, const double d[3][3], double eye, double& r, double& x, double& y)
{
	double s[3];
	for (int a = 0; a < 3; ++a)
		s[a] = o.q[0] * d[a][0] + o.q[1] * d[a][1] + o.q[2] * d[a][2];
	// Q's size m is zoom / (1 - z zoom / eye).
	r = o.m / (1 + o.m * s[2] / eye);
	const double f = r / o.m;
	if (!(r > 0) || f <= 0.05)
		return false;
	x = o.cx + (o.sx - o.cx) * f - s[0] * r;
	y = o.cy + (o.sy - o.cy) * f - s[1] * r;
	return true;
}

// st's pan and zoom for the cells: from the orbit at the rotation the cells
// are laid out with while ZBrush turns in Persp, as read otherwise. Keeps
// the still state, gathers ZBrush's pan and zoom through a turn and solves Q.
// gOrbitOn while the cells are to use it (ProjInvD then takes the eye the
// orbit went by: the record as read here).
static void OrbitPan(OverlayState& st)
{
	Orbit& o = gOrbit;
	gOrbitOn = false;
	double d[3][3];
	bool persp = false;
	double eye = 0;
	if (gTesting || !CameraAxes(d) || !ProjRead(st.docW, st.docH, persp, eye) || !persp || eye <= 0 || st.ratio <= 0) {
		o.restOk = o.turning = false;
		return;
	}
	// Each drag turns about its own point: a new button press starts a new
	// turn, from the orbit's state where the last one stopped (ZBrush's pan
	// and zoom may not have come to rest in between: build 221 kept the first
	// drag's point for the second, 260 px off at its end).
	static POINT down = {-1, -1};
	const bool newDrag = gGesture.active && (gGesture.down.x != down.x || gGesture.down.y != down.y);
	if (gGesture.active)
		down = gGesture.down;
	if (newDrag && o.turning && o.solved) {
		double r, x, y;
		if (OrbitCam(o, d, eye, r, x, y)) {
			o.restR = r;
			o.restX = x;
			o.restY = y;
			o.restEye = eye;
			memcpy(o.restD, d, sizeof d);
			o.restOk = true;
		}
		o.turning = false;
	}
	if (!Turning()) {
		static DWORD endedAt = 0;
		if (o.turning && o.solved) {
			// After the button is let go ZBrush's pan and zoom come to rest a
			// frame or more after its rotation; the orbit is used until they
			// meet it, so the cells do not move on release (build 221 went back
			// to ZBrush's pan and zoom at once, a frame of the old ones). A
			// second at most.
			double r, x, y;
			const bool ok = OrbitCam(o, d, eye, r, x, y);
			if (endedAt == 0)
				endedAt = GetTickCount();
			const bool met = ok && fabs(x - st.px) < 1.5 && fabs(y - st.py) < 1.5 && fabs(r / st.ratio - 1) < 0.003;
			if (ok && !met && GetTickCount() - endedAt < 1000) {
				st.ratio = r;
				st.px = x;
				st.py = y;
				gOrbitEye = eye;
				gOrbitOn = true;
				return;
			}
			if (ok) {
				char line[300];
				snprintf(line, sizeof line, "ended %lu ms after the button: the orbit puts the origin at %.2f %.2f zoom %.4f, ZBrush %.2f %.2f zoom %.4f%s",
					GetTickCount() - endedAt, x, y, r, st.px, st.py, st.ratio, met ? "" : " (NOT MET: the cells move to ZBrush's)");
				OrbitLog(line);
			}
		}
		endedAt = 0;
		o.turning = false;
		// Still, no button down: the state the next turn starts from. Build
		// 224 waited for a read to repeat it, but the cells are not laid out
		// again while nothing moves, so after the first turn every turn
		// started from the state before the last and found no orbit (the
		// cells on ZBrush's pan and zoom, snapping on release).
		if (!gGesture.active) {
			o.restOk = true;
			o.restR = st.ratio;
			o.restX = st.px;
			o.restY = st.py;
			o.restEye = eye;
			memcpy(o.restD, d, sizeof d);
			o.cx = st.docW * 0.5;
			o.cy = st.docH * 0.5;
		}
		return;
	}
	if (!o.turning) {
		o.turning = true;
		o.solved = o.logged = false;
		o.samples.clear();
	}
	if (!o.restOk)
		return;
	// A pan and zoom ZBrush has written this turn (up to 60 of them). Q is
	// solved again with each: from 6, the ones off the fit by over 3 units
	// left out (a read can take x, y and zoom from two of ZBrush's writes) and
	// it fitted again. Build 221 took 4, one more than Q's three unknowns,
	// which any read fits, and kept them: 50 px off at the turn's end.
	const bool fresh = (st.ratio != o.restR || st.px != o.restX || st.py != o.restY) &&
		(o.samples.empty() || st.ratio != o.samples.back()[0] || st.px != o.samples.back()[1] || st.py != o.samples.back()[2]);
	if (fresh && o.samples.size() < 60) {
		o.samples.push_back({st.ratio, st.px, st.py, eye});
		if (o.samples.size() >= 6) {
			// From where it was last solved, then from the surface the cells
			// showed at the turn's start (Q is a point near it: the nose, 30
			// units from it).
			double best[3] = {}, bestRms = 1e30;
			const double* starts[2] = {o.q, gFocusG};
			for (int s = 0; s < 2; ++s) {
				if (s == 0 && !o.haveQ)
					continue;
				if (s == 1 && !gFocusOk)
					continue;
				double q[3] = {starts[s][0], starts[s][1], starts[s][2]}, rms;
				if (OrbitFit(o, q, rms) && rms < bestRms) {
					bestRms = rms;
					memcpy(best, q, sizeof best);
				}
			}
			// Without the reads off the fit, then fitted again on the rest.
			size_t kept = o.samples.size();
			std::vector<double> res;
			if (bestRms < 1e29 && OrbitResiduals(o, best, res)) {
				std::vector<std::array<double, 4>> all = o.samples, in;
				for (size_t i = 0; i < all.size(); ++i)
					if (fabs(res[i]) <= 3)
						in.push_back(all[i]);
				kept = in.size();
				if (kept >= 6 && kept < all.size()) {
					o.samples = in;
					double rms;
					if (OrbitFit(o, best, rms))
						bestRms = rms;
					o.samples = all;
				}
			}
			// The samples must spread over more than a turn's first step,
			// and fit to under a unit, with 6 or more left.
			double spread = 0;
			for (const auto& p : o.samples)
				spread = std::max(spread, hypot(p[1] - o.samples[0][1], p[2] - o.samples[0][2]));
			o.rms = bestRms;
			if (bestRms < 1 && kept >= 6 && spread > 30) {
				memcpy(o.q, best, sizeof o.q);
				o.haveQ = true;
				o.solved = OrbitAnchor(o, o.q, o.sx, o.sy, o.m);
				if (o.solved && !o.logged) {
					o.logged = true;
					char line[300];
					snprintf(line, sizeof line, "about %.1f %.1f %.1f, held at %.2f %.2f size %.4f, from %zu of %zu of ZBrush's pans to %.2f units (eye %.1f at the start, %.1f now)",
						o.q[0], o.q[1], o.q[2], o.sx, o.sy, o.m, kept, o.samples.size(), o.rms, o.restEye, eye);
					OrbitLog(line);
				}
			}
		}
	}
	double r, x, y;
	if (o.solved && OrbitCam(o, d, eye, r, x, y)) {
		st.ratio = r;
		st.px = x;
		st.py = y;
		gOrbitEye = eye;
		gOrbitOn = true;
	} else if (o.samples.size() >= 6 && !o.solved) {
		// Why not, once a turn (the fit's state).
		static DWORD said = 0;
		if (GetTickCount() - said > 2000) {
			said = GetTickCount();
			char line[200];
			snprintf(line, sizeof line, "no orbit yet from %zu of ZBrush's pans (best fit %.2f units)", o.samples.size(), o.rms);
			OrbitLog(line);
		}
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

// Reads every candidate. Returns the code most of the snapped (axis-aligned)
// ones hold (every such code on a tie), and whether any changed since the
// last read (more than rounding). With sinceSettled, only the copies that
// changed since the view was last looked up count: a copy ZBrush stopped
// updating would otherwise keep reporting its old view. A lone copy left on
// an old view does not count either (nav_log: one copy still on Left while
// eight were on Front showed a Left image loaded on Front).
static std::vector<int> RotCodes(bool* changed = nullptr, bool sinceSettled = false)
{
	std::vector<int> codes, counts;
	bool any = false;
	// The camera turns only while a mouse button is held or in the snap
	// animation just after: a copy that changes with the buttons up and idle
	// follows something else (the brush under the cursor) and is dropped. So
	// is one that changes in a pan or a zoom, which never turn the camera
	// (build 205: one copy of nine read Front for a frame of an Alt pan in
	// Right; the view counted as turning for the rest of the drag, and the
	// pan went into the image's offset, leaving it behind). A copy that
	// changes in a pan or zoom is not counted then, and is only dropped once
	// the drag is over if no other copy has come to the same rotation: the
	// one copy ZBrush updates during a turn was dropped when Alt went down
	// mid-turn (build 231), and in a drag taken for an Alt pan that ZBrush
	// turned (build 233, just after PureRef Mode was pressed in the ZPlugin
	// menu); from then on the cells turned only on release, snapping.
	const bool idle = gButtonsDown == 0 && !gGesture.active && GetTickCount() - gLastButtonAt > 2000;
	const bool panning = gGesture.active && !gGesture.rotated && (gGesture.mode == kDragMove || gGesture.mode == kDragScale);
	if (!gGesture.active && GetTickCount() - gDragEndAt > 300)
		for (size_t ci = 0; ci < gRotCands.size(); ++ci) {
			if (!gRotCands[ci].panStep)
				continue;
			gRotCands[ci].panStep = false;
			int others = 0, agree = 0;
			for (size_t cj = 0; cj < gRotCands.size(); ++cj) {
				if (cj == ci || gRotCands[cj].panStep)
					continue;
				++others;
				bool same = true;
				for (int i = 0; i < 9; ++i)
					if (fabs(gRotCands[cj].m[i] - gRotCands[ci].m[i]) > 1e-3)
						same = false;
				agree += same;
			}
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu rotation copy %zu changed in a pan or zoom: %d of %d others agree after it, %s\n", GetTickCount(), ci, agree, others,
					others && !agree ? "not the camera, dropped" : "kept");
				fclose(f);
			}
			if (others && !agree) {
				gRotCands.erase(gRotCands.begin() + ci);
				--ci;
			} else {
				// The camera did turn: looked up again as after any turn.
				any = true;
			}
		}
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
				fprintf(f, "%lu rotation copy %zu changed with the mouse buttons up: not the camera, dropped (%zu left)\n", GetTickCount(), ci,
					gRotCands.size() - 1);
				fclose(f);
			}
			gRotCands.erase(gRotCands.begin() + ci);
			--ci;
			continue;
		}
		if (step && panning) {
			c.panStep = true;
			memcpy(c.m, m, sizeof(m));
			continue;
		}
		any = any || step;
		if (step)
			c.changedAt = GetTickCount();
		memcpy(c.m, m, sizeof(m));
		if (sinceSettled && !moved)
			continue;
		if (c.rel < 0)
			continue;
		double canon[9];
		RotApply(c.rel, m, canon);
		// A copy turned to no view counts too, as -1: build 230 left them
		// out, and a stray copy that moved onto Front while the eight that
		// follow the camera turned away from it was the only one counted, so the
		// view never counted as left, nor snapping back to it as a snap (the
		// turns' pans stayed in Classic's offset, the image off the model).
		const int code = std::max(-1, RotCode(canon));
		const size_t at = std::find(codes.begin(), codes.end(), code) - codes.begin();
		if (at == codes.size()) {
			codes.push_back(code);
			counts.push_back(0);
		}
		++counts[at];
	}
	if (changed)
		*changed = any;
	const int most = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
	std::vector<int> top;
	for (size_t i = 0; i < codes.size(); ++i)
		if (counts[i] == most) {
			// Most copies on no view: turned away, whatever the rest say.
			if (codes[i] < 0)
				return {};
			top.push_back(codes[i]);
		}
	return top;
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
	gTurnKnown = gRotLocked;
	if (!gRotLocked) {
		gTurning = false;
		return;
	}
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
	gTurning = !gRotSettled;
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
	// The rotation first: MemFollow holds the image's size while it turns.
	RotFollow();
	MemFollow();
	RefreshUiRects();
	if (gAdjustUndoKey) {
		EnterCriticalSection(&gOverlayLock);
		const LONG key = InterlockedExchange(&gAdjustUndoKey, 0);
		if (gAdjust && AdjustUndo(key == 2))
			gRedrawPending = true;
		LeaveCriticalSection(&gOverlayLock);
	}
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
		st.imgSX != shown.imgSX || st.imgSY != shown.imgSY || st.imgX != shown.imgX || st.imgY != shown.imgY || st.imgRot != shown.imgRot ||
		st.fix != shown.fix || st.alpha != shown.alpha || st.screenX != shown.screenX ||
		st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc;
	if (layoutChanged) {
		if (st.screenX != shown.screenX || st.screenY != shown.screenY || st.pixelsPerDoc != shown.pixelsPerDoc) {
			// The overlay moved (recalibrated): it may be on another monitor.
			gDup.Release();
			gDup.failed = false;
			gViewFixed = gCalVX1 > gCalVX0 && gCalVY1 > gCalVY0;
			gViewX0 = gCalVX0;
			gViewY0 = gCalVY0;
			gViewX1 = gCalVX1;
			gViewY1 = gCalVY1;
			gRowBg.clear();
			gGizmoBottom = 0;
		}
		LARGE_INTEGER b0, b1, bf;
		QueryPerformanceCounter(&b0);
		if (!BuildRefLayer(st)) {
			ShowWindow(wnd, SW_HIDE);
			return false;
		}
		QueryPerformanceCounter(&b1);
		QueryPerformanceFrequency(&bf);
		gStageMs[2] = 1000.0 * (b1.QuadPart - b0.QuadPart) / bf.QuadPart;
		shown = st;
	} else {
		gStageMs[2] = 0;
	}
	if (gKeyState == 1 && !tested && !gUseWgc) {
		tested = true;
		if (!ExclusionWorks(wnd, st)) {
			gKeyState = -1;
			BuildRefLayer(st);  // drawn by the CPU now (the GPU drew it only to composite)
		}
	}
	if (gKeyState != 1) {
		if (layoutChanged)
			Present(wnd, st, gRefLayer, static_cast<BYTE>(255));
		return false;
	}
	const bool aside = UpdatePanelAside(st);
	const bool changed = Capture(st);
	if (changed || layoutChanged || aside) {
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
	ViewRefreshStep(st);
	DepthStep(st);
	DepthMapStep(st);
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
		// Not when the GPU's window is swapped for the CPU's (OverlayThread).
		if (wnd != gOverlayWnd)
			return 0;
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
	// The GPU's window first: no bitmap of its own (DirectComposition shows
	// its frames), layered and transparent so clicks go through to ZBrush.
	gOverlayWnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		wc.lpszClassName, L"JRef", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, nullptr);
	if (gOverlayWnd)
		SetLayeredWindowAttributes(gOverlayWnd, 0, 255, LWA_ALPHA);
	// Capturing ZBrush's window sees what is underneath the overlay, and
	// screenshots still show the overlay. Without it, WDA_EXCLUDEFROMCAPTURE
	// (Windows 10 2004 and later) hides the overlay from screen capture, its
	// own included.
#ifdef RS_TEST_HIDE_CAPTURE
	gKeyState = 1;
#else
	gUseWgc = gOverlayWnd && owner && gWgc.Init(owner);
	const bool gpu = gOverlayWnd && gGpu.Init(gOverlayWnd, gUseWgc ? gWgc.device : nullptr) && gGpu.SelfTest(owner);
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu overlay drawn by the %s%s%s\n", GetTickCount(), gpu ? "GPU" : "CPU (layered window)",
			gpu ? (gUseWgc ? ", on the window capture's device" : ", on its own device") : ": ",
			gpu ? "" : (gGpu.why.empty() ? "no window" : gGpu.why.c_str()));
		fclose(f);
	}
	if (!gpu && gOverlayWnd) {
		// The CPU's layered window instead (UpdateLayeredWindow needs the
		// bitmap the GPU's window has not got).
		const HWND old = gOverlayWnd;
		gOverlayWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			wc.lpszClassName, L"JRef", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, nullptr);
		DestroyWindow(old);
	}
	gKeyState = gUseWgc || (gOverlayWnd && SetWindowDisplayAffinity(gOverlayWnd, 0x11)) ? 1 : 0;
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
// best placement correlates under 0.4. hint (capture pixels, k > 0) is the
// last press's placement: when the canvas has not moved, only the fine
// passes run there (well under a second instead of about 20).
static bool Calibrate(const Rgb& frame, const Rgb& screen, Calibration& out, const Calibration* hint = nullptr)
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
	const int d = 8;
	double best = -1e9, bestCorr = -1, bestK = 1, bestX = 0, bestY = 0;
	// Each scale is tried on its own core, and the best of them taken (in
	// scale order, so the result does not depend on which finishes first):
	// on one core the full search took 10.5 s (build 188).
	struct Best { double s = -1e9, corr = -1, k = 1, x = 0, y = 0; };
	auto bestOf = [&](const std::vector<double>& ks, const auto& tryScale) {
		std::vector<Best> each(ks.size());
		concurrency::parallel_for(size_t(0), ks.size(), [&](size_t i) { tryScale(ks[i], each[i]); });
		best = -1e9;
		for (const Best& b : each)
			if (b.s > best) {
				best = b.s;
				bestCorr = b.corr;
				bestK = b.k;
				bestX = b.x;
				bestY = b.y;
			}
	};

	// Refine at full resolution. The coarse fit is good to about half a large
	// block, the medium one to about a small block.
	struct Pass { const std::vector<Sample>* samples; double scaleRange, scaleStep; int offRange, offStep; };
	auto refine = [&](size_t first) {
		const int halfBlock = static_cast<int>(0.6 * bestK * frame.w / kBigBlocks) + 2 * d;
		const Pass passes[] = {
			{&medium, 0.05, 0.01, halfBlock, 2},
			{&fine, 0.012, 0.002, 2 + static_cast<int>(1.2 * bestK * frame.w / kSmallBlocks), 1},
			{&fine, 0.004, 0.001, 2, 1}};
		for (size_t p = first; p < 3; ++p) {
			const Pass& pass = passes[p];
			const double k0 = bestK, x0 = bestX, y0 = bestY;
			std::vector<double> ks;
			for (double f = 1 - pass.scaleRange; f <= 1 + pass.scaleRange + 1e-9; f += pass.scaleStep)
				ks.push_back(k0 * f);
			bestOf(ks, [&](double k, Best& b) {
				std::vector<int> lums;
				SampleLums(frame, *pass.samples, 0.5 / k, lums);
				// Keep the document centre fixed while trying scales.
				const double cx = floor(x0 + (k0 - k) * frame.w * 0.5), cy = floor(y0 + (k0 - k) * frame.h * 0.5);
				for (int dy = -pass.offRange; dy <= pass.offRange; dy += pass.offStep)
					for (int dx = -pass.offRange; dx <= pass.offRange; dx += pass.offStep) {
						int n = 0;
						const double c = Score(screen, *pass.samples, lums, cx + dx, cy + dy, k, &n);
						const double s = c > -1 ? Significance(c, n) : -1e9;
						if (s > b.s)
							b = {s, c, k, cx + dx, cy + dy};
					}
			});
		}
	};
	auto done = [&]() {
		out.pixelsPerDoc = bestK;
		out.screenX = bestX;
		out.screenY = bestY;
		out.percent = 100.0 * bestCorr;
		return bestCorr >= 0.4;
	};
	if (hint && hint->pixelsPerDoc > 0) {
		bestK = hint->pixelsPerDoc;
		bestX = hint->screenX;
		bestY = hint->screenY;
		refine(1);
		if (bestCorr >= 0.6)
			return done();
		best = -1e9;
		bestCorr = -1;
	}

	// Coarse search on the capture shrunk by d, over scales 0.25 to 4.
	const Rgb small = Shrink(screen, d);
	std::vector<double> ks;
	for (double k = 0.25; k <= 4.0; k *= 1.04)
		ks.push_back(k);
	bestOf(ks, [&](double k, Best& b) {
		const double kd = k / d;  // small-image pixels per document pixel
		std::vector<int> lums;
		SampleLums(frame, coarse, 0.5 / kd, lums);
		const double tw = kd * frame.w, th = kd * frame.h;
		// Step a quarter of a large block, at least one small-image pixel.
		const double step = fmax(1.0, tw / kBigBlocks / 4);
		for (double oy = -th * 0.85; oy <= small.h - th * 0.15; oy += step)
			for (double ox = -tw * 0.85; ox <= small.w - tw * 0.15; ox += step) {
				int n = 0;
				const double c = Score(small, coarse, lums, ox, oy, kd, &n);
				const double s = c > -1 ? Significance(c, n) : -1e9;
				if (s > b.s)
					b = {s, c, k, ox * d, oy * d};
			}
	});
	refine(0);
	return done();
}

// The part of the document the capture shows the pattern in (the viewport;
// ZBrush's interface hides the rest), in overlay pixels: each sampled
// column and row where at least 60% of the samples have the pattern's
// brightness (fitted to how the screen shows it). False if unclear.
static bool PatternViewport(const Rgb& frame, const Rgb& screen, double ox, double oy, double k, int ow, int oh,
	int& x0, int& y0, int& x1, int& y1)
{
	const int step = 4;
	const int cols = ow / step, rows = oh / step;
	if (cols < 8 || rows < 8 || k <= 0)
		return false;
	std::vector<int> fl(static_cast<size_t>(cols) * rows, -1), sl(fl.size(), -1);
	double sf = 0, ss = 0, sff = 0, sfs = 0;
	int n = 0;
	for (int r = 0; r < rows; ++r)
		for (int c = 0; c < cols; ++c) {
			const int fx = static_cast<int>((c * step + step / 2) / k), fy = static_cast<int>((r * step + step / 2) / k);
			const int sx = static_cast<int>(floor(ox + c * step + step / 2)), sy = static_cast<int>(floor(oy + r * step + step / 2));
			if (fx < 0 || fy < 0 || fx >= frame.w || fy >= frame.h || sx < 0 || sy < 0 || sx >= screen.w || sy >= screen.h)
				continue;
			const size_t i = static_cast<size_t>(r) * cols + c;
			// Both averaged over about 3 x 3 screen pixels: the pattern's
			// fine noise is not resolved the same way by ZBrush's display.
			int fc[3];
			FrameColor(frame, fx, fy, std::max(1.0, 1.5 / k), fc);
			const unsigned char fp[3] = {static_cast<unsigned char>(fc[0]), static_cast<unsigned char>(fc[1]), static_cast<unsigned char>(fc[2])};
			fl[i] = Lum(fp);
			int sum = 0, m = 0;
			for (int j = sy - 1; j <= sy + 1; ++j)
				for (int q = sx - 1; q <= sx + 1; ++q)
					if (q >= 0 && j >= 0 && q < screen.w && j < screen.h) {
						sum += Lum(screen.at(q, j));
						++m;
					}
			sl[i] = sum / m;
			sf += fl[i];
			ss += sl[i];
			sff += static_cast<double>(fl[i]) * fl[i];
			sfs += static_cast<double>(fl[i]) * sl[i];
			++n;
		}
	const double var = sff - sf * sf / std::max(n, 1);
	if (n < 64 || var <= 0)
		return false;
	const double a = (sfs - sf * ss / n) / var, b = (ss - a * sf) / n;
	if (a <= 0.2)
		return false;
	std::vector<int> colHit(cols, 0), colAll(cols, 0), rowHit(rows, 0), rowAll(rows, 0);
	for (int r = 0; r < rows; ++r)
		for (int c = 0; c < cols; ++c) {
			const size_t i = static_cast<size_t>(r) * cols + c;
			if (fl[i] < 0)
				continue;
			const bool hit = fabs(sl[i] - (a * fl[i] + b)) < 30 * a + 8;
			colAll[c]++;
			rowAll[r]++;
			colHit[c] += hit;
			rowHit[r] += hit;
		}
	int c0 = -1, c1 = -1, r0 = -1, r1 = -1;
	// 60%: the canvas matches at about 90%, ZBrush's dark interface strips
	// (close to the pattern's dark blocks) at up to about 50%.
	for (int c = 0; c < cols; ++c)
		if (colAll[c] && colHit[c] * 5 >= colAll[c] * 3) { if (c0 < 0) c0 = c; c1 = c; }
	for (int r = 0; r < rows; ++r)
		if (rowAll[r] && rowHit[r] * 5 >= rowAll[r] * 3) { if (r0 < 0) r0 = r; r1 = r; }
	if (c0 < 0 || r0 < 0 || c1 - c0 < cols / 4 || r1 - r0 < rows / 4)
		return false;
	x0 = c0 * step;
	y0 = r0 * step;
	x1 = std::min(ow, (c1 + 1) * step);
	y1 = std::min(oh, (r1 + 1) * step);
	return true;
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
	// The last placement found (screen pixels, kept in calibration.txt), tried
	// first: the canvas rarely moves between presses.
	Calibration hint;
	if (FILE* f = _wfopen((DataFolder() + L"calibration.txt").c_str(), L"rb")) {
		if (fscanf(f, "%lf %lf %lf", &hint.pixelsPerDoc, &hint.screenX, &hint.screenY) != 3)
			hint.pixelsPerDoc = 0;
		fclose(f);
	}
	hint.screenX -= r.left;
	hint.screenY -= r.top;
	Calibration cal;
	const DWORD calAt = GetTickCount();
	const bool found = Calibrate(frame, screen, cal, &hint);
	// The viewport: where the capture shows the pattern.
	gCalVX0 = gCalVY0 = gCalVX1 = gCalVY1 = 0;
	if (found) {
		const int ow = static_cast<int>(frame.w * cal.pixelsPerDoc + 0.5), oh = static_cast<int>(frame.h * cal.pixelsPerDoc + 0.5);
		int x0, y0, x1, y1;
		if (PatternViewport(frame, screen, cal.screenX, cal.screenY, cal.pixelsPerDoc, ow, oh, x0, y0, x1, y1)) {
			gCalVX0 = x0;
			gCalVY0 = y0;
			gCalVX1 = x1;
			gCalVY1 = y1;
		}
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu calibration %s in %lu ms (%s), match %.0f%%, viewport %d %d to %d %d\n", GetTickCount(), found ? "found" : "failed",
			GetTickCount() - calAt, hint.pixelsPerDoc > 0 && cal.pixelsPerDoc > 0 && fabs(cal.pixelsPerDoc / hint.pixelsPerDoc - 1) < 0.02 ? "near the last one" : "searched",
			cal.percent, gCalVX0, gCalVY0, gCalVX1, gCalVY1);
		fclose(f);
	}
	cal.screenX += r.left;
	cal.screenY += r.top;
	if (found)
		if (FILE* f = _wfopen((DataFolder() + L"calibration.txt").c_str(), L"wb")) {
			fprintf(f, "%.6f %.3f %.3f\n", cal.pixelsPerDoc, cal.screenX, cal.screenY);
			fclose(f);
		}
	// The frame's size in document pixels can differ from docW if ZBrush
	// reports the document size differently; scale to the frame.
	cal.pixelsPerDoc *= frame.w / dw;
	gCalibration = cal;
	if (!found)
		return -5;
	// For finding a ZBrush menu that is open now (UpdateMenus).
	gCalScreen = screen;
	gCalFrame = frame;
	gCalScreenX = r.left;
	gCalScreenY = r.top;
	gCalPending = true;

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
	gViewFixed = gCalVX1 > gCalVX0 && gCalVY1 > gCalVY0;
	gViewX0 = gCalVX0;
	gViewY0 = gCalVY0;
	gViewX1 = gCalVX1;
	gViewY1 = gCalVY1;
	gRowBgAt = GetTickCount() - 1000;
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
	double sx, px, py, anchor, r[3];
	const int got = text ? sscanf(text, "%15s %lf %lf %lf %lf %lf %lf %lf", mode, &sx, &px, &py, &anchor, &r[0], &r[1], &r[2]) : 0;
	if (got < 5 || sx <= 0 || anchor <= 0)
		return -1;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	gAnchorScale = anchor;
	if (strcmp(mode, "start") == 0) {
		gMemLocked = false;
		gMemScanned = false;
		// With the rotation ("start sx px py anchor rx ry rz", the view turned
		// off a snap): its search shares the pass (RSRotProbe then narrows).
		if (got == 8) {
			gRotLocked = false;
			CameraScan(sx, px, py, r);
		}
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

// text: "scale x y", the view TransformGet reports now. Returns 1 if the
// camera and rotation found by an earlier Overlay press are still read
// right (the memory scans, a few seconds, are then not needed again),
// else 0.
ZFUNC RSProbeKeep(char* text, double, char*, int, char*, int, char**)
{
	double sx, px, py;
	if (!text || sscanf(text, "%lf %lf %lf", &sx, &px, &py) != 3 || sx <= 0)
		return 0;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	double ms = sx, mx = px, my = py;
	bool ok = gMemLocked && gRotLocked && MemValue(gMemScale, ms) && MemValue(gMemX, mx) && MemValue(gMemY, my) &&
		MemMatch(ms, sx) && MemMatch(mx, px) && MemMatch(my, py);
	if (ok) {
		int readable = 0;
		for (const RotCand& c : gRotCands) {
			double m[9];
			if (RotRead(c, m))
				++readable;
		}
		ok = readable > 0;
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu earlier camera and rotation %s\n", GetTickCount(), ok ? "still read right: scans skipped" : "not usable: scanning");
		fclose(f);
	}
	LeaveCriticalSection(&gOverlayLock);
	return ok ? 1 : 0;
}

// Saves what the overlay last captured (snap_capture.png, the canvas as
// ZBrush drew it) and what it showed (snap_overlay.png) next to the DLL.
// Returns 1, or 0 if the overlay has not drawn anything yet.
ZFUNC RSSnapshot(char*, double, char*, int, char*, int, char**)
{
	if (!gOverlayLockReady || !gCapture.bits || !gOutput.bits)
		return 0;
	StartGdiplus();
	if (GpuComposes()) {
		// The GPU copies its next frame into gOutput.
		gGpu.snapDone = false;
		gGpu.snapWanted = true;
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
		for (int i = 0; i < 100 && !gGpu.snapDone; ++i)
			Sleep(10);
	}
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
	gOverlay.fix = 1;
	LeaveCriticalSection(&gOverlayLock);
	return had ? 1 : 0;
}

// text: "name value" sets a display setting and returns it: "mode" (0
// Classic, 1 PureRef), "left" (PureRef panel on the left), "multi" (two
// views in the panel), "auto3d" (3D Auto Track), "a" and "b" (the two views,
// slots 0-5), "back" (the
// grey shown around the panel), "gamma" and "sat" (the reference's gamma
// and saturation in percent), "modelop" (Model Opacity in percent), "zoomout"
// (Multiview's Zoom Out in percent, 100-1000). The value "-1" toggles mode, left and multi
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
		strcmp(name, "auto3d") == 0 ? &gAuto3D : strcmp(name, "autopersp") == 0 ? &gAutoPersp : strcmp(name, "back") == 0 ? &gBackGrey : strcmp(name, "gamma") == 0 ? &gGamma : strcmp(name, "sat") == 0 ? &gSat : strcmp(name, "modelop") == 0 ? &gModelOpacity : strcmp(name, "zoomout") == 0 ? &gCellZoomOut : nullptr;
	float result = -1;
	if (field) {
		const bool view = field == &gMultiA || field == &gMultiB;
		const bool level = field == &gBackGrey || field == &gGamma || field == &gSat || field == &gModelOpacity || field == &gCellZoomOut;
		if (value == -1 && !level)
			*field = view ? (*field + 1) % 6 : !*field;
		else if (value >= 0)
			*field = view ? value % 6 : field == &gBackGrey ? std::min(value, 255) :
				field == &gGamma ? std::max(20, std::min(value, 300)) : field == &gSat ? std::min(value, 200) :
				field == &gModelOpacity ? std::min(value, 100) : field == &gCellZoomOut ? std::max(100, std::min(value, 1000)) : value != 0;
		if (field == &gMulti && gMulti && value != -2) {
			// Multiview turned on with a view without an image above or below
			// (or the same one twice): views that have one go there instead,
			// the showing view's first.
			LoadViews();
			std::vector<int> have;
			if (gActiveView >= 0 && gActiveView < kDefaultView && gViews[gActiveView].used)
				have.push_back(gActiveView);
			for (int s = 0; s < kDefaultView; ++s)
				if (gViews[s].used && s != gActiveView)
					have.push_back(s);
			if (!gViews[gMultiA].used)
				for (int s : have)
					if (s != gMultiB || !gViews[gMultiB].used) {
						gMultiA = s;
						break;
					}
			if (!gViews[gMultiB].used || gMultiB == gMultiA)
				for (int s : have)
					if (s != gMultiA) {
						gMultiB = s;
						break;
					}
		}
		if (value != -2) {
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu display %s (upper %d lower %d)\n", GetTickCount(), text, gMultiA, gMultiB);
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
// place for the showing view), "2" only checks whether it could go on, "3"
// (Reset) turns it off and puts every view's image back to fitted and
// centred. Returns 1, 0 if no image is showing, -1 if the overlay is not on,
// -2 in PureRef.
ZFUNC RSAdjust(char* text, double, char*, int, char*, int, char**)
{
	const int req = text ? atoi(text) : 0;
	const bool on = req == 1 || req == 2;
	if (req == 3) {
		EnsureLock();
		EnterCriticalSection(&gOverlayLock);
		LoadViews();
		gAdjust = false;
		gAdjustDrag = 0;
		for (int s = 0; s < kViewSlots; ++s) {
			gViews[s].imgSX = gViews[s].imgSY = 1;
			gViews[s].imgX = gViews[s].imgY = gViews[s].imgRot = 0;
		}
		gOverlay.imgSX = gOverlay.imgSY = 1;
		gOverlay.imgX = gOverlay.imgY = gOverlay.imgRot = 0;
		gAdjustUndo.clear();
		gAdjustRedo.clear();
		SaveViews();
		gRedrawPending = true;
		LeaveCriticalSection(&gOverlayLock);
		if (gOverlayWnd)
			PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
		ViewLog("image size and place reset", -1);
		return 1;
	}
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

// The showing view's image, typed into the plugin's sliders: "width N" and
// "height N" (percent of the image fitted to the document), "x N" and "y N"
// (its centre's shift, anchored document pixels, as project.jref keeps it),
// "turn N" (degrees, clockwise), each kept as a step Ctrl+Z takes back; the
// name alone returns the value (-1 for width and height when no view's
// image is showing). "undo" and "redo" step back and on. Returns the value,
// 1 for undo and redo done, or 0.
ZFUNC RSImage(char* text, double, char*, int, char*, int, char**)
{
	char name[16] = {0};
	double value = 0;
	const int n = text ? sscanf(text, "%15s %lf", name, &value) : 0;
	if (n < 1)
		return 0;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	float result = -1;
	const bool showing = gActiveView >= 0 && gActiveView < kViewSlots && gViews[gActiveView].used;
	double* field = strcmp(name, "width") == 0 ? &gOverlay.imgSX : strcmp(name, "height") == 0 ? &gOverlay.imgSY :
		strcmp(name, "turn") == 0 ? &gOverlay.imgRot : strcmp(name, "x") == 0 ? &gOverlay.imgX : strcmp(name, "y") == 0 ? &gOverlay.imgY : nullptr;
	const bool turn = field == &gOverlay.imgRot, shift = field == &gOverlay.imgX || field == &gOverlay.imgY;
	const double unit = turn || shift ? 1 : 100;
	// No view's image showing (not snapped to one since the overlay started):
	// the sliders are the first view with an image's, Front's when it has
	// one, and a typed value goes into it. Build 202 left a typed value
	// nowhere while the slider kept it (a user's Width 129.4734 showed, the
	// image stayed at 100).
	int idle = -1;
	for (int i = 0; !showing && idle < 0 && i < kDefaultView; ++i)
		if (gViews[i].used)
			idle = i;
	if (!showing && idle >= 0 && field) {
		View& v = gViews[idle];
		double* vf = field == &gOverlay.imgSX ? &v.imgSX : field == &gOverlay.imgSY ? &v.imgSY : turn ? &v.imgRot : field == &gOverlay.imgX ? &v.imgX : &v.imgY;
		if (n == 2) {
			const double was = *vf * unit;
			*vf = shift ? std::max(-100000.0, std::min(100000.0, value)) : turn ? value - 360 * floor((value + 180) / 360) : std::max(0.1, std::min(10000.0, value)) / unit;
			SaveViews();
			if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
				fprintf(f, "%lu image typed into %s: %g (was %g); view %d (not showing) now width %.4f height %.4f x %.2f y %.2f turn %.3f\n", GetTickCount(), name,
					value, was, idle, 100 * v.imgSX, 100 * v.imgSY, v.imgX, v.imgY, v.imgRot);
				fclose(f);
			}
		}
		result = static_cast<float>(*vf * unit);
	} else if (!showing) {
		result = field && !shift ? -1.0f : 0.0f;
	} else if (strcmp(name, "undo") == 0 || strcmp(name, "redo") == 0) {
		result = AdjustUndo(name[0] == 'r') ? 1.0f : 0.0f;
		gRedrawPending = true;
	} else if (field && n == 2) {
		const AdjustState before = AdjustNow();
		*field = shift ? std::max(-100000.0, std::min(100000.0, value)) : turn ? value - 360 * floor((value + 180) / 360) : std::max(0.1, std::min(10000.0, value)) / unit;
		// Each typed value in the log, with the input it came from (a user
		// found the Front image's Height at 20.87, the X they had typed, and
		// nothing showed which input had taken it).
		if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
			const AdjustState a = AdjustNow();
			fprintf(f, "%lu image typed into %s: %g (was %g); view %d now width %.4f height %.4f x %.2f y %.2f turn %.3f\n", GetTickCount(), name, value,
				shift ? (field == &gOverlay.imgX ? before.x : before.y) : turn ? before.rot : 100 * (field == &gOverlay.imgSX ? before.sx : before.sy),
				gActiveView, 100 * a.sx, 100 * a.sy, a.x, a.y, a.rot);
			fclose(f);
		}
		if (!(AdjustNow() == before))
			AdjustRemember(before);
		SaveAdjustToView();
		gRedrawPending = true;
		result = static_cast<float>(*field * unit);
	} else if (field) {
		result = static_cast<float>(*field * unit);
	}
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
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
// thumbnails) does not count; a project without a .jref keeps the references
// open at the time.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION gZprLock;
static bool gZprReady = false;
struct ZprFile {
	HANDLE handle;
	std::wstring path;
	bool write;
	LONGLONG readEnd;  // furthest byte read (reads at an offset do not move the file position)
	bool mapped;       // read through a file mapping
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
		// A project without a link starts with no references (the first image
		// loaded makes it a JRef project, and saving links it). Builds 90-186
		// kept the references open, and another project showed the last one's
		// images. One saved before JRef watched it is reopened with Project >
		// Open Project.
		if (_wcsicmp(dir.c_str(), gProjectDir.c_str()) != 0)
			SwitchProject(dir);
	}
	LeaveCriticalSection(&gOverlayLock);
	if (gOverlayWnd)
		PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
}

static void TrackZprAs(HANDLE h, const std::wstring& path, bool write)
{
	if (h == INVALID_HANDLE_VALUE || !h || !IsZpr(path))
		return;
	wchar_t full[MAX_PATH] = {0};
	const std::wstring p = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) ? std::wstring(full) : path;
	EnterCriticalSection(&gZprLock);
	gZprFiles.push_back({h, p, write, 0, false});
	gZprCount = static_cast<LONG>(gZprFiles.size());
	LeaveCriticalSection(&gZprLock);
	ProjectLog(write ? "zpr opened for writing" : "zpr opened for reading", p);
}

static void TrackZpr(HANDLE h, const std::wstring& path, DWORD access, DWORD disposition)
{
	TrackZprAs(h, path, (access & (GENERIC_WRITE | FILE_WRITE_DATA)) != 0 || disposition == CREATE_ALWAYS || disposition == CREATE_NEW);
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

// A handle is about to be closed: true (with what to do after the close in
// done and full) if it is a tracked .zpr.
static bool ZprClosing(HANDLE h, ZprFile& done, bool& full)
{
	bool found = false;
	full = false;
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
		if (GetFileSizeEx(h, &size) && SetFilePointerEx(h, zero, &pos, FILE_CURRENT) && size.QuadPart > 0) {
			const LONGLONG end = std::max(pos.QuadPart, done.readEnd);
			full = end >= size.QuadPart * 9 / 10 || (done.mapped && done.readEnd == 0);
		}
		char what[128];
		snprintf(what, sizeof(what), "zpr closed after reading %lld (furthest %lld%s) of %lld bytes:", static_cast<long long>(pos.QuadPart),
			static_cast<long long>(done.readEnd), done.mapped ? ", mapped" : "", static_cast<long long>(size.QuadPart));
		ProjectLog(what, done.path);
	}
	return found;
}

static BOOL WINAPI HookCloseHandle(HANDLE h)
{
	if (gZprCount == 0)
		return gRealCloseHandle(h);
	ZprFile done;
	bool full = false;
	const bool found = ZprClosing(h, done, full);
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

// The other ways a save can put a finished file in place of the project
// (saving over the open project, rather than Save As to a new name).
static void ZprWrittenTo(const char* how, LPCWSTR to)
{
	if (!to || !IsZpr(to))
		return;
	wchar_t full[MAX_PATH] = {0};
	ProjectLog(how, to);
	ZprDone(GetFullPathNameW(to, MAX_PATH, full, nullptr) ? std::wstring(full) : std::wstring(to), true);
}

static void ZprWrittenToA(const char* how, LPCSTR to)
{
	if (!to)
		return;
	wchar_t w[MAX_PATH] = {0};
	MultiByteToWideChar(CP_ACP, 0, to, -1, w, MAX_PATH);
	ZprWrittenTo(how, w);
}

typedef BOOL(WINAPI* MoveFileExAFn)(LPCSTR, LPCSTR, DWORD);
typedef BOOL(WINAPI* MoveFileProgressWFn)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD);
typedef BOOL(WINAPI* ReplaceFileWFn)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
typedef BOOL(WINAPI* ReplaceFileAFn)(LPCSTR, LPCSTR, LPCSTR, DWORD, LPVOID, LPVOID);
typedef BOOL(WINAPI* CopyFileWFn)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL(WINAPI* CopyFileExWFn)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
typedef BOOL(WINAPI* SetFileInfoFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
static MoveFileExAFn gRealMoveFileExA = nullptr;
static MoveFileProgressWFn gRealMoveFileProgressW = nullptr;
static ReplaceFileWFn gRealReplaceFileW = nullptr;
static ReplaceFileAFn gRealReplaceFileA = nullptr;
static CopyFileWFn gRealCopyFileW = nullptr;
static CopyFileExWFn gRealCopyFileExW = nullptr;
static SetFileInfoFn gRealSetFileInfo = nullptr;

static BOOL WINAPI HookMoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
	const BOOL ok = gRealMoveFileExA(from, to, flags);
	if (ok)
		ZprWrittenToA("zpr written by renaming (A) to", to);
	return ok;
}

static BOOL WINAPI HookMoveFileA(LPCSTR from, LPCSTR to)
{
	return HookMoveFileExA(from, to, MOVEFILE_COPY_ALLOWED);
}

static BOOL WINAPI HookMoveFileProgressW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, DWORD flags)
{
	const BOOL ok = gRealMoveFileProgressW(from, to, progress, data, flags);
	if (ok)
		ZprWrittenTo("zpr written by moving to", to);
	return ok;
}

static BOOL WINAPI HookReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID ex, LPVOID res)
{
	const BOOL ok = gRealReplaceFileW(replaced, replacement, backup, flags, ex, res);
	if (ok)
		ZprWrittenTo("zpr written by replacing", replaced);
	return ok;
}

static BOOL WINAPI HookReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags, LPVOID ex, LPVOID res)
{
	const BOOL ok = gRealReplaceFileA(replaced, replacement, backup, flags, ex, res);
	if (ok)
		ZprWrittenToA("zpr written by replacing (A)", replaced);
	return ok;
}

static BOOL WINAPI HookCopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
	const BOOL ok = gRealCopyFileW(from, to, failIfExists);
	if (ok)
		ZprWrittenTo("zpr written by copying to", to);
	return ok;
}

static BOOL WINAPI HookCopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel, DWORD flags)
{
	const BOOL ok = gRealCopyFileExW(from, to, progress, data, cancel, flags);
	if (ok)
		ZprWrittenTo("zpr written by copying (ex) to", to);
	return ok;
}

// A rename through the file's handle (FileRenameInfo, FileRenameInfoEx).
static BOOL WINAPI HookSetFileInfo(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
	const BOOL ok = gRealSetFileInfo(h, cls, info, size);
	if (ok && info && (cls == FileRenameInfo || static_cast<int>(cls) == 22) && size >= sizeof(FILE_RENAME_INFO)) {
		const FILE_RENAME_INFO* r = static_cast<const FILE_RENAME_INFO*>(info);
		const std::wstring to(r->FileName, r->FileNameLength / sizeof(wchar_t));
		ZprWrittenTo("zpr written by renaming its handle to", to.c_str());
	}
	return ok;
}

// ---------------------------------------------------------------------------
// The same, one level down: ntdll's NtCreateFile, NtOpenFile, NtReadFile,
// NtCreateSection, NtSetInformationFile and NtClose, which every way of
// opening, reading, renaming and closing a file ends in (whichever module or
// runtime ZBrush uses). Each is a system call stub, "mov r10, rcx; mov eax,
// n" (8 bytes) then the call; its first 5 bytes become a jump to the hook (by
// way of a relay page near ntdll), and the hook calls the stub through a copy
// of those 8 bytes that jumps back to the rest.
// ---------------------------------------------------------------------------
typedef NTSTATUS(NTAPI* NtCreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
	ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI* NtOpenFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
typedef NTSTATUS(NTAPI* NtReadFileFn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
typedef NTSTATUS(NTAPI* NtCreateSectionFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef NTSTATUS(NTAPI* NtSetInformationFileFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
typedef NTSTATUS(NTAPI* NtCloseFn)(HANDLE);
static NtCreateFileFn gNtCreateFile = nullptr;
static NtOpenFileFn gNtOpenFile = nullptr;
static NtReadFileFn gNtReadFile = nullptr;
static NtCreateSectionFn gNtCreateSection = nullptr;
static NtSetInformationFileFn gNtSetInformationFile = nullptr;
static NtCloseFn gNtClose = nullptr;

// The file a call names: "\??\C:\..." as a Win32 path, joined to its root
// directory if it has one. Empty unless it is a .zpr.
static std::wstring NtZprPath(POBJECT_ATTRIBUTES attr)
{
	if (!attr || !attr->ObjectName || !attr->ObjectName->Buffer)
		return L"";
	std::wstring name(attr->ObjectName->Buffer, attr->ObjectName->Length / sizeof(wchar_t));
	if (!IsZpr(name))
		return L"";
	if (name.compare(0, 8, L"\\??\\UNC\\") == 0)
		name = L"\\\\" + name.substr(8);
	else if (name.compare(0, 4, L"\\??\\") == 0 || name.compare(0, 4, L"\\\\?\\") == 0)
		name = name.substr(4);
	if (attr->RootDirectory) {
		wchar_t root[MAX_PATH] = {0};
		const DWORD n = GetFinalPathNameByHandleW(attr->RootDirectory, root, MAX_PATH, FILE_NAME_NORMALIZED);
		if (n && n < MAX_PATH) {
			std::wstring r = root;
			if (r.compare(0, 4, L"\\\\?\\") == 0)
				r = r.substr(4);
			if (!r.empty() && r.back() != L'\\')
				r += L'\\';
			name = r + name;
		}
	}
	return name;
}

static bool NtWrites(ACCESS_MASK access, ULONG disposition)
{
	// FILE_SUPERSEDE 0, FILE_CREATE 2, FILE_OVERWRITE 4, FILE_OVERWRITE_IF 5.
	return (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 || disposition == 0 || disposition == 2 ||
		disposition == 4 || disposition == 5;
}

static NTSTATUS NTAPI HookNtCreateFile(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
	PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLen)
{
	const NTSTATUS st = gNtCreateFile(h, access, attr, io, alloc, attrs, share, disposition, options, ea, eaLen);
	if (st >= 0 && h) {
		const std::wstring path = NtZprPath(attr);
		if (!path.empty())
			TrackZprAs(*h, path, NtWrites(access, disposition));
	}
	return st;
}

static NTSTATUS NTAPI HookNtOpenFile(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io, ULONG share,
	ULONG options)
{
	const NTSTATUS st = gNtOpenFile(h, access, attr, io, share, options);
	if (st >= 0 && h) {
		const std::wstring path = NtZprPath(attr);
		if (!path.empty())
			TrackZprAs(*h, path, NtWrites(access, 1));
	}
	return st;
}

static NTSTATUS NTAPI HookNtReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, PVOID buf, ULONG len,
	PLARGE_INTEGER offset, PULONG key)
{
	const NTSTATUS st = gNtReadFile(h, ev, apc, ctx, io, buf, len, offset, key);
	if (gZprCount && st >= 0 && offset && offset->QuadPart >= 0) {
		EnterCriticalSection(&gZprLock);
		for (ZprFile& z : gZprFiles)
			if (z.handle == h)
				z.readEnd = std::max(z.readEnd, offset->QuadPart + static_cast<LONGLONG>(len));
		LeaveCriticalSection(&gZprLock);
	}
	return st;
}

static NTSTATUS NTAPI HookNtCreateSection(PHANDLE section, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PLARGE_INTEGER size,
	ULONG protect, ULONG attrs, HANDLE file)
{
	const NTSTATUS st = gNtCreateSection(section, access, attr, size, protect, attrs, file);
	if (gZprCount && st >= 0 && file) {
		EnterCriticalSection(&gZprLock);
		for (ZprFile& z : gZprFiles)
			if (z.handle == file)
				z.mapped = true;
		LeaveCriticalSection(&gZprLock);
	}
	return st;
}

static NTSTATUS NTAPI HookNtSetInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls)
{
	const NTSTATUS st = gNtSetInformationFile(h, io, info, len, cls);
	// FileRenameInformation 10, FileRenameInformationEx 65; the same layout
	// as FILE_RENAME_INFO.
	if (st >= 0 && info && (cls == 10 || cls == 65) && len >= sizeof(FILE_RENAME_INFO)) {
		const FILE_RENAME_INFO* r = static_cast<const FILE_RENAME_INFO*>(info);
		UNICODE_STRING name = {static_cast<USHORT>(r->FileNameLength), static_cast<USHORT>(r->FileNameLength),
			const_cast<PWSTR>(r->FileName)};
		OBJECT_ATTRIBUTES attr = {};
		attr.Length = sizeof(attr);
		attr.RootDirectory = r->RootDirectory;
		attr.ObjectName = &name;
		const std::wstring to = NtZprPath(&attr);
		if (!to.empty())
			ZprWrittenTo("zpr written by renaming to", to.c_str());
	}
	return st;
}

static NTSTATUS NTAPI HookNtClose(HANDLE h)
{
	if (gZprCount == 0)
		return gNtClose(h);
	ZprFile done;
	bool full = false;
	const bool found = ZprClosing(h, done, full);
	const NTSTATUS st = gNtClose(h);
	if (found && (done.write || full))
		ZprDone(done.path, done.write);
	return st;
}

// A page within 2 GB of ntdll, for the relays and the copied stub starts.
static BYTE* NearPage(BYTE* target)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	const ULONG_PTR gran = si.dwAllocationGranularity;
	ULONG_PTR base = reinterpret_cast<ULONG_PTR>(target) & ~(gran - 1);
	for (ULONG_PTR d = gran; d < 0x70000000ull; d += gran) {
		for (int sign = -1; sign <= 1; sign += 2) {
			const ULONG_PTR at = sign < 0 ? base - d : base + d;
			if (sign < 0 && d > base)
				continue;
			if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
				return static_cast<BYTE*>(p);
		}
	}
	return nullptr;
}

static void PutJump(BYTE* at, const void* to)
{
	// jmp [rip+0]; dq to
	at[0] = 0xFF;
	at[1] = 0x25;
	memset(at + 2, 0, 4);
	memcpy(at + 6, &to, 8);
}

// Hooks ntdll's stub name with hook; real gets a way to call the stub.
// False (nothing changed) if the stub does not start as expected.
static bool HookStub(HMODULE ntdll, const char* name, const void* hook, void** real, BYTE*& page)
{
	BYTE* fn = reinterpret_cast<BYTE*>(GetProcAddress(ntdll, name));
	if (!fn || !page)
		return false;
	if (!(fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8))
		return false;
	BYTE* copy = page;    // the first 8 bytes, then a jump to the rest
	BYTE* relay = page + 32;  // the jump to the hook
	const LONGLONG rel = relay - (fn + 5);
	if (rel > 0x7FFFFFFFll || rel < -0x80000000ll)
		return false;
	memcpy(copy, fn, 8);
	PutJump(copy + 8, fn + 8);
	PutJump(relay, hook);
	DWORD old;
	if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old))
		return false;
	*real = copy;
	BYTE jmp[5] = {0xE9};
	const int32_t r32 = static_cast<int32_t>(rel);
	memcpy(jmp + 1, &r32, 4);
	memcpy(fn, jmp, 5);
	VirtualProtect(fn, 8, old, &old);
	FlushInstructionCache(GetCurrentProcess(), fn, 8);
	page += 64;
	return true;
}

// Installs all six or none (any stub not as expected); true if installed.
static bool InstallNtHooks()
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return false;
	const char* names[6] = {"NtCreateFile", "NtOpenFile", "NtReadFile", "NtCreateSection", "NtSetInformationFile", "NtClose"};
	for (const char* n : names) {
		const BYTE* fn = reinterpret_cast<const BYTE*>(GetProcAddress(ntdll, n));
		if (!fn || !(fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8)) {
			ProjectLog("ntdll stub not as expected, watching imports instead:", Widen(n));
			return false;
		}
	}
	BYTE* page = NearPage(reinterpret_cast<BYTE*>(GetProcAddress(ntdll, "NtClose")));
	if (!page)
		return false;
	// NtClose last: the others must be in place before closes are watched.
	bool ok = HookStub(ntdll, "NtCreateFile", reinterpret_cast<const void*>(&HookNtCreateFile), reinterpret_cast<void**>(&gNtCreateFile), page);
	ok = ok && HookStub(ntdll, "NtOpenFile", reinterpret_cast<const void*>(&HookNtOpenFile), reinterpret_cast<void**>(&gNtOpenFile), page);
	ok = ok && HookStub(ntdll, "NtReadFile", reinterpret_cast<const void*>(&HookNtReadFile), reinterpret_cast<void**>(&gNtReadFile), page);
	ok = ok && HookStub(ntdll, "NtCreateSection", reinterpret_cast<const void*>(&HookNtCreateSection),
		reinterpret_cast<void**>(&gNtCreateSection), page);
	ok = ok && HookStub(ntdll, "NtSetInformationFile", reinterpret_cast<const void*>(&HookNtSetInformationFile),
		reinterpret_cast<void**>(&gNtSetInformationFile), page);
	ok = ok && HookStub(ntdll, "NtClose", reinterpret_cast<const void*>(&HookNtClose), reinterpret_cast<void**>(&gNtClose), page);
	return ok;
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
	// Only the first JRef DLL in ZBrush watches its files: a second one (the
	// installed copy and this folder's both loaded, as when an update
	// installs a copy ZBrush is still about to load) would hook the hooks and
	// see every save twice. The mutex lives as long as ZBrush.
	wchar_t name[64];
	swprintf(name, 64, L"Local\\JRefFileHooks-%lu", GetCurrentProcessId());
	CreateMutexW(nullptr, FALSE, name);
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		ProjectLog("another JRef DLL already watches ZBrush's project files; this one does not:", DllFolder());
		return;
	}
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll");
	gRealCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(k32, "CreateFileW"));
	gRealCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(k32, "CreateFileA"));
	gRealCloseHandle = reinterpret_cast<CloseHandleFn>(GetProcAddress(k32, "CloseHandle"));
	gRealMoveFileExW = reinterpret_cast<MoveFileExWFn>(GetProcAddress(k32, "MoveFileExW"));
	if (!gRealCreateFileW || !gRealCreateFileA || !gRealCloseHandle || !gRealMoveFileExW)
		return;
	if (InstallNtHooks()) {
		HWND zb = nullptr;
		EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&zb));
		wchar_t title[512] = {0};
		if (zb)
			GetWindowTextW(zb, title, 512);
		ProjectLog("watching ZBrush's project files (ntdll), window:", title);
		return;
	}
	gRealMoveFileExA = reinterpret_cast<MoveFileExAFn>(GetProcAddress(k32, "MoveFileExA"));
	gRealMoveFileProgressW = reinterpret_cast<MoveFileProgressWFn>(GetProcAddress(k32, "MoveFileWithProgressW"));
	gRealReplaceFileW = reinterpret_cast<ReplaceFileWFn>(GetProcAddress(k32, "ReplaceFileW"));
	gRealReplaceFileA = reinterpret_cast<ReplaceFileAFn>(GetProcAddress(k32, "ReplaceFileA"));
	gRealCopyFileW = reinterpret_cast<CopyFileWFn>(GetProcAddress(k32, "CopyFileW"));
	gRealCopyFileExW = reinterpret_cast<CopyFileExWFn>(GetProcAddress(k32, "CopyFileExW"));
	gRealSetFileInfo = reinterpret_cast<SetFileInfoFn>(GetProcAddress(k32, "SetFileInformationByHandle"));
	auto at = [](HMODULE m, const char* name) { return m ? reinterpret_cast<const void*>(GetProcAddress(m, name)) : nullptr; };
	// Each function as kernel32 and kernelbase export it, and its hook (one
	// whose real function is missing is left alone).
	const struct { const char* name; const void* hook; bool real; } hooks[] = {
		{"CreateFileW", reinterpret_cast<const void*>(&HookCreateFileW), true},
		{"CreateFileA", reinterpret_cast<const void*>(&HookCreateFileA), true},
		{"CloseHandle", reinterpret_cast<const void*>(&HookCloseHandle), true},
		{"MoveFileExW", reinterpret_cast<const void*>(&HookMoveFileExW), true},
		{"MoveFileW", reinterpret_cast<const void*>(&HookMoveFileW), true},
		{"MoveFileExA", reinterpret_cast<const void*>(&HookMoveFileExA), gRealMoveFileExA != nullptr},
		{"MoveFileA", reinterpret_cast<const void*>(&HookMoveFileA), gRealMoveFileExA != nullptr},
		{"MoveFileWithProgressW", reinterpret_cast<const void*>(&HookMoveFileProgressW), gRealMoveFileProgressW != nullptr},
		{"ReplaceFileW", reinterpret_cast<const void*>(&HookReplaceFileW), gRealReplaceFileW != nullptr},
		{"ReplaceFileA", reinterpret_cast<const void*>(&HookReplaceFileA), gRealReplaceFileA != nullptr},
		{"CopyFileW", reinterpret_cast<const void*>(&HookCopyFileW), gRealCopyFileW != nullptr},
		{"CopyFileExW", reinterpret_cast<const void*>(&HookCopyFileExW), gRealCopyFileExW != nullptr},
		{"SetFileInformationByHandle", reinterpret_cast<const void*>(&HookSetFileInfo), gRealSetFileInfo != nullptr},
	};
	const void* from[32] = {nullptr};
	const void* to[32] = {nullptr};
	int count = 0;
	for (const auto& h : hooks) {
		if (!h.real)
			continue;
		from[count] = at(k32, h.name);
		to[count++] = h.hook;
		from[count] = at(kb, h.name);
		to[count++] = h.hook;
	}
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
		PatchImports(me.hModule, from, to, count);
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
	if (!gCurrentZpr.empty())
		WriteLink(gCurrentZpr);
	w.imgSX = w.imgSY = 1;
	w.imgX = w.imgY = w.imgRot = 0;
	// Shown (and the model's outline recorded) now only when snapped to that
	// view; otherwise kept until it is snapped to.
	const std::vector<int> codes = CurrentRotCodes();
	const bool here = gRotLocked && AtView(codes, gPrepSlot);
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu image for slot %d: rotation locked %d, %zu copies, snapped codes", GetTickCount(), gPrepSlot, gRotLocked ? 1 : 0,
			gRotCands.size());
		for (int c : codes)
			fprintf(f, " %d", c);
		fprintf(f, " (slot's code %d): %s\n", gPrepSlot < 6 ? kViewCodes[gPrepSlot] : -1, here ? "shown now" : "kept for later");
		fclose(f);
	}
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
// ---------------------------------------------------------------------------
// Status: a small message box over ZBrush's window, shown from pressing
// Overlay until it is done. A ZBrush note only stays up while the script
// waits on it, and the script is busy in the DLL for most of the press, so
// the box is the DLL's own window, on its own thread (ZBrush does not pump
// messages while a script runs). It is click-through and left out of screen
// captures, so the canvas search and the overlay never see it.
// ---------------------------------------------------------------------------
static SRWLOCK gStatusLock = SRWLOCK_INIT;
static std::wstring gStatusText;
static HWND gStatusWnd = nullptr;
static const UINT WM_RS_STATUS = WM_APP + 41;

static void PlaceStatus(HWND wnd)
{
	AcquireSRWLockShared(&gStatusLock);
	const std::wstring text = gStatusText;
	ReleaseSRWLockShared(&gStatusLock);
	if (text.empty()) {
		ShowWindow(wnd, SW_HIDE);
		return;
	}
	HDC dc = GetDC(wnd);
	HFONT font = static_cast<HFONT>(GetPropW(wnd, L"font"));
	HGDIOBJ old = SelectObject(dc, font);
	RECT tr = {0, 0, 900, 0};
	DrawTextW(dc, text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	SelectObject(dc, old);
	ReleaseDC(wnd, dc);
	const int w = tr.right + 48, h = tr.bottom + 28;
	HWND zb = nullptr;
	EnumWindows(FindZBrushWindow, reinterpret_cast<LPARAM>(&zb));
	RECT zr;
	if (!zb || !GetWindowRect(zb, &zr))
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &zr, 0);
	const int x = (zr.left + zr.right - w) / 2, y = zr.top + (zr.bottom - zr.top) / 5;
	SetWindowPos(wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(wnd, nullptr, TRUE);
}

static LRESULT CALLBACK StatusProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_RS_STATUS:
		// wp 1: a short notice, gone by itself after 3.5 s.
		KillTimer(wnd, 1);
		if (wp)
			SetTimer(wnd, 1, 3500, nullptr);
		PlaceStatus(wnd);
		return 0;
	case WM_TIMER:
		KillTimer(wnd, 1);
		AcquireSRWLockExclusive(&gStatusLock);
		gStatusText.clear();
		ReleaseSRWLockExclusive(&gStatusLock);
		PlaceStatus(wnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(wnd, &ps);
		RECT r;
		GetClientRect(wnd, &r);
		HBRUSH bg = CreateSolidBrush(RGB(36, 36, 36)), bar = CreateSolidBrush(RGB(232, 128, 32));
		FillRect(dc, &r, bg);
		RECT b = {0, 0, 5, r.bottom};
		FillRect(dc, &b, bar);
		DeleteObject(bg);
		DeleteObject(bar);
		AcquireSRWLockShared(&gStatusLock);
		const std::wstring text = gStatusText;
		ReleaseSRWLockShared(&gStatusLock);
		HGDIOBJ old = SelectObject(dc, static_cast<HFONT>(GetPropW(wnd, L"font")));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(235, 235, 235));
		RECT tr = {24, 14, r.right - 24, r.bottom - 14};
		DrawTextW(dc, text.c_str(), -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
		SelectObject(dc, old);
		EndPaint(wnd, &ps);
		return 0;
	}
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

static DWORD WINAPI StatusThread(LPVOID ready)
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = StatusProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"JRefStatus";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, L"JRefStatus",
		L"JRef", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
	if (wnd) {
		SetLayeredWindowAttributes(wnd, 0, 240, LWA_ALPHA);
		SetWindowDisplayAffinity(wnd, 0x11);  // WDA_EXCLUDEFROMCAPTURE
		HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
		SetPropW(wnd, L"font", font);
	}
	gStatusWnd = wnd;
	SetEvent(static_cast<HANDLE>(ready));
	if (!wnd)
		return 0;
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	return 0;
}

// text: the message to show over ZBrush ("\n" for a new line), or empty to
// hide it. Starting with "~", a short notice that goes by itself (the
// plugin shows no popups). Returns 1, or 0 if the box could not be made.
ZFUNC RSStatus(char* text, double, char*, int, char*, int, char**)
{
	std::string t = text ? text : "";
	const bool brief = !t.empty() && t[0] == '~';
	if (brief)
		t.erase(0, 1);
	if (FILE* f = brief ? _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab") : nullptr) {
		fprintf(f, "%lu notice: %s\n", GetTickCount(), t.c_str());
		fclose(f);
	}
	AcquireSRWLockExclusive(&gStatusLock);
	gStatusText = Widen(t.c_str());
	ReleaseSRWLockExclusive(&gStatusLock);
	if (!gStatusWnd) {
		if (t.empty())
			return 1;
		HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		HANDLE th = CreateThread(nullptr, 0, StatusThread, ready, 0, nullptr);
		if (th) {
			WaitForSingleObject(ready, 2000);
			CloseHandle(th);
		}
		CloseHandle(ready);
		if (!gStatusWnd)
			return 0;
	}
	PostMessageW(gStatusWnd, WM_RS_STATUS, brief ? 1 : 0, 0);
	return 1;
}

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

// text: a switch of the plugin's: "overlay", "overlayoff", "classic",
// "pureref", "left", "multi", "auto3d", "adjust", or "a N" / "b N" (view N in the
// upper / lower Multiview image). Returns 1 if it is on, else 0: each switch
// starts as this, and ZBrush loads the plugin again for every press.
// text "1" or "0": ZBrush's Persp wanted on or off (Auto Persp, when no Persp
// button is found for ZScript to set). Persp is read from ZBrush's projection
// record; if it is not as wanted, ZBrush's Persp key (P) is pressed, which
// ZBrush handles once the script returns. Returns 1 if pressed, 0 if already
// so, -1 if Persp cannot be read or ZBrush is not in front. Logged.
ZFUNC RSPersp(char* text, double, char*, int, char*, int, char**)
{
	const bool want = text && atoi(text) != 0;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	const double docW = gOverlay.docW, docH = gOverlay.docH;
	LeaveCriticalSection(&gOverlayLock);
	bool persp = false;
	double eye = 0;
	float result = -1;
	if (ProjRead(docW, docH, persp, eye) && ZBrushInFront()) {
		result = 0;
		if (persp != want) {
			INPUT in[2] = {};
			in[0].type = in[1].type = INPUT_KEYBOARD;
			in[0].ki.wVk = in[1].ki.wVk = 'P';
			in[1].ki.dwFlags = KEYEVENTF_KEYUP;
			result = SendInput(2, in, sizeof(INPUT)) == 2 ? 1.0f : -1.0f;
		}
	}
	if (FILE* f = _wfopen((DataFolder() + L"nav_log.txt").c_str(), L"ab")) {
		fprintf(f, "%lu auto persp: no Persp button; Persp wanted %s, is %s: %s\n", GetTickCount(), want ? "on" : "off", persp ? "on" : "off",
			result == 1 ? "pressed P" : result == 0 ? "already so" : "could not read Persp or press P");
		fclose(f);
	}
	return result;
}

ZFUNC RSIsOn(char* text, double, char*, int, char*, int, char**)
{
	char name[16] = {0};
	int n = -1;
	if (!text || sscanf(text, "%15s %d", name, &n) < 1)
		return 0;
	EnsureLock();
	EnterCriticalSection(&gOverlayLock);
	LoadDisplay();
	const bool visible = gOverlay.visible;
	bool on = false;
	if (strcmp(name, "overlay") == 0) on = visible;
	else if (strcmp(name, "overlayoff") == 0) on = !visible;
	else if (strcmp(name, "classic") == 0) on = gMode == 0;
	else if (strcmp(name, "pureref") == 0) on = gMode == 1;
	else if (strcmp(name, "left") == 0) on = gPanelLeft != 0;
	else if (strcmp(name, "multi") == 0) on = gMulti != 0;
	else if (strcmp(name, "auto3d") == 0) on = gAuto3D != 0;
	else if (strcmp(name, "autopersp") == 0) on = gAutoPersp != 0;
	else if (strcmp(name, "perspwanted") == 0) on = gPerspFor[gMode == 1];
	else if (strcmp(name, "adjust") == 0) on = gAdjust;
	else if (strcmp(name, "a") == 0) on = gMultiA == n;
	else if (strcmp(name, "b") == 0) on = gMultiB == n;
	LeaveCriticalSection(&gOverlayLock);
	return on ? 1.0f : 0.0f;
}

// Copies JRef into ZBrush's ZStartup\ZPlugs64 folder (JRef.zsc there as
// ZZ_JRef.zsc, the DLL and icon in JRef_Data next to it), so loading JRef.txt once with
// ZScript > Load installs it. Returns 1 if installed, 2 if installed over
// another build (an update: ZBrush then runs the installed copy, which makes
// the buttons), 0 if this already is the installed copy, -1 if it could not
// be written (no permission), -2 if ZBrush has no ZPlugs64 folder, -3 if
// JRef.zsc is not next to JRef_Data.
ZFUNC RSInstall(char*, double, char*, int, char*, int, char**)
{
	wchar_t exe[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring plugs = FolderOf(exe) + L"ZStartup\\ZPlugs64\\";
	if (!IsFolder(plugs))
		return -2;
	const std::wstring own = DllFolder();  // the JRef_Data folder
	const std::wstring parent = FolderOf(own.substr(0, own.size() - 1));  // where JRef.zsc is
	if (_wcsicmp(parent.c_str(), plugs.c_str()) == 0)
		return 0;
	// Once a session, the same answer after: the top level (and this) runs
	// on every press.
	static int done = 0;
	if (done)
		return static_cast<float>(done == 1 ? 0 : done);
	const std::wstring zsc = parent + L"JRef.zsc";
	if (GetFileAttributesW(zsc.c_str()) == INVALID_FILE_ATTRIBUTES)
		return -3;
	// An installed copy that differs from this one is an older build.
	auto readAll = [](const std::wstring& path, std::string& out) {
		FILE* f = _wfopen(path.c_str(), L"rb");
		if (!f)
			return false;
		char buf[65536];
		size_t got;
		while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, got);
		fclose(f);
		return true;
	};
	std::string mine, theirs;
	const bool update = readAll(plugs + L"ZZ_JRef.zsc", theirs) && readAll(zsc, mine) && mine != theirs;
	const std::wstring data = plugs + L"JRef_Data\\";
	CreateDirectoryW(data.c_str(), nullptr);
	const wchar_t* files[2] = {L"JRef64.dll", L"trash.psd"};
	for (const wchar_t* f : files) {
		const std::wstring from = own + f, to = data + f;
		if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
			// The installed DLL is loaded (ZBrush started with it): a loaded
			// DLL can be renamed, then replaced.
			const std::wstring old = to + L".old";
			DeleteFileW(old.c_str());
			if (!MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) || !CopyFileW(from.c_str(), to.c_str(), FALSE))
				return -1;
		}
	}
	// Installed as ZZ_JRef.zsc: ZBrush loads ZPlugs64's plugins in name
	// order, and an update (the installed copy loading this folder's
	// JRef.txt at startup) stops the plugins after it from loading, so JRef
	// goes last. A JRef.zsc from before is removed (it would load twice).
	if (!CopyFileW(zsc.c_str(), (plugs + L"ZZ_JRef.zsc").c_str(), FALSE))
		return -1;
	DeleteFileW((plugs + L"JRef.zsc").c_str());
	// Where it was installed from, for the installed copy to update itself
	// at startup when that folder has another build: the folder, then a 0
	// byte (ZScript's MemReadString reads up to it).
	if (FILE* f = _wfopen((data + L"source.txt").c_str(), L"wb")) {
		char path[MAX_PATH * 2] = {0};
		WideCharToMultiByte(CP_ACP, 0, parent.c_str(), -1, path, sizeof(path), nullptr, nullptr);
		fwrite(path, 1, strlen(path) + 1, f);
		fclose(f);
	}
	ProjectLog(update ? "installed (an update) into" : "installed into", plugs);
	done = update ? 2 : 1;
	return static_cast<float>(done);
}
