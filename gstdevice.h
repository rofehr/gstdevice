#pragma once

#include "gstreamer.h"
#include "config.h"
#include "tsparser.h"

// forward declaration
class cGstOsd;
extern cGstOsd *GstOsd;

// ---- Live stream info (filled by QueryAndUpdateStreamInfo) ----
struct sGstStreamInfo {
    std::string videoCodec;
    int         videoWidth        = 0;
    int         videoHeight       = 0;
    double      videoFps          = 0.0;
    bool        hwDecode          = false;
    std::string audioCodec;
    int         audioSampleRate   = 0;
    int         audioChannels     = 0;
    std::string pipelineState;       // "Playing" | "Paused" | "Buffering"
    int         bufferingPercent  = 0;
};
extern sGstStreamInfo GstStreamInfo;

// ============================================================
//  cGstDevice
// ============================================================
class cGstDevice : public cDevice
{
public:
    cGstDevice();
    virtual ~cGstDevice() override;

    // ---- cDevice interface ----
    virtual bool    HasDecoder()  const override { return true; }
    virtual bool    CanReplay()   const override { return true; }

    virtual bool    SetPlayMode(ePlayMode PlayMode) override;
    virtual void    TrickSpeed(int Speed, bool Forward) override;
    virtual void    Clear() override;
    virtual void    Play() override;
    virtual void    Freeze() override;
    virtual void    Mute() override;
    virtual void    SetVolumeDevice(int Volume) override;

    // Legacy ES data path (used by some replay modes)
    virtual int     PlayVideo(const uchar *Data, int Length) override;
    virtual int     PlayAudio(const uchar *Data, int Length, uchar Id) override;

    // TS packet paths – VDR 2.2+ primary delivery mechanism
    // PlayTs   : mixed TS buffer (PIDs auto-detected)
    // PlayTsVideo / PlayTsAudio: pre-filtered single-PID packets from VDR
    virtual int     PlayTs(const uchar *Data, int Length) override;
    virtual int     PlayTsVideo(const uchar *Data, int Length) override;
    virtual int     PlayTsAudio(const uchar *Data, int Length) override;

    virtual bool    Poll(cPoller &Poller, int TimeoutMs = 0) override;
    virtual bool    Flush(int TimeoutMs = 0) override;
    virtual int64_t GetSTC() override;

    // ---- OSD integration ----
    // Called by VDR on every channel switch
    virtual void    SetChannelDevice(const cChannel *Channel, bool LiveView) override;

    // Called by the plugin's key handler
    eOSState        ProcessKey(eKeys Key);

    // ---- Pipeline control (called from cPluginGstreamer) ----
    bool InitPipeline();
    void DestroyPipeline();
    void ReconfigurePipeline();
    bool IsRunning() const { return m_running.load(); }

private:
    // ---- GStreamer elements ----
    GstElement *m_pipeline    = nullptr;

    // Video branch: appsrc → parse → decode → [convert] → sink
    GstElement *m_videoSrc    = nullptr;
    GstElement *m_videoParse  = nullptr;
    GstElement *m_videoDec    = nullptr;
    GstElement *m_videoConv   = nullptr;   // only in SW path
    GstElement *m_videoSink   = nullptr;

    // Audio branch: appsrc → parse → decode → convert → resample → identity → sink
    GstElement *m_audioSrc    = nullptr;
    GstElement *m_audioParse  = nullptr;
    GstElement *m_audioDec    = nullptr;
    GstElement *m_audioConv   = nullptr;
    GstElement *m_audioResamp = nullptr;
    GstElement *m_audioSync   = nullptr;   // identity – carries ts-offset
    GstElement *m_audioSink   = nullptr;

    GstBus     *m_bus         = nullptr;

    // ---- Playback state ----
    ePlayMode   m_playMode = pmNone;
    std::atomic<bool> m_running{false};
    std::mutex  m_mutex;

    // Active pipeline config snapshot (for change detection)
    int  m_activeVideoCodec  = -1;
    bool m_activeHwDecode    = false;
    int  m_activeAudioCodec  = -1;
    int  m_activeAudioOffset = 0;

    // ---- TS demux state ----
    int  m_videoPid = -1;
    int  m_audioPid = -1;
    std::unique_ptr<cTsParser> m_videoParser;
    std::unique_ptr<cTsParser> m_audioParser;

    // PTS normalisation state (handles 33-bit wrap-around and discontinuities)
    GstClockTime m_videoBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_audioBasePts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastVideoPts = GST_CLOCK_TIME_NONE;
    GstClockTime m_lastAudioPts = GST_CLOCK_TIME_NONE;

    static constexpr GstClockTime kPtsDiscThreshold = 500 * GST_MSECOND;

    // ---- Private helpers ----
    bool BuildPipeline();
    void TeardownPipeline();
    bool CreateVideoElements();
    bool CreateAudioElements();
    void InitTsParsers();
    void ResetTsState();

    void ApplyAudioOffset(int ms);
    void ApplyVolume(int vol);

    // Push a demuxed ES buffer into appsrc, stamping it with pts
    int  PushEs(GstElement *src, const uint8_t *data, int len, GstClockTime pts);

    // Legacy path: push without PTS
    int  PushBuffer(GstElement *src, const uchar *data, int len);

    // Normalise 33-bit PTS (ns) to monotonic pipeline timestamps
    GstClockTime NormalisePts(GstClockTime rawNs,
                              GstClockTime &basePts,
                              GstClockTime &lastPts);

    // Query GStreamer caps → fill GstStreamInfo, push to OSD
    void QueryAndUpdateStreamInfo();

    static GstBusSyncReply BusSyncHandler(GstBus *, GstMessage *, gpointer);
    void HandleBusMessage(GstMessage *msg);
};
