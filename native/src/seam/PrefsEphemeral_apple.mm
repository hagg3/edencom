// PrefsEphemeral_apple.mm — X.21. On Apple targets NSUserDefaults is the real Foundation one (the
// user's ~/Library/Preferences/<exe>.plist), so a `--*-selftest` read the developer's stored settings
// (X.19: Input mode) and wrote its own back (console history, ...). eden_prefs_set_ephemeral(1) makes
// +standardUserDefaults answer a private, empty suite instead, deleted at exit. Call before the first
// defaults access. (The non-Apple twin is a flag inside NSUserDefaults_native.mm.)
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <cstdlib>
#include <unistd.h>

static NSString* g_ephemeralSuite = nil;
static NSUserDefaults* g_ephemeralDefaults = nil;

static void eden_prefs_ephemeral_cleanup(void) {
    if (!g_ephemeralSuite) return;
    [g_ephemeralDefaults removePersistentDomainForName:g_ephemeralSuite];
    [g_ephemeralDefaults synchronize];
    NSString* path = [NSString stringWithFormat:@"%@/Library/Preferences/%@.plist", NSHomeDirectory(), g_ephemeralSuite];
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
}

extern "C" void eden_prefs_set_ephemeral(int on) {
    if (!on || g_ephemeralSuite) return;
    g_ephemeralSuite = [[NSString stringWithFormat:@"emod.selftest.%d", (int)getpid()] retain];
    NSUserDefaults* suite = g_ephemeralDefaults = [[NSUserDefaults alloc] initWithSuiteName:g_ephemeralSuite];
    Method m = class_getClassMethod([NSUserDefaults class], @selector(standardUserDefaults));
    method_setImplementation(m, imp_implementationWithBlock(^id(id) { return suite; }));
    atexit(eden_prefs_ephemeral_cleanup);
}
