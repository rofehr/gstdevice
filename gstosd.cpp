/*
 * gstosd.cpp  –  GStreamer info banner OSD implementation
 *
 * VDR 2.6+ API:
 *   LOCK_SCHEDULES_READ  – replaces cSchedulesLock (VDR 2.4+)
 *   cOsd / cPixmap       – TrueColour drawing
 *   cFont::GetFont()     – theme fonts; fontOsdTitle since VDR 2.4
 */

#include "gstosd.h"

#include <vdr/osdbase.h>
#include <vdr/skins.h>

#include <cmath>
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

cGstOsd::cGstOsd()
    : cThread("GstOsd", /*lowPriority=*/true)
{
    dsyslog("[gstreamer] cGstOsd()");
}

cGstOsd::~cGstOsd()
{
    Hide();
    Cancel(3);
    dsyslog("[gstreamer] ~cGstOsd()");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public interface
// ─────────────────────────────────────────────────────────────────────────────

void cGstOsd::ShowForChannel(const cChannel *Channel)
{
    if (!Channel) return;

    std::lock_guard<std::mutex> lk(m_mutex);

    m_channelNumber = Channel->Number();
    m_channelName   = Channel->Name();  // never nullptr in VDR 2.7+
    m_epgTitle = m_epgStart = m_epgStop = "";
    m_epgProgress = 0.0;

    // ── EPG lookup (VDR 2.4+ LOCK_SCHEDULES_READ macro) ──────────────────────
    // Declares: const cSchedules *Schedules (valid until end of block).
    {
        LOCK_SCHEDULES_READ;
        if (Schedules) {
            const cSchedule *sched = Schedules->GetSchedule(Channel);
            if (sched) {
                const cEvent *ev = sched->GetPresentEvent();
                if (ev) {
                    m_epgTitle = ev->Title();  // non-null since VDR 2.7.5

                    const time_t tStart = ev->StartTime();
                    const time_t tStop  = tStart + ev->Duration();
                    struct tm tmA, tmB;
                    localtime_r(&tStart, &tmA);
                    localtime_r(&tStop,  &tmB);
                    char buf[8];
                    strftime(buf, sizeof(buf), "%H:%M", &tmA); m_epgStart = buf;
                    strftime(buf, sizeof(buf), "%H:%M", &tmB); m_epgStop  = buf;

                    if (ev->Duration() > 0) {
                        const time_t now = time(nullptr);
                        m_epgProgress = std::max(0.0, std::min(1.0,
                            (double)(now - tStart) / ev->Duration()));
                    }
                }
            }
        }
    }  // LOCK_SCHEDULES_READ released here

    Show();
}

void cGstOsd::Toggle()
{
    if (m_visible.load()) Hide();
    else                  Show();
}

void cGstOsd::Hide()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    Close();
}

void cGstOsd::UpdateStreamInfo(const sGstStreamInfo &info)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streamInfo = info;
    }
    if (m_visible.load())
        Render();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Show / Close / Render
// ─────────────────────────────────────────────────────────────────────────────

