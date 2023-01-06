//
// Copyright (c) 2020 Stylus Labs - see LICENSE.txt
//   based on nanovg and nanosvg:
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//   and stb_truetype:
// Public domain; authored from 2009-2020 by Sean Barrett / RAD Game Tools
//
#ifndef NANOVG_SW_H
#define NANOVG_SW_H

#ifdef __cplusplus
extern "C" {
#endif

enum NVGSWcreateFlags {
  NVGSW_PATHS_XC = 1<<3  // use exact coverage algorithm for path rendering
};


// Create a NanoVG context; flags should be combination of the create flags above.
NVGcontext* nvgswCreate(int flags);
void nvgswDelete(NVGcontext* ctx);
void nvgswSetFramebuffer(NVGcontext* vg, void* dest, int w, int h, int rshift, int gshift, int bshift, int ashift);

typedef void (*taskFn_t)(void*);
typedef void (*poolSubmit_t)(taskFn_t, void*);
typedef void (*poolWait_t)(void);
void nvgswSetThreading(NVGcontext* vg, int xthreads, int ythreads, poolSubmit_t submit, poolWait_t wait);

#ifdef __cplusplus
}
#endif

#endif /* NANOVG_SW_H */

#ifdef NANOVG_SW_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nanovg.h"

#ifndef NVG_LOG
#include <stdio.h>
#define NVG_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

enum SWNVGpaintType {
  SWNVG_PAINT_NONE = 0,
  SWNVG_PAINT_COLOR,
  SWNVG_PAINT_GRAD,
  SWNVG_PAINT_IMAGE,
  SWNVG_PAINT_ATLAS
};

#define SWNVG__SUBSAMPLES	5
#define SWNVG__FIXSHIFT		10
#define SWNVG__FIX			(1 << SWNVG__FIXSHIFT)
#define SWNVG__FIXMASK		(SWNVG__FIX-1)
#define SWNVG__MEMPAGE_SIZE	1024

typedef unsigned int rgba32_t;

struct SWNVGtexture {
  int id;
  void* data;
  int width, height;
  int type;
  int flags;
};
typedef struct SWNVGtexture SWNVGtexture;

struct SWNVGcall {
  int type;
  int flags;
  int image;
  int edgeOffset;
  int edgeCount;
  int triangleOffset;
  int triangleCount;
  int uniformOffset;
  int imgOffset;
  int bounds[4];
  //GLNVGblend blendFunc;
  SWNVGtexture* tex;

  float scissorMat[6];
  float paintMat[6];
  rgba32_t innerCol;
  rgba32_t outerCol;
  float scissorExt[2];
  float scissorScale[2];
  float extent[2];
  float radius;
  float feather;
};
typedef struct SWNVGcall SWNVGcall;

typedef struct SWNVGedge {
  float x0,y0, x1,y1;
  int dir;
  struct SWNVGedge* next;
} SWNVGedge;

typedef struct SWNVGactiveEdge {
  int x,dx;
  float ey;
  int dir;
  struct SWNVGactiveEdge *next;
} SWNVGactiveEdge;

typedef struct SWNVGmemPage {
  unsigned char mem[SWNVG__MEMPAGE_SIZE];
  int size;
  struct SWNVGmemPage* next;
} SWNVGmemPage;

struct SWNVGcontext;
typedef struct SWNVGthreadCtx {
  struct SWNVGcontext* context;
  int threadnum;
  int x0, y0, x1, y1;

  SWNVGactiveEdge* freelist;
  SWNVGmemPage* pages;
  SWNVGmemPage* curpage;

  unsigned char* scanline;
  int cscanline;

  int* lineLimits;
} SWNVGthreadCtx;

struct SWNVGcontext {
  unsigned char* bitmap;
  int width, height, stride;
  int rshift, gshift, bshift, ashift;
  SWNVGtexture* textures;
  int ntextures;
  int ctextures;
  int textureId;
  float devicePixelRatio;
  int flags;

  // Per frame buffers
  SWNVGcall* calls;
  int ccalls;
  int ncalls;
  struct NVGvertex* verts;
  int cverts;
  int nverts;

  // rasterizer data
  SWNVGedge* edges;
  int nedges;
  int cedges;

  poolSubmit_t poolSubmit;
  poolWait_t poolWait;
  SWNVGthreadCtx* threads;
  int xthreads;
  int ythreads;
  float* covtex;
};
typedef struct SWNVGcontext SWNVGcontext;

#define LINEAR_TO_SRGB_DIV 2047
static rgba32_t sRGBToLinear[256];
static unsigned char linearToSRGB[LINEAR_TO_SRGB_DIV + 1];
static float sRGBgamma = 2.31f;

static void swnvg__sRGBLUTCalc()
{
  int i;
  for (i = 0; i < 256; ++i)
    sRGBToLinear[i] = (rgba32_t)(0.5f + powf(i/255.0f, sRGBgamma)*LINEAR_TO_SRGB_DIV);
  for (i = 0; i < LINEAR_TO_SRGB_DIV + 1; ++i)
    linearToSRGB[i] = (unsigned char)(0.5f + powf(i/((float)LINEAR_TO_SRGB_DIV), 1/sRGBgamma)*255);
}

static SWNVGmemPage* swnvg__nextPage(SWNVGthreadCtx* r, SWNVGmemPage* cur)
{
  SWNVGmemPage *newp;
  // If using existing chain, return the next page in chain
  if (cur != NULL && cur->next != NULL) return cur->next;
  // Alloc new page
  newp = (SWNVGmemPage*)malloc(sizeof(SWNVGmemPage));
  if (newp == NULL) return NULL;
  memset(newp, 0, sizeof(SWNVGmemPage));
  // Add to linked list
  if (cur != NULL)
    cur->next = newp;
  else
    r->pages = newp;
  return newp;
}

static void swnvg__resetPool(SWNVGthreadCtx* r)
{
  SWNVGmemPage* p = r->pages;
  while (p != NULL) {
    p->size = 0;
    p = p->next;
  }
  r->curpage = r->pages;
}

static unsigned char* swnvg__alloc(SWNVGthreadCtx* r, int size)
{
  unsigned char* buf;
  if (size > SWNVG__MEMPAGE_SIZE) return NULL;
  if (r->curpage == NULL || r->curpage->size+size > SWNVG__MEMPAGE_SIZE) {
    r->curpage = swnvg__nextPage(r, r->curpage);
  }
  buf = &r->curpage->mem[r->curpage->size];
  r->curpage->size += size;
  return buf;
}

static void swnvg__addEdge(SWNVGcontext* r, NVGvertex* vtx)
{
  SWNVGedge* e;
  // Skip horizontal edges
  if (vtx->y0 == vtx->y1) return;
  if (r->nedges+1 > r->cedges) {
    r->cedges = r->cedges > 0 ? r->cedges * 2 : 64;
    r->edges = (SWNVGedge*)realloc(r->edges, sizeof(SWNVGedge) * r->cedges);
    if (r->edges == NULL) return;
  }
  e = &r->edges[r->nedges];
  r->nedges++;
  if (vtx->y0 < vtx->y1) {
    e->x0 = vtx->x0;
    e->y0 = vtx->y0 * SWNVG__SUBSAMPLES;
    e->x1 = vtx->x1;
    e->y1 = vtx->y1 * SWNVG__SUBSAMPLES;
    e->dir = 1;
  } else {
    e->x0 = vtx->x1;
    e->y0 = vtx->y1 * SWNVG__SUBSAMPLES;
    e->x1 = vtx->x0;
    e->y1 = vtx->y0 * SWNVG__SUBSAMPLES;
    e->dir = -1;
  }
}

static SWNVGactiveEdge* swnvg__addActive(SWNVGthreadCtx* r, SWNVGedge* e, float startPoint)
{
   SWNVGactiveEdge* z;

  if (r->freelist != NULL) {
    // Restore from freelist.
    z = r->freelist;
    r->freelist = z->next;
  } else {
    // Alloc new edge.
    z = (SWNVGactiveEdge*)swnvg__alloc(r, sizeof(SWNVGactiveEdge));
    if (z == NULL) return NULL;
  }

  float dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);
//	STBTT_assert(e->y0 <= start_point);
  // round dx down to avoid going too far
  if (dxdy < 0)
    z->dx = (int)(-floorf(SWNVG__FIX * -dxdy));
  else
    z->dx = (int)floorf(SWNVG__FIX * dxdy);
  z->x = (int)floorf(SWNVG__FIX * (e->x0 + dxdy * (startPoint - e->y0)));
