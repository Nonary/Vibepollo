// Generates the macOS app icon: the white Apollo star on the web UI's indigo/violet accent.
// Usage (from the repo root):
//   swift src_assets/macos/build/make-icon.swift apollo.png /tmp/vibepollo.iconset
//   iconutil -c icns -o src_assets/macos/build/vibepollo.icns /tmp/vibepollo.iconset
import CoreGraphics
import Foundation
import ImageIO

// Web UI accent tokens (src_assets/common/assets/web/design/tokens.json).
let gradientStart = 0xA78BFA  // accent hover, violet (top left)
let gradientEnd = 0x818CF8  // accent default, indigo (bottom right)

// macOS icon grid on a 1024pt canvas: 824pt body, 100pt margin.
let bodyRect = CGRect(x: 100, y: 100, width: 824, height: 824)
let cornerRadius: CGFloat = 185.4
let glyphSize: CGFloat = 620

let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

func color(_ hex: Int) -> CGColor {
  CGColor(
    colorSpace: srgb,
    components: [
      CGFloat((hex >> 16) & 0xff) / 255, CGFloat((hex >> 8) & 0xff) / 255, CGFloat(hex & 0xff) / 255, 1,
    ])!
}

let args = CommandLine.arguments
guard args.count == 3,
  let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil),
  let glyph = CGImageSourceCreateImageAtIndex(source, 0, nil)
else {
  FileHandle.standardError.write("usage: make-icon.swift <glyph.png> <out.iconset>\n".data(using: .utf8)!)
  exit(1)
}
let outDir = URL(fileURLWithPath: args[2])
try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

func render(pixels: Int) -> CGImage {
  let ctx = CGContext(
    data: nil, width: pixels, height: pixels, bitsPerComponent: 8, bytesPerRow: 0, space: srgb,
    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
  ctx.scaleBy(x: CGFloat(pixels) / 1024, y: CGFloat(pixels) / 1024)
  ctx.interpolationQuality = .high

  ctx.saveGState()
  ctx.addPath(CGPath(roundedRect: bodyRect, cornerWidth: cornerRadius, cornerHeight: cornerRadius, transform: nil))
  ctx.clip()
  let gradient = CGGradient(
    colorsSpace: srgb, colors: [color(gradientStart), color(gradientEnd)] as CFArray, locations: [0, 1])!
  ctx.drawLinearGradient(
    gradient, start: CGPoint(x: bodyRect.minX, y: bodyRect.maxY), end: CGPoint(x: bodyRect.maxX, y: bodyRect.minY),
    options: [])
  ctx.restoreGState()

  ctx.draw(glyph, in: CGRect(x: 512 - glyphSize / 2, y: 512 - glyphSize / 2, width: glyphSize, height: glyphSize))
  return ctx.makeImage()!
}

let variants: [(name: String, pixels: Int)] = [
  ("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128),
  ("128x128@2x", 256), ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024),
]
for variant in variants {
  let url = outDir.appendingPathComponent("icon_\(variant.name).png")
  let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil)!
  CGImageDestinationAddImage(dest, render(pixels: variant.pixels), nil)
  guard CGImageDestinationFinalize(dest) else { fatalError("failed to write \(url.path)") }
}
