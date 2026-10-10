// AppleNet_native.mm — NetBackend_native.h on macOS and iOS, over NSURLSession (ROADMAP 5.9).
//
// Compiled as its own object library with the REAL SDK headers and ARC (native/CMakeLists.txt,
// next to eden_apple_audio, for the same reason: the main target's include path shadows parts of
// the SDK with this port's stubs). Exports only the plain-C wall in NetBackend_native.h.
//
// Why NSURLSession and not libcurl on the Mac too: iOS has no libcurl, and this way the code the
// iPad runs is the code every macOS selftest exercises.
//
// THE EDEN SERVERS ARE PLAIN HTTP (app/app2.edengame.net refuse TLS), which App Transport Security
// blocks by default in an .app. native/ios/Info.plist.in carries the exception for edengame.net
// only; the community archive (hagg3.github.io) is HTTPS and needs none. A bare macOS executable
// has no Info.plist and is not subject to ATS.
//
// Shape: a delegate-based data task on a private serial queue (so bytes stream to the sink as they
// arrive — a world can be hundreds of MB and must never sit in RAM), with the calling worker
// thread parked on a semaphore it wakes every 100 ms to check the cancel flag.
#import <Foundation/Foundation.h>

#include "NetBackend_native.h"

#include <cstdio>

@interface EdenNetTask : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, assign) const EdenNetCallbacks* cb;
@property (nonatomic, assign) int status;
@property (nonatomic, assign) BOOL writeFailed;
@property (nonatomic, assign) BOOL badStatus;
@property (nonatomic, strong) NSError* error;
@property (nonatomic, strong) dispatch_semaphore_t done;
@end

@implementation EdenNetTask
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session; (void)task;
    int code = 200;
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) code = (int)[(NSHTTPURLResponse*)response statusCode];
    self.status = code;
    if (code < 200 || code >= 300) {
        self.badStatus = YES;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    if (response.expectedContentLength >= 0 && self.cb->on_total)
        self.cb->on_total(self.cb->ctx, response.expectedContentLength);
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    (void)session;
    if (self.writeFailed) return;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        if (!self.cb->on_data(self.cb->ctx, bytes, range.length)) {
            self.writeFailed = YES;
            *stop = YES;
        }
    }];
    if (self.writeFailed) [task cancel];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session; (void)task;
    self.error = error;
    dispatch_semaphore_signal(self.done);
}
@end

extern "C" int eden_net_backend_available(void) { return 1; }

// GET when contentType is null; otherwise a POST whose body is the file at bodyPath (an upload task
// from a file: NSURLSession sets Content-Length from it and streams it, never chunked).
static int eden_net_apple_run(const char* url, const char* contentType, const char* bodyPath,
                              const EdenNetCallbacks* cb, int* httpStatus, char* err, int errcap) {
    @autoreleasepool {
        *httpStatus = 0;
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!u) { std::snprintf(err, errcap, "bad address"); return 0; }

        EdenNetTask* t = [[EdenNetTask alloc] init];
        t.cb = cb;
        t.done = dispatch_semaphore_create(0);

        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 30;         // seconds of silence, not the whole transfer
        cfg.timeoutIntervalForResource = 6 * 3600;  // a 1 GB world on a slow link is legitimate
        cfg.HTTPAdditionalHeaders = @{ @"User-Agent": @"Emod/2.1.1" };
        NSOperationQueue* q = [[NSOperationQueue alloc] init];
        q.maxConcurrentOperationCount = 1;          // the delegate is not re-entrant
        NSURLSession* s = [NSURLSession sessionWithConfiguration:cfg delegate:t delegateQueue:q];
        NSURLSessionTask* task;
        if (contentType) {
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
            req.HTTPMethod = @"POST";
            [req setValue:[NSString stringWithUTF8String:contentType] forHTTPHeaderField:@"Content-Type"];
            NSURL* body = [NSURL fileURLWithPath:[NSString stringWithUTF8String:bodyPath]];
            task = [s uploadTaskWithRequest:req fromFile:body];
        } else {
            task = [s dataTaskWithURL:u];
        }
        [task resume];

        bool cancelSent = false;
        while (dispatch_semaphore_wait(t.done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
            if (!cancelSent && cb->cancelled && cb->cancelled(cb->ctx)) { [task cancel]; cancelSent = true; }
        }
        // The session retains its delegate until it is invalidated; without this every fetch
        // would leak a session, a queue and the delegate.
        [s finishTasksAndInvalidate];

        *httpStatus = t.status;
        if (t.badStatus)   { std::snprintf(err, errcap, "HTTP %d", t.status); return 0; }
        if (t.writeFailed) { std::snprintf(err, errcap, "could not write the download"); return 0; }
        if (cancelSent)    { std::snprintf(err, errcap, "cancelled"); return 0; }
        if (t.error) {
            std::snprintf(err, errcap, "%s", [[t.error localizedDescription] UTF8String] ?: "network error");
            return 0;
        }
        return 1;
    }
}

extern "C" int eden_net_backend_fetch(const char* url, const EdenNetCallbacks* cb, int* httpStatus,
                                      char* err, int errcap) {
    return eden_net_apple_run(url, nullptr, nullptr, cb, httpStatus, err, errcap);
}

extern "C" int eden_net_backend_post_file(const char* url, const char* contentType, const char* bodyPath,
                                          const EdenNetCallbacks* cb, int* httpStatus, char* err, int errcap) {
    return eden_net_apple_run(url, contentType, bodyPath, cb, httpStatus, err, errcap);
}