//	z->x -= off_x * FIX;
  z->ey = e->y1;
  z->next = 0;
  z->dir = e->dir;

  return z;
}

static void swnvg__freeActive(SWNVGthreadCtx* r, SWNVGactiveEdge* z)
{
  z->next = r->freelist;
  r->freelist = z;
}

static void swnvg__fillScanlineAA(unsigned char* scanline, int len, int x0, int x1, int i, int j)
{
  int maxWeight = (255 / SWNVG__SUBSAMPLES);  // weight per vertical scanline
  if (i == j) {
    // x0,x1 are the same pixel, so compute combined coverage
    scanline[i] = (unsigned char)(scanline[i] + ((x1 - x0) * maxWeight >> SWNVG__FIXSHIFT));
  } else {
    if (i >= 0) // add antialiasing for x0
      scanline[i] = (unsigned char)(scanline[i] + (((SWNVG__FIX - (x0 & SWNVG__FIXMASK)) * maxWeight) >> SWNVG__FIXSHIFT));
    else
      i = -1; // clip

    if (j < len) // add antialiasing for x1
      scanline[j] = (unsigned char)(scanline[j] + (((x1 & SWNVG__FIXMASK) * maxWeight) >> SWNVG__FIXSHIFT));
    else
      j = len; // clip

    for (++i; i < j; ++i) // fill pixels between x0 and x1
      scanline[i] = (unsigned char)(scanline[i] + maxWeight);
  }
}

// non-AA - assumes only one (sub)sample per actual scanline
static void swnvg__fillScanline(unsigned char* scanline, int len, int x0, int x1, int i, int j)
{
  unsigned char maxWeight = 255;
  if (i == j) {
    // x0,x1 are the same pixel - determine if they span the center
    scanline[i] = (x0 & SWNVG__FIXMASK) <= SWNVG__FIX/2 && (x1 & SWNVG__FIXMASK) > SWNVG__FIX/2 ? maxWeight : 0;
  } else {
    if (i >= 0)
      scanline[i] = (x0 & SWNVG__FIXMASK) <= SWNVG__FIX/2 ? maxWeight : 0;
    else
      i = -1; // clip

    if (j < len)
      scanline[j] = (x1 & SWNVG__FIXMASK) > SWNVG__FIX/2 ? maxWeight : 0;
    else
      j = len; // clip

    for (++i; i < j; ++i) // fill pixels between x0 and x1
      scanline[i] = maxWeight;
  }
}

static void swnvg__fillActiveEdges(SWNVGthreadCtx* r, SWNVGactiveEdge* e, int* xmin, int* xmax, int flags)
{
  int x0 = 0, w = 0, left = r->x0, right = r->x1;
  unsigned char* scanline = r->scanline;
  int len = right - left + 1;
  while (e != NULL) {
    if (w == 0) {
      // if we're currently at zero, we need to record the edge start point
      x0 = e->x;
      w = (flags & NVG_PATH_EVENODD) ? 1 : w + e->dir;
    } else {
      int x1 = e->x;
      w = (flags & NVG_PATH_EVENODD) ? 0 : w + e->dir;
      // if we went to zero, we need to draw
      if (w == 0) {
        int i = x0 >> SWNVG__FIXSHIFT;
        int j = x1 >> SWNVG__FIXSHIFT;
        if (i <= right && j >= left) {
          if (i < *xmin) *xmin = i;
          if (j > *xmax) *xmax = j;
          (flags & NVG_PATH_NO_AA) ? swnvg__fillScanline(scanline, len, x0, x1, i - left, j - left)
              : swnvg__fillScanlineAA(scanline, len, x0, x1, i - left, j - left);
        }
      }
    }
    e = e->next;
  }
}

static float swnvg__lengthf(float x, float y) { return sqrtf(x*x + y*y); }
static float swnvg__maxf(float a, float b) { return a < b ? b : a; }
static float swnvg__minf(float a, float b) { return a < b ? a : b; }
static float swnvg__clampf(float a, float mn, float mx) { return a < mn ? mn : (a > mx ? mx : a); }
static int swnvg__maxi(int a, int b) { return a < b ? b : a; }
static int swnvg__mini(int a, int b) { return a < b ? a : b; }
static int swnvg__clampi(int a, int mn, int mx) { return a < mn ? mn : (a > mx ? mx : a); }

#define COLOR0(c) (c & 0xff)
#define COLOR1(c) ((c >> 8) & 0xff)
#define COLOR2(c) ((c >> 16) & 0xff)
#define COLOR3(c) ((c >> 24) & 0xff)
#define RGBA32_IS_OPAQUE(c) ((c & 0xFF000000) == 0xFF000000)

// all modern compilers appear to have built-in optimizations like this for x/255 and other const division
//static inline int swnvg__div255(int x) { return ((x+1) * 257) >> 16 } ;  // this isn't exact in general

// note that we assume color is not premultiplied and do the equivalent of
//  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
static void swnvg__blendSRGB(unsigned char* dst, int cover, int cr, int cg, int cb, int ca)
{
  int srca = (cover * ca)/255;
  int ia = 255 - srca;
  // src over blend
  int r = (srca*cr + ia*(unsigned int)dst[0])/255;
  int g = (srca*cg + ia*(unsigned int)dst[1])/255;
  int b = (srca*cb + ia*(unsigned int)dst[2])/255;
  int a = srca + (ia*(unsigned int)dst[3])/255;  // *ca
  // write
  dst[0] = (unsigned char)r;
  dst[1] = (unsigned char)g;
  dst[2] = (unsigned char)b;
  dst[3] = (unsigned char)a;
}

static void swnvg__blendLinear(unsigned char* dst, int cover, int cr, int cg, int cb, int ca)
{
  unsigned int d0 = sRGBToLinear[dst[0]];
  unsigned int d1 = sRGBToLinear[dst[1]];
  unsigned int d2 = sRGBToLinear[dst[2]];
  unsigned int s0 = sRGBToLinear[cr];
  unsigned int s1 = sRGBToLinear[cg];
  unsigned int s2 = sRGBToLinear[cb];
  int srca = (cover * ca)/255;
  int ia = 255 - srca;
  // src over blend - src RGB is already premultiplied by A, so only mul by cover
  int r = (srca*s0 + ia*d0)/255;
  int g = (srca*s1 + ia*d1)/255;
  int b = (srca*s2 + ia*d2)/255;
  int a = srca + (ia*(unsigned int)dst[3])/255;  //*ca
  // write to bitmap
  dst[0] = linearToSRGB[r];  // swnvg__mini(r, 2047)];  -- clamping only needed for premultiplication w/
  dst[1] = linearToSRGB[g];  // swnvg__mini(g, 2047)];  --  conversion back to ints
  dst[2] = linearToSRGB[b];  // swnvg__mini(b, 2047)];
  dst[3] = (unsigned char)a;
}

static void swnvg__blend(unsigned char* dst, int cover, int cr, int cg, int cb, int ca, int linear)
{
  linear ? swnvg__blendLinear(dst, cover, cr, cg, cb, ca) : swnvg__blendSRGB(dst, cover, cr, cg, cb, ca);
}

static void swnvg__blend8888(unsigned char* dst, int cover, int cr, int cg, int cb, int ca, int linear)
{
  if(cover == 255 && ca == 255) {
    dst[0] = (unsigned char)cr;
    dst[1] = (unsigned char)cg;
    dst[2] = (unsigned char)cb;
    dst[3] = 255;
  }
  else
    swnvg__blend(dst, cover, cr, cg, cb, ca, linear);
}

// assumes color is opaque
static void swnvg__blendOpaque(unsigned char* dst, int cover, rgba32_t rgba, int linear)
{
  if(cover == 255)
    *(rgba32_t*)dst = rgba;
  else
    swnvg__blend(dst, cover, COLOR0(rgba), COLOR1(rgba), COLOR2(rgba), COLOR3(rgba), linear);
}

static rgba32_t texelFetchRGBA32(SWNVGtexture* tex, int x, int y)
{
  rgba32_t* data = (rgba32_t*)tex->data;
  return data[x + y*tex->width];
}

static unsigned int swnvg__mix8(float fx, float fy, int t00, int t10, int t01, int t11)
{
  //return mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
  float t0 = t00 + fx*(t10 - t00);
  float t1 = t01 + fx*(t11 - t01);
  return (unsigned int)(0.5f + t0 + fy*(t1 - t0));
}

