#import "NSTimer.h"

@implementation NSTimer

+ (NSTimer *)scheduledTimerWithTimeInterval:(double)interval target:(id)target
                                     selector:(SEL)sel userInfo:(id)info repeats:(BOOL)repeats {
    // TODO P7 (if ever needed off the seam): wire to a JS setInterval/setTimeout bridge. Not
    // implemented — see header, no known non-seam caller.
    (void)interval; (void)target; (void)sel; (void)info; (void)repeats;
    return [[[NSTimer alloc] init] autorelease];
}

- (void)invalidate {}

@end
