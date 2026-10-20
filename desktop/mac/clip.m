/* macOS clipboard (plain text) through NSPasteboard; its changeCount is the
 * sequence number the agent watches. */
#import <AppKit/AppKit.h>

#include <stdlib.h>
#include <string.h>

#include "desk.h"

unsigned long dsk_clip_seq(void)
{
    @autoreleasepool {
        return (unsigned long)[[NSPasteboard generalPasteboard] changeCount];
    }
}

char *dsk_clip_get(void)
{
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return s ? strdup(s.UTF8String ?: "") : NULL;
    }
}

int dsk_clip_set(const char *utf8)
{
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb setString:[NSString stringWithUTF8String:utf8] ?: @"" forType:NSPasteboardTypeString] ? 0 : -1;
    }
}
