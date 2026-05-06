// macOS app entry — pure C++. NSApplicationMain has C linkage; no Obj-C++
// required. Replace with a call into the CMake-built `glibre_runtime`
// static lib once `runtime/` lands.

extern "C" int NSApplicationMain(int argc, const char* argv[]);

int main(int argc, const char* argv[]) { return NSApplicationMain(argc, argv); }
