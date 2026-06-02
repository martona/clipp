// iOS pulls shared C++/ObjC++ into the target by #include rather than adding out-of-tree
// file references (Xcode insists on copying/moving those). NetworkRuntime.cpp depends on
// the interface-address fingerprint and the nw_path change monitor, so compile those into
// this same TU. Only the Apple .mm of the monitor is needed here -- NetworkChangeMonitor.cpp
// is empty under __APPLE__ (the impl lives in the .mm, which needs ARC + Network.framework).
#include "../../../src/NetworkInterfaces.cpp"
#include "../../../src/NetworkChangeMonitor_Apple.mm"
#include "../../../src/NetworkRuntime.cpp"
