#pragma once

/*
 * globals.h
 * Declares all plugin-global singletons.
 * Definitions (storage) are in gstreamer.cpp.
 *
 * Include chain (no cycles):
 *   config.h   defines cGstConfig, sGstStreamInfo   (no plugin deps)
 *   globals.h  includes config.h, forward-declares cGstDevice/cGstOsd
 *   gstdevice.h / gstosd.h  include globals.h
 */

#include "config.h"   // cGstConfig, sGstStreamInfo (complete types needed for extern objects)

// Pointer-only forward declarations – full type not required for pointer externs
class cGstDevice;
class cGstOsd;

// ---- Global singletons (defined once in gstreamer.cpp) ----
extern cGstConfig     GstConfig;       // full object – config.h provides complete type
extern cGstDevice    *GstDevice;       // pointer    – forward-declare is sufficient
extern cGstOsd       *GstOsd;          // pointer    – forward-declare is sufficient
extern sGstStreamInfo GstStreamInfo;   // full object – config.h provides complete type
