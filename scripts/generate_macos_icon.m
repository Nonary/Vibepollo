#import <Cocoa/Cocoa.h>

void renderIcon(int size, NSString *outputPath) {
    @autoreleasepool {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:size
                          pixelsHigh:size
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:size * 4
                        bitsPerPixel:32];

        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext setCurrentContext:ctx];
        CGContextRef cg = (CGContextRef)[ctx CGContext];

        CGFloat scale = (CGFloat)size / 1024.0;

        // 1. Draw outer shadow for squircle
        CGFloat tileOrigin = 100.0 * scale;
        CGFloat tileSize = 824.0 * scale;
        CGFloat cornerRadius = 185.0 * scale;
        CGRect tileRect = CGRectMake(tileOrigin, tileOrigin, tileSize, tileSize);

        CGContextSaveGState(cg);
        CGColorRef shadowColor = CGColorCreateGenericRGB(0.0, 0.05, 0.15, 0.35);
        CGContextSetShadowWithColor(cg, CGSizeMake(0, -18 * scale), 32 * scale, shadowColor);
        CGColorRelease(shadowColor);

        NSBezierPath *tilePath = [NSBezierPath bezierPathWithRoundedRect:NSRectFromCGRect(tileRect)
                                                                 xRadius:cornerRadius
                                                                 yRadius:cornerRadius];
        [[NSColor colorWithCalibratedRed:0.05 green:0.25 blue:0.65 alpha:1.0] setFill];
        [tilePath fill];
        CGContextRestoreGState(cg);

        // 2. Draw gradient inside squircle tile
        CGContextSaveGState(cg);
        [tilePath addClip];

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGFloat locations[3] = {0.0, 0.55, 1.0};
        CGFloat components[12] = {
            0.02, 0.12, 0.35, 1.0,  // Bottom: Deep rich navy
            0.06, 0.32, 0.78, 1.0,  // Middle: Vibrant royal cobalt
            0.12, 0.58, 0.98, 1.0   // Top: Radiant azure/electric blue
        };
        CGGradientRef gradient = CGGradientCreateWithColorComponents(colorSpace, components, locations, 3);
        CGContextDrawLinearGradient(cg, gradient,
                                    CGPointMake(size / 2.0, tileOrigin),
                                    CGPointMake(size / 2.0, tileOrigin + tileSize),
                                    0);
        CGGradientRelease(gradient);

        // Subtle top inner rim highlight (specular reflection)
        CGContextSaveGState(cg);
        CGContextSetLineWidth(cg, 3.0 * scale);
        CGColorRef rimColor = CGColorCreateGenericRGB(1.0, 1.0, 1.0, 0.28);
        CGContextSetStrokeColorWithColor(cg, rimColor);
        CGColorRelease(rimColor);
        [tilePath stroke];
        CGContextRestoreGState(cg);

        CGContextRestoreGState(cg); // End tile clip

        // 3. Draw Apollo starburst emblem
        NSImage *svg = [[NSImage alloc] initWithContentsOfFile:@"apollo.svg"];
        if (svg) {
            CGFloat emblemSize = 540.0 * scale;
            CGFloat emblemOrigin = (size - emblemSize) / 2.0;
            NSRect emblemRect = NSMakeRect(emblemOrigin, emblemOrigin, emblemSize, emblemSize);

            CGContextSaveGState(cg);
            CGColorRef emblemShadow = CGColorCreateGenericRGB(0.01, 0.05, 0.20, 0.55);
            CGContextSetShadowWithColor(cg, CGSizeMake(0, -10 * scale), 20 * scale, emblemShadow);
            CGColorRelease(emblemShadow);

            [svg drawInRect:emblemRect
                   fromRect:NSZeroRect
                  operation:NSCompositingOperationSourceOver
                   fraction:1.0];

            CGContextRestoreGState(cg);
        }

        [NSGraphicsContext setCurrentContext:nil];

        NSData *pngData = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [pngData writeToFile:outputPath atomically:YES];
    }
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSString *iconset = @"/tmp/Vibepollo.iconset";
        NSFileManager *fm = [NSFileManager defaultManager];
        [fm removeItemAtPath:iconset error:nil];
        [fm createDirectoryAtPath:iconset withIntermediateDirectories:YES attributes:nil error:nil];

        struct IconSpec {
            const char *name;
            int size;
        } specs[] = {
            {"icon_16x16.png", 16},
            {"icon_16x16@2x.png", 32},
            {"icon_32x32.png", 32},
            {"icon_32x32@2x.png", 64},
            {"icon_128x128.png", 128},
            {"icon_128x128@2x.png", 256},
            {"icon_256x256.png", 256},
            {"icon_256x256@2x.png", 512},
            {"icon_512x512.png", 512},
            {"icon_512x512@2x.png", 1024}
        };

        for (int i = 0; i < sizeof(specs)/sizeof(specs[0]); i++) {
            NSString *file = [iconset stringByAppendingPathComponent:[NSString stringWithUTF8String:specs[i].name]];
            renderIcon(specs[i].size, file);
            NSLog(@"Rendered %s (%dx%d)", specs[i].name, specs[i].size, specs[i].size);
        }

        // Also render top-level 1024x1024 png
        renderIcon(1024, @"apollo.png");
        NSLog(@"Rendered apollo.png (1024x1024)");
    }
    return 0;
}
