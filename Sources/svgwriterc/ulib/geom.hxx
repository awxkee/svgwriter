#pragma once

// from https://github.com/sgorsten/linalg
//#include "linalg.h"
#include <cmath>
#include <algorithm>  // std::min etc.
#include <vector>
#include <float.h>
#include <limits.h>

// for most use cases, float would probably be fine
typedef double real;

#define NaN real(NAN)
#define REAL_MAX  real(FLT_MAX)
// OK for IEEE-754, but not fixed SVGPoint, etc
#define REAL_MIN  real(-FLT_MAX)

constexpr real degToRad(real deg) {
    return (deg*M_PI/180);
}

// floating SVGPoint comparison; we should also support relative and units-in-last-place compare
// see: https://randomascii.wordpress.com/2012/02/25/comparing-floating-SVGPoint-numbers-2012-edition/
inline bool approxEq(real a, real b, real eps) { return fabs(a - b) < eps; }

inline real quantize(real a, real d) { return std::round(a/d)*d; }

class SVGPoint {
public:
    real x,y;
    SVGPoint(real _x, real _y) : x(_x), y(_y) {}
    SVGPoint() : SVGPoint(0,0) {}
    SVGPoint& translate(real dx, real dy) { x += dx; y += dy; return *this; }
    real dist(const SVGPoint& p) const { real dx = x - p.x; real dy = y - p.y; return std::sqrt(dx*dx + dy*dy); }
    real dist() const { return std::sqrt(x*x + y*y); }
    real dist2() const { return x*x + y*y; }
    bool isZero() const { return x == 0 && y == 0; }
    bool isNaN() const { return std::isnan(x) || std::isnan(y); }
    SVGPoint& normalize() { if(x != 0 || y != 0) { real d = dist(); x /= d; y /= d; } return *this; }

    SVGPoint& operator+=(const SVGPoint& p) { x += p.x; y += p.y; return *this; }
    SVGPoint& operator-=(const SVGPoint& p) { x -= p.x; y -= p.y; return *this; }
    SVGPoint& operator*=(real a) { x *= a; y *= a; return *this; }
    SVGPoint& operator/=(real a) { x /= a; y /= a; return *this; }
    SVGPoint& neg() { x = -x; y = -y; return *this; }
    SVGPoint operator-() const { return SVGPoint(*this).neg(); }
    friend bool operator==(const SVGPoint& p1, const SVGPoint& p2) { return p1.x == p2.x && p1.y == p2.y; }
    friend bool operator!=(const SVGPoint& p1, const SVGPoint& p2) { return !(p1 == p2); }
    friend bool approxEq(const SVGPoint& p1, const SVGPoint& p2, real eps)
    { return approxEq(p1.x, p2.x, eps) && approxEq(p1.y, p2.y, eps); }
    friend SVGPoint operator+(SVGPoint p1, const SVGPoint& p2) { return p1 += p2; }
    friend SVGPoint operator-(SVGPoint p1, const SVGPoint& p2) { return p1 -= p2; }
    friend SVGPoint operator*(SVGPoint p1, real a) { return p1 *= a; }
    friend SVGPoint operator/(SVGPoint p1, real a) { return p1 /= a; }
    friend SVGPoint operator*(real a, SVGPoint p1) { return p1 *= a; }
    //friend SVGPoint operator/(Dim a, SVGPoint p1) { return p1 /= a; }

    friend real dot(const SVGPoint& p1, const SVGPoint& p2) { return p1.x*p2.x + p1.y*p2.y; }
    friend real cross(const SVGPoint& a, const SVGPoint& b) { return a.x*b.y - a.y*b.x; }
    friend SVGPoint normalize(const SVGPoint& v) { return SVGPoint(-v.y, v.x).normalize(); }  // +90 deg (CCW) rotation
};

// should we do left -> left() etc.?
struct SVGRect
{
    real left, top, right, bottom;

