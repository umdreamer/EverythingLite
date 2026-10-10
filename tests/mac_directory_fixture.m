// Standard AppKit panels on synthetic temporary directories only.
// No activation, real user files, accessibility requests, or saved files.
#import <AppKit/AppKit.h>
#include <unistd.h>

static BOOL validDirectory(NSString *path) {
    if (!([path hasPrefix:@"/private/tmp/"] || [path hasPrefix:@"/tmp/"])) return NO;
    if ([path hasSuffix:@"/"] || [path containsString:@"//"]) return NO;
    for (NSString *component in path.pathComponents)
        if ([component isEqualToString:@"."] || [component isEqualToString:@".."]) return NO;
    return YES;
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSString *kind = [NSString stringWithUTF8String:argv[1]];
        NSString *directory = [NSString stringWithUTF8String:argv[2]];
        if (!directory || !validDirectory(directory) ||
            !([kind isEqualToString:@"open"] || [kind isEqualToString:@"save"] || [kind isEqualToString:@"folder"])) return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        NSSavePanel *panel;
        const BOOL saving = [kind isEqualToString:@"save"];
        if (saving) {
            panel = [NSSavePanel savePanel];
            panel.nameFieldStringValue = @"EL_SYNTHETIC_SAVE_SENTINEL.txt";
        } else {
            NSOpenPanel *openPanel = [NSOpenPanel openPanel];
            openPanel.canChooseFiles = ![kind isEqualToString:@"folder"];
            openPanel.canChooseDirectories = [kind isEqualToString:@"folder"];
            openPanel.allowsMultipleSelection = NO;
            panel = openPanel;
        }
        NSURL *expected = [NSURL fileURLWithPath:directory isDirectory:YES];
        panel.directoryURL = expected;
        [panel beginWithCompletionHandler:^(NSModalResponse response) {
            (void)response;
            [NSApp stop:nil];
            exit(0);
        }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            const BOOL directoryMatches = [panel.directoryURL.path isEqualToString:directory];
            const BOOL saveNameMatches = saving && [panel.nameFieldStringValue isEqualToString:@"EL_SYNTHETIC_SAVE_SENTINEL.txt"];
            printf("{\"protocol\":1,\"pid\":%d,\"kind\":\"%s\",\"directory_matches\":%s,\"save_name_matches\":%s}\n",
                   getpid(), saving ? "save" : ([kind isEqualToString:@"folder"] ? "folder" : "open"),
                   directoryMatches ? "true" : "false", saveNameMatches ? "true" : "false");
            fflush(stdout);
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [panel cancel:nil];
            exit(0);
        });
        [NSApp run];
    }
    return 0;
}
