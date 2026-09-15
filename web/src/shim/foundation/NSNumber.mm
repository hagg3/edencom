#import "NSNumber.h"

@implementation NSNumber

+ (NSNumber *)numberWithInt:(int)v {
    NSNumber *n = [[NSNumber alloc] init]; n->_kind = kInt; n->_v.i = v; return [n autorelease];
}
+ (NSNumber *)numberWithFloat:(float)v {
    NSNumber *n = [[NSNumber alloc] init]; n->_kind = kFloat; n->_v.f = v; return [n autorelease];
}
+ (NSNumber *)numberWithDouble:(double)v {
    NSNumber *n = [[NSNumber alloc] init]; n->_kind = kDouble; n->_v.d = v; return [n autorelease];
}
+ (NSNumber *)numberWithBool:(BOOL)v {
    NSNumber *n = [[NSNumber alloc] init]; n->_kind = kBool; n->_v.b = v; return [n autorelease];
}
+ (NSNumber *)numberWithUnsignedInt:(unsigned int)v {
    NSNumber *n = [[NSNumber alloc] init]; n->_kind = kUInt; n->_v.u = v; return [n autorelease];
}

- (int)intValue {
    switch (_kind) {
        case kInt: return _v.i; case kFloat: return (int)_v.f; case kDouble: return (int)_v.d;
        case kBool: return _v.b ? 1 : 0; case kUInt: return (int)_v.u;
    }
    return 0;
}
- (float)floatValue {
    switch (_kind) {
        case kInt: return (float)_v.i; case kFloat: return _v.f; case kDouble: return (float)_v.d;
        case kBool: return _v.b ? 1.f : 0.f; case kUInt: return (float)_v.u;
    }
    return 0;
}
- (double)doubleValue {
    switch (_kind) {
        case kInt: return _v.i; case kFloat: return _v.f; case kDouble: return _v.d;
        case kBool: return _v.b ? 1.0 : 0.0; case kUInt: return _v.u;
    }
    return 0;
}
- (BOOL)boolValue {
    switch (_kind) {
        case kInt: return _v.i != 0; case kFloat: return _v.f != 0; case kDouble: return _v.d != 0;
        case kBool: return _v.b; case kUInt: return _v.u != 0;
    }
    return NO;
}
- (unsigned int)unsignedIntValue {
    switch (_kind) {
        case kInt: return (unsigned int)_v.i; case kFloat: return (unsigned int)_v.f;
        case kDouble: return (unsigned int)_v.d; case kBool: return _v.b ? 1 : 0; case kUInt: return _v.u;
    }
    return 0;
}

@end
