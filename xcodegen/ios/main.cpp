// iOS app entry — pure C++. UIApplicationMain has C linkage; the
// principal/delegate-class params take NSString* which accepts nullptr.
// No Obj-C++ required. Replace with a call into the CMake-built
// `glibre_runtime` static lib once `runtime/` lands.

extern "C" int UIApplicationMain(
    int argc, char* argv[], const void* principalClassName, const void* delegateClassName
);

int main(int argc, char* argv[]) { return UIApplicationMain(argc, argv, nullptr, nullptr); }
