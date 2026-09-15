// NSString.mm — class-cluster implementation (see the layout note at the top of NSString.h).
//
// NSString itself is abstract and holds no storage. EVERY method here is written against the
// two primitives -UTF8String and -length, so it behaves identically whether the receiver (or
// any NSString* argument) is a heap-allocated EdenConcreteString or one of the 743 statically
// emitted `@"..."` NSConstantString literals in the engine. This is why no method may ever go
// back to touching a `_std` ivar through an `NSString *` — that pointer might be a literal,
// which has no such field. Use EdenStd(x) instead.
//
// Formatting (stringWithFormat: et al.) handles %@ (recursively calling -UTF8String on the
// NSString/NSObject arg) by pre-scanning the format string and splitting into a real printf
// call per-%@-boundary; every other specifier is forwarded to vsnprintf verbatim, so
// %d/%f/%s/%lld/%x/%c/%u all behave exactly like C printf (which is how the engine's mixed-
// format call sites — a few genuinely pass bare C specifiers to Foundation's formatter, iOS
// tolerates this — are already written).
#import "NSString.h"
#import "NSData.h"
#import "NSArray.h"
#include <cstdio>
#include <cstdarg>
#include <new>       // placement new for the _std ivar — see EdenConcreteString's -init
#include <cstring>
#include <fstream>
#include <sstream>

// Read any NSString-cluster object's bytes, polymorphically and nil-safely. Uses -length (not
// strlen) so embedded NULs survive. Returns by value: these are all cold paths (filenames, log
// and format strings, share URLs) — no hot loop in the engine formats strings, so the copy is
// not worth optimizing away. TODO (only if a profile ever says so): add a -edenStdRef fast
// path for EdenConcreteString receivers.
static inline std::string EdenStd(NSString *s) {
    if (!s) return std::string();
    const char *p = [s UTF8String];
    if (!p) return std::string();
    return std::string(p, (size_t)[s length]);
}

// Every NSString factory funnels through here, so the concrete class is named in exactly one
// place if it ever changes.
static inline NSString *EdenMakeString(const std::string &s) {
    return [EdenConcreteString stringWithStd:s];
}

// Shared %@-aware formatter, used by stringWithFormat:/initWithFormat:/stringByAppendingFormat:/
// appendFormat:. NOT re-entrant-safe across threads (fine — all Foundation use is main-thread
// per CLAUDE.md convention #4).
std::string eden_format_nsstring(const char *fmt, va_list args) {
    std::string out;
    const char *p = fmt;
    while (*p) {
        if (p[0] == '%' && p[1] == '@') {
            id obj = va_arg(args, id);
            if (obj) {
                NSString *desc = nullptr;
                if ([obj isKindOfClass:[NSString class]]) {
                    desc = (NSString *)obj;
                } else {
                    desc = [obj description];
                }
                out += desc ? EdenStd(desc) : std::string("(null description)");
            } else {
                out += "(null)";
            }
            p += 2;
            continue;
        }
        if (p[0] == '%' && p[1] == '%') {
            out += '%';
            p += 2;
            continue;
        }
        if (p[0] == '%') {
            // Copy one C-style conversion (%[-+ 0#]*[width][.prec][length]conv) and format it
            // with vsnprintf on a per-specifier basis so va_arg consumption stays correct
            // regardless of type width.
            const char *start = p;
            p++; // skip '%'
            while (*p && strchr("-+ 0#", *p)) p++;
            while (*p && isdigit((unsigned char)*p)) p++;
            if (*p == '.') { p++; while (*p && isdigit((unsigned char)*p)) p++; }
            while (*p && strchr("hlLqjzt", *p)) p++;
            if (!*p) { out.append(start, p - start); break; }
            char conv = *p++;
            std::string spec(start, p - start);
            char buf[256];
            switch (conv) {
                case 'd': case 'i': case 'c': {
                    int v = va_arg(args, int);
                    snprintf(buf, sizeof(buf), spec.c_str(), v);
                    out += buf;
                    break;
                }
                case 'u': case 'x': case 'X': case 'o': {
                    unsigned v = va_arg(args, unsigned);
                    snprintf(buf, sizeof(buf), spec.c_str(), v);
                    out += buf;
                    break;
                }
                case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
                    double v = va_arg(args, double);
                    snprintf(buf, sizeof(buf), spec.c_str(), v);
                    out += buf;
                    break;
                }
                case 's': {
                    const char *v = va_arg(args, const char *);
                    snprintf(buf, sizeof(buf), spec.c_str(), v ? v : "(null)");
                    out += buf;
                    break;
                }
                case 'p': {
                    void *v = va_arg(args, void *);
                    snprintf(buf, sizeof(buf), spec.c_str(), v);
                    out += buf;
                    break;
                }
                default:
                    out += spec; // unknown conversion — emit literally rather than crash
                    break;
            }
            continue;
        }
        out += *p++;
    }
    return out;
}