    SVGRect() : SVGRect(REAL_MAX, REAL_MAX, REAL_MIN, REAL_MIN) {}
    static SVGRect ltrb(real l, real t, real r, real b) { return SVGRect(l, t, r, b); }
    static SVGRect ltwh(real l, real t, real w, real h) { return SVGRect(l, t, l+w, t+h); }
    static SVGRect wh(real w, real h) { return SVGRect(0, 0, w, h); }
    static SVGRect centerwh(const SVGPoint& p, real w, real h) { return SVGRect(p.x, p.y, p.x, p.y).pad(w/2, h/2); }
    static SVGRect corners(const SVGPoint& a, const SVGPoint& b) { return SVGRect().rectUnion(a).rectUnion(b); }

    SVGRect& pad(real d);
    SVGRect& pad(real dx, real dy);
    SVGRect& round();
    SVGRect& translate(real dx, real dy);
    SVGRect& translate(const SVGPoint& p);
    SVGRect& scale(real sx, real sy);
    SVGRect& scale(real s) { return scale(s, s); }
    bool contains(const SVGRect& r) const;
    bool contains(const SVGPoint& p) const;
    bool overlaps(const SVGRect& r) const;
    SVGRect& rectUnion(const SVGRect& r);
    SVGRect& rectUnion(const SVGPoint& p);
    SVGRect& rectIntersect(const SVGRect& r);
    SVGPoint center() const;
    bool isValid() const;
    real width() const;
    real height() const;
    SVGPoint origin() const { return SVGPoint(left, top); }

    void setHeight(real h) { bottom = top + h; }
    void setWidth(real w) { right = left + w; }
    bool intersects(const SVGRect& r) const { return overlaps(r); }
    SVGRect united(const SVGRect& r) const { return SVGRect(*this).rectUnion(r); }
    SVGRect toSize() const { return SVGRect::wh(width(), height()); }

    friend bool operator==(const SVGRect& a, const SVGRect& b);
    friend bool operator!=(const SVGRect& a, const SVGRect& b) { return !(a == b); }
    friend bool approxEq(const SVGRect& a, const SVGRect& b, real eps);
    SVGRect& operator*=(real a) { return scale(a); }
    SVGRect& operator/=(real a) { return scale(1/a); }
    friend SVGRect operator*(SVGRect r, real a) { return r *= a; }
    friend SVGRect operator/(SVGRect r, real a) { return r /= a; }
    friend SVGRect operator*(real a, SVGRect r) { return r *= a; }
private:
    /*SVGRect(Dim _left = MAX_X_DIM, Dim _top = MAX_Y_DIM, Dim _right = MIN_X_DIM, Dim _bottom = MIN_Y_DIM)
     : left(_left), top(_top), right(_right), bottom(_bottom) {} */
    SVGRect(real _left, real _top, real _right, real _bottom)
    : left(_left), top(_top), right(_right), bottom(_bottom) {}

};

// we previously used a 3x3 matrix from linalg.h, but 3x3 mult requires 27 scalar mults, while only 12 scalar
//  mults are actually needed for 2D transform w/ translation; linalg was dominating CPU profile
// layout is:
// [ m0 m2 m4 ] [x]
// [ m1 m3 m5 ] [y]
// [  0  0  1 ] [1]
class Transform2D
{
public:
    real m[6];  //std::array<Dim, 6> m;

    Transform2D() : m{ 1, 0, 0, 1, 0, 0 } {}
    Transform2D(real m0, real m1, real m2, real m3, real m4, real m5) : m{m0, m1, m2, m3, m4, m5} {}
    Transform2D(real* array) { for(int ii = 0; ii < 6; ++ii) m[ii] = array[ii]; }

    SVGPoint mult(const SVGPoint& p) const;
    SVGRect mult(const SVGRect& r) const;
    real xoffset() const { return m[4]; }
    real yoffset() const { return m[5]; }
    real xscale() const { return m[0]; }
    real yscale() const { return m[3]; }
    real avgScale() const;
    real* asArray() { return &m[0]; }

    SVGRect mapRect(const SVGRect& r) const { return mult(r); }
    SVGPoint map(const SVGPoint& p) const { return mult(p); }

