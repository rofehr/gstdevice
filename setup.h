#pragma once

#include <vdr/menuitems.h>
#include "config.h"

// ============================================================
//  cGstMenuSetup – plugin setup page (VDR OSD)
//
//  VDR 2.7.7 API notes:
//    • cMenuEditStrItem no longer takes an 'allowed' character
//      set parameter – the signature is now:
//        cMenuEditStrItem(const char *Name, char *Value, int Length)
//    • cMenuEditStraItem is unchanged
//    • cMenuEditBoolItem, cMenuEditIntItem unchanged
// ============================================================
class cGstMenuSetup : public cMenuSetupPage
{
public:
    cGstMenuSetup();

protected:
    virtual void    Store() override;
    virtual eOSState ProcessKey(eKeys Key) override;

private:
    void BuildMenu();
    void RebuildIfNeeded();

    // Local working copies committed on Store()
    int  m_videoCodec;
    int  m_hwDecode;
    int  m_audioCodec;
    int  m_audioOffset;
    int  m_volume;
    int  m_osdTimeout;
    char m_videoSink[64];
    char m_audioSink[64];

    // Change detection for live menu rebuild
    int  m_prevVideoCodec;
    int  m_prevHwDecode;

    static const char * const kVideoCodecNames[];
    static const char * const kAudioCodecNames[];
};
