#include "setup.h"
#include "gstosd.h"
#include <vdr/tools.h>
#include <cstring>

// ============================================================
//  Static option arrays
// ============================================================
const char * const cGstMenuSetup::kVideoCodecNames[] = {
    "H.264 (HD)",
    "H.265 / HEVC (UHD)",
    nullptr
};

const char * const cGstMenuSetup::kAudioCodecNames[] = {
    "AAC",
    "MP3",
    nullptr
};

// ============================================================
//  Constructor
// ============================================================
cGstMenuSetup::cGstMenuSetup()
{
    // Copy global config into local working copies
    m_videoCodec     = GstConfig.VideoCodec;
    m_hardwareDecode = GstConfig.HardwareDecode ? 1 : 0;
    m_audioCodec     = GstConfig.AudioCodec;
    m_audioOffset    = GstConfig.AudioOffset;
    m_volume         = GstConfig.Volume;
    m_osdTimeout     = GstConfig.OsdTimeout;

    strncpy(m_videoSink, GstConfig.VideoSink.c_str(), sizeof(m_videoSink) - 1);
    m_videoSink[sizeof(m_videoSink) - 1] = '\0';

    strncpy(m_audioSink, GstConfig.AudioSink.c_str(), sizeof(m_audioSink) - 1);
    m_audioSink[sizeof(m_audioSink) - 1] = '\0';

    m_prevVideoCodec     = m_videoCodec;
    m_prevHardwareDecode = m_hardwareDecode;

    SetSection(tr("GStreamer Output Plugin"));
    BuildMenu();
}

// ============================================================
//  Menu construction
// ============================================================
void cGstMenuSetup::BuildMenu()
{
    int current = Current();
    Clear();

    // ---- Section: Video ----
    Add(new cOsdItem(tr("--- Video ---"), osUnknown, false));

    Add(new cMenuEditStraItem(tr("Video Codec"),
        &m_videoCodec,
        vcCount,
        kVideoCodecNames));

    Add(new cMenuEditBoolItem(tr("Hardware Decode (VA-API)"),
        &m_hardwareDecode,
        tr("Software"),
        tr("VA-API")));

    // Show active decoder info (read-only hint)
    {
        cGstConfig tmp;
        tmp.VideoCodec     = m_videoCodec;
        tmp.HardwareDecode = (m_hardwareDecode != 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s  →  %s",
            tmp.VideoParseName(),
            tmp.VideoDecoderName());
        cString info = cString::sprintf(tr("  Active decoder: %s"), buf);
        Add(new cOsdItem(*info, osUnknown, false));
    }

    Add(new cMenuEditStrItem(tr("Video Sink"),
        m_videoSink,
        sizeof(m_videoSink)));

    // ---- Section: Audio ----
    Add(new cOsdItem(tr("--- Audio ---"), osUnknown, false));

    Add(new cMenuEditStraItem(tr("Audio Codec"),
        &m_audioCodec,
        acCount,
        kAudioCodecNames));

    Add(new cMenuEditStrItem(tr("Audio Sink"),
        m_audioSink,
        sizeof(m_audioSink)));

    // ---- Section: Sync & Volume ----
    Add(new cOsdItem(tr("--- Sync & Volume ---"), osUnknown, false));

    Add(new cMenuEditIntItem(tr("Audio Offset (ms)"),
        &m_audioOffset,
        -500, 500));

    Add(new cMenuEditIntItem(tr("Volume"),
        &m_volume,
        0, 255));

    // ---- Section: OSD ----
    Add(new cOsdItem(tr("--- OSD ---"), osUnknown, false));

    Add(new cMenuEditIntItem(tr("Info Banner Timeout (s)"),
        &m_osdTimeout,
        0, 30));
    {
        cString hint = cString::sprintf(tr("  (0 = stay until OK pressed)"));
        Add(new cOsdItem(*hint, osUnknown, false));
    }

    // ---- Section: Info ----
    Add(new cOsdItem(tr("--- Pipeline Info ---"), osUnknown, false));
    {
        cGstConfig tmp;
        tmp.VideoCodec     = m_videoCodec;
        tmp.HardwareDecode = (m_hardwareDecode != 0);
        tmp.AudioCodec     = m_audioCodec;
        tmp.VideoSink      = m_videoSink;

        cString vline = cString::sprintf("  Video: %s + %s",
            tmp.VideoDecoderName(),
            tmp.EffectiveVideoSink().c_str());
        cString aline = cString::sprintf("  Audio: %s + %s",
            tmp.AudioDecoderName(),
            m_audioSink);

        Add(new cOsdItem(*vline, osUnknown, false));
        Add(new cOsdItem(*aline, osUnknown, false));
    }

    SetCurrent(Get(current));
    Display();
}

void cGstMenuSetup::RebuildMenu()
{
    // Only rebuild when a codec or HW toggle has actually changed
    if (m_videoCodec     != m_prevVideoCodec ||
        m_hardwareDecode != m_prevHardwareDecode)
    {
        m_prevVideoCodec     = m_videoCodec;
        m_prevHardwareDecode = m_hardwareDecode;
        BuildMenu();
    }
}

// ============================================================
//  Key handler – live preview of decoder info while navigating
// ============================================================
eOSState cGstMenuSetup::ProcessKey(eKeys Key)
{
    eOSState state = cMenuSetupPage::ProcessKey(Key);

    // After any edit key, refresh the info line if codec changed
    if (Key != kNone)
        RebuildMenu();

    return state;
}

// ============================================================
//  Store – commit local copies → global config → VDR setup.conf
// ============================================================
void cGstMenuSetup::Store()
{
    GstConfig.VideoCodec     = m_videoCodec;
    GstConfig.HardwareDecode = (m_hardwareDecode != 0);
    GstConfig.AudioCodec     = m_audioCodec;
    GstConfig.AudioOffset    = m_audioOffset;
    GstConfig.Volume         = m_volume;
    GstConfig.VideoSink      = m_videoSink;
    GstConfig.AudioSink      = m_audioSink;
    GstConfig.OsdTimeout     = m_osdTimeout;

    // Persist all values in VDR's setup.conf
    SetupStore("VideoCodec",     GstConfig.VideoCodec);
    SetupStore("HardwareDecode", GstConfig.HardwareDecode ? 1 : 0);
    SetupStore("AudioCodec",     GstConfig.AudioCodec);
    SetupStore("AudioOffset",    GstConfig.AudioOffset);
    SetupStore("Volume",         GstConfig.Volume);
    SetupStore("VideoSink",      GstConfig.VideoSink.c_str());
    SetupStore("AudioSink",      GstConfig.AudioSink.c_str());
    SetupStore("OsdTimeout",     GstConfig.OsdTimeout);

    // Apply timeout change live
    if (GstOsd)
        GstOsd->SetTimeout(GstConfig.OsdTimeout);

    isyslog("[gstreamer] Config saved: VideoCodec=%s HW=%d AudioCodec=%s "
            "AudioOffset=%dms Volume=%d VideoSink=%s AudioSink=%s",
        GstConfig.VideoCodecName(),
        GstConfig.HardwareDecode,
        GstConfig.AudioCodecName(),
        GstConfig.AudioOffset,
        GstConfig.Volume,
        GstConfig.VideoSink.c_str(),
        GstConfig.AudioSink.c_str());
}
