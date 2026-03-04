#include "gstdevice.h"
#include <vdr/tools.h>
#include <cstring>

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
    // Check if anything affecting pipeline structure changed
    bool needRebuild =
        (m_activeVideoCodec  != GstConfig.VideoCodec)  ||
        (m_activeHwDecode    != GstConfig.HardwareDecode) ||
        (m_activeAudioCodec  != GstConfig.AudioCodec);

    bool needOffsetUpdate =
        (m_activeAudioOffset != GstConfig.AudioOffset);

    if (needRebuild) {
        isyslog("[gstreamer] Config changed, rebuilding pipeline...");
        std::lock_guard<std::mutex> lock(m_mutex);
        TeardownPipeline();
        BuildPipeline();
    } else if (needOffsetUpdate) {
        ApplyAudioOffset(GstConfig.AudioOffset);
        m_activeAudioOffset = GstConfig.AudioOffset;
    }

    ApplyVolume(GstConfig.Volume);
}

// ============================================================
//  Pipeline build / teardown
// ============================================================
bool cGstDevice::BuildPipeline()
{
    m_pipeline = gst_pipeline_new("vdr-gst-pipeline");
    if (!m_pipeline) {
        esyslog("[gstreamer] ERROR: gst_pipeline_new failed");
        return false;
    }

    if (!CreateVideoElements() || !CreateAudioElements()) {
        TeardownPipeline();
        return false;
    }

    // Bus
    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    gst_bus_set_sync_handler(m_bus, BusSyncHandler, this, nullptr);

    // Start
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        esyslog("[gstreamer] ERROR: Failed to set pipeline to PLAYING");
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

    m_running = true;
    isyslog("[gstreamer] Pipeline running: video=%s(%s) audio=%s hw=%d offset=%dms",
        GstConfig.VideoCodecName(), GstConfig.VideoDecoderName(),
        GstConfig.AudioCodecName(),
        GstConfig.HardwareDecode,
        GstConfig.AudioOffset);
    return true;
}

