// SPDX-License-Identifier: LGPL-3.0-or-later
#include "updater_adapter.h"

#include "config.h"
#include "core/github_release.h"
#include "core/version.h"

#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Small responses (release metadata, SHA256SUMS) buffered to a string.
bool httpGetToString(const std::string& url, std::string& out, std::string& err) {
    @autoreleasepool {
        NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nsurl) {
            err = "Invalid URL";
            return false;
        }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl];
        req.timeoutInterval = 15.0;
        NSString* ua =
            [NSString stringWithFormat:@"Satellite/%s (+updater)", SATELLITE_VERSION_STRING];
        [req setValue:ua forHTTPHeaderField:@"User-Agent"];
        [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        [req setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];

        // Synchronous semaphore wait is safe: already on a worker thread, so the UI doesn't block.
        __block NSData* body = nil;
        __block NSURLResponse* resp = nil;
        __block NSError* nserr = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:req
              completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
                body = d;
                resp = r;
                nserr = e;
                dispatch_semaphore_signal(sem);
              }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (nserr) {
            err = std::string("Network error: ") + nserr.localizedDescription.UTF8String;
            return false;
        }
        if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
            NSInteger status = ((NSHTTPURLResponse*)resp).statusCode;
            if (status < 200 || status >= 300) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "HTTP %ld", (long)status);
                err = buf;
                return false;
            }
        }
        out.assign((const char*)body.bytes, body.length);
        return true;
    }
}

class ProgressDelegate {
  public:
    ProgressDelegate(const std::function<void(uint64_t, uint64_t)>& cb,
                     const std::atomic<bool>* cancel)
        : cb_(cb), cancel_(cancel) {}
    void onProgress(uint64_t soFar, uint64_t total) const {
        if (cb_) cb_(soFar, total);
    }
    bool isCancelled() const { return cancel_ && cancel_->load(); }

  private:
    std::function<void(uint64_t, uint64_t)> cb_;
    const std::atomic<bool>* cancel_;
};

} // namespace

// impl is a raw pointer because ARC can't manage C++ callable types.
@interface SatelliteDownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@property(assign) ProgressDelegate* impl;
@property(strong) NSURL* destination;
@property(assign) BOOL ok;
@property(strong) NSString* errorString;
@property(strong) dispatch_semaphore_t doneSem;
@end

@implementation SatelliteDownloadDelegate

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)task
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    (void)session;
    (void)bytesWritten;
    if (self.impl) {
        self.impl->onProgress(
            (uint64_t)totalBytesWritten,
            (uint64_t)(totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : 0));
        if (self.impl->isCancelled()) { [task cancel]; }
    }
}

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)task
    didFinishDownloadingToURL:(NSURL*)location {
    (void)session;
    NSError* err = nil;
    [[NSFileManager defaultManager] removeItemAtURL:self.destination error:nil];
    if (![[NSFileManager defaultManager] moveItemAtURL:location
                                                 toURL:self.destination
                                                 error:&err]) {
        self.ok = NO;
        self.errorString = err.localizedDescription ?: @"move failed";
    } else {
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)task.response;
        if (http.statusCode < 200 || http.statusCode >= 300) {
            self.ok = NO;
            self.errorString = [NSString stringWithFormat:@"HTTP %ld", (long)http.statusCode];
        } else {
            self.ok = YES;
        }
    }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    (void)session;
    (void)task;
    if (error) {
        if (error.code == NSURLErrorCancelled) {
            self.ok = NO;
            self.errorString = @"cancelled";
        } else if (!self.errorString) {
            self.ok = NO;
            self.errorString = error.localizedDescription;
        }
    }
    if (self.doneSem) dispatch_semaphore_signal(self.doneSem);
}
@end

namespace {

bool httpGetToFile(const std::string& url, const std::string& dstPath,
                   const std::function<void(uint64_t, uint64_t)>& onProgress,
                   const std::atomic<bool>* cancel, std::string& err) {
    @autoreleasepool {
        NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nsurl) {
            err = "Invalid URL";
            return false;
        }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl];
        req.timeoutInterval = 60.0;
        NSString* ua =
            [NSString stringWithFormat:@"Satellite/%s (+updater)", SATELLITE_VERSION_STRING];
        [req setValue:ua forHTTPHeaderField:@"User-Agent"];