@implementation NSString

// --- Factories: all return the concrete cluster member ---------------------------------

+ (NSString *)string {
    return EdenMakeString(std::string());
}

+ (NSString *)stringWithFormat:(NSString *)fmt, ... {
    va_list args;
    va_start(args, fmt);
    std::string s = eden_format_nsstring(EdenStd(fmt).c_str(), args);
    va_end(args);
    return EdenMakeString(s);
}

+ (NSString *)stringWithUTF8String:(const char *)utf8 {
    return EdenMakeString(utf8 ? std::string(utf8) : std::string());
}

+ (NSString *)stringWithCString:(const char *)cstr encoding:(NSStringEncoding)enc {
    (void)enc; // TODO P1: only UTF8/ASCII observed at call sites; treat all as raw bytes.
    return [NSString stringWithUTF8String:cstr];
}

// --- init family: class-cluster swap ----------------------------------------------------
// `[[NSString alloc] initWith...]` allocates the ABSTRACT class, which has nowhere to put the
// bytes. Real Foundation solves this the same way: the init returns a different, concrete
// object and releases the placeholder. The `object_getClass(self) == [NSString class]` guard
// is what stops this recursing — EdenConcreteString overrides these same selectors, so the
// re-dispatch below lands on its implementations, not back here.

- (id)init {
    if (object_getClass(self) == [NSString class]) {
        [self release];
        return [[EdenConcreteString alloc] init];
    }
    return [super init];
}

- (id)initWithFormat:(NSString *)fmt, ... {
    va_list args;
    va_start(args, fmt);
    std::string s = eden_format_nsstring(EdenStd(fmt).c_str(), args);
    va_end(args);
    [self release];
    return [[EdenConcreteString alloc] initWithUTF8String:s.c_str()];
}

- (id)initWithUTF8String:(const char *)utf8 {
    [self release];
    return [[EdenConcreteString alloc] initWithUTF8String:utf8];
}

- (id)initWithCString:(const char *)cstr encoding:(NSStringEncoding)enc {
    (void)enc;
    return [self initWithUTF8String:cstr];
}

- (id)initWithString:(NSString *)other {
    std::string s = EdenStd(other);
    [self release];
    return [[EdenConcreteString alloc] initWithUTF8String:s.c_str()];
}

- (id)initWithBytes:(const void *)bytes length:(NSUInteger)len encoding:(NSStringEncoding)enc {
    (void)enc;
    [self release];
    EdenConcreteString *r = [[EdenConcreteString alloc] init];
    [r stdString].assign((const char *)bytes, len);
    return r;
}

// --- Primitives: abstract, overridden by every concrete member of the cluster ------------

- (const char *)UTF8String {
    return ""; // abstract; EdenConcreteString/NSConstantString/NSMutableString override.
}

- (NSUInteger)length {
    return 0; // abstract; see -UTF8String.
}

// --- Everything below is derived, and works on ANY cluster member -----------------------

- (unichar)characterAtIndex:(NSUInteger)index {
    std::string s = EdenStd(self);
    return (unichar)(unsigned char)s.at(index); // TODO P1: ASCII-only; no UTF-16 code units.
}

- (NSString *)substringFromIndex:(NSUInteger)index {
    std::string s = EdenStd(self);
    return EdenMakeString(index <= s.size() ? s.substr(index) : std::string());
}

- (NSString *)substringToIndex:(NSUInteger)index {
    return EdenMakeString(EdenStd(self).substr(0, index));
}

- (NSString *)substringWithRange:(NSRange)range {
    return EdenMakeString(EdenStd(self).substr(range.location, range.length));
}

- (NSArray *)componentsSeparatedByString:(NSString *)sep {
    NSMutableArray *arr = [NSMutableArray array];
    std::string s = EdenStd(self), d = EdenStd(sep);
    if (d.empty()) { [arr addObject:self]; return arr; }
    size_t start = 0, pos;
    while ((pos = s.find(d, start)) != std::string::npos) {
        [arr addObject:EdenMakeString(s.substr(start, pos - start))];
        start = pos + d.size();
    }
    [arr addObject:EdenMakeString(s.substr(start))];
    return arr;
}