static void swnvg__lerpAndBlend(unsigned char* dst, unsigned char cover, SWNVGtexture* tex, float ijx, float ijy, int linear)
{
  ijx = swnvg__maxf(0.0f, ijx);  ijy = swnvg__maxf(0.0f, ijy);
  int ij00x = swnvg__mini((int)ijx, tex->width-1), ij00y = swnvg__mini((int)ijy, tex->height-1);
  int ij11x = swnvg__mini((int)ijx + 1, tex->width-1), ij11y = swnvg__mini((int)ijy + 1, tex->height-1);
  rgba32_t t00 = texelFetchRGBA32(tex, ij00x, ij00y);
  rgba32_t t10 = texelFetchRGBA32(tex, ij11x, ij00y);
  rgba32_t t01 = texelFetchRGBA32(tex, ij00x, ij11y);
  rgba32_t t11 = texelFetchRGBA32(tex, ij11x, ij11y);
  float fx = ijx - (int)ijx, fy = ijy - (int)ijy;

  int c0 = swnvg__mix8(fx, fy, COLOR0(t00), COLOR0(t10), COLOR0(t01), COLOR0(t11));
  int c1 = swnvg__mix8(fx, fy, COLOR1(t00), COLOR1(t10), COLOR1(t01), COLOR1(t11));
  int c2 = swnvg__mix8(fx, fy, COLOR2(t00), COLOR2(t10), COLOR2(t01), COLOR2(t11));
  int c3 = swnvg__mix8(fx, fy, COLOR3(t00), COLOR3(t10), COLOR3(t01), COLOR3(t11));
  swnvg__blend8888(dst, cover, c0, c1, c2, c3, linear);
}

static void swnvg__scanlineSolid(unsigned char* dst, int count, unsigned char* cover, int x, int y, SWNVGcall* call)
{
  int i;
  int linear = call->flags & NVG_SRGB ? 1 : 0;
  //nvgTransformPoint(&qx, &qy, call->scissorMat, x, y);
  //float ssx = 0.5f - (fabsf(qx) - call->scissorExt[0])*call->scissorScale[0];
  //float ssy = 0.5f - (fabsf(qy) - call->scissorExt[1])*call->scissorScale[1];
  //float sscover = swnvg__clampf(ssx, 0.0f, 1.0f) * swnvg__clampf(ssy, 0.0f, 1.0f);
  // note r,g,b may not actually be R,G,B (in particular, R and G could be switched)
  if (call->type == SWNVG_PAINT_COLOR) {
    rgba32_t c = call->innerCol;
    if(RGBA32_IS_OPAQUE(c)) {
      for(i = 0; i < count; ++i, dst += 4)
        swnvg__blendOpaque(dst, *cover++, c, linear);
    }
    else {
      for(i = 0; i < count; ++i, dst += 4)
        swnvg__blend(dst, *cover++, COLOR0(c), COLOR1(c), COLOR2(c), COLOR3(c), linear);
    }
  } else if (call->type == SWNVG_PAINT_IMAGE) {
    rgba32_t* img = (rgba32_t*)call->tex->data;
    float qx, qy;
    float dqx = call->paintMat[0]*call->tex->width/call->extent[0];
    float dqy = call->paintMat[1]*call->tex->height/call->extent[1];
    nvgTransformPoint(&qx, &qy, call->paintMat, x, y);
    // +/- 0.5 determined by experiment to match nanovg_gl
    qx = (qx + 0.5f)*call->tex->width/call->extent[0] - 0.5f;
    qy = (qy + 0.5f)*call->tex->height/call->extent[1] - 0.5f;
    for (i = 0; i < count; ++i) {
      if(call->tex->flags & NVG_IMAGE_NEAREST) {
        int imgx = swnvg__clampi((int)(0.5f + qx), 0, call->tex->width-1);
        int imgy = swnvg__clampi((int)(0.5f + qy), 0, call->tex->height-1);
        rgba32_t c = img[imgy*call->tex->width + imgx];
        if(RGBA32_IS_OPAQUE(c))
          swnvg__blendOpaque(dst, *cover++, c, linear);
        else
          swnvg__blend(dst, *cover++, COLOR0(c), COLOR1(c), COLOR2(c), COLOR3(c), linear);
      }
      else
        swnvg__lerpAndBlend(dst, *cover++, call->tex, qx, qy, linear);
      qx += dqx;  // for qx,qy => qx+1,qy
      qy += dqy;
      dst += 4;
    }
  } else if (call->type == SWNVG_PAINT_GRAD) {
    float qx, qy;
    int cr0 = call->innerCol & 0xff;
    int cg0 = (call->innerCol >> 8) & 0xff;
    int cb0 = (call->innerCol >> 16) & 0xff;
    int ca0 = (call->innerCol >> 24) & 0xff;
    int cr1 = call->outerCol & 0xff;
    int cg1 = (call->outerCol >> 8) & 0xff;
    int cb1 = (call->outerCol >> 16) & 0xff;
    int ca1 = (call->outerCol >> 24) & 0xff;
    for (i = 0; i < count; ++i) {
      // can't just step qx, qy due to numerical issues w/ linear gradient
      nvgTransformPoint(&qx, &qy, call->paintMat, x++, y);
      float dx = fabsf(qx) - (call->extent[0] - call->radius);
      float dy = fabsf(qy) - (call->extent[1] - call->radius);
      float d0 = swnvg__minf(swnvg__maxf(dx, dy), 0.0f)
          + swnvg__lengthf(swnvg__maxf(dx, 0.0f), swnvg__maxf(dy, 0.0f)) - call->radius;
      float d = swnvg__clampf((d0 + call->feather*0.5f)/call->feather, 0.0f, 1.0f);
      int cr = (int)(0.5f + cr0*(1.0f - d) + cr1*d);
      int cg = (int)(0.5f + cg0*(1.0f - d) + cg1*d);
      int cb = (int)(0.5f + cb0*(1.0f - d) + cb1*d);
      int ca = (int)(0.5f + ca0*(1.0f - d) + ca1*d);
      swnvg__blend8888(dst, *cover++, cr, cg, cb, ca, linear);
      //qx += call->paintMat[0];  // instead of tf*(x+1, y)
      //qy += call->paintMat[1];
      dst += 4;
    }
  }
}

static void swnvg__rasterizeSortedEdges(SWNVGthreadCtx* r, SWNVGcall* call)
{
  SWNVGcontext* gl = r->context;
  SWNVGactiveEdge *active = NULL;
  int y, s;
  int e = call->edgeOffset;
  int eend = call->edgeOffset + call->edgeCount;
  int xmin, xmax, xmin1, xmax1;

  for (y = swnvg__maxi(r->y0, call->bounds[1]); y <= swnvg__mini(r->y1, call->bounds[3]); y++) {
    xmin = gl->width;
    xmax = 0;
    for (s = 0; s < SWNVG__SUBSAMPLES; ++s) {
      // we only process one subsample scanline for non-AA
      if((call->flags & NVG_PATH_NO_AA) && s != SWNVG__SUBSAMPLES/2)
        continue;
      // find center of pixel for this scanline
      float scany = (float)(y*SWNVG__SUBSAMPLES + s) + 0.5f;
      SWNVGactiveEdge **step = &active;

      // update active edges - remove active edges that terminate before the center of this scanline
      while (*step) {
        SWNVGactiveEdge *z = *step;
        if (z->ey <= scany) {
          *step = z->next; // delete from list
          swnvg__freeActive(r, z);
        } else {
          z->x += z->dx; // advance to position for current scanline
          step = &((*step)->next); // advance through list
        }
      }
      // resort the list if needed
      for (;;) {
        int changed = 0;
        step = &active;
        while (*step && (*step)->next) {
          if ((*step)->x > (*step)->next->x) {
            SWNVGactiveEdge* t = *step;
            SWNVGactiveEdge* q = t->next;
            t->next = q->next;
            q->next = t;
            *step = q;
            changed = 1;
          }
          step = &(*step)->next;
        }
        if (!changed) break;
      }
      // insert all edges that start before the center of this scanline -- omit ones that also end on this scanline
      while (e < eend && gl->edges[e].y0 <= scany) {
        if (gl->edges[e].y1 > scany) {
          SWNVGactiveEdge* z = swnvg__addActive(r, &gl->edges[e], scany);
          if (z == NULL) break;
          if (call->flags & NVG_PATH_NO_AA)
            z->dx *= SWNVG__SUBSAMPLES;  // AA case uses per-subscanline step
          // find insertion point
          if (active == NULL) {
            active = z;
          } else if (z->x < active->x) {
            // insert at front
            z->next = active;
            active = z;
          } else {
            // find thing to insert AFTER
            SWNVGactiveEdge* p = active;
            while (p->next && p->next->x < z->x)
              p = p->next;
            // at this point, p->next->x is NOT < z->x
            z->next = p->next;
            p->next = z;
          }
        }
        e++;
      }
      // now process all active edges in non-zero fashion
      if (active != NULL)
        swnvg__fillActiveEdges(r, active, &xmin, &xmax, call->flags);
    }
    // clip xmin, xmax for memset
    xmin = swnvg__maxi(xmin, r->x0);
    xmax = swnvg__mini(xmax, r->x1);
    // further clip for possible scissor
    xmin1 = swnvg__maxi(xmin, call->bounds[0]);
    xmax1 = swnvg__mini(xmax, call->bounds[2]);
    if (xmin1 <= xmax1)
      swnvg__scanlineSolid(&gl->bitmap[y*gl->stride + xmin1*4], xmax1-xmin1+1, &r->scanline[xmin1 - r->x0], xmin1, y, call);
    // we fill x range clipped to scissor, but we have to clear entire range written by fillActiveEdges
    if (xmin <= xmax)
      memset(&r->scanline[xmin - r->x0], 0, xmax-xmin+1);
  }
}

