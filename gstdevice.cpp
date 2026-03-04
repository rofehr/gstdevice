#include "gstdevice.h"
#include "gstosd.h"
#include <vdr/tools.h>
#include <cstring>
#include <cmath>

// ============================================================
//  Constructor / Destructor
// ============================================================
cGstDevice::cGstDevice()
{
    dsyslog("[gstreamer] cGstDevice created");
}

cGstDevice::~cGstDevice()
{
    DestroyPipeline();
    dsyslog("[gstreamer] cGstDevice destroyed");
}

// ============================================================
//  Public pipeline control
// ============================================================
bool cGstDevice::InitPipeline()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running)
        return true;
    return BuildPipeline();
}

void cGstDevice::DestroyPipeline()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    TeardownPipeline();
}

void cGstDevice::ReconfigurePipeline()
{
    bool rebuildNeeded =
        (m_activeVideoCodec != GstConfig.VideoCodec)    ||
        (m_activeHwDecode   != GstConfig.HardwareDecode) ||
        (m_activeAudioCodec != GstConfig.AudioCodec);

    if (rebuildNeeded) {
        isyslog("[gstreamer] Config changed – rebuilding pipeline");
        std::lock_guard<std::mutex> lock(m_mutex);
        TeardownPipeline();
        BuildPipeline();
    } else if (m_activeAudioOffset != GstConfig.AudioOffset) {
        ApplyAudioOffset(GstConfig.AudioOffset);
        m_activeAudioOffset = GstConfig.AudioOffset;
    }

    ApplyVolume(GstConfig.Volume);
}

// ============================================================
//  Pipeline build
// ============================================================
bool cGstDevice::BuildPipeline()
{
    m_pipeline = gst_pipeline_new("vdr-gst");
    if (!m_pipeline) {
        esyslog("[gstreamer] gst_pipeline_new failed");
        return false;
    }

    if (!CreateVideoElements() || !CreateAudioElements()) {
        TeardownPipeline();
        return false;
    }

    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    gst_bus_set_sync_handler(m_bus, BusSyncHandler, this, nullptr);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        esyslog("[gstreamer] Pipeline PLAYING transition failed");
        TeardownPipeline();
        return false;
    }

    // Snapshot active config
    m_activeVideoCodec  = GstConfig.VideoCodec;
    m_activeHwDecode    = GstConfig.HardwareDecode;
    m_activeAudioCodec  = GstConfig.AudioCodec;
    m_activeAudioOffset = GstConfig.AudioOffset;

    ApplyAudioOffset(GstConfig.AudioOffset);
    ApplyVolume(GstConfig.Volume);

    // Wire up TS parsers now that appsrc elements exist
    ResetTsState();
    InitTsParsers();

    m_running = true;
    isyslog("[gstreamer] Pipeline running – video=%s(%s) audio=%s hw=%d offset=%dms",
        GstConfig.VideoCodecName(), GstConfig.VideoDecoderName(),
        GstConfig.AudioCodecName(), (int)GstConfig.HardwareDecode,
        GstConfig.AudioOffset);
    return true;
}

// ------------------------------------------------------------
//  Video branch
//  HW: appsrc → h264/h265parse → vaapih264/h265dec → vaapisink
//  SW: appsrc → h264/h265parse → avdec_h264/h265   → videoconvert → autovideosink
// ------------------------------------------------------------
bool cGstDevice::CreateVideoElements()
{
    const bool hw = GstConfig.HardwareDecode;

    m_videoSrc   = gst_element_factory_make("appsrc",                     "video-src");
    m_videoParse = gst_element_factory_make(GstConfig.VideoParseName(),    "video-parse");
    m_videoDec   = gst_element_factory_make(GstConfig.VideoDecoderName(),  "video-dec");
    if (!hw)
        m_videoConv = gst_element_factory_make("videoconvert",             "video-conv");

    const std::string vSink = GstConfig.EffectiveVideoSink();
    m_videoSink = gst_element_factory_make(vSink.c_str(),                  "video-sink");

    if (!m_videoSrc || !m_videoParse || !m_videoDec || !m_videoSink || (!hw && !m_videoConv)) {
        esyslog("[gstreamer] Failed to create video elements (parse=%s dec=%s sink=%s hw=%d)",
            GstConfig.VideoParseName(), GstConfig.VideoDecoderName(), vSink.c_str(), (int)hw);
        return false;
    }

    g_object_set(G_OBJECT(m_videoSrc),
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        "max-bytes",   (guint64)(2 * 1024 * 1024),
        nullptr);
    g_object_set(G_OBJECT(m_videoSink), "sync", TRUE, nullptr);

    if (hw) {
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_videoSrc, m_videoParse, m_videoDec, m_videoSink, nullptr);
        if (!gst_element_link_many(m_videoSrc, m_videoParse, m_videoDec, m_videoSink, nullptr)) {
            esyslog("[gstreamer] Failed to link HW video branch"); return false;
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_videoSrc, m_videoParse, m_videoDec, m_videoConv, m_videoSink, nullptr);
        if (!gst_element_link_many(m_videoSrc, m_videoParse, m_videoDec, m_videoConv, m_videoSink, nullptr)) {
            esyslog("[gstreamer] Failed to link SW video branch"); return false;
        }
    }
    return true;
}

