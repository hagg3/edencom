#import "NSArray.h"
#include <cstdarg>
#include <cstddef>   // size_t — see NSObject.mm; do not rely on <algorithm> for it
#include <algorithm>
#include <utility>

@implementation NSArray

+ (NSArray *)array { return [[[NSArray alloc] init] autorelease]; }

+ (NSArray *)arrayWithObjects:(id)first, ... {
    NSArray *a = [[NSArray alloc] init];
    if (first) {
        a->_items.push_back(first);
        va_list args;
        va_start(args, first);
        id obj;
        while ((obj = va_arg(args, id)) != nil) a->_items.push_back(obj);
        va_end(args);
    }
    return [a autorelease];
}

- (NSUInteger)count { return (NSUInteger)_items.size(); }
- (id)objectAtIndex:(NSUInteger)index { return _items.at(index); }
- (NSUInteger)indexOfObject:(id)obj {
    for (size_t i = 0; i < _items.size(); ++i) if ([_items[i] isEqual:obj]) return (NSUInteger)i;
    return NSNotFound;
}
- (BOOL)containsObject:(id)obj { return [self indexOfObject:obj] != NSNotFound; }


// ROADMAP Phase M / M6. `_items` is a std::vector ivar and this port's hand-written ObjC runtime
// emits no `.cxx_destruct` — object_dispose() is a bare free() — so without this the vector's heap
// buffer is leaked every time an array dies. Same hazard NSData.mm, NSUserDefaults.mm and
// NSAutoreleasePool.mm document; see NSData.mm's -dealloc for the full note and why an explicit
// destructor call on a calloc'd-never-constructed vector is safe.
//
// It destroys the VECTOR, not its contents: this shim's arrays and sets do not retain what is put
// in them (+arrayWithObjects:/-addObject: just push_back), so releasing here would over-release.
// NSMutableArray inherits this.
- (void)dealloc {
    _items.~eden_id_vector();
    [super dealloc];
}

@end

@implementation NSMutableArray

+ (NSMutableArray *)array { return [[[NSMutableArray alloc] init] autorelease]; }
+ (NSMutableArray *)arrayWithCapacity:(NSUInteger)capacity {
    NSMutableArray *a = [[NSMutableArray alloc] init];
    a->_items.reserve(capacity);
    return [a autorelease];
}

- (void)addObject:(id)obj { _items.push_back(obj); }
- (void)removeObjectAtIndex:(NSUInteger)index {
    if (index < _items.size()) _items.erase(_items.begin() + index);
}
- (void)removeObject:(id)obj {
    _items.erase(std::remove_if(_items.begin(), _items.end(),
                                 [obj](id o) { return [o isEqual:obj]; }),
                 _items.end());
}
- (void)removeAllObjects { _items.clear(); }

@end

@implementation NSSet

+ (NSSet *)setWithObject:(id)obj {
    NSSet *s = [[NSSet alloc] init];
    if (obj) s->_items.push_back(obj);
    return [s autorelease];
}
+ (NSSet *)setWithArray:(NSArray *)arr {
    NSSet *s = [[NSSet alloc] init];
    if (arr) s->_items = arr->_items;
    return [s autorelease];
}
- (NSUInteger)count { return (NSUInteger)_items.size(); }
- (id)anyObject { return _items.empty() ? nil : _items.front(); }
- (BOOL)containsObject:(id)obj {
    for (id o : _items) if ([o isEqual:obj]) return YES;
    return NO;
}
- (NSArray *)allObjects {
    NSArray *a = [[NSArray alloc] init];
    a->_items = _items;
    return [a autorelease];
}
// NSFastEnumeration — backs `for (UITouch *t in touches)` in Input.mm.
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                   objects:(id __unsafe_unretained [])stackbuf
                                     count:(NSUInteger)len {
    if (state->state == 0) {
        state->state = 1;
        state->mutationsPtr = (unsigned long *)&state->extra[0]; // no real mutation tracking
        state->itemsPtr = _items.empty() ? stackbuf : (id __unsafe_unretained *)_items.data();
        return (NSUInteger)_items.size();
    }
    return 0; // single-pass: whole set handed back in the first call
}

// Same as NSArray's — see its -dealloc. NSMutableSet inherits this.
- (void)dealloc {
    _items.~eden_id_vector();
    [super dealloc];
}

@end

@implementation NSMutableSet

+ (NSMutableSet *)set { return [[[NSMutableSet alloc] init] autorelease]; }
- (void)addObject:(id)obj {
    if (![self containsObject:obj]) _items.push_back(obj);
}
- (void)removeObject:(id)obj {
    _items.erase(std::remove_if(_items.begin(), _items.end(),
                                 [obj](id o) { return [o isEqual:obj]; }),
                 _items.end());
}

@end

@implementation NSDictionary
+ (NSDictionary *)dictionary { return [[[NSDictionary alloc] init] autorelease]; }
- (id)objectForKey:(id)key { (void)key; return nil; } // TODO P1-if-blocking, see header
- (NSUInteger)count { return 0; } // TODO P1-if-blocking
@end

@implementation NSMutableDictionary
+ (NSMutableDictionary *)dictionary { return [[[NSMutableDictionary alloc] init] autorelease]; }
- (void)setObject:(id)obj forKey:(id)key { (void)obj; (void)key; } // TODO P1-if-blocking
- (void)removeObjectForKey:(id)key { (void)key; } // TODO P1-if-blocking
@end
