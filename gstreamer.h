#pragma once

/*
 * vdr-plugin-gstreamer
 * GStreamer-based output plugin for VDR
 *
 * Requires: VDR >= 2.7.7 (APIVERSNUM >= 30009)
 *           GStreamer >= 1.20
 *           C++17
 */

// ---- Enforce minimum VDR API version ----
#include <vdr/plugin.h>
#if APIVERSNUM < 30009
#error "VDR >= 2.7.7 (APIVERSNUM 30009) required"
#endif

#include <vdr/device.h>
#include <vdr/menuitems.h>
#include <vdr/osd.h>
#include <vdr/font.h>
#include <vdr/thread.h>
#include <vdr/channels.h>
#include <vdr/epg.h>
#include <vdr/tools.h>
#include <vdr/config.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>

#define PLUGIN_NAME_I18N  "gstreamer"
#define PLUGIN_VERSION    "0.3.0"
#define PLUGIN_DESCRIPTION "GStreamer output plugin for VDR (H.264/H.265, VA-API, AAC)"

// Forward declarations of global singletons (defined in gstreamer.cpp)
class cGstDevice;
class cGstOsd;
extern cGstDevice    *GstDevice;
extern cGstOsd       *GstOsd;
