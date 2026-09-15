#import "NSData.h"
#import "NSString.h"
#include <cstring>
#include <cstdlib>
#include <fstream>

@implementation NSData

+ (NSData *)data { return [[[NSData alloc] init] autorelease]; }

+ (NSData *)dataWithBytes:(const void *)bytes length:(NSUInteger)len {
    return [[[NSData alloc] initWithBytes:bytes length:len] autorelease];
}

+ (NSData *)dataWithBytesNoCopy:(void *)bytes length:(NSUInteger)len freeWhenDone:(BOOL)freeWhenDone {
    NSData *d = [[[NSData alloc] initWithBytes:bytes length:len] autorelease];  // copies
    if (freeWhenDone && bytes) free(bytes);  // honor the ownership transfer the caller intends
    return d;
}

+ (NSData *)dataWithContentsOfFile:(NSString *)path {
    return [[[NSData alloc] initWithContentsOfFile:path] autorelease];
}

- (id)initWithBytes:(const void *)bytes length:(NSUInteger)len {
    self = [super init];
    _bytes.resize(len);
    if (bytes && len) memcpy(_bytes.data(), bytes, len);
    return self;
}

- (id)initWithBytesNoCopy:(void *)bytes length:(NSUInteger)len {
    return [self initWithBytes:bytes length:len];
}

- (id)initWithContentsOfFile:(NSString *)path {
    self = [super init];
    // TODO P4: OPFS-backed read (see FileManager seam). P1-adequate direct read for headless
    // link/smoke-test.
    std::ifstream f(path ? [path UTF8String] : "", std::ios::binary | std::ios::ate);
    if (!f) { return self; }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    _bytes.resize((size_t)size);
    if (size > 0) f.read((char *)_bytes.data(), size);
    return self;
}

- (id)initWithData:(NSData *)other {
    self = [super init];
    _bytes = other ? other->_bytes : eden_byte_vector();
    return self;
}

- (const void *)bytes { return _bytes.data(); }
- (NSUInteger)length { return (NSUInteger)_bytes.size(); }

- (void)getBytes:(void *)buffer length:(NSUInteger)len {
    memcpy(buffer, _bytes.data(), len < _bytes.size() ? len : _bytes.size());
}

- (void)getBytes:(void *)buffer range:(NSRange)range {
    memcpy(buffer, _bytes.data() + range.location, range.length);
}

- (NSData *)subdataWithRange:(NSRange)range {
    return [NSData dataWithBytes:_bytes.data() + range.location length:range.length];
}

- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)atomically {
    (void)atomically; // TODO P4: real atomic temp-file+rename (N3 audit finding) — this
                       // shim's job is the class, not the save-path policy; FileManager owns
                       // atomicity.
    std::ofstream f(path ? [path UTF8String] : "", std::ios::binary | std::ios::trunc);
    if (!f) return NO;
    if (!_bytes.empty()) f.write((const char *)_bytes.data(), (std::streamsize)_bytes.size());
    return f.good() ? YES : NO;
}


// ROADMAP Phase M / M6 -- THE PER-WORLD-LOAD LEAK. `_bytes` is a std::vector ivar, and this port's
// hand-written ObjC runtime emits no `.cxx_destruct`: object_dispose() is a bare free(), so
// without this method every NSData that dies takes its heap buffer with it and never gives it
// back. It is the same hazard NSUserDefaults.mm and NSAutoreleasePool.mm already document, and it
// was the dominant term in the ~22 MB every world load leaked -- the texture loader reads ~25 PNGs
// through +dataWithContentsOfFile: and the column streamer reads every bundled-map column through
// NSFileHandle's -readDataOfLength: (~48 KB of NSMutableData each). Measured with
// tools/headless-alloc-leak-probe.js.
//
// Explicit destructor call, then [super dealloc] -- the ivar must die BEFORE the storage it lives
// in is freed. Safe on an instance that was calloc'd and never constructed (an all-zero vector is
// a valid empty one, and ~vector on it deallocates nothing), which is exactly what
// class_createInstance hands back. NSMutableData inherits this.
//
// *** THAT "ALL-ZERO IS VALID" ARGUMENT IS PER-CONTAINER AND PER-STANDARD-LIBRARY. *** Phase N
// Stage 3.2 found the limit of it the hard way. For std::vector it holds on both libc++ and
// libstdc++ — the layout is three null pointers, which is exactly what an empty vector is — so
// this class and NSArray/NSSet are correct as written. For **std::string it is FALSE on
// libstdc++**, whose string keeps a pointer that normally aims at its own local buffer; all-zero
// leaves that pointer NULL and the first assignment segfaults inside `_M_replace`. That is a real
// crash the Linux leg hit while loading a world, and EdenConcreteString now placement-news its
// ivar in every initializer (NSString.mm) rather than relying on the zeros.
//
// The durable rule, if a fifth class ever gains a C++ ivar: CONSTRUCT IT, do not reason about
// whether its zero state happens to be valid on the standard libraries you have tried. The
// vectors here are left as they are only because changing them would touch four classes to fix
// nothing that is currently broken.
- (void)dealloc {
    _bytes.~eden_byte_vector();
    [super dealloc];
}

@end

@implementation NSMutableData

+ (NSMutableData *)data { return [[[NSMutableData alloc] init] autorelease]; }
+ (NSMutableData *)dataWithCapacity:(NSUInteger)capacity {
    NSMutableData *d = [[NSMutableData alloc] init];
    d->_bytes.reserve(capacity);
    return [d autorelease];
}

- (void *)mutableBytes { return _bytes.data(); }

- (void)appendBytes:(const void *)bytes length:(NSUInteger)len {
    size_t old = _bytes.size();
    _bytes.resize(old + len);
    if (bytes && len) memcpy(_bytes.data() + old, bytes, len);
}

- (void)appendData:(NSData *)other {
    if (other) [self appendBytes:other->_bytes.data() length:(NSUInteger)other->_bytes.size()];
}

// Real Foundation zeroes the bytes -setLength: grows into, and the backing vector no longer does
// that for us (see eden_default_init_allocator in NSData.h), so do it here explicitly. Shrinking
// has nothing to initialise. Callers that are about to overwrite everything anyway should use
// -setLengthUninitialized: instead.
- (void)setLength:(NSUInteger)len {
    size_t old = _bytes.size();
    _bytes.resize(len);
    if (len > old) memset(_bytes.data() + old, 0, len - old);
}

- (void)setLengthUninitialized:(NSUInteger)len { _bytes.resize(len); }

@end
