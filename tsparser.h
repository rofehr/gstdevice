#pragma once

/*
 * cTsParser
 * Reassembles PES frames from 188-byte MPEG-TS packets and extracts
 * the 33-bit PTS, converting it to GStreamer nanoseconds.
 *
 * One instance per elementary stream (video / audio).
 * Not thread-safe – callers must serialise access externally.
 */

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include <gst/gst.h>   // for GST_CLOCK_TIME_NONE

static constexpr int     kTsPacketSize = 188;
static constexpr uint8_t kTsSyncByte   = 0x47;
static constexpr size_t  kPesBufMax    = 4 * 1024 * 1024;   // 4 MB

// Callback: (es_data, es_len, pts_in_nanoseconds)
// pts == GST_CLOCK_TIME_NONE when no PTS present in PES header
using TsPayloadCb = std::function<void(const uint8_t *data, int len, uint64_t pts_ns)>;

// ============================================================
class cTsParser
{
public:
    explicit cTsParser(TsPayloadCb cb) : m_cb(std::move(cb))
    {
        m_buf.reserve(256 * 1024);
    }

    // Feed exactly one 188-byte TS packet
    bool Feed(const uint8_t *pkt, int len = kTsPacketSize);

    // Dispatch pending PES buffer (call on channel switch / Clear)
    void Flush();

    // Discard all state without dispatching
    void Reset();

private:
    TsPayloadCb          m_cb;
    std::vector<uint8_t> m_buf;
    bool                 m_started = false;

    void     Dispatch();
    uint64_t ExtractPts(const uint8_t *pes, int len);
};

// ---- Inline implementation ----

inline bool cTsParser::Feed(const uint8_t *pkt, int len)
{
    if (len < kTsPacketSize || pkt[0] != kTsSyncByte)
        return false;

    bool pusi       = (pkt[1] & 0x40) != 0;
    bool hasAdapt   = (pkt[3] & 0x20) != 0;
    bool hasPayload = (pkt[3] & 0x10) != 0;

    if (!hasPayload)
        return true;

    int payloadOffset = 4;
    if (hasAdapt) {
        payloadOffset += 1 + pkt[4];
        if (payloadOffset >= kTsPacketSize)
            return true;
    }

    const uint8_t *payload    = pkt + payloadOffset;
    int            payloadLen = kTsPacketSize - payloadOffset;

    if (pusi) {
        if (m_started && !m_buf.empty())
            Dispatch();
        m_buf.clear();
        m_started = true;
    }

    if (!m_started)
        return true;

    if (m_buf.size() + payloadLen <= kPesBufMax)
        m_buf.insert(m_buf.end(), payload, payload + payloadLen);

    return true;
}

inline void cTsParser::Dispatch()
{
    if (m_buf.size() < 9)
        return;

    const uint8_t *pes = m_buf.data();
    int            tot = static_cast<int>(m_buf.size());

    // Verify PES start code 0x000001
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01)
        return;

    uint64_t pts        = ExtractPts(pes, tot);
    int      headerLen  = pes[8];
    int      esOffset   = 9 + headerLen;

    if (esOffset >= tot)
        return;

    m_cb(pes + esOffset, tot - esOffset, pts);
}

inline void cTsParser::Flush()
{
    if (m_started && !m_buf.empty())
        Dispatch();
    m_buf.clear();
    m_started = false;
}

inline void cTsParser::Reset()
{
    m_buf.clear();
    m_started = false;
}

inline uint64_t cTsParser::ExtractPts(const uint8_t *pes, int len)
{
    if (len < 14)
        return GST_CLOCK_TIME_NONE;

    uint8_t ptsDtsFlags = (pes[7] >> 6) & 0x03;
    if (ptsDtsFlags == 0x00)
        return GST_CLOCK_TIME_NONE;

    const uint8_t *p = pes + 9;

    uint64_t pts90 =
        (static_cast<uint64_t>(p[0] & 0x0E) << 29) |
        (static_cast<uint64_t>(p[1])         << 22) |
        (static_cast<uint64_t>(p[2] & 0xFE)  << 14) |
        (static_cast<uint64_t>(p[3])          <<  7) |
        (static_cast<uint64_t>(p[4] & 0xFE)   >>  1);

    // 90 kHz ticks → nanoseconds
    return (pts90 * 100000ULL) / 9ULL;
}
