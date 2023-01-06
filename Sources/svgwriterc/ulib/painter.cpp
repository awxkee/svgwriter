#include <string.h>

#include "platformutil.hxx"
#include "painter.hxx"
#include "path2d.hxx"
#include "image.hxx"

#include "../nanovg/nanovg_sw.h"
#ifndef NO_PAINTER_GL
#include "nanovg_gl_utils.h"
#endif


NVGcontext* Painter::vg = NULL;
bool Painter::vgInUse = false;
bool Painter::sRGB = false;
std::string Painter::defaultFontFamily;
#ifndef NO_PAINTER_GL
bool Painter::glRender = true;
#else
bool Painter::glRender = false;
#endif

Painter::Painter()
{
    painterStates.reserve(32);
    painterStates.emplace_back();
    // this is done to permit use of temporary painter for bounds calculation even when active frame
    nvgSave(vg);
    reset();
}

void Painter::reset()
{
    setTransform(Transform2D());
    setOpacity(1);
    setFillBrush(Brush(Color::BLACK));
    // stroke
    setStrokeBrush(Brush(Color::NONE));
    setStrokeWidth(1);
    setMiterLimit(0);
    setStrokeCap(FlatCap);
    setStrokeJoin(MiterJoin);
    setVectorEffect(NoVectorEffect);
    //Dim strokeDashOffset = 0;
    //std::vector<Dim> strokeDashes;
    // font
    setFontSize(12);
    setFontFamily(defaultFontFamily.c_str());
    //int fontWeight = 400;
    //FontStyle fontStyle = StyleNormal;
    //FontCapitalization fontCaps = MixedCase;
    // other
    setCompOp(CompOp_SrcOver);
    setAntiAlias(true);
    setTextAlign(AlignLeft | AlignBaseline);
    setClipRect(SVGRect());  // clear scissor
}

// maybe we should have an ImagePainter child class?
Painter::Painter(Image* image) : Painter()
{
    targetImage = image;
    int w = image->width, h = image->height;
    deviceRect = SVGRect::wh(w,h);

#ifndef NO_PAINTER_GL
    if(glRender)
        nvgFB = nvgluCreateFramebuffer(vg, w, h, NVGLU_NO_NVG_IMAGE | (sRGB ? NVG_IMAGE_SRGB : 0));
#endif
}

Painter::~Painter()
{
    // make sure we don't leave any states behind on nanovg stack; note
    while(painterStates.size() > 1)
        restore();
    nvgRestore(vg);

#ifndef NO_PAINTER_GL
    if(nvgFB)
        nvgluDeleteFramebuffer(nvgFB);
#endif
}

void Painter::beginFrame(real pxRatio)
{
    ASSERT(!vgInUse && deviceRect.isValid());
    vgInUse = true;

#ifndef NO_PAINTER_SW
    if(targetImage && !glRender)
        nvgswSetFramebuffer(vg, targetImage->bytes(), targetImage->width, targetImage->height, 0, 8, 16, 24);
#endif
    nvgBeginFrame(vg, deviceRect.width(), deviceRect.height(), pxRatio);
    // nvgBeginFrame resets nanovg state stack, so reset ours too
    ASSERT(painterStates.size() == 1);
    painterStates.resize(1);
    reset();
    if(nvgFB) {
        translate(0, deviceRect.height());
        scale(1, -1);
    }
}

void Painter::endFrame()
{
    ASSERT(vgInUse);
    // moved from Painter::beginFrame - nanovg does not make any GL calls until endFrame, so neither should we
#ifdef NO_PAINTER_GL
    nvgEndFrame(vg);
#else
    int prevFBO = -1;
    if(nvgFB)
        prevFBO = nvgluBindFramebuffer(nvgFB);

    // setup GL state
    if(glRender)
        nvgluSetViewport(0, 0, int(deviceRect.width()), int(deviceRect.height()));

    // don't clear when drawing to screen to support partial redraw
    if(targetImage) {
        if(glRender)
            nvgluClear(colorToNVGColor(bgColor));
        else
            targetImage->fill(bgColor.argb());
    }

    nvgEndFrame(vg);

    if(nvgFB) {
        nvgluReadPixels(nvgFB, targetImage->bytes());
        nvgluBindFBO(prevFBO);
    }
#endif
#ifndef NO_PAINTER_SW
    if(targetImage && !glRender)
        nvgswSetFramebuffer(vg, NULL, targetImage->width, targetImage->height, 0, 8, 16, 24);
#endif
    vgInUse = false;
}

