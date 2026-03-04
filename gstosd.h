#pragma once

#include "gstreamer.h"
#include "config.h"
#include "globals.h"

// ============================================================
//  cGstOsd
//  Info banner using the native VDR cOsd / cPixmap API.
//
//  Displays at the bottom of the screen:
//    • Channel number + name + current time
//    • EPG title, time range, progress bar
//    • Video codec, resolution, fps + HW/SW badge
//    • Audio codec, sample rate, channels
//    • A/V offset, volume
//    • Pipeline state
//
//  VDR 2.7.7 API used:
//    cOsdProvider::NewOsd()      – create OSD object
//    cOsd::SetAreas()            – register 32-bit TrueColor area
//    cOsd::CreatePixmap()        – alpha-blended drawing surface
//    cPixmap::DrawRectangle()    – filled rectangles / backgrounds
//    cPixmap::DrawText()         – text with VDR theme fonts
//    cFont::GetFont()            – theme fonts (fontOsd, fontSml)
//    cOsd::Flush()               – submit to OSD provider
//    LOCK_SCHEDULES_READ macro   – thread-safe EPG access (VDR 2.4+)
// ============================================================
class cGstOsd : public cThread
{
public:
    cGstOsd();
    virtual ~cGstOsd() override;

    void ShowForChannel(const cChannel *Channel);
    void Toggle();
    void Hide();
    void UpdateStreamInfo(const sGstStreamInfo &info);
    void SetTimeout(int seconds) { m_timeoutSec = seconds; }

    bool IsVisible() const { return m_visible.load(); }

private:
    virtual void Action() override;   // auto-hide timer

    void Show();
    void Render();
    void Close();

    void DrawBackground(cPixmap *pm, int w, int h);
    void DrawChannelInfo(cPixmap *pm, int w, int &y,
                         const cFont *fontLg, const cFont *fontSm);
    void DrawEpgInfo(cPixmap *pm, int w, int &y, const cFont *font);
    void DrawProgressBar(cPixmap *pm, int x, int y, int w, int h, double frac);
    void DrawStreamInfo(cPixmap *pm, int w, int &y, const cFont *font);
    void DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *font);

    cOsd    *m_osd    = nullptr;
    cPixmap *m_pixmap = nullptr;

    std::atomic<bool> m_visible{false};
    std::mutex        m_mutex;
    int               m_timeoutSec = 5;
    time_t            m_showTime   = 0;

    // Content
    int         m_channelNumber = 0;
    std::string m_channelName;
    std::string m_epgTitle;
    std::string m_epgStart;
    std::string m_epgStop;
    double      m_epgProgress = 0.0;

    sGstStreamInfo *m_streamInfo = nullptr;   // heap-allocated copy

    // OSD geometry
    int m_osdWidth = 0, m_osdHeight = 0;

    // Colour palette (ARGB)
    static constexpr tColor kBg       = 0xCC000000;
    static constexpr tColor kBgAccent = 0xCC0D1B2A;
    static constexpr tColor kText     = 0xFFFFFFFF;
    static constexpr tColor kSubtext  = 0xFFAAAAAA;
    static constexpr tColor kAccent   = 0xFF3FA7D6;
    static constexpr tColor kBarFg    = 0xFF3FA7D6;
    static constexpr tColor kBarBg    = 0xFF444444;
    static constexpr tColor kHwBadge  = 0xFF27AE60;   // green  = VA-API
    static constexpr tColor kSwBadge  = 0xFFE67E22;   // orange = Software
};