- (NSString *)stringByAppendingString:(NSString *)other {
    return EdenMakeString(EdenStd(self) + EdenStd(other));
}

- (NSString *)stringByAppendingFormat:(NSString *)fmt, ... {
    va_list args;
    va_start(args, fmt);
    std::string s = eden_format_nsstring(EdenStd(fmt).c_str(), args);
    va_end(args);
    return EdenMakeString(EdenStd(self) + s);
}

- (NSString *)stringByAppendingPathComponent:(NSString *)component {
    std::string s = EdenStd(self);
    if (!s.empty() && s[s.size() - 1] != '/') s += '/';
    s += EdenStd(component);
    return EdenMakeString(s);
}

- (NSString *)stringByDeletingPathExtension {
    std::string s = EdenStd(self);
    size_t slash = s.find_last_of('/');
    size_t dot = s.find_last_of('.');
    return EdenMakeString((dot != std::string::npos && (slash == std::string::npos || dot > slash))
                              ? s.substr(0, dot) : s);
}

- (NSString *)stringByDeletingLastPathComponent {
    std::string s = EdenStd(self);
    size_t slash = s.find_last_of('/');
    return EdenMakeString(slash == std::string::npos ? std::string() : s.substr(0, slash));
}

- (NSString *)pathExtension {
    std::string s = EdenStd(self);
    size_t slash = s.find_last_of('/');
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return EdenMakeString(s.substr(dot + 1));
    return EdenMakeString(std::string());
}

- (NSString *)lastPathComponent {
    std::string s = EdenStd(self);
    size_t slash = s.find_last_of('/');
    return EdenMakeString(slash == std::string::npos ? s : s.substr(slash + 1));
}

- (NSString *)stringByReplacingOccurrencesOfString:(NSString *)target withString:(NSString *)repl {
    std::string s = EdenStd(self), t = EdenStd(target), r = EdenStd(repl);
    if (!t.empty()) {
        size_t pos = 0;
        while ((pos = s.find(t, pos)) != std::string::npos) {
            s.replace(pos, t.size(), r);
            pos += r.size();
        }
    }
    return EdenMakeString(s);
}

- (BOOL)isEqualToString:(NSString *)other { return other && EdenStd(self) == EdenStd(other); }
- (NSComparisonResult)compare:(NSString *)other { return [self compare:other options:0]; }
- (NSComparisonResult)compare:(NSString *)other options:(NSUInteger)opts {
    (void)opts; // TODO P1: NSNumericSearch (used once, seam-side iOS-version compare) not
                // implemented as numeric — falls back to lexicographic, harmless since that
                // call site lives in the replaced EdenViewController.mm anyway.
    int c = EdenStd(self).compare(EdenStd(other));
    return c < 0 ? NSOrderedAscending : (c > 0 ? NSOrderedDescending : NSOrderedSame);
}
- (BOOL)isAbsolutePath {
    std::string s = EdenStd(self);
    return !s.empty() && s[0] == '/';
}
- (BOOL)hasPrefix:(NSString *)prefix {
    if (!prefix) return NO;
    std::string s = EdenStd(self), p = EdenStd(prefix);
    return p.size() <= s.size() && s.compare(0, p.size(), p) == 0;
}
- (BOOL)hasSuffix:(NSString *)suffix {
    if (!suffix) return NO;
    std::string s = EdenStd(self), x = EdenStd(suffix);
    if (x.size() > s.size()) return NO;
    return s.compare(s.size() - x.size(), x.size(), x) == 0;
}
- (NSRange)rangeOfString:(NSString *)needle {
    if (!needle) return NSMakeRange(NSNotFound, 0);
    std::string s = EdenStd(self), n = EdenStd(needle);
    size_t pos = s.find(n);
    if (pos == std::string::npos) return NSMakeRange(NSNotFound, 0);
    return NSMakeRange((NSUInteger)pos, (NSUInteger)n.size());
}

- (double)doubleValue { return atof([self UTF8String]); }
- (int)intValue { return atoi([self UTF8String]); }
- (float)floatValue { return (float)atof([self UTF8String]); }
- (BOOL)boolValue {
    // Foundation semantics: leading whitespace, optional sign, then 'Y'/'y'/'T'/'t'/nonzero digit.
    std::string s = EdenStd(self);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (isspace((unsigned char)c)) continue;
        return (c == 'Y' || c == 'y' || c == 'T' || c == 't' || (c >= '1' && c <= '9')) ? YES : NO;
    }
    return NO;
}
- (NSInteger)integerValue { return (NSInteger)atol([self UTF8String]); }