static float texFetchF32(SWNVGtexture* tex, int x, int y)
{
  float* data = (float*)tex->data;
  return data[x + y*tex->width];
}

static float texFetchF32Lerp(SWNVGtexture* tex, float ijx, float ijy, int ijminx, int ijminy, int ijmaxx, int ijmaxy)
{
  int ij00x = swnvg__clampi((int)ijx, ijminx, ijmaxx), ij00y = swnvg__clampi((int)ijy, ijminy, ijmaxy);
  int ij11x = swnvg__clampi((int)ijx + 1, ijminx, ijmaxx), ij11y = swnvg__clampi((int)ijy + 1, ijminy, ijmaxy);
  float t00 = texFetchF32(tex, ij00x, ij00y);
  float t10 = texFetchF32(tex, ij11x, ij00y);
  float t01 = texFetchF32(tex, ij00x, ij11y);
  float t11 = texFetchF32(tex, ij11x, ij11y);
  float fx = ijx - (int)ijx, fy = ijy - (int)ijy;
  //return mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
  float t0 = t00 + fx*(t10 - t00);
  float t1 = t01 + fx*(t11 - t01);
  return t0 + fy*(t1 - t0);
}

// recall that we do clamping instead of just adding a border around each glyph because dx,dy could be large
//  at small font sizes
static float summedTextCov(SWNVGtexture* tex, float ijx, float ijy, float dx, float dy, int ijminx, int ijminy, int ijmaxx, int ijmaxy)
{
  // for some reason, we need to shift by an extra (-0.5, -0.5) for summed case (here or in fons__getQuad)
  //ijx -= 0.499999f;  ijy -= 0.499999f;
  float s11 = texFetchF32Lerp(tex, ijx + dx, ijy + dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float s01 = texFetchF32Lerp(tex, ijx - dx, ijy + dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float s10 = texFetchF32Lerp(tex, ijx + dx, ijy - dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float s00 = texFetchF32Lerp(tex, ijx - dx, ijy - dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float cov = (s11 - s01 - s10 + s00)/(255.0f*4.0f*dx*dy);
  return swnvg__clampf(cov, 0.0f, 1.0f);
}

static unsigned char texFetch(SWNVGtexture* tex, int x, int y)
{
  unsigned char* data = (unsigned char*)tex->data;
  return data[x + y*tex->width];
}

static float texFetchLerp(SWNVGtexture* tex, float ijx, float ijy, int ijminx, int ijminy, int ijmaxx, int ijmaxy)
{
  int ij00x = swnvg__clampi((int)ijx, ijminx, ijmaxx), ij00y = swnvg__clampi((int)ijy, ijminy, ijmaxy);
  int ij11x = swnvg__clampi((int)ijx + 1, ijminx, ijmaxx), ij11y = swnvg__clampi((int)ijy + 1, ijminy, ijmaxy);
  float t00 = texFetch(tex, ij00x, ij00y);
  float t10 = texFetch(tex, ij11x, ij00y);
  float t01 = texFetch(tex, ij00x, ij11y);
  float t11 = texFetch(tex, ij11x, ij11y);
  float fx = ijx - (int)ijx, fy = ijy - (int)ijy;
  float t0 = t00 + fx*(t10 - t00);
  float t1 = t01 + fx*(t11 - t01);
  return t0 + fy*(t1 - t0);
}

// Super-sampled SDF text - nvgFontBlur can be used to make thicker or thinner
static float sdfCov(float D, float sdfscale, float sdfoffset)
{
  return D > 0.0f ? swnvg__clampf((D - 255.0f*0.5f)/sdfscale + sdfoffset, 0.0f, 1.0f) : 0.0f;
}

static float superSDF(SWNVGtexture* tex, float s, float dr, float ijx, float ijy, float dx, float dy, int ijminx, int ijminy, int ijmaxx, int ijmaxy)
{
  // check distance from center of nearest pixel and exit early if large enough
  // doesn't help at all for very small font sizes, but >50% for larger sizes
  int ij0x = swnvg__clampi((int)(ijx + 0.5f), ijminx, ijmaxx);
  int ij0y = swnvg__clampi((int)(ijy + 0.5f), ijminy, ijmaxy);
  float d = texFetch(tex, ij0x, ij0y);
  float sd = (d - 255.0f*0.5f)/s + (dr - 0.5f);  // note we're still using the half pixel scale here
  if(sd < -1.415f) return 0;  // sqrt(2) ... verified experimentally
  if(sd > 1.415f) return 1.0f;

  //return sdfCov(texFetchLerp(tex, ijx, ijy, ijminx, ijminy, ijmaxx, ijmaxy), 2*s);  // single sample
  float d11 = texFetchLerp(tex, ijx + dx, ijy + dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float d10 = texFetchLerp(tex, ijx - dx, ijy + dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float d01 = texFetchLerp(tex, ijx + dx, ijy - dy, ijminx, ijminy, ijmaxx, ijmaxy);
  float d00 = texFetchLerp(tex, ijx - dx, ijy - dy, ijminx, ijminy, ijmaxx, ijmaxy);
  return 0.25f*(sdfCov(d11, s, dr) + sdfCov(d10, s, dr) + sdfCov(d01, s, dr) + sdfCov(d00, s, dr));
}

static void swnvg__rasterizeQuad(SWNVGthreadCtx* r, SWNVGcall* call, SWNVGtexture* tex, NVGvertex* v00, NVGvertex* v11)
{
  SWNVGcontext* gl = r->context;
  int x, y;
  float s00 = tex->width * v00->x1;
  float t00 = tex->height * v00->y1;
  float ds = call->paintMat[0]/2;
  float dt = call->paintMat[3]/2;
#ifdef FONS_SDF
  float sdfoffset = call->radius + 0.5f;
  float sdfscale = 0.5f * 32.0f*call->paintMat[0];  // 0.5 - we're sampling 4 0.5x0.5 subpixels
  s00 += 4 + ds;  // account for 4 pixel padding in SDF
  t00 += 4 + dt;
#endif

  int linear = call->flags & NVG_SRGB;
  int cr = COLOR0(call->innerCol);
  int cg = COLOR1(call->innerCol);
  int cb = COLOR2(call->innerCol);
  int ca = COLOR3(call->innerCol);

  int extentx = (int)call->extent[0], extenty = (int)call->extent[1];
  // use texcoord center to figure out which atlas rect we are reading from
  //int ijminx = ((int)(0.5f*(v00->x1 + v11->x1)*tex->width/extentx + 0.5f))*extentx;
  //int ijminy = ((int)(0.5f*(v00->y1 + v11->y1)*tex->height/extenty + 0.5f))*extenty;
  int ijminx = ((int)(s00/extentx + 0.5f))*extentx, ijminy = ((int)(t00/extenty + 0.5f))*extenty;
  int ijmaxx = ijminx + extentx - 1, ijmaxy = ijminy + extenty - 1;
  if(ijminx < 0 || ijminy < 0) return;  // something went wrong

  int xmin = swnvg__maxi(swnvg__maxi(call->bounds[0], r->x0), (int)v00->x0);
  int ymin = swnvg__maxi(swnvg__maxi(call->bounds[1], r->y0), (int)v00->y0);
  int xmax = swnvg__mini(swnvg__mini(call->bounds[2], r->x1), (int)(ceilf(v11->x0)));
  int ymax = swnvg__mini(swnvg__mini(call->bounds[3], r->y1), (int)(ceilf(v11->y0)));
  if(ymin > ymax || xmin > xmax) return;
  float s0 = s00 - 2*ds*(v00->x0 - xmin);
  float t = t00 - 2*dt*(v00->y0 - ymin);
  for(y = ymin; y <= ymax; ++y) {
    unsigned char* dst = &gl->bitmap[y*gl->stride + xmin*4];
    float s = s0;
    for(x = xmin; x <= xmax; ++x) {
#ifdef FONS_SDF
      float cover = superSDF(tex, sdfscale, sdfoffset, s, t, ds/2, dt/2, ijminx, ijminy, ijmaxx, ijmaxy);
#else
      float cover = summedTextCov(tex, s, t, ds, dt, ijminx, ijminy, ijmaxx, ijmaxy);
#endif
      swnvg__blend8888(dst, (int)(255.0f*cover + 0.5f), cr, cg, cb, ca, linear);
      s += 2*ds;
      dst += 4;
    }
    t += 2*dt;
  }
}

static SWNVGtexture* swnvg__allocTexture(SWNVGcontext* gl)
{
  SWNVGtexture* tex = NULL;
  int i;

  for (i = 0; i < gl->ntextures; i++) {
    if (gl->textures[i].id == 0) {
      tex = &gl->textures[i];
      break;
    }
  }
  if (tex == NULL) {
    if (gl->ntextures+1 > gl->ctextures) {
      SWNVGtexture* textures;
      int ctextures = swnvg__maxi(gl->ntextures+1, 4) +  gl->ctextures/2; // 1.5x Overallocate
      textures = (SWNVGtexture*)realloc(gl->textures, sizeof(SWNVGtexture)*ctextures);
      if (textures == NULL) return NULL;
      gl->textures = textures;
      gl->ctextures = ctextures;
    }
    tex = &gl->textures[gl->ntextures++];
  }

  memset(tex, 0, sizeof(*tex));
  tex->id = ++gl->textureId;
  return tex;
}

static SWNVGtexture* swnvg__findTexture(SWNVGcontext* gl, int id)
{
  int i;
  for (i = 0; i < gl->ntextures; i++)
    if (gl->textures[i].id == id)
      return &gl->textures[i];
  return NULL;
}

static int swnvg__renderCreate(void* uptr)
{
  static int staticInited = 0;
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  if(!staticInited) {
    swnvg__sRGBLUTCalc();
    staticInited = 1;
  }

  NVG_LOG("nvg2: software renderer%s\n", gl->flags & NVGSW_PATHS_XC ? " (XC)" : "");
  return 1;
}

static void swnvg__copyRGBAData(SWNVGcontext* gl, SWNVGtexture* tex, const void* data)
{
  int ii;
  int npix = tex->width*tex->height;
  rgba32_t* dest = (rgba32_t*)tex->data;
  rgba32_t* src = (rgba32_t*)data;
  if(tex->flags & NVG_IMAGE_PREMULTIPLIED) {
    // undo premultiplication
    for(ii = 0; ii < npix; ++ii, ++dest, ++src) {
      int r = COLOR0(*src);
      int g = COLOR1(*src);
      int b = COLOR2(*src);
      int a = COLOR3(*src);
      *dest = ((255*r)/a << gl->rshift | (255*g)/a << gl->gshift | (255*b)/a << gl->bshift | a << gl->ashift);
    }
  }
  else if(gl->rshift == 0 && gl->gshift == 8 && gl->bshift == 16 && gl->ashift == 24) {
    memcpy(dest, src, npix*4);
  }
  else {
    for(ii = 0; ii < npix; ++ii, ++dest, ++src) {
      int r = COLOR0(*src);
      int g = COLOR1(*src);
      int b = COLOR2(*src);
      int a = COLOR3(*src);
      *dest = (r << gl->rshift | g << gl->gshift | b << gl->bshift | a << gl->ashift);
    }
  }
}

static int swnvg__renderCreateTexture(void* uptr, int type, int w, int h, int imageFlags, const void* data)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  SWNVGtexture* tex = swnvg__allocTexture(gl);
  tex->width = w;  tex->height = h;  tex->flags = imageFlags;  tex->type = type;
  if(imageFlags & NVG_IMAGE_NOCOPY)  // we'll require user to make sure image byte order matches framebuffer
    tex->data = (void*)data;
  else {
    size_t nbytes = tex->type == NVG_TEXTURE_ALPHA ? w*h : w*h*4;
    tex->data = malloc(nbytes);
    if(!data) {}
    else if(tex->type == NVG_TEXTURE_RGBA)
      swnvg__copyRGBAData(gl, tex, data);
    else
      memcpy(tex->data, data, nbytes);
  }
  return tex->id;
}

static int swnvg__renderDeleteTexture(void* uptr, int image)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  SWNVGtexture* tex = swnvg__findTexture(gl, image);
  if(!tex) return 0;
  if(!(tex->flags & NVG_IMAGE_NOCOPY))
    free(tex->data);
  memset(tex, 0, sizeof(SWNVGtexture));
  return 1;
}

static int swnvg__renderUpdateTexture(void* uptr, int image, int x, int y, int w, int h, const void* data)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  SWNVGtexture* tex = swnvg__findTexture(gl, image);
  if(tex->type == NVG_TEXTURE_RGBA)
    swnvg__copyRGBAData(gl, tex, data);  // only full update for now
  else {
    int nb = tex->type == NVG_TEXTURE_FLOAT ? 4 : 1;
    int dy = y*tex->width*nb;
    memcpy((char*)tex->data + dy, (const char*)data + dy, tex->width*h*nb);  // no support for partial width
  }
  return 1;
}

static int swnvg__renderGetTextureSize(void* uptr, int image, int* w, int* h)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  SWNVGtexture* tex = swnvg__findTexture(gl, image);
  if (!tex) return 0;
  if (w) *w = tex->width;
  if (h) *h = tex->height;
  return 1;
}

static void swnvg__renderViewport(void* uptr, float width, float height, float devicePixelRatio) {}

static void swnvg__renderCancel(void* uptr)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  gl->nverts = 0;
  gl->nedges = 0;
  gl->ncalls = 0;
}

// exact coverage rasterization based on GPU renderer (nanovg_gl.h)
// - we now store the difference in coverage from the pixel to left, so we no longer write solid runs or
//  recalculate integer coverage for every pixel of solid runs.  With this change, performance matches non-XC
//  for big paths and beats non-XC for small paths.

static void swnvg__addEdgeXC(SWNVGcontext* r, NVGvertex* vtx, float xmax)
{
  SWNVGedge* e;
  // Skip horizontal edges
  if (vtx->y0 == vtx->y1) return;
  if (r->nedges+1 > r->cedges) {
    r->cedges = r->cedges > 0 ? r->cedges * 2 : 64;
    r->edges = (SWNVGedge*)realloc(r->edges, sizeof(SWNVGedge) * r->cedges);
    if (r->edges == NULL) return;
  }
  e = &r->edges[r->nedges];
  r->nedges++;
  e->x0 = vtx->x0;
  e->y0 = vtx->y0;
  e->x1 = vtx->x1;
  e->y1 = vtx->y1;
  e->dir = ceilf(xmax);
}

// this fills in the +y direction, but we call with x and y swapped (see below) to fill in +x direction
static float areaEdge2(float v0x, float v0y, float v1x, float v1y, float slope)
{
  float win0 = swnvg__clampf(v0x, -0.5f, 0.5f);
  float win1 = swnvg__clampf(v1x, -0.5f, 0.5f);
  float width = win1 - win0;
  if(width == 0)
    return 0;
  //if(v0y < -0.5f && v1y < -0.5f) return width;  -- don't see much effect
  if(slope == 0)
    return width * swnvg__clampf(0.5f - v0y, 0.0f, 1.0f);
  float midx = 0.5f*(win0 + win1);
  float y = v0y + (midx - v0x)*slope;  // y value at middle of window
  float dy = fabsf(slope*width);
  float sx = swnvg__clampf(y + 0.5f*dy + 0.5f, 0.0f, 1.0f);  // shift from -0.5..0.5 to 0..1 for area calc
  float sy = swnvg__clampf(y - 0.5f*dy + 0.5f, 0.0f, 1.0f);
  float sz = swnvg__clampf((0.5f - y)/dy + 0.5f, 0.0f, 1.0f);
  float sw = swnvg__clampf((-0.5f - y)/dy + 0.5f, 0.0f, 1.0f);
  float area = 0.5f*(sz - sz*sy + 1.0f - sx + sx*sw);  // +1.0 for fill in +y direction, -1.0 for -y direction
  return area * width;
}

static void swnvg__rasterizeXC(SWNVGthreadCtx* r, SWNVGcall* call)
{
  int i, ix, iy;  //, ix0, iy0, iy1, ix1;
  SWNVGcontext* gl = r->context;
  // note that edges may lie outside bounds due to scissoring
  int xb0 = swnvg__maxi(call->bounds[0], r->x0);
  int yb0 = swnvg__maxi(call->bounds[1], r->y0);
  int xb1 = swnvg__mini(call->bounds[2], r->x1);
  int yb1 = swnvg__mini(call->bounds[3], r->y1);
  SWNVGedge* edge = &gl->edges[call->edgeOffset];
  for(i = 0; i < call->edgeCount; ++i, ++edge) {
    //if(edge->y0 == edge->y1) continue;  -- horizontal edges are never added to list
    int xedge = swnvg__mini(edge->dir, xb1);
    int dir = edge->y0 > edge->y1 ? -1 : 1;
    float ymin = swnvg__minf(edge->y0, edge->y1);
    float ymax = swnvg__maxf(edge->y0, edge->y1);
    int iymin = swnvg__maxi((int)ymin, yb0);
    int iymax = swnvg__mini((int)ymax, yb1);
    float invslope = (edge->x1 - edge->x0)/(edge->y1 - edge->y0);
    // x coords at top and bottom of current row
    float xtop = edge->y0 > edge->y1 ? edge->x1 : edge->x0;
    float xt = (iymin - ymin)*invslope + xtop;
    float xb = xt + invslope;
    float xmin = swnvg__minf(xt, xb);
    float xmax = swnvg__maxf(xt, xb);
    int ixleft = swnvg__maxi(swnvg__minf(edge->x0, edge->x1), xb0);
    int ixright = swnvg__mini(swnvg__maxf(edge->x0, edge->x1), xb1);
    // scanline x limits for this call
    int* lims = &r->lineLimits[2*(iymin - r->y0)];

    for(iy = iymin; iy <= iymax; ++iy) {
      int ixmin = swnvg__maxi((int)xmin, ixleft);
      int ixmax = swnvg__mini((int)xmax, ixright);
      float* dst = &gl->covtex[iy*gl->width + ixmin];
      float cov = 0;
      for(ix = ixmin; ix <= ixmax; ++ix) {
        float c = areaEdge2(edge->y0 - iy - 0.5f, edge->x0 - ix - 0.5f, edge->y1 - iy - 0.5f, edge->x1 - ix - 0.5f, invslope);
        *dst++ += c - cov;
        cov = c;
      }
      // coverage for remaining pixels
      if(ix <= xedge)
        *dst += dir*(swnvg__minf(ymax, iy+1) - swnvg__maxf(ymin, iy)) - cov;
      // scanline x limits
      if(ixmin < lims[0])
        lims[0] = ixmin;
      if(ixmax >= lims[1])
        lims[1] = ixmax+1;
      lims += 2;
      xmin += invslope;
      xmax += invslope;
    }
  }

  // fill
  int* lims = &r->lineLimits[2*(yb0 - r->y0)];
  int linear = call->flags & NVG_SRGB ? 1 : 0;
  rgba32_t c = call->innerCol;
  for(iy = yb0; iy <= yb1; ++iy) {
    float cover = 0;
    int icover = 0;
    int count = swnvg__mini(lims[1], xb1) - lims[0] + 1;
    unsigned char* dst = &gl->bitmap[iy*gl->stride + lims[0]*4];
    float* dcover = &gl->covtex[iy*gl->width + lims[0]];
    lims[0] = gl->width; lims[1] = 0;  // reset limits for this scanline
    lims += 2;

    if (call->type == SWNVG_PAINT_COLOR) {
      // handle solid color directly for better performance
      if(RGBA32_IS_OPAQUE(c)) {
        for(i = 0; i < count; ++i, ++dcover, dst += 4) {
          if(*dcover != 0) {
            cover += *dcover;
            icover = swnvg__mini(fabsf(cover)*255 + 0.5f, 255);
            *dcover = 0;
          }
          if(icover > 0)
            swnvg__blendOpaque(dst, icover, c, linear);
        }
      }
      else {
        for(i = 0; i < count; ++i, ++dcover, dst += 4) {
          if(*dcover != 0) {
            cover += *dcover;
            icover = swnvg__mini(fabsf(cover)*255 + 0.5f, 255);
            *dcover = 0;
          }
          if(icover > 0)
            swnvg__blend(dst, icover, COLOR0(c), COLOR1(c), COLOR2(c), COLOR3(c), linear);
        }
      }
    }
    else {
      // images and gradients
      unsigned char* sl = r->scanline;
      for(i = 0; i < count; ++i, ++dcover, ++sl) {
        if(*dcover != 0) {
          cover += *dcover;
          icover = swnvg__mini(fabsf(cover)*255 + 0.5f, 255);
          *dcover = 0;
        }
        *sl = icover;
      }
      swnvg__scanlineSolid(dst, count, r->scanline, xb0, iy, call);
    }
  }
}


// cut and paste from stbtt; this benchmarks much faster than qsort() and a bit faster than a naive quicksort
#define SWNVG__COMPARE(a,b) ((a)->y0 < (b)->y0)

static void swnvg__insSortEdges(SWNVGedge* p, int n)
{
  int i,j;
  for (i=1; i < n; ++i) {
   SWNVGedge t = p[i], *a = &t;
   j = i;
   while (j > 0) {
     SWNVGedge *b = &p[j-1];
     int c = SWNVG__COMPARE(a,b);
     if (!c) break;
     p[j] = p[j-1];
     --j;
   }
   if (i != j)
     p[j] = t;
  }
}

static void swnvg__quickSortEdges(SWNVGedge* p, int n)
{
  /* threshold for transitioning to insertion sort */
  while (n > 12) {
   SWNVGedge t;
   int c01,c12,c,m,i,j;

   /* compute median of three */
   m = n >> 1;
   c01 = SWNVG__COMPARE(&p[0],&p[m]);
   c12 = SWNVG__COMPARE(&p[m],&p[n-1]);
   /* if 0 >= mid >= end, or 0 < mid < end, then use mid */
   if (c01 != c12) {
     /* otherwise, we'll need to swap something else to middle */
     int z;
     c = SWNVG__COMPARE(&p[0],&p[n-1]);
     /* 0>mid && mid<n: 0>n => n; 0<n => 0 */
     /* 0<mid && mid>n: 0>n => 0; 0<n => n */
     z = (c == c12) ? 0 : n-1;
     t = p[z];
     p[z] = p[m];
     p[m] = t;
   }
   /* now p[m] is the median-of-three */
   /* swap it to the beginning so it won't move around */
   t = p[0];
   p[0] = p[m];
   p[m] = t;

   /* partition loop */
   i=1;
   j=n-1;
   for(;;) {
     /* handling of equality is crucial here */
     /* for sentinels & efficiency with duplicates */
     for (;;++i) {
      if (!SWNVG__COMPARE(&p[i], &p[0])) break;
     }
     for (;;--j) {
      if (!SWNVG__COMPARE(&p[0], &p[j])) break;
     }
     /* make sure we haven't crossed */
     if (i >= j) break;
     t = p[i];
     p[i] = p[j];
     p[j] = t;

     ++i;
     --j;
   }
   /* recurse on smaller side, iterate on larger */
   if (j < (n-i)) {
     swnvg__quickSortEdges(p,j);
     p = p+i;
     n = n-i;
   } else {
     swnvg__quickSortEdges(p+i, n-i);
     n = j;
   }
  }
}

static void swnvg__sortCallEdges(SWNVGedge* p, int n)
{
  swnvg__quickSortEdges(p, n);
  swnvg__insSortEdges(p, n);
}

static void swnvg__sortEdges(void* arg)
{
  int i, nthreads;
  SWNVGthreadCtx* r = (SWNVGthreadCtx*)arg;
  SWNVGcontext* gl = r->context;
  nthreads = gl->xthreads*gl->ythreads;
  for (i = r->threadnum; i < gl->ncalls; i += nthreads) {
    SWNVGcall* call = &gl->calls[i];
    if(!(call->flags & NVG_PATH_XC))
      swnvg__sortCallEdges(&gl->edges[call->edgeOffset], call->edgeCount);
  }
}

static void swnvg__rasterize(void* arg)
{
  int i, j;
  SWNVGthreadCtx* r = (SWNVGthreadCtx*)arg;
  SWNVGcontext* gl = r->context;
  // setup - lineLimits array for XC rendering
  if(gl->covtex && !r->lineLimits) {
    int k, nlims = 2*(r->y1 - r->y0 + 1);
    r->lineLimits = (int*)malloc(nlims*sizeof(int));
    if (!r->lineLimits) return;
    for(k = 0; k < nlims; k += 2) {
      r->lineLimits[k] = gl->width;
      r->lineLimits[k+1] = 0;
    }
  }
  // render
  for (i = 0; i < gl->ncalls; i++) {
    SWNVGcall* call = &gl->calls[i];
    if(call->bounds[0] <= r->x1 && call->bounds[1] <= r->y1 && call->bounds[2] >= r->x0 && call->bounds[3] >= r->y0) {
      call->tex = swnvg__findTexture(gl, call->image);
      if(call->type == SWNVG_PAINT_ATLAS) {
        SWNVGtexture* tex = swnvg__findTexture(gl, call->image);
        NVGvertex* verts = &gl->verts[call->triangleOffset];
        for(j = 0; j < call->triangleCount; j += 2) {
          swnvg__rasterizeQuad(r, call, tex, &verts[j], &verts[j+1]);
        }
      } else {
        if(call->flags & NVG_PATH_XC)
          swnvg__rasterizeXC(r, call);
        else {
          swnvg__resetPool(r);
          r->freelist = NULL;
          swnvg__rasterizeSortedEdges(r, call);
        }
      }
    }
  }
}

static void swnvg__renderFlush(void* uptr)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  int i, nthreads = gl->xthreads*gl->ythreads;
  if (gl->ncalls == 0) return;
  // we assume dest buffer has already been cleared -- for(i = 0; i < h; i++) memset(&dst[i*stride], 0, w*4);
  if(nthreads > 1) {
    for(i = 0; i < nthreads; ++i)
      gl->poolSubmit(swnvg__sortEdges, &gl->threads[i]);
    gl->poolWait();
    for(i = 0; i < nthreads; ++i)
      gl->poolSubmit(swnvg__rasterize, &gl->threads[i]);
    gl->poolWait();
  }
  else {
    swnvg__sortEdges(gl->threads);
    swnvg__rasterize(gl->threads);
  }
  // Reset calls
  gl->nverts = 0;
  gl->nedges = 0;
  gl->ncalls = 0;
}

