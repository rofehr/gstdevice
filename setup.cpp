#include "setup.h"
#include "gstdevice.h"   // for GstDevice (extern) + ReconfigurePipeline
#include "gstosd.h"

#include <vdr/plugin.h>
#include <cstring>
#include <cstdio>

// ============================================================
//  String tables (must match enum order in config.h)
// ============================================================
const char * const cGstMenuSetup::kVideoCodecNames[] = { "H.264", "H.265/HEVC", nullptr };
const char * const cGstMenuSetup::kAudioCodecNames[] = { "AAC",   "MP3",        nullptr };

// ============================================================
//  Constructor – copy working values from global config
// ============================================================
cGstMenuSetup::cGstMenuSetup()
{
    m_videoCodec  = GstConfig.VideoCodec;
    m_hwDecode    = GstConfig.HardwareDecode ? 1 : 0;
    m_audioCodec  = GstConfig.AudioCodec;
    m_audioOffset = GstConfig.AudioOffset;
    m_volume      = GstConfig.Volume;
    m_osdTimeout  = GstConfig.OsdTimeout;

    strncpy(m_videoSink, GstConfig.VideoSink.c_str(), sizeof(m_videoSink) - 1);
    m_videoSink[sizeof(m_videoSink) - 1] = '\0';
    strncpy(m_audioSink, GstConfig.AudioSink.c_str(), sizeof(m_audioSink) - 1);
    m_audioSink[sizeof(m_audioSink) - 1] = '\0';

    m_prevVideoCodec = m_videoCodec;
    m_prevHwDecode   = m_hwDecode;

    BuildMenu();
}

// ============================================================
//  BuildMenu – construct all menu items
// ============================================================
void cGstMenuSetup::BuildMenu()
{
    Clear();
    SetTitle(tr("GStreamer Plugin Setup"));

    // ---- Video ----
    Add(new cMenuEditStraItem(tr("Video codec"),        &m_videoCodec, vcCount, kVideoCodecNames));
    Add(new cMenuEditBoolItem(tr("Hardware decode (VA-API)"), &m_hwDecode));

    // Show active decoder as read-only info line
    Add(new cOsdItem(cString::sprintf("%s:	%s  →  %s",
        tr("Active decoder"),
        GstConfig.VideoParseName(),
        GstConfig.VideoDecoderName()), osUnknown, false));

    // VDR 2.7.7: cMenuEditStrItem(name, value, length)
    // The 'allowed' character-set parameter was removed in VDR 2.4+
    Add(new cMenuEditStrItem(tr("Video sink"), m_videoSink, sizeof(m_videoSink)));

    // ---- Audio ----
    Add(new cOsdItem("", osUnknown, false));   // spacer
    Add(new cMenuEditStraItem(tr("Audio codec"), &m_audioCodec, acCount, kAudioCodecNames));
    Add(new cMenuEditStrItem(tr("Audio sink"), m_audioSink, sizeof(m_audioSink)));

    // ---- Sync & Volume ----
    Add(new cOsdItem("", osUnknown, false));
    Add(new cMenuEditIntItem(tr("Audio offset (ms)"),  &m_audioOffset, -500, 500));
    Add(new cMenuEditIntItem(tr("Volume"),             &m_volume,         0, 255));

    // ---- OSD ----
    Add(new cOsdItem("", osUnknown, false));
    Add(new cMenuEditIntItem(tr("OSD timeout (s, 0=manual)"), &m_osdTimeout, 0, 30));

    // ---- Pipeline info (read-only) ----
    Add(new cOsdItem("", osUnknown, false));

    char vInfo[64], aInfo[64];
    snprintf(vInfo, sizeof(vInfo), "%s + %s",
        GstConfig.VideoDecoderName(),
        GstConfig.EffectiveVideoSink().c_str());
    snprintf(aInfo, sizeof(aInfo), "%s + %s",
        GstConfig.AudioDecoderName(),
        GstConfig.AudioSink.c_str());

    Add(new cOsdItem(cString::sprintf("%s:\t%s", tr("Video pipeline"), vInfo), osUnknown, false));
    Add(new cOsdItem(cString::sprintf("%s:\t%s", tr("Audio pipeline"), aInfo), osUnknown, false));

    Display();
}

// ============================================================
//  RebuildIfNeeded – live menu refresh when codec/HW changes
// ============================================================
void cGstMenuSetup::RebuildIfNeeded()
{
    if (m_videoCodec != m_prevVideoCodec || m_hwDecode != m_prevHwDecode) {
        // Apply temporary changes so VideoDecoderName() reflects current state
        GstConfig.VideoCodec      = m_videoCodec;
        GstConfig.HardwareDecode  = m_hwDecode != 0;
        m_prevVideoCodec          = m_videoCodec;
        m_prevHwDecode            = m_hwDecode;
        BuildMenu();
    }
}

// ============================================================
//  ProcessKey – handle remote control in setup menu
// ============================================================
eOSState cGstMenuSetup::ProcessKey(eKeys Key)
{
    eOSState state = cMenuSetupPage::ProcessKey(Key);
    if (state == osUnknown && Key != kNone)
        RebuildIfNeeded();
    return state;
}

// ============================================================
//  Store – commit working values to GstConfig + VDR setup.conf
// ============================================================
void cGstMenuSetup::Store()
{
    GstConfig.VideoCodec     = m_videoCodec;
    GstConfig.HardwareDecode = m_hwDecode != 0;
    GstConfig.AudioCodec     = m_audioCodec;
    GstConfig.AudioOffset    = m_audioOffset;
    GstConfig.Volume         = m_volume;
    GstConfig.OsdTimeout     = m_osdTimeout;
    GstConfig.VideoSink      = m_videoSink;
    GstConfig.AudioSink      = m_audioSink;

    // Persist to VDR's setup.conf
    SetupStore("VideoCodec",     GstConfig.VideoCodec);
    SetupStore("HardwareDecode", GstConfig.HardwareDecode ? 1 : 0);
    SetupStore("AudioCodec",     GstConfig.AudioCodec);
    SetupStore("AudioOffset",    GstConfig.AudioOffset);
    SetupStore("Volume",         GstConfig.Volume);
    SetupStore("OsdTimeout",     GstConfig.OsdTimeout);
    SetupStore("VideoSink",      GstConfig.VideoSink.c_str());
    SetupStore("AudioSink",      GstConfig.AudioSink.c_str());

    // Apply changes to running pipeline immediately
    if (GstDevice)
        GstDevice->ReconfigurePipeline();

    // Update OSD timeout
    if (GstOsd)
        GstOsd->SetTimeout(GstConfig.OsdTimeout);
}
