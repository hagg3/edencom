# Foundation/UIKit usage inventory (D3a)

Method-level inventory of every Foundation/UIKit touchpoint the engine actually makes,
gathered by grepping `Classes/*.mm`, `Lighting.mm`, `main.m` on 2026-07-19 — not guessed.
This is what `src/shim/foundation/*.h/.mm` needs to satisfy for Stage P1 (headless link) and
beyond. Priority tags: **P1** = needed just to link/construct `World` headless; **P2** =
needed once GL/rendering comes up (mostly Texture2D/Resources' UIKit surface, itself owned by
the P2 raster rewrite, not this shim); **P4/P5/P6** = the stage that gives the real
implementation (persistence/audio/network); everything else is low-traffic and stubbed with a
`TODO` marker returning a safe default.

Grep commands used (repeatable):
```
grep -ohE '\b(stringWithFormat|...)\b' Classes/*.mm Lighting.mm main.m | sort | uniq -c | sort -rn
```
(one pass per class; see git history of this file / the CMake-adjacent commit for exact
command lines if they need re-running after engine changes).

## Class-by-class

### NSObject (root) — P1
`alloc`(74), `init`(37), `retain`(58+93 across classes), `release`, `autorelease`,
`description`, `copy`, `dealloc`. Backing: intrusive refcount on a shim `NSObject` base;
real `@interface`/`@implementation` under the GNU objc runtime (D3a), not a fake C++ mimic —
the engine uses genuine bracket-message-send syntax throughout, so this only works if a real
ObjC frontend/runtime is present (see archive/PORT-STATUS-2026-08-13.md "P0.1 risk").

### NSAutoreleasePool — P1
`alloc`/`init`/`release`/`drain` (9 total mentions). One is used per-frame in
`EdenViewController::drawFrame` (`Classes/EdenViewController.mm:198` — seam, replaced) and at
least one wraps `main()`. Engine (non-seam) files that construct their own pools: grep for
`NSAutoreleasePool` in `Classes/*.mm` minus the seam list to re-verify per session — worth
attention because C3/H6 audit findings (`WORKING/audit-report.md`) are literally about pool
timing (per `CLAUDE.md`/plan D3).

### NSString — P1, heaviest user (232 raw mentions)
| Method | Count | Notes |
|---|---:|---|
| `release` | 93 | shared count across all classes (Foundation-wide `release`) |
| `length` | 84 | shared with NSData/NSArray |
| `stringWithFormat:` | 80 | printf-style — needs real `%@`/`%d`/`%s`/`%f`/`%llu` formatting |
| `retain` | 58 | shared count |
| `isEqualToString:` | 30 | |
| `cStringUsingEncoding:` | 30 | |
| `drawInRect:withFont:...` | 22 | **P2** — Texture2D/statusbar raster, not this shim's job |
| `initWithString:` | 11 | |
| `drawAtPoint:withFont:` | 11 | **P2** — raster |
| `stringWithUTF8String:` | 10 | |
| `UTF8String` | 6 | |
| `dataUsingEncoding:` | 5 | |
| `copy` | 5 | |
| `autorelease` | 5 | |
| `stringWithCString:encoding:` | 3 | |
| `stringByDeletingPathExtension` | 3 | |
| `hasSuffix:` | 3 | |
| `description` | 3 | |
| `cString` | 3 | |
| `writeToFile:atomically:...` | 2 | **P4** territory but trivial to implement now (std::ofstream) |
| `substringToIndex:` | 2 | |
| `stringByReplacingOccurrencesOfString:withString:` | 2 | (NSMutableString variant too) |
| `sizeWithFont:` | 2 | **P2** — raster |
| `intValue` | 2 | |
| `hasPrefix:` | 2 | |
| `compare:options:` | 2 | used for iOS version string compare in seam (EdenViewController, replaced) |
| `characterAtIndex:` | 2 | |
| `boolValue` | 2 | |
| `uppercaseString` | 1 | |
| `pathExtension` | 1 | |
| `initWithCString:encoding:` | 1 | |
| `componentsSeparatedByString:` | 1 | |

Implemented (P1, trivial over `std::string`): alloc/init family, stringWithFormat:,
stringWithUTF8String:, stringWithCString:encoding:, UTF8String, cStringUsingEncoding:,
cString, length, characterAtIndex:, substringFromIndex:/toIndex:/withRange:,
componentsSeparatedByString:, stringByAppendingString:/Format:/PathComponent:,
stringByDeletingPathExtension/LastPathComponent, pathExtension, lastPathComponent,
isEqualToString:, compare:, hasPrefix:/hasSuffix:, doubleValue/intValue/floatValue/
boolValue/integerValue, rangeOfString:, uppercaseString/lowercaseString,
stringByReplacingOccurrencesOfString:withString: (+ NSMutableString's in-place
replaceOccurrencesOfString:), description, dataUsingEncoding:, writeToFile:atomically:.
Stubbed **P2** (raster, TODO): drawAtPoint:withFont:, drawInRect:withFont:...,
sizeWithFont:.