// fallback is name of already loaded font, not filename!
bool Painter::loadFont(const char* name, const char* filename)
{
    if(nvgCreateFont(vg, name, filename) == -1)
        return false;
    if(defaultFontFamily.empty())
        defaultFontFamily = name;
    return true;
}

bool Painter::loadFontMem(const char* name, unsigned char* data, int len)
{
    if(nvgCreateFontMem(vg, name, data, len, 0) == -1)
        return false;
    if(defaultFontFamily.empty())
        defaultFontFamily = name;
    return true;
}

bool Painter::addFallbackFont(const char* name, const char* fallback)
{
    return nvgAddFallbackFont(vg, name, fallback);
}

void Painter::save()
{
    nvgSave(vg);
    painterStates.emplace_back(currState());
}

void Painter::restore()
{
    painterStates.pop_back();
    nvgRestore(vg);
}

// transforms

void Painter::setTransform(const Transform2D& tf)
{
    nvgResetTransform(vg);
    transform(tf);
}

void Painter::transform(const Transform2D& tf)
{
    nvgTransform(vg, tf.m[0], tf.m[1], tf.m[2], tf.m[3], tf.m[4], tf.m[5]);
}

Transform2D Painter::getTransform() const
{
    float m[6];
    nvgCurrentTransform(vg, &m[0]);
    return Transform2D(m[0], m[1], m[2], m[3], m[4], m[5]);
}

// PainterState.clipBounds may not be as tight as nanovg's actual scissor (if transform has any rotation) and
//  is always in global coordinate system, whereas rect passed to (set)ClipRect is in local coordinates; clip
//  can only be saved and restored with full save()/restore()
void Painter::setClipRect(const SVGRect& r)
{
    currState().clipBounds = SVGRect();
    nvgResetScissor(vg);
    clipRect(r);
}

void Painter::clipRect(SVGRect r)
{
    if(r.isValid()) {
        Transform2D tf = getTransform();
        // clip rects need to be aligned to pixel boundaries - I think we should do this in nanovg-2 instead
        if(!tf.isRotating())
            r = tf.inverse().mapRect(tf.mapRect(r).round());
        SVGRect curr = currState().clipBounds;
        currState().clipBounds = curr.isValid() ? curr.rectIntersect(tf.mapRect(r)) : tf.mapRect(r);
        nvgIntersectScissor(vg, r.left, r.top, r.width(), r.height());
    }
}

// consider adding nvgCurrentScissor() to nanovg (won't include rotation however)
SVGRect Painter::getClipBounds() const
{
    // clipBounds is in global coords, so transform back to local coords
    return currState().clipBounds.isValid() ? getTransform().inverse().mapRect(currState().clipBounds) : SVGRect();
}

// drawing

void Painter::beginPath()
{
    nvgBeginPath(vg);
}

void Painter::endPath()
{
    if(!currState().fillBrush.isNone())
        nvgFill(vg);
    if(!currState().strokeBrush.isNone()) {
        // compensate for multiplication by scale done in nvgStroke() ... w/ nanovg-2 we can now just pass width = 0
        if(currState().strokeEffect == NonScalingStroke)
            nvgStrokeWidth(vg, currState().strokeWidth/getTransform().avgScale());
        nvgStroke(vg);
        if(currState().strokeEffect == NonScalingStroke)
            nvgStrokeWidth(vg, currState().strokeWidth);  // restore
    }
}