static SWNVGcall* swnvg__allocCall(SWNVGcontext* gl)
{
  SWNVGcall* ret = NULL;
  if (gl->ncalls+1 > gl->ccalls) {
    SWNVGcall* calls;
    int ccalls = swnvg__maxi(gl->ncalls+1, 128) + gl->ccalls/2; // 1.5x Overallocate
    calls = (SWNVGcall*)realloc(gl->calls, sizeof(SWNVGcall) * ccalls);
    if (calls == NULL) return NULL;
    gl->calls = calls;
    gl->ccalls = ccalls;
  }
  ret = &gl->calls[gl->ncalls++];
  memset(ret, 0, sizeof(SWNVGcall));
  return ret;
}

static int swnvg__allocVerts(SWNVGcontext* gl, int n)
{
  int ret = 0;
  if (gl->nverts+n > gl->cverts) {
    NVGvertex* verts;
    int cverts = swnvg__maxi(gl->nverts + n, 4096) + gl->cverts/2; // 1.5x Overallocate
    verts = (NVGvertex*)realloc(gl->verts, sizeof(NVGvertex) * cverts);
    if (verts == NULL) return -1;
    gl->verts = verts;
    gl->cverts = cverts;
  }
  ret = gl->nverts;
  gl->nverts += n;
  return ret;
}