// ------------------------------------------------------------
//  Video branch
//  SW:  appsrc → h264parse/h265parse → avdec_h264/h265 → videoconvert → sink
//  HW:  appsrc → h264parse/h265parse → vaapih264dec/h265dec → vaapisink
// ------------------------------------------------------------
bool cGstDevice::CreateVideoElements()
{
    bool hw = GstConfig.HardwareDecode;

    m_videoSrc   = gst_element_factory_make("appsrc",                    "video-src");
    m_videoParse = gst_element_factory_make(GstConfig.VideoParseName(),   "video-parse");
    m_videoDec   = gst_element_factory_make(GstConfig.VideoDecoderName(), "video-dec");

    // VA-API decoder includes colour conversion; SW path needs videoconvert
    if (!hw)
        m_videoConv = gst_element_factory_make("videoconvert", "video-conv");

    std::string vSink = GstConfig.EffectiveVideoSink();
    m_videoSink = gst_element_factory_make(vSink.c_str(), "video-sink");

    if (!m_videoSrc || !m_videoParse || !m_videoDec || !m_videoSink ||
        (!hw && !m_videoConv))
    {
        esyslog("[gstreamer] ERROR: Failed to create video elements "
                "(parse=%s dec=%s sink=%s hw=%d)",
            GstConfig.VideoParseName(), GstConfig.VideoDecoderName(),
            vSink.c_str(), hw);
        return false;
    }

    // Configure appsrc
    g_object_set(G_OBJECT(m_videoSrc),
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        "max-bytes",   (guint64)(2 * 1024 * 1024),
        nullptr);

    g_object_set(G_OBJECT(m_videoSink), "sync", TRUE, nullptr);

    // Add to bin
    if (hw) {
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_videoSrc, m_videoParse, m_videoDec, m_videoSink, nullptr);
        if (!gst_element_link_many(m_videoSrc, m_videoParse, m_videoDec, m_videoSink, nullptr)) {
            esyslog("[gstreamer] ERROR: Failed to link HW video branch");
            return false;
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_videoSrc, m_videoParse, m_videoDec, m_videoConv, m_videoSink, nullptr);
        if (!gst_element_link_many(m_videoSrc, m_videoParse, m_videoDec, m_videoConv, m_videoSink, nullptr)) {
            esyslog("[gstreamer] ERROR: Failed to link SW video branch");
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------
//  Audio branch
//  appsrc → aacparse/mpegaudioparse → avdec_aac/mp3
//         → audioconvert → audioresample → identity(tsoffset) → sink
// ------------------------------------------------------------
bool cGstDevice::CreateAudioElements()
{
    m_audioSrc    = gst_element_factory_make("appsrc",                    "audio-src");
    m_audioParse  = gst_element_factory_make(GstConfig.AudioParseName(),  "audio-parse");
    m_audioDec    = gst_element_factory_make(GstConfig.AudioDecoderName(),"audio-dec");
    m_audioConv   = gst_element_factory_make("audioconvert",              "audio-conv");
    m_audioResamp = gst_element_factory_make("audioresample",             "audio-resamp");
    m_audioSync   = gst_element_factory_make("identity",                  "audio-sync");
    m_audioSink   = gst_element_factory_make(GstConfig.AudioSink.c_str(), "audio-sink");

    if (!m_audioSrc || !m_audioParse || !m_audioDec ||
        !m_audioConv || !m_audioResamp || !m_audioSync || !m_audioSink)
    {
        esyslog("[gstreamer] ERROR: Failed to create audio elements "
                "(parse=%s dec=%s sink=%s)",
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
                               m_audioConv, m_audioResamp, m_audioSync, m_audioSink, nullptr))
    {
        esyslog("[gstreamer] ERROR: Failed to link audio branch");
        return false;
    }
    return true;
}

void cGstDevice::TeardownPipeline()
{
    m_running = false;

    // Flush and destroy TS parsers before stopping the pipeline
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
        // All child elements owned by bin, freed automatically
        m_videoSrc   = m_videoParse = m_videoDec = m_videoConv = m_videoSink = nullptr;
        m_audioSrc   = m_audioParse = m_audioDec = nullptr;
        m_audioConv  = m_audioResamp = m_audioSync = m_audioSink = nullptr;
    }
    dsyslog("[gstreamer] Pipeline torn down");
}

// ============================================================
//  Audio offset (A/V sync)
//  Uses GStreamer identity element's ts-offset property (nanoseconds)
// ============================================================
void cGstDevice::ApplyAudioOffset(int offsetMs)
{
    if (!m_audioSync)
        return;
    gint64 offsetNs = (gint64)offsetMs * GST_MSECOND;
    g_object_set(G_OBJECT(m_audioSync), "ts-offset", offsetNs, nullptr);
    dsyslog("[gstreamer] Audio offset set to %d ms (%lld ns)", offsetMs, (long long)offsetNs);
}

void cGstDevice::ApplyVolume(int vol)
{
    if (!m_audioSink)
        return;
    gdouble v = (gdouble)vol / 255.0;
    // autoaudiosink / pulsesink expose a "volume" property
    g_object_set(G_OBJECT(m_audioSink), "volume", v, nullptr);
}

// ============================================================
//  Bus handler
// ============================================================
GstBusSyncReply cGstDevice::BusSyncHandler(GstBus * /*bus*/, GstMessage *msg, gpointer data)
{
    static_cast<cGstDevice*>(data)->HandleBusMessage(msg);
    return GST_BUS_PASS;
}

void cGstDevice::HandleBusMessage(GstMessage *msg)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        esyslog("[gstreamer] ERROR from <%s>: %s | debug: %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
            err ? err->message : "?", dbg ? dbg : "");
        g_clear_error(&err); g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = nullptr; gchar *dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        dsyslog("[gstreamer] WARNING from <%s>: %s",
            GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "?");
        g_clear_error(&err); g_free(dbg);
        break;
    }
    case GST_MESSAGE_EOS:
        dsyslog("[gstreamer] EOS received");
        break;
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering(msg, &percent);
        dsyslog("[gstreamer] Buffering: %d%%", percent);
        if (GstOsd && GstOsd->IsVisible()) {
            sGstStreamInfo info = GstStreamInfo;
            info.pipelineState    = "Buffering";
            info.bufferingPercent = percent;
            GstOsd->UpdateStreamInfo(info);
        }
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(m_pipeline)) {
            GstState o, n, p;
            gst_message_parse_state_changed(msg, &o, &n, &p);
            dsyslog("[gstreamer] State: %s → %s",
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
    gdouble rate = (gdouble)Speed / 100.0;
    if (!Forward) rate = -rate;
    gint64 pos = 0;
    gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos);
    GstEvent *seek = gst_event_new_seek(rate, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, pos,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    gst_element_send_event(m_pipeline, seek);
}

void cGstDevice::Clear()
{
    // Flush pending partial PES buffers
    if (m_videoParser) m_videoParser->Flush();
    if (m_audioParser) m_audioParser->Flush();

    // Reset PTS tracking so the next stream starts from t=0
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
void cGstDevice::Freeze() { if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PAUSED); }
void cGstDevice::Mute()   { if (m_audioSink) g_object_set(G_OBJECT(m_audioSink), "volume", 0.0, nullptr); }

void cGstDevice::SetVolumeDevice(int Volume)
{
    ApplyVolume(Volume);
}

// ============================================================
//  Pipeline build: initialise TS parsers alongside elements
// ============================================================

// Called at the end of BuildPipeline() – wire up parser callbacks
// (inserted after ApplyVolume call)
static void InitTsParsers_impl(cGstDevice *dev,
                                GstElement *videoSrc,
                                GstElement *audioSrc,
                                GstClockTime &videoBasePts,
                                GstClockTime &audioBasePts,
                                GstClockTime &lastVideoPts,
                                GstClockTime &lastAudioPts,
                                std::unique_ptr<cTsParser> &videoParser,
                                std::unique_ptr<cTsParser> &audioParser)
{
    videoBasePts = audioBasePts = GST_CLOCK_TIME_NONE;
    lastVideoPts = lastAudioPts = GST_CLOCK_TIME_NONE;

    videoParser = std::make_unique<cTsParser>(
        [dev, videoSrc, &videoBasePts, &lastVideoPts]
        (const uint8_t *data, int len, uint64_t pts_ns)
        {
            GstClockTime ts = (pts_ns != GST_CLOCK_TIME_NONE)
                ? dev->NormalisePts(pts_ns, videoBasePts, lastVideoPts)
                : GST_CLOCK_TIME_NONE;
            dev->PushEs(videoSrc, data, len, ts);
        });

    audioParser = std::make_unique<cTsParser>(
        [dev, audioSrc, &audioBasePts, &lastAudioPts]
        (const uint8_t *data, int len, uint64_t pts_ns)
        {
            GstClockTime ts = (pts_ns != GST_CLOCK_TIME_NONE)
                ? dev->NormalisePts(pts_ns, audioBasePts, lastAudioPts)
                : GST_CLOCK_TIME_NONE;
            dev->PushEs(audioSrc, data, len, ts);
        });
}

// ============================================================
//  PTS normalisation
//  Handles 33-bit wrap-around (every ~26.5 h at 90 kHz) and
//  large discontinuities caused by channel changes / seeks.
// ============================================================
GstClockTime cGstDevice::NormalisePts(GstClockTime raw_ns,
                                       GstClockTime &basePts,
                                       GstClockTime &lastPts)
{
    // First PTS seen – establish base
    if (basePts == GST_CLOCK_TIME_NONE) {
        basePts  = raw_ns;
        lastPts  = raw_ns;
        return 0;
    }

    // 33-bit PTS wraps at ~26.5 h → ~95443 s in ns
    static constexpr GstClockTime kWrap33 = (GstClockTime)((1ULL << 33) * 100000ULL / 9ULL);

    GstClockTime adjusted = raw_ns;

    // Detect backward jump larger than threshold → likely wrap or discontinuity
    if (lastPts != GST_CLOCK_TIME_NONE && raw_ns + kPtsDiscontinuityThreshold < lastPts) {
        // Could be a 33-bit wrap
        if (lastPts > kWrap33 / 2 && raw_ns < kWrap33 / 2)
            basePts -= kWrap33;   // compensate wrap
        else
            basePts = raw_ns;     // discontinuity – reset base
    }

    lastPts  = raw_ns;
    adjusted = (raw_ns >= basePts) ? (raw_ns - basePts) : 0;
    return adjusted;
}

// ============================================================
//  PushEs – push a demuxed ES buffer with PTS into appsrc
// ============================================================
int cGstDevice::PushEs(GstElement *src, const uint8_t *data, int len, GstClockTime pts)
{
    if (!src || !data || len <= 0)
        return 0;

    GstBuffer *buf = gst_buffer_new_allocate(nullptr, (gsize)len, nullptr);
    if (!buf)
        return -1;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return -1;
    }
    std::memcpy(map.data, data, len);
    gst_buffer_unmap(buf, &map);

    // Stamp PTS – GStreamer uses these for A/V sync
    GST_BUFFER_PTS(buf)      = pts;
    GST_BUFFER_DTS(buf)      = pts;   // DTS ≈ PTS for live streams
    GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    if (ret != GST_FLOW_OK) {
        dsyslog("[gstreamer] PushEs: flow error %d (len=%d)", (int)ret, len);
        return -1;
    }
    return len;
}