// TODO: can we decouple Painter and Path better?
void Painter::drawPath(const Path2D& path)
{
    nvgFillRule(vg, path.fillRule == Path2D::EvenOddFill ? NVG_EVENODD : NVG_NONZERO);
    beginPath();
    for(int ii = 0; ii < path.size(); ++ii) {
        if(path.command(ii) == Path2D::MoveTo)
            nvgMoveTo(vg, path.point(ii).x, path.point(ii).y);
        else if(path.command(ii) == Path2D::LineTo)
            nvgLineTo(vg, path.point(ii).x, path.point(ii).y);
        else if(path.command(ii) == Path2D::CubicTo) {
            nvgBezierTo(vg, path.point(ii).x, path.point(ii).y,
                        path.point(ii+1).x, path.point(ii+1).y, path.point(ii+2).x, path.point(ii+2).y);
            ii += 2;  // skip control points
        }
        else if(path.command(ii) == Path2D::QuadTo) {
            nvgQuadTo(vg, path.point(ii).x, path.point(ii).y, path.point(ii+1).x, path.point(ii+1).y);
            ++ii;  // skip control point
        }
        else if(path.command(ii) == Path2D::ArcTo) {
            real cx = path.point(ii).x;
            real cy = path.point(ii).y;
            real rx = path.point(ii+1).x;
            //Dim ry = path.point(ii+1).y;
            real start = path.point(ii+2).x;
            real sweep = path.point(ii+2).y;
            //if(rx != ry)
            //  logError("nanovg only supports circular arcs!");
            // nvgArc does not actually close the current subpath, so should be ok
            nvgArc(vg, cx, cy, rx, start, start + sweep, sweep < 0 ? NVG_CCW : NVG_CW);
            ii += 2;  // skip control points
        }
    }
    endPath();
    nvgFillRule(vg, NVG_NONZERO);
}

void Painter::drawLine(const SVGPoint& a, const SVGPoint& b)
{
    beginPath();
    nvgMoveTo(vg, a.x, a.y);
    nvgLineTo(vg, b.x, b.y);
    endPath();
}

void Painter::drawRect(SVGRect rect)
{
    beginPath();
    nvgRect(vg, rect.left, rect.top, rect.width(), rect.height());
    endPath();
}

void Painter::fillRect(SVGRect rect, Color c)
{
    nvgFillColor(vg, colorToNVGColor(c));
    beginPath();
    nvgRect(vg, rect.left, rect.top, rect.width(), rect.height());
    nvgFill(vg);
    setFillBrush(currState().fillBrush); // restore brush
}

void Painter::drawImage(const SVGRect& dest, const Image& image, SVGRect src, int flags)
{
    flags |= sRGB ? NVG_IMAGE_SRGB : 0;
    // help when scaling down images w/ small features? ... just seemed to make things more blurry
    //flags |= NVG_IMAGE_GENERATE_MIPMAPS;
    if(image.painterHandle < 0)
        image.painterHandle = nvgCreateImageRGBA(vg, image.width, image.height, flags, image.constBytes());
    if(!src.isValid())
        src = SVGRect::ltwh(0, 0, image.width, image.height);
    real sx = dest.width()/src.width(), sy = dest.height()/src.height();
    real ex = image.width*sx, ey = image.height*sy;
    real ox = dest.left - src.left*sx, oy = dest.top - src.top*sy;
    NVGpaint imgpaint = nvgImagePattern(vg, ox, oy, ex, ey, 0, image.painterHandle, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, dest.left, dest.top, dest.width(), dest.height());
    nvgFillPaint(vg, imgpaint);
    nvgFill(vg);
    // restore fill settings
    setFillBrush(currState().fillBrush);
}

void Painter::invalidateImage(int handle)
{
    if(vg && handle >= 0)
        nvgDeleteImage(vg, handle);
}

