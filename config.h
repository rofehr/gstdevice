#pragma once

#include <string>

enum eVideoCodec { vcH264 = 0, vcH265, vcCount };
enum eAudioCodec { acAAC  = 0, acMP3,  acCount };

// ============================================================
//  cGstConfig – persistent plugin settings (setup.conf)
// ============================================================
class cGstConfig
{
public:
    // Video
    int         VideoCodec     = vcH264;
    bool        HardwareDecode = true;
    std::string VideoSink      = "autovideosink";

    // Audio
    int         AudioCodec     = acAAC;
    std::string AudioSink      = "autoaudiosink";

    // Sync & volume
    int         AudioOffset    = 0;      // ms, -500..+500
    int         Volume         = 255;    // 0..255

    // OSD
    int         OsdTimeout     = 5;      // s, 0 = manual only

    // ---- Derived element names ----
    const char *VideoCodecName()   const { return VideoCodec == vcH265 ? "H.265/HEVC" : "H.264"; }
    const char *AudioCodecName()   const { return AudioCodec == acMP3  ? "MP3"        : "AAC";   }

    const char *VideoParseName()   const { return VideoCodec == vcH265 ? "h265parse"       : "h264parse";       }
    const char *VideoDecoderName() const {
        if (HardwareDecode)
            return VideoCodec == vcH265 ? "vaapih265dec" : "vaapih264dec";
        return VideoCodec == vcH265 ? "avdec_h265" : "avdec_h264";
    }
    const char *AudioParseName()   const { return AudioCodec == acMP3 ? "mpegaudioparse" : "aacparse";    }
    const char *AudioDecoderName() const { return AudioCodec == acMP3 ? "avdec_mp3"      : "avdec_aac";   }

    // For VA-API prefer vaapisink (zero-copy) over autovideosink
    std::string EffectiveVideoSink() const {
        if (HardwareDecode && VideoSink == "autovideosink")
            return "vaapisink";
        return VideoSink;
    }
};


// ============================================================
//  sGstStreamInfo – live stream properties (filled by the device,
//  consumed by the OSD).  Lives here so globals.h can include
//  config.h alone without pulling in all of gstdevice.h.
// ============================================================
struct sGstStreamInfo {
    std::string videoCodec;
    int         videoWidth       = 0;
    int         videoHeight      = 0;
    double      videoFps         = 0.0;
    bool        hwDecode         = false;
    std::string audioCodec;
    int         audioSampleRate  = 0;
    int         audioChannels    = 0;
    std::string pipelineState;       // "Playing" | "Paused" | "Buffering"
    int         bufferingPercent = 0;
};

extern cGstConfig GstConfig;
