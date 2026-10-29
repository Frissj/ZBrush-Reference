// Dependency-free 2D similarity registration. Coordinates are image pixels;
// only candidate scoring runs concurrently, so reductions are deterministic.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <ppl.h>

namespace auto_fit {
struct Point { double x, y; };
struct Shape {
    int w = 0, h = 0, area = 0;
    double cx = 0, cy = 0, span = 0;
    std::vector<unsigned char> mask;
    std::vector<float> distance;
    std::vector<Point> edge;
};

// Keep the largest connected foreground, then fill its enclosed holes.
inline void Clean(std::vector<unsigned char>& m, int w, int h) {
    std::vector<unsigned char> seen(m.size(), 0);
    std::vector<int> best, queue;
    auto flood = [&](int start, bool foreground) {
        queue.clear(); queue.push_back(start); seen[start] = 1;
        for (size_t j = 0; j < queue.size(); ++j) {
            const int i = queue[j], x = i % w, y = i / w;
            const int neighbours[] = {x ? i-1 : -1, x+1<w ? i+1 : -1,
                                     y ? i-w : -1, y+1<h ? i+w : -1};
            for (int n : neighbours)
                if (n >= 0 && !seen[n] && bool(m[n]) == foreground) {
                    seen[n] = 1; queue.push_back(n);
                }
        }
    };
    for (int i = 0; i < static_cast<int>(m.size()); ++i)
        if (m[i] && !seen[i]) {
            flood(i, true);
            if (queue.size() > best.size()) best = queue;
        }
    std::fill(m.begin(), m.end(), 0);
    for (int i : best) m[i] = 1;
    std::fill(seen.begin(), seen.end(), 0);
    for (int i = 0; i < static_cast<int>(m.size()); ++i)
        if (!m[i] && !seen[i] && (i<w || i>=w*(h-1) || i%w==0 || i%w==w-1))
            flood(i, false);
    for (size_t i = 0; i < m.size(); ++i) if (!seen[i]) m[i] = 1;
}

inline bool Prepare(Shape& s) {
    if (s.w < 8 || s.h < 8 || s.mask.size() != size_t(s.w)*s.h) return false;
    s.area = 0; s.edge.clear();
    int x0=s.w, y0=s.h, x1=0, y1=0;
    s.distance.assign(s.mask.size(), 1e6f);
    for (int y=0; y<s.h; ++y) for (int x=0; x<s.w; ++x) {
        const int i=y*s.w+x;
        if (!s.mask[i]) continue;
        ++s.area; x0=std::min(x0,x); x1=std::max(x1,x);
        y0=std::min(y0,y); y1=std::max(y1,y);
        if (!x || !y || x==s.w-1 || y==s.h-1 || !s.mask[i-1] ||
            !s.mask[i+1] || !s.mask[i-s.w] || !s.mask[i+s.w]) {
            s.edge.push_back({double(x),double(y)}); s.distance[i]=0;
        }
    }
    if (s.area < 80 || s.area > s.mask.size()*0.90 || s.edge.size()<30) return false;
    s.cx=(x0+x1)*0.5; s.cy=(y0+y1)*0.5;
    s.span=std::hypot(double(x1-x0+1),double(y1-y0+1));
    // Chamfer distance, including diagonals. Retain the full field but bound
    // scoring work independently of image resolution.
    auto relax = [&](int x,int y,int dx,int dy,float cost) {
        const int xx=x+dx, yy=y+dy;
        if (xx>=0 && yy>=0 && xx<s.w && yy<s.h)
            s.distance[y*s.w+x]=std::min(s.distance[y*s.w+x],s.distance[yy*s.w+xx]+cost);
    };
    for (int y=0;y<s.h;++y) for(int x=0;x<s.w;++x) {
        relax(x,y,-1,0,1); relax(x,y,0,-1,1);
        relax(x,y,-1,-1,1.414214f); relax(x,y,1,-1,1.414214f);
    }
    for (int y=s.h-1;y>=0;--y) for(int x=s.w-1;x>=0;--x) {
        relax(x,y,1,0,1); relax(x,y,0,1,1);
        relax(x,y,-1,1,1.414214f); relax(x,y,1,1,1.414214f);
    }
    if (s.edge.size()>600) {
        std::vector<Point> sample;
        for(size_t i=0;i<600;++i) sample.push_back(s.edge[i*s.edge.size()/600]);
        s.edge.swap(sample);
    }
    return true;
}

// Fit a smooth background to the upper border. Portraits commonly have
// clothes touching the bottom and a lighting gradient across the background;
// neither should be treated as evidence against a separable background.
inline bool Photo(const std::vector<unsigned char>& rgb, int w, int h, Shape& s) {
    if (w<8 || h<8 || rgb.size()!=size_t(w)*h*3) return false;
    std::vector<Point> border;
    for(int y=0;y<h/2;++y) for(int x=0;x<w;++x)
        if(x<2 || y<2 || x>=w-2) border.push_back({double(x),double(y)});
    std::vector<unsigned char> keep(border.size(),1);
    double bg[3][3]={};
    auto residual = [&](int x,int y) {
        const double p[3]={1,double(x)/w-0.5,double(y)/h-0.5};
        double d=0;
        for(int c=0;c<3;++c) {
            double predicted=0;
            for(int k=0;k<3;++k) predicted+=bg[k][c]*p[k];
            const double v=rgb[(y*w+x)*3+c]-std::clamp(predicted,0.0,255.0);
            d+=v*v;
        }
        return d;
    };
    for(int pass=0;pass<3;++pass) {
        double normal[3][6]={}; int count=0;
        for(size_t i=0;i<border.size();++i) if(keep[i]) {
            const int x=int(border[i].x),y=int(border[i].y);
            const double p[3]={1,double(x)/w-0.5,double(y)/h-0.5};
            ++count;
            for(int r=0;r<3;++r) {
                for(int c=0;c<3;++c) normal[r][c]+=p[r]*p[c];
                for(int c=0;c<3;++c) normal[r][c+3]+=p[r]*rgb[(y*w+x)*3+c];
            }
        }
        if(count<12) return false;
        for(int k=0;k<3;++k) {
            int pivot=k;
            for(int r=k+1;r<3;++r) if(std::abs(normal[r][k])>std::abs(normal[pivot][k])) pivot=r;
            if(std::abs(normal[pivot][k])<1e-9) return false;
            for(int c=0;c<6;++c) std::swap(normal[k][c],normal[pivot][c]);
            const double divisor=normal[k][k];
            for(int c=k;c<6;++c) normal[k][c]/=divisor;
            for(int r=0;r<3;++r) if(r!=k) {
                const double f=normal[r][k];
                for(int c=k;c<6;++c) normal[r][c]-=f*normal[k][c];
            }
        }
        for(int r=0;r<3;++r) for(int c=0;c<3;++c) bg[r][c]=normal[r][c+3];
        for(size_t i=0;i<border.size();++i)
            keep[i]=residual(int(border[i].x),int(border[i].y))<60*60;
    }
    int agree=0;
    for(Point p:border) agree+=residual(int(p.x),int(p.y))<42*42;
    if(agree<border.size()*0.60) return false;
    s.w=w; s.h=h; s.mask.resize(size_t(w)*h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) s.mask[y*w+x]=residual(x,y)>42*42;
    Clean(s.mask,w,h);
    return Prepare(s);
}

struct Fit { double scale=1, angle=0, x=0, y=0, error=1e9; };
inline double Distance(const Shape& s, double x,double y) {
    if(x<0 || y<0 || x>s.w-1 || y>s.h-1) return s.span*0.3;
    const int xx=int(x), yy=int(y), x1=std::min(xx+1,s.w-1), y1=std::min(yy+1,s.h-1);
    const double fx=x-xx, fy=y-yy;
    return (1-fy)*((1-fx)*s.distance[yy*s.w+xx]+fx*s.distance[yy*s.w+x1])+
        fy*((1-fx)*s.distance[y1*s.w+xx]+fx*s.distance[y1*s.w+x1]);
}
inline double Score(const Shape& photo,const Shape& model,const Fit& f) {
    if(f.scale<=0) return 1e9;
    const double c=cos(f.angle), s=sin(f.angle);
    std::vector<double> forward,backward;
    forward.reserve(photo.edge.size()); backward.reserve(model.edge.size());
    for(Point p:photo.edge) {
        const double x=(p.x-photo.cx)*f.scale,y=(p.y-photo.cy)*f.scale;
        forward.push_back(std::min(Distance(model,f.x+c*x-s*y,f.y+s*x+c*y)/model.span,0.15));
    }
    for(Point p:model.edge) {
        const double x=p.x-f.x,y=p.y-f.y;
        backward.push_back(std::min(Distance(photo,photo.cx+(c*x+s*y)/f.scale,
                    photo.cy+(-s*x+c*y)/f.scale)*f.scale/model.span,0.15));
    }
    // Trim the worst 20% of correspondences, as in the supplied ICP.
    // Clothing, hair and the bottom crop are not all shared by a sculpt.
    auto trimmed = [](std::vector<double>& values) {
        const size_t n=std::max(size_t(1),size_t(std::ceil(values.size()*0.8)));
        if(n<values.size()) std::nth_element(values.begin(),values.begin()+n,values.end());
        double sum=0; for(size_t i=0;i<n;++i) sum+=values[i];
        return sum/n;
    };
    // Area constrains collapsed fits and mismatched outlines.
    return 0.5*(trimmed(forward)+trimmed(backward))+
        0.025*std::abs(log(f.scale*f.scale*photo.area/model.area));
}
inline Fit Search(const Shape& photo,const Shape& model,const Fit* initial=nullptr) {
    Fit best; best.scale=model.span/photo.span; best.x=model.cx; best.y=model.cy;
    best.error=Score(photo,model,best);
    if(initial) { Fit f=*initial; f.error=Score(photo,model,f); if(f.error<best.error) best=f; }
    auto search = [&](Fit centre, int radius, double ds,double da,double dt) {
        const int n=2*radius+1;
        std::vector<Fit> candidates(size_t(n)*n*n*n);
        concurrency::parallel_for(size_t(0),candidates.size(),[&](size_t i) {
            size_t j=i;
            int dx=int(j%n)-radius; j/=n; int dy=int(j%n)-radius; j/=n;
            int a=int(j%n)-radius; j/=n; int z=int(j%n)-radius;
            Fit f=centre; f.scale*=exp(z*ds); f.angle+=a*da; f.x+=dx*dt; f.y+=dy*dt;
            f.error=Score(photo,model,f); candidates[i]=f;
        });
        for(const Fit& f:candidates) if(f.error<best.error) best=f;
    };
    Fit box; box.scale=model.span/photo.span; box.x=model.cx; box.y=model.cy;
    search(box,4,0.11,0.1308996939,model.span*0.045);
    // A useful manual placement can seed fits outside the global turn range.
    if(initial) search(*initial,2,0.06,0.065449847,model.span*0.025);
    for(int pass=0;pass<5;++pass) {
        const double step=std::pow(0.45,pass);
        search(best,2,0.035*step,0.0436332313*step,model.span*0.018*step);
    }
    return best;
}
} // namespace auto_fit
