#import <Cocoa/Cocoa.h>
#include <stdbool.h>
#include <stdio.h>

bool native_dialog_choose_source(bool choose_iso_only, const char *initial_path,
                                 char *out_path, size_t out_len) {
    (void)choose_iso_only;
    if (!out_path || out_len == 0) return false;
    out_path[0] = '\0';

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setResolvesAliases:YES];
        [panel setTitle:@"Select DVD Source"];
        [panel setMessage:@"Choose a DVD folder (VIDEO_TS / backup), mounted disc, or .iso file"];
        [panel setPrompt:@"Select"];

        if (initial_path && initial_path[0]) {
            NSString *initStr = [NSString stringWithUTF8String:initial_path];
            if (initStr) {
                BOOL isDir = NO;
                if ([[NSFileManager defaultManager] fileExistsAtPath:initStr isDirectory:&isDir]) {
                    NSURL *dirURL = isDir ? [NSURL fileURLWithPath:initStr]
                                          : [[NSURL fileURLWithPath:initStr] URLByDeletingLastPathComponent];
                    [panel setDirectoryURL:dirURL];
                }
            }
        }

        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) {
                const char *utf8 = [[url path] UTF8String];
                if (utf8) {
                    snprintf(out_path, out_len, "%s", utf8);
                    return true;
                }
            }
        }
    }
    return false;
}
