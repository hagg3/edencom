#import "NSLogSupport.h"
#import "NSString.h"
#include <cstdio>
#include <cstdarg>

void NSLog(NSString *format, ...) {
    if (!format) return;
    va_list args;
    va_start(args, format);
    std::string msg = eden_format_nsstring([format UTF8String], args);
    va_end(args);
    // TODO P1: real NSLog prefixes a timestamp + process name; the engine's own call sites
    // never depend on that prefix being present (they're diagnostic prints), so it's skipped.
    fprintf(stderr, "%s\n", msg.c_str());
}
