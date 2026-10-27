// 3D Auto Track test suite: the plugin's own tracking code (compiled in from
// ReferenceScaler.cpp) driven by a simulated ZBrush, and every panel cell
// scored against the model.
//
// A model (OBJ) stands in for the sculpt. For each camera case it renders
// the depth plane ZBrush would hand over (its values pivot - depth * zoom,
// the pivot depth moving as the model turns and 0 when snapped to a view,
// 32608 where there is no model), sets the camera's axes, zoom and pan, and
// feeds the frame through TakeFrame, as DepthMapStep does with ZBrush's
// planes. The views' silhouettes are rendered from the model the same way
// JRef records them from the canvas. Then MultiviewLayout lays out each
// other view's cell, and the image point at the cell's middle is compared
// with the truth: the model point the camera shows at the middle of its
// window (the ray through it cast against the mesh, its first surface), so
// each cell shows, in its middle, what the viewport shows in its middle.
//
// Errors are in model units and as a share of the window's half size; a
// case passes within 10% on both of the cell's axes. Where the window's
// middle is on an outline (the nose's edge over the eye beyond it), which
// side the middle pixel shows is a coin toss: rays 2% of the half window
// (at least the points' resolution, 5 units) to each side are cast too,
// and the nearest of their surfaces counts.
//
// TrackTest [model.obj] [model scale] [out folder] [case]
//   model.obj    default C:\Users\Friss\Downloads\dafoe_final_v001.obj
//   model scale  ZBrush model units per OBJ unit (default 26.478, measured
//                for that model against its depth frames)
//   out folder   default .\out\ : results.csv, and the plugin's nav_log.txt
//   case         only report cases whose label contains this, each with
//                every crossing of its ray (all cases still run, in order)

#include "../ReferenceScaler.cpp"

namespace track_test {

struct Vec { double x, y, z; };
static Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec operator*(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double Axis(Vec a, int m) { return m == 0 ? a.x : m == 1 ? a.y : a.z; }

struct Mesh {
	std::vector<Vec> v;
	std::vector<uint32_t> tri;  // three per triangle
};

static bool LoadObj(const char* path, double scale, Mesh& mesh)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::string text(static_cast<size_t>(size), '\0');
	fread(&text[0], 1, text.size(), f);
	fclose(f);
	const char* p = text.c_str();
	const char* end = p + text.size();
	while (p < end) {
		const char* eol = static_cast<const char*>(memchr(p, '\n', end - p));
		if (!eol)
			eol = end;
		if (p[0] == 'v' && p[1] == ' ') {
			char* q;
			Vec a;
			a.x = strtod(p + 2, &q);
			a.y = strtod(q, &q);
			a.z = strtod(q, &q);
			mesh.v.push_back(a * scale);
		} else if (p[0] == 'f' && p[1] == ' ') {
			uint32_t idx[8];
			int k = 0;
			const char* q = p + 2;
			while (q < eol && k < 8) {
				while (q < eol && *q == ' ')
					++q;
				if (q >= eol || *q == '\r')
					break;
				idx[k++] = static_cast<uint32_t>(strtol(q, nullptr, 10) - 1);
				while (q < eol && *q != ' ')
					++q;
			}
			for (int t = 1; t + 1 < k; ++t) {
				mesh.tri.push_back(idx[0]);
				mesh.tri.push_back(idx[t]);
				mesh.tri.push_back(idx[t + 1]);
			}
		}
		p = eol + 1;
	}
	return !mesh.v.empty() && !mesh.tri.empty();
}

// A camera as ZBrush's: axes (right, down, towards the camera, in model
// axes), zoom, and the document point the model's origin is drawn at.
struct Camera {
	Vec d[3];
	double r, px, py;
};

static const double kDocW = 1019, kDocH = 776;

// The nearest surface (largest depth towards the camera) per pixel of a grid
// whose pixel (i, j) is at document point (ox + i * ps, oy + j * ps). invD:
// Persp as ZBrush draws it about the document's middle, 0 flat.
static void Rasterize(const Mesh& m, const Camera& c, int w, int h, double ox, double oy, double ps, std::vector<double>& depth, double invD = 0)
{
	depth.assign(static_cast<size_t>(w) * h, -1e300);
	const size_t nt = m.tri.size() / 3;
	std::vector<double> sx(m.v.size()), sy(m.v.size()), sz(m.v.size());
	for (size_t i = 0; i < m.v.size(); ++i) {
		sz[i] = Dot(m.v[i], c.d[2]);
		const double q = std::max(0.05, 1 - sz[i] * invD);
		sx[i] = (kDocW * 0.5 + (c.px + c.r * Dot(m.v[i], c.d[0]) - kDocW * 0.5) / q - ox) / ps;
		sy[i] = (kDocH * 0.5 + (c.py + c.r * Dot(m.v[i], c.d[1]) - kDocH * 0.5) / q - oy) / ps;
	}
	for (size_t t = 0; t < nt; ++t) {
		const uint32_t a = m.tri[3 * t], b = m.tri[3 * t + 1], e = m.tri[3 * t + 2];
		const double x0 = sx[a], y0 = sy[a], x1 = sx[b], y1 = sy[b], x2 = sx[e], y2 = sy[e];
		const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
		if (fabs(area) < 1e-12)
			continue;
		const int i0 = std::max(0, static_cast<int>(ceil(std::min({x0, x1, x2}))));
		const int i1 = std::min(w - 1, static_cast<int>(floor(std::max({x0, x1, x2}))));
		const int j0 = std::max(0, static_cast<int>(ceil(std::min({y0, y1, y2}))));
		const int j1 = std::min(h - 1, static_cast<int>(floor(std::max({y0, y1, y2}))));
		for (int j = j0; j <= j1; ++j)
			for (int i = i0; i <= i1; ++i) {
				const double w0 = ((x1 - i) * (y2 - j) - (x2 - i) * (y1 - j)) / area;
				const double w1 = ((x2 - i) * (y0 - j) - (x0 - i) * (y2 - j)) / area;
				const double w2 = 1 - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0)
					continue;
				const double z = w0 * sz[a] + w1 * sz[b] + w2 * sz[e];
				double& d = depth[static_cast<size_t>(j) * w + i];
				if (z > d)
					d = z;
			}
	}
}