// returns new x position
real Painter::drawText(real x, real y, const char* start, const char* end)
{
    bool faux = currState().fauxItalic || currState().fauxBold;
    if(currState().strokeBrush.isNone() && !faux)
        return nvgText(vg, x, y, start, end);
    // have to render as paths
    if(faux) save();  //nvgSave(vg);
    if(currState().fauxBold && currState().strokeBrush.isNone())
        setStroke(currState().fillBrush, currState().fontPixelSize*0.05, FlatCap, MiterJoin);
    if(currState().fauxItalic) {
        // y translation must be applied before skew
        nvgTranslate(vg, -0.1*currState().fontPixelSize, y);
        nvgSkewX(vg, -13*M_PI/180);
        y = 0;
    }
    real nextx = nvgTextAsPaths(vg, x, y, start, end);
    endPath();
    if(faux) restore();  //nvgRestore(vg);
    return nextx;
}

// Note that text measurement depends on current painter state and thus cannot be static methods
// return X advance; if passed, boundsout is united with the text bounding rect (if boundsout is an
//  invalid rect, this will just set it equal to the text rect)
// TODO: adjust text bounds for faux italic/bold!
real Painter::textBounds(real x, real y, const char* s, const char* end, SVGRect* boundsout)
{
    float bounds[4];
    real advX = nvgTextBounds(vg, x, y, s, end, &bounds[0]);
    if(boundsout)
        boundsout->rectUnion(getTransform().mapRect(SVGRect::ltrb(bounds[0], bounds[1], bounds[2], bounds[3])));
    return advX;
}

int Painter::textGlyphPositions(real x, real y, const char* start, const char* end, std::vector<SVGRect>* pos_out)
{
    size_t len = end ? end - start : strlen(start);
    NVGglyphPosition* positions = new NVGglyphPosition[len];
    int npos = nvgTextGlyphPositions(vg, x, y, start, end, positions, len);
    for(int ii = 0; ii < npos; ++ii)
        pos_out->push_back(SVGRect::ltrb(positions[ii].minx, y, positions[ii].maxx, y));
    delete[] positions;
    return npos;
}

real Painter::textLineHeight()
{
    float lineh = 0;
    nvgTextMetrics(vg, NULL, NULL, &lineh);
    return lineh;
}

void Painter::setTextAlign(TextAlign align)
{
    nvgTextAlign(vg, align);
}

bool Painter::setAntiAlias(bool antialias)
{
    // nanovg state includes antialias, but it can't be read back, so just keep track of it in painterStates
    bool prev = currState().antiAlias;
    currState().antiAlias = antialias;
    nvgShapeAntiAlias(vg, antialias);
    return prev;
}

void Painter::setCompOp(CompOp op)
{
    if(op == CompOp_Clear)
        nvgGlobalCompositeBlendFunc(vg, NVG_ZERO, NVG_ZERO);
    else if(op < NOT_SUPPORTED)
        nvgGlobalCompositeOperation(vg, op);
}

void Painter::setOpacity(real opacity)
{
    float a = opacity;
    currState().globalAlpha = a;
    // this is an ugly hack, and assumes transparent color is dark
    if(a < 1.0f && a > 0.0f && sRGB && currState().sRGBAdjAlpha)
        a = 1.0f - pow(1.0f - a, 2.2f);
    nvgGlobalAlpha(vg, a);
}

// fill/stroke paint, font