- (NSString *)uppercaseString {
    std::string s = EdenStd(self);
    for (size_t i = 0; i < s.size(); i++) s[i] = toupper((unsigned char)s[i]);
    return EdenMakeString(s);
}
- (NSString *)lowercaseString {
    std::string s = EdenStd(self);
    for (size_t i = 0; i < s.size(); i++) s[i] = tolower((unsigned char)s[i]);
    return EdenMakeString(s);
}

- (const char *)cStringUsingEncoding:(NSStringEncoding)enc { (void)enc; return [self UTF8String]; }
- (const char *)cString { return [self UTF8String]; }

// Copy the string's bytes into caller-provided storage, NUL-terminated. Load-bearing: Util.mm's
// cpstring() (the std::string bridge used all over the engine, e.g. Menu's worldExists() checks on
// the load/share path) calls exactly [str getCString:buf maxLength:1000 encoding:NSUTF8StringEncoding].
// Its absence made cpstring() send an unresolved selector, which the GNU ObjC runtime turns into an
// abort() — killing the whole main loop the instant a world was clicked to load (the "clicking a
// world freezes the game" bug). Foundation semantics: returns NO (and copies only what fits, still
// NUL-terminated) if the string + terminator don't fit in maxLength; YES otherwise.
- (BOOL)getCString:(char *)buffer maxLength:(NSUInteger)maxLength encoding:(NSStringEncoding)enc {
    (void)enc;
    if (!buffer || maxLength == 0) return NO;
    const char *s = [self UTF8String];
    NSUInteger len = (NSUInteger)[self length];   // UTF8 byte count in this shim (== _std.size())
    if (!s) { buffer[0] = '\0'; return YES; }
    if (len + 1 > maxLength) {                     // doesn't fit including the NUL
        NSUInteger n = maxLength - 1;
        for (NSUInteger i = 0; i < n; i++) buffer[i] = s[i];
        buffer[n] = '\0';
        return NO;
    }
    for (NSUInteger i = 0; i < len; i++) buffer[i] = s[i];
    buffer[len] = '\0';
    return YES;
}

- (NSData *)dataUsingEncoding:(NSStringEncoding)enc {
    (void)enc;
    return [NSData dataWithBytes:[self UTF8String] length:(NSUInteger)[self length]];
}

- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)atomically encoding:(NSStringEncoding)enc
               error:(id *)error {
    (void)atomically; (void)enc; (void)error;
    // TODO P4: route through the OPFS-backed file layer once it exists; P1-adequate direct
    // write (works under MEMFS) so callers don't need special-casing before then.
    std::ofstream f(EdenStd(path).c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return NO;
    std::string s = EdenStd(self);
    f.write(s.data(), (std::streamsize)s.size());
    return f.good() ? YES : NO;
}

- (void)drawAtPoint:(CGPoint)point withFont:(UIFont *)font {
    (void)point; (void)font; // TODO P2: Texture2D/font raster (owned by Stage P2, not this shim)
}
- (void)drawInRect:(CGRect)rect withFont:(UIFont *)font {
    (void)rect; (void)font; // TODO P2
}
- (CGSize)sizeWithFont:(UIFont *)font {
    (void)font; // TODO P2
    return (CGSize){0, 0};
}

@end

// ----------------------------------------------------------------------------------------

@implementation EdenConcreteString

+ (EdenConcreteString *)stringWithStd:(const std::string &)s {
    EdenConcreteString *r = [[EdenConcreteString alloc] init];
    r->_std = s;
    return [r autorelease];
}

// *** THE `_std` IVAR MUST BE PLACEMENT-NEW'd BEFORE ANYTHING TOUCHES IT. ***
// Phase N Stage 3.2, and it is the mirror image of the explicit destructor call in -dealloc
// below. This port's hand-written runtime emits no `.cxx_construct` and allocates with calloc, so
// a C++ ivar arrives ZEROED AND UNCONSTRUCTED. -dealloc has always compensated on the destruction
// side; nothing compensated on the construction side, and until now nothing had to:
//
//   * libc++'s std::string is all-zero-valid — zeros are a short string of length 0, whose
//     character data lives inline. Assigning to it works. That is what web and iOS have.
//   * libstdc++'s is NOT. Its layout is `{ pointer _M_p; size_type _M_len; union {...} }` and
//     `_M_p` normally points at the object's own local buffer, so an all-zero string has a NULL
//     data pointer. The first assignment calls `_M_replace`, which writes through it.
//
// Measured, not deduced: the Linux leg died with SIGSEGV in
// `std::string::_M_replace` <- `-[EdenConcreteString initWithUTF8String:]` <-
// `FileManager::loadWorld` <- `loadWorldThread`. Loading a world was the first thing this port
// ever did that built a string on a non-libc++ standard library.
//
// One placement-new per initializer rather than funnelling through -init, because NSMutableString
// subclasses this and `[self init]` would dispatch to an override; constructing exactly once, in
// the method that owns the object's storage, has no such dependency.
- (id)init {
    self = [super init];
    if (self) new (&_std) eden_std_string();
    return self;
}

- (id)initWithUTF8String:(const char *)utf8 {
    self = [super init];
    if (self) {
        new (&_std) eden_std_string();
        _std = utf8 ? utf8 : "";
    }
    return self;
}

- (id)initWithFormat:(NSString *)fmt, ... {
    self = [super init];
    if (self) {
        new (&_std) eden_std_string();
        va_list args;
        va_start(args, fmt);
        _std = eden_format_nsstring(EdenStd(fmt).c_str(), args);
        va_end(args);
    }
    return self;
}

- (std::string &)stdString { return _std; }

// The two primitives every inherited NSString method is built on.
- (const char *)UTF8String { return _std.c_str(); }
- (NSUInteger)length { return (NSUInteger)_std.size(); }

// ROADMAP Phase M / M6. `_std` is a std::string ivar and this port's hand-written ObjC runtime
// emits no `.cxx_destruct` — object_dispose() is a bare free() — so without this, every string
// longer than libc++'s 22-byte short-string buffer leaked its heap buffer when it died. Same
// hazard NSData.mm, NSUserDefaults.mm and NSAutoreleasePool.mm document; NSData.mm's -dealloc has
// the full note. Its claim that "an all-zero std::string reads as a valid empty SHORT string" is
// true of libc++ and FALSE of libstdc++ — see the placement-new block above — so the object this
// destructor runs on is now genuinely constructed rather than merely zeroed.
// NSMutableString inherits this; NSConstantString does not (it is immortal and overrides -dealloc
// to do nothing, one class up the hierarchy from here).
- (void)dealloc {
    _std.~eden_std_string();
    [super dealloc];
}

@end

// ----------------------------------------------------------------------------------------

@implementation NSConstantString

// Instances of this class are NEVER created at runtime — clang emits them as static data for
// each `@"..."` in the engine. So: no init, no alloc, and the memory-management methods must
// be no-ops, because the storage was never malloc'd and freeing it would be a wild free.
// (NSObject's side-table retain count would also report a harmless constant 1 for these, but
// overriding here makes the immortality explicit and skips the hash lookup entirely.)
- (id)retain { return self; }
- (oneway void)release { /* immortal — static storage, never freed */ }
- (id)autorelease { return self; }
- (NSUInteger)retainCount { return (NSUInteger)-1; } // Foundation's convention for immortal objects
- (void)dealloc { /* never runs; deliberately does NOT call [super dealloc] */ }

// The two primitives, read straight out of the layout clang emitted.
- (const char *)UTF8String { return _cString; }
- (NSUInteger)length { return (NSUInteger)_length; }

@end

// ----------------------------------------------------------------------------------------

@implementation NSMutableString

// Inherits EdenConcreteString's _std storage and primitives; only the mutators live here.

+ (NSMutableString *)string { return [[[NSMutableString alloc] init] autorelease]; }
+ (NSMutableString *)stringWithCapacity:(NSUInteger)capacity {
    NSMutableString *r = [[[NSMutableString alloc] init] autorelease];
    r->_std.reserve((size_t)capacity);
    return r;
}

- (void)appendString:(NSString *)other { _std += EdenStd(other); }

- (void)appendFormat:(NSString *)fmt, ... {
    va_list args;
    va_start(args, fmt);
    _std += eden_format_nsstring(EdenStd(fmt).c_str(), args);
    va_end(args);
}

- (void)replaceOccurrencesOfString:(NSString *)target withString:(NSString *)repl
                            options:(NSUInteger)opts range:(NSRange)range {
    (void)opts; (void)range; // TODO P1: range/options not honored — engine call sites observed
                              // always pass the whole-string range; revisit if that changes.
    std::string t = EdenStd(target), r = EdenStd(repl);
    if (t.empty()) return;
    size_t pos = 0;
    while ((pos = _std.find(t, pos)) != std::string::npos) {
        _std.replace(pos, t.size(), r);
        pos += r.size();
    }
}

- (void)setString:(NSString *)other { _std = EdenStd(other); }

@end
