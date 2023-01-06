#pragma once

#include <vector>
#include "geom.hxx"

// what about methods named after SVG path syntax, so we could do PainterPath().m(3,4).l(3,4).c(3,4,4,6).z()?
class Path2D
{
public:
    // if path parsing fails, last command will be "Error"
    enum PathCommand { MoveTo=1, LineTo, QuadTo, CubicTo, ArcTo }; //, Close, Error};

    std::vector<SVGPoint> points;
    std::vector<PathCommand> commands;
    enum FillRule { EvenOddFill, WindingFill } fillRule = WindingFill;
    //enum PathType {Path=1, Line, Polyline, Polygon, Rectangle, Ellipse, Circle};

    void moveTo(const SVGPoint& p);
    void lineTo(const SVGPoint& p);
    void quadTo(const SVGPoint& c, const SVGPoint& p);
    void cubicTo(const SVGPoint& c1, const SVGPoint& c2, const SVGPoint& p);

    void moveTo(real x, real y) { moveTo(SVGPoint(x,y)); }
    void lineTo(real x, real y) { lineTo(SVGPoint(x,y)); }
    void quadTo(real cx, real cy, real x, real y) { quadTo(SVGPoint(cx, cy), SVGPoint(x, y)); }
    void cubicTo(real c1x, real c1y, real c2x, real c2y, real x, real y)
    { cubicTo(SVGPoint(c1x, c1y), SVGPoint(c2x, c2y), SVGPoint(x, y)); }
    void addArc(real cx, real cy, real rx, real ry, real startRad, real sweepRad, real xAxisRotRad = 0);

    void addPoint(const SVGPoint& p, PathCommand cmd = LineTo);
    void addPoint(real x, real y, PathCommand cmd = LineTo) { addPoint(SVGPoint(x,y), cmd); }

    Path2D& addEllipse(real cx, real cy, real rx, real ry);
    Path2D& addLine(const SVGPoint& a, const SVGPoint& b) { moveTo(a.x, a.y); lineTo(b.x, b.y); return *this; }
    Path2D& addRect(const SVGRect& r);

    void closeSubpath();
    void connectPath(const Path2D& other);
    void setFillRule(FillRule rule) { fillRule = rule; }

    void reserve(size_t n, bool cmds = false) { points.reserve(n); if(cmds) commands.reserve(n); }
    bool isSimple() const { return commands.empty(); }
    int size() const { return points.size(); }
    bool empty() const { return points.empty(); }
    bool isClosed() const { return !points.empty() && points.front() == points.back(); }
    void clear() { points.clear(); commands.clear(); }
    void resize(size_t n) { points.resize(n);  if(!commands.empty()) commands.resize(n); }
    SVGRect getBBox() const;
    //bool isNearSVGPoint(Dim x0, Dim y0, Dim radius) const;
    real distToPoint(const SVGPoint& p) const;
    bool isEnclosedBy(const Path2D& lasso) const;
    real pathLength() const;
    SVGPoint positionAlongPath(real offset, SVGPoint* normal_out) const;

    void translate(real x, real y);
    void scale(real sx, real sy);
    Path2D& transform(const Transform2D& tf);

    Path2D toReversed() const;
    Path2D toFlat() const;
    std::vector<Path2D> getSubPaths() const;

    // reading
    SVGPoint point(int idx) const { return points[idx]; }
    PathCommand command(int idx) const
    {
        if(idx < (int)commands.size())
            return commands[idx];
        return idx > 0 ? LineTo : MoveTo;
    }
    // get SVGPoint from back
    SVGPoint rPoint(int idx) const { return points[size() - idx]; }
    SVGPoint currentPosition() const { return getEndPoint(size() - 1); }
    SVGRect controlPointRect() const { return getBBox(); }
    SVGRect boundingRect() const { return getBBox(); }

    // maybe look into http://www.angusj.com/delphi/clipper.php
    bool intersects(const Path2D& other) { return false; }
    Path2D subtracted(const Path2D& other) { return *this; }

    static bool PRESERVE_ARCS;

private:
    void fillCommands();
    void pushPoint(const SVGPoint& p) { points.push_back(p); }
    SVGPoint getEndPoint(int ii) const;
};

// Java-style iterator for iterating over SVGPoints along path, optionally w/ a specified separation dist - in
//  which case SVGPoints are interpolated or skipped as needed
class PathPointIter
{
public:
    PathPointIter(const Path2D& _path, Transform2D _tf = Transform2D(), real _sep = 0)
    : path(_path), tf(_tf), sep2(_sep*_sep) {}
    bool hasNext() const { return idx + 1 < path.size(); }  // operator bool()
    SVGPoint next();  // operator++()

private:
    const Path2D& path;
    Transform2D tf;
    real sep2;
    SVGPoint currPoint;
    int idx = -1;
};
