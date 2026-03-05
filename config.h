#pragma once

/*
 * config.h  –  Plugin configuration and plain data types
 *
 * This header has NO dependencies on VDR or GStreamer headers so it
 * can be included first by any translation unit without side-effects.
 */

#include <string>

// ── Codec selectors ──────────────────────────────────────────────────────────
enum eVideoCodec { vcH264 = 0, vcH265, vcCount };
enum eAudioCodec { acAAC  = 0, acMP3,  acCount };

// ── Live stream properties ────────────────────────────────────────────────────
// Filled by cGstDevice::QueryAndUpdateStreamInfo(), read by cGstOsd.
// Kept here (not in gstdevice.h) so other headers can use this type
// without pulling in the full device header.
struct sGstStreamInfo {
    std::string videoCodec;
    int         videoWidth      = 0;
    int         videoHeight     = 0;
    double      videoFps        = 0.0;
    bool        hwDecode        = false;
    std::string audioCodec;
    int         audioSampleRate = 0;
    int         audioChannels   = 0;
    std::string pipelineState;       // "Playing" | "Paused" | "Buffering" | "Idle"
    int         bufferingPercent= 0;
};

// ── Persistent plugin settings ────────────────────────────────────────────────
class cGstConfig
{
public:
    // Video
    int         VideoCodec      = vcH264;
    bool        HardwareDecode  = true;
    std::string VideoSink       = "autovideosink";

    // Audio
    int         AudioCodec      = acAAC;
    std::string AudioSink       = "autoaudiosink";

    // Sync / volume
    int         AudioOffset     = 0;    // ms, range –500 … +500
    int         Volume          = 255;  // 0 … 255

    // OSD
    int         OsdTimeout      = 5;    // s, 0 = manual dismiss only

    // ── Derived GStreamer element names ───────────────────────────────────────
    const char *VideoCodecName()   const
        { return VideoCodec == vcH265 ? "H.265/HEVC" : "H.264"; }
    const char *AudioCodecName()   const
        { return AudioCodec == acMP3  ? "MP3"        : "AAC";   }

    const char *VideoParseName()   const
        { return VideoCodec == vcH265 ? "h265parse"     : "h264parse";     }
    const char *VideoDecoderName() const {
        if (HardwareDecode)
            return VideoCodec == vcH265 ? "vaapih265dec" : "vaapih264dec";
        return VideoCodec == vcH265 ? "avdec_h265" : "avdec_h264";
    }
    const char *AudioParseName()   const
        { return AudioCodec == acMP3 ? "mpegaudioparse" : "aacparse";  }
    const char *AudioDecoderName() const
        { return AudioCodec == acMP3 ? "avdec_mp3"      : "avdec_aac"; }

    // For VA-API prefer zero-copy vaapisink over generic autovideosink
    std::string EffectiveVideoSink() const {
        if (HardwareDecode && VideoSink == "autovideosink")
            return "vaapisink";
        return VideoSink;
    }
};

// ── Global singletons (defined in gstreamer.cpp) ─────────────────────────────
extern cGstConfig     GstConfig;
extern sGstStreamInfo GstStreamInfo;
