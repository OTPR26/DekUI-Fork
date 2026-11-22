#!/usr/bin/env swift
// SPDX-License-Identifier: GPL-2.0-or-later
// Run from the repository root to regenerate the Apple, Windows and Linux launcher icons.
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let source = root.appendingPathComponent("src/android/app/src/main/res/drawable-nodpi")
let output = root.appendingPathComponent("resources/mm3d/launcher-icons")
let size = 1024
let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!

func context(_ size: Int, opaque: Bool = false) -> CGContext {
    CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
              bytesPerRow: 0, space: colorSpace,
              bitmapInfo: (opaque ? CGImageAlphaInfo.noneSkipLast : .premultipliedLast).rawValue)!
}

func png(_ image: CGImage, at url: URL) {
    let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, image, nil)
    precondition(CGImageDestinationFinalize(destination))
}

func resized(_ image: CGImage, to pixels: Int) -> CGImage {
    let target = context(pixels)
    target.interpolationQuality = .high
    target.draw(image, in: CGRect(x: 0, y: 0, width: pixels, height: pixels))
    return target.makeImage()!
}

func pngBytes(_ image: CGImage) -> Data {
    let bytes = NSMutableData()
    let destination = CGImageDestinationCreateWithData(bytes, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, image, nil)
    precondition(CGImageDestinationFinalize(destination))
    return bytes as Data
}

func appendLE<T: FixedWidthInteger>(_ number: T, to data: inout Data) {
    var littleEndian = number.littleEndian
    withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
}

var primary: CGImage!
let gradient = CGGradient(colorsSpace: colorSpace,
    colors: [CGColor(red: 0.16, green: 0.106, blue: 0.24, alpha: 1),
             CGColor(red: 0.032, green: 0.019, blue: 0.052, alpha: 1)] as CFArray,
    locations: nil)!
for index in 0..<5 {
    let url = source.appendingPathComponent("launcher_icon_\(index).png")
    let input = CGImageSourceCreateWithURL(url as CFURL, nil)!
    let artwork = CGImageSourceCreateImageAtIndex(input, 0, nil)!
        .cropping(to: CGRect(x: 72, y: 72, width: 288, height: 288))!
    let image = context(size)
    let plate = CGRect(x: 96, y: 96, width: 832, height: 832)
    let shape = CGPath(roundedRect: plate, cornerWidth: 177, cornerHeight: 177, transform: nil)
    image.saveGState()
    image.setShadow(offset: CGSize(width: 0, height: -15), blur: 28,
                    color: CGColor(gray: 0, alpha: 0.28))
    image.addPath(shape)
    image.setFillColor(CGColor(red: 0.035, green: 0.021, blue: 0.061, alpha: 1))
    image.fillPath()
    image.restoreGState()

    image.addPath(shape)
    image.clip()
    image.drawLinearGradient(gradient, start: CGPoint(x: 512, y: plate.maxY),
                             end: CGPoint(x: 512, y: plate.minY), options: [])
    image.interpolationQuality = .high
    image.draw(artwork, in: plate.insetBy(dx: 48, dy: 48))
    image.addPath(shape)
    image.setStrokeColor(CGColor(gray: 1, alpha: 0.14))
    image.setLineWidth(4)
    image.strokePath()

    let final = image.makeImage()!
    png(final, at: output.appendingPathComponent("mac-icon-\(index).png"))
    if index == 0 { primary = final }

    // iOS masks the opaque artwork to the appropriate Home Screen shape.
    let mobile = context(size, opaque: true)
    let canvas = CGRect(x: 0, y: 0, width: size, height: size)
    mobile.drawLinearGradient(gradient, start: CGPoint(x: canvas.midX, y: canvas.maxY),
                               end: CGPoint(x: canvas.midX, y: canvas.minY), options: [])
    mobile.interpolationQuality = .high
    mobile.draw(artwork, in: canvas.insetBy(dx: 100, dy: 100))
    png(mobile.makeImage()!, at: output.appendingPathComponent("ios-icon-\(index).png"))
}

let temp = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
let iconset = temp.appendingPathComponent("ReMasked.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: temp) }
for pointSize in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = pointSize * scale
        let suffix = scale == 2 ? "@2x" : ""
        png(resized(primary, to: pixels), at: iconset.appendingPathComponent("icon_\(pointSize)x\(pointSize)\(suffix).png"))
    }
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", "-o", root.appendingPathComponent("dist/apple/ReMasked.icns").path, iconset.path]
try iconutil.run()
iconutil.waitUntilExit()
precondition(iconutil.terminationStatus == 0)

let dist = root.appendingPathComponent("dist")
png(resized(primary, to: 512), at: dist.appendingPathComponent("ReMasked.png"))

// Match the size ladder used by the running Windows app and its shortcuts.
let sizes = [16, 24, 32, 48, 64, 128, 256]
let frames = sizes.map { pngBytes(resized(primary, to: $0)) }
var ico = Data([0, 0, 1, 0])
appendLE(UInt16(frames.count), to: &ico)
var offset = 6 + 16 * frames.count
for (pixels, frame) in zip(sizes, frames) {
    let dimension = UInt8(truncatingIfNeeded: pixels)
    ico.append(contentsOf: [dimension, dimension, 0, 0])
    appendLE(UInt16(1), to: &ico)
    appendLE(UInt16(32), to: &ico)
    appendLE(UInt32(frame.count), to: &ico)
    appendLE(UInt32(offset), to: &ico)
    offset += frame.count
}
for frame in frames { ico.append(frame) }
try ico.write(to: dist.appendingPathComponent("ReMasked.ico"), options: .atomic)
