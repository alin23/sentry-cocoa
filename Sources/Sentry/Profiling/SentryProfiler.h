#import <Foundation/Foundation.h>

@class SentryEnvelopeItem, SentryTransaction;

NS_ASSUME_NONNULL_BEGIN

@interface SentryProfiler : NSObject

/** Clears all accumulated profiling data and starts profiling. */
- (void)start;
/** Stops profiling. */
- (void)stop;

/**
 * Builds an envelope item using the currently accumulated profile data.
 */
- (nullable SentryEnvelopeItem *)buildEnvelopeItemForTransaction:(SentryTransaction *)transaction;

@end

NS_ASSUME_NONNULL_END
