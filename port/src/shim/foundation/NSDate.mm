#import "NSDate.h"
#import "NSString.h"
#include <chrono>

static double eden_now_seconds() {
    // TODO P1: switch to emscripten_get_now() (millisecond double, monotonic, matches
    // requestAnimationFrame's clock) once building under emcc — std::chrono is the
    // toolchain-agnostic placeholder so this compiles today on a plain host too.
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

@implementation NSDate

+ (NSDate *)date {
    NSDate *d = [[NSDate alloc] init];
    d->_secondsSinceEpoch = eden_now_seconds();
    return [d autorelease];
}

+ (NSDate *)dateWithTimeIntervalSinceNow:(NSTimeInterval)secs {
    NSDate *d = [[NSDate alloc] init];
    d->_secondsSinceEpoch = eden_now_seconds() + secs;
    return [d autorelease];
}

- (NSTimeInterval)timeIntervalSinceNow {
    return _secondsSinceEpoch - eden_now_seconds();
}

- (NSTimeInterval)timeIntervalSince1970 {
    return _secondsSinceEpoch;
}

- (NSTimeInterval)timeIntervalSinceDate:(NSDate *)other {
    return _secondsSinceEpoch - (other ? other->_secondsSinceEpoch : 0);
}

@end

@implementation NSDateFormatter

- (void)setDateFormat:(NSString *)fmt { (void)fmt; } // TODO P4
- (NSString *)dateFormat { return [NSString stringWithUTF8String:""]; } // TODO P4
- (NSString *)stringFromDate:(NSDate *)date {
    (void)date;
    return [NSString stringWithUTF8String:"1970-01-01"]; // TODO P4: real strftime-style format
}
- (NSDate *)dateFromString:(NSString *)str { (void)str; return [NSDate date]; } // TODO P4

@end