// ------------------------------------------------------------
//  Audio branch
//  appsrc → aacparse/mpegaudioparse → avdec_aac/mp3
//         → audioconvert → audioresample → identity(ts-offset) → sink
// ------------------------------------------------------------
bool cGstDevice::CreateAudioElements()
{
    m_audioSrc    = gst_element_factory_make("appsrc",                     "audio-src");
    m_audioParse  = gst_element_factory_make(GstConfig.AudioParseName(),   "audio-parse");
    m_audioDec    = gst_element_factory_make(GstConfig.AudioDecoderName(), "audio-dec");
    m_audioConv   = gst_element_factory_make("audioconvert",               "audio-conv");
    m_audioResamp = gst_element_factory_make("audioresample",              "audio-resamp");
    m_audioSync   = gst_element_factory_make("identity",                   "audio-sync");
    m_audioSink   = gst_element_factory_make(GstConfig.AudioSink.c_str(),  "audio-sink");

    if (!m_audioSrc || !m_audioParse || !m_audioDec ||
        !m_audioConv || !m_audioResamp || !m_audioSync || !m_audioSink) {
        esyslog("[gstreamer] Failed to create audio elements (parse=%s dec=%s sink=%s)",
            GstConfig.AudioParseName(), GstConfig.AudioDecoderName(),
            GstConfig.AudioSink.c_str());
        return false;
    }

    g_object_set(G_OBJECT(m_audioSrc),
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        "max-bytes",   (guint64)(512 * 1024),
        nullptr);
    g_object_set(G_OBJECT(m_audioSink), "sync", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline),
        m_audioSrc, m_audioParse, m_audioDec,
        m_audioConv, m_audioResamp, m_audioSync, m_audioSink, nullptr);

    if (!gst_element_link_many(m_audioSrc, m_audioParse, m_audioDec,
                               m_audioConv, m_audioResamp, m_audioSync, m_audioSink, nullptr)) {
        esyslog("[gstreamer] Failed to link audio branch");
        return false;
    }
    return true;
}

// ============================================================
//  TS parser setup
// ============================================================
void cGstDevice::InitTsParsers()
{
    m_videoParser = std::make_unique<cTsParser>(
        [this](const uint8_t *data, int len, uint64_t pts_ns) {
            GstClockTime ts = (pts_ns != GST_CLOCK_TIME_NONE)
                ? NormalisePts(pts_ns, m_videoBasePts, m_lastVideoPts)
                : GST_CLOCK_TIME_NONE;
            PushEs(m_videoSrc, data, len, ts);
        });

    m_audioParser = std::make_unique<cTsParser>(
        [this](const uint8_t *data, int len, uint64_t pts_ns) {
            GstClockTime ts = (pts_ns != GST_CLOCK_TIME_NONE)
                ? NormalisePts(pts_ns, m_audioBasePts, m_lastAudioPts)
                : GST_CLOCK_TIME_NONE;
            PushEs(m_audioSrc, data, len, ts);
        });
}

void cGstDevice::ResetTsState()
{
    m_videoPid = m_audioPid = -1;
    if (m_videoParser) m_videoParser->Reset();
    if (m_audioParser) m_audioParser->Reset();
    m_videoBasePts = m_audioBasePts = GST_CLOCK_TIME_NONE;
    m_lastVideoPts = m_lastAudioPts = GST_CLOCK_TIME_NONE;
}

