//
//  CSvgWriter.m
//  
//
//  Created by Radzivon Bartoshyk on 05/01/2023.
//

#import <Foundation/Foundation.h>
#include "CSvgWriter.h"
#define PLATFORMUTIL_IMPLEMENTATION
#define NANOVG_SW_IMPLEMENTATION
#define STRINGUTIL_IMPLEMENTATION
#include "pugixml/pugixml.hxx"
#include "ulib/geom.hxx"
#include "ulib/stringutil.hxx"
#include "ulib/platformutil.hxx"
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_sw.h"
#include "usvg/svgwriter.hxx"
#include "usvg/svgpainter.hxx"
#include "usvg/svgnode.hxx"
#include <mutex>

static void releaseNVGContext(NVGcontext * ctx) {
    nvgswDelete(ctx);
}

std::mutex nvgMutex;

@implementation CSvgWriter {
}

+(nullable NSData*) createSVG:(nonnull NSData*)image error:(NSError *_Nullable * _Nullable)error {
    Image refImage = Image::decodeBuffer((const unsigned char*)image.bytes, image.length);
    if (refImage.width <= 0 || refImage.height <= 0) {
        *error = [[NSError alloc] initWithDomain:@"CSvgWriter" code:500 userInfo:@{ NSLocalizedDescriptionKey: @"Decoding image was failed" }];
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(nvgMutex);
    std::shared_ptr<NVGcontext> nvgContext(nvgswCreate(NVG_AUTOW_DEFAULT | NVG_IMAGE_SRGB), releaseNVGContext);
    Painter::vg = nvgContext.get();
    auto document = new SvgDocument(0, 0, refImage.width, refImage.height);
    Image frameImage(refImage.width, refImage.height);
    auto imgPtr = new SvgImage(refImage, SVGRect::ltwh(0, 0, refImage.width, refImage.height));
    document->addChild(imgPtr);

#if DEBUG
    SvgWriter::DEBUG_CSS_STYLE = true;
#endif
    XmlStreamWriter xmlwriter;
    SvgWriter(xmlwriter).serialize(document);
    std::stringstream ss;
    xmlwriter.save(ss);
    std::string str = ss.str();
    auto nsString = [[NSString alloc] initWithUTF8String:str.c_str()];
    delete document;
    return [nsString dataUsingEncoding:NSUTF8StringEncoding];
}

@end