// Every depth (along dir) where the line base + t * dir crosses the mesh,
// nearest the camera (largest t) first; facing, the sign of the first
// crossing's triangle winding against dir (which side of the surface the
// camera is on).
static std::vector<double> Crossings(const Mesh& m, Vec base, Vec dir, double* facing = nullptr)
{
	std::vector<std::pair<double, double>> hits;
	const size_t nt = m.tri.size() / 3;
	for (size_t t = 0; t < nt; ++t) {
		const Vec a = m.v[m.tri[3 * t]], b = m.v[m.tri[3 * t + 1]], c = m.v[m.tri[3 * t + 2]];
		const Vec e1 = b - a, e2 = c - a, pv = Cross(dir, e2);
		const double det = Dot(e1, pv);
		if (fabs(det) < 1e-12)
			continue;
		const Vec tv = base - a;
		const double u = Dot(tv, pv) / det;
		if (u < 0 || u > 1)
			continue;
		const Vec qv = Cross(tv, e1);
		const double v = Dot(dir, qv) / det;
		if (v < 0 || u + v > 1)
			continue;
		hits.push_back({Dot(e2, qv) / det, det > 0 ? 1.0 : -1.0});
	}
	std::sort(hits.begin(), hits.end(), std::greater<std::pair<double, double>>());
	if (facing)
		*facing = hits.empty() ? 0 : hits[0].second;
	// The same crossing found twice (on an edge shared by two triangles).
	std::vector<double> out;
	for (const auto& h : hits)
		if (out.empty() || out.back() - h.first > 1e-6)
			out.push_back(h.first);
	return out;
}

static Camera Orbit(double yawDeg, double pitchDeg, double zoom)
{
	const double y = yawDeg * kPi / 180, p = pitchDeg * kPi / 180;
	Camera c;
	c.d[2] = {-sin(y) * cos(p), sin(p), cos(y) * cos(p)};
	c.d[0] = {cos(y), 0, sin(y)};
	c.d[1] = Cross(c.d[0], c.d[2]);
	c.r = zoom;
	c.px = c.py = 0;
	return c;
}

static const double kAnchorScale = 40.834045, kAnchorX = 536.340820, kAnchorY = 784.979126;

// A view slot set up as JRef records it, its silhouette rendered from the
// model in the view's image space (cells of 1.59 from -509.5, -388, as the
// recorded ones).
static void SetUpView(const Mesh& mesh, int slot)
{
	View& v = gViews[slot];
	v = View();
	v.used = true;
	v.scale = kAnchorScale;
	v.ax = kAnchorX;
	v.ay = kAnchorY;
	v.codes = {kViewCodes[slot]};
	v.path = slot == 0 ? "Front.jpg" : "Right.jpg";
	v.gw = 1280;
	v.gh = 975;
	v.cell = 1.59;
	v.u0 = -509.5;
	v.v0 = -388;
	double d[3][3];
	ViewAxes(slot, d);
	Camera c;
	for (int a = 0; a < 3; ++a)
		c.d[a] = {d[a][0], d[a][1], d[a][2]};
	c.r = 1;
	c.px = v.ax;
	c.py = v.ay;
	std::vector<double> depth;
	Rasterize(mesh, c, v.gw, v.gh, v.u0 + 0.5 * v.cell, v.v0 + 0.5 * v.cell, v.cell, depth);
	v.mask.resize(depth.size());
	for (size_t i = 0; i < depth.size(); ++i)
		v.mask[i] = depth[i] > -1e299 ? 2 : 1;
}

struct Target { const char* name; Vec p; };

struct Result {
	std::string label;
	int slot;
	double errU, errV, halfU, halfV;  // model units, along the cell's two axes
	double shareU, shareV;
	bool pass;
	// The points' depth at the window's middle (CloudDepth, as the layout asks
	// it) against the truth's, and how many bins it went by (0: none found).
	double cloudErr;
	int cloudSamples;
	bool edge;  // the middle on an outline: the rays round it meet surfaces apart
	// Persp cases: the cell's zoom over the model's as drawn there, less 1.
	int kind = 0;  // 0 flat, 1 Persp after the flat frames, 2 Persp from no points
	double scaleErr = 0;
};