    bool isIdentity() const { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0; }
    bool isTranslate() const { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1; }
    bool isRotating() const { return m[1] != 0 || m[2] != 0; }
    //bool isSimple() const { return m[1] == 0 && m[2] == 0; }
    Transform2D& reset() { *this = Transform2D(); return *this; }
    Transform2D inverse() const;

    Transform2D& translate(real dx, real dy) { m[4] += dx; m[5] += dy; return *this;  }
    Transform2D& translate(SVGPoint dr) { return translate(dr.x, dr.y);  }
    Transform2D& scale(real sx, real sy) { m[0] *= sx; m[1] *= sy; m[2] *= sx; m[3] *= sy; m[4] *= sx; m[5] *= sy; return *this; }
    Transform2D& scale(real s) { return scale(s, s); }
    Transform2D& rotate(real rad, SVGPoint pos = SVGPoint(0,0));
    Transform2D& shear(real sx, real sy);

    friend Transform2D operator*(const Transform2D& a, const Transform2D& b);

    friend bool operator==(const Transform2D& a, const Transform2D& b)
    {
        return a.m[0] == b.m[0] && a.m[1] == b.m[1] && a.m[2] == b.m[2]
        && a.m[3] == b.m[3] && a.m[4] == b.m[4] && a.m[5] == b.m[5];
    }
    friend bool operator!=(const Transform2D& a, const Transform2D& b) { return !(a == b); }
    friend bool approxEq(const Transform2D& a, const Transform2D& b, real eps);

    static Transform2D translating(real dx, real dy) { return Transform2D(1, 0, 0, 1, dx, dy); }
    static Transform2D translating(SVGPoint p) { return translating(p.x, p.y); }
    // Transform2D().scale() requires 6 unnecessary mults
    static Transform2D scaling(real sx, real sy) { return Transform2D(sx, 0, 0, sy, 0, 0); }
    static Transform2D scaling(real s) { return Transform2D(s, 0, 0, s, 0, 0); }
    static Transform2D rotating(real rad, SVGPoint pos = SVGPoint(0,0)) { return Transform2D().rotate(rad, pos); }
};


real calcAngle(SVGPoint a, SVGPoint b, SVGPoint c);
real distToSegment2(SVGPoint start, SVGPoint end, SVGPoint pt);
inline real distToSegment(SVGPoint start, SVGPoint end, SVGPoint pt) { return std::sqrt(distToSegment2(start, end, pt)); }
SVGPoint lineIntersection(SVGPoint a0, SVGPoint b0, SVGPoint a1, SVGPoint b1);
SVGPoint segmentIntersection(SVGPoint p0, SVGPoint p1, SVGPoint p2, SVGPoint p3);
bool SVGPointInPolygon(const std::vector<SVGPoint>& poly, SVGPoint p);
real polygonArea(const std::vector<SVGPoint>& SVGPoints);

// Ramer-Douglas-Peucker line simplification (O(n^2) version - O(n log n) is much less trivial)
// - we don't necessarily want this to be done in place in Path - see use for LassoSelector
// - end index is inclusive - fewer +/- 1s that way
// Consider implementing Visvalingam algorithm for comparison (uses area of triangles formed by adjacent
//  SVGPoints as criteria instead of perpendicular distance)
template<class T>
std::vector<T> simplifyRDP(const std::vector<T>& SVGPoints, int start, int end, real thresh)
{
    real maxdist2 = 0;
    int argmax = 0;
    SVGPoint p0(SVGPoints[start].x, SVGPoints[start].y);
    SVGPoint p1(SVGPoints[end].x, SVGPoints[end].y);
    for(int ii = start + 1; ii < end; ++ii) {
        real d2 = distToSegment2(p0, p1, SVGPoint(SVGPoints[ii].x, SVGPoints[ii].y));
        if(d2 > maxdist2) {
            maxdist2 = d2;
            argmax = ii;
        }
    }
    if(maxdist2 < thresh*thresh)
        return { SVGPoints[start], SVGPoints[end] };
    std::vector<T> left = simplifyRDP(SVGPoints, start, argmax, thresh);
    std::vector<T> right = simplifyRDP(SVGPoints, argmax, end, thresh);
    left.insert(left.end(), right.begin() + 1, right.end());
    return left;
}
