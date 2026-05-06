// macOS app entry — thin shim. Replace body with a call into the
// CMake-built `glibre_runtime` static lib once `runtime/` lands.
#import <Cocoa/Cocoa.h>

int main(int argc, const char* argv[]) {
    return NSApplicationMain(argc, argv);
}