// Note that w/ SW renderer, premultiplying by alpha doesn't provide any benefit
static rgba32_t swnvg__convertColor(SWNVGcontext* gl, NVGcolor c)
{
  // old way: if NVG_SRGB is set, c is assumed to be in linear RGB space (otherwise, in sRGB), in which case
  //  we convert (back) to sRGB so that opaque colors can be written quickly to output
  //int r = gl->flags & NVG_SRGB ? (int)(0.5f + powf(c.r, 1/sRGBgamma)*255.0f) : (int)(0.5f + c.r*255.0f);
  //int g = gl->flags & NVG_SRGB ? (int)(0.5f + powf(c.g, 1/sRGBgamma)*255.0f) : (int)(0.5f + c.g*255.0f);
  //int b = gl->flags & NVG_SRGB ? (int)(0.5f + powf(c.b, 1/sRGBgamma)*255.0f) : (int)(0.5f + c.b*255.0f);
  //return r << gl->rshift | g << gl->gshift | b << gl->bshift | ((rgba32_t)(c.a*255.0f + 0.5f) << gl->ashift);
  return c.r << gl->rshift | c.g << gl->gshift | c.b << gl->bshift | c.a << gl->ashift;
}

static int swnvg__convertPaint(SWNVGcontext* gl, SWNVGcall* call, NVGpaint* paint, NVGscissor* scissor, int flags)
{
  call->flags = flags | (gl->flags & NVG_SRGB);
  call->innerCol = swnvg__convertColor(gl, paint->innerColor);
  call->outerCol = swnvg__convertColor(gl, paint->outerColor);
  memcpy(call->extent, paint->extent, sizeof(call->extent));

  call->bounds[0] = 0; call->bounds[1] = 0; call->bounds[2] = gl->width-1; call->bounds[3] = gl->height-1;
  if (scissor->extent[0] > -0.5f && scissor->extent[1] > -0.5f) {
    // clip bounds to scissor rect
    if(scissor->xform[1] == 0 && scissor->xform[2] == 0) {
      float l0 = -scissor->extent[0], r0 = scissor->extent[0], l1, r1;
      float t0 = -scissor->extent[1], b0 = scissor->extent[1], t1, b1;
      nvgTransformPoint(&l1, &t1, scissor->xform, l0, t0);
      nvgTransformPoint(&r1, &b1, scissor->xform, r0, b0);
      // should we round or do floor and ceil?
      call->bounds[0] = swnvg__maxi(call->bounds[0], (int)(l1 + 0.5f));
      call->bounds[1] = swnvg__maxi(call->bounds[1], (int)(t1 + 0.5f));
      call->bounds[2] = swnvg__mini(call->bounds[2], (int)(r1 + 0.5f));
      call->bounds[3] = swnvg__mini(call->bounds[3], (int)(b1 + 0.5f));
    }
#ifndef NDEBUG
    // TODO: in this case we should at least transform all four corners of scissor box, then take the bbox of
    //  the four transformed points
    else
      NVG_LOG("nanovg_sw only supports axis aligned scissor!\n");
#endif
  }

  if (paint->image != 0) {
    call->image = paint->image;
    call->radius = paint->radius;  // distance offset for SDF text
    //tex = swnvg__findTexture(gl, paint->image);
    //if (tex == NULL) return 0;
    //if ((tex->flags & NVG_IMAGE_FLIPY) != 0) {
    //  float m1[6], m2[6];
    //  nvgTransformTranslate(m1, 0.0f, frag->extent[1] * 0.5f);
    //  nvgTransformMultiply(m1, paint->xform);
    //  nvgTransformScale(m2, 1.0f, -1.0f);
    //  nvgTransformMultiply(m2, m1);
    //  nvgTransformTranslate(m1, 0.0f, -frag->extent[1] * 0.5f);
    //  nvgTransformMultiply(m1, m2);
    //  nvgTransformInverse(invxform, m1);
    //} else {
      nvgTransformInverse(call->paintMat, paint->xform);
    //}
    call->type = SWNVG_PAINT_IMAGE;
  } else if (paint->innerColor.c != paint->outerColor.c) {
    call->type = SWNVG_PAINT_GRAD;
    call->radius = paint->radius;
    call->feather = paint->feather;
    nvgTransformInverse(call->paintMat, paint->xform);
  } else {
    call->type = SWNVG_PAINT_COLOR;
  }
  return 1;
}

