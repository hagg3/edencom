// ---------------------------------------------------------------------------------------------
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): on a target with a real
// Foundation, this shim class does not exist — the system one does, and defining ours alongside
// libobjc's/Foundation's would be a duplicate-class collision with undefined resolution. Files
// that #import this header by relative path (the seam files do, and cannot be redirected with
// -I because quoted includes always resolve against the including file's own directory first)
// therefore get the real Foundation here instead.
//
// EDEN_USE_SYSTEM_FOUNDATION is defined by native/CMakeLists.txt only; read that file's header
// for why the native macOS target cannot use this port's hand-written ObjC runtime (MEASURED:
// clang cannot lower ANY GNU ObjC ABI to Mach-O — it emits selector-name globals in a COMDAT,
// which the MachO backend does not support. gnustep-1.9, gnustep-1.8, gcc and objfw all fail
// identically).
// ---------------------------------------------------------------------------------------------
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#import <Foundation/Foundation.h>
#else
// NSData.h — D3a shim, backed by std::vector<uint8_t>. See foundation-usage.md "NSData /
// NSMutableData". writeToFile:/initWithContentsOfFile: are P1-adequate (fopen-backed, works
// under MEMFS); Stage P4 swaps the *callers* (FileManager) to OPFS, not this class.
#ifndef EDEN_SHIM_NSDATA_H
#define EDEN_SHIM_NSDATA_H

#import "NSObject.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <utility>

// B6 (ROADMAP Phase B): std::vector<uint8_t>::resize() VALUE-initialises what it grows into, i.e.
// it zero-fills. Every one of this shim's grow sites (initWithBytes:, initWithContentsOfFile:,
// appendBytes:, and NSFileHandle's -readDataOfLength:) overwrites 100% of those bytes on the very
// next statement, so the fill is pure waste -- and on the chunk-streaming burst path it is ~48 KB
// of it per bundled-map column, thousands of columns per teleport. This allocator is the standard
// escape hatch: identical to std::allocator except that a value-less construct() does a DEFAULT
// initialisation (a no-op for uint8_t) instead of a value initialisation, which makes resize()
// leave new bytes untouched. It changes NOTHING an NSData caller can observe, because -setLength:
// below still explicitly zeroes what it grows into, exactly like real Foundation does.
template <class T>
struct eden_default_init_allocator : std::allocator<T> {
    using std::allocator<T>::allocator;
    template <class U> struct rebind { typedef eden_default_init_allocator<U> other; };
    template <class U, class... Args> void construct(U *p, Args &&...args) {
        ::new ((void *)p) U(std::forward<Args>(args)...);
    }
    template <class U> void construct(U *p) { ::new ((void *)p) U; }  // default-init: no zero-fill
};

typedef std::vector<uint8_t, eden_default_init_allocator<uint8_t> > eden_byte_vector;

@class NSString;
typedef struct _NSRange NSRange;

@interface NSData : NSObject {
@public
    eden_byte_vector _bytes;
}

+ (NSData *)data;
+ (NSData *)dataWithBytes:(const void *)bytes length:(NSUInteger)len;
// P4: FileManager wraps transient stack/heap buffers in NSData for -writeData: (freeWhenDone:FALSE)
// and a couple of malloc'd headers (freeWhenDone:TRUE). This shim COPIES the bytes (like
// -initWithBytesNoCopy:), so the "no copy" is nominal; it honors freeWhenDone by free()ing the
// caller's buffer after the copy when TRUE, matching the ownership contract the engine relies on.
+ (NSData *)dataWithBytesNoCopy:(void *)bytes length:(NSUInteger)len freeWhenDone:(BOOL)freeWhenDone;
+ (NSData *)dataWithContentsOfFile:(NSString *)path;

- (id)initWithBytes:(const void *)bytes length:(NSUInteger)len;
- (id)initWithBytesNoCopy:(void *)bytes length:(NSUInteger)len; // copies anyway (P1: simplest
                                                                  // correct behavior; no engine
                                                                  // call site relies on the
                                                                  // no-copy optimization).
- (id)initWithContentsOfFile:(NSString *)path;
- (id)initWithData:(NSData *)other;

- (const void *)bytes;
- (NSUInteger)length;
- (void)getBytes:(void *)buffer length:(NSUInteger)len;
- (void)getBytes:(void *)buffer range:(NSRange)range;
- (NSData *)subdataWithRange:(NSRange)range;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)atomically;

@end

@interface NSMutableData : NSData
+ (NSMutableData *)data;
+ (NSMutableData *)dataWithCapacity:(NSUInteger)capacity;
- (void *)mutableBytes;
- (void)appendBytes:(const void *)bytes length:(NSUInteger)len;
- (void)appendData:(NSData *)other;
- (void)setLength:(NSUInteger)len;
// Shim-only (no real-Foundation counterpart). Same as -setLength: but skips zeroing the bytes it
// grows into -- for the "grow, then immediately overwrite every byte" shape, which is what the
// NSFileHandle read path does. Growing with this and then NOT filling leaks whatever the allocator
// handed back, so only use it immediately before a full overwrite.
- (void)setLengthUninitialized:(NSUInteger)len;
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
