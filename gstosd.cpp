#include "gstosd.h"
#include "gstdevice.h"     // for sGstStreamInfo, GstStreamInfo

#include <vdr/osdbase.h>
#include <vdr/skins.h>
#include <vdr/device.h>

#include <cmath>
#include <cstring>
#include <ctime>

// ============================================================
//  Constructor / Destructor
// ============================================================
cGstOsd::cGstOsd()
    : cThread("GstOsd-timer", /*lowPriority=*/true)
{
    m_streamInfo = new sGstStreamInfo();
    dsyslog("[gstreamer] cGstOsd created");
}

cGstOsd::~cGstOsd()
{
    Hide();
    Cancel(3);
    delete m_streamInfo;
    dsyslog("[gstreamer] cGstOsd destroyed");
}

// ============================================================
//  Public interface
// ============================================================
void cGstOsd::ShowForChannel(const cChannel *Channel)
{
    if (!Channel) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_channelNumber = Channel->Number();
    m_channelName   = Channel->Name();   // cChannel::Name() never NULL in 2.7+

    m_epgTitle = m_epgStart = m_epgStop = "";
    m_epgProgress = 0.0;

    // ---- EPG lookup using VDR 2.4+ LOCK_SCHEDULES_READ macro ----
    // This macro declares a const cSchedules *Schedules pointer that is valid
    // until the end of the enclosing block.  No manual Remove() needed.
    {
        LOCK_SCHEDULES_READ;
        if (Schedules) {
            const cSchedule *sched = Schedules->GetSchedule(Channel);
            if (sched) {
                const cEvent *ev = sched->GetPresentEvent();
                if (ev) {
                    // cEvent::Title() guaranteed non-NULL since VDR 2.7.5
                    m_epgTitle = ev->Title();

                    struct tm tmA, tmB;
                    time_t tStart = ev->StartTime();
                    time_t tStop  = tStart + ev->Duration();
                    localtime_r(&tStart, &tmA);
                    localtime_r(&tStop,  &tmB);

                    char buf[8];
                    strftime(buf, sizeof(buf), "%H:%M", &tmA); m_epgStart = buf;
                    strftime(buf, sizeof(buf), "%H:%M", &tmB); m_epgStop  = buf;

                    if (ev->Duration() > 0) {
                        time_t now = time(nullptr);
                        m_epgProgress = std::max(0.0, std::min(1.0,
                            static_cast<double>(now - tStart) / ev->Duration()));
                    }
                }
            }
        }
    }   // LOCK_SCHEDULES_READ released here

    Show();
}

void cGstOsd::Toggle()
{
    if (m_visible.load()) Hide();
    else                  Show();
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
        *m_streamInfo = info;
    }
    if (m_visible.load())
        Render();
}