// ============================================================
//  Pipeline teardown
// ============================================================
void cGstDevice::TeardownPipeline()
{
    m_running = false;

    // Flush parsers before stopping pipeline
    if (m_videoParser) { m_videoParser->Flush(); m_videoParser.reset(); }
    if (m_audioParser) { m_audioParser->Flush(); m_audioParser.reset(); }
    m_videoPid = m_audioPid = -1;
    m_videoBasePts = m_audioBasePts = GST_CLOCK_TIME_NONE;
    m_lastVideoPts = m_lastAudioPts = GST_CLOCK_TIME_NONE;

    if (m_pipeline)
        gst_element_set_state(m_pipeline, GST_STATE_NULL);

    if (m_bus) {
        gst_bus_set_sync_handler(m_bus, nullptr, nullptr, nullptr);
        gst_object_unref(m_bus);
        m_bus = nullptr;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline   = nullptr;
        // All child elements are owned by the bin; freed automatically
        m_videoSrc   = m_videoParse = m_videoDec = m_videoConv = m_videoSink = nullptr;
        m_audioSrc   = m_audioParse = m_audioDec = nullptr;
        m_audioConv  = m_audioResamp = m_audioSync = m_audioSink = nullptr;
    }
    dsyslog("[gstreamer] Pipeline torn down");
}

// ============================================================
//  GStreamer bus handler
// ============================================================
GstBusSyncReply cGstDevice::BusSyncHandler(GstBus *, GstMessage *msg, gpointer data)
{
    static_cast<cGstDevice *>(data)->HandleBusMessage(msg);
    return GST_BUS_PASS;
}

void cGstDevice::HandleBusMessage(GstMessage *msg)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        esyslog("[gstreamer] ERROR <%s>: %s – %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
            err ? err->message : "?", dbg ? dbg : "");
        g_clear_error(&err); g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        dsyslog("[gstreamer] WARNING <%s>: %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "?");
        g_clear_error(&err); g_free(dbg);
        break;
    }
    case GST_MESSAGE_EOS:
        dsyslog("[gstreamer] EOS");
        break;
    case GST_MESSAGE_BUFFERING: {
        gint pct = 0;
        gst_message_parse_buffering(msg, &pct);
        dsyslog("[gstreamer] Buffering %d%%", pct);
        if (GstOsd && GstOsd->IsVisible()) {
            sGstStreamInfo info = GstStreamInfo;
            info.pipelineState    = "Buffering";
            info.bufferingPercent = pct;
            GstOsd->UpdateStreamInfo(info);
        }
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(m_pipeline)) {
            GstState o, n, p;
            gst_message_parse_state_changed(msg, &o, &n, &p);
            dsyslog("[gstreamer] State %s → %s",
                gst_element_state_get_name(o), gst_element_state_get_name(n));
        }
        break;
    default: break;
    }
}

// ============================================================
//  cDevice: SetPlayMode
// ============================================================
bool cGstDevice::SetPlayMode(ePlayMode PlayMode)
{
    m_playMode = PlayMode;
    switch (PlayMode) {
    case pmNone:
        if (m_pipeline)
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
        break;
    case pmAudioVideo:
    case pmAudioOnly:
    case pmVideoOnly:
    case pmExtern_THIS_SHOULD_BE_AVOIDED:
        if (!m_running) {
            std::lock_guard<std::mutex> lock(m_mutex);
            BuildPipeline();
        }
        if (m_pipeline)
            gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        break;
    default: break;
    }
    return true;
}

void cGstDevice::TrickSpeed(int Speed, bool Forward)
{
    if (!m_pipeline) return;
    gdouble rate = static_cast<gdouble>(Speed) / 100.0;
    if (!Forward) rate = -rate;
    gint64 pos = 0;
    gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos);
    GstEvent *seek = gst_event_new_seek(rate, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, pos, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    gst_element_send_event(m_pipeline, seek);
    dsyslog("[gstreamer] TrickSpeed rate=%.2f", rate);
}

