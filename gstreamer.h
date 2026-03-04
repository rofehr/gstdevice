#pragma once

#include <vdr/plugin.h>
#include <vdr/device.h>
#include <vdr/player.h>
#include <vdr/remux.h>
#include <vdr/menuitems.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <string>
#include <mutex>
#include <atomic>

#define PLUGIN_NAME_I18N  "gstreamer"
#define PLUGIN_VERSION    "0.2.0"
#define PLUGIN_DESCRIPTION "GStreamer output plugin for VDR (H.264/H.265, VA-API, AAC)"
