import svgwriterc
#if !os(macOS)
import UIKit.UIImage
import UIKit.UIColor
/// Alias for `UIImage`.
public typealias PlatformImage = UIImage
#else
import AppKit.NSImage
/// Alias for `NSImage`.
public typealias PlatformImage = NSImage
#endif
import pngquant
import mozjpeg

public struct SvgWriter {

    private static func ensure8BitImage(image: PlatformImage) throws -> PlatformImage {
        guard let imageRef = image.cgImage else {
            throw SvgWriterError()
        }
        let width = imageRef.width
        let height = imageRef.height
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bytesPerPixel = 4
        let bytesPerRow = bytesPerPixel * width
        let bitsPerComponent = 8
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)

        guard let context = CGContext(data: nil, width: width, height: height,
                                      bitsPerComponent: bitsPerComponent, bytesPerRow: bytesPerRow, space: colorSpace, bitmapInfo: bitmapInfo.rawValue) else {
            throw SvgWriterError()
        }
        context.draw(imageRef, in: CGRect(x: 0, y: 0, width: width, height: height))
        guard let image = context.makeImage() else {
            throw SvgWriterError()
        }
        return PlatformImage(cgImage: image)
    }

    public static func encodePNG(image: PlatformImage) throws -> Data {
        let pngData = try image.pngQuantData(speed: 9)
        return try CSvgWriter.createSVG(pngData)
    }

    public static func encodeJPG(image: PlatformImage) throws -> Data {
        let jpegData = try image.mozjpegRepresentation(quality: 0.8, progressive: false, premultiply: true)
        return try CSvgWriter.createSVG(jpegData)
    }

    public struct SvgWriterError: LocalizedError {
        public var errorDescription: String? {
            "Can't encode image"
        }
    }
}