void cGstDevice::Clear()
{
    if (m_videoParser) m_videoParser->Flush();
    if (m_audioParser) m_audioParser->Flush();
    m_videoBasePts = m_audioBasePts = GST_CLOCK_TIME_NONE;
    m_lastVideoPts = m_lastAudioPts = GST_CLOCK_TIME_NONE;

    if (m_videoSrc) gst_app_src_end_of_stream(GST_APP_SRC(m_videoSrc));
    if (m_audioSrc) gst_app_src_end_of_stream(GST_APP_SRC(m_audioSrc));

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_READY);
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }
}

void cGstDevice::Play()   { if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PLAYING); }
void cGstDevice::Freeze() { if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PAUSED);  }
void cGstDevice::Mute()   { if (m_audioSink) g_object_set(G_OBJECT(m_audioSink), "volume", 0.0, nullptr); }

void cGstDevice::SetVolumeDevice(int Volume) { ApplyVolume(Volume); }

void cGstDevice::ApplyAudioOffset(int ms)
{
    if (!m_audioSync) return;
    gint64 ns = static_cast<gint64>(ms) * GST_MSECOND;
    g_object_set(G_OBJECT(m_audioSync), "ts-offset", ns, nullptr);
    dsyslog("[gstreamer] Audio offset %d ms", ms);
}

void cGstDevice::ApplyVolume(int vol)
{
    if (!m_audioSink) return;
    g_object_set(G_OBJECT(m_audioSink), "volume",
        static_cast<gdouble>(vol) / 255.0, nullptr);
}

// ============================================================
//  PTS normalisation (33-bit wrap-around + discontinuity)
// ============================================================
GstClockTime cGstDevice::NormalisePts(GstClockTime rawNs,
                                       GstClockTime &basePts,
                                       GstClockTime &lastPts)
{
    if (basePts == GST_CLOCK_TIME_NONE) {
        basePts = lastPts = rawNs;
        return 0;
    }

    // 33-bit PTS in nanoseconds ≈ 2^33 * 100000/9 ns ≈ 26.5 hours
    static constexpr GstClockTime kWrap33 = (GstClockTime)((1ULL << 33) * 100000ULL / 9ULL);

    // Backward jump larger than threshold → wrap-around or discontinuity
    if (lastPts != GST_CLOCK_TIME_NONE &&
        rawNs + kPtsDiscThreshold < lastPts)
    {
        if (lastPts > kWrap33 / 2 && rawNs < kWrap33 / 2)
            basePts -= kWrap33;   // 33-bit wrap
        else
            basePts = rawNs;      // genuine discontinuity – reset base
    }

    lastPts = rawNs;
    return (rawNs >= basePts) ? (rawNs - basePts) : 0;
}

// ============================================================
//  PushEs – copy ES data into a GstBuffer and push to appsrc
// ============================================================
int cGstDevice::PushEs(GstElement *src, const uint8_t *data, int len, GstClockTime pts)
{
    if (!src || !data || len <= 0) return 0;

    GstBuffer *buf = gst_buffer_new_allocate(nullptr, static_cast<gsize>(len), nullptr);
    if (!buf) return -1;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf); return -1;
    }
    std::memcpy(map.data, data, len);
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf)      = pts;
    GST_BUFFER_DTS(buf)      = pts;
    GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    if (ret != GST_FLOW_OK) {
        dsyslog("[gstreamer] push_buffer error %d (len=%d)", (int)ret, len);
        return -1;
    }
    return len;
}

int cGstDevice::PushBuffer(GstElement *src, const uchar *data, int len)
{
    return PushEs(src, data, len, GST_CLOCK_TIME_NONE);
}

// ============================================================
//  PlayTsVideo – VDR pre-filtered single-PID video packets
// ============================================================
int cGstDevice::PlayTsVideo(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    if (!m_videoParser) InitTsParsers();

    if (m_videoPid == -1)
        m_videoPid = ((Data[1] & 0x1F) << 8) | Data[2];

    if (m_videoParser)
        m_videoParser->Feed(Data, kTsPacketSize);

    return Length;
}

// ============================================================
//  PlayTsAudio – VDR pre-filtered single-PID audio packets
// ============================================================
int cGstDevice::PlayTsAudio(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    if (!m_audioParser) InitTsParsers();

    if (m_audioPid == -1)
        m_audioPid = ((Data[1] & 0x1F) << 8) | Data[2];

    if (m_audioParser)
        m_audioParser->Feed(Data, kTsPacketSize);

    return Length;
}

