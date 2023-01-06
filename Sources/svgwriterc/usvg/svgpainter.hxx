#pragma once
#include "svgnode.hxx"

class SvgPainter
{
public:
    struct ExtraState
    {
        float fillOpacity = 1.0;
        float strokeOpacity = 1.0;
        const SvgFont* svgFont = NULL;
        Painter::TextAlign textAnchor = Painter::AlignLeft;
        Path2D::FillRule fillRule = Path2D::WindingFill;
        const SvgNode* fillServer = NULL;
        const SvgNode* strokeServer = NULL;
        const SvgAttr* dashServer = NULL;
        const SvgAttr* fontFamily = NULL;
        Color currentColor = Color::NONE;
    };

    Painter* p;
    std::vector<ExtraState> extraStates;
    ExtraState& extraState() { return extraStates.back(); }
    // stacks for glyph coordinates
    std::vector<real> textX;
    std::vector<real> textY;
    Path2D textPath;
    real textPathOffset = 0;
    Transform2D initialTransform;
    Transform2D initialTransformInv;
    SVGRect dirtyRect;
    int insideUse = 0;

    SvgPainter(Painter* _p) : p(_p) {}
    void drawNode(const SvgNode* node, const SVGRect& dirty = SVGRect());
    std::vector<SVGRect> glyphPositions(const SvgText* node);
    SVGRect nodeBounds(const SvgNode* node);

    static std::string breakText(const SvgText* node, real maxWidth);
    static void elideText(SvgText* textnode, real maxWidth);
    static SVGRect calcDirtyRect(const SvgNode* node);
    static void clearDirty(const SvgNode* node);

private:
    void initPainter();
    void applyParentStyle(const SvgNode* node);
    void applyStyle(const SvgNode* node);
    Brush gradientBrush(const SvgGradient* gradnode, const SvgNode* dest);
    void resolveFont(SvgDocument* doc);  //, const char* families);

    void draw(const SvgNode* node);
    void _draw(const SvgDocument* node);
    //void _draw(const SvgG* node);
    void _draw(const SvgPath* node);
    void _draw(const SvgImage* node);
    void _draw(const SvgUse* node);
    void _draw(const SvgText* node);
    void _draw(const SvgCustomNode* node);
    void drawChildren(const SvgContainerNode* node);
    void drawPattern(const SvgPattern* pattern, const Path2D* path);

    SVGPoint drawTextTspans(const SvgTspan* node, SVGPoint pos,
                         real* lineh, SVGRect* boundsOut = NULL, std::vector<SVGRect>* glyphPos = NULL);
    SVGPoint drawTextText(const SvgTspan* node, SVGPoint pos, real* lineh, SVGRect* boundsOut, std::vector<SVGRect>* glyphPos);

    SVGRect bounds(const SvgNode* node);
    SVGRect _bounds(const SvgDocument* node);
    //Rect _bounds(const SvgContainerNode* node);
    SVGRect _bounds(const SvgPath* node);
    SVGRect _bounds(const SvgImage* node);
    SVGRect _bounds(const SvgUse* node);
    SVGRect _bounds(const SvgText* node);
    SVGRect _bounds(const SvgCustomNode* node);
    SVGRect childrenBounds(const SvgContainerNode* node);
};
