#pragma once

/*
 * gstreamer.h  –  Common system includes and plugin-wide constants
 *
 * Include order every translation unit should follow:
 *   1. gstreamer.h   (system + VDR + GStreamer headers)
 *   2. config.h      (plain types, no system deps)
 *   3. own header    (gstdevice.h / gstosd.h / setup.h)
 */

// ── VDR minimum version guard ────────────────────────────────────────────────
#include <vdr/plugin.h>
#if APIVERSNUM < 20600
#  error "VDR >= 2.6.0 required (APIVERSNUM 20600)"
#endif

// ── VDR headers ──────────────────────────────────────────────────────────────
#include <vdr/device.h>
#include <vdr/menuitems.h>
#include <vdr/osd.h>
#include <vdr/font.h>
#include <vdr/thread.h>
#include <vdr/channels.h>
#include <vdr/epg.h>
#include <vdr/tools.h>
#include <vdr/config.h>

// ── GStreamer headers ─────────────────────────────────────────────────────────
// NOTE: <gst/gst.h> pulls in <gst/gstdevice.h> which defines the C type
//       GstDevice.  We therefore never use "GstDevice" as an identifier
//       in our own code – we use "GstOutputDevice" for our cGstDevice*.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

// ── C++ standard library ─────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ── Plugin metadata ───────────────────────────────────────────────────────────
#define PLUGIN_NAME_I18N   "gstreamer"
#define PLUGIN_VERSION     "0.3.0"
#define PLUGIN_DESCRIPTION "GStreamer output plugin for VDR (H.264/H.265, VA-API, AAC/MP3)"