        ProgressDelegate impl(onProgress, cancel);
        SatelliteDownloadDelegate* delegate = [[SatelliteDownloadDelegate alloc] init];
        delegate.impl = &impl;
        delegate.destination =
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:dstPath.c_str()]];
        delegate.doneSem = dispatch_semaphore_create(0);
        delegate.ok = NO;

        NSURLSession* session = [NSURLSession
            sessionWithConfiguration:[NSURLSessionConfiguration ephemeralSessionConfiguration]
                            delegate:delegate
                       delegateQueue:nil];
        NSURLSessionDownloadTask* task = [session downloadTaskWithRequest:req];
        [task resume];
        dispatch_semaphore_wait(delegate.doneSem, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (!delegate.ok) {
            err = delegate.errorString ? delegate.errorString.UTF8String : "download failed";
            return false;
        }
        return true;
    }
}

// Matches satellite-macos-*.zip.
bool pickMacAsset(const GitHubRelease& rel, GitHubAsset& out) {
    for (const auto& a : rel.assets) {
        if (a.name.find("satellite-macos") == std::string::npos) continue;
        if (a.name.size() < 4 || a.name.compare(a.name.size() - 4, 4, ".zip") != 0) continue;
        out = a;
        return true;
    }
    return false;
}

std::string fetchAssetDigest(const GitHubRelease& rel, const std::string& assetName) {
    for (const auto& a : rel.assets) {
        if (a.name == "SHA256SUMS") {
            std::string body, e;
            if (!httpGetToString(a.browserUrl, body, e)) return "";
            return lookupSha256(body, assetName);
        }
    }
    return "";
}

bool sha256OfFile(const std::string& path, std::string& hexOut, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "Cannot open downloaded file for hashing";
        return false;
    }
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    }
    std::fclose(f);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    static const char* kHex = "0123456789abcdef";
    hexOut.clear();
    hexOut.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        hexOut += kHex[digest[i] >> 4];
        hexOut += kHex[digest[i] & 0xF];
    }
    return true;
}

// Empty if not running inside an .app (development build), which disables
// in-place self-update.
std::string currentAppBundle() {
    @autoreleasepool {
        NSBundle* b = [NSBundle mainBundle];
        if (!b) return "";
        NSString* p = b.bundlePath;
        if (!p || ![p hasSuffix:@".app"]) return "";
        return p.UTF8String;
    }
}

} // namespace

MacOSUpdaterAdapter::MacOSUpdaterAdapter(std::string owner, std::string repo)
    : owner_(std::move(owner)), repo_(std::move(repo)) {}

bool MacOSUpdaterAdapter::fetchLatestRelease(const std::string& channel,
                                             const std::string& currentVersion, UpdateInfo& out,
                                             std::string& outError) {
    out = {};
    const bool wantPrerelease = (channel == "prerelease");
    std::string url = "https://api.github.com/repos/" + owner_ + "/" + repo_;
    if (wantPrerelease) {
        url += "/releases?per_page=30";
    } else {
        url += "/releases/latest";
    }
    std::string body;
    if (!httpGetToString(url, body, outError)) return false;

    GitHubRelease pick;
    if (wantPrerelease) {
        std::vector<GitHubRelease> list;
        if (!parseGitHubReleaseList(body, list) || list.empty()) {
            outError = "Failed to parse releases list";
            return false;
        }
        bool found = false;
        for (const auto& r : list) {
            if (r.draft) continue;
            pick = r;
            found = true;
            break;
        }
        if (!found) {
            outError = "No suitable release found";
            return false;
        }
    } else {
        if (!parseGitHubRelease(body, pick)) {
            outError = "Failed to parse release JSON";
            return false;
        }
    }

    if (pick.tagName.empty()) {
        outError = "Release missing tag_name";
        return false;
    }

    GitHubAsset asset;
    if (!pickMacAsset(pick, asset)) {
        outError = "No macOS .zip asset in release " + pick.tagName;
        return false;
    }

    out.version = stripTagPrefix(pick.tagName);
    out.channel = pick.prerelease ? "prerelease" : "stable";
    out.assetName = asset.name;
    out.assetUrl = asset.browserUrl;
    out.assetSize = asset.size;
    out.assetSha256 = fetchAssetDigest(pick, asset.name);
    out.releaseNotes = pick.body.size() > 8192 ? pick.body.substr(0, 8192) + "..." : pick.body;
    out.htmlUrl = pick.htmlUrl;
    out.publishedAtEpoch = isoToEpoch(pick.publishedAt);
    out.installMethod =
        currentAppBundle().empty() ? InstallMethod::Manual : InstallMethod::SelfInstall;
    if (out.installMethod == InstallMethod::Manual) {
        out.manualInstruction = "Download " + asset.name + " from " + out.htmlUrl +
                                " and replace your existing satellite.app.";
    }
    out.available = (out.version != currentVersion);
    return true;
}