// ============================================================
//  Show / Close / Render
// ============================================================
void cGstOsd::Show()
{
    Close();   // ensure clean state

    int totalW = cOsd::OsdWidth();
    int totalH = cOsd::OsdHeight();
    if (totalW <= 0) totalW = 1920;
    if (totalH <= 0) totalH = 1080;

    m_osdWidth  = totalW;
    m_osdHeight = totalH * 22 / 100;   // bottom 22 %
    int panelY  = totalH - m_osdHeight;

    m_osd = cOsdProvider::NewOsd(0, panelY);
    if (!m_osd) {
        esyslog("[gstreamer] OSD: cOsdProvider::NewOsd failed");
        return;
    }

    tArea area = { 0, 0, m_osdWidth - 1, m_osdHeight - 1, 32 };
    if (m_osd->SetAreas(&area, 1) != oeOk) {
        esyslog("[gstreamer] OSD: SetAreas failed");
        DELETENULL(m_osd);
        return;
    }

    m_pixmap = m_osd->CreatePixmap(0,
        cRect(0, 0, m_osdWidth, m_osdHeight),
        cRect(0, 0, m_osdWidth, m_osdHeight));
    if (!m_pixmap) {
        esyslog("[gstreamer] OSD: CreatePixmap failed");
        DELETENULL(m_osd);
        return;
    }

    m_visible  = true;
    m_showTime = time(nullptr);

    Render();

    if (m_timeoutSec > 0)
        Start();   // start auto-hide timer thread
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

void cGstOsd::Render()
{
    if (!m_osd || !m_pixmap) return;

    const int w = m_osdWidth;
    const int h = m_osdHeight;

    // ---- Fonts: use VDR theme fonts (VDR 2.7.7 API) ----
    // cFont::GetFont() returns a pointer owned by VDR – do NOT delete.
    // fontOsd  = main OSD font (always available)
    // fontSml  = small OSD font (always available)
    // fontOsdTitle was added in VDR 2.4.0 – guard with APIVERSNUM.
#if APIVERSNUM >= 20400
    const cFont *fontLg = cFont::GetFont(fontOsdTitle);
#else
    const cFont *fontLg = cFont::GetFont(fontOsd);
#endif
    const cFont *fontMd = cFont::GetFont(fontOsd);
    const cFont *fontSm = cFont::GetFont(fontSml);

    // GetFont() never returns nullptr for fontOsd/fontSml in VDR 2.7+,
    // but guard defensively.
    if (!fontLg) fontLg = fontMd;
    if (!fontMd) fontMd = fontSm;

    m_pixmap->Lock();
    m_pixmap->Fill(clrTransparent);

    int y = 0;
    DrawBackground(m_pixmap, w, h);
    DrawChannelInfo(m_pixmap, w, y, fontLg, fontMd);
    DrawEpgInfo(m_pixmap, w, y, fontMd);
    DrawStreamInfo(m_pixmap, w, y, fontSm);
    DrawPipelineState(m_pixmap, w, y, fontSm);

    m_pixmap->Unlock();
    m_osd->Flush();
}

// ============================================================
//  Drawing helpers
// ============================================================
void cGstOsd::DrawBackground(cPixmap *pm, int w, int h)
{
    pm->DrawRectangle(cRect(0, 0, w, h), kBg);
    pm->DrawRectangle(cRect(0, 0, w, 4), kAccent);   // top accent bar
    pm->DrawRectangle(cRect(0, 0, 6, h), kBgAccent); // left stripe
}

void cGstOsd::DrawChannelInfo(cPixmap *pm, int w, int &y,
                               const cFont *fontLg, const cFont *fontSm)
{
    if (!fontLg || !fontSm) return;
    const int M = 20;
    y = 12;

    // Channel number badge
    char num[8];
    snprintf(num, sizeof(num), "%d", m_channelNumber);
    int numW = fontLg->Width(num) + 16;
    pm->DrawRectangle(cRect(M, y, numW, fontLg->Height()), kAccent);
    pm->DrawText(cPoint(M + 8, y), num, kText, clrTransparent, fontLg);

    // Channel name
    pm->DrawText(cPoint(M + numW + 12, y),
        m_channelName.c_str(), kText, clrTransparent, fontLg);

    // Current time (right-aligned)
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char tbuf[8];
    strftime(tbuf, sizeof(tbuf), "%H:%M", &tmNow);
    pm->DrawText(cPoint(w - M - fontLg->Width(tbuf), y),
        tbuf, kAccent, clrTransparent, fontLg);

    y += fontLg->Height() + 6;
}

void cGstOsd::DrawEpgInfo(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font || m_epgTitle.empty()) return;
    const int M = 20;

    // EPG title
    pm->DrawText(cPoint(M + 8, y),
        m_epgTitle.c_str(), kText, clrTransparent, font);

    // Time range (right-aligned)
    if (!m_epgStart.empty()) {
        std::string tr = m_epgStart + " – " + m_epgStop;
        pm->DrawText(cPoint(w - M - font->Width(tr.c_str()), y),
            tr.c_str(), kSubtext, clrTransparent, font);
    }
    y += font->Height() + 4;

    // Progress bar
    DrawProgressBar(pm, M + 8, y, w - 2 * M - 8, 6, m_epgProgress);
    y += 6 + 10;
}

