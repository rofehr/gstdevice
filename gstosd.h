#pragma once

/*
 * gstosd.h  –  Channel / EPG / stream-info banner
 *
 * Uses the native VDR cOsd / cPixmap TrueColour API.
 * EPG is accessed via the LOCK_SCHEDULES_READ macro (VDR 2.4+).
 */

#include "gstreamer.h"  // VDR system headers (cOsd, cFont, cThread …)
#include "config.h"     // sGstStreamInfo, cGstConfig

// ─────────────────────────────────────────────────────────────────────────────
class cGstOsd : public cThread
{
public:
    cGstOsd();
    virtual ~cGstOsd() override;

    // Show info banner for the given channel (reads EPG internally)
    void ShowForChannel(const cChannel *Channel);

    // Toggle visibility
    void Toggle();

    // Hide immediately
    void Hide();

    // Refresh stream-info section without re-reading EPG
    void UpdateStreamInfo(const sGstStreamInfo &info);

    // Set auto-hide delay (0 = never auto-hide)
    void SetTimeout(int seconds) { m_timeoutSec = seconds; }

    bool IsVisible() const { return m_visible.load(); }

private:
    // cThread: auto-hide timer
    virtual void Action() override;

    void Show();
    void Render();
    void Close();

    // Drawing helpers
    void DrawBackground  (cPixmap *pm, int w, int h);
    void DrawChannelInfo (cPixmap *pm, int w, int &y,
                          const cFont *fontLg, const cFont *fontMd);
    void DrawEpgInfo     (cPixmap *pm, int w, int &y, const cFont *font);
    void DrawProgressBar (cPixmap *pm, int x, int y, int w, int h, double frac);
    void DrawStreamInfo  (cPixmap *pm, int w, int &y, const cFont *font);
    void DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *font);

    // VDR OSD objects
    cOsd    *m_osd    = nullptr;
    cPixmap *m_pixmap = nullptr;

    // State
    std::atomic<bool> m_visible{false};
    std::mutex        m_mutex;
    int               m_timeoutSec = 5;
    time_t            m_showTime   = 0;

    // Content
    int            m_channelNumber = 0;
    std::string    m_channelName;
    std::string    m_epgTitle;
    std::string    m_epgStart;
    std::string    m_epgStop;
    double         m_epgProgress = 0.0;

    sGstStreamInfo m_streamInfo;   // copy updated by UpdateStreamInfo()

    // OSD panel geometry
    int m_osdW = 0;
    int m_osdH = 0;   // panel height = 22 % of screen

    // Colour palette (ARGB)
    static constexpr tColor kBg       = 0xCC000000u;
    static constexpr tColor kBgStripe = 0xCC0D1B2Au;
    static constexpr tColor kText     = 0xFFFFFFFFu;
    static constexpr tColor kSub      = 0xFFAAAAAAu;
    static constexpr tColor kAccent   = 0xFF3FA7D6u;
    static constexpr tColor kBarFg    = 0xFF3FA7D6u;
    static constexpr tColor kBarBg    = 0xFF444444u;
    static constexpr tColor kHwBadge  = 0xFF27AE60u;  // green  – VA-API
    static constexpr tColor kSwBadge  = 0xFFE67E22u;  // orange – Software
};

// Singleton – defined in gstreamer.cpp
extern cGstOsd *GstOsd;
