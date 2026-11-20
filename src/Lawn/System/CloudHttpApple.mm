/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// The HTTP client of cloud save on iOS: NSURLSession, which checks certificates against the system's trust store, so
// the app needs no libcurl and no CA bundle of its own. It does what the libcurl client in CloudSync.cpp does: no
// caches or cookies, no compression (an ETag must stay that of the stored file), redirects only when asked for, to
// https only and without the credentials once the host changes. Built with ARC.

#if defined(PVZP_CLOUD_SAVE) && defined(PVZP_CLOUD_HTTP_APPLE)

#import <Foundation/Foundation.h>

#include "CloudSync.h"
#include <string>
#include <utility>

#if !__has_feature(objc_arc)
#error CloudHttpApple.mm must be built with ARC (-fobjc-arc)
#endif

static const NSInteger MAX_REDIRECTS = 5;

@interface PvzpHttpDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation PvzpHttpDelegate

- (void)URLSession:(NSURLSession*)theSession
			  task:(NSURLSessionTask*)theTask
willPerformHTTPRedirection:(NSHTTPURLResponse*)theResponse
		newRequest:(NSURLRequest*)theRequest
 completionHandler:(void (^)(NSURLRequest* _Nullable))theHandler
{
	(void)theSession;
	(void)theResponse;
	// taskDescription carries the redirects left to follow; none: the 3xx itself is the answer
	NSInteger aLeft = theTask.taskDescription.integerValue;
	if (aLeft <= 0 || ![theRequest.URL.scheme.lowercaseString isEqualToString:@"https"])
	{
		theHandler(nil);
		return;
	}
	theTask.taskDescription = [NSString stringWithFormat:@"%ld", (long)(aLeft - 1)];

	NSMutableURLRequest* aNext = [theRequest mutableCopy];
	NSString* aFrom = theTask.originalRequest.URL.host.lowercaseString;
	NSString* aTo = theRequest.URL.host.lowercaseString;
	if (aFrom == nil || aTo == nil || ![aFrom isEqualToString:aTo])
		[aNext setValue:nil forHTTPHeaderField:@"Authorization"];	// as libcurl without CURLOPT_UNRESTRICTED_AUTH
	theHandler(aNext);
}

@end

namespace CloudSync
{

namespace
{

NSString* ToNs(const std::string& theText)
{
	NSString* aString = [[NSString alloc] initWithBytes:theText.data() length:theText.size() encoding:NSUTF8StringEncoding];
	return aString != nil ? aString : @"";
}

std::string FromNs(NSString* theText)
{
	const char* aUtf8 = theText != nil ? theText.UTF8String : nullptr;
	return aUtf8 != nullptr ? std::string(aUtf8) : std::string();
}

class AppleTransport : public Transport
{
	std::function<bool()>	mStop;
	NSURLSession*			mSession = nil;
	PvzpHttpDelegate*		mDelegate = nil;

public:
	explicit AppleTransport(std::function<bool()> theStop) : mStop(std::move(theStop))
	{
		NSURLSessionConfiguration* aConfig = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		aConfig.URLCache = nil;
		aConfig.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		aConfig.HTTPCookieStorage = nil;
		aConfig.HTTPShouldSetCookies = NO;
		aConfig.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
		aConfig.timeoutIntervalForRequest = 30;
		aConfig.timeoutIntervalForResource = 30;
		aConfig.waitsForConnectivity = NO;
		mDelegate = [PvzpHttpDelegate new];
		// the delegate runs on a queue of its own: a request may be made on the main thread, which then waits for it
		NSOperationQueue* aQueue = [NSOperationQueue new];
		aQueue.maxConcurrentOperationCount = 1;
		mSession = [NSURLSession sessionWithConfiguration:aConfig delegate:mDelegate delegateQueue:aQueue];
	}

	~AppleTransport() override
	{
		[mSession invalidateAndCancel];	// the session holds its delegate until it is invalidated
	}