void cGstOsd::DrawProgressBar(cPixmap *pm, int x, int y, int w, int h, double frac)
{
    pm->DrawRectangle(cRect(x, y, w, h), kBarBg);
    int filled = static_cast<int>(w * std::max(0.0, std::min(1.0, frac)));
    if (filled > 0)
        pm->DrawRectangle(cRect(x, y, filled, h), kBarFg);
}

void cGstOsd::DrawStreamInfo(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font || !m_streamInfo) return;
    const int M    = 20;
    const int col2 = w / 2;
    const int lh   = font->Height();

    // ---- Video info + HW/SW badge ----
    {
        char buf[128];
        if (m_streamInfo->videoWidth > 0)
            snprintf(buf, sizeof(buf), "%s  %dx%d  %.4g fps",
                m_streamInfo->videoCodec.c_str(),
                m_streamInfo->videoWidth, m_streamInfo->videoHeight,
                m_streamInfo->videoFps);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo->videoCodec.empty() ? "Video: –" : m_streamInfo->videoCodec.c_str());

        pm->DrawText(cPoint(M + 8, y), buf, kSubtext, clrTransparent, font);

        const char *badge    = m_streamInfo->hwDecode ? "VA-API" : "SW";
        tColor      badgeCol = m_streamInfo->hwDecode ? kHwBadge : kSwBadge;
        int         bw       = font->Width(badge) + 10;
        int         bx       = col2 - bw - 8;
        pm->DrawRectangle(cRect(bx, y, bw, lh - 2), badgeCol);
        pm->DrawText(cPoint(bx + 5, y), badge, kText, clrTransparent, font);
    }

    // ---- Audio info ----
    {
        char buf[128];
        if (m_streamInfo->audioSampleRate > 0)
            snprintf(buf, sizeof(buf), "%s  %d Hz  %dch",
                m_streamInfo->audioCodec.c_str(),
                m_streamInfo->audioSampleRate,
                m_streamInfo->audioChannels);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo->audioCodec.empty() ? "Audio: –" : m_streamInfo->audioCodec.c_str());

        pm->DrawText(cPoint(col2 + 8, y), buf, kSubtext, clrTransparent, font);
    }
    y += lh + 4;

    // ---- A/V offset + Volume ----
    {
        char sync[32], vol[32];
        snprintf(sync, sizeof(sync), "A/V Offset: %+d ms", GstConfig.AudioOffset);
        snprintf(vol,  sizeof(vol),  "Vol: %d%%",           GstConfig.Volume * 100 / 255);
        pm->DrawText(cPoint(M + 8,    y), sync, kSubtext, clrTransparent, font);
        pm->DrawText(cPoint(col2 + 8, y), vol,  kSubtext, clrTransparent, font);
    }
    y += lh + 4;
}

void cGstOsd::DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *font)
{
    if (!font || !m_streamInfo || m_streamInfo->pipelineState.empty()) return;
    const int M = 20;

    std::string state = "Pipeline: " + m_streamInfo->pipelineState;
    if (m_streamInfo->pipelineState == "Buffering")
        state += " " + std::to_string(m_streamInfo->bufferingPercent) + " %";

    tColor col = (m_streamInfo->pipelineState == "Playing") ? kHwBadge : kAccent;
    pm->DrawText(cPoint(w - M - font->Width(state.c_str()), y),
        state.c_str(), col, clrTransparent, font);
}

// ============================================================
//  Auto-hide timer thread
// ============================================================
void cGstOsd::Action()
{
    while (Running()) {
        cCondWait::SleepMs(500);
        if (!m_visible.load()) break;
        if (m_timeoutSec > 0 &&
            difftime(time(nullptr), m_showTime) >= m_timeoutSec)
        {
            dsyslog("[gstreamer] OSD auto-hide");
            Hide();
            break;
        }
    }
}