// The window's middle (document pixels) and half size (model units), as
// MultiviewLayout works them out: the viewport beside the panel.
static void Window(double r, double& midX, double& midY, double half[2])
{
	int vx0, vy0, vx1, vy1, px, py, pw, ph;
	const int w = static_cast<int>(kDocW + 0.5), h = static_cast<int>(kDocH + 0.5);
	ViewportRect(w, h, vx0, vy0, vx1, vy1);
	PanelRect(w, h, px, py, pw, ph);
	if (gPanelLeft)
		vx0 = px + pw;
	else
		vx1 = px;
	midX = (vx0 + vx1) * 0.5;
	midY = (vy0 + vy1) * 0.5;
	half[0] = (vx1 - vx0) * 0.5 / r;
	half[1] = (vy1 - vy0) * 0.5 / r;
}

// The image's size through pans and zooms (HoldScale, as MemFollow drives
// it): the size shown, ZBrush's zoom times fix, after each sequence. False
// for any off by over 0.5% from what it should be.
static bool ScaleTests()
{
	struct Step { bool hold; double r; };
	auto run = [](const std::vector<Step>& steps) {
		ScaleHold h;
		double ratio = steps.front().r, fix = 1;
		for (const Step& s : steps) {
			HoldScale(h, s.hold, ratio, s.r, fix);
			ratio = s.r;
		}
		HoldScale(h, false, ratio, ratio, fix);
		return ratio * fix;
	};
	std::vector<Step> jitter, zoom, small;
	// A pan at zoom 2.5, ZBrush's scale reading 2.48 to 2.52 through it.
	for (int i = 0; i <= 30; ++i)
		jitter.push_back({i > 0, 2.5 + 0.02 * sin(i * 0.7)});
	jitter.push_back({false, 2.5});
	// Alt + right drag zooms out to 0.47 and back to 0.83, read as pans.
	for (int i = 0; i <= 30; ++i)
		zoom.push_back({i > 0, 1.0 - 0.53 * i / 30});
	zoom.push_back({false, 0.47});
	for (int i = 0; i <= 30; ++i)
		zoom.push_back({i > 0, 0.47 + 0.36 * i / 30});
	zoom.push_back({false, 0.83});
	// Twenty short zooms in of 3% each, inside pans.
	double r = 1;
	small.push_back({false, r});
	for (int k = 0; k < 20; ++k) {
		for (int i = 1; i <= 5; ++i)
			small.push_back({true, r * (1 + 0.03 * i / 5)});
		r *= 1.03;
		small.push_back({false, r});
	}
	struct Case { const char* name; double shown, want; };
	const Case cases[] = {
		{"pan with ZBrush's jitter", run(jitter), 2.5},
		{"zooms out and in read as pans", run(zoom), 0.83},
		{"twenty 3% zooms inside pans", run(small), r},
	};
	bool ok = true;
	printf("image size through pans and zooms:\n");
	for (const Case& c : cases) {
		const bool pass = fabs(c.shown / c.want - 1) <= 0.005;
		ok = ok && pass;
		printf("  %-32s shown at %.4f, should be %.4f: %s\n", c.name, c.shown, c.want, pass ? "pass" : "FAIL");
	}
	return ok;
}

// A view's saved silhouette with a palette drawn over the empty canvas next
// to the model (SampleSilhouette, as recording, matching and the refresh
// use it): with ZBrush's depth the palette is not the model; the model is.
static bool SilhouetteTests()
{
	const int w = 1000, h = 800;
	gKeyW = w;
	gKeyH = h;
	gKeyShare.assign(static_cast<size_t>(w) * h, 255);  // magenta: the empty canvas
	std::vector<float> depth(static_cast<size_t>(w) * h, 35840.0f);
	int modelPx = 0, palettePx = 0;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const size_t at = static_cast<size_t>(y) * w + x;
			if ((x - 500) * (x - 500) + (y - 400) * (y - 400) < 150 * 150) {
				gKeyShare[at] = 0;
				depth[at] = -100.0f - 0.1f * (x % 7);
				++modelPx;
			} else if (x >= 700 && x < 950 && y >= 100 && y < 700) {
				gKeyShare[at] = 0;  // a palette: drawn, but no depth
				++palettePx;
			}
		}
	OverlayState st;
	st.px = 500;
	st.py = 400;
	st.ratio = 1;
	st.ax = st.ay = 0;
	st.pixelsPerDoc = 1;
	st.docW = w;
	st.docH = h;
	auto count = [&](bool withDepth, int& model, int& palette) {
		gDepth = DepthBuffer();
		if (withDepth) {
			gDepth.ok = true;
			gDepth.base = reinterpret_cast<uintptr_t>(depth.data());
			gDepth.pitch = sizeof(float) * w;
			gDepth.stride = sizeof(float);
			gDepth.w = w;
			gDepth.h = h;
			gDepth.bg = 35840;
		}
		View g;
		std::vector<unsigned char> out;
		SampleSilhouette(st, 0, 0, g, out);
		model = palette = 0;
		for (int j = 0; j < g.gh; ++j)
			for (int i = 0; i < g.gw; ++i) {
				if (out[static_cast<size_t>(j) * g.gw + i] != 2)
					continue;
				const double x = st.px + g.u0 + (i + 0.5) * g.cell, y = st.py + g.v0 + (j + 0.5) * g.cell;
				(x >= 700 && x < 950 && y >= 100 && y < 700 ? palette : model) += 1;
			}
		gDepth = DepthBuffer();
	};
	int m0, p0, m1, p1;
	count(false, m0, p0);
	count(true, m1, p1);
	gKeyShare.clear();
	gKeyW = gKeyH = 0;
	const bool pass = p0 > 0 && p1 == 0 && m1 > 0 && m1 >= m0 * 0.99;
	printf("view silhouette with a palette over the canvas:\n");
	printf("  capture alone: %d model cells, %d on the palette; with depth: %d model cells, %d on the palette: %s\n", m0, p0, m1, p1,
		pass ? "pass" : "FAIL");
	return pass;
}

