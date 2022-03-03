#import "SentryProtoPolyfills.h"
#import "SentryTime.h"

@implementation SentryBacktrace

- (instancetype)init {
    self = [super init];
    if (!self) {
        return nil;
    }

    addresses = [NSMutableArray array];
    return self;
}

@end

@implementation SentryProfilingEntry

- (instancetype)init {
    self = [super init];
    if (!self) {
        return nil;
    }

    backtrace = [[SentryBacktrace alloc] init];
    return self;
}

@end

@implementation SentryProfilingTraceLogger

- (instancetype)init {
    self = [super init];
    if (!self) {
        return nil;
    }

    referenceUptimeNs = sentry::profiling::time::getUptimeNs();
    return self;
}

@end