### NSData / NSMutableData — P1/P4 (54+4 mentions)
`dataWithBytes:length:`, `initWithBytes:length:`, `initWithBytesNoCopy:length:`,
`initWithContentsOfFile:`, `dataWithContentsOfFile:`, `initWithData:`, `appendBytes:length:`,
`appendData:`, `bytes`, `mutableBytes`, `length`, `writeToFile:atomically:`,
`getBytes:length:`/`getBytes:range:`, `subdataWithRange:`. Backing: a `std::vector<uint8_t>`
with a **default-init allocator** (`eden_default_init_allocator`, NSData.h) — `resize()` therefore
does NOT zero what it grows into, because every grow site here overwrites those bytes on the next
statement (B6, 2026-08-28). `-setLength:` re-adds the zero-fill explicitly so it still matches real
Foundation; the shim-only `-setLengthUninitialized:` is the escape hatch for "grow, then fill
completely", which is what the file-read path does.
`-dealloc` destroys `_bytes` explicitly — mandatory for any class here with a C++ ivar, since the
runtime emits no `.cxx_destruct` (see `web/docs/objc-runtime.md`; missing it here was the bulk of
the ~22 MB-per-world-load leak fixed by Phase M / M6, 2026-09-02).
All implemented (trivial). `initWithContentsOfFile:`/`writeToFile:` are the **P4** seam
(FileManager's actual save I/O) — implemented here over plain `fopen`/`FILE*` for P1 headless
correctness; P4 swaps the *seam* (FileManager.mm's callers), not this class, to OPFS.

### NSNumber — P1 (17 mentions)
`numberWithInt:`(4), `numberWithBool:`(1), plus declared-but-ungrepped-with-args
`numberWithFloat:`/`numberWithDouble:`/`numberWithUnsignedInt:` (present in the 17 total,
low-frequency). Backing: a tagged union. Implemented.

### NSArray / NSMutableArray — P1 (4+2 mentions, but 102 `count` calls shared with
NSDictionary/NSSet/NSData/NSString `length`-adjacent uses — re-verify with a class-scoped
grep before trusting the 102 figure)
`arrayWithObjects:`, `addObject:`, `objectAtIndex:`, `removeObjectAtIndex:`, `count`,
`indexOfObject:`, `removeAllObjects`, `array`. Backing: `std::vector<id>`. Implemented.

### NSDictionary — low priority (2 mentions). Header declared, methods stubbed
`// TODO P1-if-blocking` (objectForKey:/setObject:forKey: only, backed by
`std::map<id,id>` using `-hash`/`-isEqual:` — NOT implemented, `NSUserDefaults`'s own storage
does NOT go through this class, see below).

### NSSet — P1 (8 mentions), **load-bearing for input, not optional**
`Input.h`'s public API (`Classes/Input.h`, ENGINE file, never modified per the port plan) is
`void touchesBegan(NSSet* touches, UIEvent* event)` etc. — four call sites, all real
touch-set iteration (`count`, `anyObject`, fast enumeration `for (UITouch* t in touches)`).
This is NOT optional/stubbable if Stage P3 (input) is to work; implemented now (over
`std::vector<id>`, order-independent semantics preserved) even though P1 itself doesn't need
it functionally (only to link).

### NSDate / NSDateFormatter — P1 (15+2 mentions)
`date`, `dateWithTimeIntervalSinceNow:`, `timeIntervalSinceNow`, `timeIntervalSince1970`,
`timeIntervalSinceDate:`. Backing: `double` seconds since epoch via
`emscripten_get_now()`/`std::chrono` (per plan: "CFAbsoluteTime -> emscripten_get_now").
`NSDateFormatter`: `dateFormat`/`setDateFormat:`/`stringFromDate:`/`dateFromString:` — only 2
raw mentions, used for a save/creature-file timestamp per `docs/save-load.md`; stubbed
**TODO P4** (needs real strftime-style formatting, not worth guessing the exact format string
without re-reading the two call sites when P4 starts).

### NSFileHandle — P1 (to link) / **P4** (real behavior). 17+19+19+19+11+6+5+1+1+1 mentions
`fileHandleForReadingAtPath:`, `fileHandleForWritingAtPath:`, `fileHandleForUpdatingAtPath:`,
`seekToFileOffset:`, `seekToEndOfFile`, `readDataOfLength:`, `readDataToEndOfFile`,
`writeData:`, `closeFile`, `offsetInFile`. This is THE append-only `.eden` format I/O
(`docs/eden-file-format.md`, `docs/save-load.md`) — `WORKING/archive/PORT-STATUS-2026-08-13.md`/plan D1 calls out
OPFS `FileSystemSyncAccessHandle` as the eventual backing (synchronous seek/read/write,
matching this API almost 1:1). **This pass**: header + a P1-only backing over plain
`fopen`/`fseek`/`fread`/`fwrite` (works under Emscripten's default MEMFS, enough to link and
smoke-test in-memory), clearly marked `// TODO P4: swap to OPFS FileSystemSyncAccessHandle,
see WORKING/web-port-plan.md Stage P4` at every method.

**Read-path performance (B6, 2026-08-28).** `-readDataOfLength:` is the world file's read
primitive and it is on the chunk-streaming burst path, so its per-call overhead matters. Two
things were removed: the `-setLength:` zero-fill (see NSData above), and the unconditional
per-`fread` `emscripten_get_now()` pair that fed B1's I/O split — that timing is now behind
`eden_debug_set_io_timing(1)` and OFF by default, so shipped builds pay one branch instead of two
wasm→JS crossings per read (`tools/headless-mesh-burst-probe.js --io-timing` turns it back on).
What is deliberately **not** here is a read-ahead buffer: a 16 KB one was implemented, measured,
and thrown away — it made the column read ~60% *slower*, because under this port the file is the
lazy `Eden.eden` node serving reads out of 32 KB blocks, so over-reading costs a real copy. The
fix that worked was at the caller (`fmh_readColumnRawFromDefault` asks for the whole ~1.2 KB
record once instead of eight times), which left nothing for a buffer here to earn. Measure with
`tools/headless-column-read-bench.js` before adding one back.

### NSFileManager — P1 (to link) / **P4**. 10 mentions
`defaultManager`, `fileExistsAtPath:`, `createFileAtPath:contents:attributes:`,
`removeItemAtPath:error:`, `copyItemAtPath:toPath:error:`, `moveItemAtPath:toPath:error:`,
`contentsOfDirectoryAtPath:error:`, `NSSearchPathForDirectoriesInDomains` (C function, not a
method). Backing: POSIX `stat`/`unlink`/`opendir` over MEMFS for P1; **TODO P4**: OPFS
directory API.

### NSThread — P1 (14 mentions, but see below — nearly moot)
`detachNewThreadSelector:toTarget:withObject:`, `isMainThread`, `sleepForTimeInterval:`.
**Important finding**: grep shows the *only real* (non-commented) call sites for
`detachNewThreadSelector:` are in `Classes/Appirater.mm` (×3) — a file this port **strips
entirely** (plan: "Strip: Appirater.mm (rating)"), never linked. `Classes/Terrain.mm`'s and
`Classes/World.mm`'s `detachNewThreadSelector:` call sites are **commented-out archaeology**
(`Classes/Terrain.mm:61-63`) — CLAUDE.md says keep, don't delete, but they don't compile.
`Classes/World.mm`'s real world-load thread uses **raw `pthread_create`**
(`Classes/World.mm:339-340`), not `NSThread` at all — this is the "world-load pthread"
CLAUDE.md convention #4 refers to, and it needs NO Foundation shim (pthreads work natively
under Emscripten's `-pthread`/`EDEN_THREADED`, see CMakeLists.txt). Net effect: `NSThread`'s
shim only needs to exist enough to *parse* (nothing meaningfully calls it once Appirater is
gone) — implemented as thin no-op-ish wrappers, not a priority.

### NSBundle — P1 (9 mentions)
`mainBundle`, `pathForResource:ofType:`, `bundlePath`, `resourcePath`. Backing: a fixed
virtual root (`"/bundle"`) that the build's asset-fetch step (Stage P4's lazy `Eden.eden`
fetch, `public/`) populates; for P1 headless this can return a fake absolute path string
without a real filesystem behind it (nothing reads through it before GL/assets matter).

