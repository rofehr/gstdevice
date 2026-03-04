#pragma once

#include <vdr/menuitems.h>
#include "config.h"

// ============================================================
//  cGstMenuSetup – VDR OSD setup page for the GStreamer plugin
// ============================================================
class cGstMenuSetup : public cMenuSetupPage
{
public:
    cGstMenuSetup();

protected:
    virtual void Store() override;

private:
    void BuildMenu();
    void RebuildMenu();
    virtual eOSState ProcessKey(eKeys Key) override;

    // Local copies of all settings (committed on Store())
    int         m_videoCodec;
    int         m_hardwareDecode;
    int         m_audioCodec;
    int         m_audioOffset;
    int         m_volume;
    int         m_osdTimeout;

    char        m_videoSink[64];
    char        m_audioSink[64];

    // For detecting codec/HW changes that require menu rebuild
    int         m_prevVideoCodec;
    int         m_prevHardwareDecode;

    // Static option arrays for AddEdit calls
    static const char * const kVideoCodecNames[];
    static const char * const kAudioCodecNames[];
};
