/*
 * gstreamer.cpp  –  VDR plugin entry point
 *
 * Defines all global singletons.
 * Creates cGstDevice and cGstOsd in Initialize(), starts the pipeline
 * in Start(), and cleans up in Stop().
 */

#include "gstreamer.h"
#include "config.h"
#include "gstdevice.h"
#include "gstosd.h"
#include "setup.h"

#include <vdr/plugin.h>
#include <cstring>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
//  Global singletons
//  (declared extern in config.h, gstdevice.h, gstosd.h)
// ─────────────────────────────────────────────────────────────────────────────

cGstConfig     GstConfig;
sGstStreamInfo GstStreamInfo;
cGstDevice    *GstOutputDevice = nullptr;
cGstOsd       *GstOsd          = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  cPluginGstreamer
// ─────────────────────────────────────────────────────────────────────────────

class cPluginGstreamer : public cPlugin
{
public:
    cPluginGstreamer() = default;
    virtual ~cPluginGstreamer() override = default;

    virtual const char *Version()     override { return PLUGIN_VERSION;     }
    virtual const char *Description() override { return PLUGIN_DESCRIPTION; }

    virtual const char *CommandLineHelp() override;
    virtual bool        ProcessArgs(int argc, char *argv[]) override;
    virtual bool        Initialize() override;
    virtual bool        Start() override;
    virtual void        Stop() override;
    virtual void        Housekeeping() override {}

    virtual const char     *MainMenuEntry()  override { return nullptr; }
    virtual cOsdObject      *MainMenuAction() override { return nullptr; }
    virtual cMenuSetupPage  *SetupMenu()      override;
    virtual bool             SetupParse(const char *Name, const char *Value) override;
    virtual bool             Service(const char *Id, void *Data = nullptr) override;

private:
    std::string m_argVideoSink;
    std::string m_argAudioSink;
};

VDRPLUGINCREATOR(cPluginGstreamer)

// ─────────────────────────────────────────────────────────────────────────────

const char *cPluginGstreamer::CommandLineHelp()
{
    return
        "  --videosink=ELEMENT   GStreamer video sink (default: autovideosink)\n"
        "  --audiosink=ELEMENT   GStreamer audio sink (default: autoaudiosink)\n";
}

bool cPluginGstreamer::ProcessArgs(int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
        if      (strncmp(argv[i], "--videosink=", 12) == 0) m_argVideoSink = argv[i] + 12;
        else if (strncmp(argv[i], "--audiosink=", 12) == 0) m_argAudioSink = argv[i] + 12;
        else {
            esyslog("[gstreamer] Unknown argument: %s", argv[i]);
            return false;
        }
    }
    return true;
}

bool cPluginGstreamer::Initialize()
{
    if (!m_argVideoSink.empty()) GstConfig.VideoSink = m_argVideoSink;
    if (!m_argAudioSink.empty()) GstConfig.AudioSink = m_argAudioSink;

    // Initialise GStreamer without touching VDR's argc/argv
    if (!gst_is_initialized()) {
        GError *err = nullptr;
        if (!gst_init_check(nullptr, nullptr, &err)) {
            esyslog("[gstreamer] gst_init_check failed: %s",
                err ? err->message : "unknown error");
            g_clear_error(&err);
            return false;
        }
    }

    guint maj, min, mic, nano;
    gst_version(&maj, &min, &mic, &nano);
    isyslog("[gstreamer] GStreamer %u.%u.%u.%u", maj, min, mic, nano);

    // cGstDevice registers itself with VDR's device list automatically
    GstOutputDevice = new cGstDevice();

    GstOsd = new cGstOsd();
    GstOsd->SetTimeout(GstConfig.OsdTimeout);

    isyslog("[gstreamer] Plugin v%s initialised", PLUGIN_VERSION);
    return true;
}

bool cPluginGstreamer::Start()
{
    if (GstOutputDevice && !GstOutputDevice->InitPipeline()) {
        esyslog("[gstreamer] Pipeline init failed");
        return false;
    }
    return true;
}

void cPluginGstreamer::Stop()
{
    // OSD must be destroyed before the device (it holds references to channel info)
    delete GstOsd;
    GstOsd = nullptr;

    if (GstOutputDevice) {
        GstOutputDevice->DestroyPipeline();
        // cDevice lifetime is managed by VDR's internal device list.
        // We only null our pointer; VDR will not free it either —
        // cDevice objects are typically allocated for the lifetime of VDR.
        GstOutputDevice = nullptr;
    }

    gst_deinit();
    isyslog("[gstreamer] Plugin stopped");
}

cMenuSetupPage *cPluginGstreamer::SetupMenu()
{
    return new cGstMenuSetup();
}

bool cPluginGstreamer::SetupParse(const char *Name, const char *Value)
{
    if      (!strcmp(Name, "VideoCodec"))     { GstConfig.VideoCodec     = atoi(Value);       return true; }
    else if (!strcmp(Name, "HardwareDecode")) { GstConfig.HardwareDecode = atoi(Value) != 0;  return true; }
    else if (!strcmp(Name, "AudioCodec"))     { GstConfig.AudioCodec     = atoi(Value);       return true; }
    else if (!strcmp(Name, "AudioOffset"))    { GstConfig.AudioOffset    = atoi(Value);       return true; }
    else if (!strcmp(Name, "Volume"))         { GstConfig.Volume         = atoi(Value);       return true; }
    else if (!strcmp(Name, "OsdTimeout"))     { GstConfig.OsdTimeout     = atoi(Value);       return true; }
    else if (!strcmp(Name, "VideoSink"))      { GstConfig.VideoSink      = Value;             return true; }
    else if (!strcmp(Name, "AudioSink"))      { GstConfig.AudioSink      = Value;             return true; }
    return false;
}

bool cPluginGstreamer::Service(const char *Id, void * /*Data*/)
{
    if (!strcmp(Id, "GstreamerReconfigure")) {
        if (GstOutputDevice) GstOutputDevice->ReconfigurePipeline();
        return true;
    }
    return false;
}