bool MacOSUpdaterAdapter::downloadArtifact(
    const UpdateInfo& info, const std::function<void(uint64_t, uint64_t)>& onProgress,
    const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) {
    @autoreleasepool {
        NSString* dir = [NSString stringWithFormat:@"%@/satellite/updates", NSTemporaryDirectory()];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        std::string dst = std::string(dir.UTF8String) + "/" + info.assetName;
        if (!httpGetToFile(info.assetUrl, dst, onProgress, cancel, outError)) return false;
        outLocalPath = dst;
        return true;
    }
}

bool MacOSUpdaterAdapter::verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                         std::string& outError) {
    if (info.assetSha256.empty()) return true;
    std::string actual;
    if (!sha256OfFile(localPath, actual, outError)) return false;
    std::string expected = info.assetSha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected) {
        outError = "SHA-256 mismatch: expected " + expected + ", got " + actual;
        return false;
    }
    return true;
}

bool MacOSUpdaterAdapter::applyUpdate(const std::string& localPath, const UpdateInfo& info,
                                      std::string& outError) {
    (void)info;
    @autoreleasepool {
        std::string bundle = currentAppBundle();
        if (bundle.empty()) {
            outError = "Running outside an .app bundle; in-place update not supported";
            return false;
        }

        NSString* zipPath = [NSString stringWithUTF8String:localPath.c_str()];
        NSString* stagingDir = [NSString
            stringWithFormat:@"%@/satellite/staging-%d", NSTemporaryDirectory(), (int)getpid()];
        [[NSFileManager defaultManager] removeItemAtPath:stagingDir error:nil];
        [[NSFileManager defaultManager] createDirectoryAtPath:stagingDir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        // ditto preserves extended attributes and resource forks, required to
        // keep codesigned bundles valid.
        NSTask* task = [[NSTask alloc] init];
        task.launchPath = @"/usr/bin/ditto";
        task.arguments = @[ @"-xk", zipPath, stagingDir ];
        [task launch];
        [task waitUntilExit];
        if (task.terminationStatus != 0) {
            outError = "ditto -xk failed";
            return false;
        }

        NSArray<NSString*>* entries =
            [[NSFileManager defaultManager] contentsOfDirectoryAtPath:stagingDir error:nil];
        NSString* newApp = nil;
        for (NSString* e in entries) {
            if ([e hasSuffix:@".app"]) {
                newApp = [stagingDir stringByAppendingPathComponent:e];
                break;
            }
        }
        if (!newApp) {
            outError = "Unpacked bundle did not contain *.app";
            return false;
        }

        // The swap must outlive this process, so it runs in a detached helper
        // that waits for our PID to exit before swapping and relaunching. Paths
        // come from a controlled NSTemporaryDirectory tree and are quoted below.
        NSString* helper = [NSString
            stringWithFormat:@"%@/satellite/swap-%d.sh", NSTemporaryDirectory(), (int)getpid()];
        NSString* script = [NSString
            stringWithFormat:@"#!/bin/bash\n"
                              "set -e\n"
                              "PID=%d\n"
                              "SRC=\"%@\"\n"
                              "DST=\"%s\"\n"
                              "# Wait up to 30s for the current process to release the bundle.\n"
                              "for i in $(seq 1 60); do\n"
                              "  if ! kill -0 \"$PID\" 2>/dev/null; then break; fi\n"
                              "  sleep 0.5\n"
                              "done\n"
                              "# Move-aside-then-move-in keeps the swap atomic per directory.\n"
                              "rm -rf \"$DST.old\" || true\n"
                              "if [ -d \"$DST\" ]; then mv \"$DST\" \"$DST.old\"; fi\n"
                              "mv \"$SRC\" \"$DST\"\n"
                              "rm -rf \"$DST.old\" || true\n"
                              "open \"$DST\"\n",
                             (int)getpid(), newApp, bundle.c_str()];
        [script writeToFile:helper atomically:YES encoding:NSUTF8StringEncoding error:nil];
        chmod(helper.UTF8String, 0755);

        NSTask* run = [[NSTask alloc] init];
        run.launchPath = @"/bin/bash";
        run.arguments = @[ helper ];
        [run launch];

        // Terminate so the detached helper can swap the bundle we still hold open.
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        return true;
    }
}