void cGstOsd::Show()
{
    Close();

    int screenW = cOsd::OsdWidth();
    int screenH = cOsd::OsdHeight();
    if (screenW <= 0) screenW = 1920;
    if (screenH <= 0) screenH = 1080;

    m_osdW = screenW;
    m_osdH = screenH * 22 / 100;  // bottom 22 % of screen
    const int panelY = screenH - m_osdH;

    m_osd = cOsdProvider::NewOsd(0, panelY);
    if (!m_osd) { esyslog("[gstreamer] OSD: NewOsd failed"); return; }

    tArea area = { 0, 0, m_osdW - 1, m_osdH - 1, 32 };
    if (m_osd->SetAreas(&area, 1) != oeOk) {
        esyslog("[gstreamer] OSD: SetAreas failed");
        DELETENULL(m_osd);
        return;
    }

    m_pixmap = m_osd->CreatePixmap(0,
        cRect(0, 0, m_osdW, m_osdH),
        cRect(0, 0, m_osdW, m_osdH));
    if (!m_pixmap) {
        esyslog("[gstreamer] OSD: CreatePixmap failed");
        DELETENULL(m_osd);
        return;
    }

    m_visible  = true;
    m_showTime = time(nullptr);
    Render();

    if (m_timeoutSec > 0)
        Start();  // launch auto-hide timer thread
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

    // ── Fonts ─────────────────────────────────────────────────────────────────
    // cFont::GetFont() returns a VDR-owned pointer; do NOT delete.
    // fontOsdTitle exists since VDR 2.4.0.
#if APIVERSNUM >= 20400
    const cFont *fLg = cFont::GetFont(fontOsdTitle);
#else
    const cFont *fLg = cFont::GetFont(fontOsd);
#endif
    const cFont *fMd = cFont::GetFont(fontOsd);
    const cFont *fSm = cFont::GetFont(fontSml);

    if (!fLg) fLg = fMd;
    if (!fMd) fMd = fSm;
    if (!fSm) fSm = fMd;
    if (!fLg || !fMd || !fSm) return;  // no fonts at all – give up

    m_pixmap->Lock();
    m_pixmap->Fill(clrTransparent);

    int y = 0;
    DrawBackground  (m_pixmap, m_osdW, m_osdH);
    DrawChannelInfo (m_pixmap, m_osdW, y, fLg, fMd);
    DrawEpgInfo     (m_pixmap, m_osdW, y, fMd);
    DrawStreamInfo  (m_pixmap, m_osdW, y, fSm);
    DrawPipelineState(m_pixmap, m_osdW, y, fSm);

    m_pixmap->Unlock();
    m_osd->Flush();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Drawing helpers
// ─────────────────────────────────────────────────────────────────────────────

void cGstOsd::DrawBackground(cPixmap *pm, int w, int h)
{
    pm->DrawRectangle(cRect(0, 0, w, h), kBg);
    pm->DrawRectangle(cRect(0, 0, w, 4), kAccent);    // top accent bar
    pm->DrawRectangle(cRect(0, 0, 6, h), kBgStripe);  // left colour stripe
}

void cGstOsd::DrawChannelInfo(cPixmap *pm, int w, int &y,
                               const cFont *fLg, const cFont *fMd)
{
    const int M = 20;
    y = 12;

    // Channel-number badge
    char num[8];
    snprintf(num, sizeof(num), "%d", m_channelNumber);
    const int numW = fLg->Width(num) + 16;
    pm->DrawRectangle(cRect(M, y, numW, fLg->Height()), kAccent);
    pm->DrawText(cPoint(M + 8, y), num, kText, clrTransparent, fLg);

    // Channel name
    pm->DrawText(cPoint(M + numW + 12, y),
        m_channelName.c_str(), kText, clrTransparent, fLg);

    // Current time (right-aligned)
    time_t now = time(nullptr);
    struct tm tmNow; localtime_r(&now, &tmNow);
    char tbuf[8]; strftime(tbuf, sizeof(tbuf), "%H:%M", &tmNow);
    pm->DrawText(cPoint(w - M - fLg->Width(tbuf), y),
        tbuf, kAccent, clrTransparent, fLg);

    y += fLg->Height() + 6;
}

void cGstOsd::DrawEpgInfo(cPixmap *pm, int w, int &y, const cFont *f)
{
    if (m_epgTitle.empty()) return;
    const int M = 20;

    pm->DrawText(cPoint(M + 8, y),
        m_epgTitle.c_str(), kText, clrTransparent, f);

    if (!m_epgStart.empty()) {
        const std::string tr = m_epgStart + " \xe2\x80\x93 " + m_epgStop;
        pm->DrawText(cPoint(w - M - f->Width(tr.c_str()), y),
            tr.c_str(), kSub, clrTransparent, f);
    }
    y += f->Height() + 4;

    DrawProgressBar(pm, M + 8, y, w - 2 * M - 8, 6, m_epgProgress);
    y += 16;
}

void cGstOsd::DrawProgressBar(cPixmap *pm, int x, int y, int w, int h, double frac)
{
    pm->DrawRectangle(cRect(x, y, w, h), kBarBg);
    const int filled = static_cast<int>(w * std::max(0.0, std::min(1.0, frac)));
    if (filled > 0)
        pm->DrawRectangle(cRect(x, y, filled, h), kBarFg);
}

void cGstOsd::DrawStreamInfo(cPixmap *pm, int w, int &y, const cFont *f)
{
    const int M   = 20;
    const int c2  = w / 2;
    const int lh  = f->Height();

    // ── Video ─────────────────────────────────────────────────────────────────
    {
        char buf[128];
        if (m_streamInfo.videoWidth > 0)
            snprintf(buf, sizeof(buf), "%s  %dx%d  %.4g fps",
                m_streamInfo.videoCodec.c_str(),
                m_streamInfo.videoWidth, m_streamInfo.videoHeight,
                m_streamInfo.videoFps);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo.videoCodec.empty()
                    ? "Video: \xe2\x80\x93" : m_streamInfo.videoCodec.c_str());

        pm->DrawText(cPoint(M + 8, y), buf, kSub, clrTransparent, f);

        // HW / SW badge
        const char *badge    = m_streamInfo.hwDecode ? "VA-API" : "SW";
        const tColor badgeC  = m_streamInfo.hwDecode ? kHwBadge : kSwBadge;
        const int    bw      = f->Width(badge) + 10;
        const int    bx      = c2 - bw - 8;
        pm->DrawRectangle(cRect(bx, y, bw, lh - 2), badgeC);
        pm->DrawText(cPoint(bx + 5, y), badge, kText, clrTransparent, f);
    }

    // ── Audio ─────────────────────────────────────────────────────────────────
    {
        char buf[128];
        if (m_streamInfo.audioSampleRate > 0)
            snprintf(buf, sizeof(buf), "%s  %d Hz  %dch",
                m_streamInfo.audioCodec.c_str(),
                m_streamInfo.audioSampleRate,
                m_streamInfo.audioChannels);
        else
            snprintf(buf, sizeof(buf), "%s",
                m_streamInfo.audioCodec.empty()
                    ? "Audio: \xe2\x80\x93" : m_streamInfo.audioCodec.c_str());

        pm->DrawText(cPoint(c2 + 8, y), buf, kSub, clrTransparent, f);
    }
    y += lh + 4;

    // ── A/V offset + Volume ───────────────────────────────────────────────────
    char sync[32], vol[32];
    snprintf(sync, sizeof(sync), "A/V: %+d ms", GstConfig.AudioOffset);
    snprintf(vol,  sizeof(vol),  "Vol: %d%%",   GstConfig.Volume * 100 / 255);
    pm->DrawText(cPoint(M + 8, y), sync, kSub, clrTransparent, f);
    pm->DrawText(cPoint(c2 + 8, y), vol,  kSub, clrTransparent, f);
    y += lh + 4;
}

void cGstOsd::DrawPipelineState(cPixmap *pm, int w, int &y, const cFont *f)
{
    if (m_streamInfo.pipelineState.empty()) return;
    const int M = 20;

    std::string state = "Pipeline: " + m_streamInfo.pipelineState;
    if (m_streamInfo.pipelineState == "Buffering")
        state += " " + std::to_string(m_streamInfo.bufferingPercent) + "%";

    const tColor col =
        (m_streamInfo.pipelineState == "Playing") ? kHwBadge : kAccent;
    pm->DrawText(cPoint(w - M - f->Width(state.c_str()), y),
        state.c_str(), col, clrTransparent, f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Auto-hide timer (cThread::Action)
// ─────────────────────────────────────────────────────────────────────────────

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
