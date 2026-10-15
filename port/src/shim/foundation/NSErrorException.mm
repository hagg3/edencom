#import "NSErrorException.h"
#import "NSString.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

NSString *const NSInternalInconsistencyException = @"NSInternalInconsistencyException";

@implementation NSError {
@public
    NSString *_reason;
}
+ (NSError *)errorWithDomain:(NSString *)domain code:(NSInteger)code userInfo:(NSDictionary *)info {
    (void)domain; (void)code; (void)info;
    return [[[NSError alloc] init] autorelease];
}
- (NSString *)localizedDescription { return _reason; }
@end

@implementation NSException

+ (NSException *)exceptionWithName:(NSString *)name reason:(NSString *)reason
                           userInfo:(NSDictionary *)info {
    (void)info;
    fprintf(stderr, "NSException: %s: %s\n", name ? [name UTF8String] : "(nil)",
            reason ? [reason UTF8String] : "(nil)");
    return [[[NSException alloc] init] autorelease];
}

+ (void)raise:(NSString *)name format:(NSString *)fmt, ... {
    // TODO P1: real Foundation unwinds via @throw; this shim has no @try/@catch support (see
    // header). Anything that reaches this call site today crashes the process, matching what
    // an *uncaught* NSException would eventually do anyway.
    fprintf(stderr, "NSException raised: %s: ", name ? [name UTF8String] : "(nil)");
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, [fmt UTF8String], args);
        va_end(args);
    }
    fprintf(stderr, "\n");
    abort();
}

- (NSString *)reason { return nil; }

@end
