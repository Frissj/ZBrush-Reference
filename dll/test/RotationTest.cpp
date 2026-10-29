// Exercises the production camera reader against matrices in this process.
// No ZBrush process, window, input, screenshot or external memory access.
#include "../ReferenceScaler.cpp"

static int failures = 0;
static void Check(bool ok, const char* label) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", label);
	if (!ok) ++failures;
}
static void Matrix(float* m, double degrees) {
	const double a = degrees*kPi/180;
	const float values[9] = {float(cos(a)),0,float(-sin(a)),0,1,0,float(sin(a)),0,float(cos(a))};
	memcpy(m,values,sizeof(values));
}
static RotCand Candidate(float* memory, double previous=0) {
	RotCand c = {};
	c.addr = reinterpret_cast<uintptr_t>(memory); c.stride=3; c.unit=true; c.rel=0;
	float old[9]; Matrix(old,previous);
	for(int i=0;i<9;++i) c.m[i]=c.m1[i]=c.m2[i]=c.settled[i]=old[i];
	return c;
}
static void Reset() {
	gRotCands.clear(); gRotLocked=true; gRotSettled=false;
	gRotNarrowings=0; gRotWatchUntil=0;
	gGesture={}; gButtonsDown=0; gLastButtonAt=GetTickCount()-4000;
	gDragEndAt=GetTickCount()-1000;
	gTurnKnown=gTurning=true; gAxesHeldSet=true; gViewAway=false;
}
int main(int argc, char** argv) {
	if(argc!=2) { puts("Usage: RotationTest scratch-folder"); return 2; }
	gTesting=true; gDataFolder=Widen(argv[1]);
	if(gDataFolder.back()!=L'\\') gDataFolder+=L'\\';
	gProjectDir=gDataFolder; gProjectTried=true; gViewsLoaded=true;
	float memory[9], other[9];
	Reset(); Matrix(memory,90); gRotCands={Candidate(memory)};
	Check(RotCodes().empty() && gRotCands.empty() && !gRotLocked,
		"last idle candidate dropped: lock clears");
	Check(!gTurnKnown && !gTurning && !gAxesHeldSet && gViewAway,
		"tracking loss clears stale camera state");
	Reset(); Matrix(memory,90); auto pan=Candidate(memory,90); pan.panStep=true;
	Matrix(other,0); auto unknown=Candidate(other); unknown.rel=-1;
	gRotCands={pan,unknown}; gLastButtonAt=GetTickCount();
	Check(RotCodes().empty() && !gRotLocked,"last usable pan-rejected candidate clears lock");
	Reset(); auto invalid=Candidate(memory); invalid.addr=0; gRotCands={invalid};
	Check(RotCodes().empty() && !gRotLocked && gRotCands.empty(),"unreadable camera memory clears lock");
	Reset(); Matrix(memory,0); auto unrelated=Candidate(memory); unrelated.rel=-1; gRotCands={unrelated};
	Check(RotCodes().empty() && !gRotLocked,"unrelated copies cannot keep a lock alive");
	Reset(); Matrix(memory,90); Matrix(other,0);
	gRotCands={Candidate(memory),Candidate(other)};
	const auto codes=RotCodes();
	Check(gRotLocked && gRotCands.size()==1 && codes.size()==1 && codes[0]==kViewCodes[0],
		"rejecting a false copy preserves a valid Front camera");
	Reset(); Matrix(memory,90); gRotCands={Candidate(memory)};
	gRotWatchUntil=GetTickCount()+1500;
	const auto grace=RotCodes();
	Check(gRotLocked && gRotCands.size()==1 && !grace.empty(),"probe redraw grace preserves camera without mouse input");
	gRotWatchUntil=GetTickCount()-1; Matrix(memory,0);
	Check(RotCodes().empty() && !gRotLocked,"idle rejection resumes after probe grace expires");
	Reset(); gRotLocked=false; Matrix(memory,0); Matrix(other,30);
	gRotCands={Candidate(memory),Candidate(other)};
	RotNarrow();
	Check(gRotCands.size()==1 && gRotCands[0].addr==reinterpret_cast<uintptr_t>(other),
		"probe narrowing rejects unchanged matrices and retains changed camera");
	// Finish the real exported probe, then deliver its late redraw. The copy
	// is retained and both loaded views can activate, without a mouse event.
	Reset(); gRotLocked=false; Matrix(memory,0); gRotCands={Candidate(memory,30)};
	gRotNarrowings=1;
	char last[]="last 0 0 0";
	Check(RSRotProbe(last,0,nullptr,0,nullptr,0,nullptr)==1 && gRotLocked,
		"completed probe locks a validated camera");
	gViews[0].used=gViews[2].used=true;
	gViews[0].path="Front.jpg"; gViews[2].path="Right.jpg";
	gViews[0].scale=gViews[2].scale=1;
	gActiveView=-2;
	const auto front=RotCodes(); ViewTickCodes(front);
	Check(gActiveView==0,"Front reference activates after probe");
	Matrix(memory,-90);
	const auto right=RotCodes(); ViewTickCodes(right);
	Check(gRotLocked && gActiveView==2,"Right reference activates after delayed camera redraw");
	Reset(); gRotLocked=false; Matrix(memory,30); gRotCands={Candidate(memory,30)};
	gRotNarrowings=1;
	Check(RSRotProbe(last,0,nullptr,0,nullptr,0,nullptr)==0 && !gRotLocked,
		"failed final probe does not advertise a camera lock");

	// Replay the failure pattern from build 236 through the production scan
	// and exported probes. Drawing matrices can change correctly without
	// having the trace predicted by the reported TransformGet Euler angles.
	Reset(); gRotLocked=false;
	std::array<std::array<float,16>,63> copies;
	for(auto& values:copies) { values.fill(1000); Matrix(values.data(),25); }
	const double seed[3]={0,25,0};
	{
		RotScanJob scan(seed);
		scan.Piece(0,reinterpret_cast<uintptr_t>(copies.data()),
			reinterpret_cast<const unsigned char*>(copies.data()),sizeof copies,sizeof copies);
		scan.Finish(sizeof copies,GetTickCount());
	}
	Check(gRotCands.size()==63,"initial scanner finds 63 seeded rotation copies");
	for(int i=0;i<9;++i) Matrix(copies[i].data(),70);
	char nextWithFrame[]="next 31.690 -40.660 13.596";
	Check(RSRotProbe(nextWithFrame,0,nullptr,0,nullptr,0,nullptr)==9,
		"63-candidate regression: nine changing copies survive nonmatching Euler trace");
	for(int i=0;i<9;++i) Matrix(copies[i].data(),0);
	char lastWithFrame[]="last -8.310 -10.660 3.598";
	Check(RSRotProbe(lastWithFrame,0,nullptr,0,nullptr,0,nullptr)==9 && gRotLocked,
		"63-candidate regression: final probe locks all nine live copies");
	gActiveView=-2;
	ViewTickCodes(RotCodes());
	Check(gActiveView==0,"63-candidate regression: Front reference activates");
	for(int i=0;i<9;++i) Matrix(copies[i].data(),-90);
	ViewTickCodes(RotCodes());
	Check(gActiveView==2 && gRotLocked,"63-candidate regression: Right reference activates");
	printf("%d failures\n",failures);
	return failures?1:0;
}
