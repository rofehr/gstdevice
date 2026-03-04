#pragma once

#include "gstreamer.h"
#include "config.h"
#include "gstosd.h"
#include "tsparser.h"

#include <memory>

// ============================================================
//  cGstDevice – VDR output device backed by a GStreamer pipeline
//  Supports: H.264, H.265/HEVC video | AAC, MP3 audio
//            VA-API hardware decode  | configurable A/V sync offset
// ============================================================
class cGstDevice : public cDevice
{
public:
    cGstDevice();
    virtual ~cGstDevice();

    // --- cDevice interface ---
    virtual bool    HasDecoder()       const override { return true; }
    virtual bool    CanReplay()        const override { return true; }
    virtual bool    SetPlayMode(ePlayMode PlayMode) override;
    virtual void    TrickSpeed(int Speed, bool Forward) override;
    virtual void    Clear() override;
    virtual void    Play() override;
    virtual void    Freeze() override;
    virtual void    Mute() override;
    virtual void    SetVolumeDevice(int Volume) override;
    virtual int     PlayVideo(const uchar *Data, int Length) override;
    virtual int     PlayAudio(const uchar *Data, int Length, uchar Id) override;
    virtual int     PlayTsVideo(const uchar *Data, int Length) override;
    virtual int     PlayTsAudio(const uchar *Data, int Length) override;

    // ---- Full TS packet path (called by VDR for live TV and recordings) ----
    // VDR delivers one or more complete 188-byte TS packets per call.
    // We demux here and push the resulting ES + PTS into appsrc.
    virtual int     PlayTs(const uchar *Data, int Length) override;

    virtual bool    Poll(cPoller &Poller, int TimeoutMs = 0) override;
    virtual bool    Flush(int TimeoutMs = 0) override;
    virtual int64_t GetSTC() override;

    // Pipeline control
    bool InitPipeline();
    void DestroyPipeline();
    void ReconfigurePipeline();   // called after OSD config change
    bool IsRunning() const { return m_running.load(); }

    // --- OSD integration ---
    // Called by VDR when the user switches channels
    virtual void SetChannelDevice(const cChannel *Channel, bool LiveView) override;
    // Called by VDR to deliver remote-control keys to the device
    virtual bool HasIBPFrames() const override { return false; }
    eOSState ProcessKey(eKeys Key);

private:
    // ---- GStreamer elements ----
    GstElement  *m_pipeline    = nullptr;

    // Video branch
    GstElement  *m_videoSrc    = nullptr;   // appsrc
    GstElement  *m_videoParse  = nullptr;   // h264parse / h265parse
    GstElement  *m_videoDec    = nullptr;   // vaapih264dec / avdec_h264 etc.
    GstElement  *m_videoConv   = nullptr;   // videoconvert (SW path)
    GstElement  *m_videoSink   = nullptr;   // vaapisink / autovideosink

    // Audio branch
    GstElement  *m_audioSrc    = nullptr;   // appsrc
    GstElement  *m_audioParse  = nullptr;   // aacparse / mpegaudioparse
    GstElement  *m_audioDec    = nullptr;   // avdec_aac / avdec_mp3
    GstElement  *m_audioConv   = nullptr;   // audioconvert
    GstElement  *m_audioResamp = nullptr;   // audioresample
    GstElement  *m_audioSync   = nullptr;   // identity (tsoffset for A/V sync)
    GstElement  *m_audioSink   = nullptr;   // autoaudiosink / alsasink

    GstBus      *m_bus         = nullptr;

    // ---- State ----
    ePlayMode    m_playMode    = pmNone;
    std::atomic<bool> m_running{false};
    std::mutex   m_mutex;

    // Config snapshot active in current pipeline (detect changes)
    int          m_activeVideoCodec  = -1;
    bool         m_activeHwDecode    = false;
    int          m_activeAudioCodec  = -1;
    int          m_activeAudioOffset = 0;

    // ---- Internal helpers ----
    bool BuildPipeline();
    void TeardownPipeline();
    bool CreateVideoElements();
    bool CreateAudioElements();
    void ApplyAudioOffset(int offsetMs);
    void ApplyVolume(int vol);

    // Push a raw ES buffer (already demuxed) with PTS into appsrc
    int  PushEs(GstElement *src, const uint8_t *data, int len, GstClockTime pts);

    // Legacy ES path (PlayVideo / PlayAudio) – no PTS available
    int  PushBuffer(GstElement *src, const uchar *data, int len);

    static GstBusSyncReply BusSyncHandler(GstBus *bus, GstMessage *msg, gpointer data);
    void HandleBusMessage(GstMessage *msg);

    // Queries GStreamer pipeline for current stream properties
    // and pushes them to the OSD
    void QueryAndUpdateStreamInfo();

    // ---- TS demux state ----
    // Tracked PIDs (set by VDR via SetPid / detected from first TS packet)
    int  m_videoPid = -1;
    int  m_audioPid = -1;

    // One parser per elementary stream
    std::unique_ptr<cTsParser> m_videoParser;
    std::unique_ptr<cTsParser> m_audioParser;

    // Running PTS wrap-around counter (33-bit PTS rolls over ~26.5 h)
    GstClockTime m_videoBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_audioBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastVideoPts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastAudioPts = GST_CLOCK_TIME_NONE;

    // PTS discontinuity threshold: 500 ms jump → treat as wrap / seek
    static constexpr GstClockTime kPtsDiscontinuityThreshold = 500 * GST_MSECOND;

    // Resolve PTS wrap-around and discontinuities, returns monotonic ns timestamp
    GstClockTime NormalisePts(GstClockTime raw_ns, GstClockTime &basePts, GstClockTime &lastPts);
};
