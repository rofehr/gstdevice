/*
 * gstdevice.cpp  –  GStreamer output device implementation
 */

#include "gstdevice.h"
#include "gstosd.h"

#include <vdr/tools.h>

#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

cGstDevice::cGstDevice()
{
    dsyslog("[gstreamer] cGstDevice()");
}

cGstDevice::~cGstDevice()
{
    DestroyPipeline();
    dsyslog("[gstreamer] ~cGstDevice()");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public pipeline lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool cGstDevice::InitPipeline()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running)
        return true;
    return BuildPipeline();
}

void cGstDevice::DestroyPipeline()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    TeardownPipeline();
}

void cGstDevice::ReconfigurePipeline()
{
    const bool needRebuild =
        (m_activeVideoCodec != GstConfig.VideoCodec)     ||
        (m_activeHwDecode   != GstConfig.HardwareDecode) ||
        (m_activeAudioCodec != GstConfig.AudioCodec);

    if (needRebuild) {
        isyslog("[gstreamer] Config changed – rebuilding pipeline");
        std::lock_guard<std::mutex> lk(m_mutex);
        TeardownPipeline();
        BuildPipeline();
    } else if (m_activeAudioOffset != GstConfig.AudioOffset) {
        ApplyAudioOffset(GstConfig.AudioOffset);
        m_activeAudioOffset = GstConfig.AudioOffset;
    }
    ApplyVolume(GstConfig.Volume);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Pipeline construction
// ─────────────────────────────────────────────────────────────────────────────

bool cGstDevice::BuildPipeline()
{
    m_pipeline = gst_pipeline_new("vdr-gst");
    if (!m_pipeline) {
        esyslog("[gstreamer] gst_pipeline_new() failed");
        return false;
    }

    if (!CreateVideoElements() || !CreateAudioElements()) {
        TeardownPipeline();
        return false;
    }

    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    gst_bus_set_sync_handler(m_bus, BusSyncHandler, this, nullptr);

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        esyslog("[gstreamer] Pipeline → PLAYING failed");
        TeardownPipeline();
        return false;
    }

    m_activeVideoCodec  = GstConfig.VideoCodec;
    m_activeHwDecode    = GstConfig.HardwareDecode;
    m_activeAudioCodec  = GstConfig.AudioCodec;
    m_activeAudioOffset = GstConfig.AudioOffset;

    ApplyAudioOffset(GstConfig.AudioOffset);
    ApplyVolume(GstConfig.Volume);

    ResetTsState();
    InitTsParsers();

    m_running = true;
    isyslog("[gstreamer] Pipeline running  video=%s(%s)  audio=%s  hw=%d  offset=%d ms",
        GstConfig.VideoCodecName(), GstConfig.VideoDecoderName(),
        GstConfig.AudioCodecName(), (int)GstConfig.HardwareDecode,
        GstConfig.AudioOffset);
    return true;
}

// ── Video branch ──────────────────────────────────────────────────────────────
//  HW: appsrc → h264/h265parse → vaapih264/h265dec → vaapisink
//  SW: appsrc → h264/h265parse → avdec_h264/h265   → videoconvert → autovideosink

bool cGstDevice::CreateVideoElements()
{
    const bool hw        = GstConfig.HardwareDecode;
    const auto vsinkName = GstConfig.EffectiveVideoSink();

    m_videoSrc   = gst_element_factory_make("appsrc",                    "v-src");
    m_videoParse = gst_element_factory_make(GstConfig.VideoParseName(),   "v-parse");
    m_videoDec   = gst_element_factory_make(GstConfig.VideoDecoderName(), "v-dec");
    if (!hw)
        m_videoConv = gst_element_factory_make("videoconvert",            "v-conv");
    m_videoSink  = gst_element_factory_make(vsinkName.c_str(),            "v-sink");

    if (!m_videoSrc || !m_videoParse || !m_videoDec ||
        (!hw && !m_videoConv) || !m_videoSink) {
        esyslog("[gstreamer] Failed to create video elements (dec=%s sink=%s)",
            GstConfig.VideoDecoderName(), vsinkName.c_str());
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
        if (!gst_element_link_many(
                m_videoSrc, m_videoParse, m_videoDec, m_videoSink, nullptr)) {
            esyslog("[gstreamer] Cannot link HW video branch");
            return false;
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_videoSrc, m_videoParse, m_videoDec, m_videoConv, m_videoSink, nullptr);
        if (!gst_element_link_many(
                m_videoSrc, m_videoParse, m_videoDec,
                m_videoConv, m_videoSink, nullptr)) {
            esyslog("[gstreamer] Cannot link SW video branch");
            return false;
        }
    }
    return true;
}

