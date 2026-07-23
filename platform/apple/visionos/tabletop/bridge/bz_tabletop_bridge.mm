#import "bz_tabletop_bridge.h"
#include "bz_tabletop_lifecycle.h"
#include <stdlib.h>
#include <string.h>

@implementation BZTabletopBridge {
    bzTabletopLifecycle_t *_lc;
}

- (instancetype)initWithArguments:(NSArray<NSString *> *)arguments {
    self = [super init];
    if (!self) {
        return nil;
    }

    int argc = (int)arguments.count;
    const char **argv = (const char **)calloc((size_t)argc, sizeof(const char *));
    for (int i = 0; i < argc; i++) {
        argv[i] = strdup(arguments[i].UTF8String);
    }
    _lc = BZ_TabletopCreate(argc, argv);
    for (int i = 0; i < argc; i++) {
        free((void *)argv[i]); // BZ_TabletopCreate deep-copies its own argv
    }
    free(argv);

    if (!_lc) {
        NSLog(@"BZTabletopBridge: BZ_TabletopCreate failed");
        return nil;
    }
    return self;
}

- (void)dealloc {
    BZ_TabletopDestroy(_lc);
}

- (void)start {
    BZ_TabletopStart(_lc);
}

- (void)suspend {
    BZ_TabletopSuspend(_lc);
}

- (void)resume {
    BZ_TabletopResume(_lc);
}

- (void)stop {
    BZ_TabletopStop(_lc);
}

- (BZTabletopState)state {
    switch (BZ_TabletopGetState(_lc)) {
        case BZ_TABLETOP_STATE_IDLE: return BZTabletopStateIdle;
        case BZ_TABLETOP_STATE_STARTING: return BZTabletopStateStarting;
        case BZ_TABLETOP_STATE_RUNNING: return BZTabletopStateRunning;
        case BZ_TABLETOP_STATE_FAILED: return BZTabletopStateFailed;
        case BZ_TABLETOP_STATE_SUSPENDED: return BZTabletopStateSuspended;
        case BZ_TABLETOP_STATE_STOPPED: return BZTabletopStateStopped;
    }
    return BZTabletopStateFailed; // unreachable: silences -Wreturn-type for the enum switch above
}

- (nullable NSString *)lastError {
    const char *err = BZ_TabletopLastError(_lc);
    return err ? @(err) : nil;
}

@end
