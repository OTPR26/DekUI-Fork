// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>

#include "hud_download.h"
#include "../hud_setup.h"
#include <stdexcept>

@interface ReMaskedHudDownload : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) NSUInteger maximum;
@property(nonatomic, strong) NSMutableData* bytes;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic, strong) dispatch_semaphore_t completed;
@end

@implementation ReMaskedHudDownload
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))complete {
    NSInteger status = [(NSHTTPURLResponse*)response statusCode];
    complete(status == 200 && response.expectedContentLength <= static_cast<int64_t>(self.maximum)
        ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    if (data.length > self.maximum - self.bytes.length) {
        [task cancel];
        return;
    }
    [self.bytes appendData:data];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    self.failure = error;
    dispatch_semaphore_signal(self.completed);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))complete {
    complete(nil);
}
@end

namespace ReMaskedIOS {
namespace {
NSData* Download(NSString* url, NSUInteger maximum) {
    ReMaskedHudDownload* download = [ReMaskedHudDownload new];
    download.maximum = maximum;
    download.bytes = [NSMutableData data];
    download.completed = dispatch_semaphore_create(0);
    NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 15;
    config.timeoutIntervalForResource = 30;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:download delegateQueue:nil];
    [[session dataTaskWithURL:[NSURL URLWithString:url]] resume];
    const bool timedOut = dispatch_semaphore_wait(download.completed,
        dispatch_time(DISPATCH_TIME_NOW, 35 * NSEC_PER_SEC)) != 0;
    if (timedOut || download.failure || !download.bytes.length) {
        [session invalidateAndCancel];
        throw std::runtime_error("Could not download the Project Restoration HUD. Check your connection and retry.");
    }
    [session finishTasksAndInvalidate];
    return download.bytes;
}
}

void PrepareDownloadedHud(const std::filesystem::path& staging) {
    NSData* archive = Download(@"https://zora.re/dl/mm3d_big_screen_layout_779512b.7z", 128 * 1024);
    NSData* atlas = Download(@"https://zora.re/dl/tex1_512x256_E2CEBCB19DA2608F_13.png", 2 * 1024 * 1024);
    if (!remasked_prepare_hud(staging.c_str(), static_cast<const uint8_t*>(archive.bytes), archive.length,
                              static_cast<const uint8_t*>(atlas.bytes), atlas.length)) {
        throw std::runtime_error("The Project Restoration HUD could not be verified or rebuilt for this ROM.");
    }
}
}