// ── Audio branch ──────────────────────────────────────────────────────────────
//  appsrc → parse → decode → audioconvert → audioresample → identity → sink

bool cGstDevice::CreateAudioElements()
{
    m_audioSrc    = gst_element_factory_make("appsrc",                    "a-src");
    m_audioParse  = gst_element_factory_make(GstConfig.AudioParseName(),  "a-parse");
    m_audioDec    = gst_element_factory_make(GstConfig.AudioDecoderName(),"a-dec");
    m_audioConv   = gst_element_factory_make("audioconvert",              "a-conv");
    m_audioResamp = gst_element_factory_make("audioresample",             "a-resamp");
    m_audioSync   = gst_element_factory_make("identity",                  "a-sync");
    m_audioSink   = gst_element_factory_make(GstConfig.AudioSink.c_str(), "a-sink");

    if (!m_audioSrc || !m_audioParse || !m_audioDec ||
        !m_audioConv || !m_audioResamp || !m_audioSync || !m_audioSink) {
        esyslog("[gstreamer] Failed to create audio elements (dec=%s sink=%s)",
            GstConfig.AudioDecoderName(), GstConfig.AudioSink.c_str());
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

    if (!gst_element_link_many(
            m_audioSrc, m_audioParse, m_audioDec,
            m_audioConv, m_audioResamp, m_audioSync, m_audioSink, nullptr)) {
        esyslog("[gstreamer] Cannot link audio branch");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TS parser management
// ─────────────────────────────────────────────────────────────────────────────

void cGstDevice::InitTsParsers()
{
    m_videoParser = std::make_unique<cTsParser>(
        [this](const uint8_t *data, int len, uint64_t pts_ns) {
            const GstClockTime ts =
                (pts_ns != GST_CLOCK_TIME_NONE)
                ? NormalisePts(pts_ns, m_videoBasePts, m_lastVideoPts)
                : GST_CLOCK_TIME_NONE;
            PushEs(m_videoSrc, data, len, ts);
        });

    m_audioParser = std::make_unique<cTsParser>(
        [this](const uint8_t *data, int len, uint64_t pts_ns) {
            const GstClockTime ts =
                (pts_ns != GST_CLOCK_TIME_NONE)
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

// ─────────────────────────────────────────────────────────────────────────────
//  Pipeline teardown
// ─────────────────────────────────────────────────────────────────────────────

void cGstDevice::TeardownPipeline()
{
    m_running = false;

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
        m_pipeline    = nullptr;
        // Child elements freed automatically by the bin
        m_videoSrc    = m_videoParse = m_videoDec = m_videoConv = m_videoSink = nullptr;
        m_audioSrc    = m_audioParse = m_audioDec  = nullptr;
        m_audioConv   = m_audioResamp = m_audioSync = m_audioSink = nullptr;
    }
    dsyslog("[gstreamer] Pipeline torn down");
}

// ─────────────────────────────────────────────────────────────────────────────
//  GStreamer bus messages
// ─────────────────────────────────────────────────────────────────────────────

GstBusSyncReply cGstDevice::BusSyncHandler(GstBus *, GstMessage *msg, gpointer ud)
{
    static_cast<cGstDevice *>(ud)->HandleBusMessage(msg);
    return GST_BUS_PASS;
}

void cGstDevice::HandleBusMessage(GstMessage *msg)
{
    switch (GST_MESSAGE_TYPE(msg)) {

    case GST_MESSAGE_ERROR: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        esyslog("[gstreamer] ERROR from <%s>: %s — %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
            err ? err->message : "?",
            dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        break;
    }

    case GST_MESSAGE_WARNING: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        dsyslog("[gstreamer] WARNING from <%s>: %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
            err ? err->message : "?");
        g_clear_error(&err);
        g_free(dbg);
        break;
    }

    case GST_MESSAGE_EOS:
        dsyslog("[gstreamer] EOS");
        break;

    case GST_MESSAGE_BUFFERING: {
        gint pct = 0;
        gst_message_parse_buffering(msg, &pct);
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
            dsyslog("[gstreamer] State: %s → %s",
                gst_element_state_get_name(o),
                gst_element_state_get_name(n));
        }
        break;

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  cDevice overrides
// ─────────────────────────────────────────────────────────────────────────────

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
            std::lock_guard<std::mutex> lk(m_mutex);
            BuildPipeline();
        }
        if (m_pipeline)
            gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        break;
    default:
        break;
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
    GstEvent *ev = gst_event_new_seek(
        rate, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, pos,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    gst_element_send_event(m_pipeline, ev);
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

void cGstDevice::Play()
{
    if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

void cGstDevice::Freeze()
{
    if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
}

void cGstDevice::Mute()
{
    if (m_audioSink)
        g_object_set(G_OBJECT(m_audioSink), "volume", 0.0, nullptr);
}

void cGstDevice::SetVolumeDevice(int Volume)
{
    ApplyVolume(Volume);
}

void cGstDevice::ApplyAudioOffset(int ms)
{
    if (!m_audioSync) return;
    g_object_set(G_OBJECT(m_audioSync),
        "ts-offset", static_cast<gint64>(ms) * GST_MSECOND, nullptr);
    dsyslog("[gstreamer] Audio offset %d ms", ms);
}

void cGstDevice::ApplyVolume(int vol)
{
    if (!m_audioSink) return;
    g_object_set(G_OBJECT(m_audioSink),
        "volume", static_cast<gdouble>(vol) / 255.0, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  PTS normalisation  (33-bit wrap-around + discontinuity handling)
// ─────────────────────────────────────────────────────────────────────────────

GstClockTime cGstDevice::NormalisePts(GstClockTime rawNs,
                                       GstClockTime &basePts,
                                       GstClockTime &lastPts)
{
    if (basePts == GST_CLOCK_TIME_NONE) {
        basePts = lastPts = rawNs;
        return 0;
    }

    // 33-bit PTS wrap period ≈ 2^33 / 90000 s ≈ 26.5 h in nanoseconds
    static constexpr GstClockTime kWrap33 =
        static_cast<GstClockTime>((1ULL << 33) * 100000ULL / 9ULL);

    if (lastPts != GST_CLOCK_TIME_NONE &&
        rawNs + kPtsDiscThreshold < lastPts)
    {
        if (lastPts > kWrap33 / 2 && rawNs < kWrap33 / 2)
            basePts -= kWrap33;   // 33-bit rollover
        else
            basePts = rawNs;      // genuine discontinuity – reset
    }

    lastPts = rawNs;
    return (rawNs >= basePts) ? (rawNs - basePts) : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Buffer push helpers
// ─────────────────────────────────────────────────────────────────────────────

int cGstDevice::PushEs(GstElement *src,
                        const uint8_t *data, int len,
                        GstClockTime pts)
{
    if (!src || !data || len <= 0) return 0;

    GstBuffer *buf = gst_buffer_new_allocate(nullptr, static_cast<gsize>(len), nullptr);
    if (!buf) return -1;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return -1;
    }
    std::memcpy(map.data, data, len);
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf)      = pts;
    GST_BUFFER_DTS(buf)      = pts;
    GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

    const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    if (ret != GST_FLOW_OK) {
        dsyslog("[gstreamer] push_buffer returned %d (len=%d)", (int)ret, len);
        return -1;
    }
    return len;
}

int cGstDevice::PushBuffer(GstElement *src, const uchar *data, int len)
{
    return PushEs(src, data, len, GST_CLOCK_TIME_NONE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TS delivery
//
//  VDR 2.2+ pre-filters TS packets by PID and calls exactly one of these
//  two methods per 188-byte packet.  There is no PlayTs() in cDevice.
// ─────────────────────────────────────────────────────────────────────────────

int cGstDevice::PlayTsVideo(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    // Lazily create parsers once the appsrc elements exist
    if (!m_videoParser && m_videoSrc && m_audioSrc)
        InitTsParsers();

    if (m_videoPid == -1) {
        m_videoPid = ((Data[1] & 0x1F) << 8) | Data[2];
        dsyslog("[gstreamer] Video PID %d", m_videoPid);
    }

    if (m_videoParser)
        m_videoParser->Feed(Data, kTsPacketSize);

    return Length;
}

int cGstDevice::PlayTsAudio(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    if (!m_audioParser && m_videoSrc && m_audioSrc)
        InitTsParsers();

    if (m_audioPid == -1) {
        m_audioPid = ((Data[1] & 0x1F) << 8) | Data[2];
        dsyslog("[gstreamer] Audio PID %d", m_audioPid);
    }

    if (m_audioParser)
        m_audioParser->Feed(Data, kTsPacketSize);

    return Length;
}

// ── Legacy PES path ───────────────────────────────────────────────────────────

int cGstDevice::PlayVideo(const uchar *D, int L)
{
    return PushBuffer(m_videoSrc, D, L);
}

int cGstDevice::PlayAudio(const uchar *D, int L, uchar /*Id*/)
{
    return PushBuffer(m_audioSrc, D, L);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Poll / Flush / GetSTC
// ─────────────────────────────────────────────────────────────────────────────

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
        return static_cast<int64_t>(pos / 90);  // ns → 90 kHz ticks
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SetChannelDevice  (returns bool in VDR 2.6+)
// ─────────────────────────────────────────────────────────────────────────────

bool cGstDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
    if (!cDevice::SetChannelDevice(Channel, LiveView))
        return false;

    if (LiveView && Channel) {
        ResetTsState();
        dsyslog("[gstreamer] Channel switch – TS state reset");

        if (GstOsd) {
            cCondWait::SleepMs(300);
            QueryAndUpdateStreamInfo();
            GstOsd->ShowForChannel(Channel);
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Remote key handler
// ─────────────────────────────────────────────────────────────────────────────

eOSState cGstDevice::ProcessKey(eKeys Key)
{
    if (!GstOsd) return osUnknown;
    switch (Key) {
    case kOk:
    case kInfo:
        QueryAndUpdateStreamInfo();
        GstOsd->Toggle();
        return osContinue;
    default:
        return osUnknown;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Stream info query  →  GstStreamInfo  →  OSD
// ─────────────────────────────────────────────────────────────────────────────

void cGstDevice::QueryAndUpdateStreamInfo()
{
    sGstStreamInfo info;
    info.videoCodec = GstConfig.VideoCodecName();
    info.audioCodec = GstConfig.AudioCodecName();
    info.hwDecode   = GstConfig.HardwareDecode;

    if (m_pipeline) {
        GstState st = GST_STATE_NULL;
        gst_element_get_state(m_pipeline, &st, nullptr, 0);
        switch (st) {
        case GST_STATE_PLAYING: info.pipelineState = "Playing"; break;
        case GST_STATE_PAUSED:  info.pipelineState = "Paused";  break;
        case GST_STATE_READY:   info.pipelineState = "Ready";   break;
        default:                info.pipelineState = "Idle";    break;
        }
    }

    auto readCaps = [](GstElement *elem, const char *padName,
                       int &w, int &h, double &fps,
                       int &rate, int &ch) {
        if (!elem) return;
        GstPad *pad = gst_element_get_static_pad(elem, padName);
        if (!pad) return;
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps) {
            const GstStructure *s = gst_caps_get_structure(caps, 0);
            if (s) {
                gst_structure_get_int(s, "width",    &w);
                gst_structure_get_int(s, "height",   &h);
                gst_structure_get_int(s, "rate",     &rate);
                gst_structure_get_int(s, "channels", &ch);
                const GValue *fv = gst_structure_get_value(s, "framerate");
                if (fv && GST_VALUE_HOLDS_FRACTION(fv)) {
                    int n = gst_value_get_fraction_numerator(fv);
                    int d = gst_value_get_fraction_denominator(fv);
                    if (d > 0) fps = (double)n / d;
                }
            }
            gst_caps_unref(caps);
        }
        gst_object_unref(pad);
    };

    readCaps(m_videoSink, "sink",
             info.videoWidth, info.videoHeight, info.videoFps,
             info.audioSampleRate, info.audioChannels);
    readCaps(m_audioSink, "sink",
             info.videoWidth, info.videoHeight, info.videoFps,
             info.audioSampleRate, info.audioChannels);

    GstStreamInfo = info;
    if (GstOsd)
        GstOsd->UpdateStreamInfo(info);
}
