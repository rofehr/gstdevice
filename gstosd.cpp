#include "gstosd.h"
#include <vdr/osdbase.h>
#include <vdr/skins.h>
#include <vdr/device.h>
#include <cmath>
#include <cstring>

// ============================================================
//  Constructor / Destructor
// ============================================================
cGstOsd::cGstOsd()
    : cThread("GstOsd-timer", true)
{
    dsyslog("[gstreamer] cGstOsd created");
}

cGstOsd::~cGstOsd()
{
    Hide();
    Cancel(3);
    dsyslog("[gstreamer] cGstOsd destroyed");
}

// ============================================================
//  Public interface
// ============================================================
void cGstOsd::ShowForChannel(const cChannel *Channel)
{
    if (!Channel)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_channelNumber = Channel->Number();
    m_channelName   = Channel->Name() ? Channel->Name() : "";

    // EPG lookup
    m_epgTitle    = "";
    m_epgStart    = "";
    m_epgStop     = "";
    m_epgProgress = 0.0;

    {
        cSchedulesLock schedulesLock;
        const cSchedules *schedules = cSchedules::Schedules(schedulesLock);
        if (schedules) {
            const cSchedule *sched = schedules->GetSchedule(Channel);
            if (sched) {
                const cEvent *ev = sched->GetPresentEvent();
                if (ev) {
                    m_epgTitle = ev->Title() ? ev->Title() : "";

                    // Format start / stop times
                    struct tm tmStart, tmStop;
                    time_t tStart = ev->StartTime();
                    time_t tStop  = ev->StartTime() + ev->Duration();
                    localtime_r(&tStart, &tmStart);
                    localtime_r(&tStop,  &tmStop);
                    char buf[16];
                    strftime(buf, sizeof(buf), "%H:%M", &tmStart); m_epgStart = buf;
                    strftime(buf, sizeof(buf), "%H:%M", &tmStop);  m_epgStop  = buf;

                    // Progress fraction
                    time_t now = time(nullptr);
                    if (ev->Duration() > 0)
                        m_epgProgress = std::max(0.0, std::min(1.0,
                            (double)(now - tStart) / (double)ev->Duration()));
                }
            }
        }
    }

    Show();
}

void cGstOsd::Toggle()
{
    if (m_visible.load())
        Hide();
    else
        Show();
}

void cGstOsd::Hide()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Close();
}

void cGstOsd::UpdateStreamInfo(const sGstStreamInfo &info)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_streamInfo = info;
    }
    if (m_visible.load())
        Render();
}

// ============================================================
//  Show / Hide / Render
// ============================================================
void cGstOsd::Show()
{
    Close();   // close any previous OSD first

    // Determine OSD area: full width, bottom ~22 % of screen
    int osdW = cOsd::OsdWidth();
    int osdH = cOsd::OsdHeight();
    if (osdW <= 0) osdW = 1920;
    if (osdH <= 0) osdH = 1080;

    int panelH  = osdH * 22 / 100;   // ~238 px on 1080p
    int panelY  = osdH - panelH;

    m_osdLeft   = 0;
    m_osdTop    = panelY;
    m_osdWidth  = osdW;
    m_osdHeight = panelH;

    // Create VDR OSD object via the registered OSD provider
    m_osd = cOsdProvider::NewOsd(m_osdLeft, m_osdTop);
    if (!m_osd) {
        esyslog("[gstreamer] OSD: cOsdProvider::NewOsd() failed");
        return;
    }

    // Create a single TrueColor area covering the entire panel
    tArea area = { 0, 0, m_osdWidth - 1, m_osdHeight - 1, 32 };
    if (m_osd->SetAreas(&area, 1) != oeOk) {
        esyslog("[gstreamer] OSD: SetAreas failed");
        DELETENULL(m_osd);
        return;
    }

    // Create pixmap (layer 0, full panel size)
    m_pixmap = m_osd->CreatePixmap(0,
        cRect(0, 0, m_osdWidth, m_osdHeight),
        cRect(0, 0, m_osdWidth, m_osdHeight));
    if (!m_pixmap) {
        esyslog("[gstreamer] OSD: CreatePixmap failed");
        DELETENULL(m_osd);
        return;
    }

    m_visible   = true;
    m_showTime  = time(nullptr);

    Render();

    // Start auto-hide timer thread
    if (m_timeoutSec > 0)
        Start();
}

