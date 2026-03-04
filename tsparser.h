#pragma once

// ============================================================
//  cTsParser
//
//  Parses 188-byte MPEG-TS packets as delivered by VDR's
//  PlayTs() / PlayTsVideo() / PlayTsAudio() callbacks.
//
//  For each complete PES packet it calls the registered
//  callback with:
//    - the raw PES payload (ES data)
//    - the PTS extracted from the PES header (or GST_CLOCK_TIME_NONE)
//    - a flag indicating video vs. audio
//
//  Design:
//   • One cTsParser instance per elementary stream (video / audio).
//   • Internally reassembles fragmented PES packets across multiple
//     TS packets using a flat byte buffer.
//   • Thread-safe via external locking in cGstDevice (m_mutex).
// ============================================================

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

// MPEG-TS constants
static constexpr int    kTsPacketSize   = 188;
static constexpr uint8_t kTsSyncByte   = 0x47;

// Maximum PES reassembly buffer (4 MB – one complete I-frame for H.265 4K)
static constexpr size_t kTsBufMax      = 4 * 1024 * 1024;

// Callback: (payload_data, payload_len, pts_ns)
//   pts_ns == GST_CLOCK_TIME_NONE when no PTS present in PES header
using TsPayloadCb = std::function<void(const uint8_t *data, int len, uint64_t pts_ns)>;

// ============================================================
class cTsParser
{
public:
    explicit cTsParser(TsPayloadCb cb) : m_cb(std::move(cb))
    {
        m_buf.reserve(256 * 1024);
    }

    // Feed one 188-byte TS packet.
    // Returns false if the packet is malformed / sync lost.
    bool Feed(const uint8_t *pkt, int len = kTsPacketSize);

    // Flush any pending incomplete PES (e.g. on channel change / Clear())
    void Flush();

    // Reset parser state (e.g. after pipeline rebuild)
    void Reset();

private:
    TsPayloadCb          m_cb;
    std::vector<uint8_t> m_buf;          // PES reassembly buffer
    bool                 m_started = false; // saw first PUSI

    // Parse PTS from PES header; returns GST_CLOCK_TIME_NONE if absent.
    static uint64_t ExtractPts(const uint8_t *pes, int len);

    // Dispatch completed PES buffer to callback
    void Dispatch();
};

// ============================================================
//  Inline implementation (header-only for simplicity)
// ============================================================

inline bool cTsParser::Feed(const uint8_t *pkt, int len)
{
    if (len < kTsPacketSize || pkt[0] != kTsSyncByte)
        return false;   // sync error

    // TS header fields
    bool  pusi        = (pkt[1] & 0x40) != 0;   // payload_unit_start_indicator
    bool  hasAdapt    = (pkt[3] & 0x20) != 0;
    bool  hasPayload  = (pkt[3] & 0x10) != 0;

    if (!hasPayload)
        return true;   // adaptation-only packet, nothing to do

    // Skip adaptation field
    int payloadOffset = 4;
    if (hasAdapt) {
        int adaptLen = pkt[4];
        payloadOffset += 1 + adaptLen;
        if (payloadOffset >= kTsPacketSize)
            return true;   // no room for payload
    }

    const uint8_t *payload    = pkt + payloadOffset;
    int            payloadLen = kTsPacketSize - payloadOffset;

    if (pusi) {
        // New PES starts here – dispatch previous one first
        if (m_started && !m_buf.empty())
            Dispatch();

        m_buf.clear();
        m_started = true;
    }

    if (!m_started)
        return true;   // skip until first PUSI

    // Append payload to reassembly buffer (guard against runaway streams)
    if (m_buf.size() + payloadLen <= kTsBufMax) {
        m_buf.insert(m_buf.end(), payload, payload + payloadLen);
    }

    return true;
}

inline void cTsParser::Dispatch()
{
    if (m_buf.size() < 9)
        return;   // too short to be a valid PES header

    const uint8_t *pes = m_buf.data();
    int            tot = (int)m_buf.size();

    // Verify PES start code prefix (0x000001)
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01)
        return;

    uint64_t pts = ExtractPts(pes, tot);

    // PES header data length is in byte [8]; ES payload starts after that
    int headerDataLen  = pes[8];
    int esOffset       = 9 + headerDataLen;

    if (esOffset >= tot)
        return;   // no ES payload

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

// ============================================================
//  PTS extraction from PES header
//
//  PES header layout (simplified):
//    [0..2]  start code 0x000001
//    [3]     stream_id
//    [4..5]  PES_packet_length
//    [6]     flags byte 1
//    [7]     flags byte 2  – bit7=PTS_DTS_flags[1], bit6=[0]
//    [8]     header_data_length
//    [9..]   optional fields (PTS/DTS first if present)
//
//  PTS is a 33-bit value encoded in 5 bytes:
//    byte0: 0b0010_PPP1   (or 0b0011 if DTS also present)
//    byte1: PPPPPPPP
//    byte2: PPPPPPP1
//    byte3: PPPPPPPP
//    byte4: PPPPPPP1
// ============================================================
inline uint64_t cTsParser::ExtractPts(const uint8_t *pes, int len)
{
    // Need at least 14 bytes for PTS to be present
    if (len < 14)
        return GST_CLOCK_TIME_NONE;

    uint8_t ptsDtsFlags = (pes[7] >> 6) & 0x03;
    if (ptsDtsFlags == 0x00)
        return GST_CLOCK_TIME_NONE;   // no PTS in this PES

    const uint8_t *p = pes + 9;   // start of optional PES fields

    // Decode 33-bit PTS
    uint64_t pts90 =
        ((uint64_t)(p[0] & 0x0E) << 29) |
        ((uint64_t)(p[1])        << 22) |
        ((uint64_t)(p[2] & 0xFE) << 14) |
        ((uint64_t)(p[3])        <<  7) |
        ((uint64_t)(p[4] & 0xFE) >>  1);

    // Convert from 90 kHz ticks → GStreamer nanoseconds
    // pts_ns = pts90 * 1_000_000_000 / 90_000
    //        = pts90 * 100_000 / 9
    return (pts90 * 100000ULL) / 9ULL;
}
