//
//  Header.h
//  
//
//  Created by Radzivon Bartoshyk on 05/01/2023.
//

#ifndef CSvgWriter_h
#define CSvgWriter_h

#import "TargetConditionals.h"

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#define PlatformImage   NSImage
#else
#import <UIKit/UIKit.h>
#define PlatformImage   UIImage
#endif

@interface CSvgWriter : NSObject
+(nullable NSData*) createSVG:(nonnull NSData*)image error:(NSError *_Nullable * _Nullable)error;
@end

#endif /* Header_h */
