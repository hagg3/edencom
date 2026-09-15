// NSOperation.mm — see NSOperation.h for who uses this and why it exists at all.
#import "NSOperation.h"

@implementation NSOperation {
  BOOL _finished;
  BOOL _cancelled;
}

// Subclasses override this; the base does nothing, as on Apple's.
- (void)main {}

- (void)start {
  if (_cancelled) return;
  [self main];
  _finished = YES;
}

- (BOOL)isFinished { return _finished; }
- (BOOL)isCancelled { return _cancelled; }
- (void)cancel { _cancelled = YES; }

@end

@implementation NSOperationQueue

// SYNCHRONOUS on purpose. The alternative stubs are both worse:
//   * a no-op would make -loadBuffersAsynchronously: silently never load anything, which looks
//     like a missing-asset bug rather than an unimplemented queue;
//   * spawning a real thread would put audio decoding on a second thread, and CLAUDE.md
//     convention #4 is explicit that this engine has exactly one background thread (the world
//     load) and that nothing should be added to it.
// Running inline preserves the observable outcome (the buffers ARE loaded, in order, before the
// caller continues) and only loses the asynchrony — which on web belongs to Stage P5's Web Audio
// decodeAudioData, an async API of its own, rather than to an NSOperationQueue emulation.
- (void)addOperation:(NSOperation *)op {
  [op start];
}

- (void)setMaxConcurrentOperationCount:(NSInteger)count { (void)count; }
- (void)cancelAllOperations {}

@end