void cGstOsd::Close()
{
    m_visible = false;
    if (m_osd) {
        m_osd->DestroyPixmap(m_pixmap);
        m_pixmap = nullptr;
        DELETENULL(m_osd);
    }
}

// ============================================================
//  Render – draws all content onto the pixmap
// ============================================================
void cGstOsd::Render()
{
    if (!m_osd || !m_pixmap)
        return;

    int w = m_osdWidth;
    int h = m_osdHeight;

    // --- Fonts from VDR theme ---
    const cFont *fontOsdTitle = cFont::GetFont(fontOsdTitle);
    const cFont *fontOsd      = cFont::GetFont(fontOsd);
    const cFont *fontSml      = cFont::GetFont(fontSml);

    // Fallback: if theme fonts unavailable use fixed sizes
    if (!fontOsdTitle) fontOsdTitle = cFont::CreateFont(cFont::GetFontFileName(Setup.FontOsd),    Setup.FontOsdSize + 6);
    if (!fontOsd)      fontOsd      = cFont::CreateFont(cFont::GetFontFileName(Setup.FontOsd),    Setup.FontOsdSize);
    if (!fontSml)      fontSml      = cFont::CreateFont(cFont::GetFontFileName(Setup.FontSml),    Setup.FontSmlSize);

    m_pixmap->Lock();

    // Clear
    m_pixmap->Fill(clrTransparent);

    int y = 0;

    DrawBackground(m_pixmap, w, h);
    DrawChannelInfo(m_pixmap, w, y, fontOsdTitle, fontOsd);
    DrawEpgInfo(m_pixmap, w, y, fontOsd);
    DrawStreamInfo(m_pixmap, w, y, fontSml);
    DrawPipelineState(m_pixmap, w, y, fontSml);

    m_pixmap->Unlock();
    m_osd->Flush();
}

// ============================================================
//  Drawing helpers
// ============================================================
void cGstOsd::DrawBackground(cPixmap *pm, int w, int h)
{
    // Main background – semi-transparent dark panel
    pm->DrawRectangle(cRect(0, 0, w, h), kColBg);

    // Top accent bar (4 px)
    pm->DrawRectangle(cRect(0, 0, w, 4), kColAccent);

    // Left accent stripe
    pm->DrawRectangle(cRect(0, 0, 6, h), kColBgAccent);
}

void cGstOsd::DrawChannelInfo(cPixmap *pm, int w, int &y,
                               const cFont *fontLarge, const cFont *fontSmall)
{
    if (!fontLarge || !fontSmall) return;

    int margin  = 20;
    int lineH   = fontLarge->Height();

    y = 12;

    // Channel number badge
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%d", m_channelNumber);
    int numW = fontLarge->Width(numBuf) + 16;
    pm->DrawRectangle(cRect(margin, y, numW, lineH), kColAccent);
    pm->DrawText(cPoint(margin + 8, y), numBuf, kColText, clrTransparent, fontLarge);

    // Channel name
    pm->DrawText(cPoint(margin + numW + 12, y),
        m_channelName.c_str(), kColText, clrTransparent, fontLarge);

    // Current time (top-right)
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char timeBuf[8];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmNow);
    int timeW = fontLarge->Width(timeBuf);
    pm->DrawText(cPoint(w - margin - timeW, y),
        timeBuf, kColAccent, clrTransparent, fontLarge);

    y += lineH + 6;
}

void cGstOsd::DrawEpgInfo(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font || m_epgTitle.empty()) return;

    int margin = 20;
    int lineH  = font->Height();

    // Title
    pm->DrawText(cPoint(margin + 8, y),
        m_epgTitle.c_str(), kColText, clrTransparent, font);

    // Time range (right-aligned)
    if (!m_epgStart.empty()) {
        std::string timeRange = m_epgStart + " – " + m_epgStop;
        int trW = font->Width(timeRange.c_str());
        pm->DrawText(cPoint(w - margin - trW, y),
            timeRange.c_str(), kColSubtext, clrTransparent, font);
    }

    y += lineH + 4;

    // Progress bar
    int barH = 6;
    int barW = w - 2 * margin - 8;
    DrawProgressBar(pm, margin + 8, y, barW, barH, m_epgProgress);

    y += barH + 10;
}

