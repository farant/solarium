#import <Cocoa/Cocoa.h>
#include "platform_clipboard.h"
#include <stdlib.h>
#include <string.h>

sol_bool clipboard_read_image(unsigned char **out_bytes, int *out_len) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSData       *png = nil;

        /* Prefer an existing PNG representation. */
        NSData *direct = [pb dataForType:NSPasteboardTypePNG];
        if (direct && [direct length] > 0) {
            png = direct;
        } else {
            /* Fall back: read TIFF / an NSImage and re-encode to PNG. */
            NSData *tiff = [pb dataForType:NSPasteboardTypeTIFF];
            if (!tiff || [tiff length] == 0) {
                NSArray *imgs = [pb readObjectsForClasses:@[[NSImage class]] options:nil];
                if (imgs && [imgs count] > 0) {
                    NSImage *img = [imgs objectAtIndex:0];
                    tiff = [img TIFFRepresentation];
                }
            }
            if (tiff && [tiff length] > 0) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:tiff];
                if (rep)
                    png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
            }
        }

        if (!png || [png length] == 0) return SOL_FALSE;

        {
            NSUInteger     n   = [png length];
            unsigned char *buf = (unsigned char *)malloc(n);
            if (!buf) return SOL_FALSE;
            memcpy(buf, [png bytes], n);
            *out_bytes = buf;
            *out_len   = (int)n;
        }
        return SOL_TRUE;
    }
}