// ============================================================
//  PushBuffer – legacy path (no PTS, used by PlayVideo/Audio ES)
// ============================================================
int cGstDevice::PushBuffer(GstElement *src, const uchar *data, int len)
{
    return PushEs(src, data, len, GST_CLOCK_TIME_NONE);
}

// ============================================================
//  PlayTs – main entry point for live TV and recordings
//
//  VDR calls this with one or more contiguous 188-byte TS packets.
//  Each packet carries either video or audio payload for a single PID.
//
//  We:
//   1. Identify the PID from the TS header
//   2. Route the packet to the matching cTsParser
//   3. The parser reassembles PES frames and calls our callback
//      which calls PushEs() with the extracted PTS
// ============================================================
int cGstDevice::PlayTs(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    // Lazily initialise parsers the first time PlayTs is called
    // (pipeline must be up first)
    if (!m_videoParser && m_videoSrc && m_audioSrc) {
        InitTsParsers_impl(this,
            m_videoSrc, m_audioSrc,
            m_videoBasePts, m_audioBasePts,
            m_lastVideoPts, m_lastAudioPts,
            m_videoParser, m_audioParser);
        dsyslog("[gstreamer] TS parsers initialised");
    }

    const uchar *p   = Data;
    int          rem = Length;

    while (rem >= kTsPacketSize) {
        // Sync byte check
        if (p[0] != kTsSyncByte) {
            // Try to re-sync by scanning forward
            ++p; --rem;
            continue;
        }

        // Extract 13-bit PID from bytes 1-2
        int pid = ((p[1] & 0x1F) << 8) | p[2];

        // Auto-detect PIDs on first packets if not set by SetPid()
        // VDR always sends video on one PID and audio on another.
        // We learn them from the stream_id in the PES header when PUSI=1.
        if (m_videoPid == -1 || m_audioPid == -1) {
            bool pusi = (p[1] & 0x40) != 0;
            if (pusi) {
                // Find payload start
                bool hasAdapt   = (p[3] & 0x20) != 0;
                int  pesOffset  = 4 + (hasAdapt ? 1 + p[4] : 0);
                if (pesOffset + 3 < kTsPacketSize) {
                    const uchar *pes = p + pesOffset;
                    // PES start code 0x000001
                    if (pes[0] == 0x00 && pes[1] == 0x00 && pes[2] == 0x01) {
                        uint8_t streamId = pes[3];
                        // stream_id 0xE0-0xEF = video, 0xC0-0xDF = audio
                        if (streamId >= 0xE0 && streamId <= 0xEF && m_videoPid == -1) {
                            m_videoPid = pid;
                            dsyslog("[gstreamer] Auto-detected video PID: %d (stream_id=0x%02X)",
                                pid, streamId);
                        } else if (streamId >= 0xC0 && streamId <= 0xDF && m_audioPid == -1) {
                            m_audioPid = pid;
                            dsyslog("[gstreamer] Auto-detected audio PID: %d (stream_id=0x%02X)",
                                pid, streamId);
                        }
                    }
                }
            }
        }

        // Route packet to the right parser
        if (pid == m_videoPid && m_videoParser)
            m_videoParser->Feed(p);
        else if (pid == m_audioPid && m_audioParser)
            m_audioParser->Feed(p);
        // Silently discard PAT, PMT, NIT and other PIDs

        p   += kTsPacketSize;
        rem -= kTsPacketSize;
    }

    return Length;
}