void cGstOsd::DrawProgressBar(cPixmap *pm, int x, int y, int w, int h, double fraction)
{
    // Background track
    pm->DrawRectangle(cRect(x, y, w, h), kColBarBg);

    // Filled portion
    int filled = (int)(w * std::max(0.0, std::min(1.0, fraction)));
    if (filled > 0)
        pm->DrawRectangle(cRect(x, y, filled, h), kColBar);

    // Progress percentage label (small)
    // (drawn after bar, no font needed for the bar itself)
}

void cGstOsd::DrawStreamInfo(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font) return;

    int margin = 20;
    int lineH  = font->Height();
    int col2   = w / 2;

    // ---- Video info ----
    {
        char buf[128];
        if (m_streamInfo.videoWidth > 0 && m_streamInfo.videoHeight > 0)
            snprintf(buf, sizeof(buf), "%s  %dx%d  %.2g fps",
                m_streamInfo.videoCodec.c_str(),
                m_streamInfo.videoWidth, m_streamInfo.videoHeight,
                m_streamInfo.videoFps);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo.videoCodec.empty() ? "Video: –" : m_streamInfo.videoCodec.c_str());

        pm->DrawText(cPoint(margin + 8, y),
            buf, kColSubtext, clrTransparent, font);

        // HW/SW badge
        const char *badge   = m_streamInfo.hwDecode ? "VA-API" : "SW";
        tColor      badgeCol= m_streamInfo.hwDecode ? kColHwBadge : kColSwBadge;
        int         badgeW  = font->Width(badge) + 10;
        int         badgeX  = col2 - badgeW - 8;
        pm->DrawRectangle(cRect(badgeX, y, badgeW, lineH - 2), badgeCol);
        pm->DrawText(cPoint(badgeX + 5, y), badge, kColText, clrTransparent, font);
    }

    // ---- Audio info ----
    {
        char buf[128];
        if (m_streamInfo.audioSampleRate > 0)
            snprintf(buf, sizeof(buf), "%s  %d Hz  %dch",
                m_streamInfo.audioCodec.c_str(),
                m_streamInfo.audioSampleRate,
                m_streamInfo.audioChannels);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo.audioCodec.empty() ? "Audio: –" : m_streamInfo.audioCodec.c_str());

        pm->DrawText(cPoint(col2 + 8, y),
            buf, kColSubtext, clrTransparent, font);
    }

    y += lineH + 4;

    // ---- A/V Sync offset + Volume ----
    {
        char syncBuf[64];
        snprintf(syncBuf, sizeof(syncBuf), "A/V Offset: %+d ms", GstConfig.AudioOffset);
        pm->DrawText(cPoint(margin + 8, y),
            syncBuf, kColSubtext, clrTransparent, font);

        char volBuf[32];
        snprintf(volBuf, sizeof(volBuf), "Vol: %d%%", GstConfig.Volume * 100 / 255);
        pm->DrawText(cPoint(col2 + 8, y),
            volBuf, kColSubtext, clrTransparent, font);
    }

    y += lineH + 4;
}

void cGstOsd::DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font || m_streamInfo.pipelineState.empty()) return;

    int margin = 20;

    std::string stateStr = "Pipeline: " + m_streamInfo.pipelineState;
    if (m_streamInfo.pipelineState == "Buffering")
        stateStr += " " + std::to_string(m_streamInfo.bufferingPercent) + " %";

    tColor col = (m_streamInfo.pipelineState == "Playing") ? kColHwBadge : kColAccent;
    pm->DrawText(cPoint(w - margin - font->Width(stateStr.c_str()), y),
        stateStr.c_str(), col, clrTransparent, font);
}

// ============================================================
//  Auto-hide timer thread
// ============================================================
void cGstOsd::Action()
{
    while (Running()) {
        cCondWait::SleepMs(500);
        if (!m_visible.load())
            break;
        if (m_timeoutSec > 0 &&
            difftime(time(nullptr), m_showTime) >= m_timeoutSec)
        {
            dsyslog("[gstreamer] OSD auto-hide timeout");
            Hide();
            break;
        }
    }
}
