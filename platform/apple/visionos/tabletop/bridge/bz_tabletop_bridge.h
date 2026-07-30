#ifndef __bz_tabletop_bridge_h__
#define __bz_tabletop_bridge_h__

#import <Foundation/Foundation.h>

/*
 * BZTabletopBridge — thin Objective-C++ NSObject wrapper around the
 * portable bz_tabletop_lifecycle.h/.c core, satisfying this layer's
 * "minimal Objective-C++ lifecycle host" requirement.
 *
 * This class holds no business logic of its own: every method below is a
 * direct forward to the matching BZ_Tabletop* call. It contains zero
 * Swift/SwiftUI/RealityKit code by design — a later layer hosts this
 * class from Swift, but this file must not know that layer exists.
 */

typedef NS_ENUM(NSInteger, BZTabletopState) {
    BZTabletopStateIdle = 0,
    BZTabletopStateStarting,
    BZTabletopStateRunning,
    BZTabletopStateFailed,
    BZTabletopStateSuspended,
    BZTabletopStateStopped,
};

NS_ASSUME_NONNULL_BEGIN

@interface BZTabletopBridge : NSObject

// argv[0]-style process name plus engine args (e.g. "-data", "<dir>",
// "+map", "<name>"), matching common/bz_runtime.h's bzRuntimeArgs_t shape.
- (instancetype)initWithArguments:(NSArray<NSString *> *)arguments NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

// Spawns the dedicated, single-shot engine thread and blocks until
// BZ_RuntimeInit() completes (state leaves Starting). Failed and Stopped
// are terminal — a further call after either is rejected as a no-op;
// create a new BZTabletopBridge instance to run again.
- (void)start;
- (void)suspend;
- (void)resume;
- (BOOL)submitMap:(NSString *)map;
// Blocks until the engine thread has shut down and been joined.
- (void)stop;

@property (nonatomic, readonly) BZTabletopState state;
@property (nonatomic, readonly, nullable) NSString *lastError;

@end

NS_ASSUME_NONNULL_END

#endif
