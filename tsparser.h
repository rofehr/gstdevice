#pragma once

/*
 * tsparser.h  –  MPEG-TS PES reassembler with 33-bit PTS extraction
 *
 * One instance per elementary stream (video or audio).
 * Feed 188-byte TS packets; the callback fires with each complete PES
 * payload together with its PTS converted to GStreamer nanoseconds.
 *
 * NOT thread-safe – callers must hold a mutex around Feed()/Flush()/Reset().
 */

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include <gst/gst.h>    // GST_CLOCK_TIME_NONE

static constexpr int     kTsPacketSize = 188;
static constexpr uint8_t kTsSyncByte   = 0x47u;
static constexpr size_t  kPesBufMax    = 4u * 1024u * 1024u;  // 4 MB guard

// Callback: payload pointer, byte length, PTS in nanoseconds
// (PTS == GST_CLOCK_TIME_NONE when the PES header carries no PTS)
using TsPayloadCb = std::function<void(const uint8_t *data, int len, uint64_t pts_ns)>;

// ─────────────────────────────────────────────────────────────────────────────
class cTsParser
{
public:
    explicit cTsParser(TsPayloadCb cb) : m_cb(std::move(cb))
    {
        m_buf.reserve(256 * 1024);
    }

    // Feed one 188-byte TS packet.
    // Returns false only when the sync byte is missing (corrupt packet).
    bool Feed(const uint8_t *pkt, int len = kTsPacketSize);

    // Dispatch the pending PES buffer (call before channel switch / Stop).
    void Flush();

    // Discard buffered data without dispatching (call after seek/error).
    void Reset();

private:
    TsPayloadCb          m_cb;
    std::vector<uint8_t> m_buf;
    bool                 m_started = false;

    void     Dispatch();
    uint64_t ExtractPts(const uint8_t *pes, int pesLen) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Inline implementations
// ─────────────────────────────────────────────────────────────────────────────

inline bool cTsParser::Feed(const uint8_t *pkt, int len)
{
    if (len < kTsPacketSize || pkt[0] != kTsSyncByte)
        return false;

    const bool pusi       = (pkt[1] & 0x40u) != 0;
    const bool hasAdapt   = (pkt[3] & 0x20u) != 0;
    const bool hasPayload = (pkt[3] & 0x10u) != 0;

    if (!hasPayload)
        return true;

    int payloadOff = 4;
    if (hasAdapt) {
        payloadOff += 1 + static_cast<int>(pkt[4]);
        if (payloadOff >= kTsPacketSize)
            return true;
    }

    const uint8_t *payload    = pkt + payloadOff;
    const int      payloadLen = kTsPacketSize - payloadOff;

    if (pusi) {
        if (m_started && !m_buf.empty())
            Dispatch();
        m_buf.clear();
        m_started = true;
    }

    if (!m_started)
        return true;

    if (m_buf.size() + static_cast<size_t>(payloadLen) <= kPesBufMax)
        m_buf.insert(m_buf.end(), payload, payload + payloadLen);

    return true;
}

inline void cTsParser::Dispatch()
{
    if (m_buf.size() < 9u)
        return;

    const uint8_t *pes = m_buf.data();
    const int      tot = static_cast<int>(m_buf.size());

    // PES start code 0x000001
    if (pes[0] != 0x00u || pes[1] != 0x00u || pes[2] != 0x01u)
        return;

    const uint64_t pts       = ExtractPts(pes, tot);
    const int      headerLen = static_cast<int>(pes[8]);
    const int      esOffset  = 9 + headerLen;

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

inline uint64_t cTsParser::ExtractPts(const uint8_t *pes, int pesLen) const
{
    // Need at least 14 bytes: 6 fixed + 3 optional + 5 PTS bytes
    if (pesLen < 14)
        return GST_CLOCK_TIME_NONE;

    const uint8_t ptsDtsFlags = (pes[7] >> 6u) & 0x03u;
    if (ptsDtsFlags == 0x00u)
        return GST_CLOCK_TIME_NONE;

    const uint8_t *p = pes + 9;  // first byte of optional PES header fields

    // ISO 13818-1 §2.4.3.7 – 33-bit PTS encoded in 5 bytes
    const uint64_t pts90 =
        (static_cast<uint64_t>(p[0] & 0x0Eu) << 29u) |
        (static_cast<uint64_t>(p[1]         ) << 22u) |
        (static_cast<uint64_t>(p[2] & 0xFEu) << 14u) |
        (static_cast<uint64_t>(p[3]         ) <<  7u) |
        (static_cast<uint64_t>(p[4] & 0xFEu) >>  1u);

    // 90 kHz ticks → nanoseconds:  ns = pts90 * 1e9 / 90000 = pts90 * 100000 / 9
    return (pts90 * 100000ULL) / 9ULL;
}
