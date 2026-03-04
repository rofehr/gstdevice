#pragma once

// ============================================================
//  cGstOsd – GStreamer plugin OSD output via native VDR cOsd API
//
//  Uses the standard VDR OSD stack:
//    cOsdProvider::NewOsd()  → cOsd
//    cOsd::CreatePixmap()    → cPixmap  (TrueColor, alpha-blended)
//    cFont::GetFont()        → cFont    (VDR theme fonts)
//
//  Renders a channel-info banner (bottom 20 % of screen) showing:
//    • Channel number + name
//    • Current EPG event title + progress bar
//    • Video codec, resolution, framerate
//    • Audio codec, sample-rate, channels
//    • A/V sync offset, volume
//    • Pipeline state (Playing / Paused / Buffering …%)
//
//  The OSD is shown:
//    – automatically on channel switch (cGstOsd::ShowForChannel)
//    – on demand via cGstOsd::Toggle() (bound to a key in cGstPlayer)
//    – auto-hides after a configurable timeout (default 5 s)
// ============================================================

#include <vdr/osd.h>
#include <vdr/font.h>
#include <vdr/thread.h>
#include <vdr/channels.h>
#include <vdr/epg.h>
#include <vdr/tools.h>

#include "config.h"

#include <string>
#include <atomic>
#include <mutex>

// ---- Video stream info filled by cGstDevice ----
struct sGstStreamInfo {
    // Video
    std::string videoCodec;          // "H.264", "H.265"
    int         videoWidth    = 0;
    int         videoHeight   = 0;
    double      videoFps      = 0.0;
    bool        hwDecode      = false;

    // Audio
    std::string audioCodec;          // "AAC", "MP3"
    int         audioSampleRate = 0;
    int         audioChannels   = 0;

    // Pipeline
    std::string pipelineState;       // "Playing", "Paused", "Buffering"
    int         bufferingPercent = 0;
};

extern sGstStreamInfo GstStreamInfo;   // defined in gstreamer.cpp

// ============================================================
class cGstOsd : public cThread
{
public:
    cGstOsd();
    virtual ~cGstOsd();

    // Show OSD for a specific channel (called on channel switch)
    void ShowForChannel(const cChannel *Channel);

    // Toggle OSD on/off (called on key press)
    void Toggle();

    // Force hide immediately
    void Hide();

    // Update stream info and redraw if visible
    void UpdateStreamInfo(const sGstStreamInfo &info);

    bool IsVisible() const { return m_visible.load(); }

    // Timeout in seconds (0 = stay until manually closed)
    void SetTimeout(int seconds) { m_timeoutSec = seconds; }

private:
    // cThread
    virtual void Action() override;   // auto-hide timer thread

    void Show();
    void Render();
    void Close();

    // Drawing helpers
    void DrawBackground(cPixmap *pm, int w, int h);
    void DrawChannelInfo(cPixmap *pm, int w, int &y, const cFont *fontLarge, const cFont *fontSmall);
    void DrawEpgInfo(cPixmap *pm, int w, int &y, const cFont *font);
    void DrawProgressBar(cPixmap *pm, int x, int y, int w, int h, double fraction);
    void DrawStreamInfo(cPixmap *pm, int w, int &y, const cFont *font);
    void DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *font);

    // OSD objects (owned)
    cOsd        *m_osd     = nullptr;
    cPixmap     *m_pixmap  = nullptr;

    // State
    std::atomic<bool>  m_visible{false};
    std::mutex         m_mutex;
    int                m_timeoutSec  = 5;
    time_t             m_showTime    = 0;

    // Content
    int                m_channelNumber = 0;
    std::string        m_channelName;
    std::string        m_epgTitle;
    std::string        m_epgStart;
    std::string        m_epgStop;
    double             m_epgProgress  = 0.0;   // 0.0 … 1.0
    sGstStreamInfo     m_streamInfo;

    // OSD geometry (computed from OSD area)
    int  m_osdLeft   = 0;
    int  m_osdTop    = 0;
    int  m_osdWidth  = 0;
    int  m_osdHeight = 0;

    // Colours (ARGB)
    static constexpr tColor kColBg        = 0xCC000000;  // semi-transparent black
    static constexpr tColor kColBgAccent  = 0xCC1A1A2E;  // dark blue tint
    static constexpr tColor kColText      = 0xFFFFFFFF;  // white
    static constexpr tColor kColSubtext   = 0xFFAAAAAA;  // grey
    static constexpr tColor kColAccent    = 0xFF3FA7D6;  // VDR-blue
    static constexpr tColor kColBar       = 0xFF3FA7D6;
    static constexpr tColor kColBarBg     = 0xFF444444;
    static constexpr tColor kColHwBadge   = 0xFF27AE60;  // green  = VA-API
    static constexpr tColor kColSwBadge   = 0xFFE67E22;  // orange = Software
};

// Global OSD instance (defined in gstreamer.cpp)
extern cGstOsd *GstOsd;
