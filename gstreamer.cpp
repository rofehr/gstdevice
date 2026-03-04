#include "gstreamer.h"
#include "gstdevice.h"
#include "gstosd.h"
#include "setup.h"
#include "config.h"
#include <vdr/plugin.h>
#include <cstring>

// ============================================================
//  Global instances
// ============================================================
cGstConfig      GstConfig;
cGstOsd        *GstOsd        = nullptr;
sGstStreamInfo  GstStreamInfo;

// ============================================================
//  cPluginGstreamer
// ============================================================
class cPluginGstreamer : public cPlugin
{
public:
    cPluginGstreamer() = default;
    virtual ~cPluginGstreamer();

    virtual const char *Version()        override { return PLUGIN_VERSION; }
    virtual const char *Description()    override { return tr(PLUGIN_DESCRIPTION); }
    virtual const char *MainMenuEntry()  override { return nullptr; }

    virtual bool Initialize() override;
    virtual bool Start()      override;
    virtual void Stop()       override;
    virtual void Housekeeping()override {}

    // Setup OSD page
    virtual cMenuSetupPage *SetupMenu() override;
    virtual bool SetupParse(const char *Name, const char *Value) override;

    // CLI options
    virtual const char *CommandLineHelp() override;
    virtual bool ProcessArgs(int argc, char *argv[]) override;

private:
    cGstDevice *m_device      = nullptr;
    bool        m_gstInitDone = false;
};

// ============================================================
//  Destructor
// ============================================================
cPluginGstreamer::~cPluginGstreamer()
{
    if (m_gstInitDone)
        gst_deinit();
}

// ============================================================
//  Command-line help & argument parsing
// ============================================================
const char *cPluginGstreamer::CommandLineHelp()
{
    return
        "  -V SINK  --videosink=SINK   GStreamer video sink (default: autovideosink)\n"
        "  -A SINK  --audiosink=SINK   GStreamer audio sink (default: autoaudiosink)\n";
}

bool cPluginGstreamer::ProcessArgs(int argc, char *argv[])
{
    static struct option longOpts[] = {
        { "videosink", required_argument, nullptr, 'V' },
        { "audiosink", required_argument, nullptr, 'A' },
        { nullptr, 0, nullptr, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "V:A:", longOpts, nullptr)) != -1) {
        switch (c) {
        case 'V': GstConfig.VideoSink = optarg; break;
        case 'A': GstConfig.AudioSink = optarg; break;
        default:  return false;
        }
    }
    return true;
}

// ============================================================
//  Setup persistence  (setup.conf)
// ============================================================
cMenuSetupPage *cPluginGstreamer::SetupMenu()
{
    return new cGstMenuSetup();
}

bool cPluginGstreamer::SetupParse(const char *Name, const char *Value)
{
    if      (strcmp(Name, "VideoCodec")     == 0) { GstConfig.VideoCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "HardwareDecode") == 0) { GstConfig.HardwareDecode = atoi(Value) != 0; return true; }
    else if (strcmp(Name, "AudioCodec")     == 0) { GstConfig.AudioCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "AudioOffset")    == 0) { GstConfig.AudioOffset    = atoi(Value); return true; }
    else if (strcmp(Name, "Volume")         == 0) { GstConfig.Volume         = atoi(Value); return true; }
    else if (strcmp(Name, "VideoSink")      == 0) { GstConfig.VideoSink      = Value;       return true; }
    else if (strcmp(Name, "AudioSink")      == 0) { GstConfig.AudioSink      = Value;       return true; }
    else if (strcmp(Name, "OsdTimeout")     == 0) { GstConfig.OsdTimeout     = atoi(Value); return true; }
    return false;
}

// ============================================================
//  Plugin lifecycle
// ============================================================
bool cPluginGstreamer::Initialize()
{
    gst_init(nullptr, nullptr);
    m_gstInitDone = true;

    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    isyslog("[gstreamer] GStreamer %u.%u.%u initialized | plugin v%s",
        major, minor, micro, PLUGIN_VERSION);

    m_device = new cGstDevice();

    // Create the OSD manager
    GstOsd = new cGstOsd();
    GstOsd->SetTimeout(GstConfig.OsdTimeout);

    return true;
}

bool cPluginGstreamer::Start()
{
    if (!m_device)
        return false;

    if (!m_device->InitPipeline()) {
        esyslog("[gstreamer] ERROR: Pipeline initialisation failed!");
        return false;
    }
    isyslog("[gstreamer] Started: codec=%s hw=%d audioCodec=%s offset=%dms sink=%s/%s osdTimeout=%ds",
        GstConfig.VideoCodecName(), GstConfig.HardwareDecode,
        GstConfig.AudioCodecName(), GstConfig.AudioOffset,
        GstConfig.EffectiveVideoSink().c_str(), GstConfig.AudioSink.c_str(),
        GstConfig.OsdTimeout);
    return true;
}