### NSUserDefaults — **P7**-ish but easy now (9+7+6+6+4+4+2+2 mentions)
`standardUserDefaults`, `objectForKey:`/`setObject:forKey:`, `integerForKey:`/
`setInteger:forKey:`, `boolForKey:`/`setBool:forKey:`, `stringForKey:`, `synchronize`.
Backing: in-memory `std::map<std::string, ...>` now; **TODO P7**: persist via `localStorage`
(through a small JS `EM_ASM` bridge) or an OPFS-backed key file — low priority, not
load-bearing for P1-P4.

### NSURL / NSURLRequest / NSURLConnection — **P6**, heavily stubbed
`URLWithString:`, `initWithURL:`, `requestWithURL:`, `setHTTPMethod:`, `setHTTPBody:`,
`initWithRequest:delegate:`, `connectionDidFinishLoading:`, `connection:didReceiveData:`,
`connection:didReceiveResponse:`, `connection:didFailWithError:`. This whole cluster is
`FileDownload.mm`/`FileUpload.mm`/`SharedList.mm`/`ShareUtil.mm`/`ShareMenu.mm`/`Alert.mm` —
**already in the seam-exclusion list** (not compiled this pass at all), so this class only
needs to exist enough that *other* files parse if they happen to `#import` it transitively —
grep shows no non-seam engine file references `NSURL*` directly. Header skeleton only, all
methods `// TODO P6`, no implementation.

