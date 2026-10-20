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
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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

// Returns 1 so the script can check the DLL loads at all.
ZFUNC RSVersion(char*, double, char*, int, char*, int, char**)
{
	return 1;
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

// text: "docW docH frameName". Writes an all-black document-sized frame, used
// to clear the canvas while the overlay shows the reference. Returns 1, -1 for
// bad arguments, -4 if the frame cannot be written.
ZFUNC RSBlank(char* text, double, char*, int, char*, int, char**)
{
	double docW, docH;
	char frameName[256] = {0};
	if (!text || sscanf(text, "%lf %lf %255s", &docW, &docH, frameName) != 3 || docW < 1 || docH < 1 || docW > 16384 || docH > 16384)
		return -1;
	StartGdiplus();
	Gdiplus::Bitmap frame(static_cast<INT>(docW), static_cast<INT>(docH), PixelFormat24bppRGB);
	{
		Gdiplus::Graphics g(&frame);
		g.Clear(Gdiplus::Color(255, 0, 0, 0));
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
// Overlay window (stage 1 of the live reference)
// ---------------------------------------------------------------------------
// A click-through layered window owned by ZBrush's main window (so it stays
// above ZBrush but not above other programs), placed over the canvas and
// redrawn on its own thread. It shows the reference with the same layout as
// RSRender, at a constant opacity. The DLL pins itself when the overlay
// starts, since ZBrush may unload it between [FileExecute] calls while the
// overlay thread is still running.

struct OverlayState {
	double screenX = 0, screenY = 0;  // screen position of document (0, 0)
	double pixelsPerDoc = 1;          // screen pixels per document pixel
	double docW = 0, docH = 0;
	double ratio = 1, px = 0, py = 0, ax = 0, ay = 0;
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

// Renders the current state into a premultiplied 32bpp bitmap the size of
// the document area on screen and pushes it to the layered window.
static void OverlayRedraw(HWND wnd)
{
	EnterCriticalSection(&gOverlayLock);
	const OverlayState st = gOverlay;
	const bool haveImage = LoadSavedImage();
	if (!st.visible || !haveImage || st.docW < 1 || st.docH < 1) {
		LeaveCriticalSection(&gOverlayLock);
		ShowWindow(wnd, SW_HIDE);
		return;
	}
	const int w = static_cast<int>(st.docW * st.pixelsPerDoc + 0.5);
	const int h = static_cast<int>(st.docH * st.pixelsPerDoc + 0.5);

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
	{
		Gdiplus::Bitmap target(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
		Gdiplus::Graphics g(&target);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

		const double imgW = gImage->GetWidth(), imgH = gImage->GetHeight();
		const double fit = (st.docW / imgW < st.docH / imgH) ? st.docW / imgW : st.docH / imgH;
		const double baseW = imgW * fit, baseH = imgH * fit;
		const double baseX = (st.docW - baseW) * 0.5, baseY = (st.docH - baseH) * 0.5;
		const double x = st.px + st.ratio * (baseX - st.ax);
		const double y = st.py + st.ratio * (baseY - st.ay);
		const double k = st.pixelsPerDoc;
		g.DrawImage(gImage, Gdiplus::RectF(static_cast<Gdiplus::REAL>(x * k), static_cast<Gdiplus::REAL>(y * k),
			static_cast<Gdiplus::REAL>(baseW * st.ratio * k), static_cast<Gdiplus::REAL>(baseH * st.ratio * k)));
	}
	LeaveCriticalSection(&gOverlayLock);

	POINT dst = {static_cast<LONG>(st.screenX), static_cast<LONG>(st.screenY)};
	SIZE size = {w, h};
	POINT src = {0, 0};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(st.alpha), AC_SRC_ALPHA};
	UpdateLayeredWindow(wnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
	ShowWindow(wnd, SW_SHOWNOACTIVATE);

	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK OverlayProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_RS_REDRAW:
		OverlayRedraw(wnd);
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
	SetEvent(static_cast<HANDLE>(ready));
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	gOverlayThreadId = 0;
	return 0;
}

// Starts the overlay thread once and keeps the DLL loaded for it.
static bool EnsureOverlay()
{
	if (!gOverlayLockReady) {
		InitializeCriticalSection(&gOverlayLock);
		gOverlayLockReady = true;
	}
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
// last RSOverlayStart; "4" 1 if the overlay is showing, else 0.
ZFUNC RSOverlayInfo(char* text, double, char*, int, char*, int, char**)
{
	const int which = text ? atoi(text) : -1;
	switch (which) {
	case 0: return static_cast<float>(gCalibration.screenX);
	case 1: return static_cast<float>(gCalibration.screenY);
	case 2: return static_cast<float>(floor(gCalibration.pixelsPerDoc * 1000 + 0.5));
	case 3: return static_cast<float>(floor(gCalibration.percent));
	case 4: return gOverlayWnd && gOverlay.visible ? 1 : 0;
	}
	return -1;
}

// text: "ratio px py anchorX anchorY", as for RSRender. Returns 1, -1 for bad
// arguments, -2 if the overlay is not running.
ZFUNC RSOverlaySet(char* text, double, char*, int, char*, int, char**)
{
	double r, px, py, ax, ay;
	if (!text || sscanf(text, "%lf %lf %lf %lf %lf", &r, &px, &py, &ax, &ay) != 5 || r <= 0)
		return -1;
	if (!gOverlayWnd)
		return -2;
	EnterCriticalSection(&gOverlayLock);
	gOverlay.ratio = r;
	gOverlay.px = px;
	gOverlay.py = py;
	gOverlay.ax = ax;
	gOverlay.ay = ay;
	LeaveCriticalSection(&gOverlayLock);
	PostMessageW(gOverlayWnd, WM_RS_REDRAW, 0, 0);
	return 1;
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
