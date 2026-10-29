#include "../ReferenceScaler.cpp"

static int failures=0;
static void Check(bool ok,const char* name) { printf("%s %s\n",ok?"PASS":"FAIL",name); failures+=!ok; }
static void Matrix(float* m,double degrees) {
	const double a=degrees*kPi/180;
	const float v[9]={float(cos(a)),0,float(-sin(a)),0,1,0,float(sin(a)),0,float(cos(a))};
	memcpy(m,v,sizeof v);
}
static RotCand Candidate(float* m) {
	RotCand c={}; c.addr=reinterpret_cast<uintptr_t>(m); c.stride=3; c.unit=true; c.rel=0;
	for(int i=0;i<9;++i) c.m[i]=c.settled[i]=c.m1[i]=c.m2[i]=m[i];
	return c;
}
struct Camera { double scale,x,y; };
static MemTriplet Triplet(Camera& camera) {
	MemTriplet t={};
	t.s={reinterpret_cast<uintptr_t>(&camera.scale),true,camera.scale,0};
	t.x={reinterpret_cast<uintptr_t>(&camera.x),true,camera.x,0};
	t.y={reinterpret_cast<uintptr_t>(&camera.y),true,camera.y,0};
	return t;
}
static std::array<double,4> Layout() {
	double x,y,w,h;
	if(!ImageRectFor(gOverlay,512,768,x,y,w,h)) return {};
	const auto& st=gOverlay;
	return {((x+w*0.5)/st.pixelsPerDoc-st.px)/st.ratio,
		((y+h*0.5)/st.pixelsPerDoc-st.py)/st.ratio,
		w/st.pixelsPerDoc/st.ratio,h/st.pixelsPerDoc/st.ratio};
}
int main(int argc,char** argv) {
	if(argc!=2) return 2;
	gTesting=true; gDataFolder=Widen(argv[1]); if(gDataFolder.back()!=L'\\') gDataFolder+=L'\\';
	gProjectDir=gDataFolder; gProjectTried=gViewsLoaded=true; EnsureLock();
	gRotLocked=gRotSettled=gTurnKnown=true; gTurning=false;
	gMemLocked=true; gMemWatchUntil=0; gAnchorScale=40.834;
	gOverlay={}; gOverlay.docW=1019; gOverlay.docH=776; gOverlay.visible=true;
	gOverlay.pixelsPerDoc=1.5; gOverlay.px=274.594; gOverlay.py=753.61;
	gOverlay.ax=536.34; gOverlay.ay=784.98;
	gOverlay.imgSX=1.224748; gOverlay.imgSY=1.274748;
	gOverlay.imgX=114.724; gOverlay.imgY=-87.769; gOverlay.imgRot=-4.1757;
	gOverlay.offX=12; gOverlay.offY=-9;
	const auto expected=Layout();
	Camera camera={gAnchorScale,gOverlay.px,gOverlay.py};
	gTriplets={Triplet(camera)}; gTripletInUse=0;
	gGesture={}; gGesture.active=gGesture.right=gGesture.rotated=true;
	gGesture.mode=kDragNone; gButtonsDown=1; gLastButtonAt=GetTickCount();
	gRotWatchUntil=GetTickCount()+1500;
	std::array<std::array<float,9>,10> matrices;
	for(auto& m:matrices) { Matrix(m.data(),-90); gRotCands.push_back(Candidate(m.data())); }
	// Exact failure: one navigation-head copy changes to Front; the nine
	// camera copies remain Right, while an unclassified right drag zooms.
	Matrix(matrices[1].data(),0);
	RotFollow();
	Check(!gTurning && gRotSettled,"one stray Front copy cannot turn a Right camera");
	const double scales[]={39.9,25,10.6477,10.6948,20.9338,17.4598,51.3999,99.1757,120.0776,27.7311,20.4043,40.834};
	bool layoutOK=true,offsetOK=true,scaleOK=true;
	for(int cycle=0;cycle<6;++cycle) for(double scale:scales) {
		camera.scale=scale;
		camera.x=600+(274.594-600)*(scale/gAnchorScale);
		camera.y=190+(753.61-190)*(scale/gAnchorScale);
		// A large discontinuity requires a second read to reject transient
		// camera memory. The ordinary overlay supplies these successive frames.
		for(int frame=0;frame<3;++frame) { RotFollow(); MemFollow(); }
		const auto actual=Layout();
		for(size_t i=0;i<actual.size();++i) layoutOK=layoutOK && fabs(actual[i]-expected[i])<1e-7;
		offsetOK=offsetOK && gOverlay.offX==12 && gOverlay.offY==-9;
		scaleOK=scaleOK && fabs(gOverlay.ratio-scale/gAnchorScale)<1e-9 && fabs(gOverlay.fix-1)<1e-9;
	}
	Check(layoutOK,"72 zoom changes preserve image alignment in camera coordinates");
	Check(offsetOK,"zoom does not accumulate rotation offsets");
	Check(scaleOK,"image follows live scale throughout repeated zooms");
	Check(gGesture.observedZoom && gTriplets[0].scaleFrames>0,"zoom is recognised without modifier-key classification");
	bool returned=fabs(gOverlay.ratio-1)<1e-9;
	const auto final=Layout();
	for(size_t i=0;i<final.size();++i) returned=returned && fabs(final[i]-expected[i])<1e-7;
	Check(returned,"zooming back to the original scale is reversible");
	// A genuine pan can jitter the reported scale. Preserve the existing
	// scale hold instead of relabelling every small fluctuation as a zoom.
	gGesture.mode=kDragMove; camera.scale=gAnchorScale*1.01; camera.x+=10; camera.y-=8;
	RotFollow(); MemFollow();
	Check(fabs(gOverlay.ratio*gOverlay.fix-1)<1e-9 && gOverlay.offX==12 && gOverlay.offY==-9,
		"ordinary pan retains image size despite camera scale jitter");
	gGesture.active=false; camera={gAnchorScale,274.594,753.61};
	RotFollow(); MemFollow();
	Check(fabs(gOverlay.fix-1)<1e-9,"pan scale hold releases without cumulative size changes");
	gGesture.active=true; gGesture.mode=kDragNone;
	// A real turn updates three live copies before seven cached copies catch
	// up. Full-matrix agreement must preserve that behaviour.
	for(int i=0;i<3;++i) Matrix(matrices[i].data(),-72);
	RotFollow();
	Check(gTurning && !gRotSettled,"three live turning copies beat stale snapped copies");
	camera.x+=30; camera.y-=20; MemFollow();
	Check(fabs(gOverlay.offX-42)<1e-9 && fabs(gOverlay.offY+29)<1e-9,
		"confirmed real turn still applies rotation compensation");
	// A pause in a drag must not leave turning latched until mouse-up.
	gRotChangedAt=GetTickCount()-250;
	RotFollow();
	Check(!gTurning && gRotSettled,"rotation settles while the mouse button remains down");
	// A turn can leave and return to the same view faster than the settling
	// delay. Production used to miss that departure and retain its offsets.
	gActiveView=2; gViewAway=false;
	gViews[2].used=true; gViews[2].scale=gAnchorScale;
	gViews[2].ax=gOverlay.ax; gViews[2].ay=gOverlay.ay; gViews[2].path="Right.jpg";
	for(auto& m:matrices) Matrix(m.data(),-90);
	RotFollow(); gRotChangedAt=GetTickCount()-250; RotFollow();
	gViewAway=false;
	for(auto& m:matrices) Matrix(m.data(),-65);
	RotFollow();
	Check(gViewAway && !gRotSettled,"departure is remembered before the 200 ms settling delay");
	camera.x+=55; camera.y-=43; MemFollow();
	for(auto& m:matrices) Matrix(m.data(),-90);
	RotFollow(); gRotChangedAt=GetTickCount()-250; RotFollow();
	Check(!gViewAway && gOverlay.offX==0 && gOverlay.offY==0 && gOverlay.fix==1 && !gImageScaleHold.on,
		"quick same-view snap clears navigation offsets and the scale hold");

	// Replay the build-245 failure: fit/adjust while a large temporary
	// translation is present, then snap, undo, redo and zoom repeatedly.
	gGesture={}; gViewAway=false;
	gOverlay.offX=287.35; gOverlay.offY=340.80; gOverlay.fix=1.0123;
	gOverlay.imgSX=1.29; gOverlay.imgSY=1.31;
	gOverlay.imgX=305.24225; gOverlay.imgY=274.84433; gOverlay.imgRot=4.952;
	gAdjustUndoView=2; gAdjustUndo.clear(); gAdjustRedo.clear();
	const auto rawBefore=AdjustNow();
	gOverlay.imgX-=7; gOverlay.imgY+=5;
	const auto undoLayout=Layout();
	gOverlay.imgX+=7; gOverlay.imgY-=5;
	auto undo=rawBefore; undo.x-=7; undo.y+=5;
	gAdjustUndo.push_back(undo);
	const auto adjusted=Layout();
	gImageScaleHold={true,.8,1.2};
	SaveAdjustToView();
	auto same=[](const auto& a,const auto& b) { for(size_t i=0;i<a.size();++i) if(fabs(a[i]-b[i])>1e-8) return false; return true; };
	Check(same(adjusted,Layout()) && gOverlay.offX==0 && gOverlay.offY==0 && gOverlay.fix==1 && !gImageScaleHold.on,
		"saving a compensated fit preserves its complete displayed rectangle");
	gViewAway=true; ViewTickCodes({kViewCodes[2]});
	Check(same(adjusted,Layout()),"saved fit remains aligned after snapping to its view");
	Check(AdjustUndo(false) && same(undoLayout,Layout()),"undo preserves its placement across save and snap");
	Check(AdjustUndo(true) && same(adjusted,Layout()),"redo restores the fitted placement across save and snap");
	bool snappedZoomOK=true;
	for(double scale:scales) {
		camera.scale=scale; gGesture.active=true; gGesture.mode=kDragScale;
		for(int frame=0;frame<3;++frame) MemFollow();
		snappedZoomOK=snappedZoomOK && same(adjusted,Layout());
		gViewAway=true; ViewTickCodes({kViewCodes[2]});
		snappedZoomOK=snappedZoomOK && same(adjusted,Layout());
	}
	Check(snappedZoomOK,"repeated zoom and same-view snaps preserve the fitted placement");
	printf("%d failures\n",failures);
	return failures?1:0;
}