	HttpResponse Request(const HttpRequest& theRequest) override
	{
		@autoreleasepool		// requests come from threads that have no pool of their own
		{
			return Send(theRequest);
		}
	}

private:
	HttpResponse Send(const HttpRequest& theRequest)
	{
		HttpResponse aResponse;
		NSURL* anUrl = [NSURL URLWithString:ToNs(theRequest.mUrl)];
		NSString* aScheme = anUrl.scheme.lowercaseString;
		if (anUrl == nil || !([aScheme isEqualToString:@"http"] || [aScheme isEqualToString:@"https"]))
		{
			aResponse.mError = "not an http or https address";
			return aResponse;
		}

		NSMutableURLRequest* aRequest = [NSMutableURLRequest requestWithURL:anUrl];
		aRequest.HTTPMethod = ToNs(theRequest.mMethod);
		aRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		aRequest.timeoutInterval = 30;
		[aRequest setValue:@"PvZ-Portable cloud save" forHTTPHeaderField:@"User-Agent"];
		[aRequest setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
		if (!theRequest.mUsername.empty())
		{
			NSData* aPair = [ToNs(theRequest.mUsername + ":" + theRequest.mPassword) dataUsingEncoding:NSUTF8StringEncoding];
			[aRequest setValue:[@"Basic " stringByAppendingString:[aPair base64EncodedStringWithOptions:0]] forHTTPHeaderField:@"Authorization"];
		}
		else if (!theRequest.mBearer.empty())
			[aRequest setValue:ToNs("Bearer " + theRequest.mBearer) forHTTPHeaderField:@"Authorization"];
		if (theRequest.mBody)
		{
			aRequest.HTTPBody = [NSData dataWithBytes:theRequest.mBody->data() length:theRequest.mBody->size()];
			[aRequest setValue:ToNs(theRequest.mContentType.empty() ? "application/octet-stream" : theRequest.mContentType)
				forHTTPHeaderField:@"Content-Type"];
		}
		for (const std::string& aHeader : theRequest.mHeaders)
		{
			size_t aColon = aHeader.find(':');
			if (aColon == std::string::npos)
				continue;
			std::string aValue = aHeader.substr(aColon + 1);
			size_t aStart = aValue.find_first_not_of(" \t");
			aValue = aStart == std::string::npos ? std::string() : aValue.substr(aStart);
			[aRequest setValue:ToNs(aValue) forHTTPHeaderField:ToNs(aHeader.substr(0, aColon))];
		}

		dispatch_semaphore_t aDone = dispatch_semaphore_create(0);
		__block NSData* aBody = nil;
		__block NSHTTPURLResponse* anAnswer = nil;
		__block NSError* anError = nil;
		NSURLSessionDataTask* aTask = [mSession dataTaskWithRequest:aRequest
			completionHandler:^(NSData* theData, NSURLResponse* theResponse, NSError* theError)
		{
			aBody = theData;
			anAnswer = [theResponse isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)theResponse : nil;
			anError = theError;
			dispatch_semaphore_signal(aDone);
		}];
		aTask.taskDescription = [NSString stringWithFormat:@"%ld", (long)(theRequest.mFollowRedirects ? MAX_REDIRECTS : 0)];
		[aTask resume];

		bool aStopped = false;
		while (dispatch_semaphore_wait(aDone, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) != 0)
		{
			if (!aStopped && mStop && mStop())
			{
				aStopped = true;
				[aTask cancel];		// the completion handler still runs, with NSURLErrorCancelled
			}
		}

		if (aStopped)
		{
			aResponse.mError = "stopped";
			return aResponse;
		}
		if (anError != nil || anAnswer == nil)
		{
			aResponse.mError = anError != nil ? FromNs(anError.localizedDescription) : "the server sent no HTTP answer";
			return aResponse;
		}
		aResponse.mStatus = static_cast<long>(anAnswer.statusCode);
		if (aBody != nil && aBody.length > 0)
			aResponse.mBody.assign(static_cast<const char*>(aBody.bytes), aBody.length);
		aResponse.mETag = FromNs([anAnswer valueForHTTPHeaderField:@"ETag"]);
		return aResponse;
	}
};

}

std::unique_ptr<Transport> MakeHttpTransport(std::function<bool()> theStop)
{
	@autoreleasepool
	{
		return std::make_unique<AppleTransport>(std::move(theStop));
	}
}

}

#endif
