// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Makes the iOS app icon from any image macOS reads: 1024x1024, drawn onto white, since iOS shows the transparent
// parts of an icon as black. Used by CMakeLists.txt (from icon.png) and by ios/install-on-iphone.sh --icon.
//
//   xcrun swift ios/flatten-icon.swift <image> <out.png>

import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 3, let image = NSImage(contentsOfFile: arguments[1]) else {
	FileHandle.standardError.write("usage: flatten-icon.swift <image> <out.png>, with an image macOS can read\n".data(using: .utf8)!)
	exit(1)
}
let side = 1024
guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: side, pixelsHigh: side, bitsPerSample: 8,
		samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
	let context = NSGraphicsContext(bitmapImageRep: bitmap) else { exit(1) }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
let square = NSRect(x: 0, y: 0, width: side, height: side)
NSColor.white.setFill()
square.fill()
image.draw(in: square, from: .zero, operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()
guard let png = bitmap.representation(using: .png, properties: [:]) else { exit(1) }
do { try png.write(to: URL(fileURLWithPath: arguments[2])) } catch { exit(1) }