NVGpaint Painter::getGradientPaint(const Gradient* grad)
{
    if(grad->stops().empty())
        return nvgLinearGradient(vg, 0, 0, 1, 1, colorToNVGColor(Color::NONE), colorToNVGColor(Color::NONE));

    NVGcolor cin = colorToNVGColor(grad->stops().front().second);
    NVGcolor cout = colorToNVGColor(grad->stops().back().second);
    if(grad->type == Gradient::Linear) {
        const Gradient::LinearGradCoords& g = grad->coords.linear;
        return nvgLinearGradient(vg, g.x1, g.y1, g.x2, g.y2, cin, cout);
    }
    if(grad->type == Gradient::Radial) {
        const Gradient::RadialGradCoords& g = grad->coords.radial;
        // nanosvg doesn't fully support SVG-style radial gradient
        real inner_radius = SVGPoint(g.fx - g.cx, g.fy - g.cy).dist();
        return nvgRadialGradient(vg, g.cx, g.cy, inner_radius, g.radius, cin, cout);
    }
    if(grad->type == Gradient::Box) {
        const Gradient::BoxGradCoords& g = grad->coords.box;
        return nvgBoxGradient(vg, g.x, g.y, g.w, g.h, g.r, g.feather, cin, cout);
    }
    // have to return something
    return nvgLinearGradient(vg, 0, 0, 1, 1, cin, cout);
}

void Painter::setFillBrush(const Brush& b)
{
    currState().fillBrush = b;
    if(b.gradient()) {
        const Gradient* g = b.gradient();
        if(g->coordinateMode() == Gradient::ObjectBoundingMode && g->objectBBox.isValid()) {
            // nanovg applies current transform when setting paint, so include object bbox transform
            Transform2D oldtf = getTransform();
            transform(Transform2D().scale(g->objectBBox.width(), g->objectBBox.height())
                      .translate(g->objectBBox.left, g->objectBBox.top));
            nvgFillPaint(vg, getGradientPaint(g));
            setTransform(oldtf);
        }
        else
            nvgFillPaint(vg, getGradientPaint(g));
    }
    else
        nvgFillColor(vg, colorToNVGColor(b.color()));
}

void Painter::setStrokeBrush(const Brush& b)
{
    currState().strokeBrush = b;
    if(b.gradient()) {
        const Gradient* g = b.gradient();
        if(g->coordinateMode() == Gradient::ObjectBoundingMode && g->objectBBox.isValid()) {
            // nanovg applies current transform when setting paint, so include object bbox transform
            Transform2D oldtf = getTransform();
            transform(Transform2D().scale(g->objectBBox.width(), g->objectBBox.height())
                      .translate(g->objectBBox.left, g->objectBBox.top));
            nvgStrokePaint(vg, getGradientPaint(g));
            setTransform(oldtf);
        }
        else
            nvgStrokePaint(vg, getGradientPaint(g));
    }
    else
        nvgStrokeColor(vg, colorToNVGColor(b.color()));
}

void Painter::setStrokeCap(CapStyle cap) { currState().strokeCap = cap;  nvgLineCap(vg, cap); }
void Painter::setStrokeJoin(JoinStyle join) { currState().strokeJoin = join;  nvgLineJoin(vg, join); }
void Painter::setMiterLimit(real lim) { currState().strokeMiterLimit = lim;  nvgMiterLimit(vg, lim); }
void Painter::setStrokeWidth(real w) { currState().strokeWidth = w;  nvgStrokeWidth(vg, w); }
void Painter::setDashArray(float* dashes) { currState().strokeDashes = dashes;  nvgDashArray(vg, dashes); }
void Painter::setDashOffset(real offset) { currState().strokeDashOffset = offset;  nvgDashOffset(vg, offset); }

void Painter::setStroke(const Brush& b, real w, CapStyle cap, JoinStyle join)
{
    setStrokeBrush(b);
    setStrokeWidth(w);
    setStrokeCap(cap);
    setStrokeJoin(join);
}

void Painter::resolveFont()
{
    bool italic = currState().fontStyle != StyleNormal;
    bool bold  = currState().fontWeight > 550;
    int res = -1;
    if(bold && italic) {
        res = nvgFontFaceId(vg, currState().boldItalicFontId);
        bold = italic = res < 0;  // still need bold and italic?
    }
    if(res < 0 && bold) {
        res = nvgFontFaceId(vg, currState().boldFontId);
        bold = res < 0;  // still need (faux) bold?
    }
    if(res < 0 && italic) {
        res = nvgFontFaceId(vg, currState().italicFontId);
        italic = res < 0;  // still need (faux) italic?
    }
    if(res < 0)
        nvgFontFaceId(vg, currState().fontId);
    currState().fauxBold = bold;
    currState().fauxItalic = italic;
}