// Whether the view counts as turning (Turning, after RotFollow reads the
// rotation's copies, as each overlay frame does), with nine copies of
// ZBrush's rotation in this process's memory, snapped to Right:
// - an Alt pan in which one copy reads Front for a frame (build 205's log):
//   not turning, and that copy is dropped;
// - a turn drag in which three of the copies move (as ZBrush's do): turning.
static bool RotationTests()
{
	static double mats[9][9];
	const double right[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0}, front[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const double a = 0.2, turned[9] = {cos(a), 0, sin(a), 0, 1, 0, -sin(a), 0, cos(a)};
	auto setUp = [&]() {
		gRotCands.clear();
		for (int k = 0; k < 9; ++k) {
			memcpy(mats[k], right, sizeof(right));
			RotCand c = {};
			c.addr = reinterpret_cast<uintptr_t>(mats[k]);
			c.isDouble = true;
			c.stride = 3;
			c.unit = true;
			c.rel = 0;
			memcpy(c.m, right, sizeof(right));
			memcpy(c.settled, right, sizeof(right));
			gRotCands.push_back(c);
		}
		gRotLocked = true;
		gRotSettled = true;
		gGesture = Gesture();
		gGesture.active = true;
		gButtonsDown = 1;
		gLastButtonAt = GetTickCount();
	};
	// The pan.
	setUp();
	gGesture.mode = kDragMove;
	RotFollow();
	memcpy(mats[1], front, sizeof(front));
	RotFollow();
	const bool panFlicker = Turning();
	memcpy(mats[1], right, sizeof(right));
	RotFollow();
	const bool panAfter = Turning();
	const size_t panLeft = gRotCands.size();
	// The turn.
	setUp();
	gGesture.mode = kDragNone;
	RotFollow();
	for (int k = 0; k < 3; ++k)
		memcpy(mats[k], turned, sizeof(turned));
	RotFollow();
	const bool turnSeen = Turning();
	const size_t turnLeft = gRotCands.size();
	// The camera's axes through the turn: the three moving copies', not the
	// six still holding the rotation from before it; and after the release,
	// when all nine hold the new one, the same.
	auto axes = [](double d[3][3]) {
		gTesting = false;
		const bool ok = CameraAxes(d);
		gTesting = true;
		return ok;
	};
	double before[3][3] = {}, during[3][3] = {}, after[3][3] = {};
	const bool duringOk = axes(during);
	for (int k = 3; k < 9; ++k)
		memcpy(mats[k], turned, sizeof(turned));
	RotFollow();
	const bool afterOk = axes(after);
	for (int k = 0; k < 9; ++k)
		memcpy(mats[k], right, sizeof(right));
	RotFollow();
	const bool beforeOk = axes(before);
	double off = 0, turnSize = 0;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			off = std::max(off, fabs(during[r][c] - after[r][c]));
			turnSize = std::max(turnSize, fabs(before[r][c] - after[r][c]));
		}
	const bool axesOk = duringOk && afterOk && beforeOk && off < 1e-6 && turnSize > 0.1;

	gRotCands.clear();
	gRotLocked = false;
	gRotSettled = true;
	gTurnKnown = gTurning = false;
	gGesture = Gesture();
	gButtonsDown = 0;
	const bool panOk = !panFlicker && !panAfter && panLeft == 8, turnOk = turnSeen && turnLeft == 9;
	printf("turning, read from nine copies of ZBrush's rotation:\n");
	printf("  Alt pan, one copy flickers to Front    turning %d then %d, %zu copies kept: %s\n", panFlicker, panAfter, panLeft,
		panOk ? "pass" : "FAIL");
	printf("  turn drag, three copies move           turning %d, %zu copies kept: %s\n", turnSeen, turnLeft, turnOk ? "pass" : "FAIL");
	printf("  camera axes through the turn           %.4f off the turned rotation: %s\n", off, axesOk ? "pass" : "FAIL");
	return panOk && turnOk && axesOk;
}

