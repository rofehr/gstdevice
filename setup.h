#pragma once

/*
 * setup.h  –  VDR plugin setup page
 *
 * VDR 2.6+ API used:
 *   cMenuEditStrItem(name, buf, len)   – no 'allowed' charset parameter
 *   cMenuEditStraItem                  – string-array selector (unchanged)
 *   cMenuEditBoolItem / cMenuEditIntItem – unchanged
 */

#include <vdr/menuitems.h>
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
class cGstMenuSetup : public cMenuSetupPage
{
public:
    cGstMenuSetup();

protected:
    virtual void     Store() override;
    virtual eOSState ProcessKey(eKeys Key) override;

private:
    void BuildMenu();
    void RebuildIfNeeded();

    // Working copies – committed to GstConfig only on Store()
    int  m_videoCodec;
    int  m_hwDecode;
    int  m_audioCodec;
    int  m_audioOffset;
    int  m_volume;
    int  m_osdTimeout;
    char m_videoSink[64];
    char m_audioSink[64];

    // Change-detection for live menu rebuild
    int  m_prevVideoCodec;
    int  m_prevHwDecode;

    static const char * const kVideoCodecNames[];
    static const char * const kAudioCodecNames[];
};