// nanovg won't render or calc bounds if unknown font is set; don't set and return false if font not found
bool Painter::setFontFamily(const char* family)
{
    int fontid = nvgFindFont(vg, family);
    if(fontid == -1)
        return false;
    if(fontid == currState().fontId)
        return true;
    std::string fam(family);
    currState().fontId = fontid;
    currState().boldFontId = nvgFindFont(vg, (fam + "-bold").c_str());
    currState().italicFontId = nvgFindFont(vg, (fam + "-italic").c_str());
    currState().boldItalicFontId = nvgFindFont(vg, (fam + "-bold-italic").c_str());
    resolveFont();
    return true;
}

void Painter::setFontWeight(int weight)
{ if(currState().fontWeight != weight) { currState().fontWeight = weight; resolveFont(); } }

void Painter::setFontStyle(FontStyle style)
{ if(currState().fontStyle != style) { currState().fontStyle = style; resolveFont(); } }

void Painter::setFontSize(real px) { currState().fontPixelSize = px;  nvgFontSize(vg, px);}  //pt * 2.08

void Painter::setLetterSpacing(real px) { currState().letterSpacing = px;  nvgTextLetterSpacing(vg, px);}

// sRGB: see nanovg-2 readme for more, but in short, images and colors (i.e. #XXXXXX) are always in sRGB
//  space, but blending should happen in linear space, i.e. shaders should work in linear space, so inputs to
//  shaders should be converted to linear (automatic w/ sRGB texture for images, manual for colors as below)
//  and outputs converted back to sRGB (by writing to sRGB framebuffer and enabling GL_FRAMEBUFFER_SRGB)

// nanovg now handles sRGB conversion based on NVG_SRGB flag passed to nvgCreate
NVGcolor Painter::colorToNVGColor(Color color, float alpha)
{
    float a = alpha >= 0 ? alpha : color.alpha() / 255.0f;
    if(a < 1.0f && a > 0.0f && sRGB && currState().sRGBAdjAlpha)
        a = 1.0f - std::pow(1.0f - a, 2.2f);
    color.color ^= currState().colorXorMask;
    return nvgRGBA(color.red(), color.green(), color.blue(), (unsigned char)(a*255.0f + 0.5f));
}

// Color

// HSV ref: Foley, van Dam, et. al. (1st ed. in C) 13.3
// probably should use floats for HSV calculations, even if we return ints
Color Color::fromHSV(int h, int s, int v, int a)
{
    h = h >= 360 ? 0 : h;
    int h6floor = h/60;
    int h6frac = h%60;
    int p = v * (255 - s);  // v * (1 - s);
    int q = v * (255 - (s*h6frac)/60);  // v * (1 - s*h6frac);
    int t = v * (255 - (s*(60 - h6frac))/60);  // v * (1 - (s * (1 - h6frac)));
    int r = 0, g = 0, b = 0;
    switch(h6floor) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return Color(r/255, g/255, b/255, a);
}

int Color::hueHSV() const
{
    int max = valueHSV();
    int min = std::min(std::min(red(), green()), blue());
    int delta = max - min;
    int hue = 0;
    if(max == 0 || delta == 0)  // i.e., if sat == 0
        hue = 0;
    else if(max == red())
        hue = (green() - blue())/delta;
    else if(max == green())
        hue = 120 + (60*(blue() - red()))/delta;
    else if(max == blue())
        hue = 240 + (60*(red() - green()))/delta;
    if(hue < 0)
        hue += 360;
    return hue;
}

int Color::satHSV() const
{
    int max = valueHSV();
    int min = std::min(std::min(red(), green()), blue());
    return max == 0 ? 0 : (255*(max - min))/max;
}

int Color::valueHSV() const
{
    return std::max(std::max(red(), green()), blue());
}
