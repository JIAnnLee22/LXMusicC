#ifndef LXM_PLATFORM_H
#define LXM_PLATFORM_H
#include <android/native_activity.h>
#include <stdbool.h>
// Pixel insets of system bars and display cutout; read-only framework JNI.
bool platform_insets(ANativeActivity *activity, int insets[4]);
#endif