// ============================================================
//  PlayTsVideo / PlayTsAudio
//  VDR pre-filters and calls these with a single TS packet
//  whose PID is already known to be video or audio.
//  We simply route directly to the matching parser.
// ============================================================
int cGstDevice::PlayTsVideo(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    if (!m_videoParser && m_videoSrc && m_audioSrc) {
        InitTsParsers_impl(this,
            m_videoSrc, m_audioSrc,
            m_videoBasePts, m_audioBasePts,
            m_lastVideoPts, m_lastAudioPts,
            m_videoParser, m_audioParser);
    }

    // Learn PID from this packet if not yet known
    if (m_videoPid == -1)
        m_videoPid = ((Data[1] & 0x1F) << 8) | Data[2];

    if (m_videoParser)
        m_videoParser->Feed(Data, kTsPacketSize);

    return Length;
}

int cGstDevice::PlayTsAudio(const uchar *Data, int Length)
{
    if (!Data || Length < kTsPacketSize || !m_running)
        return Length;

    if (!m_audioParser && m_videoSrc && m_audioSrc) {
        InitTsParsers_impl(this,
            m_videoSrc, m_audioSrc,
            m_videoBasePts, m_audioBasePts,
            m_lastVideoPts, m_lastAudioPts,
            m_videoParser, m_audioParser);
    }

    if (m_audioPid == -1)
        m_audioPid = ((Data[1] & 0x1F) << 8) | Data[2];

    if (m_audioParser)
        m_audioParser->Feed(Data, kTsPacketSize);

    return Length;
}