void cPluginGstreamer::Stop()
{
    // Hide and destroy OSD before device
    if (GstOsd) {
        GstOsd->Hide();
        delete GstOsd;
        GstOsd = nullptr;
    }
    if (m_device) {
        m_device->DestroyPipeline();
        m_device = nullptr;
    }
    isyslog("[gstreamer] Stopped");
}

// ============================================================
//  VDR plugin registration
// ============================================================
VDRPLUGINCREATOR(cPluginGstreamer);

// ============================================================
//  cPluginGstreamer
// ============================================================
class cPluginGstreamer : public cPlugin
{
public:
    cPluginGstreamer() = default;
    virtual ~cPluginGstreamer();

    virtual const char *Version()        override { return PLUGIN_VERSION; }
    virtual const char *Description()    override { return tr(PLUGIN_DESCRIPTION); }
    virtual const char *MainMenuEntry()  override { return nullptr; }

    virtual bool Initialize() override;
    virtual bool Start()      override;
    virtual void Stop()       override;
    virtual void Housekeeping()override {}

    // Setup OSD page
    virtual cMenuSetupPage *SetupMenu() override;
    virtual bool SetupParse(const char *Name, const char *Value) override;

    // CLI options
    virtual const char *CommandLineHelp() override;
    virtual bool ProcessArgs(int argc, char *argv[]) override;

private:
    cGstDevice *m_device      = nullptr;
    bool        m_gstInitDone = false;
};

// ============================================================
//  Destructor
// ============================================================
cPluginGstreamer::~cPluginGstreamer()
{
    if (m_gstInitDone)
        gst_deinit();
}

// ============================================================
//  Command-line help & argument parsing
// ============================================================
const char *cPluginGstreamer::CommandLineHelp()
{
    return
        "  -V SINK  --videosink=SINK   GStreamer video sink (default: autovideosink)\n"
        "  -A SINK  --audiosink=SINK   GStreamer audio sink (default: autoaudiosink)\n";
}

bool cPluginGstreamer::ProcessArgs(int argc, char *argv[])
{
    static struct option longOpts[] = {
        { "videosink", required_argument, nullptr, 'V' },
        { "audiosink", required_argument, nullptr, 'A' },
        { nullptr, 0, nullptr, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "V:A:", longOpts, nullptr)) != -1) {
        switch (c) {
        case 'V': GstConfig.VideoSink = optarg; break;
        case 'A': GstConfig.AudioSink = optarg; break;
        default:  return false;
        }
    }
    return true;
}

// ============================================================
//  Setup persistence  (setup.conf)
// ============================================================
cMenuSetupPage *cPluginGstreamer::SetupMenu()
{
    return new cGstMenuSetup();
}

bool cPluginGstreamer::SetupParse(const char *Name, const char *Value)
{
    if      (strcmp(Name, "VideoCodec")     == 0) { GstConfig.VideoCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "HardwareDecode") == 0) { GstConfig.HardwareDecode = atoi(Value) != 0; return true; }
    else if (strcmp(Name, "AudioCodec")     == 0) { GstConfig.AudioCodec     = atoi(Value); return true; }
    else if (strcmp(Name, "AudioOffset")    == 0) { GstConfig.AudioOffset    = atoi(Value); return true; }
    else if (strcmp(Name, "Volume")         == 0) { GstConfig.Volume         = atoi(Value); return true; }
    else if (strcmp(Name, "VideoSink")      == 0) { GstConfig.VideoSink      = Value;       return true; }
    else if (strcmp(Name, "AudioSink")      == 0) { GstConfig.AudioSink      = Value;       return true; }
    return false;
}

// ============================================================
//  Plugin lifecycle
// ============================================================
bool cPluginGstreamer::Initialize()
{
    gst_init(nullptr, nullptr);
    m_gstInitDone = true;

    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    isyslog("[gstreamer] GStreamer %u.%u.%u initialized | plugin v%s",
        major, minor, micro, PLUGIN_VERSION);

    m_device = new cGstDevice();
    return true;
}

bool cPluginGstreamer::Start()
{
    if (!m_device)
        return false;

    if (!m_device->InitPipeline()) {
        esyslog("[gstreamer] ERROR: Pipeline initialisation failed!");
        return false;
    }
    isyslog("[gstreamer] Started: codec=%s hw=%d audioCodec=%s offset=%dms sink=%s/%s",
        GstConfig.VideoCodecName(), GstConfig.HardwareDecode,
        GstConfig.AudioCodecName(), GstConfig.AudioOffset,
        GstConfig.EffectiveVideoSink().c_str(), GstConfig.AudioSink.c_str());
    return true;
}

void cPluginGstreamer::Stop()
{
    if (m_device) {
        m_device->DestroyPipeline();
        m_device = nullptr;
    }
    isyslog("[gstreamer] Stopped");
}

// ============================================================
//  VDR plugin registration
// ============================================================
VDRPLUGINCREATOR(cPluginGstreamer);
