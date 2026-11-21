#include "dsh/game/mission_wire.h"

#include "dsh/net/bytes.h"

#include <string>
#include <vector>

namespace dsh::game {

std::vector<std::uint8_t> encode_mission_list(const std::vector<MissionListEntry>& entries,
                                              std::string& note) {
    note.clear();
    dsh::net::ByteWriter w;

    std::uint16_t written = 0;
    std::uint32_t skippedEmpty = 0;
    std::uint32_t skippedLong = 0;
    std::uint32_t skippedState = 0;
    std::uint32_t skippedOverflow = 0;

    for (const MissionListEntry& e : entries) {
        if (e.id.empty()) { ++skippedEmpty; continue; }
        if (e.id.size() > kMaxMissionIdLength) { ++skippedLong; continue; }
        if (e.state > 2) { ++skippedState; continue; }
        if (written >= kMaxMissionListEntries) { ++skippedOverflow; continue; }
        ++written;
    }

    w.u8(written == 0 ? 0 : 1);
    w.u16(written);
    std::uint16_t remaining = written;
    std::uint32_t payloadBytes = 2;
    for (const MissionListEntry& e : entries) {
        if (remaining == 0) break;
        if (e.id.empty() || e.id.size() > kMaxMissionIdLength || e.state > 2) continue;
        w.u8(static_cast<std::uint8_t>(e.id.size()));
        w.raw(e.id.data(), e.id.size());
        w.u8(e.state);
        payloadBytes += 2 + static_cast<std::uint32_t>(e.id.size());
        --remaining;
    }
    note = std::to_string(payloadBytes) + " byte(s) encoded";
    if (skippedEmpty != 0) note += ", " + std::to_string(skippedEmpty) + " skipped for having no id";
    if (skippedLong != 0) {
        note += ", " + std::to_string(skippedLong) + " skipped for an id longer than " +
                std::to_string(static_cast<unsigned>(kMaxMissionIdLength)) + " byte(s)";
    }
    if (skippedState != 0) note += ", " + std::to_string(skippedState) + " skipped for a state above 2";
    if (skippedOverflow != 0) {
        note += ", " + std::to_string(skippedOverflow) + " skipped for the cap of " +
                std::to_string(static_cast<unsigned>(kMaxMissionListEntries)) + " entries";
    }
    return w.take();
}

bool decode_mission_list(std::span<const std::uint8_t> bytes,
                         std::vector<MissionListEntry>& out, bool& noneReported, std::string& note) {
    note.clear();
    noneReported = false;

    dsh::net::ByteReader r(bytes);
    std::uint8_t holdsRecords = 0;
    if (!r.u8(holdsRecords)) {
        note = "the payload is empty (not even the holds-records flag)";
        return false;
    }
    if (holdsRecords > 1) {
        note = "the payload''s first byte (" + std::to_string(holdsRecords) +
               ") is neither 0 (no records) nor 1 (records follow)";
        return false;
    }
    std::uint16_t count = 0;
    if (!r.u16(count)) {
        note = "the payload is shorter than the 2-byte count";
        return false;
    }
    if (holdsRecords == 0 && count != 0) {
        note = "the payload says THIS SAVE HOLDS NO RECORD but also claims " + std::to_string(count) +
               " entr(ies) -- REFUSED, it contradicts itself";
        return false;
    }
    if (count > kMaxMissionListEntries) {
        note = "the payload claims " + std::to_string(count) + " entries, above the cap of " +
               std::to_string(static_cast<unsigned>(kMaxMissionListEntries)) +
               " -- REFUSED rather than resized";
        return false;
    }

    std::vector<MissionListEntry> decoded;
    decoded.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint8_t len = 0;
        if (!r.u8(len)) {
            note = "the payload ended before entry " + std::to_string(i) + "'s id length";
            return false;
        }
        if (len == 0) {
            note = "entry " + std::to_string(i) + " has a zero-length id";
            return false;
        }
        if (len > kMaxMissionIdLength) {
            note = "entry " + std::to_string(i) + "'s id length (" + std::to_string(len) +
                   ") is above the cap of " + std::to_string(static_cast<unsigned>(kMaxMissionIdLength));
            return false;
        }
        MissionListEntry e;
        e.id.resize(len);
        if (!r.raw(e.id.data(), len)) {
            note = "the payload ended inside entry " + std::to_string(i) + "'s id";
            return false;
        }
        std::uint8_t state = 0;
        if (!r.u8(state)) {
            note = "the payload ended before entry " + std::to_string(i) + "'s state";
            return false;
        }
        if (state > 2) {
            note = "entry " + std::to_string(i) + " (" + e.id + ") carries state " + std::to_string(state) +
                   ", which is not one of the three EDAMissionState values";
            return false;
        }
        e.state = state;
        decoded.push_back(std::move(e));
    }

    if (r.remaining() != 0) {
        note = "the payload decoded " + std::to_string(count) + " entr(ies) but " +
               std::to_string(r.remaining()) + " byte(s) are left over -- REFUSED";
        return false;
    }
    out = std::move(decoded);
    noneReported = (holdsRecords == 0);
    note = noneReported ? std::string("the sender reports NO record at all")
                        : (std::to_string(out.size()) + " entr(ies) decoded");
    return true;
}

}