static void swnvg__renderFill(void* uptr, NVGpaint* paint, NVGcompositeOperationState compositeOperation,
                NVGscissor* scissor, int flags, const float* bounds, const NVGpath* paths, int npaths)
{
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  int i, j, maxverts = 0;
  int ibounds[4] = { (int)bounds[0], (int)bounds[1], (int)(ceilf(bounds[2])), (int)(ceilf(bounds[3])) };
  SWNVGcall* call = NULL;
  for (i = 0; i < npaths; ++i)
    maxverts += paths[i].nfill;
  if (maxverts == 0) return;
  call = swnvg__allocCall(gl);
  if (call == NULL) return;

  swnvg__convertPaint(gl, call, paint, scissor, flags);
  if((gl->flags & NVGSW_PATHS_XC) && !(call->flags & NVG_PATH_NO_AA) && !(call->flags & NVG_PATH_EVENODD)) {
    call->flags |= NVG_PATH_XC;
    if(!gl->covtex) {
      size_t n = gl->width * gl->height * sizeof(float);
      gl->covtex = (float*)malloc(n);
      memset(gl->covtex, 0, n);
    }
  }
  // since bounds are inclusive, a bit tricky to distinguish totally clipped path later
  if (ibounds[0] > call->bounds[2] || ibounds[1] > call->bounds[3]
      || ibounds[2] < call->bounds[0] || ibounds[3] < call->bounds[1]) {
    --gl->ncalls;
    return;
  }
  call->bounds[0] = swnvg__maxi(ibounds[0], call->bounds[0]);
  call->bounds[1] = swnvg__maxi(ibounds[1], call->bounds[1]);
  call->bounds[2] = swnvg__mini(ibounds[2], call->bounds[2]);
  call->bounds[3] = swnvg__mini(ibounds[3], call->bounds[3]);

  call->triangleCount = 0;
  call->edgeOffset = gl->nedges;
  for (i = 0; i < npaths; ++i) {
    const NVGpath* path = &paths[i];
    for(j = 0; j < path->nfill; ++j)
      if(call->flags & NVG_PATH_XC)
        swnvg__addEdgeXC(gl, &path->fill[j], path->bounds[2]);
      else
        swnvg__addEdge(gl, &path->fill[j]);
  }
  call->edgeCount = gl->nedges - call->edgeOffset;
}

