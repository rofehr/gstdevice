#include "gstreamer.h"
#include "config.h"
#include "gstdevice.h"
#include "gstosd.h"
#include "setup.h"

#include <vdr/plugin.h>
#include <vdr/remote.h>
#include <cstring>
#include <cstdlib>

// ============================================================
//  Global singletons
// ============================================================
cGstConfig    GstConfig;
sGstStreamInfo GstStreamInfo;
cGstDevice   GstDevice = nullptr;
cGstOsd      GstOsd    = nullptr;

// ============================================================
//  cPluginGstreamer – VDR plugin class
// ============================================================
class cPluginGstreamer : public cPlugin
{
public:
    cPluginGstreamer();
    virtual ~cPluginGstreamer() override;

    // cPlugin interface
    virtual const char *Version()     override { return PLUGIN_VERSION;     }
    virtual const char *Description() override { return PLUGIN_DESCRIPTION; }
    virtual const char *CommandLineHelp() override;
    virtual bool        ProcessArgs(int argc, char *argv[]) override;
    virtual bool        Initialize() override;
    virtual bool        Start() override;
    virtual void        Stop() override;
    virtual void        Housekeeping() override;
    virtual const char *MainMenuEntry() override { return nullptr; }
    virtual cOsdObject *MainMenuAction() override { return nullptr; }
    virtual cMenuSetupPage *SetupMenu() override;
    virtual bool        SetupParse(const char *Name, const char *Value) override;
    virtual bool        Service(const char *Id, void *Data = nullptr) override;

private:
    // Parsed command-line overrides (applied before Initialize)
    std::string m_argVideoSink;
    std::string m_argAudioSink;
};

VDRPLUGINCREATOR(cPluginGstreamer)

// ============================================================
//  Constructor / Destructor
// ============================================================
cPluginGstreamer::cPluginGstreamer()
{
    // Nothing – GStreamer is initialised in Initialize()
}

cPluginGstreamer::~cPluginGstreamer()
{
    // Stop() is guaranteed to be called before destructor
}

// ============================================================
//  CommandLineHelp
// ============================================================
const char *cPluginGstreamer::CommandLineHelp()
{
    return
        "  --videosink=ELEMENT   GStreamer video sink element (default: autovideosink)\n"
        "  --audiosink=ELEMENT   GStreamer audio sink element (default: autoaudiosink)\n";
}

// ============================================================
//  ProcessArgs
// ============================================================
bool cPluginGstreamer::ProcessArgs(int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "--videosink=", 12) == 0)
            m_argVideoSink = argv[i] + 12;
        else if (strncmp(argv[i], "--audiosink=", 12) == 0)
            m_argAudioSink = argv[i] + 12;
        else {
            esyslog("[gstreamer] Unknown argument: %s", argv[i]);
            return false;
        }
    }
    return true;
}

// ============================================================
//  Initialize – called before VDR enters main loop
//  (GStreamer init, device + OSD creation)
// ============================================================
bool cPluginGstreamer::Initialize()
{
    // Apply command-line sink overrides
    if (!m_argVideoSink.empty()) GstConfig.VideoSink = m_argVideoSink;
    if (!m_argAudioSink.empty()) GstConfig.AudioSink = m_argAudioSink;

    // Initialise GStreamer; pass nullptr so it doesn't consume VDR's argv
    if (!gst_is_initialized()) {
        GError *err = nullptr;
        if (!gst_init_check(nullptr, nullptr, &err)) {
            esyslog("[gstreamer] gst_init_check failed: %s",
                err ? err->message : "unknown");
            g_clear_error(&err);
            return false;
        }
    }

    // Log GStreamer version
    guint maj, min, mic, nano;
    gst_version(&maj, &min, &mic, &nano);
    isyslog("[gstreamer] GStreamer %u.%u.%u.%u", maj, min, mic, nano);

    // Create output device (registers itself with VDR automatically)
    GstDevice = new cGstDevice();

    // Create OSD handler
    GstOsd = new cGstOsd();
    GstOsd->SetTimeout(GstConfig.OsdTimeout);

    isyslog("[gstreamer] Plugin v%s initialised", PLUGIN_VERSION);
    return true;
}

// ============================================================
//  Start – called when VDR is fully up
// ============================================================
bool cPluginGstreamer::Start()
{
    if (GstDevice && !GstDevice->InitPipeline()) {
        esyslog("[gstreamer] Pipeline initialisation failed");
        return false;
    }
    return true;
}

// ============================================================
//  Stop – called on VDR shutdown
// ============================================================
void cPluginGstreamer::Stop()
{
    if (GstOsd) {
        delete GstOsd;
        GstOsd = nullptr;
    }
    if (GstDevice) {
        GstDevice->DestroyPipeline();
        // cDevice is not heap-deleted; VDR manages device lifetime
        // via the cDevice list.  Just null the pointer here.
        GstDevice = nullptr;
    }
    gst_deinit();
    isyslog("[gstreamer] Plugin stopped");
}

// ============================================================
//  Housekeeping – called once per second from VDR main loop
// ============================================================
void cPluginGstreamer::Housekeeping()
{
    // Nothing currently required; GStreamer manages its own mainloop
    // via the pipeline's internal threads.
}

// ============================================================
//  SetupMenu
// ============================================================
cMenuSetupPage *cPluginGstreamer::SetupMenu()
{
    return new cGstMenuSetup();
}

// ============================================================
//  SetupParse – restore settings from VDR's setup.conf
// ============================================================
bool cPluginGstreamer::SetupParse(const char *Name, const char *Value)
{
    if      (strcmp(Name, "VideoCodec")     == 0) { GstConfig.VideoCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "HardwareDecode") == 0) { GstConfig.HardwareDecode = atoi(Value) != 0; return true; }
    else if (strcmp(Name, "AudioCodec")     == 0) { GstConfig.AudioCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "AudioOffset")    == 0) { GstConfig.AudioOffset    = atoi(Value); return true; }
    else if (strcmp(Name, "Volume")         == 0) { GstConfig.Volume         = atoi(Value); return true; }
    else if (strcmp(Name, "OsdTimeout")     == 0) { GstConfig.OsdTimeout     = atoi(Value); return true; }
    else if (strcmp(Name, "VideoSink")      == 0) { GstConfig.VideoSink      = Value;        return true; }
    else if (strcmp(Name, "AudioSink")      == 0) { GstConfig.AudioSink      = Value;        return true; }

    return false;
}

// ============================================================
//  Service – inter-plugin communication
// ============================================================
bool cPluginGstreamer::Service(const char *Id, void * /*Data*/)
{
    // "GstreamerReconfigure" – called by external plugins to trigger
    // a live pipeline rebuild after changing GstConfig directly
    if (strcmp(Id, "GstreamerReconfigure") == 0) {
        if (GstDevice) GstDevice->ReconfigurePipeline();
        return true;
    }
    return false;
}
