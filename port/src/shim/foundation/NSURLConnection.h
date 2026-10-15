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
// NSURLConnection.h — D3a shim, **P6 territory, header-only this pass**. Per
// foundation-usage.md: every real call site (NSURL/NSURLRequest/NSURLConnection) lives in
// FileDownload.mm/FileUpload.mm/SharedList.mm/ShareUtil.mm/ShareMenu.mm/Alert.mm — all
// already seam-excluded (not compiled this pass, see CMakeLists.txt). This header exists only
// so that IF a future non-seam file transitively references one of these types, it still
// parses. No implementation; every method is `// TODO P6`. When Stage P6 lands, this whole
// cluster should probably be replaced outright by `fetch`/XMLHttpRequest calls at the seam
// (web-port-plan.md Stage P6), not by fleshing out these Foundation shims — keeping them
// stub-only avoids wasted effort here.
#ifndef EDEN_SHIM_NSURLCONNECTION_H
#define EDEN_SHIM_NSURLCONNECTION_H

#import "NSObject.h"

@class NSString;
@class NSData;

@interface NSURL : NSObject
+ (NSURL *)URLWithString:(NSString *)str;
- (id)initWithString:(NSString *)str;
@end

@interface NSURLRequest : NSObject
+ (NSURLRequest *)requestWithURL:(NSURL *)url;
- (id)initWithURL:(NSURL *)url;
@end

@interface NSMutableURLRequest : NSURLRequest
- (void)setHTTPMethod:(NSString *)method;   // TODO P6
- (void)setHTTPBody:(NSData *)body;         // TODO P6
@end

@interface NSURLResponse : NSObject
@end

@protocol NSURLConnectionDelegate <NSObject>
@optional
- (void)connection:(id)connection didReceiveResponse:(NSURLResponse *)response;
- (void)connection:(id)connection didReceiveData:(NSData *)data;
- (void)connectionDidFinishLoading:(id)connection;
- (void)connection:(id)connection didFailWithError:(id)error;
@end

@interface NSURLConnection : NSObject
+ (NSURLConnection *)connectionWithRequest:(NSURLRequest *)request delegate:(id)delegate; // TODO P6
- (id)initWithRequest:(NSURLRequest *)request delegate:(id)delegate; // TODO P6
@end

// NSOutputStream — declared here rather than in its own header because its only appearance in
// this tree is Classes/FileDownload.h's `NSOutputStream *fileStream;` ivar, the download path's
// incremental writer. FileDownload.mm itself is seam-excluded (P6), but its HEADER is still
// parsed by files that are compiled, so the type has to exist for them.
//
// TODO P6: on web the download path is `fetch` + a Response body stream written into OPFS, so
// this class is likely to disappear rather than be implemented — see web-port-plan.md Stage P6.
@interface NSOutputStream : NSObject
+ (NSOutputStream *)outputStreamToFileAtPath:(NSString *)path append:(BOOL)shouldAppend; // TODO P6
+ (NSOutputStream *)outputStreamToMemory;                                               // TODO P6
- (void)open;                                                                           // TODO P6
- (void)close;                                                                          // TODO P6
- (NSInteger)write:(const unsigned char *)buffer maxLength:(NSUInteger)length;           // TODO P6
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
