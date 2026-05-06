// iOS app entry — thin shim. Replace body with a call into the
// CMake-built `glibre_runtime` static lib once `runtime/` lands.
#import <UIKit/UIKit.h>

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, nil);
    }
}
