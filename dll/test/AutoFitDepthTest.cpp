#include "../ReferenceScaler.cpp"
static int failures=0;
static void Check(bool ok,const char* text) { printf("%s %s\n",ok?"PASS":"FAIL",text); failures+=!ok; }
int main(int argc,char** argv) {
	if(argc<2) return 2;
	gTesting=true; gDataFolder=Widen(argv[1]); if(gDataFolder.back()!=L'\\') gDataFolder+=L'\\';
	gProjectDir=gDataFolder; gProjectTried=gViewsLoaded=true; EnsureLock();
	const int w=640,h=480;
	gKeyW=w; gKeyH=h; gKeyShare.assign(size_t(w)*h,255);
	std::vector<float> plane(size_t(w)*h,32608);
	for(int y=0;y<h;++y) for(int x=0;x<w;++x)
		if(std::pow((x+0.5-350)/100,2)+std::pow((y+0.5-240)/150,2)<1) {
			plane[y*w+x]=-100; gKeyShare[y*w+x]=0;
		}
	const auto cleanCapture=gKeyShare;
	const auto cleanPlane=plane;
	gDepth={}; gDepth.ok=true; gDepth.base=reinterpret_cast<uintptr_t>(plane.data());
	gDepth.w=w; gDepth.h=h; gDepth.pitch=w*4; gDepth.stride=4; gDepth.scale=1; gDepth.bg=32608;
	gOverlay={}; gOverlay.docW=w; gOverlay.docH=h;
	gOverlay.ax=gOverlay.px=w/2; gOverlay.ay=gOverlay.py=h/2; gOverlay.visible=true;
	gActiveView=0; gViewAway=false; gViews[0].used=true;
	Check(AutoFitDepthReady(gOverlay),"depth validates on a clear canvas");
	Check(AutoFitDepthEvidenceMatches(gOverlay),"clear-canvas depth evidence is remembered for the current view");
	View grid; grid.gw=400; grid.gh=300; grid.cell=3.2; grid.u0=-320; grid.v0=-240;
	std::vector<unsigned char> baseline,behindPalette;
	SampleSilhouette(gOverlay,0,0,grid,baseline,true);
	View savedOutline=grid; savedOutline.mask=baseline;
	auto cropped=baseline;
	for(int y=0;y<125;++y) for(int x=0;x<grid.gw;++x) cropped[y*grid.gw+x]=0;
	Check(CompleteFitSilhouette(grid,savedOutline,cropped)>0 && cropped==baseline,
		"off-screen outline is completed from a matching saved view");
	View wrong=savedOutline;
	for(int y=0;y<grid.gh;++y) for(int x=0;x<grid.gw;++x)
		wrong.mask[y*grid.gw+x]=x>=80?baseline[y*grid.gw+x-80]:1;
	cropped=baseline;
	for(int y=0;y<125;++y) for(int x=0;x<grid.gw;++x) cropped[y*grid.gw+x]=0;
	const auto beforeCompletion=cropped;
	Check(CompleteFitSilhouette(grid,wrong,cropped)==0 && cropped==beforeCompletion,
		"an unrelated saved outline cannot replace cropped depth");
	gMenuBoxes.push_back({0,0,336,h});
	for(int y=0;y<h;++y) for(int x=0;x<336;++x) gKeyShare[y*w+x]=0;
	const auto samples=DepthSamples(gOverlay,600);
	bool unobscured=!samples.empty();
	for(const auto& sample:samples) unobscured=unobscured && sample.x>=352;
	Check(unobscured,"depth validation excludes JRef palette pixels and their margin");
	gDepth.ok=false;
	Check(AutoFitDepthReady(gOverlay) && gDepth.ok,"button immediately recovers a good plane invalidated by the palette");
	SampleSilhouette(gOverlay,0,0,grid,behindPalette,true);
	Check(behindPalette==baseline,"Auto Fit sees the complete model silhouette behind the palette");
	// The actual build-239 failure: menu detection reports no palette at all
	// while its pixels are still included in the canvas capture.
	gMenuBoxes.clear(); gDepth.ok=false;
	Check(AutoFitDepthReady(gOverlay),"valid depth survives a palette missed entirely by menu detection");
	OverlayState shifted=gOverlay; shifted.px+=40;
	Check(!AutoFitDepthEvidenceMatches(shifted),"cached evidence is not reused after a camera pan");
	shifted=gOverlay; shifted.ratio*=1.2;
	Check(!AutoFitDepthEvidenceMatches(shifted),"cached evidence is not reused after a camera zoom");
	gActiveView=2;
	Check(!AutoFitDepthEvidenceMatches(gOverlay),"cached evidence is not reused for a different model view");
	gActiveView=0;
	// Run the full production fitting path, including photo load, depth
	// validation, registration, applying placement and saving the undo step.
	StartGdiplus();
	Gdiplus::Bitmap photo(w,h,PixelFormat24bppRGB);
	{
		Gdiplus::Graphics draw(&photo); draw.Clear(Gdiplus::Color(255,255,255));
		Gdiplus::SolidBrush ink(Gdiplus::Color(30,30,30));
		draw.FillEllipse(&ink,250,90,200,300);
	}
	Check(WritePng(gDataFolder+L"autofit-depth-photo.png",photo),"synthetic reference saved in test scratch folder");
	gImgW=w; gImgH=h; gOverlayPath="autofit-depth-photo.png";
	gActiveView=0; gViewAway=false; gMode=0;
	gViews[0].used=true; gViews[0].path=gOverlayPath;
	gOverlay.imgSX=gOverlay.imgSY=0.8; gOverlay.imgX=40; gOverlay.imgY=-20;
	gDepth.ok=false;
	const auto before=AdjustNow();
	const int fit=AutoFitOnOverlay();
	if(fit!=1) printf("AutoFitOnOverlay returned %d\n",fit);
	Check(fit==1,"full Auto Fit succeeds with an undetected JRef palette covering the model");
	Check(fit==1 && fabs(gOverlay.imgSX-1)<0.02 && fabs(gOverlay.imgX)<3 && fabs(gOverlay.imgY)<3,
		"full Auto Fit applies the recovered photo placement");
	Check(!gAdjustUndo.empty() && gAdjustUndo.back()==before,"full Auto Fit keeps the previous placement as one undo step");
	// A genuinely empty or stale plane must still fail, rather than bypass
	// depth validation just because a menu is open.
	std::fill(plane.begin(),plane.end(),32608.0f);
	gDepth.ok=false;
	Check(!AutoFitDepthReady(gOverlay) && !gDepth.ok,"empty depth is rejected and its state preserved");
	plane=cleanPlane;
	for(int y=0;y<h;++y) for(int x=0;x<w;++x)
		plane[y*w+x]=x>=150?cleanPlane[y*w+x-150]:32608;
	gDepth.base=reinterpret_cast<uintptr_t>(plane.data());
	Check(!AutoFitDepthReady(gOverlay),"depth from a different camera position is rejected");
	gDepth.base=1;
	Check(!AutoFitDepthReady(gOverlay),"unreadable depth is rejected safely");
	if(argc>=5 && atoi(argv[3])>0 && atoi(argv[4])>0) {
		const int fw=atoi(argv[3]),fh=atoi(argv[4]),dw=fw*2,dh=fh*2;
		std::vector<float> saved(size_t(fw)*fh),full(size_t(dw)*dh);
		FILE* raw=_wfopen(Widen(argv[2]).c_str(),L"rb");
		const bool loaded=raw && fread(saved.data(),4,saved.size(),raw)==saved.size();
		if(raw) fclose(raw);
		Check(loaded,"saved ZBrush depth capture loaded read-only");
		if(loaded) {
			gKeyW=dw; gKeyH=dh; gKeyShare.resize(full.size());
			for(int y=0;y<dh;++y) for(int x=0;x<dw;++x) {
				full[y*dw+x]=saved[(y/2)*fw+x/2];
				gKeyShare[y*dw+x]=full[y*dw+x]==32608?255:0;
			}
			gAutoFitDepthEvidence={}; gMenuBoxes.clear();
			gOverlay={}; gOverlay.docW=dw; gOverlay.docH=dh;
			gActiveView=0; gViewAway=false;
			gDepth={}; gDepth.base=reinterpret_cast<uintptr_t>(full.data());
			gDepth.w=dw; gDepth.h=dh; gDepth.pitch=dw*4; gDepth.stride=4; gDepth.scale=1; gDepth.bg=32608;
			Check(AutoFitDepthReady(gOverlay),"real saved depth establishes validated camera evidence");
			for(int y=0;y<dh;++y) for(int x=0;x<336;++x) gKeyShare[y*dw+x]=0;
			gDepth.ok=false;
			Check(AutoFitDepthReady(gOverlay),"real saved depth recovers with an unrecognised palette over the capture");
			if(argc>=7 && argv[5][0] && argv[6][0]) {
				View stored;
				FILE* shape=_wfopen(Widen(argv[5]).c_str(),L"rb");
				bool shapeOK=shape && fscanf(shape,"%d %d %lf %lf %lf",&stored.gw,&stored.gh,&stored.cell,&stored.u0,&stored.v0)==5 && fgetc(shape)=='\n';
				if(shapeOK) {
					stored.mask.resize(size_t(stored.gw)*stored.gh);
					shapeOK=fread(stored.mask.data(),1,stored.mask.size(),shape)==stored.mask.size();
				}
				if(shape) fclose(shape);
				Check(shapeOK,"actual saved Front outline loaded read-only");
				Rgb reference;
				const bool imageOK=LoadRgb(Widen(argv[6]),reference);
				Check(imageOK,"actual Front photo loaded read-only");
				if(shapeOK && imageOK) {
					gOverlay.visible=true;
					std::string metadata=argv[2]; metadata.replace(metadata.size()-4,4,".txt");
					FILE* meta=_wfopen(Widen(metadata.c_str()).c_str(),L"rb");
					bool cameraRead=false;
					if(meta) {
						char line[256];
						while(fgets(line,sizeof line,meta)) {
							if(sscanf(line,"pos %lf %lf",&gOverlay.px,&gOverlay.py)==2) cameraRead=true;
							sscanf(line,"zoom %lf",&gOverlay.ratio);
						}
						fclose(meta);
					}
					Check(cameraRead,"actual captured camera position read from its metadata");
					gOverlay.ax=536.34082; gOverlay.ay=784.979126;
					gViews[0]=stored; gViews[0].used=true; gViews[0].path=argv[6];
					gImgW=reference.w; gImgH=reference.h; gOverlayPath=argv[6];
					gAutoFitDepthEvidence={};
					for(size_t i=0;i<full.size();++i) gKeyShare[i]=full[i]==32608?255:0;
					Check(AutoFitDepthReady(gOverlay),"actual captured camera is revalidated before fitting");
					for(int y=0;y<dh;++y) for(int x=0;x<336;++x) gKeyShare[y*dw+x]=0;
					gDepth.ok=false;
					const int result=AutoFitOnOverlay();
					printf("Actual reference fit result %d, width %.3f%%, x %.3f, y %.3f, turn %.3f\n",result,
						100*gOverlay.imgSX,gOverlay.imgX,gOverlay.imgY,gOverlay.imgRot);
					Check(result==1,"full Auto Fit succeeds on actual cropped ZBrush depth, saved outline and Front photo");
					Check(result==1 && gOverlay.imgSX>1.2 && gOverlay.imgSX<1.45 && gOverlay.imgY<-35 &&
						gOverlay.imgY>-100 && gOverlay.imgRot>1 && gOverlay.imgRot<8,
						"actual reference fit stays near the earlier manual alignment rather than the bad silhouette fit");
					Check(result==1 && fabs(gOverlay.imgSX*100-129.4734)<.5 && fabs(gOverlay.imgX-20.87)<1 &&
						fabs(gOverlay.imgY+67.90)<1 && fabs(gOverlay.imgRot-4.978)<.3,
						"actual reference matches the benchmark within 0.5 percent scale, 1 pixel and 0.3 degree");
					const auto canonical=AdjustNow();
					gOverlay.offX=287.35; gOverlay.offY=340.80; gOverlay.fix=1.0123;
					gOverlay.imgSX=canonical.sx/gOverlay.fix; gOverlay.imgSY=canonical.sy/gOverlay.fix;
					gOverlay.imgX=gOverlay.ax+gOverlay.offX+(dw*.5+canonical.x-gOverlay.ax)/gOverlay.fix-dw*.5;
					gOverlay.imgY=gOverlay.ay+gOverlay.offY+(dh*.5+canonical.y-gOverlay.ay)/gOverlay.fix-dh*.5;
					const int compensated=AutoFitOnOverlay();
					printf("Compensated reference fit result %d, width %.3f%%, x %.3f, y %.3f, turn %.3f\n",compensated,
						100*gOverlay.imgSX,gOverlay.imgX,gOverlay.imgY,gOverlay.imgRot);
					Check(compensated==1 && fabs(gOverlay.imgSX-canonical.sx)<.002 && fabs(gOverlay.imgX-canonical.x)<.3 &&
						fabs(gOverlay.imgY-canonical.y)<.3 && fabs(gOverlay.imgRot-canonical.rot)<.15 &&
						gOverlay.offX==0 && gOverlay.offY==0 && gOverlay.fix==1,
						"full Auto Fit with the logged large offset produces the same canonical placement");
					const auto fitted=AdjustNow();
					gViewAway=true; ViewTickCodes({kViewCodes[0]});
					Check(AdjustNow()==fitted && gOverlay.offX==0 && gOverlay.offY==0 && gOverlay.fix==1,
						"full fitted placement survives a same-view snap unchanged");
				}
			}
		}
	}
	printf("%d failures\n",failures);
	return failures?1:0;
}
