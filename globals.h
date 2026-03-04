#pragma once

/*
 * globals.h
 * Central declaration of all plugin-global singleton pointers and structs.
 * Defined (= storage allocated) exactly once in gstreamer.cpp.
 *
 * Every translation unit that needs access to GstDevice, GstOsd,
 * GstConfig or GstStreamInfo should include this header.
 *
 * Include order:
 *   gstreamer.h  (VDR + GStreamer system headers)
 *   globals.h    (this file)
 *
 * Never include gstdevice.h or gstosd.h from here – that would create
 * circular dependencies.  Forward declarations are sufficient.
 */

// ---- Forward declarations ----
class cGstConfig;
class cGstDevice;
class cGstOsd;
struct sGstStreamInfo;

// ---- Externs (defined in gstreamer.cpp) ----
extern cGstConfig     GstConfig;
extern cGstDevice    *GstDevice;
extern cGstOsd       *GstOsd;
extern sGstStreamInfo GstStreamInfo;