static void swnvg__renderTriangles(void* uptr, NVGpaint* paint, NVGcompositeOperationState compositeOperation,
    NVGscissor* scissor, const NVGvertex* verts, int nverts)
{
  int i;
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  SWNVGcall* call = swnvg__allocCall(gl);
  if (call == NULL) return;

  // Allocate vertices for all the paths.
  call->triangleCount = nverts/3;
  int offset = swnvg__allocVerts(gl, call->triangleCount);
  //if (offset == -1) goto error;
  call->triangleOffset = offset;

  for (i = 0; i < nverts; i += 6) {
    gl->verts[offset++] = verts[i];
    gl->verts[offset++] = verts[i+1];
  }
  swnvg__convertPaint(gl, call, paint, scissor, 0);
  call->type = SWNVG_PAINT_ATLAS;
  return;
}

static void swnvg__renderDelete(void* uptr)
{
  int ii, nthreads;
  SWNVGcontext* gl = (SWNVGcontext*)uptr;
  if (gl == NULL) return;

  nthreads = gl->xthreads*gl->ythreads;
  for(ii = 0; ii < nthreads; ++ii) {
    SWNVGmemPage* p = gl->threads[ii].pages;
    while (p != NULL) {
      SWNVGmemPage* next = p->next;
      free(p);
      p = next;
    }
    free(gl->threads[ii].scanline);
    free(gl->threads[ii].lineLimits);
  }
  free(gl->threads);
  free(gl->covtex);
  free(gl->textures);
  free(gl->verts);
  free(gl->calls);
  free(gl->edges);
  free(gl);
}

NVGcontext* nvgswCreate(int flags)
{
  NVGparams params;
  NVGcontext* ctx = NULL;
  SWNVGcontext* gl = (SWNVGcontext*)malloc(sizeof(SWNVGcontext));
  if (gl == NULL) goto error;
  memset(gl, 0, sizeof(SWNVGcontext));

  flags |= NVG_ROTATED_TEXT_AS_PATHS;  // we don't support rotated text with font atlas
  memset(&params, 0, sizeof(params));
  params.renderCreate = swnvg__renderCreate;
  params.renderCreateTexture = swnvg__renderCreateTexture;
  params.renderDeleteTexture = swnvg__renderDeleteTexture;
  params.renderUpdateTexture = swnvg__renderUpdateTexture;
  params.renderGetTextureSize = swnvg__renderGetTextureSize;
  params.renderViewport = swnvg__renderViewport;
  params.renderCancel = swnvg__renderCancel;
  params.renderFlush = swnvg__renderFlush;
  params.renderFill = swnvg__renderFill;
  params.renderTriangles = swnvg__renderTriangles;
  params.renderDelete = swnvg__renderDelete;
  params.userPtr = gl;
  params.flags = flags;

  gl->flags = flags;
  ctx = nvgCreateInternal(&params);
  if (ctx == NULL) goto error;
  // default (no threading) setup
  gl->xthreads = 1;
  gl->ythreads = 1;
  gl->threads = (SWNVGthreadCtx*)malloc(sizeof(SWNVGthreadCtx));
  if (gl->threads == NULL) goto error;
  memset(gl->threads, 0, sizeof(SWNVGthreadCtx));
  gl->threads[0].threadnum = 0;
  gl->threads[0].context = gl;

  return ctx;
error:
  // 'gl' is freed by swnvg__renderDelete via nvgDeleteInternal
  if (ctx != NULL) nvgDeleteInternal(ctx);
  return NULL;
}

void nvgswSetThreading(NVGcontext* vg, int xthreads, int ythreads, poolSubmit_t submit, poolWait_t wait)
{
  SWNVGcontext* gl = (SWNVGcontext*)nvgInternalParams(vg)->userPtr;
  int i, nthreads = xthreads*ythreads;
  if (nthreads < 2 || gl->bitmap) return;  // can't call this fn after setFramebuffer
  gl->threads = (SWNVGthreadCtx*)realloc(gl->threads, sizeof(SWNVGthreadCtx) * nthreads);
  if (gl->threads == NULL) return;
  memset(gl->threads, 0, sizeof(SWNVGthreadCtx) * nthreads);
  for (i = 0; i < nthreads; ++i) {
    gl->threads[i].threadnum = i;
    gl->threads[i].context = gl;
  }
  gl->xthreads = xthreads;
  gl->ythreads = ythreads;
  gl->poolSubmit = submit;
  gl->poolWait = wait;
  NVG_LOG("nvg2: %d x %d threads\n", xthreads, ythreads);
}

void nvgswSetFramebuffer(NVGcontext* vg, void* dest, int w, int h, int rshift, int gshift, int bshift, int ashift)
{
  int ii, jj;
  SWNVGcontext* gl = (SWNVGcontext*)nvgInternalParams(vg)->userPtr;
  if(gl->covtex && (w != gl->width || h != gl->height)) {
    free(gl->covtex);
    gl->covtex = NULL;
  }
  gl->bitmap = (unsigned char*)dest;  gl->width = w;  gl->height = h;  gl->stride = 4*w;
  gl->rshift = rshift;  gl->gshift = gshift;  gl->bshift = bshift;  gl->ashift = ashift;

  int threadw = w/gl->xthreads + 1;
  int threadh = h/gl->ythreads + 1;
  for (jj = 0; jj < gl->ythreads; ++jj) {
    for (ii = 0; ii < gl->xthreads; ++ii) {
      SWNVGthreadCtx* r = &gl->threads[jj*gl->xthreads + ii];
      r->x0 = ii*threadw;
      r->y0 = jj*threadh;
      r->x1 = swnvg__mini(w, r->x0 + threadw) - 1;
      r->y1 = swnvg__mini(h, r->y0 + threadh) - 1;
      if (r->x1 - r->x0 + 1 > r->cscanline) {
        r->cscanline = r->x1 - r->x0 + 1;
        r->scanline = (unsigned char*)realloc(r->scanline, r->cscanline);
        if (r->scanline == NULL) return;
        memset(r->scanline, 0, r->cscanline);
      }
      // reset lineLimits whenever covtex is reset (whenever FB dimensions change)
      if(r->lineLimits && !gl->covtex) {
        free(r->lineLimits);
        r->lineLimits = NULL;
      }
    }
  }
}

void nvgswDelete(NVGcontext* ctx)
{
  nvgDeleteInternal(ctx);
}

#endif /* NANOVG_SW_IMPLEMENTATION */
