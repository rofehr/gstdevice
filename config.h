#pragma once

#include <string>
#include <vdr/config.h>

// ============================================================
//  Codec selection enum
// ============================================================
enum eVideoCodec {
    vcH264 = 0,
    vcH265,
    vcCount
};

enum eAudioCodec {
    acAAC = 0,
    acMP3,
    acCount
};

// ============================================================
//  cGstConfig – persistent plugin settings
//  Stored in VDR's setup.conf via cPlugin::SetupStore/Parse
// ============================================================
class cGstConfig
{
public:
    // --- Video ---
    int         VideoCodec       = vcH264;      // eVideoCodec
    bool        HardwareDecode   = true;        // VA-API on/off
    std::string VideoSink        = "autovideosink";

    // --- Audio ---
    int         AudioCodec       = acAAC;       // eAudioCodec
    std::string AudioSink        = "autoaudiosink";

    // --- Sync & Volume ---
    int         AudioOffset      = 0;           // ms, range -500..+500
    int         Volume           = 255;         // 0-255

    // --- OSD ---
    int         OsdTimeout       = 5;           // seconds, 0 = manual only

    // --- Helpers ---
    const char *VideoCodecName() const {
        switch (VideoCodec) {
        case vcH264: return "H.264";
        case vcH265: return "H.265/HEVC";
        default:     return "H.264";
        }
    }

    const char *AudioCodecName() const {
        switch (AudioCodec) {
        case acAAC: return "AAC";
        case acMP3: return "MP3";
        default:    return "AAC";
        }
    }

    // GStreamer element names for selected codec + HW mode
    const char *VideoParseName() const {
        return (VideoCodec == vcH265) ? "h265parse" : "h264parse";
    }

    const char *VideoDecoderName() const {
        if (HardwareDecode) {
            // VA-API: one element handles both parse+decode
            return (VideoCodec == vcH265) ? "vaapih265dec" : "vaapih264dec";
        }
        return (VideoCodec == vcH265) ? "avdec_h265" : "avdec_h264";
    }

    const char *AudioParseName() const {
        return (AudioCodec == acMP3) ? "mpegaudioparse" : "aacparse";
    }

    const char *AudioDecoderName() const {
        return (AudioCodec == acMP3) ? "avdec_mp3" : "avdec_aac";
    }

    // VA-API: use vaapisink instead of autovideosink for zero-copy
    std::string EffectiveVideoSink() const {
        if (HardwareDecode && VideoSink == "autovideosink")
            return "vaapisink";
        return VideoSink;
    }
};

// Global config instance (defined in gstreamer.cpp)
extern cGstConfig GstConfig;