### NSError / NSException — low priority (4+4 mentions)
`errorWithDomain:code:userInfo:`, `localizedDescription`; `raise:format:`,
`exceptionWithName:reason:userInfo:`. Stubbed: NSException maps to `NSLog` + `abort()` (no
Objective-C `@try/@catch` unwinding support assumed — TODO verify no engine file actually
`@catches` one, or this stub is wrong); NSError is a passive data holder, trivial.

### NSTimer — seam-adjacent (3 mentions, real user is `EdenViewController.mm`, replaced)
`scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:`, `invalidate`. Only
needed if a non-seam engine file uses it — grep shows none; header exists for completeness,
`// TODO P7` if one shows up, otherwise unused by the ported (non-seam) tree.

### NSLog — P1, 233 mentions (the highest-traffic single symbol in the whole inventory)
Needs real `%@`/`%d`/`%f`/`%s`/`%lld`/`%llu`/`%x` handling since call sites freely mix
Foundation format specifiers with C ones (`Eden_Prefix.pch` also `#define`s it to route
through `printf`-family when unoptimized — see that file's already-present macro).
Implemented as a small varargs-forwarding shim that walks the format string and substitutes
`%@` arguments by calling `-description`/`UTF8String` before handing the rest to `vprintf`.

## UIKit-adjacent types (NOT in the original D3a "~8 classes" ask — added because the
engine's own ENGINE files, not just seam files, use them and cannot be modified)
Grepped across `Classes/*.mm` + `Lighting.mm`: `UIImage`(61), `CGRect`(50), `CGPoint`(28),
`UIFont`(17), `UIEvent`(8), `NSSet`(8, see above), `CGSize`(7), `UITouch`(3),
`UIAccelerometer`(3), `UIView`(2).
- **`CGPoint`/`CGRect`/`CGSize`** — plain structs, zero behavior, fully implemented
  (`uikit_stubs.h`).
- **`UITouch`/`UIEvent`** — needed for `Input.h`'s real signature
  (`touchesBegan(NSSet*, UIEvent*)` etc.) — implemented minimally: `UITouch` carries
  {location, phase, timestamp, an opaque identity pointer}; `UIEvent` is close to a marker
  type (grep shows the engine only ever passes it through, never reads fields off it) —
  see `uikit_stubs.h`.
- **`UIImage`/`UIFont`/`UIColor`/`UIView`/`UIAccelerometer`** — used by `Resources.mm`
  (declares `UIImage* storedSkins[5][2]` etc. — opaque pointers, no methods called on them in
  that file) and `statusbar.mm` (`[UIFont systemFontOfSize:]`, real raster). These are the
  **Texture2D/statusbar raster surface — Stage P2's problem, not this shim's**. Forward-declared
  as opaque classes here (enough to satisfy `Resources.h`'s field declarations and let P1 link
  without a real image decoder) with every method `// TODO P2`.
- `VKeyboard.mm` additionally calls `[G_EAGL_VIEW insertSubview:atIndex:]` — a real `UIView`
  method. Because of this one call, `VKeyboard.mm` was reclassified from "Aux UIKit: verify"
  to **seam-excluded** in this pass (see `WORKING/archive/PORT-STATUS-2026-08-13.md` — a native `<input>` overlay at
  the JS/HTML layer replaces it, matching the original's "real OS text field over the GL view"
  approach in spirit). `statusbar.mm` and `Gamepad.mm` were verified clean (no `UIView`
  subclassing/`addSubview` calls) and stay classified as ordinary engine files.

## What this pass implements vs. stubs (summary)
Implemented (real, STL-backed): NSObject, NSAutoreleasePool, NSString, NSData/NSMutableData,
NSNumber, NSArray/NSMutableArray, NSSet, NSDate, NSFileHandle (P1 fopen-backed),
NSFileManager (P1 POSIX-backed), NSBundle (fake root), NSLog.
Stubbed with `// TODO <phase>` markers: NSDateFormatter (P4), NSThread (near-moot, see above),
NSUserDefaults (in-memory now, P7 persistence), NSURL/NSURLRequest/NSURLConnection (P6, no
implementation), NSDictionary (unused enough to leave declared-only), NSError/NSException
(P1 trivial stub, not a priority), NSTimer (unused by non-seam engine files), UIImage/UIFont/
UIColor/UIView/UIAccelerometer (P2, opaque).