// ============================================================
//  Legacy ES path
// ============================================================
int cGstDevice::PlayVideo(const uchar *D, int L)         { return PushBuffer(m_videoSrc, D, L); }
int cGstDevice::PlayAudio(const uchar *D, int L, uchar)  { return PushBuffer(m_audioSrc, D, L); }

// ============================================================
//  Poll / Flush / GetSTC
// ============================================================
bool cGstDevice::Poll(cPoller & /*P*/, int /*T*/)
{
    if (!m_videoSrc) return true;
    guint64 q = 0;
    g_object_get(G_OBJECT(m_videoSrc), "current-level-bytes", &q, nullptr);
    return q < static_cast<guint64>(1 * 1024 * 1024);
}

bool cGstDevice::Flush(int /*T*/) { return true; }

int64_t cGstDevice::GetSTC()
{
    if (!m_pipeline) return -1;
    gint64 pos = 0;
    if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos))
        return static_cast<int64_t>(pos / 90);   // ns → 90 kHz
    return -1;
}

// ============================================================
//  OSD: channel switch
// ============================================================
void cGstDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
    cDevice::SetChannelDevice(Channel, LiveView);

    if (LiveView && Channel) {
        ResetTsState();
        dsyslog("[gstreamer] Channel switch → TS state reset");

        if (GstOsd) {
            cCondWait::SleepMs(300);
            QueryAndUpdateStreamInfo();
            GstOsd->ShowForChannel(Channel);
        }
    }
}

// ============================================================
//  OSD: key handler (OK / Info → toggle info banner)
// ============================================================
eOSState cGstDevice::ProcessKey(eKeys Key)
{
    if (!GstOsd) return osUnknown;
    switch (Key) {
    case kOk:
    case kInfo:
        QueryAndUpdateStreamInfo();
        GstOsd->Toggle();
        return osConsumed;
    default:
        return osUnknown;
    }
}

// ============================================================
//  Query GStreamer caps → fill GstStreamInfo, push to OSD
// ============================================================
void cGstDevice::QueryAndUpdateStreamInfo()
{
    sGstStreamInfo info;
    info.videoCodec = GstConfig.VideoCodecName();
    info.audioCodec = GstConfig.AudioCodecName();
    info.hwDecode   = GstConfig.HardwareDecode;

    if (m_pipeline) {
        GstState state = GST_STATE_NULL;
        gst_element_get_state(m_pipeline, &state, nullptr, 0);
        switch (state) {
        case GST_STATE_PLAYING: info.pipelineState = "Playing"; break;
        case GST_STATE_PAUSED:  info.pipelineState = "Paused";  break;
        case GST_STATE_READY:   info.pipelineState = "Ready";   break;
        default:                info.pipelineState = "Idle";    break;
        }
    }

    // Video caps from videoSink sink pad
    if (m_videoSink) {
        GstPad *pad = gst_element_get_static_pad(m_videoSink, "sink");
        if (pad) {
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (caps) {
                const GstStructure *s = gst_caps_get_structure(caps, 0);
                if (s) {
                    gst_structure_get_int(s, "width",  &info.videoWidth);
                    gst_structure_get_int(s, "height", &info.videoHeight);
                    const GValue *fps = gst_structure_get_value(s, "framerate");
                    if (fps && GST_VALUE_HOLDS_FRACTION(fps)) {
                        int num = gst_value_get_fraction_numerator(fps);
                        int den = gst_value_get_fraction_denominator(fps);
                        if (den > 0) info.videoFps = (double)num / den;
                    }
                }
                gst_caps_unref(caps);
            }
            gst_object_unref(pad);
        }
    }

    // Audio caps from audioSink sink pad
    if (m_audioSink) {
        GstPad *pad = gst_element_get_static_pad(m_audioSink, "sink");
        if (pad) {
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (caps) {
                const GstStructure *s = gst_caps_get_structure(caps, 0);
                if (s) {
                    gst_structure_get_int(s, "rate",     &info.audioSampleRate);
                    gst_structure_get_int(s, "channels", &info.audioChannels);
                }
                gst_caps_unref(caps);
            }
            gst_object_unref(pad);
        }
    }

    GstStreamInfo = info;
    if (GstOsd)
        GstOsd->UpdateStreamInfo(info);
}
