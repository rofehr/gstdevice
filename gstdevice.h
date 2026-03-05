#pragma once

/*
 * gstdevice.h  –  GStreamer output device
 *
 * Implements cDevice for VDR 2.6 / 2.7.
 *
 * VDR API notes (2.6+):
 *   • SetChannelDevice() returns bool (not void)
 *   • PlayTs() does NOT exist in cDevice; VDR calls PlayTsVideo / PlayTsAudio
 *   • LOCK_SCHEDULES_READ macro replaces cSchedulesLock (used in gstosd.cpp)
 *
 * Naming note:
 *   The GStreamer library defines a C type called GstDevice (gst/gstdevice.h,
 *   pulled in via <gst/gst.h>).  We therefore name our singleton pointer
 *   GstOutputDevice to avoid any collision.
 */

#include "gstreamer.h"  // VDR + GStreamer system headers
#include "config.h"     // cGstConfig, sGstStreamInfo (no further deps)
#include "tsparser.h"   // cTsParser

// Include gstosd.h for cGstOsd and GstOsd when needed.
// Forward declaration only (avoids circular include):
class cGstOsd;

// ─────────────────────────────────────────────────────────────────────────────
class cGstDevice : public cDevice
{
public:
    cGstDevice();
    virtual ~cGstDevice() override;

    // ── cDevice interface ────────────────────────────────────────────────────
    virtual bool    HasDecoder()  const override { return true; }
    virtual bool    CanReplay()   const override { return true; }

    virtual bool    SetPlayMode(ePlayMode PlayMode) override;
    virtual void    TrickSpeed(int Speed, bool Forward) override;
    virtual void    Clear() override;
    virtual void    Play() override;
    virtual void    Freeze() override;
    virtual void    Mute() override;
    virtual void    SetVolumeDevice(int Volume) override;

    // Legacy PES path (some replay / recording modes)
    virtual int     PlayVideo(const uchar *Data, int Length) override;
    virtual int     PlayAudio(const uchar *Data, int Length, uchar Id) override;

    // TS packet delivery (VDR 2.2+).
    // VDR pre-filters by PID and calls one of these two methods.
    // PlayTs() was never a stable virtual in cDevice – do not override it.
    virtual int     PlayTsVideo(const uchar *Data, int Length) override;
    virtual int     PlayTsAudio(const uchar *Data, int Length) override;

    virtual bool    Poll(cPoller &Poller, int TimeoutMs = 0) override;
    virtual bool    Flush(int TimeoutMs = 0) override;
    virtual int64_t GetSTC() override;

    // Returns bool in VDR 2.6+ (was void in older VDR)
    virtual bool    SetChannelDevice(const cChannel *Channel, bool LiveView) override;

    // ── OSD / remote key handling ────────────────────────────────────────────
    eOSState        ProcessKey(eKeys Key);

    // ── Pipeline lifecycle (called by cPluginGstreamer) ──────────────────────
    bool InitPipeline();
    void DestroyPipeline();
    void ReconfigurePipeline();
    bool IsRunning() const { return m_running.load(); }

private:
    // ── GStreamer elements ───────────────────────────────────────────────────
    GstElement *m_pipeline    = nullptr;

    // Video branch:  appsrc → parse → decode → [videoconvert] → sink
    GstElement *m_videoSrc    = nullptr;
    GstElement *m_videoParse  = nullptr;
    GstElement *m_videoDec    = nullptr;
    GstElement *m_videoConv   = nullptr;  // SW path only
    GstElement *m_videoSink   = nullptr;

    // Audio branch:  appsrc → parse → decode → convert → resample → identity → sink
    GstElement *m_audioSrc    = nullptr;
    GstElement *m_audioParse  = nullptr;
    GstElement *m_audioDec    = nullptr;
    GstElement *m_audioConv   = nullptr;
    GstElement *m_audioResamp = nullptr;
    GstElement *m_audioSync   = nullptr;  // identity element carrying ts-offset
    GstElement *m_audioSink   = nullptr;

    GstBus     *m_bus         = nullptr;

    // ── Playback state ───────────────────────────────────────────────────────
    ePlayMode         m_playMode = pmNone;
    std::atomic<bool> m_running{false};
    std::mutex        m_mutex;

    // Snapshot of active config – used to detect when rebuild is needed
    int  m_activeVideoCodec  = -1;
    bool m_activeHwDecode    = false;
    int  m_activeAudioCodec  = -1;
    int  m_activeAudioOffset = 0;

    // ── TS demux state ───────────────────────────────────────────────────────
    int  m_videoPid = -1;
    int  m_audioPid = -1;
    std::unique_ptr<cTsParser> m_videoParser;
    std::unique_ptr<cTsParser> m_audioParser;

    // PTS normalisation – handles 33-bit wrap-around (~26.5 h) + discontinuities
    GstClockTime m_videoBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_audioBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastVideoPts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastAudioPts = GST_CLOCK_TIME_NONE;

    // Jump larger than this → wrap or discontinuity
    static constexpr GstClockTime kPtsDiscThreshold = 500 * GST_MSECOND;

    // ── Private helpers ──────────────────────────────────────────────────────
    bool BuildPipeline();
    void TeardownPipeline();
    bool CreateVideoElements();
    bool CreateAudioElements();

    void InitTsParsers();
    void ResetTsState();

    void ApplyAudioOffset(int ms);
    void ApplyVolume(int vol);

    // Push demuxed ES bytes into an appsrc with a normalised PTS stamp
    int PushEs(GstElement *src, const uint8_t *data, int len, GstClockTime pts);

    // Legacy push (no PTS knowledge)
    int PushBuffer(GstElement *src, const uchar *data, int len);

    // Convert raw 90 kHz-derived ns PTS to a monotonic pipeline clock value
    GstClockTime NormalisePts(GstClockTime rawNs,
                              GstClockTime &basePts,
                              GstClockTime &lastPts);

    // Read GStreamer caps from sink pads → populate GstStreamInfo → notify OSD
    void QueryAndUpdateStreamInfo();

    // GStreamer bus
    static GstBusSyncReply BusSyncHandler(GstBus *, GstMessage *, gpointer);
    void HandleBusMessage(GstMessage *msg);
};

// Singleton pointer – defined in gstreamer.cpp
// Named GstOutputDevice to avoid collision with GStreamer's own GstDevice type.
extern cGstDevice *GstOutputDevice;
