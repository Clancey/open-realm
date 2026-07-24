#import "bz_tabletop_bridge.h"

/*
 * Link-only smoke test for build.mk's <platform>-bridge targets: proves
 * libopenwarcraft3-bridge.a (bz_tabletop_lifecycle.o + bz_tabletop_bridge.o)
 * resolves every symbol it needs from libopenwarcraft3-engine.a plus
 * Foundation/libz/libpthread. Deliberately never calls -start (that would
 * run BZ_RuntimeInit(), which needs real WC3 data files this build has no
 * access to) - constructing and destroying the bridge is enough to touch
 * every exported symbol path this static archive combination provides.
 *
 * This binary is a -target ...-simulator/-target ...-xros Mach-O and is
 * NOT runnable directly from a bare xcrun/clang toolchain (it needs
 * simctl or a device to execute) - the <platform>-bridge Make targets only
 * build it, they do not run it.
 */
int main(void) {
    @autoreleasepool {
        NSArray<NSString *> *args = @[@"tabletop"];
        BZTabletopBridge *bridge = [[BZTabletopBridge alloc] initWithArguments:args];
        if (!bridge) {
            return 1;
        }
    }
    return 0;
}