static int Run(int argc, char** argv)
{
	const char* objPath = argc > 1 ? argv[1] : "C:\\Users\\Friss\\Downloads\\dafoe_final_v001.obj";
	const double scale = argc > 2 ? atof(argv[2]) : 26.478;
	const std::string outDir = argc > 3 ? argv[3] : "out\\";
	const std::string only = argc > 4 ? argv[4] : "";
	CreateDirectoryA(outDir.c_str(), nullptr);
	const DWORD t0 = GetTickCount();
	Mesh mesh;
	if (!LoadObj(objPath, scale, mesh)) {
		printf("could not read %s\n", objPath);
		return 2;
	}
	printf("model: %zu vertices, %zu triangles (%lu ms)\n", mesh.v.size(), mesh.tri.size() / 3, GetTickCount() - t0);
	const bool scaleOk = ScaleTests() & SilhouetteTests();

	// The plugin as it runs in PureRef mode with Multiview on, Front over
	// Right, 3D Auto Track on, its log in the out folder.
	InitializeCriticalSection(&gOverlayLock);
	gOverlayLockReady = true;
	gTesting = true;
	gDataFolder = std::wstring(outDir.begin(), outDir.end());
	_wremove((gDataFolder + L"nav_log.txt").c_str());
	const bool rotOk = RotationTests();
	gViewsLoaded = true;
	gAnchorScale = kAnchorScale;
	gMode = 1;
	gMulti = true;
	gMultiA = 0;
	gMultiB = 2;
	gAuto3D = true;
	gPanelLeft = false;
	SetUpView(mesh, 0);
	SetUpView(mesh, 2);
	printf("silhouettes rendered (%lu ms)\n", GetTickCount() - t0);

	// Points to look at, on the surface: the nose tip, and points found by
	// casting straight in from the front and from the side.
	Vec nose = mesh.v[0];
	for (const Vec& p : mesh.v)
		if (p.z > nose.z)
			nose = p;
	auto fromFront = [&](const char* name, double dx, double dy) {
		const Vec base = {nose.x + dx, nose.y + dy, 0};
		const std::vector<double> ts = Crossings(mesh, base, {0, 0, 1});
		return Target{name, ts.empty() ? nose : base + Vec{0, 0, 1} * ts[0]};
	};
	auto fromSide = [&](const char* name, double dz, double dy, double side) {
		const Vec base = {0, nose.y + dy, nose.z + dz};
		const std::vector<double> ts = Crossings(mesh, base, {side, 0, 0});
		return Target{name, ts.empty() ? nose : base + Vec{side, 0, 0} * ts[0]};
	};
	const std::vector<Target> targets = {
		{"nose tip", nose},
		fromFront("left eye", 75, 95),
		fromFront("right eye", -75, 95),
		fromFront("mouth", 0, -95),
		fromFront("chin", 0, -190),
		fromFront("forehead", 0, 250),
		fromFront("left cheek", 120, -10),
		fromSide("left ear", -330, 40, 1),
		fromSide("right ear", -330, 40, -1),
	};

	// Which winding faces the camera: the nose tip's, seen from the front.
	double outside = 0;
	Crossings(mesh, {nose.x, nose.y, 0}, {0, 0, 1}, &outside);

	std::vector<Result> results;
	std::vector<float> plane(static_cast<size_t>(kDocW) * static_cast<size_t>(kDocH));
	std::vector<double> depth;
	DWORD tick = 1000;
	int cases = 0, skipped = 0;
	struct Angle { double yaw, pitch; int snapped; };
	std::vector<Angle> angles;
	for (double yaw : {0.0, 15.0, 30.0, 45.0, 60.0, 75.0, 90.0, -30.0, -60.0, -90.0, 135.0})
		for (double pitch : {0.0, 25.0, -25.0})
			angles.push_back({yaw, pitch, pitch == 0 && yaw == 0 ? 0 : pitch == 0 && yaw == 90 ? 2 : -1});
	for (const Angle& an : angles)
		for (double zoom : {1.0, 2.5, 5.0})
			for (const Target& tg : targets) {
				Camera cam = Orbit(an.yaw, an.pitch, zoom);
				double midX, midY, half[2];
				Window(zoom, midX, midY, half);
				// Panned so the target is at the window's middle.
				cam.px = midX - zoom * Dot(tg.p, cam.d[0]);
				cam.py = midY - zoom * Dot(tg.p, cam.d[1]);
				// The truth: what the ray through the window's middle meets.
				const Vec base = cam.d[0] * ((midX - cam.px) / zoom) + cam.d[1] * ((midY - cam.py) / zoom);
				double facing = 0;
				const std::vector<double> ts = Crossings(mesh, base, cam.d[2], &facing);
				if (ts.size() < 2 || facing != outside) {
					++skipped;  // looking past the model, or into it through an opening (the neck)
					continue;
				}
				// The view the camera is snapped to, if any (its cell is not shown).
				gActiveView = an.snapped;
				gViewAway = an.snapped < 0;
				// ZBrush's pivot depth: 0 snapped, otherwise wherever the turn left it.
				const double pivot = an.snapped >= 0 ? 0 : 400 * sin(an.yaw * kPi / 180 + 0.3) * zoom + 90 * an.pitch / 25;
				for (int a = 0; a < 3; ++a) {
					gTestCam.d[a][0] = cam.d[a].x;
					gTestCam.d[a][1] = cam.d[a].y;
					gTestCam.d[a][2] = cam.d[a].z;
				}
				gTestCam.pivot = pivot;
				OverlayState st;
				st.docW = kDocW;
				st.docH = kDocH;
				st.pixelsPerDoc = 1;
				st.ratio = zoom;
				st.px = cam.px;
				st.py = cam.py;
				st.visible = true;
				// The finished frame ZBrush would draw at this camera.
				Rasterize(mesh, cam, static_cast<int>(kDocW), static_cast<int>(kDocH), 0, 0, 1, depth);
				for (size_t i = 0; i < plane.size(); ++i)
					plane[i] = depth[i] > -1e299 ? static_cast<float>(pivot - depth[i] * zoom) : 32608.0f;
				TakeFrame(st, gTestCam.d, plane, static_cast<int>(kDocW), static_cast<int>(kDocH), tick += 500);
				++cases;
				const Vec truth = base + cam.d[2] * ts[0];
				// The surfaces just beside the middle, for the middle on an outline.
				std::vector<Vec> truths = {truth};
				bool edge = false;
				for (int k = 0; k < 4; ++k) {
					// 2% of the half window, at least the points' resolution (the
					// plugin keeps a point per 2 units, from half-size frames; its
					// bins are 2.5 of those).
					const double step = std::max(0.02 * half[0], 2.5 * kCloudCell);
					const Vec off = cam.d[k / 2] * (k % 2 ? step : -step);
					const std::vector<double> near = Crossings(mesh, base + off, cam.d[2]);
					if (near.empty())
						continue;
					truths.push_back(base + off + cam.d[2] * near[0]);
					edge = edge || fabs(near[0] - ts[0]) > 0.1 * half[0];
				}
				double cloudFront = 0, cloudThick = 0;
				int cloudSamples = 0;
				{
					Cam lc;
					LiveCam(st, gTestCam.d, lc);
					const double reach = 0.25 * std::min(half[0], half[1]) * 2;
					if (!CloudDepth(lc, midX, midY, reach, cloudFront, cloudThick, cloudSamples))
						cloudSamples = 0;
				}
				for (int slot : {0, 2}) {
					if (slot == an.snapped)
						continue;
					OverlayState out;
					std::string path;
					if (!MultiviewLayout(slot, st, out, path))
						continue;
					const ViewFrame vf = FrameFor(slot, 0, 0);
					const double centre[2] = {(midX - out.px) / out.ratio + out.ax, (midY - out.py) / out.ratio + out.ay};
					Result res;
					char label[160];
					snprintf(label, sizeof label, "yaw %+4.0f pitch %+3.0f zoom %.1f %-11s", an.yaw, an.pitch, zoom, tg.name);
					res.label = label;
					res.slot = slot;
					double err[2] = {0, 0}, best = -1;
					for (const Vec& tp : truths) {
						double e[2];
						for (int a = 0; a < 2; ++a)
							e[a] = (centre[a] - ToImage(vf, a, Axis(tp, vf.m[a]))) * vf.f;
						const double worst = std::max(fabs(e[0]) / half[0], fabs(e[1]) / half[1]);
						if (best < 0 || worst < best) {
							best = worst;
							err[0] = e[0];
							err[1] = e[1];
						}
					}
					res.edge = edge;
					res.errU = err[0];
					res.errV = err[1];
					res.halfU = half[0];
					res.halfV = half[1];
					res.shareU = fabs(err[0]) / half[0];
					res.shareV = fabs(err[1]) / half[1];
					res.pass = res.shareU <= 0.10 && res.shareV <= 0.10;
					res.cloudErr = cloudSamples ? cloudFront - ts[0] : 0;
					res.cloudSamples = cloudSamples;
					if (!only.empty()) {
						if (res.label.find(only) == std::string::npos)
							continue;
						printf("%s %s cell: error %+.1f %+.1f; points' depth %.1f (%d bins); crossings (depth, model point):", label, slot == 0 ? "Front" : "Right", err[0], err[1], cloudFront, cloudSamples);
						for (double t : ts) {
							const Vec p = base + cam.d[2] * t;
							printf(" [%.1f: %.0f %.0f %.0f]", t, p.x, p.y, p.z);
						}
						printf("\n");
					}
					results.push_back(res);
				}
			}

	// Persp (Draw > Persp, 50 mm on 1019 x 776: the eye at 1415.28 document
	// pixels up to zoom 1.38, 0.7256 x the zoom x that past it, as ZBrush's
	// record has it), its frames as ZBrush draws them: a point Z = zoom * depth
	// in front of the pivot's plane at E / (E - Z) times its flat place, its
	// value pivot - 2E - Z * E / (E - Z). Twice: after the flat frames above,
	// and from no points (Persp on from the start, as Auto Persp has it in
	// PureRef), each case's frame taken before its layout. The camera panned so
	// the target is drawn at the window's middle; the truth: what the ray
	// through the middle meets, towards the eye. Each cell, the snapped view's
	// too, should centre on it and be zoomed as the model is drawn there.
	const double kEye = 1415.28;
	gTestProjD0 = kEye;
	int perspCases = 0, perspTaken[3] = {}, perspTried[3] = {};
	for (int kind : {1, 2}) {
		if (kind == 2) {
			gCloud.clear();
			gCloudAt.clear();
			gFrames.clear();
		}
		for (const Angle& an : angles)
			for (double zoom : {1.0, 2.5})
				for (const Target& tg : targets) {
					Camera cam = Orbit(an.yaw, an.pitch, zoom);
					const double eye = std::max(kEye, 0.72562 * zoom * kEye), invD = zoom / eye, cx = kDocW * 0.5, cy = kDocH * 0.5;
					double midX, midY, half[2];
					Window(zoom, midX, midY, half);
					const double q = 1 - Dot(tg.p, cam.d[2]) * invD;
					cam.px = cx + (midX - cx) * q - zoom * Dot(tg.p, cam.d[0]);
					cam.py = cy + (midY - cy) * q - zoom * Dot(tg.p, cam.d[1]);
					const Vec base = cam.d[0] * ((midX - cam.px) / zoom) + cam.d[1] * ((midY - cam.py) / zoom);
					const Vec dir = cam.d[2] - (cam.d[0] * (midX - cx) + cam.d[1] * (midY - cy)) * (invD / zoom);
					double facing = 0;
					const std::vector<double> ts = Crossings(mesh, base, dir, &facing);
					// Past the model, into it, or the model reaching the eye.
					if (ts.size() < 2 || facing != outside || ts[0] * invD > 0.9) {
						++skipped;
						continue;
					}
					gActiveView = an.snapped;
					gViewAway = an.snapped < 0;
					const double pivot = an.snapped >= 0 ? 0 : 400 * sin(an.yaw * kPi / 180 + 0.3) * zoom + 90 * an.pitch / 25;
					for (int a = 0; a < 3; ++a) {
						gTestCam.d[a][0] = cam.d[a].x;
						gTestCam.d[a][1] = cam.d[a].y;
						gTestCam.d[a][2] = cam.d[a].z;
					}
					gTestCam.pivot = pivot;
					OverlayState st;
					st.docW = kDocW;
					st.docH = kDocH;
					st.pixelsPerDoc = 1;
					st.ratio = zoom;
					st.px = cam.px;
					st.py = cam.py;
					st.visible = true;
					Rasterize(mesh, cam, static_cast<int>(kDocW), static_cast<int>(kDocH), 0, 0, 1, depth, invD);
					for (size_t i = 0; i < plane.size(); ++i)
						plane[i] = depth[i] > -1e299 ? static_cast<float>(pivot - 2 * eye - zoom * depth[i] / (1 - depth[i] * invD)) : 32608.0f;
					const int version = gCloudVersion;
					TakeFrame(st, gTestCam.d, plane, static_cast<int>(kDocW), static_cast<int>(kDocH), tick += 500);
					perspTaken[kind] += gCloudVersion != version;
					++perspTried[kind];
					++perspCases;
					// How much larger than on the pivot's plane the model is drawn at
					// the surface looked at.
					const double grow = 1 / (1 - ts[0] * invD);
					std::vector<Vec> truths = {base + dir * ts[0]};
					std::vector<double> truthT = {ts[0]};
					bool edge = false;
					for (int k = 0; k < 4; ++k) {
						const double step = std::max(0.02 * half[0], 2.5 * kCloudCell);
						const Vec off = cam.d[k / 2] * (k % 2 ? step : -step);
						const std::vector<double> near = Crossings(mesh, base + off, dir);
						if (near.empty())
							continue;
						truths.push_back(base + off + dir * near[0]);
						truthT.push_back(near[0]);
						edge = edge || fabs(near[0] - ts[0]) > 0.1 * half[0];
					}
					for (int slot : {0, 2}) {
						OverlayState out;
						std::string path;
						if (!MultiviewLayout(slot, st, out, path))
							continue;
						const ViewFrame vf = FrameFor(slot, 0, 0);
						const double centre[2] = {(midX - out.px) / out.ratio + out.ax, (midY - out.py) / out.ratio + out.ay};
						// The window the cell shows, in model units at the surface.
						const double halfS[2] = {half[0] / grow, half[1] / grow};
						Result res;
						char label[160];
						snprintf(label, sizeof label, "%s yaw %+4.0f pitch %+3.0f zoom %.1f %-11s", kind == 1 ? "persp " : "persp0", an.yaw, an.pitch, zoom, tg.name);
						res.label = label;
						res.slot = slot;
						double err[2] = {0, 0}, best = -1;
						for (const Vec& tp : truths) {
							double e[2];
							for (int a = 0; a < 2; ++a)
								e[a] = (centre[a] - ToImage(vf, a, Axis(tp, vf.m[a]))) * vf.f;
							const double worst = std::max(fabs(e[0]) / halfS[0], fabs(e[1]) / halfS[1]);
							if (best < 0 || worst < best) {
								best = worst;
								err[0] = e[0];
								err[1] = e[1];
							}
						}
						res.edge = edge;
						res.errU = err[0];
						res.errV = err[1];
						res.halfU = halfS[0];
						res.halfV = halfS[1];
						res.shareU = fabs(err[0]) / halfS[0];
						res.shareV = fabs(err[1]) / halfS[1];
						res.kind = kind;
						// On an outline, as drawn at whichever surface beside the middle is nearest.
						res.scaleErr = out.ratio / (zoom * vf.f * grow) - 1;
						for (double t : truthT) {
							const double e = out.ratio * (1 - t * invD) / (zoom * vf.f) - 1;
							if (fabs(e) < fabs(res.scaleErr))
								res.scaleErr = e;
						}
						res.pass = res.shareU <= 0.10 && res.shareV <= 0.10 && fabs(res.scaleErr) <= 0.05;
						res.cloudErr = 0;
						res.cloudSamples = 0;
						if (!only.empty()) {
							if (res.label.find(only) == std::string::npos)
								continue;
							printf("%s %s cell: error %+.1f %+.1f, zoom %+.1f%%; surface at %.1f, drawn x%.3f\n", label, slot == 0 ? "Front" : "Right", err[0], err[1], 100 * res.scaleErr, ts[0], grow);
						}
						results.push_back(res);
					}
				}
	}
	gTestProjD0 = 0;
	// Every Persp frame taken: its depth from the eye, none put elsewhere by
	// the points (PerspCheck).
	const bool perspFramesOk = perspTaken[1] == perspTried[1] && perspTaken[2] == perspTried[2];
	printf("Persp frames taken: %d of %d after the flat frames, %d of %d from no points: %s\n", perspTaken[1], perspTried[1], perspTaken[2], perspTried[2], perspFramesOk ? "pass" : "FAIL");

	// Report: every failure, a summary per view and per zoom, results.csv.
	int pass = 0;
	for (const Result& r : results)
		pass += r.pass;
	if (FILE* f = fopen((outDir + "results.csv").c_str(), "wb")) {
		fprintf(f, "case,cell,error across (model units),error down,half window across,half window down,share across,share down,points' depth error,bins,on an edge,pass\n");
		for (const Result& r : results)
			fprintf(f, "%s,%s,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.2f,%d,%d,%d\n", r.label.c_str(), r.slot == 0 ? "Front" : "Right", r.errU, r.errV, r.halfU, r.halfV, r.shareU, r.shareV, r.cloudErr, r.cloudSamples, r.edge ? 1 : 0, r.pass ? 1 : 0);
		fclose(f);
	}
	printf("\nfailures (error as a share of the window's half size, across / down the cell):\n");
	for (const Result& r : results)
		if (!r.pass)
			if (r.kind)
				printf("  %s  %s cell  %+7.1f %+7.1f units  (%3.0f%% %3.0f%%)  zoom %+.1f%% off%s\n", r.label.c_str(), r.slot == 0 ? "Front" : "Right", r.errU, r.errV, 100 * r.shareU, 100 * r.shareV, 100 * r.scaleErr, r.edge ? "  on an edge" : "");
			else
				printf("  %s  %s cell  %+7.1f %+7.1f units  (%3.0f%% %3.0f%%)  points' depth %+6.1f off (%d bins)%s\n", r.label.c_str(), r.slot == 0 ? "Front" : "Right", r.errU, r.errV, 100 * r.shareU, 100 * r.shareV, r.cloudErr, r.cloudSamples, r.edge ? "  on an edge" : "");
	const char* kindName[3] = {"", "Persp ", "Persp from no points, "};
	for (int kind : {0, 1, 2})
		for (int slot : {0, 2}) {
			int n = 0, ok = 0;
			std::vector<double> shares, zooms;
			for (const Result& r : results)
				if (r.slot == slot && r.kind == kind) {
					++n;
					ok += r.pass;
					shares.push_back(std::max(r.shareU, r.shareV));
					zooms.push_back(fabs(r.scaleErr));
				}
			std::sort(shares.begin(), shares.end());
			std::sort(zooms.begin(), zooms.end());
			if (n)
				printf("%s%s cell: %d of %d pass; worst-axis error median %.0f%%, 90th %.0f%%, max %.0f%% of the half window%s", kindName[kind], slot == 0 ? "Front" : "Right", ok, n,
					100 * shares[shares.size() / 2], 100 * shares[shares.size() * 9 / 10], 100 * shares.back(), kind ? "" : "\n");
			if (n && kind)
				printf("; zoom off median %.1f%%, max %.1f%%\n", 100 * zooms[zooms.size() / 2], 100 * zooms.back());
		}
	printf("%d cases, %d in Persp (%d looking past or into the model skipped), %zu cells: %d pass, %zu fail (%lu ms)\n", cases + perspCases, perspCases, skipped, results.size(), pass, results.size() - pass, GetTickCount() - t0);
	return pass == static_cast<int>(results.size()) && scaleOk && rotOk && perspFramesOk ? 0 : 1;
}

}  // namespace track_test

int main(int argc, char** argv)
{
	return track_test::Run(argc, argv);
}