// ============================================================
//  Legacy ES path (used when VDR calls PlayVideo / PlayAudio
//  directly, e.g. for certain replay modes)
// ============================================================
int cGstDevice::PlayVideo(const uchar *D, int L) { return PushBuffer(m_videoSrc, D, L); }
int cGstDevice::PlayAudio(const uchar *D, int L, uchar) { return PushBuffer(m_audioSrc, D, L); }

bool cGstDevice::Poll(cPoller & /*P*/, int /*T*/)
{
    if (!m_videoSrc) return true;
    guint64 q = 0;
    g_object_get(G_OBJECT(m_videoSrc), "current-level-bytes", &q, nullptr);
    return q < (guint64)(1 * 1024 * 1024);
}

bool cGstDevice::Flush(int /*T*/) { return true; }

int64_t cGstDevice::GetSTC()
{
    if (!m_pipeline) return -1;
    gint64 pos = 0;
    if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos))
        return (int64_t)(pos / 90);   // ns → 90 kHz PTS ticks
    return -1;
}

// ============================================================
//  OSD: channel switch → show info banner
// ============================================================
void cGstDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
    cDevice::SetChannelDevice(Channel, LiveView);

    if (LiveView && Channel) {
        // Reset PID detection and PTS state for the new channel
        m_videoPid = -1;
        m_audioPid = -1;
        if (m_videoParser) m_videoParser->Reset();
        if (m_audioParser) m_audioParser->Reset();
        m_videoBasePts = m_audioBasePts = GST_CLOCK_TIME_NONE;
        m_lastVideoPts = m_lastAudioPts = GST_CLOCK_TIME_NONE;

        dsyslog("[gstreamer] Channel switch: PIDs and PTS reset");

        if (GstOsd) {
            cCondWait::SleepMs(300);
            QueryAndUpdateStreamInfo();
            GstOsd->ShowForChannel(Channel);
        }
    }
}

// ============================================================
//  OSD: remote-control key → toggle OSD on OK/INFO
// ============================================================
eOSState cGstDevice::ProcessKey(eKeys Key)
{
    if (!GstOsd)
        return osUnknown;

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
//  Query GStreamer pipeline for video/audio caps → OSD info
// ============================================================
void cGstDevice::QueryAndUpdateStreamInfo()
{
    sGstStreamInfo info;

    // Fill static config info
    info.videoCodec  = GstConfig.VideoCodecName();
    info.audioCodec  = GstConfig.AudioCodecName();
    info.hwDecode    = GstConfig.HardwareDecode;

    // Pipeline state
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
                    // Framerate as fraction
                    const GValue *fpsVal = gst_structure_get_value(s, "framerate");
                    if (fpsVal && GST_VALUE_HOLDS_FRACTION(fpsVal)) {
                        int num = gst_value_get_fraction_numerator(fpsVal);
                        int den = gst_value_get_fraction_denominator(fpsVal);
                        if (den > 0) info.videoFps = (double)num / (double)den;
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
