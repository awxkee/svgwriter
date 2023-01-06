// swift-tools-version: 5.7
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "svgwriter",
    platforms: [.iOS(.v14), .macOS(.v12), .macCatalyst(.v14)],
    products: [
        .library(
            name: "svgwriter",
            targets: ["svgwriter"]),
    ],
    dependencies: [
        .package(url: "https://github.com/awxkee/pngquant.swift.git", "1.0.0"..<"2.0.0"),
        .package(url: "https://github.com/awxkee/mozjpeg.swift.git", "1.0.0"..<"2.0.0")
    ],
    targets: [
        .target(
            name: "svgwriter",
            dependencies: ["svgwriterc"]),
        .target(name: "svgwriterc",
                dependencies: [.product(name: "pngquant", package: "pngquant.swift"),
                               .product(name: "mozjpeg", package: "mozjpeg.swift")],
                publicHeadersPath: ".",
                cSettings: [.define("NO_PAINTER_GL"), .define("PUGIXML_NO_EXCEPTIONS"), .define("PUGIXML_NO_XPATH"), .define("NO_MINIZ")],
                cxxSettings: [.define("NO_PAINTER_GL"), .define("PUGIXML_NO_EXCEPTIONS"), .define("PUGIXML_NO_XPATH"), .define("NO_MINIZ"), .headerSearchPath(".")]),
        .testTarget(
            name: "svg-writerTests",
            dependencies: ["svgwriter"]),
    ],
    cxxLanguageStandard: .cxx17
)
