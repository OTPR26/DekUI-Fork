// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic platform asset sizing of supplied artwork; no image generation.
import AppKit
import ImageIO
import UniformTypeIdentifiers
let args = CommandLine.arguments
if args.count != 6 { fatalError("usage: apple_tv_icon <source> <output> <width> <height> <art|clear>") }
let width = Int(args[3])!, height = Int(args[4])!
let context = CGContext(data:nil,width:width,height:height,bitsPerComponent:8,bytesPerRow:width*4,
    space:CGColorSpaceCreateDeviceRGB(),bitmapInfo:CGImageAlphaInfo.premultipliedLast.rawValue)!
if args[5] == "art" {
    let source = CGImageSourceCreateWithURL(URL(fileURLWithPath:args[1]) as CFURL,nil)!
    let image = CGImageSourceCreateImageAtIndex(source,0,nil)!
    context.setFillColor(CGColor(red:0.02,green:0.035,blue:0.14,alpha:1))
    context.fill(CGRect(x:0,y:0,width:width,height:height))
    // Apple TV icons are 5:3; fill the frame with the supplied 2:1 art.
    // A centered crop keeps the original pixels/proportions and removes bars.
    let scale = max(Double(width)/Double(image.width),Double(height)/Double(image.height))
    let w=Double(image.width)*scale,h=Double(image.height)*scale
    context.interpolationQuality = .high
    context.draw(image,in:CGRect(x:(Double(width)-w)/2,y:(Double(height)-h)/2,width:w,height:h))
}
let output = CGImageDestinationCreateWithURL(URL(fileURLWithPath:args[2]) as CFURL,UTType.png.identifier as CFString,1,nil)!
CGImageDestinationAddImage(output,context.makeImage()!,nil)
if !CGImageDestinationFinalize(output) {fatalError("Cannot write icon asset")}
