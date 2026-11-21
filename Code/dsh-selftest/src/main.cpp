#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <array>
#include "dsh/game/appearance_wire.h"
#include "dsh/game/doll_stance.h"
#include "dsh/game/exclusive_mode.h"
#include "dsh/game/health_wire.h"
#include "dsh/game/ammo_spawn.h"
#include "dsh/game/item_wire.h"
#include "dsh/game/module_component.h"
#include "dsh/game/movement_input.h"
#include "dsh/game/movement_state.h"
#include "dsh/game/observed_watch.h"
#include "dsh/game/puppet_dolls.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dsh/net/bytes.h"
#include "dsh/net/discovery.h"
#include "dsh/net/packet.h"
#include "dsh/net/session.h"
#include "dsh/net/transport.h"

#include "dsh/reflect/enemies.h"

#include "dsh/reflect/reflection.h"

#include "dsh/reflect/target_model.h"
#include "dsh/reflect/health_core.h"
#include "dsh/reflect/render_scan.h"
#include "dsh/platform/log.h"
#include "dsh/platform/file_digest.h"
#include "dsh/sync/cadence.h"
#include "dsh/sync/interpolator.h"
#include "dsh/sync/playback_clock.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (condition) {
        std::printf(" [ok] %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    } else {
        ++g_failures;
        std::printf(" [FAIL] %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

void section(const char* name) {
    std::printf("\n=== %s ===\n", name);
}

std::string hex_offset(std::uintptr_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

struct WirePacket {

    dsh::net::Endpoint  from;
    dsh::net::Endpoint  to;
    std::vector<std::uint8_t> bytes;
    std::uint64_t       deliverAtMs = 0;
};

class HostileLink {
public:
    std::uint64_t oneWayDelayMs = 0;
    int           lossPercent = 0;
    bool          duplicateEveryPacket = false;
    int           reorderPercent = 0;
    std::uint64_t extraReorderDelayMs = 40;

    void send(const dsh::net::Endpoint& to, std::span<const std::uint8_t> bytes,
              std::uint64_t nowMs) {
        send_from(dsh::net::Endpoint{}, to, bytes, nowMs);
    }

    void send_from(const dsh::net::Endpoint& from, const dsh::net::Endpoint& to,
                   std::span<const std::uint8_t> bytes, std::uint64_t nowMs) {
        ++sent_;
        if (lossPercent > 0 && (dist_(rng_) % 100) < static_cast<unsigned>(lossPercent)) {
            ++lost_;
            return;
        }
        std::uint64_t delay = oneWayDelayMs;
        if (reorderPercent > 0 && (dist_(rng_) % 100) < static_cast<unsigned>(reorderPercent)) {
            delay += extraReorderDelayMs;
            ++reordered_;
        }
        WirePacket p;
        p.from = from;
        p.to = to;
        p.bytes.assign(bytes.begin(), bytes.end());
        p.deliverAtMs = nowMs + delay;
        inFlight_.push_back(std::move(p));
        if (duplicateEveryPacket) {
            WirePacket d = inFlight_.back();
            d.deliverAtMs += 1;
            inFlight_.push_back(std::move(d));
            ++duplicated_;
        }
    }

    void pump(std::uint64_t nowMs, std::vector<WirePacket>& out) {
        out.clear();
        for (std::size_t i = 0; i < inFlight_.size();) {
            if (inFlight_[i].deliverAtMs <= nowMs) {
                out.push_back(std::move(inFlight_[i]));
                inFlight_.erase(inFlight_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t lost() const noexcept { return lost_; }
    [[nodiscard]] std::uint64_t duplicated() const noexcept { return duplicated_; }
    [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }

private:
    std::vector<WirePacket> inFlight_;
    std::uint64_t sent_ = 0, lost_ = 0, duplicated_ = 0, reordered_ = 0;
    std::mt19937  rng_{20261013u};
    std::uniform_int_distribution<std::uint32_t> dist_{0, 99};
};

dsh::net::Endpoint ep(std::uint8_t tag) { return dsh::net::Endpoint{{tag}}; }

void test_quantisation() {
    section("quantisation");

    float worstPos = 0.0f;
    for (int i = 0; i <= 20000; ++i) {
        const float metres = -1600.0f + static_cast<float>(i) * 0.16f;
        const std::uint16_t q = dsh::net::quantise_position_axis(metres);
        const float back = dsh::net::dequantise_position_axis(q);
        worstPos = std::max(worstPos, std::abs(back - metres));
    }
    check(worstPos < 0.06f, "position error stays under 6 cm over the whole sweep",
          "worst=" + std::to_string(worstPos) + " m");

    check(dsh::net::quantise_position_axis(-99999.0f) == 0,
          "an out-of-range negative position clamps to the bottom of the range");
    check(dsh::net::quantise_position_axis(99999.0f) == 65535,
          "an out-of-range positive position clamps to the top of the range");

    float worstAngle = 0.0f;
    for (int i = 0; i <= 20000; ++i) {
        const float rad = static_cast<float>(i) * 0.0003141592f;
        const std::uint8_t q = dsh::net::quantise_angle(rad);
        float back = dsh::net::dequantise_angle(q);
        float diff = std::abs(back - rad);
        if (diff > 3.14159265f) diff = 6.2831853f - diff;
        worstAngle = std::max(worstAngle, diff);
    }
    check(worstAngle < 0.0125f, "angle error stays at the half-step budget (0.72 deg)",
          "worst=" + std::to_string(worstAngle) + " rad");

    const std::uint8_t qa = dsh::net::quantise_angle(6.2831853f - 0.001f);
    const std::uint8_t qb = dsh::net::quantise_angle(0.001f);
    check(qa == 255, "an angle just below 2*pi maps to the top bucket");
    check(qb == 0, "an angle just above zero maps to the bottom bucket");
    check(qa != qb, "the two sides of the wrap point do NOT share a bucket");

    check(dsh::net::quantise_angle(-0.1f) == dsh::net::quantise_angle(6.2831853f - 0.1f),
          "negative angles wrap instead of clamping");
}

void test_interpolator() {
    section("interpolator");

    dsh::sync::InterpolatorConfig cfg;
    dsh::sync::PoseInterpolator interp(cfg);

    dsh::sync::PoseSample a;
    a.samplerTimeMs = 1000;
    a.x = 0.0f;
    a.yaw = 0.0f;
    interp.push(a);

    dsh::sync::PoseSample b;
    b.samplerTimeMs = 1100;
    b.x = 10.0f;
    b.yaw = 1.0f;
    interp.push(b);

    const auto mid = interp.sample(1050, 1100);
    check(mid.mode == dsh::sync::PoseOutput::Mode::interpolated,
          "a render time between two samples interpolates",
          "mode=" + std::to_string(static_cast<int>(mid.mode)));
    check(mid.x > 0.0f && mid.x < 10.0f, "and the position lies strictly between them",
          "x=" + std::to_string(mid.x));

    const auto beyondCap = interp.sample(1100 + 100000, 1100 + 100000);
    check(beyondCap.mode == dsh::sync::PoseOutput::Mode::frozen,
          "beyond the extrapolation cap the pose is FROZEN, not flown forward forever",
          "mode=" + std::to_string(static_cast<int>(beyondCap.mode)));
    check(beyondCap.x == 10.0f, "and it is frozen at the last known pose, not an invented one",
          "x=" + std::to_string(beyondCap.x));

    dsh::sync::PoseInterpolator stale(cfg);
    dsh::sync::PoseSample old;
    old.samplerTimeMs = 1000;
    old.x = 1.0f;
    stale.push(old);
    const auto aged = stale.sample(5000, 5000);
    check(aged.mode != dsh::sync::PoseOutput::Mode::noData,
          "a peer that stopped sending is NOT reported as 'no data' (it did send once)",
          "mode=" + std::to_string(static_cast<int>(aged.mode)));
    check(aged.newestSampleAgeMs >= 3900.0f, "and its age reflects the real elapsed time",
          "ageMs=" + std::to_string(aged.newestSampleAgeMs));

    dsh::sync::PoseInterpolator empty(cfg);
    const auto none = empty.sample(1000, 1000);
    check(none.mode == dsh::sync::PoseOutput::Mode::noData,
          "an interpolator with no samples reports noData");
    check(!none.renderable(), "and says it is not renderable");

    check(interp.stats().interpolatedFrames >= 1, "interpolated frames are counted");
    check(interp.stats().frozenFrames >= 1, "frozen frames are counted");
    check(stale.stats().staleFrames >= 1 || aged.mode != dsh::sync::PoseOutput::Mode::noData,
          "staleness is accounted rather than hidden");
}

void test_playback_clock() {
    section("playback clock");

    const auto drive = [](dsh::sync::PlaybackClock& clock, bool& monotone, std::uint64_t& worstGapMs) {
        std::uint64_t previous = 0;
        monotone = true;
        worstGapMs = 0;
        for (int i = 0; i < 60; ++i) {

            const std::uint64_t stamp = 1000 + static_cast<std::uint64_t>(i) * 50;
            const std::uint64_t arrival = 1000 + static_cast<std::uint64_t>(i) * 62;
            clock.note_arrival(stamp, arrival);
            for (int step = 1; step <= 4; ++step) {
                const auto frame = clock.frame(arrival + static_cast<std::uint64_t>(step) * 15, 100);
                if (previous != 0 && frame.renderMs < previous) monotone = false;
                if (previous != 0 && frame.renderMs > previous) {
                    const std::uint64_t gap = frame.renderMs - previous;
                    if (gap > worstGapMs) worstGapMs = gap;
                }
                previous = frame.renderMs;
            }
        }
    };

    {
        dsh::sync::PlaybackClock clock;
        bool monotone = false;
        std::uint64_t worstGapMs = 0;
        drive(clock, monotone, worstGapMs);
        const auto stats = clock.stats();
        check(monotone, "the render time never goes backwards while the doll is moving");
        check(stats.backSteps == 0,
              "and no advance had to step it back at all",
              "backSteps=" + std::to_string(stats.backSteps));
        check(clock.rate() > 0.75 && clock.rate() < 0.85,
              "the sender's own rate is measured from its stamps (50 ms of stamp per 62 ms of real time)",
              "rate=" + std::to_string(clock.rate()));
        check(stats.worstBackwardMs <= 3,
              "and the pull-back the old formula asked for has cancelled to estimator noise",
              "worstBackwardMs=" + std::to_string(static_cast<long long>(stats.worstBackwardMs)));
        check(worstGapMs <= 25,
              "the render time advances smoothly instead of in whole cadence jumps",
              "worstGapMs=" + std::to_string(static_cast<unsigned long long>(worstGapMs)));
    }

    {

        dsh::sync::PlaybackClock::Config pinned;
        pinned.minRate = 1.0;
        pinned.maxRate = 1.0;
        dsh::sync::PlaybackClock clock(pinned);
        bool monotone = false;
        std::uint64_t worstGapMs = 0;
        drive(clock, monotone, worstGapMs);
        const auto stats = clock.stats();
        check(stats.backwardRequests >= 50,
              "control: with the arrival term advanced at 1x, almost every arrival asks to go backward",
              "backwardRequests=" + std::to_string(stats.backwardRequests));
        check(stats.worstBackwardMs >= 10,
              "control: and the pull-back is the cadence error itself, not noise",
              "worstBackwardMs=" + std::to_string(static_cast<long long>(stats.worstBackwardMs)));
        check(stats.worstStepBackMs > 0,
              "control: the clock can only give back what the bounded correction allows per advance",
              "worstStepBackMs=" + std::to_string(static_cast<long long>(stats.worstStepBackMs)));
    }

    {

        dsh::sync::PlaybackClock clock;
        clock.note_arrival(1000, 5000);
        const auto first = clock.frame(5000, 100);
        clock.note_arrival(1050, 5150);
        const auto afterStall = clock.frame(5160, 100);
        check(afterStall.renderMs >= first.renderMs,
              "a 150 ms sender stall does not move the render time backwards",
              "before=" + std::to_string(first.renderMs) + " after=" + std::to_string(afterStall.renderMs));
        clock.note_arrival(1100, 9000);
        clock.frame(9010, 100);
        check(clock.stats().resyncs >= 1,
              "a multi-second gap is a resync, not a correction",
              "resyncs=" + std::to_string(clock.stats().resyncs));
    }

    {
        dsh::sync::PlaybackClock clock;
        const auto none = clock.frame(1000, 100);
        check(none.renderMs == 0 && none.samplerNowMs == 0,
              "a clock that has never seen a sample asks for nothing (the interpolator answers noData)");
    }
}

void test_tick_grid() {
    section("tick grid (the send cadence, v661)");

    const std::uint64_t epoch = 1000;
    const std::uint64_t interval = 50;
    const std::uint64_t spanMs = 10000;

    for (const std::uint64_t periodUs : {4500ull, 5480ull, 6061ull, 7900ull}) {
        std::uint64_t gridTicks = 0;
        std::uint64_t gridSkipped = 0;
        std::uint64_t lastIndex = 0;
        std::uint64_t oldTicks = 0;
        std::uint64_t oldDeadline = epoch;
        const std::uint64_t lastIndexInWindow = spanMs / interval;

        for (std::uint64_t tUs = 0; tUs <= spanMs * 1000 + periodUs; tUs += periodUs) {
            const std::uint64_t now = epoch + tUs / 1000;
            const std::uint64_t index = dsh::sync::tick_index(epoch, now, interval);
            if (index > lastIndexInWindow) break;
            if (index > lastIndex) {
                gridSkipped += dsh::sync::ticks_skipped(lastIndex, index);
                lastIndex = index;
                ++gridTicks;
            }
            if (now >= oldDeadline) {
                ++oldTicks;
                oldDeadline = now + interval;
            }
        }
        const std::string at = "poll " + std::to_string(periodUs) + " us";
        check(gridTicks == lastIndexInWindow,
              "a 10 s window on the 50 ms grid is 200 ticks at every poll period the game showed",
              at + " ticks=" + std::to_string(gridTicks));
        check(gridSkipped == 0,
              "and no poll period that fits inside one interval ever skips a grid point",
              at + " skipped=" + std::to_string(gridSkipped));
        check(oldTicks >= 175 && oldTicks <= 195,
              "control: the rule this replaced lands 10-20 ticks short, which is what 660-1 measured",
              at + " oldTicks=" + std::to_string(oldTicks));
    }

    {

        std::uint64_t lastIndex = dsh::sync::tick_index(epoch, epoch, interval);
        check(lastIndex == 0, "before the first interval no tick is due");
        std::uint64_t ticks = 0;
        std::uint64_t skipped = 0;
        for (const std::uint64_t now : {epoch + 55ull, epoch + 60ull, epoch + 1035ull, epoch + 1040ull}) {
            const std::uint64_t index = dsh::sync::tick_index(epoch, now, interval);
            if (index > lastIndex) {
                skipped += dsh::sync::ticks_skipped(lastIndex, index);
                lastIndex = index;
                ++ticks;
            }
        }
        check(ticks == 2,
              "a 985 ms stall produces ONE tick, not a catch-up burst",
              "ticks=" + std::to_string(ticks));
        check(skipped == 18,
              "and the 18 grid points it walked past are counted, not resent",
              "skipped=" + std::to_string(skipped));
    }

    {

        std::uint64_t lastIndex = 0;
        std::uint64_t ticks = 0;
        std::uint64_t skipped = 0;
        const std::uint64_t longSpanMs = 600000;
        for (std::uint64_t tUs = 0; tUs <= longSpanMs * 1000 + 5480; tUs += 5480) {
            const std::uint64_t index = dsh::sync::tick_index(epoch, epoch + tUs / 1000, interval);
            if (index > longSpanMs / interval) break;
            if (index > lastIndex) {
                skipped += dsh::sync::ticks_skipped(lastIndex, index);
                lastIndex = index;
                ++ticks;
            }
        }
        check(ticks == longSpanMs / interval,
              "ten minutes of 5.48 ms polls is exactly 12,000 ticks -- a grid does not drift",
              "ticks=" + std::to_string(ticks));
        check(skipped == 0, "and skips none of them", "skipped=" + std::to_string(skipped));
    }

    {

        check(dsh::sync::tick_index(epoch, epoch + 5000, 0) == 0,
              "a zero interval answers no tick instead of dividing by it");
        check(dsh::sync::tick_index(epoch, epoch, interval) == 0,
              "at the epoch no grid point has been reached");
        check(dsh::sync::tick_index(epoch, epoch + 49, interval) == 0,
              "one millisecond before the first deadline is still no tick");
        check(dsh::sync::tick_index(epoch, epoch + 50, interval) == 1,
              "on the deadline itself the first tick is due");
        check(dsh::sync::tick_index(epoch, epoch + 1000, interval) == 20,
              "a thousand milliseconds into the session the index is 20");
        check(dsh::sync::ticks_skipped(20, 21) == 0 && dsh::sync::ticks_skipped(20, 20) == 0,
              "consecutive ticks skip nothing, and asking twice skips nothing");
        check(dsh::sync::ticks_skipped(20, 23) == 2,
              "a jump of three indices is two skipped grid points");

        bool monotone = true;
        std::uint64_t previous = 0;
        for (std::uint64_t ms = 0; ms <= 2000; ms += 3) {
            const std::uint64_t index = dsh::sync::tick_index(epoch, epoch + ms, interval);
            if (index < previous) monotone = false;
            previous = index;
        }
        check(monotone, "the index never goes backwards as the clock advances");
    }
}

void test_pose_stamp_grid() {
    section("pose stamps are the sender's grid (v673)");

    const std::uint64_t interval = 50;
    const std::uint64_t stallTicks = 25;
    const std::uint64_t stallMs = (stallTicks + 1) * interval;
    const auto drive = [&](bool gridStamps, std::uint64_t& resyncs, std::int64_t& worstStepBack,
                           bool& monotone, std::uint64_t& lastStampGap) {
        dsh::sync::PlaybackClock clock;

        const std::uint64_t arrivalBase = 1000;
        std::uint64_t now = arrivalBase;
        std::uint64_t previousStamp = 0;
        monotone = true;
        lastStampGap = 0;
        for (std::uint64_t i = 0; i < 41; ++i) {

            const std::uint64_t gridPoint = (i < 20) ? (i + 1) : (i + 1 + stallTicks);
            const std::uint64_t stamp = (gridStamps ? gridPoint : i + 1) * interval;
            const std::uint64_t arrival = arrivalBase + (gridPoint - 1) * interval;
            while (now < arrival) {
                (void)clock.frame(now, 100);
                now += 15;
            }
            clock.note_arrival(stamp, arrival);
            (void)clock.frame(now, 100);
            now += 15;
            if (previousStamp != 0) {
                if (stamp <= previousStamp) monotone = false;
                if (i == 20) lastStampGap = stamp - previousStamp;
            }
            previousStamp = stamp;
        }
        (void)clock.frame(now, 100);
        resyncs = clock.stats().resyncs;
        worstStepBack = clock.stats().worstStepBackMs;
    };

    {
        std::uint64_t resyncs = 0;
        std::int64_t worstStepBack = 0;
        bool monotone = false;
        std::uint64_t lastStampGap = 0;
        drive(true, resyncs, worstStepBack, monotone, lastStampGap);
        check(monotone, "grid stamps never go backwards");
        check(lastStampGap == stallMs,
              "the sample after the stall is stamped by the time that actually passed, not by one interval",
              "stampGap=" + std::to_string(lastStampGap) + " expected=" + std::to_string(stallMs));
        check(resyncs == 0,
              "and the playback clock never has to resync for a stall the sender already waited out",
              "resyncs=" + std::to_string(resyncs));
        check(worstStepBack <= 2,
              "nor step the render time backwards (this is the 1,466 ms 672-1 measured)",
              "worstStepBackMs=" + std::to_string(static_cast<long long>(worstStepBack)));
    }
    {

        std::uint64_t resyncs = 0;
        std::int64_t worstStepBack = 0;
        bool monotone = false;
        std::uint64_t lastStampGap = 0;
        drive(false, resyncs, worstStepBack, monotone, lastStampGap);
        check(monotone, "control: a counter is still monotone, which is why this defect looked like a live stream");
        check(lastStampGap == interval,
              "control: the counter reports one interval for the whole stall",
              "stampGap=" + std::to_string(lastStampGap));
        check(resyncs >= 1,
              "control: and the playback clock snaps -- which is the regression this test exists to catch",
              "resyncs=" + std::to_string(resyncs));
        check(worstStepBack >= 500,
              "control: with a backward step the size of the stall",
              "worstStepBackMs=" + std::to_string(static_cast<long long>(worstStepBack)));
    }
}

void test_packet_envelope() {
    section("packet envelope");

    dsh::net::PacketHeader h;
    h.version = dsh::net::kProtocolVersion;
    h.type = dsh::net::MessageType::playerState;
    h.senderPeer = 3;
    h.flags = dsh::net::kFlagHasAck;
    h.sequence = 77;
    h.ackThrough = 41;
    h.ackSelective = 0x5;

    const std::vector<std::uint8_t> payload{1, 2, 3, 4};
    const auto packet = dsh::net::build_packet(h, payload);

    dsh::net::ByteReader r(packet);
    dsh::net::PacketHeader parsed;
    check(dsh::net::read_header(r, parsed), "a built header parses back");
    check(parsed.type == h.type && parsed.senderPeer == h.senderPeer && parsed.sequence == 77,
          "round-tripped fields match");
    check(parsed.ackThrough == 41 && parsed.ackSelective == 0x5, "ack fields round-trip");
    check(r.remaining() == payload.size(), "exactly the payload remains after the header",
          "remaining=" + std::to_string(r.remaining()));

    {
        dsh::net::ByteReader trunc(std::span<const std::uint8_t>(packet.data(), 5));
        dsh::net::PacketHeader ignored;
        check(!dsh::net::read_header(trunc, ignored), "a truncated header is refused");
        check(!trunc.ok(), "and the reader records the failure");
    }

    {
        auto wrong = packet;
        wrong[0] = 99;
        dsh::net::ByteReader wr(wrong);
        dsh::net::PacketHeader vh;
        check(!dsh::net::read_header(wr, vh), "a version mismatch is refused");
        check(vh.version == 99, "the offending version is reported, not swallowed",
              "version=" + std::to_string(vh.version));
    }

    {
        auto unknown = packet;
        unknown[2] = 0xEE;
        unknown[3] = 0xEE;
        dsh::net::ByteReader ur(unknown);
        dsh::net::PacketHeader uh;
        check(dsh::net::read_header(ur, uh), "an unknown message type still parses");
    }
}

struct Loop {
    HostileLink link;
    dsh::net::Session host;
    dsh::net::Session client;
    dsh::net::Endpoint hostEp = ep(1);
    dsh::net::Endpoint clientEp = ep(2);

    explicit Loop(std::uint8_t maxPlayers = 4) {
        dsh::net::SessionConfig hc;
        hc.maxPlayers = maxPlayers;
        hc.playerName = "host";
        host = dsh::net::Session(hc);
        host.become_host(dsh::PeerId{0});
        host.set_send_function([this](const dsh::net::Endpoint& to,
                                     std::span<const std::uint8_t> b) {
            link.send(to, b, nowMs_);
        });

        dsh::net::SessionConfig cc;
        cc.maxPlayers = maxPlayers;
        cc.playerName = "client";
        client = dsh::net::Session(cc);
        client.become_client(dsh::PeerId{1}, hostEp);
        client.set_send_function([this](const dsh::net::Endpoint& to,
                                       std::span<const std::uint8_t> b) {

            link.send(to, b, nowMs_);
        });
    }

    std::uint64_t nowMs_ = 0;
    std::int64_t  clientSkewMs = 0;

    void run_to(std::uint64_t endMs, std::uint64_t stepMs = 20) {
        std::vector<WirePacket> due;
        for (; nowMs_ <= endMs; nowMs_ += stepMs) {
            host.update(nowMs_);
            client.update(nowMs_ + (clientSkewMs > 0 ? static_cast<std::uint64_t>(clientSkewMs)
                                                     : 0));
            link.pump(nowMs_, due);
            for (auto& p : due) {
                if (p.to == hostEp) {
                    host.on_datagram(nowMs_, clientEp, p.bytes);
                } else if (p.to == clientEp) {
                    client.on_datagram(nowMs_ + (clientSkewMs > 0
                                                     ? static_cast<std::uint64_t>(clientSkewMs)
                                                     : 0),
                                       hostEp, p.bytes);
                }
            }
        }
    }
};

void test_session_handshake() {
    section("session: handshake and membership");

    Loop loop;
    loop.run_to(2000);

    check(loop.client.connected_count() == 1, "the client reaches 'connected'",
          loop.client.status_line(loop.nowMs_));
    check(loop.host.connected_count() == 1, "the host counts exactly one connected peer",
          loop.host.status_line(loop.nowMs_));
    check(loop.client.local_id() != dsh::kInvalidPeer,
          "the host assigned the client a real peer id");

    check(loop.host.peers().size() == 1, "exactly one peer was created, not several",
          "peers=" + std::to_string(loop.host.peers().size()));

    std::vector<dsh::net::SessionEvent> events;
    loop.host.take_events(events);
    int joins = 0;
    for (const auto& e : events) {
        if (e.kind == dsh::net::SessionEventKind::peerJoined) ++joins;
    }
    check(joins <= 1, "at most ONE join event was emitted for the single client",
          "joins=" + std::to_string(joins));

    const auto before = loop.host.peers().size();
    const std::vector<std::uint8_t> junk{9, 9, 9, 9};
    loop.host.on_datagram(loop.nowMs_, ep(77), junk);
    check(loop.host.peers().size() == before, "a junk datagram from a stranger adds no peer",
          "peers=" + std::to_string(loop.host.peers().size()));
}

void test_session_clock_sync() {
    section("session: clock synchronisation");

    Loop loop;
    loop.clientSkewMs = 5000;
    loop.link.oneWayDelayMs = 25;
    loop.run_to(6000);

    const auto rt = loop.client.remote_time(loop.nowMs_ + 5000);
    check(rt.valid, "the client has a valid clock estimate");
    const std::uint64_t hostNow = loop.nowMs_;
    const auto err = static_cast<std::int64_t>(rt.ms) - static_cast<std::int64_t>(hostNow);
    check(std::abs(err) <= 60, "the host's timeline is recovered within 60 ms",
          "error=" + std::to_string(err) + " ms, rtt=" + std::to_string(
              loop.client.peers().empty() ? 0 : loop.client.peers()[0].clockRttMs) + " ms");

    const auto offBefore = loop.client.peers().empty() ? 0
                                                       : loop.client.peers()[0].clockOffsetMs;
    const auto rttBefore = loop.client.peers().empty() ? 0
                                                       : loop.client.peers()[0].clockRttMs;
    loop.run_to(12000);
    const auto offAfter = loop.client.peers().empty() ? 0 : loop.client.peers()[0].clockOffsetMs;
    const auto rttAfter = loop.client.peers().empty() ? 0 : loop.client.peers()[0].clockRttMs;
    check(std::abs(offAfter - offBefore) <= 20,
          "the offset estimate is stable as more samples arrive (best-sample-wins)",
          "offset " + std::to_string(offBefore) + " -> " + std::to_string(offAfter) +
              " ms; rtt " + std::to_string(rttBefore) + " -> " + std::to_string(rttAfter) +
              " ms (expected offset near -5000)");
}

void test_session_timeout() {
    section("session: disconnect detection");

    Loop loop;
    loop.run_to(2000);
    check(loop.host.connected_count() == 1, "connected before the silence");

    loop.link.lossPercent = 100;
    loop.run_to(2000 + 4000);

    const auto* p = loop.host.find_peer(loop.host.peers().empty() ? dsh::kInvalidPeer
                                                                 : loop.host.peers()[0].id);
    check(p != nullptr && p->state == dsh::net::PeerState::timedOut,
          "the host marks a silent peer as timedOut",
          p ? std::string("state=") + dsh::net::to_string(p->state) : "no peer");
    check(loop.host.connected_count() == 0, "and it is no longer counted as connected");

    std::vector<dsh::net::SessionEvent> events;
    loop.host.take_events(events);
    bool sawTimeout = false;
    for (const auto& e : events) {
        if (e.kind == dsh::net::SessionEventKind::peerTimedOut) sawTimeout = true;
    }
    check(sawTimeout, "a timeout event was emitted, so the game layer can react");
}

void test_session_budget_alarm() {
    section("session: per-client bandwidth budget is an ALARM");

    Loop loop;
    loop.run_to(2000);

    const auto* hostPeer = loop.host.peers().empty() ? nullptr : &loop.host.peers()[0];
    check(hostPeer != nullptr && hostPeer->state == dsh::net::PeerState::connected,
          "peer is connected before the budget test");

    std::vector<std::uint8_t> blob(600, 0xAB);
    for (int i = 0; i < 60; ++i) {
        loop.host.send_state_to(hostPeer->id, static_cast<std::uint32_t>(i), blob);
        loop.nowMs_ += 10;
    }
    loop.host.update(loop.nowMs_);

    std::vector<dsh::net::SessionEvent> events;
    loop.host.take_events(events);
    bool alarmed = false;
    for (const auto& e : events) {
        if (e.kind == dsh::net::SessionEventKind::budgetExceeded) alarmed = true;
    }
    check(alarmed || loop.host.stats().budgetAlarms > 0,
          "exceeding 20 KB/s raises a REPORTED alarm rather than silently dropping",
          "alarms=" + std::to_string(loop.host.stats().budgetAlarms));
}

void test_session_under_hostile_link() {
    section("session: handshake under 30% loss + reorder + every packet duplicated");

    Loop loop;
    loop.link.lossPercent = 30;
    loop.link.duplicateEveryPacket = true;
    loop.link.reorderPercent = 30;
    loop.link.oneWayDelayMs = 20;
    loop.run_to(8000);

    check(loop.client.connected_count() == 1,
          "the client still completes the handshake under 30% loss and duplication",
          loop.client.status_line(loop.nowMs_));
    check(loop.host.connected_count() == 1, "the host agrees",
          loop.host.status_line(loop.nowMs_));

    std::vector<dsh::net::SessionEvent> events;
    loop.host.take_events(events);
    int joins = 0;
    for (const auto& e : events) {
        if (e.kind == dsh::net::SessionEventKind::peerJoined) ++joins;
    }
    check(joins == 1,
          "EXACTLY one join, even though the duplicate hellos arrived (a second join would "
          "spawn the player's body twice)",
          "joins=" + std::to_string(joins) + " duplicatesIgnored=" +
              std::to_string(loop.host.stats().duplicatesIgnored));

    check(loop.link.lost() > 0 && loop.link.duplicated() > 0,
          "loss and duplication really were injected (the test is not vacuous)",
          "sent=" + std::to_string(loop.link.sent()) + " lost=" +
              std::to_string(loop.link.lost()) + " dup=" +
              std::to_string(loop.link.duplicated()));
}

void test_session_full_server() {
    section("session: the host refuses a fifth player EXPLICITLY");

    Loop loop(2);
    loop.run_to(2000);
    check(loop.host.connected_count() == 1, "the first client joined");

    dsh::net::SessionConfig cc;
    cc.playerName = "late";
    dsh::net::Session late(cc);
    late.become_client(dsh::PeerId{2}, loop.hostEp);
    late.set_send_function([&loop](const dsh::net::Endpoint& to,
                                   std::span<const std::uint8_t> b) {
        loop.link.send(to, b, loop.nowMs_);
    });

    std::vector<WirePacket> due;
    for (std::uint64_t t = loop.nowMs_; t <= 4000; t += 20) {
        loop.nowMs_ = t;
        late.update(t);
        loop.link.pump(t, due);
        for (auto& p : due) {
            if (p.to == loop.hostEp) loop.host.on_datagram(t, ep(3), p.bytes);
            else if (p.to == ep(3)) late.on_datagram(t, loop.hostEp, p.bytes);
        }
    }

    check(loop.host.peers().size() <= 2, "the peer table never exceeded maxPlayers",
          "peers=" + std::to_string(loop.host.peers().size()));
    check(late.connected_count() == 0, "the late client did not join");

    std::vector<dsh::net::SessionEvent> events;
    loop.host.take_events(events);
    bool refusalReported = false;
    for (const auto& e : events) {
        if (e.kind == dsh::net::SessionEventKind::handshakeFailed) refusalReported = true;
    }
    check(refusalReported, "the refusal was REPORTED as an event with a reason");
}

struct Loop3 {
    HostileLink link;
    dsh::net::Session host;
    dsh::net::Session a;
    dsh::net::Session b;
    dsh::net::Endpoint hostEp = ep(1);
    dsh::net::Endpoint aEp = ep(2);
    dsh::net::Endpoint bEp = ep(3);
    std::vector<WirePacket> sentByA;
    std::vector<WirePacket> sentByB;
    std::vector<WirePacket> gotByHost;
    std::vector<WirePacket> gotByA;
    std::vector<WirePacket> gotByB;
    std::uint64_t nowMs_ = 0;

    Loop3() {
        dsh::net::SessionConfig hc;
        hc.maxPlayers = 4;
        hc.playerName = "host";
        host = dsh::net::Session(hc);
        host.become_host(dsh::PeerId{0});
        host.set_send_function([this](const dsh::net::Endpoint& to,
                                      std::span<const std::uint8_t> bytes) {
            link.send_from(hostEp, to, bytes, nowMs_);
        });

        dsh::net::SessionConfig ac;
        ac.maxPlayers = 4;
        ac.playerName = "client-a";
        a = dsh::net::Session(ac);
        a.become_client(dsh::PeerId{1}, hostEp);
        a.set_send_function([this](const dsh::net::Endpoint& to,
                                   std::span<const std::uint8_t> bytes) {
            WirePacket p;
            p.from = aEp;
            p.to = to;
            p.bytes.assign(bytes.begin(), bytes.end());
            sentByA.push_back(std::move(p));
            link.send_from(aEp, to, bytes, nowMs_);
        });

        dsh::net::SessionConfig bc;
        bc.maxPlayers = 4;
        bc.playerName = "client-b";
        b = dsh::net::Session(bc);
        b.become_client(dsh::PeerId{1}, hostEp);
        b.set_send_function([this](const dsh::net::Endpoint& to,
                                   std::span<const std::uint8_t> bytes) {
            WirePacket p;
            p.from = bEp;
            p.to = to;
            p.bytes.assign(bytes.begin(), bytes.end());
            sentByB.push_back(std::move(p));
            link.send_from(bEp, to, bytes, nowMs_);
        });
    }

    dsh::net::Session* session_for(const dsh::net::Endpoint& e) {
        if (e == hostEp) return &host;
        if (e == aEp) return &a;
        if (e == bEp) return &b;
        return nullptr;
    }

    void run_to(std::uint64_t endMs, std::uint64_t stepMs = 20, bool relay = false) {
        std::vector<WirePacket> due;
        for (; nowMs_ <= endMs; nowMs_ += stepMs) {
            host.update(nowMs_);
            a.update(nowMs_);
            b.update(nowMs_);
            link.pump(nowMs_, due);
            for (const auto& p : due) {
                dsh::net::Session* dst = session_for(p.to);
                if (dst == nullptr) continue;
                dst->on_datagram(nowMs_, p.from, p.bytes);
                if (dst == &host) gotByHost.push_back(p);
                else if (dst == &a) gotByA.push_back(p);
                else gotByB.push_back(p);
                if (relay && dst == &host) {
                    dsh::net::ByteReader r(p.bytes);
                    dsh::net::PacketHeader h;
                    if (!dsh::net::read_header(r, h)) continue;
                    if (h.type == dsh::net::MessageType::appearanceAck) {

                        const auto body = std::span<const std::uint8_t>(p.bytes).subspan(r.cursor());
                        if (body.size() == dsh::game::kAppearanceAckBytes && body[0] != 0) {
                            host.relay_to_peer(p.from, dsh::PeerId{body[0]}, p.bytes, nowMs_);
                        }
                    } else if (dsh::net::message_is_relayed(h.type)) {
                        host.relay_from_endpoint(p.from, p.bytes, nowMs_);
                    }
                }
            }
        }
    }
};

void test_session_three_ways() {
    section("session: three players -- every client stamps its OWN assigned number (v530)");

    Loop3 loop;
    loop.run_to(4000);

    check(loop.host.connected_count() == 2, "both clients joined the host",
          "connected=" + std::to_string(loop.host.connected_count()));
    check(loop.a.local_id().value == 1, "client A knows it is peer 1",
          "local_id=" + std::to_string(loop.a.local_id().value));
    check(loop.b.local_id().value == 2,
          "client B knows it is peer 2 -- NOT the provisional 1 it started with",
          "local_id=" + std::to_string(loop.b.local_id().value));

    auto last_state_sender = [](const std::vector<WirePacket>& sent) -> int {
        int id = -1;
        for (const WirePacket& p : sent) {
            dsh::net::ByteReader r(p.bytes);
            dsh::net::PacketHeader h;
            if (!dsh::net::read_header(r, h)) continue;
            if (h.type == dsh::net::MessageType::playerState) id = h.senderPeer;
        }
        return id;
    };
    loop.sentByA.clear();
    loop.sentByB.clear();
    loop.a.send_state_to(loop.a.peers().front().id, 501, std::span<const std::uint8_t>());
    loop.b.send_state_to(loop.b.peers().front().id, 502, std::span<const std::uint8_t>());
    loop.run_to(loop.nowMs_ + 40);
    check(last_state_sender(loop.sentByA) == 1, "A's position datagram says senderPeer 1");
    check(last_state_sender(loop.sentByB) == 2, "B's position datagram says senderPeer 2");

    check(loop.a.peers().front().id.value == 0, "client A's only peer is the host, id 0",
          "peer id=" + std::to_string(loop.a.peers().front().id.value));
    check(loop.b.peers().front().id.value == 0, "client B's only peer is the host, id 0",
          "peer id=" + std::to_string(loop.b.peers().front().id.value));
    loop.gotByHost.clear();
    loop.b.send_appearance_ack_to(dsh::PeerId{0}, 900, std::span<const std::uint8_t>());
    loop.run_to(loop.nowMs_ + 40);
    int ackFromB = -1;
    for (const WirePacket& p : loop.gotByHost) {
        dsh::net::ByteReader r(p.bytes);
        dsh::net::PacketHeader h;
        if (!dsh::net::read_header(r, h)) continue;
        if (h.type == dsh::net::MessageType::appearanceAck) ackFromB = h.senderPeer;
    }
    check(ackFromB == 2,
          "B's acknowledgement of the host's manifest REACHES the host, stamped with B's own id (2)",
          "senderPeer=" + std::to_string(ackFromB));

    std::vector<std::uint8_t> hello;
    {
        dsh::net::PacketHeader h;
        h.type = dsh::net::MessageType::hello;
        h.senderPeer = 9;
        const std::string name = "stranger";
        hello = dsh::net::build_packet(
            h, std::span<const std::uint8_t>(
                   reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    }
    const std::size_t clientsBefore = loop.a.peers().size();
    loop.a.on_datagram(loop.nowMs_, ep(9), hello);
    check(loop.a.peers().size() == clientsBefore,
          "a hello from a stranger adds NO peer on a client",
          "peers=" + std::to_string(loop.a.peers().size()));

    const std::size_t hostBefore = loop.host.peers().size();
    loop.host.on_datagram(loop.nowMs_, ep(9), hello);
    check(loop.host.peers().size() == hostBefore + 1,
          "the host DOES learn a new endpoint from a hello -- that is how anyone joins");
}

void test_session_relay() {
    section("session: the host forwards a client's datagram to the other client (v531)");

    check(dsh::net::message_is_relayed(dsh::net::MessageType::playerState),
          "a position datagram is relayed");
    check(dsh::net::message_is_relayed(dsh::net::MessageType::playerStateBatch),
          "an appearance manifest is relayed");
    check(dsh::net::message_is_relayed(dsh::net::MessageType::ammoSpawn),
          "a projectile is relayed");
    check(dsh::net::message_is_relayed(dsh::net::MessageType::playerHealth),
          "the player's own health is relayed");
    check(!dsh::net::message_is_relayed(dsh::net::MessageType::healthReport),
          "a damage report is NOT relayed -- it is for the host only");
    check(!dsh::net::message_is_relayed(dsh::net::MessageType::healthBroadcast),
          "an enemy-health broadcast is NOT relayed -- the host is its only writer");
    check(!dsh::net::message_is_relayed(dsh::net::MessageType::missionListRequest),
          "a mission-list request is NOT relayed -- it is a question to the host");
    check(!dsh::net::message_is_relayed(dsh::net::MessageType::appearanceAck),
          "an appearance ack is NOT broadcast -- it is routed to the manifest's owner");
    check(!dsh::net::message_is_relayed(dsh::net::MessageType::keepAlive),
          "a keepalive is NOT relayed -- membership is per link");

    Loop3 loop;
    loop.run_to(4000, 20, true);
    check(loop.host.connected_count() == 2, "two clients joined");

    const std::vector<std::uint8_t> payload(75, 0x5A);
    const std::uint32_t seq = 700;
    loop.gotByA.clear();
    loop.gotByB.clear();
    loop.a.send_state_to(loop.a.peers().front().id, seq,
                         std::span<const std::uint8_t>(payload.data(), payload.size()));
    loop.run_to(loop.nowMs_ + 60, 20, true);

    int seenByB = 0;
    int idSeenByB = -1;
    dsh::net::Endpoint fromSeenByB;
    std::vector<std::uint8_t> payloadSeenByB;
    for (const WirePacket& p : loop.gotByB) {
        dsh::net::ByteReader r(p.bytes);
        dsh::net::PacketHeader h;
        if (!dsh::net::read_header(r, h)) continue;
        if (h.type != dsh::net::MessageType::playerState || h.sequence != seq) continue;
        ++seenByB;
        idSeenByB = h.senderPeer;
        fromSeenByB = p.from;
        payloadSeenByB.assign(p.bytes.begin() + static_cast<std::ptrdiff_t>(r.cursor()),
                              p.bytes.end());
    }
    check(seenByB == 1, "B received the copied datagram exactly ONCE",
          "count=" + std::to_string(seenByB));
    check(idSeenByB == 1, "and it carries A's id (1) -- the id the HOST assigned",
          "senderPeer=" + std::to_string(idSeenByB));
    check(fromSeenByB == loop.hostEp, "and it arrived from the HOST's endpoint, not from A's");
    check(payloadSeenByB == payload, "and the payload is byte-identical to what A sent",
          "bytes=" + std::to_string(payloadSeenByB.size()));

    int seenByA = 0;
    for (const WirePacket& p : loop.gotByA) {
        dsh::net::ByteReader r(p.bytes);
        dsh::net::PacketHeader h;
        if (!dsh::net::read_header(r, h)) continue;
        if (h.type == dsh::net::MessageType::playerState && h.sequence == seq) ++seenByA;
    }
    check(seenByA == 0, "A did NOT get its own datagram back");

    std::vector<std::uint8_t> one;
    {
        dsh::net::PacketHeader h;
        h.type = dsh::net::MessageType::playerState;
        h.senderPeer = 1;
        h.sequence = 701;
        one = dsh::net::build_packet(h, std::span<const std::uint8_t>(payload.data(), payload.size()));
    }
    const std::uint64_t refusedOnA = loop.a.stats().relayRefused;
    loop.a.relay_from_endpoint(loop.aEp, one, loop.nowMs_);
    check(loop.a.stats().relayRefused == refusedOnA + 1,
          "a CLIENT refuses to relay -- the host is the only hub",
          "refused=" + std::to_string(loop.a.stats().relayRefused));

    const std::uint64_t refusedOnHost = loop.host.stats().relayRefused;
    loop.host.relay_from_endpoint(ep(9), one, loop.nowMs_);
    check(loop.host.stats().relayRefused == refusedOnHost + 1,
          "the host refuses a datagram from an endpoint it does not know");

    loop.gotByA.clear();
    loop.gotByB.clear();
    const std::uint8_t ownerOfA = 1;
    loop.b.send_appearance_ack_to(dsh::PeerId{0}, 910,
                                  std::span<const std::uint8_t>(&ownerOfA, 1));
    loop.run_to(loop.nowMs_ + 60, 20, true);
    int acksAtA = 0;
    int ackSenderAtA = -1;
    std::uint8_t ackOwnerAtA = 0;
    for (const WirePacket& p : loop.gotByA) {
        dsh::net::ByteReader r(p.bytes);
        dsh::net::PacketHeader h;
        if (!dsh::net::read_header(r, h)) continue;
        if (h.type != dsh::net::MessageType::appearanceAck) continue;
        ++acksAtA;
        ackSenderAtA = h.senderPeer;
        const auto body = std::span<const std::uint8_t>(p.bytes).subspan(r.cursor());
        if (!body.empty()) ackOwnerAtA = body[0];
    }
    check(acksAtA == 1, "B's acknowledgement reaches A exactly ONCE",
          "count=" + std::to_string(acksAtA));
    check(ackSenderAtA == 2, "and A sees it as B's -- the HOST stamped the acker into the header",
          "senderPeer=" + std::to_string(ackSenderAtA));
    check(ackOwnerAtA == 1, "and the payload still names A's manifest");
    int acksAtB = 0;
    for (const WirePacket& p : loop.gotByB) {
        dsh::net::ByteReader r(p.bytes);
        dsh::net::PacketHeader h;
        if (!dsh::net::read_header(r, h)) continue;
        if (h.type == dsh::net::MessageType::appearanceAck) ++acksAtB;
    }
    check(acksAtB == 0, "and B did not get its own acknowledgement back");
}

void test_session_over_real_udp() {
    section("session: handshake over REAL UDP sockets (loopback)");

    auto hostTransport = dsh::net::make_udp_transport();
    auto clientTransport = dsh::net::make_udp_transport();
    check(hostTransport != nullptr && clientTransport != nullptr,
          "two UDP transports were created");
    if (!hostTransport || !clientTransport) return;

    check(hostTransport->listen("", 0), "the host transport binds an ephemeral port",
          hostTransport->stats_line().empty() ? "" : std::string(hostTransport->stats_line()));
    check(clientTransport->listen("", 0), "the client transport binds an ephemeral port");

    const auto local = hostTransport->local_endpoint();
    check(local.bytes.size() == sizeof(sockaddr_in),
          "the local endpoint is a raw sockaddr_in, exactly as the transport defines it",
          "size=" + std::to_string(local.bytes.size()) + " expected=" +
              std::to_string(sizeof(sockaddr_in)));
    if (local.bytes.size() != sizeof(sockaddr_in)) {
        hostTransport->close();
        clientTransport->close();
        return;
    }
    sockaddr_in addr{};
    std::memcpy(&addr, local.bytes.data(), sizeof(addr));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dsh::net::Endpoint hostEp;
    hostEp.bytes.resize(sizeof(addr));
    std::memcpy(hostEp.bytes.data(), &addr, sizeof(addr));

    dsh::net::SessionConfig hc;
    hc.playerName = "host";
    dsh::net::Session host(hc);
    host.become_host(dsh::PeerId{0});
    host.set_send_function([&](const dsh::net::Endpoint& to, std::span<const std::uint8_t> b) {
        hostTransport->send(to, b);
    });

    dsh::net::SessionConfig cc;
    cc.playerName = "client";
    dsh::net::Session client(cc);
    client.become_client(dsh::PeerId{1}, hostEp);
    client.set_send_function([&](const dsh::net::Endpoint& to, std::span<const std::uint8_t> b) {
        clientTransport->send(to, b);
    });

    std::vector<dsh::net::Datagram> inbox;
    const std::uint64_t start = GetTickCount64();
    std::uint64_t now = start;
    while (now - start < 4000 && client.connected_count() == 0) {
        Sleep(5);
        now = GetTickCount64();

        inbox.clear();
        hostTransport->poll(inbox);
        for (const auto& d : inbox) host.on_datagram(now, d.from, d.payload);
        inbox.clear();
        clientTransport->poll(inbox);
        for (const auto& d : inbox) client.on_datagram(now, d.from, d.payload);

        host.update(now);
        client.update(now);
    }

    check(client.connected_count() == 1,
          "the client completed a REAL-UDP handshake inside 4 s",
          std::string("elapsed=") + std::to_string(now - start) + " ms; " +
              client.status_line(now));
    check(host.connected_count() == 1, "the host sees exactly one connected peer",
          host.status_line(now));

    {
        const std::string stats(hostTransport->stats_line());
        const auto sentAt = stats.find("sent=");
        const bool named = stats.rfind("udp ", 0) == 0;
        const bool counted =
            sentAt != std::string::npos &&
            std::strtoull(stats.c_str() + sentAt + std::strlen("sent="), nullptr, 10) > 0;
        check(named && counted,
              "transport statistics are available, and they count the datagrams the handshake sent", stats);
    }

    for (int i = 0; i < 200; ++i) {
        Sleep(5);
        const std::uint64_t t = GetTickCount64();
        inbox.clear();
        hostTransport->poll(inbox);
        for (const auto& d : inbox) host.on_datagram(t, d.from, d.payload);
        inbox.clear();
        clientTransport->poll(inbox);
        for (const auto& d : inbox) client.on_datagram(t, d.from, d.payload);
        host.update(t);
        client.update(t);
    }
    const auto rt = client.remote_time(GetTickCount64());
    check(rt.valid, "the client obtained a clock estimate over the real socket");
    const auto skew = rt.ms - static_cast<std::int64_t>(GetTickCount64());
    check(std::abs(skew) < 200, "and the offset is near zero on loopback (both clocks are one)",
          "skew=" + std::to_string(skew) + " ms, rtt=" +
              std::to_string(client.peers().empty() ? 0 : client.peers()[0].clockRttMs) + " ms");

    hostTransport->close();
    clientTransport->close();
    std::printf(" [info] transport stats: host=%s\n",
                std::string(hostTransport->stats_line()).c_str());
}

}

void test_loadout_manifest_chunking() {

    dsh::reflect::MachineLoadout full;
    full.className = "BP_MobilityModule_WL005_C";
    full.classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/BP_MobilityModule_WL005.BP_MobilityModule_WL005_C";
    full.trigger[0] = 1;
    full.trigger[1] = 2;
    full.trigger[2] = 3;

    full.slots[0].present = true;
    full.slots[0].className = "BP_HangerModule_AC601_C";
    full.slots[0].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/HangerModules/"
        "BP_HangerModule_AC601.BP_HangerModule_AC601_C";
    full.slots[0].level = 1;
    full.slots[1].present = true;
    full.slots[1].className = "BP_HeadsetModule_AC601_C";
    full.slots[1].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/HeadsetModules/"
        "BP_HeadsetModule_AC601.BP_HeadsetModule_AC601_C";
    full.slots[1].level = 2;
    full.slots[2].present = true;
    full.slots[2].className = "BP_MobilityModule_WL005_C";
    full.slots[2].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/MobilityModules/"
        "BP_MobilityModule_WL005.BP_MobilityModule_WL005_C";
    full.slots[2].level = 3;
    full.slots[3].present = true;
    full.slots[3].className = "BP_ThrusterModule_AC601_C";
    full.slots[3].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/ThrusterModules/"
        "BP_ThrusterModule_AC601.BP_ThrusterModule_AC601_C";
    full.slots[3].level = 4;
    full.slots[4].present = true;
    full.slots[4].className = "BP_UtilityModule_AC601_C";
    full.slots[4].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/UtilityModules/"
        "BP_UtilityModule_AC601.BP_UtilityModule_AC601_C";
    full.slots[4].level = 5;
    full.slots[5].present = true;
    full.slots[5].className = "BP_FrontWeaponModule_AssaultRifle01_C";
    full.slots[5].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/FrontWeaponModules/"
        "BP_FrontWeaponModule_AssaultRifle01.BP_FrontWeaponModule_AssaultRifle01_C";
    full.slots[5].level = 6;
    full.slots[6].present = true;
    full.slots[6].className = "BP_FrontWeaponModule_Sword01_C";
    full.slots[6].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/FrontWeaponModules/"
        "BP_FrontWeaponModule_Sword01.BP_FrontWeaponModule_Sword01_C";
    full.slots[6].level = 7;
    full.slots[7].present = true;
    full.slots[7].className = "BP_RearWeaponModule_AC601_C";
    full.slots[7].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/RearWeaponModules/"
        "BP_RearWeaponModule_AC601.BP_RearWeaponModule_AC601_C";
    full.slots[7].level = 8;
    full.slots[8].present = true;
    full.slots[8].className = "BP_RearWeaponModule_AC602_C";
    full.slots[8].classPath =
        "/Game/DollsNest/Blueprints/Characters/Machine/Modules/RearWeaponModules/"
        "BP_RearWeaponModule_AC602.BP_RearWeaponModule_AC602_C";
    full.slots[8].level = 9;
    full.customizePresent = true;
    full.partsItemName[0] = "DA_BodyParts_CH020_DLC001";
    full.partsItemPath[0] =
        "/Game/DollsNest/Blueprints/Characters/Doll/Parts/Body/"
        "DA_BodyParts_CH020_DLC001.DA_BodyParts_CH020_DLC001";
    full.partsItemName[1] = "DA_FaceParts_CH020_DLC001";
    full.partsItemPath[1] =
        "/Game/DollsNest/Blueprints/Characters/Doll/Parts/Face/"
        "DA_FaceParts_CH020_DLC001.DA_FaceParts_CH020_DLC001";
    full.partsItemName[2] = "DA_FrontHairParts_CH020_DLC001";
    full.partsItemPath[2] =
        "/Game/DollsNest/Blueprints/Characters/Doll/Parts/FrontHair/"
        "DA_FrontHairParts_CH020_DLC001.DA_FrontHairParts_CH020_DLC001";
    full.partsItemName[3] = "DA_BackHairParts_CH020_DLC001";
    full.partsItemPath[3] =
        "/Game/DollsNest/Blueprints/Characters/Doll/Parts/BackHair/"
        "DA_BackHairParts_CH020_DLC001.DA_BackHairParts_CH020_DLC001";
    full.inverseMesh[0] = 1;
    full.inverseMesh[1] = 0;
    full.inverseMesh[2] = 1;
    full.moduleColoringPresent = true;
    for (int m = 0; m < dsh::reflect::kModuleColorCount; ++m) {
        for (int ch = 0; ch < dsh::reflect::kModuleColorChannels; ++ch) {
            for (int c = 0; c < dsh::reflect::kModuleColorComponents; ++c) {
                full.moduleColoring[m][ch][c] = 0.25f * static_cast<float>(m) + 0.0625f * static_cast<float>(c);
            }
        }
    }

    const auto bytes = dsh::game::encode_loadout_manifest(&full);
    check(!bytes.empty(), "the loadout manifest encodes");

    const std::uint32_t id = 0xABCD1234u;
    const auto chunks = dsh::game::chunk_appearance(bytes, id);
    check(chunks.size() >= 3, "the loadout fixture really does span more than two chunks",
          std::to_string(bytes.size()) + " byte(s) in " + std::to_string(chunks.size()) + " chunk(s)");
    check(!chunks.empty(), "the manifest chunks");
    if (!chunks.empty()) {
        std::size_t biggest = 0;
        for (const auto& c : chunks) if (c.size() > biggest) biggest = c.size();
        check(biggest <= dsh::game::kAppearanceChunkBytes,
              "no chunk exceeds the datagram budget", std::to_string(biggest));
    }

    {
        dsh::game::AppearanceAssembler asm1;
        std::string an;
        bool done = false;
        for (const auto& c : chunks) done = asm1.add_chunk(c, an) || done;
        check(done && asm1.complete(), "in-order chunks assemble into a complete manifest");
        dsh::reflect::MachineLoadout arrived;
        check(asm1.take(an, &arrived), "the assembled manifest decodes", an);
        check(arrived.className == full.className && arrived.slot_count() == full.slot_count() &&
                  arrived.slots[8].classPath == full.slots[8].classPath &&
                  arrived.partsItemPath[3] == full.partsItemPath[3],
              "the assembled manifest carries the loadout that was sent");
    }

    if (chunks.size() > 1) {
        dsh::game::AppearanceAssembler asm2;
        std::string an;
        bool done = false;
        for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) done = asm2.add_chunk(*it, an) || done;
        check(done && asm2.complete(), "reversed chunks assemble just as well");
        dsh::reflect::MachineLoadout arrived;
        check(asm2.take(an, &arrived), "the reversed assembly decodes", an);
        check(arrived.slot_count() == full.slot_count() &&
                  arrived.slots[8].classPath == full.slots[8].classPath,
              "and carries the same loadout");
    }

    {
        dsh::game::AppearanceAssembler asm3;
        std::string an;
        bool completed = false;
        for (const auto& c : chunks) {
            if (asm3.add_chunk(c, an)) completed = true;
            if (asm3.add_chunk(c, an)) completed = true;
        }
        check(completed && asm3.complete(), "a duplicated set still completes the manifest");
        check(asm3.chunksReceived() == chunks.size(),
              "duplicate chunks are counted once, not twice",
              std::to_string(asm3.chunksReceived()) + " of " + std::to_string(chunks.size()));
    }

    {
        dsh::game::AppearanceAssembler asm4;
        std::string an;
        const std::span<const std::uint8_t> tiny(chunks[0].data(), 3);
        check(!asm4.add_chunk(tiny, an), "a chunk shorter than its envelope is refused", an);
    }

    if (chunks.size() > 1) {
        dsh::game::AppearanceAssembler asm5;
        std::string an;
        asm5.add_chunk(chunks[0], an);
        const auto other = dsh::game::chunk_appearance(bytes, id + 1);
        check(!other.empty(), "a second manifest chunks");
        if (!other.empty()) {
            asm5.add_chunk(other[0], an);
            check(asm5.manifest_id() == id + 1,
                  "a chunk from a new manifest replaces the one in progress");
            check(!asm5.complete(), "and the new manifest is not complete from one chunk");
        }
    }

    {
        auto truncated = bytes;
        truncated.resize(bytes.size() / 2);
        dsh::reflect::MachineLoadout out;
        std::string bnote;
        check(!dsh::game::decode_loadout_manifest(truncated, bnote, &out),
              "a truncated manifest is refused", bnote);
    }
}

void test_machine_loadout_wire() {
    dsh::reflect::MachineLoadout machine;
    machine.className = "BP_MobilityModule_WL005_C";
    machine.classPath =
        "/Game/DollsNest/Chara/WL005/BP_MobilityModule_WL005.BP_MobilityModule_WL005_C";
    machine.trigger[0] = 1;
    machine.trigger[1] = 2;
    machine.trigger[2] = 3;

    machine.slots[2].present = true;
    machine.slots[2].className = "BP_MobilityModule_WL005_C";
    machine.slots[2].classPath =
        "/Game/DollsNest/Chara/WL005/BP_MobilityModule_WL005.BP_MobilityModule_WL005_C";
    machine.slots[2].level = 5;
    machine.slots[5].present = true;
    machine.slots[5].className = "BP_FrontWeapon_Sword01_C";
    machine.slots[5].classPath = "/Game/DollsNest/Weapon/BP_FrontWeapon_Sword01.BP_FrontWeapon_Sword01_C";
    machine.slots[5].level = 3;

    machine.customizePresent = true;
    machine.partsItemName[0] = "DA_BodyParts_CH016";
    machine.partsItemPath[0] = "/Game/DollsNest/Parts/DA_BodyParts_CH016.DA_BodyParts_CH016";
    machine.partsItemName[1] = "DA_FaceParts_CH016";
    machine.partsItemPath[1] = "/Game/DollsNest/Parts/DA_FaceParts_CH016.DA_FaceParts_CH016";
    machine.partsItemName[2] = "DA_FrontHairParts_CH016";
    machine.partsItemPath[2] = "/Game/DollsNest/Parts/DA_FrontHairParts_CH016.DA_FrontHairParts_CH016";
    machine.partsItemName[3] = "DA_BackHairParts_CH016";
    machine.partsItemPath[3] = "/Game/DollsNest/Parts/DA_BackHairParts_CH016.DA_BackHairParts_CH016";
    for (int i = 0; i < 13; ++i) {
        for (int c = 0; c < 4; ++c) {
            machine.customize.coloring[i][c] = 0.125f * static_cast<float>(i) - 0.5f;
        }
    }
    for (int i = 0; i < 5; ++i) machine.customize.figure[i] = 1.0f + 0.25f * static_cast<float>(i);
    machine.inverseMesh[0] = 1;
    machine.inverseMesh[1] = 0;
    machine.inverseMesh[2] = 1;

    machine.moduleColoringPresent = true;
    for (int m = 0; m < dsh::reflect::kModuleColorCount; ++m) {
        for (int ch = 0; ch < dsh::reflect::kModuleColorChannels; ++ch) {
            for (int c = 0; c < dsh::reflect::kModuleColorComponents; ++c) {
                machine.moduleColoring[m][ch][c] =
                    0.03125f * static_cast<float>((m * 16) + (ch * 4) + c) + 0.0078125f;
            }
        }
    }

    const auto bytes = dsh::game::encode_loadout_manifest(&machine);
    check(!bytes.empty(), "a manifest with a machine loadout encodes");

    const auto withoutLoadout = dsh::game::encode_loadout_manifest();
    check(bytes.size() > withoutLoadout.size() + 500,
          "the loadout adds several hundred bytes to the payload, which is why it travels in the "
          "chunked manifest rather than in a datagram of its own",
          std::to_string(bytes.size()) + " byte(s) with it, " +
              std::to_string(withoutLoadout.size()) + " without");

    dsh::reflect::MachineLoadout back;
    std::string note;
    check(dsh::game::decode_loadout_manifest(bytes, note, &back),
          "a manifest with a machine loadout decodes", note);
    check(back.className == machine.className && back.classPath == machine.classPath,
          "the machine's class name and path survive the round trip");
    check(back.trigger[0] == machine.trigger[0] && back.trigger[1] == machine.trigger[1] &&
              back.trigger[2] == machine.trigger[2],
          "FDATriggerData survives the round trip");

    bool slotsOk = true;
    for (int i = 0; i < 9; ++i) {
        if (back.slots[i].present != machine.slots[i].present) slotsOk = false;
        if (!machine.slots[i].present) continue;
        if (back.slots[i].className != machine.slots[i].className) slotsOk = false;
        if (back.slots[i].classPath != machine.slots[i].classPath) slotsOk = false;
        if (back.slots[i].level != machine.slots[i].level) slotsOk = false;
    }
    check(slotsOk, "all nine slots survive, with the empty ones still empty and the levels intact");

    bool customizeOk = back.customizePresent;
    for (int i = 0; i < 13 && customizeOk; ++i) {
        for (int c = 0; c < 4; ++c) {
            if (back.customize.coloring[i][c] != machine.customize.coloring[i][c]) customizeOk = false;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (back.customize.figure[i] != machine.customize.figure[i]) customizeOk = false;
    }
    for (int k = 0; k < 4; ++k) {
        if (back.partsItemName[k] != machine.partsItemName[k]) customizeOk = false;
        if (back.partsItemPath[k] != machine.partsItemPath[k]) customizeOk = false;
    }
    if (back.inverseMesh[0] != 1 || back.inverseMesh[1] != 0 || back.inverseMesh[2] != 1) {
        customizeOk = false;
    }
    check(customizeOk, "FDACustomizeData survives the round trip, the three flags included");

    {
        bool moduleColorsOk = back.moduleColoringPresent;
        int  moduleColorsWrong = 0;
        for (int m = 0; m < dsh::reflect::kModuleColorCount; ++m) {
            for (int ch = 0; ch < dsh::reflect::kModuleColorChannels; ++ch) {
                for (int c = 0; c < dsh::reflect::kModuleColorComponents; ++c) {
                    if (back.moduleColoring[m][ch][c] != machine.moduleColoring[m][ch][c]) {
                        ++moduleColorsWrong;
                    }
                }
            }
        }
        check(moduleColorsOk, "the armour shell's colours arrive as a present block",
              moduleColorsOk ? "" : "moduleColoringPresent is false after the round trip");
        check(moduleColorsWrong == 0,
              "and all 576 of them survive the round trip, value by value",
              std::to_string(moduleColorsWrong) + " of 576 value(s) differ");

        check(back.moduleColoring[0][0][0] == machine.moduleColoring[0][0][0] &&
                  back.moduleColoring[8][3][3] == machine.moduleColoring[8][3][3],
              "the first and last values on the wire are in the right places",
              std::to_string(back.moduleColoring[0][0][0]) + " and " +
                  std::to_string(back.moduleColoring[8][3][3]));
    }

    {
        dsh::reflect::MachineLoadout noShell = machine;
        noShell.moduleColoringPresent = false;
        const auto withShell = dsh::game::encode_loadout_manifest(&machine);
        const auto withoutShell = dsh::game::encode_loadout_manifest(&noShell);
        dsh::reflect::MachineLoadout noShellBack;
        std::string noShellNote;
        check(dsh::game::decode_loadout_manifest(withoutShell, noShellNote, &noShellBack),
              "a loadout with no armour-shell colours still encodes and decodes", noShellNote);
        check(!noShellBack.moduleColoringPresent,
              "and it comes back as NOT PRESENT rather than as a present block of zeroes");

        check(withShell.size() == withoutShell.size() + dsh::reflect::kModuleColorBytes,
              "the shell's colours add exactly 576 bytes to the payload -- the presence flag is in "
              "both encodings and only its value changes",
              std::to_string(withShell.size() - withoutShell.size()) + " byte(s)");
    }

    {
        const auto chunks = dsh::game::chunk_appearance(bytes, 0x1234ABCDu);
        dsh::game::AppearanceAssembler asmOutOfOrder;
        std::string an;
        bool done = false;
        for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
            done = asmOutOfOrder.add_chunk(*it, an) || done;
        }
        dsh::reflect::MachineLoadout got;
        check(done && asmOutOfOrder.take(an, &got),
              "the loadout reassembles from chunks that arrive out of order", an);
        check(got.className == machine.className &&
                  got.slots[5].classPath == machine.slots[5].classPath &&
                  got.slot_count() == machine.slot_count(),
              "the reassembled loadout is the one that was sent");
    }

    {
        auto older = bytes;
        older[0] = 2;
        dsh::reflect::MachineLoadout ignored;
        std::string vnote;
        check(!dsh::game::decode_loadout_manifest(older, vnote, &ignored),
              "a version-2 manifest is refused rather than read as 'no loadout'", vnote);
        check(vnote.find("2") != std::string::npos &&
                  vnote.find(std::to_string(dsh::game::kAppearanceVersion)) != std::string::npos,
              "and the refusal NAMES both versions", vnote);

        check(vnote.find("version 3") == std::string::npos,
              "and it does NOT claim the payload was version 3's, which is not what a v2 or v4 "
              "manifest is", vnote);
    }
    {
        auto previous = bytes;
        previous[0] = 4;
        dsh::reflect::MachineLoadout ignored;
        std::string vnote;
        check(!dsh::game::decode_loadout_manifest(previous, vnote, &ignored),
              "a version-4 manifest -- this payload minus the shell's 576 bytes -- is refused",
              vnote);
        check(vnote.find("4") != std::string::npos &&
                  vnote.find(std::to_string(dsh::game::kAppearanceVersion)) != std::string::npos,
              "and that refusal names 4 and the version this build reads", vnote);
    }

    {
        const auto emptyBytes = dsh::game::encode_loadout_manifest();
        dsh::reflect::MachineLoadout emptyOut;
        std::string enote;
        check(dsh::game::decode_loadout_manifest(emptyBytes, enote, &emptyOut),
              "a manifest encoded without a loadout still decodes", enote);
        check(emptyOut.classPath.empty() && emptyOut.slot_count() == 0 && !emptyOut.customizePresent &&
                  !emptyOut.moduleColoringPresent,
              "and it decodes as an EMPTY loadout, not as a missing section");
    }
}

void test_log_truncation_is_marked() {
    std::printf("\n--- logging: a line that does not fit ---\n");
    dsh::platform::log_init();
    const std::string path(dsh::platform::log_path());
    if (path.empty()) {
        check(false, "the log could not be opened, so truncation cannot be observed at all", "");
        return;
    }

    const std::string huge(4000, 'x');
    DSH_INFO("truncation probe, this line is deliberately far too long: %s", huge.c_str());
    dsh::platform::log_shutdown();

    std::ifstream in(path);
    if (!in) {
        check(false, "the log was opened but cannot be read back", path);
        return;
    }
    bool sawMarkedLine = false;
    bool sawTruncationCount = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("truncation probe") != std::string::npos) {
            sawMarkedLine = line.find("TRUNCATED") != std::string::npos;
            break;
        }
    }
    in.clear();
    in.seekg(0);
    while (std::getline(in, line)) {
        if (line.rfind("[log] closing:", 0) == 0 && line.find("truncated=") != std::string::npos &&
            line.find("truncated=0") == std::string::npos) {
            sawTruncationCount = true;
            break;
        }
    }
    check(sawMarkedLine,
          "a log line too long for the buffer is MARKED, not cut in silence");
    check(sawTruncationCount,
          "and the closing line counts it, the way dropped lines are already counted");
}

namespace synth {
constexpr std::size_t kActor          = 0x0100;
constexpr std::size_t kSiblingActor   = 0x0200;
constexpr std::size_t kClassSibling   = 0x1000;
constexpr std::size_t kClassMiddle    = 0x1100;
constexpr std::size_t kClassCharacter = 0x1200;
constexpr std::size_t kClassOther     = 0x1300;
constexpr std::size_t kCyclic         = 0x1400;
constexpr std::size_t kComponent      = 0x4000;
constexpr std::size_t kController     = 0x5000;

constexpr std::size_t kClassMachine    = 0x1500;

constexpr std::size_t kClassDerivedMove = 0x1600;

constexpr std::uint32_t kUObjectClassOffset = 0x10;
constexpr std::uint32_t kUStructSuperOffset = 0x40;
}

class SyntheticMemory {
public:
    SyntheticMemory() {
        base_ = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (base_ != nullptr) std::memset(base_, 0, kBytes);
    }
    ~SyntheticMemory() {
        if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
    }
    SyntheticMemory(const SyntheticMemory&) = delete;
    SyntheticMemory& operator=(const SyntheticMemory&) = delete;

    [[nodiscard]] bool ok() const noexcept { return base_ != nullptr; }
    [[nodiscard]] std::uintptr_t at(std::size_t offset) const noexcept {
        return reinterpret_cast<std::uintptr_t>(base_ + offset);
    }
    void poke(std::size_t offset, std::uintptr_t value) {
        std::memcpy(base_ + offset, &value, sizeof(value));
    }
    void poke_byte(std::size_t offset, std::uint8_t value) { base_[offset] = value; }
    void poke_f32(std::size_t offset, float value) { std::memcpy(base_ + offset, &value, sizeof(value)); }
    void poke_bytes(std::size_t offset, const float* values, std::size_t bytes) {
        std::memcpy(base_ + offset, values, bytes);
    }

    static bool read(std::uintptr_t address, void* dst, std::size_t bytes) {
        if (dst == nullptr || bytes == 0 || s_active == nullptr) return false;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(s_active->base_);
        if (address < base || address + bytes > base + kBytes) return false;
        std::memcpy(dst, reinterpret_cast<const void*>(address), bytes);
        return true;
    }

    static bool watch_read(std::uintptr_t address, void* dst, std::size_t bytes) {
        if (s_active != nullptr) {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(s_active->base_);
            if (address == base + synth::kActor + dsh::game::kCharacterDefaultStanceOffset) {
                s_active->touchedDefaultStance = true;
            }
            if (address == base + synth::kActor + dsh::game::kCharacterCurrentStanceOffset) {
                s_active->touchedCurrentStance = true;
            }
            if (address >= base + synth::kSiblingActor + dsh::game::kCharacterDefaultStanceOffset &&
                address <= base + synth::kSiblingActor + dsh::game::kCharacterCurrentStanceOffset) {
                s_active->touchedSiblingStance = true;
            }

            if (address == base + synth::kActor + dsh::game::kCharacterMovementOffset) {
                s_active->touchedCharacterMovement = true;
            }
            if (address == base + synth::kActor + dsh::game::kDACharacterMovementOffset) {
                s_active->touchedDACharacterMovement = true;
            }

            if (address == base + synth::kActor + dsh::game::kCharacterIgnoreFrictionOffset) {
                s_active->touchedMovementInput = true;
            }

            if (address >= base + synth::kActor + dsh::game::kCharacterIntermissionTimeOffset &&
                address < base + synth::kActor + dsh::game::kCharacterIntermissionTimeOffset +
                              dsh::game::kEnergyTimerBytes) {
                s_active->touchedEnergyTimers = true;
            }

            if (address >= base + synth::kActor + dsh::game::kMachineFrontWeaponInventoryOffset &&
                address < base + synth::kActor + dsh::game::kMachineFrontWeaponInventoryOffset +
                              dsh::game::kMachineFrontWeaponSpanBytes) {
                s_active->touchedFrontWeapon = true;
            }

            if (watchTracker != nullptr) {
                watchTracker->addresses.push_back(address);
                if (address >= base + synth::kComponent + dsh::game::kDerivedMovementSpanBegin &&
                    address < base + synth::kComponent + dsh::game::kDerivedMovementSpanEnd) {
                    watchTracker->sawDerivedSpan = true;
                }
            }
        }
        return read(address, dst, bytes);
    }

    static void set_active(SyntheticMemory* m) noexcept { s_active = m; }

    static bool read_nul_terminated(std::uintptr_t address, std::string& out) {
        out.clear();
        if (s_active == nullptr) return false;
        constexpr std::size_t kNameLimit = 256;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(s_active->base_);
        if (address < base || address >= base + kBytes) return false;
        const std::size_t room = static_cast<std::size_t>(base + kBytes - address);
        char buffer[kNameLimit] = {};
        std::strncpy(buffer, reinterpret_cast<const char*>(address),
                     (std::min)(room - 1, static_cast<std::size_t>(kNameLimit - 1)));
        const std::size_t length = std::strlen(buffer);
        if (length == 0) return false;
        out.assign(buffer, length);
        return true;
    }

    bool touchedDefaultStance = false;
    bool touchedCurrentStance = false;
    bool touchedSiblingStance = false;
    bool touchedCharacterMovement = false;
    bool touchedDACharacterMovement = false;
    bool touchedMovementInput = false;
    bool touchedEnergyTimers = false;
    bool touchedFrontWeapon = false;

    struct ReadTracker {
        std::vector<std::uintptr_t> addresses;
        bool sawDerivedSpan = false;
    };
    static ReadTracker* watchTracker;

    static void set_watch_tracker(ReadTracker* t) noexcept { watchTracker = t; }

private:
    static constexpr std::size_t kBytes = 0x10000;
    static SyntheticMemory* s_active;
    std::uint8_t* base_ = nullptr;
};

SyntheticMemory* SyntheticMemory::s_active = nullptr;
SyntheticMemory::ReadTracker* SyntheticMemory::watchTracker = nullptr;

void test_doll_stance_payload() {
    section("the doll's stance: the pose payload");

    check(dsh::game::kStatePayloadBytes == 75,
          "the state payload is 75 bytes: five pose float32, the two stance bytes, three velocity "
          "float32, the MovementMode byte, the five movement-input flag bytes, the twelve-byte "
          "control-input vector, the host's two energy timer float32, the host's front-weapon byte, "
          "the host's three acceleration float32 and the host's exclusive-mode pair",
          "kStatePayloadBytes=" + std::to_string(dsh::game::kStatePayloadBytes));
    check(dsh::game::kStatePayloadVelocityBytes == 12 && dsh::game::kStatePayloadModeBytes == 1,
          "the velocity round added exactly twelve velocity bytes and one mode byte",
          "velocity=" + std::to_string(dsh::game::kStatePayloadVelocityBytes) + " mode=" +
              std::to_string(dsh::game::kStatePayloadModeBytes));
    check(dsh::game::kStatePayloadInputFlagBytes == 5 && dsh::game::kStatePayloadInputVectorBytes == 12,
          "and the movement-input round added exactly five flag bytes and twelve vector bytes",
          "flags=" + std::to_string(dsh::game::kStatePayloadInputFlagBytes) + " vector=" +
              std::to_string(dsh::game::kStatePayloadInputVectorBytes));

    check(dsh::game::kStatePayloadEnergyTimerBytes == 8 &&
              dsh::game::kStatePayloadBytes == 60 + dsh::game::kStatePayloadFrontWeaponBytes +
                                                  dsh::game::kStatePayloadAccelerationBytes +
                                                  dsh::game::kStatePayloadExclusiveBytes,
          "and the sum of all the parts still accounts for every byte: 60 + the one front-weapon byte + "
          "the twelve acceleration bytes + the exclusive-mode pair",
          "frontWeaponBytes=" + std::to_string(dsh::game::kStatePayloadFrontWeaponBytes) +
              " accelerationBytes=" + std::to_string(dsh::game::kStatePayloadAccelerationBytes) +
              " exclusiveBytes=" + std::to_string(dsh::game::kStatePayloadExclusiveBytes));
    check(dsh::game::kStatePayloadAccelerationBytes == 12 &&
              dsh::game::kStatePayloadExclusiveBytes == 2,
          "the acceleration round added twelve bytes and THIS round added exactly two -- the flag byte and "
          "the owner's slot classification",
          "accelerationBytes=" + std::to_string(dsh::game::kStatePayloadAccelerationBytes) +
              " exclusiveBytes=" + std::to_string(dsh::game::kStatePayloadExclusiveBytes));
    check(dsh::game::kStatePayloadEnergyTimerBytes == 8 &&
              dsh::game::kStatePayloadBytes == 52 + dsh::game::kStatePayloadEnergyTimerBytes +
                                                  dsh::game::kStatePayloadFrontWeaponBytes +
                                                  dsh::game::kStatePayloadAccelerationBytes +
                                                  dsh::game::kStatePayloadExclusiveBytes,
          "and the sum of all the parts still accounts for every byte: 52 + the two energy timers + the "
          "one front-weapon byte + the twelve acceleration bytes + the exclusive-mode pair",
          "energyTimerBytes=" + std::to_string(dsh::game::kStatePayloadEnergyTimerBytes));

    check(dsh::game::kCharacterIntermissionTimeOffset == 0x0700 &&
              dsh::game::kCharacterFastChargeStartOffset == 0x0704 &&
              dsh::game::kEnergyTimerBytes == 8,
          "the two energy timers are at and, exactly four bytes apart and eight bytes "
          "in total -- the game's own declarations on DACharacter",
          "intermission=" + std::to_string(dsh::game::kCharacterIntermissionTimeOffset) +
              " fastCharge=" + std::to_string(dsh::game::kCharacterFastChargeStartOffset));
    check(dsh::game::kMachineFrontWeaponInventoryOffset == 0x1620 &&
              dsh::game::kMachineCurrentFrontWeaponOffset == 0x1630 &&
              dsh::game::kMachineFrontWeaponSpanBytes == 0x18,
          "the two front-weapon fields are at and -- the game's own declarations on "
          "ADAMachine -- and their three pointers are one 0x18-byte span",
          "inventory=" + std::to_string(dsh::game::kMachineFrontWeaponInventoryOffset) + " current=" +
              std::to_string(dsh::game::kMachineCurrentFrontWeaponOffset));
    check(dsh::game::kFrontWeaponSlot0 == 0 && dsh::game::kFrontWeaponSlot1 == 1 &&
              dsh::game::kFrontWeaponNone == 2 && dsh::game::kFrontWeaponNotRead == 3,
          "the one byte has FOUR values and not three: the two slots, the game's own nothing-in-hand "
          "state, and a refusal of its own -- because 'the host has nothing in its hand' and 'the host "
          "could not read its own pointers' must never print as the same thing",
          "slots=0,1 none=2 notRead=3");
    check(dsh::game::kCharacterCurrentStanceOffset == 0x10F6 &&
              dsh::game::kCharacterDefaultStanceOffset == 0x10F5,
          "the two bytes are at the offsets the game gives, CurrentStance last");

    dsh::game::StanceWireState out;
    out.posM[0] = 12.5f;
    out.posM[1] = -3.25f;
    out.posM[2] = 0.75f;
    out.yawRad = 1.5f;
    out.pitchRad = -0.25f;
    out.stance.defaultStance = dsh::game::kStanceNeutral;
    out.stance.currentStance = dsh::game::kStanceBeware;
    out.velocity[0] = 686.5f;
    out.velocity[1] = -12.25f;
    out.velocity[2] = 1007.75f;
    out.movementMode = 5;

    out.movementInput.ignoreFriction = 1;
    out.movementInput.hover = 0;
    out.movementInput.sprint = 1;
    out.movementInput.boostJump = 0;
    out.movementInput.boostDash = 1;
    out.movementInput.recentInput[0] = -0.75f;
    out.movementInput.recentInput[1] = 512.5f;
    out.movementInput.recentInput[2] = 3.25f;

    out.energyTimers.intermissionTime = 12.375f;
    out.energyTimers.fastChargeStartTime = 1042.5f;

    out.frontWeaponIndex = dsh::game::kFrontWeaponSlot1;

    out.acceleration[0] = -611.75f;
    out.acceleration[1] = 1750.5f;
    out.acceleration[2] = -3.5f;

    out.exclusiveTypes = 3;
    out.exclusiveOwnerKind = 2;

    dsh::net::ByteWriter w;
    const std::size_t written = dsh::game::encode_stance_state(w, out);
    check(written == 75 && w.size() == 75, "the host writes 75 bytes",
          "written=" + std::to_string(written));

    check(w.buffer()[20] == dsh::game::kStanceNeutral && w.buffer()[21] == dsh::game::kStanceBeware,
          "the two stance bytes are at 20 and 21, so the five pose floats keep their offsets",
          "bytes 20,21 = " + std::to_string(w.buffer()[20]) + "," + std::to_string(w.buffer()[21]));
    {

        const std::uint32_t bits = static_cast<std::uint32_t>(w.buffer()[22]) |
                                   (static_cast<std::uint32_t>(w.buffer()[23]) << 8) |
                                   (static_cast<std::uint32_t>(w.buffer()[24]) << 16) |
                                   (static_cast<std::uint32_t>(w.buffer()[25]) << 24);
        float vx = 0.0f;
        std::memcpy(&vx, &bits, sizeof(vx));
        check(vx == 686.5f, "the first velocity float is little-endian at byte 22",
              "vx=" + std::to_string(vx));
    }
    check(w.buffer()[34] == 5,
          "the MovementMode byte is the LAST byte of the velocity round's payload, so a reader cannot "
          "take it for a velocity byte and the round that added the movement input did not move it",
          "byte 34 = " + std::to_string(w.buffer()[34]));

    check(w.buffer()[35] == 1 && w.buffer()[36] == 0 && w.buffer()[37] == 1 &&
              w.buffer()[38] == 0 && w.buffer()[39] == 1,
          "the five movement-input flags are at 35..39 in the game's own declaration order",
          "bytes 35..39 = " + std::to_string(w.buffer()[35]) + "," + std::to_string(w.buffer()[36]) +
              "," + std::to_string(w.buffer()[37]) + "," + std::to_string(w.buffer()[38]) + "," +
              std::to_string(w.buffer()[39]));
    {

        const std::uint32_t bits = static_cast<std::uint32_t>(w.buffer()[40]) |
                                   (static_cast<std::uint32_t>(w.buffer()[41]) << 8) |
                                   (static_cast<std::uint32_t>(w.buffer()[42]) << 16) |
                                   (static_cast<std::uint32_t>(w.buffer()[43]) << 24);
        float cx = 0.0f;
        std::memcpy(&cx, &bits, sizeof(cx));
        check(cx == -0.75f, "and the control-input vector begins little-endian at byte 40",
              "cx=" + std::to_string(cx));
    }
    {

        const std::uint32_t first = static_cast<std::uint32_t>(w.buffer()[52]) |
                                    (static_cast<std::uint32_t>(w.buffer()[53]) << 8) |
                                    (static_cast<std::uint32_t>(w.buffer()[54]) << 16) |
                                    (static_cast<std::uint32_t>(w.buffer()[55]) << 24);
        const std::uint32_t second = static_cast<std::uint32_t>(w.buffer()[56]) |
                                     (static_cast<std::uint32_t>(w.buffer()[57]) << 8) |
                                     (static_cast<std::uint32_t>(w.buffer()[58]) << 16) |
                                     (static_cast<std::uint32_t>(w.buffer()[59]) << 24);
        float intermission = 0.0f;
        float fastCharge = 0.0f;
        std::memcpy(&intermission, &first, sizeof(intermission));
        std::memcpy(&fastCharge, &second, sizeof(fastCharge));
        check(intermission == 12.375f && fastCharge == 1042.5f,
              "the two energy timers are little-endian at bytes 52 and 56, in the game's own order "
              "(IntermissionTime @ then EnergyFastChargeStartTime @)",
              "(" + std::to_string(intermission) + ", " + std::to_string(fastCharge) + ")");
    }

    check(w.buffer()[60] == dsh::game::kFrontWeaponSlot1,
          "the front-weapon byte is at 60, the last byte of the 61-byte payload the previous build ended "
          "on, and holds the value the encoder was given",
          "byte 60 = " + std::to_string(w.buffer()[60]));
    {

        const std::uint32_t bits = static_cast<std::uint32_t>(w.buffer()[61]) |
                                   (static_cast<std::uint32_t>(w.buffer()[62]) << 8) |
                                   (static_cast<std::uint32_t>(w.buffer()[63]) << 16) |
                                   (static_cast<std::uint32_t>(w.buffer()[64]) << 24);
        float ax = 0.0f;
        std::memcpy(&ax, &bits, sizeof(ax));
        check(ax == -611.75f,
              "the first acceleration float is little-endian at byte 61, the byte the 61-byte payload "
              "ended on",
              "ax=" + std::to_string(ax));
    }

    check(w.buffer()[73] == 3 && w.buffer()[74] == 2,
          "the exclusive-mode pair is at 73 and 74: the flag byte then the owner's slot classification",
          "bytes 73,74 = " + std::to_string(w.buffer()[73]) + "," + std::to_string(w.buffer()[74]));

    dsh::game::StanceWireState back;
    std::string note;
    check(dsh::game::decode_stance_state(w.buffer(), back, note),
          "the client reads its own encoder's payload back", note);
    check(back.posM[0] == 12.5f && back.posM[1] == -3.25f && back.posM[2] == 0.75f &&
              back.yawRad == 1.5f && back.pitchRad == -0.25f,
          "the five pose floats survive the round trip exactly");
    check(back.stance.defaultStance == dsh::game::kStanceNeutral &&
              back.stance.currentStance == dsh::game::kStanceBeware,
          "and so do the two stance bytes");
    check(back.velocity[0] == 686.5f && back.velocity[1] == -12.25f && back.velocity[2] == 1007.75f,
          "and the three velocity floats, which are the engine's own cm/s values copied verbatim -- "
          "nothing rounds or rescales them on either side",
          "(" + std::to_string(back.velocity[0]) + ", " + std::to_string(back.velocity[1]) + ", " +
              std::to_string(back.velocity[2]) + ")");
    check(back.movementMode == 5,
          "and the host's MovementMode byte, which the client prints and never writes",
          "mode=" + std::to_string(back.movementMode));
    check(back.movementInput.ignoreFriction == 1 && back.movementInput.hover == 0 &&
              back.movementInput.sprint == 1 && back.movementInput.boostJump == 0 &&
              back.movementInput.boostDash == 1,
          "and the five movement-input flags, each in its own field rather than in a mask");
    check(back.movementInput.recentInput[0] == -0.75f && back.movementInput.recentInput[1] == 512.5f &&
              back.movementInput.recentInput[2] == 3.25f,
          "and the control-input vector, copied verbatim -- nothing rounds or rescales it on either side",
          "(" + std::to_string(back.movementInput.recentInput[0]) + ", " +
              std::to_string(back.movementInput.recentInput[1]) + ", " +
              std::to_string(back.movementInput.recentInput[2]) + ")");

    check(back.energyTimers.intermissionTime == 12.375f &&
              back.energyTimers.fastChargeStartTime == 1042.5f,
          "and the host's two energy timers, copied verbatim -- nothing converts, scales or clamps them "
          "on either side",
          "(" + std::to_string(back.energyTimers.intermissionTime) + ", " +
              std::to_string(back.energyTimers.fastChargeStartTime) + ")");

    check(back.frontWeaponIndex == dsh::game::kFrontWeaponSlot1,
          "and the host's front-weapon byte arrives as the value the host classified -- the decoder copies "
          "it and does not turn it into anything",
          "frontWeaponIndex=" + std::to_string(back.frontWeaponIndex));

    check(back.exclusiveTypes == 3 && back.exclusiveOwnerKind == 2,
          "and the host's exclusive-mode pair arrives as the flag byte and the slot the host classified",
          "types=" + std::to_string(back.exclusiveTypes) + " kind=" +
              std::to_string(back.exclusiveOwnerKind));

    check(back.acceleration[0] == -611.75f && back.acceleration[1] == 1750.5f &&
              back.acceleration[2] == -3.5f,
          "and the host's three acceleration floats, copied verbatim -- nothing converts, scales or clamps "
          "them on either side",
          "(" + std::to_string(back.acceleration[0]) + ", " + std::to_string(back.acceleration[1]) + ", " +
              std::to_string(back.acceleration[2]) + ")");

    std::vector<std::uint8_t> exact(w.buffer().begin(), w.buffer().begin() + 75);
    for (const std::size_t shortBy : {std::size_t(1), std::size_t(2), std::size_t(10)}) {
        std::vector<std::uint8_t> cut(exact.begin(),
                                      exact.begin() + static_cast<std::ptrdiff_t>(75 - shortBy));
        dsh::game::StanceWireState ignored;
        std::string cutNote;
        const bool refused = !dsh::game::decode_stance_state(cut, ignored, cutNote);
        check(refused, "a payload a few bytes short is REFUSED, not padded",
              std::to_string(shortBy) + " byte(s) short: " + cutNote);
    }
    {

        const std::vector<std::uint8_t> v218(exact.begin(), exact.begin() + 60);
        dsh::game::StanceWireState ignored;
        std::string v218Note;
        check(!dsh::game::decode_stance_state(v218, ignored, v218Note) &&
                  v218Note.find("60") != std::string::npos &&
                  v218Note.find("75") != std::string::npos &&
                  v218Note.find("front-weapon byte") != std::string::npos,
              "the previous build's 60-byte payload is refused BY NAME, both byte counts and the field "
              "that grew the payload -- this is the only place a mixed-version pair can be told apart",
              v218Note);
    }
    {

        const std::vector<std::uint8_t> v217(exact.begin(), exact.begin() + 52);
        dsh::game::StanceWireState ignored;
        std::string v217Note;
        check(!dsh::game::decode_stance_state(v217, ignored, v217Note) &&
                  v217Note.find("52") != std::string::npos &&
                  v217Note.find("75") != std::string::npos &&
                  v217Note.find("energy timer") != std::string::npos,
              "and the v217 build's 52-byte payload is refused BY NAME with the same counts",
              v217Note);
    }
    {

        const std::vector<std::uint8_t> v206(exact.begin(), exact.begin() + 35);
        dsh::game::StanceWireState ignored;
        std::string v206Note;
        check(!dsh::game::decode_stance_state(v206, ignored, v206Note) &&
                  v206Note.find("35") != std::string::npos &&
                  v206Note.find("75") != std::string::npos &&
                  v206Note.find("control-input vector") != std::string::npos,
              "and the v206 build's 35-byte payload is refused BY NAME with the same counts",
              v206Note);
    }
    {
        const std::vector<std::uint8_t> twenty(exact.begin(), exact.begin() + 20);
        dsh::game::StanceWireState ignored;
        std::string v196Note;
        check(!dsh::game::decode_stance_state(twenty, ignored, v196Note) &&
                  v196Note.find("20") != std::string::npos &&
                  v196Note.find("75") != std::string::npos,
              "and the older 20-byte pose-only payload is refused with the same counts",
              v196Note);
    }
    {
        const std::vector<std::uint8_t> twelve(exact.begin(), exact.begin() + 12);
        dsh::game::StanceWireState ignored;
        std::string oldNote;
        check(!dsh::game::decode_stance_state(twelve, ignored, oldNote),
              "and the oldest 12-byte payload is refused too");
    }
    check(dsh::game::decode_stance_state(exact, back, note),
          "a 75-byte payload with trailing bytes beyond it still decodes (the header is consumed "
          "outside this function, so a longer datagram is not a refusal)");

    check(std::string(dsh::game::stance_name(dsh::game::kStanceNeutral)) == "Neutral" &&
              std::string(dsh::game::stance_name(dsh::game::kStanceFight)) == "Fight" &&
              std::string(dsh::game::stance_name(dsh::game::kStanceBeware)) == "Beware",
          "the three stance values are named, so a log line printing 2 means Beware");
    check(std::string(dsh::game::stance_name(3)) == "?",
          "and EDACharacterStanceType_MAX is NOT given a stance name, because it is not one",
          "name(3)=" + std::string(dsh::game::stance_name(3)));
}

void test_doll_stance_class_chain() {
    section("the doll's stance: the class-chain check in front of the offset");

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    const std::uintptr_t classOther     = mem.at(synth::kClassOther);
    const std::uintptr_t cyclic         = mem.at(synth::kCyclic);

    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, classOther);

    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kCyclic + synth::kUStructSuperOffset, cyclic);
    check(dsh::game::actor_class_chain_reaches(mem.at(synth::kActor), classCharacter,
                                               SyntheticMemory::read),
          "a chain of three SuperStruct hops is walked and the class is FOUND");
    check(!dsh::game::actor_class_chain_reaches(mem.at(synth::kSiblingActor), classCharacter,
                                                SyntheticMemory::read),
          "an actor whose chain reaches a DIFFERENT root is refused");
    check(!dsh::game::actor_class_chain_reaches(mem.at(synth::kActor), 0,
                                                SyntheticMemory::read),
          "a zero class pointer is a refusal, not a default: nothing is checked, so nothing is read");
    check(!dsh::game::actor_class_chain_reaches(0x10, classCharacter, SyntheticMemory::read),
          "an implausible actor pointer is refused before any read");
    check(!dsh::game::actor_class_chain_reaches(mem.at(synth::kActor), classCharacter, nullptr),
          "a null reader is refused rather than dereferenced");
    check(!dsh::game::actor_class_chain_reaches(cyclic, classCharacter, SyntheticMemory::read),
          "a chain that LOOPS terminates instead of spinning -- this runs at 20 Hz");

    mem.poke_byte(synth::kActor + dsh::game::kCharacterDefaultStanceOffset, dsh::game::kStanceFight);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterCurrentStanceOffset, dsh::game::kStanceBeware);
    dsh::game::DollStance readStance;
    std::string readNote;
    check(dsh::game::read_doll_stance(mem.at(synth::kActor), classCharacter, readStance, readNote,
                                      SyntheticMemory::read) &&
              readStance.defaultStance == dsh::game::kStanceFight &&
              readStance.currentStance == dsh::game::kStanceBeware,
          "when the chain reaches the class, BOTH bytes are read from and", readNote);

    dsh::game::DollStance neverRead;
    std::string refusal;
    const bool chainFound = dsh::game::actor_class_chain_reaches(
        mem.at(synth::kSiblingActor), classCharacter, SyntheticMemory::watch_read);
    const bool readOk = dsh::game::read_doll_stance(mem.at(synth::kSiblingActor), classCharacter,
                                                    neverRead, refusal, SyntheticMemory::watch_read);

    check(!chainFound, "the sibling actor's chain does not reach the stance class at all",
          "the same answer the guard must act on");

    check(!readOk,
          "an actor that is NOT an ADACharacter is REFUSED, and nothing is read from its",
          refusal);
    check(refusal.find("DACharacter") != std::string::npos,
          "the refusal NAMES the class it was looking for, so 'failed' cannot be misread as a "
          "different problem", refusal);
    check(!mem.touchedSiblingStance,
          "and the refused actor's OWN/ were never opened -- the refusal is the guard "
          "working, not a read that happened to fail");

    mem.touchedDefaultStance = false;
    mem.touchedCurrentStance = false;
    dsh::game::DollStance controlStance;
    const bool controlOk = dsh::game::read_doll_stance(mem.at(synth::kActor), classCharacter,
                                                       controlStance, refusal,
                                                       SyntheticMemory::watch_read);
    check(controlOk && mem.touchedDefaultStance && mem.touchedCurrentStance,
          "the CONTROL: on an actor whose chain does reach the class, the same watch sees BOTH "
          "offsets -- so the assertion above is about the guard and not about a reader that never "
          "looks");

    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, 0);
    dsh::game::DollStance nullCls;
    std::string nullNote;
    check(!dsh::game::read_doll_stance(mem.at(synth::kSiblingActor), classCharacter, nullCls,
                                       nullNote, SyntheticMemory::read),
          "an actor whose class pointer is null is refused", nullNote);

    SyntheticMemory::set_active(nullptr);
}

void test_machine_movement_offsets() {
    section("movement: the offsets, the mode names and the branch the line prints");

    check(dsh::game::kCharacterMovementOffset == 0x0288 &&
              dsh::game::kDACharacterMovementOffset == 0x06D0,
          "the two movement-component pointers are at ACharacter and ADACharacter",
          hex_offset(dsh::game::kCharacterMovementOffset) + " / " +
              hex_offset(dsh::game::kDACharacterMovementOffset));
    check(dsh::game::kMovementVelocityOffset == 0x00C4,
          "Velocity is UMovementComponent (valid through UCharacterMovementComponent)",
          hex_offset(dsh::game::kMovementVelocityOffset));
    check(dsh::game::kMovementModeOffset == 0x0168 &&
              dsh::game::kCustomMovementModeOffset == 0x0169,
          "MovementMode and CustomMovementMode are two adjacent bytes at and",
          hex_offset(dsh::game::kMovementModeOffset) + " / " +
              hex_offset(dsh::game::kCustomMovementModeOffset));
    check(dsh::game::kGravityScaleOffset == 0x0150 && dsh::game::kMaxWalkSpeedOffset == 0x018C &&
              dsh::game::kMaxAccelerationOffset == 0x01A0,
          "GravityScale, MaxWalkSpeed, MaxAcceleration",
          hex_offset(dsh::game::kGravityScaleOffset) + " / " +
              hex_offset(dsh::game::kMaxWalkSpeedOffset) + " / " +
              hex_offset(dsh::game::kMaxAccelerationOffset));
    check(dsh::game::kAccelerationOffset == 0x022C &&
              dsh::game::kLastUpdateLocationOffset == 0x0250 &&
              dsh::game::kLastUpdateVelocityOffset == 0x025C,
          "Acceleration, LastUpdateLocation, LastUpdateVelocity -- the two "
          "LastUpdate fields being exactly 12 bytes apart is the check that they are the FVector pair "
          "and not two halves of something else",
          hex_offset(dsh::game::kAccelerationOffset) + " / " +
              hex_offset(dsh::game::kLastUpdateLocationOffset) + " / " +
              hex_offset(dsh::game::kLastUpdateVelocityOffset));
    check(dsh::game::kJustTeleportedByteOffset == 0x0387 &&
              dsh::game::kJustTeleportedBitIndex == 5 &&
              dsh::game::kRequestedVelocityFlagOffset == 0x038A &&
              dsh::game::kRequestedVelocityBitIndex == 1,
          "bJustTeleported is 0x0387 bit 5 and bHasRequestedVelocity is 0x038A bit 1 -- a bit index "
          "one out prints the wrong flag and nothing else would say so",
          hex_offset(dsh::game::kJustTeleportedByteOffset) + ":" +
              std::to_string(dsh::game::kJustTeleportedBitIndex) + " / " +
              hex_offset(dsh::game::kRequestedVelocityFlagOffset) + ":" +
              std::to_string(dsh::game::kRequestedVelocityBitIndex));
    check(dsh::game::kRequestedVelocityOffset == 0x03A0,
          "RequestedVelocity is at", hex_offset(dsh::game::kRequestedVelocityOffset));

    check(dsh::game::kNoControllerPhysicsByteOffset == 0x01F1 &&
              dsh::game::kNoControllerPhysicsBitIndex == 2,
          "bRunPhysicsWithNoController is the byte at, bit 2 -- the same byte as "
          "bEnableServerDualMoveScopedMovementUpdates (bit 0), bForceMaxAccel (bit 1) and "
          "bForceNextFloorCheck (bit 3), which is why the BIT INDEX is asserted and not only the offset",
          hex_offset(dsh::game::kNoControllerPhysicsByteOffset) + ":" +
              std::to_string(dsh::game::kNoControllerPhysicsBitIndex));
    check(dsh::game::kDefaultLandMovementModeOffset == 0x0384,
          "DefaultLandMovementMode is at and DefaultWaterMovementMode at -- the mode "
          "the component returns to on landing, read so a mode that reverts is visible rather than "
          "invisible",
          hex_offset(dsh::game::kDefaultLandMovementModeOffset));
    check(dsh::game::kPawnControllerOffset == 0x0258,
          "APawn::Controller is at -- the field that makes 'not driven by input' a different "
          "statement from 'input is zero'",
          hex_offset(dsh::game::kPawnControllerOffset));
    check(dsh::game::kActorReplicatedMovementOffset == 0x0060 &&
              dsh::game::kReplicatedLinearVelocityOffset == 0x0000,
          "AActor::ReplicatedMovement is at actor and its LinearVelocity at of the "
          "struct -- the fallback instrument if writing Velocity turns out to be the ownership fight",
          hex_offset(dsh::game::kActorReplicatedMovementOffset) + " + " +
              hex_offset(dsh::game::kReplicatedLinearVelocityOffset));

    check(std::string(dsh::game::movement_mode_name(dsh::game::kMoveNone)) == "MOVE_None" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveWalking)) == "MOVE_Walking" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveNavWalking)) ==
                  "MOVE_NavWalking" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveFalling)) == "MOVE_Falling" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveSwimming)) == "MOVE_Swimming" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveFlying)) == "MOVE_Flying" &&
              std::string(dsh::game::movement_mode_name(dsh::game::kMoveCustom)) == "MOVE_Custom",
          "all seven EMovementMode values are named, so a log line prints 'MOVE_Flying' and not '5'");
    check(dsh::game::kMoveNone == 0 && dsh::game::kMoveWalking == 1 &&
              dsh::game::kMoveFalling == 3 && dsh::game::kMoveFlying == 5 &&
              dsh::game::kMoveCustom == 6 && dsh::game::kMoveMax == 7,
          "the enum's NUMBERS are the game's, not a remembered order",
          "None=" + std::to_string(dsh::game::kMoveNone) + " Falling=" +
              std::to_string(dsh::game::kMoveFalling) + " Flying=" +
              std::to_string(dsh::game::kMoveFlying));
    check(std::string(dsh::game::movement_mode_name(dsh::game::kMoveMax)) == "?" &&
              std::string(dsh::game::movement_mode_name(200)) == "?",
          "MOVE_MAX (7) and a byte outside the enum are NOT given a mode name -- 7 is the enum's count, "
          "and naming it would be inventing a mode the game does not define");

    check(dsh::game::mode_write_decision(dsh::game::kMoveNone) ==
              dsh::game::ModeWriteDecision::alreadyNone,
          "MOVE_None is NOT written: the component is already in it, so the write could only fail to "
          "change anything -- and a build that wrote it would be unable to distinguish 'the mode was "
          "re-asserted' from 'the mode stood still'");
    {
        bool allRealModesWritten = true;
        for (std::uint8_t mode = dsh::game::kMoveWalking; mode <= dsh::game::kMoveCustom; ++mode) {
            if (dsh::game::mode_write_decision(mode) != dsh::game::ModeWriteDecision::writeOk) {
                allRealModesWritten = false;
            }
        }
        check(allRealModesWritten,
              "all six integrating modes -- Walking, NavWalking, Falling, Swimming, Flying and Custom "
              "-- are written, so a host hovering (Flying) or falling (Falling) is followed by the "
              "puppet's own mode byte and not only by its velocity",
              "1..6 all writeOk");
    }
    {
        bool allOutsideRefused = true;
        for (std::uint8_t mode = dsh::game::kMoveMax; mode <= 8; ++mode) {
            if (dsh::game::mode_write_decision(mode) != dsh::game::ModeWriteDecision::outsideEnum) {
                allOutsideRefused = false;
            }
        }
        check(allOutsideRefused,
              "MOVE_MAX (7, the enum's COUNT and not a mode) and everything above it are REFUSED, so a "
              "byte a wrong offset or a torn read produced is never written into the engine's mode field "
              "-- and the refusal is named rather than silently skipped");
    }
    check(dsh::game::mode_write_decision(0) != dsh::game::mode_write_decision(1) &&
              dsh::game::mode_write_decision(6) != dsh::game::mode_write_decision(7),
          "the two boundaries are where the rule says they are: 0/1 and 6/7 fall on different sides, so "
          "an off-by-one in either direction fails here");

    dsh::game::MachineMovementState st;
    st.readable = true;
    st.actor = 0x12340000;
    st.component = 0x12350000;
    st.componentOffset = dsh::game::kCharacterMovementOffset;
    st.controller = 0x12360000;
    st.gravityScale = 1.75f;
    st.maxWalkSpeed = 600.0f;
    st.maxAcceleration = 2048.0f;
    st.velocity[0] = 0.0f;
    st.velocity[1] = 0.0f;
    st.velocity[2] = 0.0f;
    st.movementMode = dsh::game::kMoveNone;
    st.justTeleported = true;

    const std::string idle = dsh::game::movement_state_line("puppet", st, 0.0f);
    check(idle.find("MovementMode=MOVE_None(0)") != std::string::npos,
          "a MOVE_None puppet prints the mode by NAME beside its number", idle);
    check(idle.find("controller=set") != std::string::npos,
          "and it says whether the puppet has a controller at all, which is what makes its zero "
          "interpretable", idle);
    check(idle.find("component=") != std::string::npos &&
              idle.find("(CharacterMovement)") != std::string::npos,
          "and which of the two component fields the numbers came from", idle);
    check(idle.find("MOVE_None, so PerformMovement is never called") != std::string::npos,
          "and it names the SAFE branch: MOVE_None cannot integrate a written velocity", idle);
    check(idle.find("ReplicatedMovement.LinearVelocity=") != std::string::npos,
          "and it carries the engine's own replicated velocity, which is the fallback instrument for "
          "the round after this one", idle);

    check(idle.find("bRunPhysicsWithNoController=NOT read") != std::string::npos &&
              idle.find("DefaultLandMovementMode=?") != std::string::npos &&
              idle.find("LastUpdateLocation is all zero") == std::string::npos,
          "the line names bRunPhysicsWithNoController and DefaultLandMovementMode, and an unread one "
          "prints NOT read / ? rather than a fabricated 0 -- and the LastUpdateLocation SENTENCE is not "
          "printed either, because a state whose location was never read has no location to describe",
          idle);

    dsh::game::MachineMovementState noController = st;
    noController.controller = 0;
    noController.noControllerPhysicsRead = true;
    noController.runPhysicsWithNoController = false;
    noController.movementMode = dsh::game::kMoveWalking;
    noController.defaultLandMovementModeRead = true;
    noController.defaultLandMovementMode = dsh::game::kMoveWalking;
    const std::string noPhysics =
        dsh::game::movement_state_line("puppet", noController, 0.0f);
    check(noPhysics.find("bRunPhysicsWithNoController=0(no controller and this bit clear") !=
              std::string::npos,
          "a controller-less component with the bit clear says in words that a written Velocity is "
          "inert on it whatever the mode says -- asserted so the wording cannot silently invert",
          noPhysics);
    check(noPhysics.find("MovementMode=MOVE_Walking(1)") != std::string::npos &&
              noPhysics.find("DefaultLandMovementMode=MOVE_Walking(1)") != std::string::npos,
          "and the mode and the landing mode are both named beside their numbers, so a reader does not "
          "have to hold the game", noPhysics);
    check(noPhysics.find("a written Velocity is inert HERE anyway") != std::string::npos &&
              noPhysics.find("OWNERSHIP FIGHT") == std::string::npos &&
              noPhysics.find("IS integrated") == std::string::npos,
          "the branch for that case says the mode is what the animation graph reads and does NOT "
          "announce an ownership fight that the actor cannot have", noPhysics);

    dsh::game::MachineMovementState controllerRun = noController;
    controllerRun.controller = 0x12360000;
    const std::string fightLive =
        dsh::game::movement_state_line("puppet", controllerRun, 0.0f);
    check(fightLive.find("this actor can run physics") != std::string::npos &&
              fightLive.find("IS integrated") != std::string::npos &&
              fightLive.find("ownership fight") != std::string::npos,
          "an integrating mode on an actor that CAN run physics names the ownership fight as live -- "
          "the branch the run exists to reach", fightLive);

    dsh::game::MachineMovementState located = controllerRun;
    located.lastUpdateLocationRead = true;
    const std::string locatedZero = dsh::game::movement_state_line("puppet", located, 0.0f);
    check(locatedZero.find("[LastUpdateLocation is all zero:") != std::string::npos,
          "a READ location that is all zero is said in words to mean no movement update has moved this "
          "component's record", locatedZero);
    dsh::game::MachineMovementState moved = located;
    moved.lastUpdateLocation[0] = 1234.5f;
    moved.lastUpdateLocation[1] = -6.25f;
    check(dsh::game::movement_state_line("puppet", moved, 0.0f)
              .find("[LastUpdateLocation is NON-ZERO, so this component HAS performed a movement "
                    "update]") != std::string::npos,
          "and a non-zero one is said to mean the component HAS performed a movement update -- the "
          "statement the per-window distance on the two-second line is built on");
    check(noPhysics.find("[LastUpdateLocation is all zero:") == std::string::npos,
          "a state whose location was NOT read prints neither sentence: 'all zero' is a measurement and "
          "a refused read is not, which is the rule this project has paid for twice");

    dsh::game::MachineMovementState bitUnread = noController;
    bitUnread.noControllerPhysicsRead = false;
    const std::string undecidedMode =
        dsh::game::movement_state_line("puppet", bitUnread, 0.0f);
    check(undecidedMode.find("bRunPhysicsWithNoController=NOT read") != std::string::npos &&
              undecidedMode.find("UNDECIDED") != std::string::npos,
          "an unread bRunPhysicsWithNoController prints NOT read and makes the branch UNDECIDED rather "
          "than assuming the safe answer", undecidedMode);

    const std::string hostSeen = dsh::game::movement_state_line("puppet", st, 812.5f);
    check(hostSeen.find("current=0.0 max=812.5") != std::string::npos,
          "the current speed and the WINDOW MAXIMUM are printed as two different numbers -- a point "
          "sample of a fluctuating value is what this round was told not to rely on", hostSeen);

    dsh::game::MachineMovementState moving = st;
    moving.velocity[0] = 3.0f;
    moving.velocity[1] = 4.0f;
    moving.velocity[2] = 0.0f;
    const std::string alive = dsh::game::movement_state_line("puppet", moving, 5.0f);
    check(alive.find("Velocity=(3.0, 4.0, 0.0)") != std::string::npos,
          "a non-zero velocity is printed verbatim: |(3,4,0)| is 5", alive);
    check(alive.find("the component's Velocity is non-zero") != std::string::npos &&
              alive.find("something else put there") != std::string::npos &&
              alive.find("MOVE_None, so PerformMovement is never called") == std::string::npos,
          "and the line says the component is ALREADY holding a velocity this build did not necessarily "
          "put there -- the branch that kills 'nothing has ever written Velocity', and it must not fall "
          "through to the MOVE_None sentence", alive);
    check(dsh::game::MachineMovementState{.readable = true}.speed() == 0.0f,
          "a zero velocity has a speed of exactly zero, so a measured standstill cannot be mistaken "
          "for a read that refused");

    dsh::game::MachineMovementState flying = st;
    flying.movementMode = dsh::game::kMoveFlying;
    const std::string fight = dsh::game::movement_state_line("puppet", flying, 0.0f);
    check(fight.find("MovementMode=MOVE_Flying(5)") != std::string::npos &&
              fight.find("MOVE_None, so PerformMovement is never called") == std::string::npos,
          "an integrating mode is named by name and does not print the MOVE_None safe-branch sentence",
          fight);

    dsh::game::MachineMovementState weird = st;
    weird.movementMode = 9;
    check(dsh::game::movement_state_line("puppet", weird, 0.0f).find("UNDECIDED") !=
              std::string::npos,
          "a MovementMode outside the enum prints as undecided, not as a mode");

    dsh::game::MachineMovementState refused;
    refused.actor = 0x12340000;
    refused.note = "the actor's class chain does not reach ADACharacter";
    const std::string refusal = dsh::game::movement_state_line("puppet", refused, 0.0f);
    check(refusal.find("NOT readable") != std::string::npos &&
              refusal.find("MovementMode=") == std::string::npos &&
              refusal.find("Velocity=") == std::string::npos,
          "a refused read prints the refusal and no field values -- 'a plausible MOVE_None nobody can "
          "contradict' is exactly the failure this project has shipped twice", refusal);
    check(refusal.find("controller=NONE") != std::string::npos,
          "and even a refusal says whether the actor has a controller", refusal);

    const std::string withTrailing = dsh::game::movement_state_line(
        "puppet", st, 0.0f,
        "velocity written=(0.0, 0.0, 0.0) applied=7 LastUpdateVelocity held the last written value in "
        "7 of 9 sample(s)");
    check(withTrailing.find("MOVE_None, so PerformMovement is never called") != std::string::npos &&
              withTrailing.find("velocity written=(0.0, 0.0, 0.0)") != std::string::npos &&
              withTrailing.find("in 7 of 9 sample(s)") != std::string::npos,
          "a caller's velocity/kept writeup rides the SAME line and the branch sentence survives it",
          withTrailing);
    check(dsh::game::movement_state_line("puppet", st, 0.0f, "") ==
              dsh::game::movement_state_line("puppet", st, 0.0f),
          "an empty trailing block prints nothing at all, so the callers with nothing to add do not "
          "have to say so");
    check(idle.find("velocity written=") == std::string::npos,
          "and the default argument adds no field to a line whose caller passed nothing");
}

void test_machine_movement_class_chain() {
    section("movement: the class-chain check in front of and");

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    const std::uintptr_t classOther     = mem.at(synth::kClassOther);
    const std::uintptr_t cyclic         = mem.at(synth::kCyclic);

    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, classOther);
    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kCyclic + synth::kUStructSuperOffset, cyclic);

    const dsh::game::MachineMovementState noClass =
        dsh::game::movement_state_probe(mem.at(synth::kActor), 0, SyntheticMemory::read);
    check(!noClass.readable &&
              noClass.note.find("ADACharacter") != std::string::npos,
          "a zero class pointer refuses BY NAME before any read -- without the class there is nothing "
          "to check the offsets against", noClass.note);

    mem.touchedCharacterMovement = false;
    mem.touchedDACharacterMovement = false;
    const dsh::game::MachineMovementState sibling =
        dsh::game::movement_state_probe(mem.at(synth::kSiblingActor), classCharacter,
                                        SyntheticMemory::watch_read);
    check(!sibling.readable,
          "an actor whose chain reaches a DIFFERENT root is refused", sibling.note);
    check(!mem.touchedCharacterMovement && !mem.touchedDACharacterMovement,
          "and NEITHER component pointer was opened on the way to that refusal -- the guard refusing "
          "is not the same claim as the guard refusing BEFORE the read, and only the second one is safe");

    std::uintptr_t component = mem.at(synth::kComponent);
    mem.poke(synth::kActor + dsh::game::kCharacterMovementOffset, component);
    mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, 0);

    const float velocity[3] = {7.0f, -3.5f, 42.25f};
    const float acceleration[3] = {1.5f, 2.5f, -3.5f};
    const float requested[3] = {-11.0f, 12.0f, 13.0f};
    const float lastVel[3] = {100.0f, 200.0f, 300.0f};
    const float lastLoc[3] = {1000.0f, 2000.0f, 3000.0f};
    const float replicated[3] = {250.0f, 0.0f, -75.0f};
    mem.poke_bytes(synth::kComponent + dsh::game::kMovementVelocityOffset, velocity, sizeof(velocity));
    mem.poke_bytes(synth::kComponent + dsh::game::kAccelerationOffset, acceleration,
                   sizeof(acceleration));
    mem.poke_bytes(synth::kComponent + dsh::game::kRequestedVelocityOffset, requested,
                   sizeof(requested));
    mem.poke_bytes(synth::kComponent + dsh::game::kLastUpdateVelocityOffset, lastVel, sizeof(lastVel));
    mem.poke_bytes(synth::kComponent + dsh::game::kLastUpdateLocationOffset, lastLoc, sizeof(lastLoc));
    mem.poke_bytes(synth::kActor + dsh::game::kActorReplicatedMovementOffset +
                       dsh::game::kReplicatedLinearVelocityOffset,
                   replicated, sizeof(replicated));
    mem.poke_f32(synth::kComponent + dsh::game::kGravityScaleOffset, 1.75f);
    mem.poke_f32(synth::kComponent + dsh::game::kMaxWalkSpeedOffset, 600.0f);
    mem.poke_f32(synth::kComponent + dsh::game::kMaxAccelerationOffset, 2048.0f);
    mem.poke_byte(synth::kComponent + dsh::game::kMovementModeOffset, dsh::game::kMoveFlying);
    mem.poke_byte(synth::kComponent + dsh::game::kCustomMovementModeOffset, 4);

    mem.poke_byte(synth::kComponent + dsh::game::kJustTeleportedByteOffset,
                  static_cast<std::uint8_t>(1u << dsh::game::kJustTeleportedBitIndex));
    mem.poke_byte(synth::kComponent + dsh::game::kRequestedVelocityFlagOffset,
                  static_cast<std::uint8_t>(1u << dsh::game::kRequestedVelocityBitIndex));

    mem.poke_byte(synth::kComponent + dsh::game::kNoControllerPhysicsByteOffset,
                  static_cast<std::uint8_t>((1u << 0) | (1u << 2) | (1u << 3)));
    mem.poke_byte(synth::kComponent + dsh::game::kDefaultLandMovementModeOffset,
                  dsh::game::kMoveFalling);
    mem.poke(synth::kActor + dsh::game::kPawnControllerOffset, mem.at(synth::kController));

    mem.touchedCharacterMovement = false;
    mem.touchedDACharacterMovement = false;
    const dsh::game::MachineMovementState got =
        dsh::game::movement_state_probe(mem.at(synth::kActor), classCharacter,
                                        SyntheticMemory::watch_read);
    check(got.readable, "an actor whose chain DOES reach ADACharacter is read", got.note);
    check(mem.touchedCharacterMovement && mem.touchedDACharacterMovement,
          "THE CONTROL: the same watch sees BOTH component pointers opened -- so the refusal assertion "
          "above is about the guard, not about a probe that never looks");

    check(got.movementMode == dsh::game::kMoveFlying && got.customMovementMode == 4,
          "MovementMode is read from and CustomMovementMode from",
          "mode=" + std::to_string(got.movementMode) + " custom=" +
              std::to_string(got.customMovementMode));
    check(got.velocity[0] == 7.0f && got.velocity[1] == -3.5f && got.velocity[2] == 42.25f,
          "Velocity is read from component, all three components",
          "(" + std::to_string(got.velocity[0]) + ", " + std::to_string(got.velocity[1]) + ", " +
              std::to_string(got.velocity[2]) + ")");
    check(got.acceleration[0] == 1.5f && got.acceleration[1] == 2.5f && got.acceleration[2] == -3.5f,
          "Acceleration is read from");
    check(got.requestedVelocity[0] == -11.0f && got.requestedVelocity[2] == 13.0f,
          "RequestedVelocity is read from");
    check(got.lastUpdateVelocity[0] == 100.0f && got.lastUpdateVelocity[2] == 300.0f &&
              got.lastUpdateLocation[0] == 1000.0f && got.lastUpdateLocation[2] == 3000.0f,
          "LastUpdateVelocity and LastUpdateLocation are read as the two DIFFERENT "
          "twelve-byte vectors they are -- a 12-byte slip between them would swap them silently");

    check(got.lastUpdateLocationRead,
          "LastUpdateLocation's read is recorded on its own flag, so the line can tell a measured zero "
          "from a location that was never read -- the two are the same twelve bytes and different "
          "statements");
    check(got.replicatedLinearVelocity[0] == 250.0f && got.replicatedLinearVelocity[2] == -75.0f,
          "AActor::ReplicatedMovement.LinearVelocity is read from actor");
    check(got.gravityScale == 1.75f && got.maxWalkSpeed == 600.0f && got.maxAcceleration == 2048.0f,
          "GravityScale, MaxWalkSpeed and MaxAcceleration are the three floats at//"
          "");
    check(got.justTeleported && got.hasRequestedVelocity,
          "bJustTeleported (0x0387 bit 5) and bHasRequestedVelocity (0x038A bit 1) are both true here, "
          "and the neighbouring bits are zero -- so a wrong bit index fails");

    check(got.noControllerPhysicsRead && got.runPhysicsWithNoController,
          "bRunPhysicsWithNoController is read from bit 2, with bits 0 and 3 set and bit 1 "
          "clear -- a probe that tested the wrong bit or the whole byte fails here");
    check(got.defaultLandMovementModeRead &&
              got.defaultLandMovementMode == dsh::game::kMoveFalling,
          "DefaultLandMovementMode is read from and NOT from, where "
          "DefaultWaterMovementMode sits -- a one-byte slip between two adjacent EMovementMode fields "
          "would otherwise be invisible",
          "mode=" + std::to_string(got.defaultLandMovementMode) + " (" +
              dsh::game::movement_mode_name(got.defaultLandMovementMode) + ")");
    check(got.controller == mem.at(synth::kController),
          "the actor's Controller is read from APawn -- the fact that makes the puppet's own "
          "zeros interpretable");
    check(got.componentOffset == dsh::game::kCharacterMovementOffset &&
              !got.useDACharacterMovement && got.otherComponent == 0,
          "the component that answered is named, and it is the ACharacter one here",
          hex_offset(got.componentOffset));
    check(got.speed() > 41.0f && got.speed() < 44.0f,
          "and the speed this line prints is the magnitude of that velocity",
          "speed=" + std::to_string(got.speed()));

    mem.poke(synth::kActor + dsh::game::kCharacterMovementOffset, 0);
    mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, component);
    const dsh::game::MachineMovementState viaDa =
        dsh::game::movement_state_probe(mem.at(synth::kActor), classCharacter, SyntheticMemory::read);
    check(viaDa.readable && viaDa.useDACharacterMovement &&
              viaDa.componentOffset == dsh::game::kDACharacterMovementOffset,
          "with null the component answers, and the state records WHICH one it was",
          hex_offset(viaDa.componentOffset));
    check(dsh::game::movement_state_line("puppet", viaDa, 0.0f).find("(DACharacterMovement)") !=
              std::string::npos,
          "and the line names it, so the numbers are traceable to one field");

    mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, 0);
    const dsh::game::MachineMovementState none =
        dsh::game::movement_state_probe(mem.at(synth::kActor), classCharacter, SyntheticMemory::read);
    check(!none.readable && none.component == 0 &&
              none.note.find("no MovementMode to report") != std::string::npos,
          "both pointers null is a refusal that says there is no mode to report, not MOVE_None",
          none.note);
    check(dsh::game::movement_state_line("puppet", none, 0.0f).find("MOVE_None") ==
              std::string::npos,
          "and the line never prints a mode for it");

    mem.poke(synth::kActor + synth::kUObjectClassOffset, cyclic);
    const dsh::game::MachineMovementState looped =
        dsh::game::movement_state_probe(mem.at(synth::kActor), classCharacter, SyntheticMemory::read);
    check(!looped.readable, "a class chain that loops terminates in a refusal, not a spin");

    check(!dsh::game::movement_state_probe(0x10, classCharacter, SyntheticMemory::read).readable,
          "an implausible actor pointer is refused before any read");
    check(!dsh::game::movement_state_probe(mem.at(synth::kActor), classCharacter, nullptr).readable,
          "a null reader is refused rather than dereferenced");

    const float sendVelocity[3] = {686.1f, 0.0f, 107.5f};
    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kActor + dsh::game::kCharacterMovementOffset, component);
    mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, 0);
    mem.poke_bytes(synth::kComponent + dsh::game::kMovementVelocityOffset, sendVelocity,
                   sizeof(sendVelocity));

    {
        std::uintptr_t offset = 0;
        std::uintptr_t other = 0;
        const std::uintptr_t viaA =
            dsh::game::movement_component_address(mem.at(synth::kActor), offset, other,
                                                  SyntheticMemory::read);
        check(viaA == component && offset == dsh::game::kCharacterMovementOffset && other == 0,
              "ACharacter's is preferred, and the offset it answered at is reported",
              hex_offset(offset));
    }
    {

        mem.poke(synth::kActor + dsh::game::kCharacterMovementOffset, 0);
        mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, component);
        std::uintptr_t offset = 0;
        std::uintptr_t other = 0;
        const std::uintptr_t viaDaField =
            dsh::game::movement_component_address(mem.at(synth::kActor), offset, other,
                                                  SyntheticMemory::read);
        check(viaDaField == component && offset == dsh::game::kDACharacterMovementOffset,
              "with the ACharacter pointer null the DACharacter one answers, and the offset says so",
              hex_offset(offset));

        dsh::game::MachineVelocity vel;
        check(dsh::game::read_machine_velocity(mem.at(synth::kActor), classCharacter, vel,
                                               SyntheticMemory::read),
              "and the velocity read finds it through the same fallback", vel.note);
        check(vel.value[0] == 686.1f && vel.value[1] == 0.0f && vel.value[2] == 107.5f &&
                  vel.component == component &&
                  vel.componentOffset == dsh::game::kDACharacterMovementOffset,
              "the three floats come from component + 0x00C4 and the state names the component it was",
              "(" + std::to_string(vel.value[0]) + ", " + std::to_string(vel.value[1]) + ", " +
                  std::to_string(vel.value[2]) + ") offset=" + hex_offset(vel.componentOffset));
    }
    {

        mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, 0);
        dsh::game::MachineVelocity vel;
        check(!dsh::game::read_machine_velocity(mem.at(synth::kActor), classCharacter, vel,
                                                SyntheticMemory::read) &&
                  vel.note.find("neither movement component pointer is set") != std::string::npos,
              "with both pointers null the velocity read refuses BY NAME", vel.note);
    }
    {

        mem.touchedCharacterMovement = false;
        mem.touchedDACharacterMovement = false;
        dsh::game::MachineVelocity vel;
        const bool readOk = dsh::game::read_machine_velocity(mem.at(synth::kSiblingActor),
                                                             classCharacter, vel,
                                                             SyntheticMemory::watch_read);
        check(!readOk && vel.note.find("class chain does not reach") != std::string::npos,
              "an actor whose chain does not reach DACharacter is refused by name", vel.note);
        check(!mem.touchedCharacterMovement && !mem.touchedDACharacterMovement,
              "and NEITHER component pointer was opened on the way to that refusal -- the same "
              "property the probe's guard is asserted on");
    }

    SyntheticMemory::set_active(nullptr);
}

void test_machine_movement_input() {
    section("movement input: the six Net fields, and the class-chain check in front of them");

    check(dsh::game::kCharacterIgnoreFrictionOffset == 0x06E0 &&
              dsh::game::kCharacterHoverOffset == 0x06E1 &&
              dsh::game::kCharacterSprintOffset == 0x06E2,
          "the three contiguous flags are at, and",
          std::to_string(dsh::game::kCharacterIgnoreFrictionOffset) + ", " +
              std::to_string(dsh::game::kCharacterHoverOffset) + ", " +
              std::to_string(dsh::game::kCharacterSprintOffset));
    check(dsh::game::kCharacterBoostJumpOffset == 0x06F0 && dsh::game::kCharacterBoostDashOffset == 0x06F1,
          "and the two BOOST bytes are at and -- NOT adjacent to the three above, which "
          "is why the write is two groups and why the wire keeps the two in their own byte pairs",
          std::to_string(dsh::game::kCharacterBoostJumpOffset) + ", " +
              std::to_string(dsh::game::kCharacterBoostDashOffset));
    check(dsh::game::kCharacterRecentInputOffset == 0x06F4,
          "and the control-input vector is at, twelve bytes (FVector: three float32)",
          std::to_string(dsh::game::kCharacterRecentInputOffset));
    check(dsh::game::kMovementInputVectorEnd == 0x0700 &&
              dsh::game::kMovementInputVectorEnd - dsh::game::kCharacterRecentInputOffset == 12,
          "so it ends exactly where the next Net field begins, at (IntermissionTime)",
          "end=" + std::to_string(dsh::game::kMovementInputVectorEnd));

    check(dsh::game::kMovementInputFlagGroupEnd == 0x06FC &&
              dsh::game::kCharacterRecentInputOffset < dsh::game::kMovementInputFlagGroupEnd,
          "the flag group's read-modify-write range ends at and CONTAINS, so the flag "
          "group must be written before the vector or its restored tail would erase it",
          "groupEnd=" + std::to_string(dsh::game::kMovementInputFlagGroupEnd));
    check(std::string(dsh::game::kMovementInputApiClass) == "DACharacter",
          "and the class the guard checks for is named WITHOUT the C++ A prefix -- the spelling that "
          "resolves nothing cost this project a round already",
          dsh::game::kMovementInputApiClass);

    {
        dsh::game::MovementInputs shown;
        shown.boostJump = 1;
        shown.recentInput[1] = 42.5f;
        const std::string line = dsh::game::movement_input_line(shown);
        check(line.find("boostJump=1") != std::string::npos &&
                  line.find("boostDash=0") != std::string::npos &&
                  line.find("hover=0") != std::string::npos &&
                  line.find("42.5") != std::string::npos,
              "the line names all five flags and prints the vector, so the two gaps this round closes "
              "can be read off it without the game", line);
    }

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    const std::uintptr_t classOther     = mem.at(synth::kClassOther);
    const std::uintptr_t cyclic         = mem.at(synth::kCyclic);

    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, classOther);
    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kCyclic + synth::kUStructSuperOffset, cyclic);

    dsh::game::MovementInputs none;
    std::string noneNote;
    check(!dsh::game::read_movement_inputs(mem.at(synth::kActor), 0, none, noneNote,
                                           SyntheticMemory::read) &&
              noneNote.find("DACharacter") != std::string::npos,
          "a zero class pointer refuses BY NAME before any read", noneNote);

    dsh::game::MovementInputs neverRead;
    std::string refusal;
    check(!dsh::game::read_movement_inputs(mem.at(synth::kSiblingActor), classCharacter, neverRead,
                                           refusal, SyntheticMemory::watch_read),
          "an actor that is NOT an ADACharacter is REFUSED, and nothing is read from its",
          refusal);
    check(refusal.find("DACharacter") != std::string::npos,
          "the refusal NAMES the class it was looking for, so 'failed' cannot be misread as a different "
          "problem", refusal);
    check(!mem.touchedMovementInput,
          "and the refused actor's own was never opened -- the refusal is the guard working, not "
          "a read that happened to fail");

    mem.poke_byte(synth::kActor + dsh::game::kCharacterIgnoreFrictionOffset, 1);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterHoverOffset, 0);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterSprintOffset, 1);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterBoostJumpOffset, 0);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterBoostDashOffset, 1);
    const float recent[3] = {-0.5f, 640.25f, 7.5f};
    mem.poke_bytes(synth::kActor + dsh::game::kCharacterRecentInputOffset, recent, sizeof(recent));

    mem.touchedMovementInput = false;
    dsh::game::MovementInputs got;
    std::string gotNote;
    check(dsh::game::read_movement_inputs(mem.at(synth::kActor), classCharacter, got, gotNote,
                                          SyntheticMemory::watch_read),
          "an actor whose chain DOES reach DACharacter is read", gotNote);
    check(mem.touchedMovementInput,
          "THE CONTROL: the same watch sees opened -- so the refusal assertion above is about "
          "the guard, not about a read that never looks");
    check(got.ignoreFriction == 1 && got.hover == 0 && got.sprint == 1,
          "the three contiguous flags are read from//",
          "ignoreFriction=" + std::to_string(got.ignoreFriction) + " hover=" +
              std::to_string(got.hover) + " sprint=" + std::to_string(got.sprint));
    check(got.boostJump == 0 && got.boostDash == 1,
          "and the two BOOST bytes from/ -- through the thirteen-byte gap, not from "
          "/ where a contiguous read would have put them",
          "boostJump=" + std::to_string(got.boostJump) + " boostDash=" +
              std::to_string(got.boostDash));
    check(got.recentInput[0] == -0.5f && got.recentInput[1] == 640.25f && got.recentInput[2] == 7.5f,
          "and the control-input vector from, all three components",
          "(" + std::to_string(got.recentInput[0]) + ", " + std::to_string(got.recentInput[1]) + ", " +
              std::to_string(got.recentInput[2]) + ")");

    mem.poke(synth::kActor + synth::kUObjectClassOffset, cyclic);
    dsh::game::MovementInputs looped;
    std::string loopNote;
    check(!dsh::game::read_movement_inputs(mem.at(synth::kActor), classCharacter, looped, loopNote,
                                           SyntheticMemory::read),
          "a class chain that loops terminates in a refusal, not a spin");

    check(!dsh::game::read_movement_inputs(0x10, classCharacter, looped, loopNote,
                                           SyntheticMemory::read),
          "an implausible actor pointer is refused before any read");
    check(!dsh::game::read_movement_inputs(mem.at(synth::kActor), classCharacter, looped, loopNote,
                                           nullptr),
          "a null reader is refused rather than dereferenced");

    {
        dsh::game::MovementInputs a;
        dsh::game::MovementInputs b;
        a.hover = 1;
        check(a != b && !a.flagsSame(b) && a.vectorSame(b),
              "a difference in one flag makes the FLAG group differ and leaves the VECTOR group equal",
              dsh::game::movement_input_line(a));
        dsh::game::MovementInputs c;
        dsh::game::MovementInputs d;
        d.recentInput[2] = 1.0f;
        check(c != d && c.flagsSame(d) && !c.vectorSame(d),
              "and a difference in the vector makes the VECTOR group differ and leaves the FLAGS equal "
              "-- which is what keeps 'the host never boosted' readable as a flag count that stops "
              "moving while the aim count climbs");
    }

    {
        dsh::game::EnergyTimers noClass;
        std::string noClassNote;
        check(!dsh::game::read_energy_timers(mem.at(synth::kActor), 0, noClass, noClassNote,
                                             SyntheticMemory::read) &&
                  noClassNote.find("DACharacter") != std::string::npos,
              "a zero class pointer refuses BY NAME before either energy timer is read", noClassNote);
        check(dsh::game::kMovementInputVectorEnd == dsh::game::kCharacterIntermissionTimeOffset &&
                  dsh::game::kEnergyTimerBytes == 8,
              "and the control-input vector ends exactly where IntermissionTime begins -- so the two "
              "timers are the next thing in the class's layout and nothing sits between them",
              "vectorEnd=" + std::to_string(dsh::game::kMovementInputVectorEnd));
    }
    {

        dsh::game::EnergyTimers neverReadTimers;
        std::string energyRefusal;
        mem.touchedEnergyTimers = false;
        check(!dsh::game::read_energy_timers(mem.at(synth::kSiblingActor), classCharacter, neverReadTimers,
                                             energyRefusal, SyntheticMemory::watch_read),
              "an actor that is NOT an ADACharacter is REFUSED for the energy timers too", energyRefusal);
        check(!mem.touchedEnergyTimers,
              "and its own was never opened -- the refusal is the guard working, not a read that "
              "happened to fail");
    }
    {

        mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
        const float timers[2] = {0.125f, -3.5f};
        mem.poke_bytes(synth::kActor + dsh::game::kCharacterIntermissionTimeOffset, timers,
                       sizeof(timers));
        mem.touchedEnergyTimers = false;
        dsh::game::EnergyTimers gotTimers;
        std::string timersNote;
        check(dsh::game::read_energy_timers(mem.at(synth::kActor), classCharacter, gotTimers, timersNote,
                                            SyntheticMemory::watch_read),
              "an actor whose chain DOES reach DACharacter has its two energy timers read", timersNote);
        check(mem.touchedEnergyTimers,
              "THE CONTROL: the same watch sees the span opened -- so the refusal assertion above "
              "is about the guard, not about a read that never looks");
        check(gotTimers.intermissionTime == 0.125f && gotTimers.fastChargeStartTime == -3.5f,
              "IntermissionTime comes from and EnergyFastChargeStartTime from, in that "
              "order, as the game declares them",
              "(" + std::to_string(gotTimers.intermissionTime) + ", " +
                  std::to_string(gotTimers.fastChargeStartTime) + ")");
    }
    {

        dsh::game::EnergyTimers a;
        dsh::game::EnergyTimers b;
        b.fastChargeStartTime = 1.0f;
        check(a != b,
              "a difference in one of the two timers makes the pair differ, which is what opens the write",
              "(" + std::to_string(a.intermissionTime) + ", " +
                  std::to_string(a.fastChargeStartTime) + ")");
        check(a == dsh::game::EnergyTimers{},
              "and two default-constructed pairs are equal, so a session where neither side has a value "
              "does not write on every datagram");
    }
    {

        const std::string notRead =
            dsh::game::energy_timer_line(dsh::game::EnergyTimers{}, false, false, 0.0f, "the class "
                                                                                          "resolved to 0");
        check(notRead.find("NOT read") != std::string::npos &&
                  notRead.find("the class resolved to 0") != std::string::npos &&
                  notRead.find("0.000") == std::string::npos,
              "a refused read prints NOT read with its reason and NO numbers", notRead);
        dsh::game::EnergyTimers shown;
        shown.intermissionTime = 12.5f;
        shown.fastChargeStartTime = 400.0f;
        const std::string first =
            dsh::game::energy_timer_line(shown, true, false, 0.0f, std::string());
        check(first.find("IntermissionTime=12.500") != std::string::npos &&
                  first.find("EnergyFastChargeStartTime=400.000") != std::string::npos &&
                  first.find("none yet") != std::string::npos,
              "a readable pair prints both values, and the first one says there is no step yet", first);
        const std::string second =
            dsh::game::energy_timer_line(shown, true, true, 12.450f, std::string());
        check(second.find("0.050") != std::string::npos,
              "and a step against a previous readable sample is printed, which is the only thing on this "
              "line that can say what the host's numbers ARE rather than what they are equal to", second);
    }

    SyntheticMemory::set_active(nullptr);
}

void test_machine_front_weapon() {
    section("the host's front weapon: the reader, the class chain and the one byte");

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    const std::uintptr_t classMachine   = mem.at(synth::kClassMachine);
    const std::uintptr_t classOther     = mem.at(synth::kClassOther);

    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, classOther);
    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, classMachine);
    mem.poke(synth::kClassMachine + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);

    check(dsh::game::kMachineFrontWeaponInventoryOffset == 0x1620 &&
              dsh::game::kMachineFrontWeaponSpanBytes == 0x18,
          "the three pointers are one 0x18-byte span from: the array's two elements and "
          "CurrentFrontWeapon at",
          "span=" + std::to_string(dsh::game::kMachineFrontWeaponSpanBytes));
    check(std::string(dsh::game::kFrontWeaponApiClass) == "DAMachine",
          "and the class the guard asks for is DAMachine -- the UClass object's own name, WITHOUT the C++ "
          "A prefix, which is the mistake that has cost this project six rounds",
          dsh::game::kFrontWeaponApiClass);

    {

        dsh::game::FrontWeaponState noClass;
        std::string noClassNote;
        mem.touchedFrontWeapon = false;
        check(!dsh::game::read_front_weapon(mem.at(synth::kActor), 0, noClass, noClassNote,
                                            SyntheticMemory::read) &&
                  noClassNote.find("DAMachine") != std::string::npos,
              "a zero class pointer refuses BY NAME before a single weapon pointer is read", noClassNote);
        check(!mem.touchedFrontWeapon, "and was never opened");
    }
    {

        dsh::game::FrontWeaponState neverRead;
        std::string refusal;
        mem.touchedFrontWeapon = false;
        check(!dsh::game::read_front_weapon(mem.at(synth::kSiblingActor), classMachine, neverRead,
                                            refusal, SyntheticMemory::watch_read),
              "an actor whose chain reaches a DIFFERENT root is refused", refusal);
        check(!mem.touchedFrontWeapon,
              "and ITS was never opened -- the refusal is the guard working, not a read that "
              "happened to fail");
    }
    {

        mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, 0);
        check(dsh::game::actor_class_chain_reaches(mem.at(synth::kActor), classCharacter,
                                                   SyntheticMemory::read),
              "CONTROL: this actor's chain still reaches DACharacter after the cut");
        dsh::game::FrontWeaponState humanoidShaped;
        std::string humanoidNote;
        mem.touchedFrontWeapon = false;
        check(!dsh::game::read_front_weapon(mem.at(synth::kActor), classMachine, humanoidShaped,
                                            humanoidNote, SyntheticMemory::watch_read) &&
                  humanoidNote.find("DAMachine") != std::string::npos,
              "an actor that reaches DACharacter but NOT DAMachine is REFUSED -- the guard is the class "
              "these three pointers are declared on, not the class the other readers use", humanoidNote);
        check(!mem.touchedFrontWeapon, "and was not opened for it either");
        mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, classMachine);
    }
    {

        const std::uintptr_t primary   = mem.at(0x6000);
        const std::uintptr_t secondary = mem.at(0x6100);
        const std::uintptr_t other     = mem.at(0x6200);
        mem.poke(synth::kActor + dsh::game::kMachineFrontWeaponInventoryOffset, primary);
        mem.poke(synth::kActor + dsh::game::kMachineFrontWeaponInventoryOffset + sizeof(std::uintptr_t),
                 secondary);
        mem.poke(synth::kActor + dsh::game::kMachineCurrentFrontWeaponOffset, secondary);

        dsh::game::FrontWeaponState got;
        std::string gotNote;
        mem.touchedFrontWeapon = false;
        check(dsh::game::read_front_weapon(mem.at(synth::kActor), classMachine, got, gotNote,
                                           SyntheticMemory::watch_read),
              "an actor whose chain DOES reach DAMachine has its three pointers read", gotNote);
        check(mem.touchedFrontWeapon,
              "THE CONTROL: the watch sees the span opened -- so the refusal assertions above are "
              "about the guard, not about a reader that never looks");
        check(got.inventory[0] == primary && got.inventory[1] == secondary && got.current == secondary,
              "FrontWeaponInventory[0] comes from, [1] from and CurrentFrontWeapon from "
              ", in that order, as the game declares them");

        check(dsh::game::front_weapon_index(got) == dsh::game::kFrontWeaponSlot1,
              "CurrentFrontWeapon pointing at the SECOND slot classifies as slot 1");
        got.current = primary;
        check(dsh::game::front_weapon_index(got) == dsh::game::kFrontWeaponSlot0,
              "and pointing at the FIRST slot classifies as slot 0");
        got.current = 0;
        check(dsh::game::front_weapon_index(got) == dsh::game::kFrontWeaponNone,
              "a NULL CurrentFrontWeapon is the game's own nothing-in-hand state, NOT slot 0 -- the value "
              "EVIDENCE-0025 read off a successful build");
        got.current = other;
        check(dsh::game::front_weapon_index(got) == dsh::game::kFrontWeaponNone,
              "and a CurrentFrontWeapon that is neither inventory entry is that same state rather than a "
              "guess at one of the two");
        got.current = 0;
        got.inventory[0] = 0;
        got.inventory[1] = 0;
        check(dsh::game::front_weapon_index(got) == dsh::game::kFrontWeaponNone,
              "and a machine that holds no front weapons at all classifies the same way, so a NULL there "
              "is never reported as a slot");
    }
    {

        const std::string slot0(dsh::game::front_weapon_wire_name(dsh::game::kFrontWeaponSlot0));
        const std::string slot1(dsh::game::front_weapon_wire_name(dsh::game::kFrontWeaponSlot1));
        const std::string none(dsh::game::front_weapon_wire_name(dsh::game::kFrontWeaponNone));
        const std::string notRead(dsh::game::front_weapon_wire_name(dsh::game::kFrontWeaponNotRead));
        check(slot0.find("") != std::string::npos && slot1.find("") != std::string::npos,
              "the two slots are named WITH THE OFFSET THEY CAME FROM, so a log line about slot 1 names "
              "the pointer it is about", slot0 + " / " + slot1);
        check(none.find("nothing-drawn") != std::string::npos && none != slot0 && none != slot1,
              "the nothing-in-hand state is a sentence of its own and not the name of a slot", none);
        check(notRead.find("NOT read") != std::string::npos,
              "and the refusal is named as a refusal", notRead);
        check(std::string(dsh::game::front_weapon_wire_name(7)) == "?",
              "a byte this build never writes is printed as '?' rather than given a name");
        const std::string notReceived =
            dsh::game::front_weapon_line(dsh::game::kFrontWeaponNotRead, false,
                                         "no state payload has been decoded on this side");
        check(notReceived.find("NOT read or received") != std::string::npos &&
                  notReceived.find("no state payload") != std::string::npos,
              "and a line for a byte that never arrived says so with its reason instead of printing a "
              "value", notReceived);
    }

    SyntheticMemory::set_active(nullptr);
}

namespace modulewalk {

constexpr std::size_t kMachine = 0x0000;
constexpr std::size_t kMachineName = 0x0800;
constexpr std::size_t kMachineClass = 0x0900;
constexpr std::size_t kModule = 0x1000;
constexpr std::size_t kModuleName = 0x1800;
constexpr std::size_t kModuleClass = 0x1900;
constexpr std::size_t kMesh = 0x2000;
constexpr std::size_t kNotNiagara = 0x3000;
constexpr std::size_t kNiagaraBase = 0x4000;
constexpr std::size_t kMachineOwnedNiagara = 0x5000;
constexpr std::size_t kForeign = 0x6000;
constexpr std::size_t kForeignActor = 0x6400;
constexpr std::size_t kSkeletalMeshClass = 0x7000;
constexpr std::size_t kNiagaraClass = 0x7800;
constexpr std::size_t kNiagaraAsset = 0x8000;

}

SyntheticMemory* g_moduleWalkMemory = nullptr;

std::uint64_t g_moduleWalkBytesAsked = 0;
std::uint64_t g_moduleRefreshBytesAsked = 0;
std::uint64_t g_moduleOuterReads = 0;
bool          g_moduleCountWalkPhase = true;

void* g_moduleActivationRefusal = nullptr;

struct ActivationRefusal {
    std::uintptr_t refuse = 0;
    std::uint64_t  bytesAsked = 0;
};

bool module_walk_counting_reader(std::uintptr_t address, void* dst, std::size_t bytes) {
    if (g_moduleCountWalkPhase) {
        g_moduleWalkBytesAsked += bytes;
    } else {
        g_moduleRefreshBytesAsked += bytes;
    }
    return SyntheticMemory::read(address, dst, bytes);
}

bool module_walk_counting_outer_reader(std::uintptr_t address, void* dst, std::size_t bytes) {
    ++g_moduleOuterReads;
    return SyntheticMemory::read(address, dst, bytes);
}

void module_walk_stub_iterator(std::uint32_t ,
                               void (*onObject)(void*, std::uintptr_t, std::uintptr_t), void* user,
                               std::uint32_t& visited) {
    visited = 0;
    if (g_moduleWalkMemory == nullptr) return;
    auto* m = g_moduleWalkMemory;

    const std::uintptr_t objects[] = {
        0,
        m->at(modulewalk::kForeign),
        m->at(modulewalk::kMesh),
        m->at(modulewalk::kNotNiagara),
        m->at(modulewalk::kNiagaraBase),
        m->at(modulewalk::kMachineOwnedNiagara),
        m->at(modulewalk::kModule),
    };
    const std::size_t count = sizeof(objects) / sizeof(objects[0]);
    for (std::size_t i = 0; i < count; ++i) {
        if (objects[i] == 0) continue;
        std::uint64_t outer = 0;
        if (!module_walk_counting_outer_reader(objects[i] + 0x20,
                                              &outer, sizeof(outer))) {

            continue;
        }
        ++visited;
        onObject(user, objects[i], static_cast<std::uintptr_t>(outer));
    }
}

std::uintptr_t g_watchComponentForFixture = 0;

std::uintptr_t watch_fixed_component(std::uintptr_t, std::uintptr_t& offset, std::uintptr_t& other,
                                     dsh::game::WatchByteReader) noexcept {
    offset = dsh::game::kDACharacterMovementOffset;
    other = 0;
    return g_watchComponentForFixture;
}

void test_observed_watch() {
    section("the differential watch: the offsets, the row table, the guards and the comparison");

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t actor   = mem.at(synth::kActor);
    const std::uintptr_t sibling = mem.at(synth::kSiblingActor);
    const std::uintptr_t movement = mem.at(synth::kComponent);
    const std::uintptr_t module  = mem.at(0x7000);
    const std::uintptr_t comp1   = mem.at(0x7800);
    const std::uintptr_t comp2   = mem.at(0x7900);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    const std::uintptr_t classMachine   = mem.at(synth::kClassMachine);
    const std::uintptr_t classDerived   = mem.at(synth::kClassDerivedMove);
    const std::uintptr_t classOther     = mem.at(synth::kClassOther);

    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kSiblingActor + synth::kUObjectClassOffset, classOther);
    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, classMachine);
    mem.poke(synth::kClassMachine + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);
    mem.poke(synth::kClassDerivedMove + synth::kUStructSuperOffset, 0);

    g_watchComponentForFixture = movement;

    dsh::game::WatchModuleKey noModule;

    {
        section("the watch catalogue: every offset with the pointer it is read from");
        check(dsh::game::watch_scalar_rows() != nullptr && dsh::game::kWatchScalarRowCount > 0,
              "the fixed catalogue is a real array with a count computed from its own macro list",
              std::to_string(dsh::game::kWatchScalarRowCount) + " row(s)");
        check(dsh::game::kWatchModuleScalarRows == 2 &&
                  dsh::game::kWatchStatusRowCount == static_cast<int>(dsh::game::kWatchStatusBytes),
              "the module contributes bPurged, ModuleLevel and one row per byte of the 0x88-byte "
              "ModuleStatus block",
              "statusRows=" + std::to_string(dsh::game::kWatchStatusRowCount));
        check(dsh::game::watch_row_count(0) ==
                  dsh::game::kWatchScalarRowCount + dsh::game::kWatchModuleScalarRows +
                      dsh::game::kWatchStatusRowCount,
              "the table's size is the fixed rows plus the module's own rows when there are no components",
              "rows=" + std::to_string(dsh::game::watch_row_count(0)));
        check(dsh::game::watch_row_count(3) ==
                  dsh::game::watch_row_count(0) + 3 * dsh::game::kWatchRowsPerComponent,
              "and exactly four rows per component above that");
        check(dsh::game::watch_component_count(1000) == dsh::game::kWatchMaxComponents,
              "a module owning more components than the cap is watched over its first ones only, and the "
              "cap is a cap on the TABLE and not on the walk",
              "cap=" + std::to_string(dsh::game::kWatchMaxComponents));

        const auto row_is = [](const char* name, std::uintptr_t offset, dsh::game::WatchKind kind,
                               dsh::game::WatchGate gate) {
            const int index = dsh::game::watch_scalar_row_index_of(name);
            if (index < 0) return false;
            const dsh::game::WatchRow& row = dsh::game::watch_scalar_rows()[index];
            return row.offset == offset && row.kind == kind && row.gate == gate;
        };
        check(row_is("bIgnoreFriction", 0x06E0, dsh::game::WatchKind::u8,
                     dsh::game::WatchGate::actor) &&
                  row_is("bHover", 0x06E1, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::actor) &&
                  row_is("bSprint", 0x06E2, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::actor),
              "the three adjacent Net flag bytes are on the ACTOR at, and "
              "(DollsNest_classes.hpp:2065-2067)");
        check(row_is("bBoostJump", 0x06F0, dsh::game::WatchKind::u8,
                     dsh::game::WatchGate::actor) &&
                  row_is("bBoostDash", 0x06F1, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::actor),
              "and the two BOOST bytes are the SEPARATE group at and -- the pad between the "
              "two groups is why a write that assumed five contiguous bytes was wrong once already");
        check(row_is("RecentInput[0]", 0x06F4, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::actor) &&
                  row_is("RecentInput[2]", 0x06FC, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::actor),
              "RecentObviousControlInputVector is three float32 from (:2072), the last of them "
              "ending exactly where IntermissionTime begins");
        check(row_is("IntermissionTime", 0x0700, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::actor) &&
                  row_is("FastChargeStart", 0x0704, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::actor),
              "the two energy timers are the two float32 at and (:2073-:2074)");
        check(row_is("CurrentStance", 0x10F6, dsh::game::WatchKind::u8,
                     dsh::game::WatchGate::actor) &&
                  row_is("DefaultStance", 0x10F5, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::actor),
              "and the stance pair is at/ (:2214-:2215)");

        check(row_is("MovementMode", 0x0168, dsh::game::WatchKind::u8,
                     dsh::game::WatchGate::component) &&
                  row_is("CustomMovementMode", 0x0169, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::component),
              "MovementMode and CustomMovementMode are the two bytes at/ on the COMPONENT "
              "(Engine_classes.hpp:6964-6965)");
        check(row_is("Velocity[0]", 0x00C4, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::component) &&
                  row_is("Acceleration[2]", 0x0234, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::component) &&
                  row_is("GravityScale", 0x0150, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::component) &&
                  row_is("MaxWalkSpeed", 0x018C, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::component),
              "Velocity is at, Acceleration at, GravityScale at and MaxWalkSpeed at "
              " -- each the game's own offset on the movement component");
        check(dsh::game::watch_scalar_row_index_of("RequestedVelocity[0]") < 0 &&
                  dsh::game::watch_scalar_row_index_of("LastUpdateVelocity[0]") < 0 &&
                  dsh::game::watch_scalar_row_index_of("bJustTeleported(:5)") < 0,
              "and the three fields that are ALREADY on the two-second movement line -- the probe's own "
              "LastUpdateVelocity, RequestedVelocity and the bJustTeleported bit -- are deliberately NOT "
              "duplicated here: this table is for the fields nothing in this project has ever watched, and a "
              "row that another line already prints every two seconds would spend this line's budget twice");

        check(dsh::game::kDerivedMovementSpanBegin == 0x0AF0 &&
                  dsh::game::kDerivedMovementSpanEnd == 0x0B40,
              "the derived block spans.., which is exactly the 0x50 bytes "
              "UDACharacterMovementComponent adds to UCharacterMovementComponent (`DollsNest_classes.hpp`"
              ":4273)",
              hex_offset(dsh::game::kDerivedMovementSpanBegin) + ".." +
                  hex_offset(dsh::game::kDerivedMovementSpanEnd));
        check(row_is("OperationalRotation[0]", 0x0AF0, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::derived) &&
                  row_is("OperationalRotationRate[0]", 0x0B14, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived) &&
                  row_is("bFlipRotationToMovement", 0x0B20, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::derived) &&
                  row_is("bUseOperationalWhenStopped", 0x0B21, dsh::game::WatchKind::u8,
                         dsh::game::WatchGate::derived),
              "OperationalRotation at, OperationalRotationRate at and the two bools at "
              "/ (:4277-4281), each carrying the DERIVED gate");
        check(row_is("MaxAccelerationFalling", 0x0B24, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::derived) &&
                  row_is("MinHoveringZVelocity", 0x0B28, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived) &&
                  row_is("HoveringZAcceleration", 0x0B2C, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived),
              "and the three hover-adjacent floats at, and (:4283-4285)");
        check(row_is("MaxAccelerationBoostJump", 0x0B30, dsh::game::WatchKind::f32,
                     dsh::game::WatchGate::derived) &&
                  row_is("MaxAccelerationBoostDash", 0x0B34, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived) &&
                  row_is("MaxBoostDashSpeed", 0x0B38, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived) &&
                  row_is("BoostDashImpulse", 0x0B3C, dsh::game::WatchKind::f32,
                         dsh::game::WatchGate::derived),
              "and the four boost floats at.. (:4286-4289)");
        check(std::string(dsh::game::kDerivedMovementApiClass) == "DACharacterMovementComponent" &&
                  std::string(dsh::game::kWatchApiClass) == "DACharacter",
              "the two class names are the UClass objects' own names, WITHOUT the C++ A/U prefix -- the "
              "mistake that has already cost this project a round",
              dsh::game::kDerivedMovementApiClass);

        check(dsh::game::kWatchComponentFields[0].offset == 0x0089 &&
                  dsh::game::kWatchComponentFields[0].bitIndex == 7 &&
                  dsh::game::kWatchComponentFields[1].offset == 0x008A &&
                  dsh::game::kWatchComponentFields[1].bitIndex == 0,
              "a component's slice starts with UActorComponent's own activation pair -- bit 7 and "
              " bit 0 (`Engine_classes.hpp:920-921`)");
        check(dsh::game::kWatchComponentFields[2].offset == 0x0450 &&
                  dsh::game::kWatchComponentFields[2].kind == dsh::game::WatchKind::ptr &&
                  dsh::game::kWatchComponentFields[3].offset == 0x0558,
              "and carries the Niagara component's Asset pointer at and its two flags at "
              "(`Niagara_classes.hpp:360`, `:369-370`)");

        check(dsh::game::watch_value_text(dsh::game::WatchKind::u8, 1) == "1" &&
                  dsh::game::watch_value_text(
                      dsh::game::WatchKind::i32,
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(-3))) == "-3",
              "a u8 prints unsigned and an i32 prints signed -- ModuleLevel is the table's only signed field");
        {
            float value = 12.5f;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            check(dsh::game::watch_value_text(dsh::game::WatchKind::f32, bits) == "12.500",
                  "a float prints with three decimals",
                  dsh::game::watch_value_text(dsh::game::WatchKind::f32, bits));
        }
        check(dsh::game::watch_value_text(dsh::game::WatchKind::ptr, 0x2142B7C9E80ull) == "0x2142B7C9E80",
              "and a pointer prints as an address with no interpretation at all",
              dsh::game::watch_value_text(dsh::game::WatchKind::ptr, 0x2142B7C9E80ull));
        check(dsh::game::watch_kind_bytes(dsh::game::WatchKind::u8) == 1 &&
                  dsh::game::watch_kind_bytes(dsh::game::WatchKind::f32) == 4 &&
                  dsh::game::watch_kind_bytes(dsh::game::WatchKind::i32) == 4 &&
                  dsh::game::watch_kind_bytes(dsh::game::WatchKind::ptr) == 8,
              "and every kind has the width its read uses");
    }

    {
        section("the watch's guards: a zero class, a foreign actor, no component, the derived class");
        SyntheticMemory::ReadTracker tracker;
        std::vector<dsh::game::WatchChange> trace;

        {
            dsh::game::ObservedWatch watch;
            SyntheticMemory::set_watch_tracker(&tracker);
            const int rows = dsh::game::watch_begin(watch, actor, 0, classDerived, noModule,
                                                    SyntheticMemory::watch_read,
                                                    dsh::game::actor_class_chain_reaches,
                                                    watch_fixed_component);
            SyntheticMemory::set_watch_tracker(nullptr);
            check(rows == dsh::game::watch_row_count(0) && !watch.characterOk && !watch.componentOk,
                  "a zero class builds the WHOLE row table and passes no guard -- the rows exist "
                  "unavailable, which is not the same thing as an empty table",
                  "rows=" + std::to_string(rows));
            check(tracker.addresses.empty(),
                  "and not one address was read: with no class, no offset in the table is known to mean "
                  "anything",
                  std::to_string(tracker.addresses.size()) + " read(s)");
            trace.clear();
            (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 1, 0, trace);
            check(trace.empty(),
                  "so a sweep of that table reports NO move -- a refusal cannot manufacture a change");
        }

        {
            dsh::game::ObservedWatch watch;
            dsh::game::watch_begin(watch, sibling, classCharacter, classDerived, noModule,
                                   SyntheticMemory::read, dsh::game::actor_class_chain_reaches,
                                   watch_fixed_component);
            check(!watch.characterOk,
                  "an actor whose class chain does not reach DACharacter passes no guard either");
        }

        {
            g_watchComponentForFixture = 0;
            dsh::game::ObservedWatch watch;
            dsh::game::watch_begin(watch, actor, classCharacter, classDerived, noModule,
                                   SyntheticMemory::read, dsh::game::actor_class_chain_reaches,
                                   watch_fixed_component);
            check(watch.characterOk && !watch.componentOk,
                  "an actor that IS a DACharacter with NO movement component is a DIFFERENT refusal from a "
                  "foreign actor, and the two have two different next steps");
            const std::string summary = dsh::game::watch_summary_line(watch, std::string());
            check(summary.find("NEITHER movement component pointer") != std::string::npos,
                  "and the summary says exactly that, rather than printing a table of zeros", summary);
            g_watchComponentForFixture = movement;
        }

        {
            mem.poke(synth::kComponent + synth::kUObjectClassOffset, classOther);
            mem.poke(synth::kClassOther + synth::kUStructSuperOffset, classDerived);
            mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, movement);

            dsh::game::ObservedWatch watch;
            tracker.addresses.clear();
            tracker.sawDerivedSpan = false;
            SyntheticMemory::set_watch_tracker(&tracker);
            dsh::game::watch_begin(watch, actor, classCharacter, classDerived, noModule,
                                   SyntheticMemory::watch_read,
                                   dsh::game::actor_class_chain_reaches, watch_fixed_component);
            (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 1, 0, trace);
            SyntheticMemory::set_watch_tracker(nullptr);
            check(watch.characterOk && watch.componentOk,
                  "CONTROL: with a component pointer set, both the character and the component guards pass",
                  hex_offset(watch.component));
            check(watch.derivedOk,
                  "and a component whose class chain reaches DACharacterMovementComponent -- TWO levels up, "
                  "as a Blueprint-generated class really does -- has the block read");
            check(tracker.sawDerivedSpan,
                  "THE CONTROL ON THE ADDRESS: the derived span was opened -- so the refusal below is about "
                  "the guard and not about a watch that never looks",
                  std::to_string(tracker.addresses.size()) + " read(s)");
            check(trace.empty(),
                  "and a FIRST sweep of a live table prints NO move: a baseline is not a change");
        }

        {
            mem.poke(synth::kClassOther + synth::kUStructSuperOffset, 0);

            dsh::game::ObservedWatch watch;
            tracker.addresses.clear();
            tracker.sawDerivedSpan = false;
            SyntheticMemory::set_watch_tracker(&tracker);
            dsh::game::watch_begin(watch, actor, classCharacter, classDerived, noModule,
                                   SyntheticMemory::watch_read,
                                   dsh::game::actor_class_chain_reaches, watch_fixed_component);
            (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 1, 0, trace);
            SyntheticMemory::set_watch_tracker(nullptr);
            check(watch.componentOk && !watch.derivedOk,
                  "an actor whose component chain does NOT reach DACharacterMovementComponent reads the "
                  "component's own fields and NOT the block");
            check(!tracker.sawDerivedSpan,
                  "and NOT ONE of those addresses was opened -- the derived rows are inert, which is the "
                  "whole reason the second guard exists",
                  std::to_string(tracker.addresses.size()) + " read(s)");
            const std::string summary = dsh::game::watch_summary_line(watch, std::string());
            check(summary.find("does NOT reach DACharacterMovementComponent") != std::string::npos,
                  "and the summary says so IN WORDS rather than printing the block's absence as a zero",
                  summary);

            mem.poke(synth::kClassOther + synth::kUStructSuperOffset, classDerived);
        }
    }

    {
        section("the comparison: a baseline is not a move, a refusal is not a value, NaN is one sample");
        mem.poke(synth::kActor + dsh::game::kDACharacterMovementOffset, movement);

        dsh::game::ObservedWatch watch;
        dsh::game::watch_begin(watch, actor, classCharacter, classDerived, noModule,
                               SyntheticMemory::read, dsh::game::actor_class_chain_reaches,
                               watch_fixed_component);
        std::vector<dsh::game::WatchChange> trace;

        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 1, 0, trace);
        check(trace.empty(), "the first sweep of a live table takes every baseline and prints NOTHING");

        {
            std::uint64_t expected = 0;
            for (const dsh::game::WatchSlot& row : watch.rows) {
                if (!row.available) continue;
                expected += dsh::game::watch_kind_bytes(row.kind);
            }
            check(watch.bytesRead == expected && watch.sweeps == 1,
                  "the sweep's cost is COUNTED and not estimated: every readable row's own width, summed",
                  std::to_string(watch.bytesRead) + " byte(s) over " + std::to_string(watch.sweeps) +
                      " sweep(s)");
        }

        mem.poke_byte(synth::kActor + 0x06E2, 1);
        mem.poke_byte(synth::kActor + 0x06E1, 1);
        mem.poke_f32(synth::kActor + 0x0700, 3.5f);
        mem.poke_f32(synth::kActor + 0x06F4, -2.25f);
        trace.clear();
        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 2, 50, trace);
        check(trace.size() == 4, "a sweep with four fields changed prints exactly four moves",
              std::to_string(trace.size()) + " move(s)");
        {

            const std::string dump = [&trace] {
                std::string all;
                for (const dsh::game::WatchChange& c : trace) {
                    all += "|";
                    all += std::string(c.name);
                    all += " dg=" + std::to_string(c.datagram);
                    all += " +" + std::to_string(c.millis) + "ms";
                    all += " " + dsh::game::watch_value_text(c.kind, c.oldValue) + "->" +
                           dsh::game::watch_value_text(c.kind, c.newValue);
                }
                return all;
            }();
            check(dump.find("bSprint dg=2 +50ms 0->1") != std::string::npos,
                  "the sprint byte's move is recorded with its OWN name, the datagram it happened on and "
                  "the millisecond offset inside that datagram", dump);
            check(dump.find("bHover dg=2 +50ms 0->1") != std::string::npos,
                  "and the hover byte's is, in the same sweep -- two one-byte flags that changed in the same "
                  "datagram are two records and not one", dump);
            check(dump.find("RecentInput[0] dg=2 +50ms 0.000->-2.250") != std::string::npos,
                  "and a float is printed as a float: the control-input vector's first component carries the "
                  "old and the new VALUE and not a pair of raw bit patterns", dump);
            check(dump.find("IntermissionTime dg=2 +50ms 0.000->3.500") != std::string::npos,
                  "and so is the intermission timer, whose old value is a measured 0.000 and not an "
                  "'unread' placeholder -- a baseline is not a move", dump);
        }

        trace.clear();
        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 3, 100, trace);
        check(trace.empty(), "a sweep in which nothing moves prints NOTHING -- not a repeated table");

        const float nan = std::nanf("");
        mem.poke_f32(synth::kActor + 0x0704, nan);
        trace.clear();
        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 4, 150, trace);
        const std::size_t nanMoves = trace.size();
        trace.clear();
        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 5, 200, trace);
        check(nanMoves <= 1 && trace.empty(),
              "a field holding a NaN moves ONCE and then compares equal to itself -- a NaN can never be a "
              "permanent change",
              "first=" + std::to_string(nanMoves) + " second=" + std::to_string(trace.size()));

        dsh::game::ObservedWatch cut;
        dsh::game::watch_begin(cut, actor, classCharacter, classDerived, noModule,
                               SyntheticMemory::read, dsh::game::actor_class_chain_reaches,
                               watch_fixed_component);
        cut.characterOk = false;
        trace.clear();
        (void)dsh::game::watch_step(cut, dsh::game::WatchModuleSample{}, 6, 250, trace);
        check(trace.empty(),
              "a row whose guard no longer holds is NOT read and NOT compared -- no gate can manufacture a "
              "transition out of a refusal");
    }

    {
        section("the module's rows: the two scalars, the 0x88-byte block and every component's bytes");

        constexpr std::size_t kModuleOffset = 0x7000;
        constexpr std::size_t kComp1Offset  = 0x7800;
        constexpr std::size_t kComp2Offset  = 0x7900;
        constexpr std::size_t kAssetOffset  = 0x7A00;

        dsh::game::WatchModuleKey key;
        key.module = module;
        key.componentCount = 2;
        key.firstComponent = comp1;
        key.usable = true;
        key.componentNames.push_back("Ni_ThrusterFlame_L");
        key.componentNames.push_back("Ni_ThrusterFlame_R");

        dsh::game::WatchModuleSample sample;
        sample.module = module;
        sample.usable = true;
        sample.purgedRead = true;
        sample.levelRead = true;
        sample.level = 1;
        sample.statusRead = true;
        dsh::game::WatchModuleSample::Component c1;
        c1.address = comp1;
        c1.name = "Ni_ThrusterFlame_L";
        c1.activationRead = true;
        c1.niagara = true;
        c1.assetRead = true;
        c1.asset = mem.at(kAssetOffset);
        c1.niagaraFlagsRead = true;
        dsh::game::WatchModuleSample::Component c2;
        c2.address = comp2;
        c2.name = "Ni_ThrusterFlame_R";
        c2.activationRead = true;
        c2.niagara = false;
        sample.components.push_back(c1);
        sample.components.push_back(c2);

        mem.poke_byte(kComp1Offset + 0x0089, 0x80);
        mem.poke_byte(kComp2Offset + 0x008A, 0x01);
        mem.poke(kComp1Offset + 0x0450, mem.at(kAssetOffset));
        mem.poke_byte(kComp1Offset + 0x0558, 0x03);

        dsh::game::ObservedWatch watch;
        dsh::game::watch_begin(watch, actor, classCharacter, classDerived, key, SyntheticMemory::read,
                               dsh::game::actor_class_chain_reaches, watch_fixed_component);
        std::vector<dsh::game::WatchChange> trace;
        (void)dsh::game::watch_step(watch, sample, 1, 0, trace);
        check(trace.empty(), "the module's rows take their baseline silently, like every other row");
        check(watch.moduleRows == dsh::game::kWatchModuleScalarRows + dsh::game::kWatchStatusRowCount &&
                  watch.componentRows == 2 * dsh::game::kWatchRowsPerComponent,
              "and the table carries the module's own rows plus four rows per component",
              "module=" + std::to_string(watch.moduleRows) + " component=" +
                  std::to_string(watch.componentRows));

        {
            std::uint8_t before = 0;
            SyntheticMemory::read(mem.at(kModuleOffset + 0x0228), &before, 1);
            mem.poke_byte(kModuleOffset + 0x0228, 1);
            std::uint8_t after = 0;
            SyntheticMemory::read(mem.at(kModuleOffset + 0x0228), &after, 1);
            check(before == 0 && after == 1,
                  "THE FIXTURE'S OWN CONTROL: the module's bPurged byte really moved from 0 to 1 in the "
                  "block the watch reads it from -- so a sweep that reports no move is the watch's failure "
                  "and not the fixture's",
                  "before=" + std::to_string(static_cast<unsigned>(before)) + " after=" +
                      std::to_string(static_cast<unsigned>(after)));
        }
        sample.purged = 1;
        sample.level = 2;
        sample.status[5] = 0x40;
        sample.components[1].isActive = 1;

        mem.poke(kModuleOffset + dsh::game::kWatchModuleLevelOffset, 2);
        mem.poke_byte(kModuleOffset + dsh::game::kWatchStatusOffset + 5, 0x40);
        mem.poke_byte(kComp2Offset + 0x008A, 0x02);
        trace.clear();
        (void)dsh::game::watch_step(watch, sample, 2, 50, trace);
        bool sawPurged = false, sawLevel = false, sawStatus = false, sawComponent = false;
        std::string moduleDump;
        for (const dsh::game::WatchChange& change : trace) {
            moduleDump += "|";
            moduleDump += std::string(change.name);
            moduleDump += " " + dsh::game::watch_value_text(change.kind, change.oldValue) + "->" +
                          dsh::game::watch_value_text(change.kind, change.newValue);
            if (change.name.find("bPurged") != std::string::npos) sawPurged = true;
            if (change.name.find("ModuleLevel") != std::string::npos) sawLevel = true;
            if (change.name.find("ModuleStatus[5]") != std::string::npos) sawStatus = true;
            if (change.name.find("Ni_ThrusterFlame_R.bIsActive") != std::string::npos) {
                sawComponent = (change.oldValue == 1 && change.newValue == 0);
            }
        }
        check(sawPurged && sawLevel,
              "bPurged and ModuleLevel move under their own names, with the offsets they came from",
              moduleDump);
        check(sawStatus,
              "and ONE BYTE of the 0x88-byte ModuleStatus block is reported BY ITS OFFSET inside the block "
              "-- one line per byte that moved, never the whole 136-byte row", moduleDump);
        check(sawComponent,
              "and a component's activation byte is named after the COMPONENT and reported as its BIT -- "
              "the byte read 0x01 and the BIT is what moved, so the row reports 1 -> 0 and not 2 -> 0",
              moduleDump);
        check(watch.statusChanges >= 1 && watch.statusBytesMoved >= 1,
              "the two status counters count the same move from both directions -- how many status bytes "
              "moved, and how many distinct ones",
              "statusChanges=" + std::to_string(watch.statusChanges));

        sample.components[1].isActive = 0;
        sample.purged = 0;
        sample.level = 1;
        sample.status[5] = 0;
        mem.poke_byte(kModuleOffset + dsh::game::kWatchStatusOffset + 5, 0x00);
        mem.poke_byte(kComp2Offset + 0x008A, 0x00);
        trace.clear();
        (void)dsh::game::watch_step(watch, sample, 3, 100, trace);
        bool sawInertMove = false;
        for (const dsh::game::WatchChange& change : trace) {
            if (change.name.find("Ni_ThrusterFlame_R.Asset") != std::string::npos ||
                change.name.find("Ni_ThrusterFlame_R.") != std::string::npos) {
                sawInertMove = true;
            }
        }
        check(!sawInertMove,
              "and the two rows of a component that is NOT a Niagara component never move -- there is no "
              "Asset to report, and a refusal is not a value");

        dsh::game::ObservedWatch blind;
        dsh::game::WatchModuleKey emptyKey;
        emptyKey.componentCount = 2;
        dsh::game::watch_begin(blind, actor, classCharacter, classDerived, emptyKey, SyntheticMemory::read,
                               dsh::game::actor_class_chain_reaches, watch_fixed_component);
        dsh::game::WatchModuleSample named;
        named.module = module;
        named.usable = true;
        named.purgedRead = true;
        named.purged = 1;
        named.statusRead = true;
        trace.clear();
        (void)dsh::game::watch_step(blind, named, 1, 0, trace);
        check(trace.empty(),
              "a module sample arriving over a key that named no module takes its BASELINE and prints no "
              "move -- the row's address is rebuilt from the sample and its first read is not a change",
              std::to_string(trace.size()) + " move(s)");
        const std::string blindSummary = dsh::game::watch_summary_line(blind, std::string());
        check(blindSummary.find("NOT opened") != std::string::npos ||
                  blindSummary.find("no walk has named a module") != std::string::npos,
              "and the summary says the module's rows were never opened, rather than printing them as "
              "zeros", blindSummary);
        (void)kModuleOffset;
        (void)kComp1Offset;
        (void)kComp2Offset;
    }

    {
        section("the summary line: its wording and its measured length");
        dsh::game::WatchModuleKey key;
        key.module = module;
        key.componentCount = 8;
        for (int i = 0; i < 8; ++i) {
            key.componentNames.push_back("Ni_ThrusterFlame_" + std::to_string(i));
        }

        dsh::game::ObservedWatch watch;
        dsh::game::watch_begin(watch, actor, classCharacter, classDerived, key, SyntheticMemory::read,
                               dsh::game::actor_class_chain_reaches, watch_fixed_component);
        std::vector<dsh::game::WatchChange> trace;
        (void)dsh::game::watch_step(watch, dsh::game::WatchModuleSample{}, 42, 2100, trace);
        const std::string summary = dsh::game::watch_summary_line(watch, std::string());
        const std::vector<std::string> tableLines = dsh::game::watch_table_lines(watch);
        std::string table;
        std::size_t longestTableLine = 0;
        for (const std::string& l : tableLines) {
            table += l;
            longestTableLine = (std::max)(longestTableLine, l.size());
        }
        std::printf(" [note] the watch summary renders %zu character(s); the table is %zu line(s), longest "
                    "%zu, over %d row(s); the log body holds 1009\n",
                    summary.size(), tableLines.size(), longestTableLine,
                    dsh::game::watch_row_count(key.componentCount));
        std::fflush(stdout);
        check(summary.size() < 1009,
              "the two-second watch summary fits the log's 1009-byte body with eight components",
              std::to_string(summary.size()) + " character(s)");
        check(longestTableLine < 1009 && tableLines.size() > 1,
              "and so does EVERY line of the once-per-session table, which is CHUNKED rather than handed "
              "over as one string for exactly this reason -- the log's `vsnprintf` drops the tail of anything "
              "past 1009 characters without a mark, and a list whose tail was dropped reads like a complete "
              "one", std::to_string(tableLines.size()) + " line(s), longest " +
                            std::to_string(longestTableLine));
        check(table.find("Ni_ThrusterFlame_7.(4 rows)") != std::string::npos &&
                  table.find("BoostDashImpulse") != std::string::npos &&
                  table.find("ModuleStatus(.., one row per byte)") != std::string::npos &&
                  table.find("bSprint") != std::string::npos,
              "and THAT list names every watched row -- the sprint byte, the derived block's last row, the "
              "eighth component and the status block as its span -- while the trace prints a component's "
              "WHOLE name (component and field) whenever one of its rows moves, so summarising the four "
              "per-component fields once loses nothing");
        check(summary.find("bSprint") != std::string::npos &&
                  summary.find("LIST TRUNCATED HERE") != std::string::npos,
              "the bounded summary names the rows that fit its own budget and SAYS IN WORDS that the rest "
              "are on the table line -- it is the GUARD-REFUSAL case above that makes this line longest, "
              "and a list truncated without a word is the confusion this pair of lines exists to prevent");
        check(summary.find("datagram(s) swept") != std::string::npos,
              "it states how many datagrams were swept, which is what makes every count beside it a rate "
              "rather than a claim");

        const std::string refused =
            dsh::game::watch_summary_line(watch, "the local pawn has not been located yet");
        check(refused.find("NOT started") != std::string::npos &&
                  refused.find("the local pawn has not been located yet") != std::string::npos &&
                  refused.find("swept") == std::string::npos,
              "a watch that never started prints its reason and NO counters -- zeros there would be a "
              "measurement nobody took", refused);

        dsh::game::WatchChange change;
        const std::string sprintName = "bSprint";
        change.name = sprintName;
        change.kind = dsh::game::WatchKind::u8;
        change.oldValue = 0;
        change.newValue = 1;
        change.datagram = 1180;
        change.millis = 59000;
        const std::string line = dsh::game::watch_change_line(change);
        check(line.find("dg=1180") != std::string::npos && line.find("+59000ms") != std::string::npos &&
                  line.find("bSprint") != std::string::npos &&
                  line.find("changed from 0 to 1") != std::string::npos,
              "the per-move line carries the datagram, the elapsed milliseconds, the field's own name and "
              "the old and new values", line);
        std::printf(" [note] a watch move line renders %zu character(s), and the log body holds 1009\n",
                    line.size());
        std::fflush(stdout);
    }

    SyntheticMemory::set_watch_tracker(nullptr);
    SyntheticMemory::set_active(nullptr);
}

void test_ammo_spawn_wire() {
    using dsh::game::decode_ammo_spawn;
    using dsh::game::encode_ammo_spawn;

    std::string note;
    dsh::reflect::AmmoSpawnRequest sent;
    sent.classPath = "/Game/Weapons/Ammo/BP_GeneralAmmo_NormalAR.BP_GeneralAmmo_NormalAR_C";
    sent.speedCurvePath = "/Game/Weapons/Ammo/C_Speed.C_Speed";
    sent.damageTypePath = "/Game/Damage/BP_Dmg_Kinetic.BP_Dmg_Kinetic_C";

    sent.targetKind = dsh::game::kAmmoTargetEnemy;
    sent.targetName = "BP_GEC_Middle_M01_NormalBR7";

    sent.locationCm[0] = -20225.75f;
    sent.locationCm[1] = -35916.25f;
    sent.locationCm[2] = 4030.5f;
    sent.rotation[0] = -12.5f;
    sent.rotation[1] = 87.25f;
    sent.rotation[2] = 0.0f;
    for (std::size_t i = 0; i < sizeof(sent.params); ++i) {
        sent.params[i] = static_cast<unsigned char>((i * 7 + 3) & 0xFF);
    }

    const std::vector<std::uint8_t> bytes = encode_ammo_spawn(sent, note);
    check(!bytes.empty() && note.empty(), "a clean projectile encodes with no note", note);

    const std::size_t expected = 3 + (2 + sent.classPath.size()) + (2 + sent.speedCurvePath.size()) + 2 +
                                 (2 + sent.damageTypePath.size()) + 1 + (2 + sent.targetName.size()) + 12 +
                                 12 + dsh::game::kAmmoParamsBytes;
    check(bytes.size() == expected, "and its length is exactly the layout written in the header",
          std::to_string(bytes.size()) + " vs " + std::to_string(expected));

    dsh::reflect::AmmoSpawnRequest got;
    check(decode_ammo_spawn(bytes, got, note), "and decodes", note);
    check(got.classPath == sent.classPath, "the class path survives");
    check(got.speedCurvePath == sent.speedCurvePath, "the speed curve path survives");
    check(got.pointDamageCurvePath.empty(), "an absent point-damage curve stays absent");
    check(got.damageTypePath == sent.damageTypePath, "the damage type path survives");
    check(got.targetKind == dsh::game::kAmmoTargetEnemy, "the target KIND survives");
    check(got.targetName == sent.targetName, "and the target NAME survives");
    check(std::memcmp(got.params, sent.params, sizeof(sent.params)) == 0,
          "the 0x120-byte FDAAmmoParams block survives bit for bit");
    check(std::memcmp(got.locationCm, sent.locationCm, sizeof(sent.locationCm)) == 0 &&
          std::memcmp(got.rotation, sent.rotation, sizeof(sent.rotation)) == 0,
          "and the host placement travels bit for bit, in centimetres and degrees");

    {
        for (const std::uint8_t kind : {dsh::game::kAmmoTargetNone, dsh::game::kAmmoTargetOwnDoll,
                                        dsh::game::kAmmoTargetRefused}) {
            dsh::reflect::AmmoSpawnRequest bare = sent;
            bare.targetKind = kind;
            bare.targetName.clear();
            const std::vector<std::uint8_t> one = encode_ammo_spawn(bare, note);
            const std::string label = "a projectile with target kind " + std::to_string(kind) + " encodes";
            check(!one.empty(), label.c_str(), note);
            dsh::reflect::AmmoSpawnRequest back;
            check(decode_ammo_spawn(one, back, note) && back.targetKind == kind,
                  "and decodes back to the same kind", note);
            check(back.targetName.empty(), "with no name, because that kind carries none");
        }
    }

    {
        dsh::reflect::AmmoSpawnRequest noClass;
        check(encode_ammo_spawn(noClass, note).empty(), "a projectile with no class is not encoded");
        check(!note.empty(), "and the refusal is named", note);
        dsh::reflect::AmmoSpawnRequest longPath = sent;
        longPath.classPath.assign(dsh::game::kMaxAmmoPath + 1, 'x');
        check(encode_ammo_spawn(longPath, note).empty(), "an over-long path is refused");
        check(!note.empty(), "and named rather than truncated", note);

        dsh::reflect::AmmoSpawnRequest enemyNoName = sent;
        enemyNoName.targetName.clear();
        check(encode_ammo_spawn(enemyNoName, note).empty(),
              "an enemy target with no name is refused: the far side could not find it");
        dsh::reflect::AmmoSpawnRequest nameNoEnemy = sent;
        nameNoEnemy.targetKind = dsh::game::kAmmoTargetOwnDoll;
        check(encode_ammo_spawn(nameNoEnemy, note).empty(),
              "a name under a kind that carries none is refused as a contradiction");
        dsh::reflect::AmmoSpawnRequest badKind = sent;
        badKind.targetKind = 9;
        check(encode_ammo_spawn(badKind, note).empty(), "a kind this build does not define is refused");
    }

    {
        const std::size_t kindAt = 3 + (2 + sent.classPath.size()) + (2 + sent.speedCurvePath.size()) + 2 +
                                   (2 + sent.damageTypePath.size());
        dsh::reflect::AmmoSpawnRequest out;
        std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
        check(!decode_ammo_spawn(truncated, out, note), "a truncated payload is refused", note);
        std::vector<std::uint8_t> trailing = bytes;
        trailing.push_back(0xAB);
        check(!decode_ammo_spawn(trailing, out, note), "trailing bytes are refused", note);
        std::vector<std::uint8_t> wrongFormat = bytes;
        wrongFormat[0] = 99;
        check(!decode_ammo_spawn(wrongFormat, out, note),
              "a format byte this build does not speak is refused", note);

        std::vector<std::uint8_t> oldFormat = bytes;
        oldFormat[0] = 2;
        check(!decode_ammo_spawn(oldFormat, out, note), "a format-2 payload is refused by this build", note);
        std::vector<std::uint8_t> wrongBlock = bytes;
        wrongBlock[1] = 0x00;
        wrongBlock[2] = 0x00;
        check(!decode_ammo_spawn(wrongBlock, out, note),
              "a block size that disagrees with FDAAmmoParams is refused instead of decoded", note);
        std::vector<std::uint8_t> badKind = bytes;
        badKind[kindAt] = 9;
        check(!decode_ammo_spawn(badKind, out, note), "a target kind outside the four is refused", note);
        check(!decode_ammo_spawn(std::span<const std::uint8_t>{}, out, note), "an empty payload is refused",
              note);

        std::vector<std::uint8_t> emptyClass;
        emptyClass.push_back(dsh::game::kAmmoWireFormat);
        const auto paramsLength = static_cast<std::uint16_t>(dsh::game::kAmmoParamsBytes);
        emptyClass.push_back(static_cast<std::uint8_t>(paramsLength & 0xFF));
        emptyClass.push_back(static_cast<std::uint8_t>((paramsLength >> 8) & 0xFF));
        for (int i = 0; i < 4; ++i) {
            emptyClass.push_back(0);
            emptyClass.push_back(0);
        }
        emptyClass.push_back(dsh::game::kAmmoTargetNone);
        emptyClass.push_back(0);
        emptyClass.push_back(0);
        emptyClass.resize(emptyClass.size() + dsh::game::kAmmoParamsBytes, 0);
        check(!decode_ammo_spawn(emptyClass, out, note), "a payload naming no class is refused", note);
    }
}

void test_ammo_copy_damage() {
    using dsh::game::AmmoCopyDamage;
    using dsh::game::clear_copy_damage;

    unsigned char block[dsh::game::kAmmoParamsBytes] = {};
    const auto put = [&block](std::size_t at, float value) { std::memcpy(block + at, &value, sizeof(value)); };
    const auto get = [&block](std::size_t at) {
        float value = 0.0f;
        std::memcpy(&value, block + at, sizeof(value));
        return value;
    };
    block[0x20] = 1;
    block[0x50] = 1;
    put(0x24 + 0x00, 2231.0f);
    put(0x24 + 0x08, 2231.0f);
    put(0x40, 10.0f);
    put(0x54 + 0x08, 900.0f);
    put(0x74 + 0x08, 300.0f);
    put(0x08, 7100.0f);
    const AmmoCopyDamage damage = clear_copy_damage(block, sizeof(block));
    check(damage.sawPointDamage, "a block carrying kinetic damage is reported as having some");
    check(damage.pointBefore[0] == 2231.0f && damage.pointBefore[2] == 2231.0f,
          "and the numbers it reports are the ones that were there");
    check(damage.pointMinBefore == 10.0f, "including the point-damage floor");
    check(damage.sawRadialDamage, "and the radial group is seen too");
    check(damage.radialBefore[2] == 900.0f && damage.radialMinBefore[2] == 300.0f,
          "with the radial floor group read from its OWN offset, not the radial one");

    check(get(0x24 + 0x00) == 0.0f && get(0x24 + 0x08) == 0.0f && get(0x24 + 0x10) == 0.0f,
          "every float of PointDamageParams is zero afterwards");
    check(get(0x40) == 0.0f, "and the floor is zero too: zeroed params under a live floor still hurt");
    check(get(0x54 + 0x08) == 0.0f && get(0x74 + 0x08) == 0.0f, "and both radial groups");
    check(block[0x20] == 1 && block[0x50] == 1,
          "while the two ENABLE flags stay on: the hit still happens, it is only worth nothing");
    check(get(0x08) == 7100.0f, "and nothing outside the four groups is touched (Speed survives)");

    unsigned char empty[dsh::game::kAmmoParamsBytes] = {};
    const AmmoCopyDamage none = clear_copy_damage(empty, sizeof(empty));
    check(!none.sawPointDamage && !none.sawRadialDamage, "an empty block reports nothing to clear");

    unsigned char shortBlock[dsh::game::kAmmoParamsBytes] = {};
    shortBlock[0x24] = 0x7F;
    const AmmoCopyDamage refused = clear_copy_damage(shortBlock, dsh::game::kAmmoParamsBytes - 1);
    check(!refused.sawPointDamage && !refused.sawRadialDamage, "a block one byte short is refused");
    check(shortBlock[0x24] == 0x7F, "and not one byte of it is written");
    check(!clear_copy_damage(nullptr, dsh::game::kAmmoParamsBytes).sawPointDamage,
          "and a null block is refused rather than dereferenced");
}

void test_health_wire() {
    using dsh::game::HealthEntry;
    using dsh::game::decode_health;
    using dsh::game::encode_health_broadcast;
    using dsh::game::encode_health_report;

    std::string note;

    {
        std::vector<HealthEntry> sent{{"BP_GEC_Light_M01_NormalSMG13", 7, 600.0f},
                                      {"BP_GEC_Jellyfish_M02_Normal", 8, 0.5f}};
        const std::vector<std::uint8_t> bytes = encode_health_report(sent, note);
        check(note.empty(), "a clean report encodes with no note");
        std::vector<HealthEntry> got;
        check(decode_health(bytes, true, got, note), "and decodes", note);
        check(got.size() == 2, "with both entries");
        check(got[0].enemyName == sent[0].enemyName && got[0].sequence == 7 && got[0].value == 600.0f,
              "the first entry survives the round trip exactly");
        check(got[1].sequence == 8 && got[1].value == 0.5f, "and so does the second");
    }

    {
        std::vector<HealthEntry> sent{{"BP_GEC_Middle_M01_NormalBR", 3, 0.0f}};
        const std::vector<std::uint8_t> bytes = encode_health_broadcast(sent, note);
        std::vector<HealthEntry> got;
        check(decode_health(bytes, false, got, note), "a broadcast decoding zero health is accepted", note);
        check(got.size() == 1 && got[0].value == 0.0f, "and zero is a legal absolute health");

        std::vector<HealthEntry> asReport;
        check(!decode_health(bytes, true, asReport, note), "the same bytes as a REPORT are refused");
        check(asReport.empty(), "and nothing is returned when a payload is refused");
        check(!note.empty(), "the refusal is named, not silent");
    }

    {
        std::vector<HealthEntry> sent{{"", 1, 10.0f},
                                      {"BP_ok", 2, 0.0f},
                                      {"BP_ok", 3, -5.0f},
                                      {"BP_ok", 4, std::nanf("")},
                                      {std::string(200, 'x'), 5, 10.0f}};
        const std::vector<std::uint8_t> bytes = encode_health_report(sent, note);
        std::vector<HealthEntry> got;
        check(decode_health(bytes, true, got, note), "the leftover payload is still well formed");
        check(got.empty(), "and it is EMPTY: every entry above was refused, not clamped");
    }

    {
        std::vector<HealthEntry> ok{{"BP_x", 1, 1.0f}};
        const std::vector<std::uint8_t> good = encode_health_report(ok, note);
        std::vector<HealthEntry> got;

        std::vector<std::uint8_t> truncated(good.begin(), good.end() - 1);
        check(!decode_health(truncated, true, got, note), "a truncated payload is refused", note);

        std::vector<std::uint8_t> trailing = good;
        trailing.push_back(0xAB);
        check(!decode_health(trailing, true, got, note), "trailing bytes are refused", note);

        std::vector<std::uint8_t> emptyName = good;
        emptyName[2] = 0;
        check(!decode_health(emptyName, true, got, note), "an entry with an empty name is refused", note);

        std::vector<std::uint8_t> hugeCount{0xFF, 0xFF};
        check(!decode_health(hugeCount, true, got, note), "a count above the cap is refused", note);

        check(!decode_health(std::span<const std::uint8_t>{}, true, got, note),
              "an empty payload is refused", note);
        std::vector<std::uint8_t> oneByte{0x01};
        check(!decode_health(oneByte, true, got, note), "a one-byte payload is refused", note);

        std::vector<std::uint8_t> nanDamage = good;
        const std::uint32_t nanBits = 0x7FC00000u;
        const std::size_t valueAt = nanDamage.size() - 4;
        for (int i = 0; i < 4; ++i) {
            nanDamage[valueAt + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((nanBits >> (8 * i)) & 0xFF);
        }
        check(!decode_health(nanDamage, true, got, note), "a NaN value on the wire is refused", note);
        check(got.empty(), "and a refused payload leaves no partial result");
    }
}

void test_player_health_wire() {
    using dsh::game::PlayerHealth;
    using dsh::game::decode_player_health;
    using dsh::game::encode_player_health;

    std::string note;

    {
        PlayerHealth sent;
        sent.health = 27190.096f;
        sent.maxHealth = 32915.0f;
        const std::vector<std::uint8_t> bytes = encode_player_health(sent, note);
        check(note.empty(), "a clean player-health value encodes with no note");
        check(bytes.size() == dsh::game::kPlayerHealthBytes, "and it is exactly the nine bytes the header says");
        PlayerHealth got;
        check(decode_player_health(bytes, got, note), "and it decodes", note);
        check(got.health == sent.health, "the health survives bit for bit, not just approximately");
        check(got.maxHealth == sent.maxHealth, "and so does the maximum");
    }

    {
        PlayerHealth sent;
        sent.health = 0.0f;
        sent.maxHealth = 32915.0f;
        const std::vector<std::uint8_t> bytes = encode_player_health(sent, note);
        PlayerHealth got;
        check(decode_player_health(bytes, got, note), "a zero health decodes: a dead player is a fact", note);
        check(got.health == 0.0f, "and it arrives as zero rather than being refused as missing");
    }

    {
        PlayerHealth nan;
        nan.health = std::nanf("");
        nan.maxHealth = 1.0f;
        check(encode_player_health(nan, note).empty(), "a NaN health is refused rather than sent");
        check(!note.empty(), "and the refusal is named, not silent");

        PlayerHealth negative;
        negative.health = -1.0f;
        negative.maxHealth = 1.0f;
        check(encode_player_health(negative, note).empty(), "a negative health is refused");
    }

    {
        PlayerHealth good;
        good.health = 100.0f;
        good.maxHealth = 200.0f;
        const std::vector<std::uint8_t> bytes = encode_player_health(good, note);
        PlayerHealth got;

        std::vector<std::uint8_t> shortOne(bytes.begin(), bytes.end() - 1);
        check(!decode_player_health(shortOne, got, note), "a payload one byte short is refused", note);

        std::vector<std::uint8_t> longOne = bytes;
        longOne.push_back(0x00);
        check(!decode_player_health(longOne, got, note), "a payload one byte long is refused", note);

        std::vector<std::uint8_t> wrongFormat = bytes;
        wrongFormat[0] = 2;
        check(!decode_player_health(wrongFormat, got, note), "a format this build does not speak is refused", note);

        std::vector<std::uint8_t> nanHealth = bytes;
        const std::uint32_t nanBits = 0x7FC00000u;
        for (int i = 0; i < 4; ++i) {
            nanHealth[1 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((nanBits >> (8 * i)) & 0xFF);
        }
        check(!decode_player_health(nanHealth, got, note), "a NaN health on the wire is refused", note);
        check(got.health == 0.0f && got.maxHealth == 0.0f, "and a refused payload leaves no partial result");

        std::vector<std::uint8_t> negativeOnWire = bytes;
        const std::uint32_t minusOneBits = 0xBF800000u;
        for (int i = 0; i < 4; ++i) {
            negativeOnWire[1 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((minusOneBits >> (8 * i)) & 0xFF);
        }
        check(!decode_player_health(negativeOnWire, got, note), "a negative health on the wire is refused", note);
    }
}
void test_health_core() {
    using dsh::reflect::HealthDeltaTracker;
    using dsh::reflect::SequenceGate;
    using dsh::reflect::clamp_reported_damage;
    using dsh::reflect::report_is_plausible;

    {
        HealthDeltaTracker t;
        check(!t.hasBaseline(), "tracker starts with no baseline");
        float dmg = 123.0f;
        check(!t.observe(100.0f, &dmg), "an observation before any adopt reports nothing");
        check(dmg == 0.0f, "and leaves the damage at zero");
        t.adopt(500.0f);
        check(t.hasBaseline() && t.baseline() == 500.0f, "adopt sets the baseline");

        t.adopt(400.0f);
        check(t.baseline() == 400.0f, "a lower authoritative value re-baselines on adopt");
        check(!t.observe(400.0f, &dmg) && dmg == 0.0f, "the value the write itself produced is NOT damage");
    }

    {
        HealthDeltaTracker t;
        t.adopt(7200.0f);
        float dmg = 0.0f;
        check(!t.observe(7200.0f, &dmg), "no change -> no report");
        check(!t.observe(7450.0f, &dmg), "a rise (regeneration/heal) -> no report");
        check(dmg == 0.0f, "and no negative damage is produced");
        check(!t.observe(std::nanf(""), &dmg), "NaN -> no report");
        check(t.observe(6600.0f, &dmg) && dmg == 600.0f, "a drop reports exactly the difference");

        check(t.observe(6600.0f, &dmg) && dmg == 600.0f,
              "re-observing without adopting reports again -- the caller owns the write-back");
        t.adopt(6600.0f);
        check(!t.observe(6600.0f, &dmg) && dmg == 0.0f, "after the write-back the same value is silent");
        t.reset();
        check(!t.hasBaseline(), "reset forgets the baseline");
    }

    {
        HealthDeltaTracker t;
        t.adopt(6600.0f);
        float total = 0.0f;
        for (int tick = 0; tick < 1000; ++tick) {

            const float regenerated = 7200.0f - 600.0f + static_cast<float>(tick) * 0.25f;
            float dmg = 0.0f;
            if (t.observe(regenerated, &dmg)) total += dmg;
            t.adopt(7200.0f - 600.0f);
        }
        check(total == 0.0f, "1000 ticks of regeneration plus write-back report exactly zero damage");
    }

    {
        SequenceGate g;
        check(g.accept(1), "first report accepted");
        check(!g.accept(1), "the same sequence again is refused");
        check(g.accept(2), "the next sequence accepted");
        check(!g.accept(1), "an older sequence refused");
        check(g.accept(3), "in order accepted");
        check(!g.accept(3 + SequenceGate::kMaxSkew + 1), "a jump beyond the skew bound is refused");
        check(g.accept(4), "and the counter is unchanged by the refusal");
        g.reset();
        check(!g.hasApplied() && g.accept(1), "reset allows a fresh stream");
    }

    {
        check(clamp_reported_damage(600.0f, 7200.0f, 0.0f) == 600.0f, "a normal report passes through");
        check(clamp_reported_damage(99999.0f, 7200.0f, 0.0f) == 7200.0f, "an overkill is clamped to health");
        check(clamp_reported_damage(99999.0f, 0.0f, 8000.0f) == 8000.0f, "health plus shield is the ceiling");
        check(clamp_reported_damage(-5.0f, 7200.0f, 0.0f) == 0.0f, "a negative report applies nothing");
        check(clamp_reported_damage(std::nanf(""), 7200.0f, 0.0f) == 0.0f, "NaN applies nothing");
        check(clamp_reported_damage(600.0f, 0.0f, 0.0f) == 0.0f, "nothing left to take -> nothing applied");
        check(!report_is_plausible(0.0f) && !report_is_plausible(-1.0f) && !report_is_plausible(std::nanf("")),
              "zero, negative and NaN reports are not plausible");
        check(report_is_plausible(1.0f), "a positive report is plausible");
    }
}
void test_thruster_module_component_walk() {
    section("the thruster module's own components: the walk, the filter, and the Niagara state");

    check(dsh::game::kMachineThrusterOffset == 0x15F8,
          "ADAMachine::Thruster is read from (DollsNest_classes.hpp:5673), which is the same slot "
          "EVIDENCE-0025 read 'BP_ThrusterModule_WL005_C' out of on the puppet",
          hex_offset(dsh::game::kMachineThrusterOffset));
    check(dsh::game::kActorComponentAutoActivateOffset == 0x0089 &&
              dsh::game::kActorComponentIsActiveOffset == 0x008A,
          "the activation pair is UActorComponent's bit 7 (bAutoActivate) and bit 0 "
          "(bIsActive) -- Engine_classes.hpp:920-921",
          hex_offset(dsh::game::kActorComponentAutoActivateOffset) + " + " +
              hex_offset(dsh::game::kActorComponentIsActiveOffset));
    check(dsh::game::kNiagaraComponentAssetOffset == 0x0450 &&
              dsh::game::kNiagaraComponentFlagsOffset == 0x0558,
          "a Niagara component's own system pointer is Asset at and its two flags are the byte at "
          " -- Niagara_classes.hpp:360 and :369-370",
          hex_offset(dsh::game::kNiagaraComponentAssetOffset) + " + " +
              hex_offset(dsh::game::kNiagaraComponentFlagsOffset));

    SyntheticMemory mem;

    SyntheticMemory::set_active(&mem);
    g_moduleWalkMemory = &mem;
    const std::uintptr_t base = mem.at(0);
    const auto put_text = [&](std::size_t offset, const char* text) {
        std::strncpy(reinterpret_cast<char*>(base + offset), text, 64);
    };
    const auto put_ptr = [&](std::size_t offset, std::size_t targetOffset) {
        const std::uintptr_t value = mem.at(targetOffset);
        std::memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value));
    };

    const auto put_slot = [&](std::size_t offset, std::size_t targetOffset) {
        const std::uintptr_t value = targetOffset == 0 ? 0 : mem.at(targetOffset);
        std::memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value));
    };

    put_text(modulewalk::kMachineName, "BPMachine");
    put_text(modulewalk::kModuleName, "BPThrust");
    put_text(modulewalk::kModuleClass + 0x18, "Thruster");
    put_text(modulewalk::kNiagaraClass + 0x18, "NiagaraC");
    put_text(modulewalk::kSkeletalMeshClass + 0x18, "SkelMesh");
    put_text(modulewalk::kMachineClass + 0x18, "BPMachine");
    put_text(modulewalk::kMesh + 0x18, "Thruster");
    put_text(modulewalk::kNotNiagara + 0x18, "Exhaust");

    put_text(modulewalk::kNiagaraBase + 0x18, "FlameFX");
    put_text(modulewalk::kMachineOwnedNiagara + 0x18, "FlameFX");

    put_ptr(modulewalk::kModule + 0x20, modulewalk::kMachine);
    put_ptr(modulewalk::kMesh + 0x20, modulewalk::kModule);
    put_ptr(modulewalk::kNotNiagara + 0x20, modulewalk::kModule);
    put_ptr(modulewalk::kNiagaraBase + 0x20, modulewalk::kModule);

    put_ptr(modulewalk::kForeign + 0x20, modulewalk::kForeignActor);
    put_ptr(modulewalk::kForeignActor + 0x10, modulewalk::kSkeletalMeshClass);
    put_ptr(modulewalk::kMachineOwnedNiagara + 0x20, modulewalk::kMesh);
    put_ptr(modulewalk::kModule + 0x10, modulewalk::kModuleClass);
    put_ptr(modulewalk::kMesh + 0x10, modulewalk::kSkeletalMeshClass);
    put_ptr(modulewalk::kNotNiagara + 0x10, modulewalk::kSkeletalMeshClass);
    put_ptr(modulewalk::kNiagaraBase + 0x10, modulewalk::kNiagaraClass);
    put_ptr(modulewalk::kForeign + 0x10, modulewalk::kSkeletalMeshClass);
    put_ptr(modulewalk::kMachineOwnedNiagara + 0x10, modulewalk::kNiagaraClass);

    put_ptr(modulewalk::kNiagaraClass + 0x40, 0);
    put_ptr(modulewalk::kSkeletalMeshClass + 0x40, 0);
    put_ptr(modulewalk::kModuleClass + 0x40, 0);

    put_ptr(modulewalk::kNiagaraBase + dsh::game::kNiagaraComponentAssetOffset,
            modulewalk::kNiagaraAsset);
    put_ptr(modulewalk::kMachineOwnedNiagara + dsh::game::kNiagaraComponentAssetOffset,
            modulewalk::kNiagaraAsset);

    mem.poke_byte(modulewalk::kModule + dsh::game::kThrusterModulePurgedOffset, 0);
    const std::int32_t level = 5;
    std::memcpy(reinterpret_cast<void*>(base + modulewalk::kModule +
                                        dsh::game::kThrusterModuleLevelOffset),
                &level, sizeof(level));
    put_ptr(modulewalk::kModule + dsh::game::kThrusterModuleMeshOffset, modulewalk::kMesh);

    auto set_activation = [&](std::size_t object, bool autoActivate, bool isActive) {
        mem.poke_byte(object + dsh::game::kActorComponentAutoActivateOffset,
                      autoActivate ? 0x80u : 0x00u);
        mem.poke_byte(object + dsh::game::kActorComponentIsActiveOffset, isActive ? 0x01u : 0x00u);
    };
    set_activation(modulewalk::kNiagaraBase, true, true);
    set_activation(modulewalk::kNotNiagara, false, false);
    mem.poke_byte(modulewalk::kNiagaraBase + dsh::game::kNiagaraComponentFlagsOffset, 0x03u);

    put_slot(modulewalk::kMachine + dsh::game::kMachineThrusterOffset, modulewalk::kModule);
    {
        std::uint64_t probe = 0;
        const bool readable = SyntheticMemory::read(
            mem.at(modulewalk::kMachine) + dsh::game::kMachineThrusterOffset, &probe, sizeof(probe));
        check(readable && probe == mem.at(modulewalk::kModule) && g_moduleWalkMemory != nullptr,
              "the graph itself holds the module address at ADAMachine::Thruster before the walk runs",
              "readable=" + std::string(readable ? "yes" : "no") + " value=" + hex_offset(probe));
    }

    const auto chain = [](std::uintptr_t object, std::uintptr_t wanted,
                          dsh::game::ModuleComponentReader read) -> bool {
        std::uint64_t cls = 0;
        if (!read(object + 0x10, &cls, sizeof(cls))) return false;
        std::uintptr_t walk = static_cast<std::uintptr_t>(cls);
        for (int depth = 0; walk >= 0x10000 && depth < 32; ++depth) {
            if (walk == wanted) return true;
            std::uint64_t super = 0;
            if (!read(walk + 0x40, &super, sizeof(super))) return false;
            walk = static_cast<std::uintptr_t>(super);
        }
        return false;
    };

    dsh::game::ModuleComponentReport report;
    g_moduleWalkBytesAsked = 0;
    g_moduleOuterReads = 0;
    g_moduleCountWalkPhase = true;
    const bool walked = dsh::game::module_component_dump(
        mem.at(modulewalk::kMachine), "puppet", mem.at(modulewalk::kModuleClass),
        mem.at(modulewalk::kNiagaraClass), 0, &module_walk_stub_iterator, &module_walk_counting_reader,
        &SyntheticMemory::read_nul_terminated, chain, report);
    const std::uint64_t walkAsked = g_moduleWalkBytesAsked;
    const std::uint64_t walkOuterReads = g_moduleOuterReads;

    check(walked && report.moduleRead && report.module == mem.at(modulewalk::kModule),
          "the module is found through ADAMachine::Thruster at and not by a name lookup",
          walked ? ("module=" + hex_offset(report.module)) : report.note);
    check(report.moduleUsable && report.levelRead && report.level == 5 && report.purgedRead &&
              !report.purged && report.meshRead && report.mesh == mem.at(modulewalk::kMesh),
          "the module's own bPurged/ModuleLevel/Mesh are read only after its class chain reaches "
          "DAThrusterModule, at//",
          "level=" + std::to_string(report.level) + " mesh=" + hex_offset(report.mesh));
    check(report.components.size() == 3,
          "the Outer filter keeps exactly the module's own three objects: the two machine-owned "
          "components and the foreign actor's component are NOT in the list",
          "components=" + std::to_string(report.components.size()) + " unnameable=" +
              std::to_string(report.unnameable) + " unnamed=" + std::to_string(report.unnamed));
    check(report.niagaraCount() == 1,
          "exactly one of them is a Niagara component -- a component whose class chain does NOT reach "
          "UNiagaraComponent is listed with its class and NOT read at",
          "niagaras=" + std::to_string(report.niagaraCount()));

    bool sawNiagara = false;
    bool sawActiveWithAsset = false;
    bool sawNonNiagaraUnread = false;
    bool sawNamedMesh = false;
    for (const auto& c : report.components) {
        if (c.objectName == "Thruster" && c.className == "SkelMesh") sawNamedMesh = true;
        if (c.niagara) {
            sawNiagara = true;
            sawActiveWithAsset = c.activationRead && c.autoActivate && c.isActive && c.assetRead &&
                                 c.asset == mem.at(modulewalk::kNiagaraAsset) && c.niagaraFlagsRead &&
                                 c.autoDestroy && c.renderingEnabled;
        } else if (c.className == "SkelMesh") {
            sawNonNiagaraUnread = !c.assetRead && !c.asset && !c.niagara;
        }
    }
    check(sawNiagara && sawActiveWithAsset,
          "the Niagara component reports bAutoActivate=1 bIsActive=1 AND a non-null Asset -- the state "
          "that separates 'active with nothing to render' from 'active and rendering'",
          "asset=" + hex_offset(report.components.empty() ? 0 : report.components.back().asset));
    check(sawNonNiagaraUnread,
          "a SkeletalMeshComponent the module owns is listed with its class and has NO Asset read at "
          " -- the class-chain guard, and the difference between 'not a Niagara component' and "
          "'a Niagara component with a null system'");
    check(sawNamedMesh && report.unnameable == 0,
          "every matched object is described by its OWN name as well as by its class -- the name is what "
          "tells two components of one class apart, which the pawn's two NiagaraComponents in "
          "EVIDENCE-0017 are",
          "unnameable=" + std::to_string(report.unnameable) + " unnamed=" +
              std::to_string(report.unnamed) + " namedMesh=" +
              std::string(sawNamedMesh ? "yes" : "no"));

    check(report.walkBytesRead > 0 && report.walkBytesRead <= walkAsked && walkAsked > 0,
          "the walk counts the bytes it asked the reader for, so its cost is a number rather than a claim",
          "walkBytesRead=" + std::to_string(report.walkBytesRead) + " readerAsked=" +
              std::to_string(walkAsked));

    mem.poke_byte(modulewalk::kNiagaraBase + dsh::game::kActorComponentIsActiveOffset, 0x00u);
    const std::size_t listSizeBefore = report.components.size();
    g_moduleRefreshBytesAsked = 0;
    g_moduleOuterReads = 0;
    g_moduleCountWalkPhase = false;
    dsh::game::refresh_module_component_state(report, mem.at(modulewalk::kModuleClass),
                                              &module_walk_counting_reader, chain);
    const std::uint64_t refreshAsked = g_moduleRefreshBytesAsked;
    const std::uint64_t refreshOuterReads = g_moduleOuterReads;
    check(!report.listFresh,
          "a refresh marks the list as CACHED, so a reader can tell a remembered list from a live walk");
    check(report.components.size() == listSizeBefore && report.unnamed == 0 &&
              report.components.front().className == "SkelMesh",
          "a refresh does NOT touch the component list: no re-walk, no re-name, no pruning -- the list is "
          "what the walk found for this actor",
          "components=" + std::to_string(report.components.size()));
    check(report.refreshBytesRead > 0 && refreshAsked > 0 && refreshAsked <= report.refreshBytesRead + 128,
          "the cheap pass reads a BOUNDED amount: two 1-byte activation reads per cached component, plus "
          "the module's own fields, plus the class-chain walk that guards them -- and no name resolution",
          "refreshBytesRead=" + std::to_string(report.refreshBytesRead) + " readerAsked=" +
              std::to_string(refreshAsked));
    check(refreshOuterReads == 0,
          "AND IT READS NO `UObject::Outer` AT ALL -- which is the cost that scales with the OBJECT TABLE "
          "rather than with the components, and the one a previous round put inside the two-second block "
          "when the host's datagram rate fell from 16.00 Hz to 10.49 Hz. In this fixture the walk read a "
          "handful of Outers; in the game the table holds about 200,000 entries",
          "outerReads: walk=" + std::to_string(walkOuterReads) + " refresh=" +
              std::to_string(refreshOuterReads));
    bool sawInactive = false;
    for (const auto& c : report.components) {
        if (c.niagara && c.activationRead && !c.isActive) sawInactive = true;
    }
    check(sawInactive,
          "and the refresh DID pick up the changed activation bit -- 'nothing was re-read' and 'the state "
          "was re-read and is the same' are different answers, and the first must not look like the second");

    {
        ActivationRefusal state;
        state.refuse = mem.at(modulewalk::kNiagaraBase + dsh::game::kActorComponentIsActiveOffset);
        const auto refusing_reader = [](std::uintptr_t address, void* dst,
                                        std::size_t bytes) -> bool {
            auto* s = static_cast<ActivationRefusal*>(g_moduleActivationRefusal);
            if (s != nullptr && address == s->refuse) {
                s->bytesAsked += bytes;
                return false;
            }
            if (s != nullptr) s->bytesAsked += bytes;
            return SyntheticMemory::read(address, dst, bytes);
        };
        g_moduleActivationRefusal = &state;
        dsh::game::ModuleComponentReport refused;
        const bool refusedWalked = dsh::game::module_component_dump(
            mem.at(modulewalk::kMachine), "puppet", mem.at(modulewalk::kModuleClass),
            mem.at(modulewalk::kNiagaraClass), 0, &module_walk_stub_iterator, refusing_reader,
            &SyntheticMemory::read_nul_terminated, chain, refused);
        g_moduleActivationRefusal = nullptr;

        std::string activationLine;
        bool sawRefusalText = false;
        bool sawOldGenericText = false;
        for (const auto& l : refused.lines()) {
            if (l.find("activation NOT read") == std::string::npos) continue;
            sawRefusalText = true;
            if (activationLine.empty()) activationLine = l;
            if (l.find("/ refused") != std::string::npos) sawOldGenericText = true;
        }
        const bool namesTheOffset =
            activationLine.find("activation NOT read ( refused (bIsActive, bit 0) while read)") !=
            std::string::npos;
        check(refusedWalked && sawRefusalText && namesTheOffset && !sawOldGenericText,
              "a component whose could not be read prints 'activation NOT read' naming THAT "
              "offset and says was read -- so the line never reads as a zero, and a reader can "
              "tell which of the two bytes the reader could not reach",
              activationLine.empty() ? std::string("<no activation NOT read line was printed>")
                                     : activationLine);

        bool refusedEntrySeen = false;
        for (const auto& c : refused.components) {
            if (c.address == mem.at(modulewalk::kNiagaraBase)) {
                refusedEntrySeen = !c.activationRead && c.activationRefusal.find(" refused") !=
                                                          std::string::npos;
            }
        }
        check(refusedEntrySeen,
              "and the report's own field carries the refusal too -- `activationRead` is false, so a "
              "downstream reader cannot mistake the unread byte for an inactive component",
              "activationRead=" + std::string(refusedEntrySeen ? "false" : "TRUE (a zero was invented)"));

        state.refuse = mem.at(modulewalk::kNiagaraBase + dsh::game::kActorComponentAutoActivateOffset);
        state.bytesAsked = 0;
        g_moduleActivationRefusal = &state;
        dsh::game::ModuleComponentReport refusedAuto;
        const bool refusedAutoWalked = dsh::game::module_component_dump(
            mem.at(modulewalk::kMachine), "puppet", mem.at(modulewalk::kModuleClass),
            mem.at(modulewalk::kNiagaraClass), 0, &module_walk_stub_iterator, refusing_reader,
            &SyntheticMemory::read_nul_terminated, chain, refusedAuto);
        g_moduleActivationRefusal = nullptr;
        std::string autoLine;
        for (const auto& l : refusedAuto.lines()) {
            if (l.find("activation NOT read") != std::string::npos && autoLine.empty()) autoLine = l;
        }
        check(refusedAutoWalked &&
                  autoLine.find("activation NOT read ( refused (bAutoActivate, bit 7) while "
                                "read)") != std::string::npos,
              "and a component whose could not be read prints the OTHER offset by name -- the "
              "sentence is built from the two read results, not from a fixed string",
              autoLine.empty() ? std::string("<no activation NOT read line was printed>") : autoLine);
    }

    {
        const std::vector<std::string> asHost = report.lines();
        dsh::game::ModuleComponentReport relabelled = report;
        relabelled.label = "host";
        const std::vector<std::string> asLocal = relabelled.lines();
        bool sameShape = asHost.size() == asLocal.size() && !asHost.empty();
        std::string firstDifference;
        if (sameShape) {
            for (std::size_t i = 0; i < asHost.size(); ++i) {
                std::string a = asHost[i];
                std::string b = asLocal[i];
                for (const std::string& tag : {std::string("walk[puppet]"), std::string("walk[host]")}) {
                    for (std::size_t at = a.find(tag); at != std::string::npos; at = a.find(tag, at)) {
                        a.replace(at, tag.size(), "walk[side]");
                    }
                    for (std::size_t at = b.find(tag); at != std::string::npos; at = b.find(tag, at)) {
                        b.replace(at, tag.size(), "walk[side]");
                    }
                }
                if (a != b) {
                    sameShape = false;
                    firstDifference = "line " + std::to_string(i) + ": '" + a + "' vs '" + b + "'";
                    break;
                }
            }
        }
        check(sameShape,
              "the same module printed under the 'host' label and under the 'puppet' label differs in "
              "NOTHING but the label -- same fields, same order, same wording -- so the two blocks a live "
              "run puts side by side can be read against each other field by field",
              sameShape ? ("lines=" + std::to_string(asHost.size()))
                        : (firstDifference.empty() ? std::string("one of the two lists was empty")
                                                   : firstDifference));

        const bool headerNamesModule =
            !asHost.empty() &&
            asHost[0].find(" -> module " + hex_offset(report.module)) != std::string::npos;
        const bool countLineNamesCount =
            asHost.size() > 1 && asHost[1].find("owns " + std::to_string(report.components.size()) +
                                                " object(s)") != std::string::npos &&
            asHost[1].find("of which " + std::to_string(report.niagaraCount()) +
                           " is/are a NiagaraComponent") != std::string::npos;
        check(headerNamesModule && countLineNamesCount,
              "the module's own address is on the first line and the component count on the second, on "
              "both sides -- so 'owns nothing', 'owns one and it is inactive' and 'owns one and it is "
              "active' are three different first lines rather than three different component lists",
              asHost.empty() ? std::string("<no lines>")
                             : (asHost[0] + " || " + (asHost.size() > 1 ? asHost[1] : std::string("<none>"))));
    }

    {

        put_slot(modulewalk::kMachine + dsh::game::kMachineThrusterOffset, modulewalk::kNotNiagara);
        dsh::game::ModuleComponentReport second;
        const bool secondWalked = dsh::game::module_component_dump(
            mem.at(modulewalk::kMachine), "puppet", mem.at(modulewalk::kModuleClass),
            mem.at(modulewalk::kNiagaraClass), 0, &module_walk_stub_iterator, &SyntheticMemory::read,
            &SyntheticMemory::read_nul_terminated, chain, second);
        check(secondWalked && second.module == mem.at(modulewalk::kNotNiagara) &&
                  second.module != report.module && second.listFresh && !second.moduleUsable &&
                  second.components.empty() &&
                  second.note.find("does not reach") != std::string::npos,
              "a walk of a DIFFERENT module produces its own report -- a different module address, that "
              "actor's own (empty) component list, its own fresh list flag and its own class-chain refusal "
              "-- so serving the first actor's list for the second would be a VISIBLE difference, not a "
              "silent one",
              "module=" + hex_offset(second.module) + " first=" + hex_offset(report.module) +
                  " components=" + std::to_string(second.components.size()) + " note=" + second.note);

        put_slot(modulewalk::kMachine + dsh::game::kMachineThrusterOffset, modulewalk::kModule);
    }

    check(!dsh::game::module_component_dump(mem.at(modulewalk::kMachine), "puppet", 0,
                                            mem.at(modulewalk::kNiagaraClass), 0,
                                            &module_walk_stub_iterator, &SyntheticMemory::read,
                                            &SyntheticMemory::read_nul_terminated, chain, report) ||
              !report.moduleUsable,
          "a ZERO DAThrusterModule class is a refusal, not a default: the module's own offsets are NOT "
          "read as its fields and the report says which class could not be resolved",
          report.note);

    put_ptr(modulewalk::kModule + dsh::game::kThrusterModuleMeshOffset, modulewalk::kMesh);

    put_slot(modulewalk::kMachine + dsh::game::kMachineThrusterOffset, 0);
    const bool nullSlot = dsh::game::module_component_dump(
        mem.at(modulewalk::kMachine), "puppet", mem.at(modulewalk::kModuleClass),
        mem.at(modulewalk::kNiagaraClass), 0, &module_walk_stub_iterator, &SyntheticMemory::read,
        &SyntheticMemory::read_nul_terminated, chain, report);
    check(!nullSlot && report.moduleRead && report.module == 0,
          "a NULL thruster slot is reported as the machine having no thruster module -- which is a "
          "different statement from a slot whose read refused",
          report.note);

    put_slot(modulewalk::kMachine + dsh::game::kMachineThrusterOffset, modulewalk::kModule);
    const bool implausible = dsh::game::module_component_dump(
        0x10, "puppet", mem.at(modulewalk::kModuleClass), mem.at(modulewalk::kNiagaraClass), 0,
        &module_walk_stub_iterator, &SyntheticMemory::read, &SyntheticMemory::read_nul_terminated, chain,
        report);
    check(!implausible && !report.note.empty(),
          "an implausible machine pointer is refused before any slot read, with the refusal named",
          report.note);
    check(!dsh::game::module_component_dump(mem.at(modulewalk::kMachine), "puppet",
                                            mem.at(modulewalk::kModuleClass),
                                            mem.at(modulewalk::kNiagaraClass), 0, nullptr,
                                            &SyntheticMemory::read,
                                            &SyntheticMemory::read_nul_terminated, chain, report),
          "a null object iterator is refused rather than walked");

    g_moduleWalkMemory = nullptr;
    SyntheticMemory::set_active(nullptr);
}

void test_discovery_addresses() {
    section("discovery: where a beacon is aimed, and why 255.255.255.255 is not enough");

    check(dsh::net::directed_broadcast(0xC0A8010Au, 0xFFFFFF00u) == 0xC0A801FFu,
          "192.168.1.10/24 -> 192.168.1.255");
    check(dsh::net::directed_broadcast(0x1A0C2238u, 0xFFFF0000u) == 0x1A0CFFFFu,
          "26.12.34.56/16 -> 26.12.255.255");
    check(dsh::net::directed_broadcast(0x0A010203u, 0xFF000000u) == 0x0AFFFFFFu,
          "10.1.2.3/8 -> 10.255.255.255");
    check(dsh::net::directed_broadcast(0x1A0C2238u, 0xFFFFFFF8u) == 0x1A0C223Fu,
          "a /29 keeps the low three bits out of the host part");
    check(dsh::net::directed_broadcast(0x0A010203u, 0xFFFFFFFFu) == 0u,
          "a /32 is REFUSED (0): the only address in that subnet is the interface itself");
    check(dsh::net::directed_broadcast(0x0A010203u, 0x00000000u) == 0u,
          "a mask of 0 is REFUSED (0): its broadcast is the limited broadcast, which the list carries once");
    check(dsh::net::directed_broadcast(0x0A010203u, 0xFFFFFFFDu) == 0u,
          "a mask that is not a run of leading ones is REFUSED (0) rather than producing a wrong subnet");

    const std::vector<dsh::net::BroadcastTarget> targets = dsh::net::broadcast_targets();
    check(!targets.empty(),
          "the target list is NEVER empty -- an empty one cannot be told from 'it was never built'");
    check(!targets.empty() && targets.front().address == 0xFFFFFFFFu,
          "the limited broadcast is always FIRST, so two machines on one physical LAN still find each other");

    bool allNamed = true;
    for (const dsh::net::BroadcastTarget& t : targets) allNamed = allNamed && !t.text.empty();
    check(allNamed, "every target carries its own text, so the log can name where a beacon was aimed");

    bool noneAreRefusals = true;
    for (std::size_t i = 1; i < targets.size(); ++i) {
        noneAreRefusals = noneAreRefusals && targets[i].address != 0u &&
                          targets[i].address != 0xFFFFFFFFu;
    }
    check(noneAreRefusals, "every directed target is a real subnet broadcast, never 0.0.0.0 or a second "
                           "copy of the limited broadcast");

    bool allDistinct = true;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            allDistinct = allDistinct && targets[i].address != targets[j].address;
        }
    }
    check(allDistinct, "no two targets share an address, so one beacon never aims twice at one subnet");

    const std::vector<dsh::net::BroadcastTarget>& first = dsh::net::broadcast_targets_cached(0);
    const std::vector<dsh::net::BroadcastTarget>& second = dsh::net::broadcast_targets_cached(0);
    check(&first == &second && !first.empty(),
          "the cached list is one object reused, and a zero window rebuilds rather than emptying it");

    const std::string summary = dsh::net::local_ipv4_summary();
    check(!summary.empty(),
          "the interface summary always has something to say, even when it says 'none could be read'");

    {
        const std::vector<std::uint32_t> swept =
            dsh::net::subnet_probe_addresses(0xC0A8010Au, 0xFFFFFF00u, 512);
        check(swept.size() == 253,
              "a /24 with one machine on it is 253 addresses to ask (254 hosts, minus this machine)",
              std::to_string(swept.size()) + " address(es)");
        bool selfExcluded = true;
        bool edgesExcluded = true;
        for (const std::uint32_t a : swept) {
            selfExcluded = selfExcluded && a != 0xC0A8010Au;
            edgesExcluded = edgesExcluded && a != 0xC0A80100u && a != 0xC0A801FFu;
        }
        check(selfExcluded, "the sweeping machine does not ask itself");
        check(edgesExcluded, "the network address and the broadcast address are not asked either");
        check(swept.empty() || swept.front() == 0xC0A80101u,
              "and the first address asked is the first host in the subnet, not an offset into it");

        check(dsh::net::subnet_probe_addresses(0x0A010203u, 0xFF000000u, 512).empty(),
              "a /8 is REFUSED whole (16 million datagrams aimed at strangers is not a search)");
        check(dsh::net::subnet_probe_addresses(0xC0A8010Au, 0xFFFFFF00u, 100).empty(),
              "and a /24 is refused when the caller's own bound is smaller than the subnet");
        check(dsh::net::subnet_probe_addresses(0xC0A8010Au, 0xFFFFFFFFu, 512).empty(),
              "a /32 has no host to ask");
        check(dsh::net::subnet_probe_addresses(0xC0A8010Au, 0xFFFFFFFEu, 512).empty(),
              "a /31 has no host to ask either");
        check(dsh::net::subnet_probe_addresses(0xC0A8010Au, 0xFFFFFFFCu, 512).size() == 1,
              "a /30 has exactly one other host, and it is the one asked");
    }

    const std::vector<std::uint8_t> probe = dsh::net::encode_probe();
    check(dsh::net::is_probe(probe), "a probe this build encodes is recognised as one");
    check(probe.size() == 6 && probe[0] == 'D' && probe[1] == 'S' && probe[2] == 'H' && probe[3] == 'Q',
          "the probe is 'DSHQ' + u16 protocol: 6 bytes, and NOT the beacon's magic",
          std::string("first four bytes ") + std::string(reinterpret_cast<const char*>(probe.data()), 4));

    const std::vector<std::uint8_t> beaconPayload =
        dsh::net::encode_beacon(47777, "Dolls Nest session", "M01A01");
    check(!dsh::net::is_probe(beaconPayload), "a beacon is NOT accepted as a probe");
    std::uint16_t ignoredPort = 0;
    std::string ignoredName;
    std::string ignoredMap;
    check(!dsh::net::decode_beacon(probe, ignoredPort, ignoredName, ignoredMap),
          "and a probe is NOT accepted as a beacon");

    {
        std::uint16_t port = 0;
        std::string name;
        std::string map;
        check(dsh::net::decode_beacon(beaconPayload, port, name, map),
              "a beacon this build encodes decodes again");
        check(port == 47777, "and the session port survives the trip", std::to_string(port));
        check(name == "Dolls Nest session", "and so does the room name");
        check(map == "M01A01", "and so does the MAP the host is on", "'" + map + "'");

        const std::vector<std::uint8_t> nameOnly = dsh::net::encode_beacon(47777, "Dolls Nest session");
        check(dsh::net::decode_beacon(nameOnly, port, name, map),
              "a beacon with no map field still decodes");
        check(map.empty(), "and its map is EMPTY rather than the name or a guess");

        dsh::net::SearchState mine;
        mine.start(5000, 1000);
        mine.mapKey = "M01A01";
        const std::vector<std::uint8_t> sender = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        check(!mine.observe(sender, dsh::net::encode_beacon(47777, "a host elsewhere", "M02B02")),
              "a host on ANOTHER map is not a room");
        check(mine.rooms.empty() && mine.roomsOnAnotherMap == 1,
              "and it is COUNTED as another map rather than as gibberish",
              std::to_string(mine.roomsOnAnotherMap) + " other-map announcement(s)");
        check(!mine.observe(sender, nameOnly),
              "a host that announced no map at all is not a room either");
        check(mine.rooms.empty() && mine.roomsOnAnotherMap == 2,
              "so 'no map' is a mismatch, not a wildcard");
        check(mine.observe(sender, beaconPayload), "a host on THIS map IS a room");
        check(mine.rooms.size() == 1 && mine.rooms.front().mapKey == "M01A01",
              "and the room carries the map it was allowed in on");
        check(mine.summary().find("ANOTHER map") != std::string::npos,
              "the summary names the other-map drops, so a silence that is not a silence is visible");
        mine.stop();

        dsh::net::SearchState blind;
        blind.start(5000, 1000);
        check(!blind.observe(sender, beaconPayload), "an unreadable map here refuses a host that named one");
        check(blind.rooms.empty(), "and the table stays empty");
        blind.stop();
    }

    std::vector<std::uint8_t> wrongProtocol = probe;
    wrongProtocol[4] = static_cast<std::uint8_t>(wrongProtocol[4] ^ 0xFFu);
    check(!dsh::net::is_probe(wrongProtocol),
          "a probe on another protocol is NOT answered -- answering means sending a datagram to an address "
          "a stranger chose");
    check(!dsh::net::is_probe(std::span<const std::uint8_t>()), "an empty datagram is not a probe");
    check(!dsh::net::is_probe(std::span<const std::uint8_t>(beaconPayload.data(), 3)),
          "a truncated datagram is not a probe");

    dsh::net::SearchState search;
    search.start(6000, 1000);
    check(search.probe_due(1000), "the first probe is due the moment the window opens");
    check(!search.probe_due(1001), "and not again in the same millisecond");
    check(!search.probe_due(1999), "nor before the interval has passed");
    check(search.probe_due(2000), "the second is due one interval later");
    int due = 0;
    for (std::uint64_t t = 1000; t <= 7000; ++t) {

        if (t % 100 == 0 && search.probe_due(t)) ++due;
    }
    check(due >= 4 && due <= 7,
          "probing a window at 10 Hz for six seconds asks a handful of times, not a flood",
          std::to_string(due) + " probe(s) in six seconds");
    search.stop();
    check(!search.probe_due(99999), "a stopped search probes nothing, however late the caller is");

    std::printf(" [info] this machine's interfaces: %s\n", summary.c_str());
    for (const dsh::net::BroadcastTarget& t : targets) {
        std::printf(" [info] a beacon would go to %s\n", t.text.c_str());
    }
}

void test_peer_list() {    section("discovery: the peer list (v533)");

    std::vector<std::uint32_t> out;
    std::string note;

    check(dsh::net::parse_peer_addresses("26.53.188.139\n192.168.71.24\n", out, note),
          "two plain addresses parse", note);
    check(out.size() == 2, "and both are kept", "count=" + std::to_string(out.size()));
    check(out.size() == 2 && out[0] == 0x1A35BC8Bu, "the first is 26.53.188.139 in host order",
          out.empty() ? std::string() : hex_offset(out[0]));
    check(out.size() == 2 && out[1] == 0xC0A84718u, "the second is 192.168.71.24 in host order",
          out.size() < 2 ? std::string() : hex_offset(out[1]));

    check(dsh::net::parse_peer_addresses(
              "# pc3 on Radmin\r\n\r\n 26.1.2.3 \r\n26.4.5.6 # trailing comment\r\n", out, note),
          "comments, CRLF, blank lines, spaces and trailing comments are all accepted", note);
    check(out.size() == 2, "and exactly the two real addresses are kept",
          "count=" + std::to_string(out.size()));

    check(!dsh::net::parse_peer_addresses("26.1.2.3\nnot-an-address\n", out, note),
          "ONE bad line refuses the whole list", note);
    check(out.empty(), "and nothing is left half-parsed");
    check(note.find("line 2") != std::string::npos, "and the refusal names the line", note);

    check(!dsh::net::parse_peer_addresses("26.1.2.300\n", out, note), "an octet above 255 is refused", note);
    check(!dsh::net::parse_peer_addresses("26.1.2\n", out, note), "three octets are refused", note);
    check(!dsh::net::parse_peer_addresses("26.1.2.3.4\n", out, note), "five octets are refused", note);
    check(!dsh::net::parse_peer_addresses("0.0.0.0\n", out, note), "0.0.0.0 is not a peer", note);
    check(!dsh::net::parse_peer_addresses("255.255.255.255\n", out, note),
          "the limited broadcast is not a peer", note);

    check(dsh::net::parse_peer_addresses("", out, note) && out.empty(),
          "an empty list is NOT an error -- it means 'no peer list'", note);
    check(dsh::net::parse_peer_addresses("# nothing but comments\n", out, note) && out.empty(),
          "a file of comments only is also 'no peers'");

    const std::string bommed("\xEF\xBB\xBF" "26.1.2.3\n");
    check(dsh::net::parse_peer_addresses(bommed, out, note) && out.size() == 1,
          "a UTF-8 BOM in front of the first address is stripped, not refused", note);
    const std::string bommedComment("\xEF\xBB\xBF" "# a header comment\n26.4.5.6\n");
    check(dsh::net::parse_peer_addresses(bommedComment, out, note) && out.size() == 1,
          "and a BOM in front of a comment line is stripped too", note);
    check(!dsh::net::parse_peer_addresses("\xEF\xBB\xBF" "26.1.2.3\nnonsense\n", out, note),
          "a BOM does not turn a bad line into a good one", note);
}

void test_puppet_dolls() {
    section("puppet dolls: one slot per peer, and no sharing (v538)");

    dsh::game::PuppetDolls dolls;
    check(dolls.size() == 0, "a fresh table holds no dolls");
    check(dolls.find(dsh::PeerId{2}) == nullptr, "an empty slot is NOT found");

    auto* a = dolls.ensure(dsh::PeerId{2});
    check(a != nullptr, "the first datagram from peer 2 creates its slot");
    check(dolls.size() == 1, "and exactly one slot is in use");
    check(dolls.ensure(dsh::PeerId{2}) == a, "asking again returns the SAME doll -- not a second one");
    check(dolls.size() == 1, "and the table did not grow");

    auto* b = dolls.ensure(dsh::PeerId{3});
    check(b != nullptr && b != a, "peer 3 gets a DIFFERENT doll");
    check(dolls.size() == 2, "two slots");

    a->lastActionSequence = 10;
    b->lastActionSequence = 20;
    a->lastExclusiveTypes = 2;
    check(a->lastActionSequence == 10 && b->lastActionSequence == 20,
          "the change gates are PER DOLL -- one peer's value cannot silence another's");
    check(b->lastExclusiveTypes == 0,
          "and the exclusive pair is per doll too (peer 3 has seen nothing)");

    a->actors.push_back(0x10000);
    check(dolls.find(dsh::PeerId{2})->actors.size() == 1, "peer 2 has one actor");
    check(dolls.find(dsh::PeerId{3})->actors.empty(), "and peer 3 has none -- the lists are per doll");

    check(dolls.ensure(dsh::kInvalidPeer) == nullptr, "kInvalidPeer is REFUSED, not given a slot");
    check(dolls.ensure(dsh::PeerId{dsh::game::PuppetDolls::kSlots}) == nullptr,
          "an id at or past the last slot is refused too");
    check(dolls.size() == 2, "and neither refusal grew the table");

    {
        const std::uint8_t peers1and3 = 0x02u | 0x08u;
        check(dsh::game::peers_needing_manifest(peers1and3, 0, dsh::PeerId{2}) == peers1and3,
              "a newcomer (peer 2) leaves peers 1 and 3 still owed peer 2's manifest -- neither has seen it",
              std::to_string(dsh::game::peers_needing_manifest(peers1and3, 0, dsh::PeerId{2})));
        check(dsh::game::peers_needing_manifest(peers1and3, peers1and3, dsh::PeerId{2}) == 0,
              "and once both have been given it, nobody is owed it again -- the replay is one shot",
              std::to_string(dsh::game::peers_needing_manifest(peers1and3, peers1and3, dsh::PeerId{2})));
        check(dsh::game::peers_needing_manifest(peers1and3, 0x02u, dsh::PeerId{2}) == 0x08u,
              "a peer that already has it is skipped and the other is still served -- the mask is per peer, "
              "not per doll",
              std::to_string(dsh::game::peers_needing_manifest(peers1and3, 0x02u, dsh::PeerId{2})));

        const std::uint8_t all3 = 0x02u | 0x04u | 0x08u;
        check(dsh::game::peers_needing_manifest(all3, 0, dsh::PeerId{2}) == (0x02u | 0x08u),
              "the OWNER is never a target -- a player is not sent its own manifest",
              std::to_string(dsh::game::peers_needing_manifest(all3, 0, dsh::PeerId{2})));
        check(dsh::game::peers_needing_manifest(0, 0, dsh::PeerId{2}) == 0,
              "nobody connected means nothing to send -- a two-player room forwards no manifests at all",
              std::to_string(dsh::game::peers_needing_manifest(0, 0, dsh::PeerId{2})));
        check(dsh::game::peers_needing_manifest(all3, 0, dsh::kInvalidPeer) == all3 &&
                  dsh::game::peers_needing_manifest(all3, 0, dsh::PeerId{8}) == all3,
              "an owner id that names nobody (kInvalidPeer, or past the last slot) clears no bit and is "
              "refused rather than subtracting some other peer's slot",
              std::to_string(dsh::game::peers_needing_manifest(all3, 0, dsh::kInvalidPeer)));
        check(dsh::game::peers_needing_manifest(0xFFu, 0, dsh::PeerId{2}) == 0xFBu,
              "all eight bits of the mask are live slots, and the owner's bit is the only one cleared",
              std::to_string(dsh::game::peers_needing_manifest(0xFFu, 0, dsh::PeerId{2})));
    }

    dolls.clear();
    check(dolls.size() == 0, "clear empties every slot");
    check(dolls.find(dsh::PeerId{2}) == nullptr, "and the cleared slots are gone");
}

void test_exclusive_mode() {
    section("the transforming weapon: the two replicated exclusive-mode fields");

    check(dsh::game::kCharacterExclusiveOwnerOffset == 0x0FE0 &&
              dsh::game::kCharacterExclusiveTypesOffset == 0x0FE8,
          "ExclusiveOwner is at ADACharacter and ExclusiveTypes at -- the game's own two "
          "lines (`DollsNest_classes.hpp:2198/:2199`), an eight-byte pointer and a one-byte flag",
          "owner=" + hex_offset(dsh::game::kCharacterExclusiveOwnerOffset) + " types=" +
              hex_offset(dsh::game::kCharacterExclusiveTypesOffset));
    check(dsh::game::kExclusiveNone == 0 && dsh::game::kExclusiveAim == 1 &&
              dsh::game::kExclusiveFirstPerson == 2,
          "EDAExclusiveType is None = 0, Aim = 1, FirstPerson = 2 (`DollsNest_structs.hpp:68-73`) -- a byte "
          "the two HasFlags verbs exist for, so 3 is a real state and not a value outside the enum");
    check(dsh::game::exclusive_types_text(0) == "None" &&
              dsh::game::exclusive_types_text(1) == "Aim" &&
              dsh::game::exclusive_types_text(2) == "FirstPerson" &&
              dsh::game::exclusive_types_text(3) == "Aim|FirstPerson",
          "the four bytes the enum defines are named, and the combination is named as a combination",
          dsh::game::exclusive_types_text(3));
    check(dsh::game::exclusive_types_text(4) == "unknown(0x04)" &&
              dsh::game::exclusive_types_text(0x0A) == "FirstPerson|unknown(0x08)",
          "a bit the game does not define is printed as itself and never folded into one of the two names "
          "-- a byte nobody has explained must not read as `aiming`",
          dsh::game::exclusive_types_text(0x0A));

    {
        dsh::game::ExclusiveState active;
        active.read = true;
        active.types = 3;
        active.owner = 0x1234;
        check(dsh::game::exclusive_state_line(active) ==
                  "exclusive mode: ACTIVE types=Aim|FirstPerson(3) owner 0x1234",
              "an active state names both types and prints the owner",
              dsh::game::exclusive_state_line(active));
        dsh::game::ExclusiveState none;
        none.read = true;
        check(dsh::game::exclusive_state_line(none) == "exclusive mode: NONE (types 0; owner is NULL)",
              "and the inactive state says NONE rather than an empty string", dsh::game::exclusive_state_line(none));
        dsh::game::ExclusiveState refused;
        refused.note = "the actor's class chain does not reach DACharacter";
        const std::string refusedLine = dsh::game::exclusive_state_line(refused);
        check(refusedLine.find("NOT read") != std::string::npos &&
                  refusedLine.find("NONE") == std::string::npos,
              "a REFUSED read says so and never prints the word NONE -- 'no special mode' and 'this side "
              "could not look' must not collapse into one sentence",
              refusedLine);
    }

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic class graph could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    const std::uintptr_t classSibling   = mem.at(synth::kClassSibling);
    const std::uintptr_t classMiddle    = mem.at(synth::kClassMiddle);
    const std::uintptr_t classCharacter = mem.at(synth::kClassCharacter);
    mem.poke(synth::kActor + synth::kUObjectClassOffset, classSibling);
    mem.poke(synth::kClassSibling + synth::kUStructSuperOffset, classMiddle);
    mem.poke(synth::kClassMiddle + synth::kUStructSuperOffset, classCharacter);
    mem.poke(synth::kClassCharacter + synth::kUStructSuperOffset, 0);

    const std::uintptr_t owner = 0x0000000123456789ull;
    mem.poke(synth::kActor + dsh::game::kCharacterExclusiveOwnerOffset, owner);
    mem.poke_byte(synth::kActor + dsh::game::kCharacterExclusiveTypesOffset,
                  static_cast<std::uint8_t>(dsh::game::kExclusiveAim | dsh::game::kExclusiveFirstPerson));

    const dsh::game::ExclusiveState state =
        dsh::game::exclusive_state_read(mem.at(synth::kActor), classCharacter, SyntheticMemory::read);
    check(state.read && state.owner == owner && state.types == 3 && state.active(),
          "the nine bytes at/ are read off an actor whose chain reaches DACharacter",
          dsh::game::exclusive_state_line(state));

    const dsh::game::ExclusiveState wrongClass =
        dsh::game::exclusive_state_read(mem.at(synth::kSiblingActor), classCharacter, SyntheticMemory::read);
    check(!wrongClass.read && !wrongClass.active() &&
              wrongClass.note.find("class chain") != std::string::npos,
          "an actor whose chain reaches a DIFFERENT class is refused before a byte is read",
          wrongClass.note);
    const dsh::game::ExclusiveState noClass =
        dsh::game::exclusive_state_read(mem.at(synth::kActor), 0, SyntheticMemory::read);
    check(!noClass.read && noClass.note.find("DACharacter") != std::string::npos,
          "an unresolved DACharacter class is a refusal that names the class, not a silent zero",
          noClass.note);
    check(!dsh::game::exclusive_state_read(0x10, classCharacter, SyntheticMemory::read).read &&
              !dsh::game::exclusive_state_read(mem.at(synth::kActor), classCharacter, nullptr).read,
          "an implausible actor and a null reader are both refused");

    check(dsh::game::kMachineLeftRearWeaponOffset == 0x1610 &&
              dsh::game::kMachineRightRearWeaponOffset == 0x1618,
          "the two rear-weapon slots are at ADAMachine/ -- the game's own two lines -- and "
          "the front pair is front_weapon.h's/",
          "left=" + hex_offset(dsh::game::kMachineLeftRearWeaponOffset) + " right=" +
              hex_offset(dsh::game::kMachineRightRearWeaponOffset));
    check(std::string(dsh::game::exclusive_owner_kind_text(dsh::game::ExclusiveOwnerKind::leftRearWeapon)) ==
                  "left rear weapon" &&
              std::string(dsh::game::exclusive_owner_kind_text(dsh::game::ExclusiveOwnerKind::other)) ==
                  "other (not a weapon slot)",
          "every kind has a name, and the refusal has one too, so a log line never prints a number alone");
    check(dsh::game::exclusive_owner_slot_offset(dsh::game::ExclusiveOwnerKind::rightRearWeapon) == 0x1618 &&
              dsh::game::exclusive_owner_slot_offset(dsh::game::ExclusiveOwnerKind::frontWeaponSlot1) == 0x1628 &&
              dsh::game::exclusive_owner_slot_offset(dsh::game::ExclusiveOwnerKind::none) == 0 &&
              dsh::game::exclusive_owner_slot_offset(dsh::game::ExclusiveOwnerKind::other) == 0,
          "each slot kind maps back to the offset the receiver reads, and the two kinds that are NOT a slot "
          "map to 0 -- a named refusal, not a guess at which module to drive");
    {

        const std::uintptr_t rearOwner = 0x0000000123456789ull;
        mem.poke(synth::kActor + dsh::game::kMachineRightRearWeaponOffset, rearOwner);
        const dsh::game::ExclusiveOwnerKindRead kind =
            dsh::game::exclusive_owner_kind(rearOwner, mem.at(synth::kActor), SyntheticMemory::read);
        check(kind.read && kind.kind == dsh::game::ExclusiveOwnerKind::rightRearWeapon,
              "an owner that IS the right rear weapon pointer is classified as that slot",
              std::string(dsh::game::exclusive_owner_kind_text(kind.kind)));
        const dsh::game::ExclusiveOwnerKindRead stranger =
            dsh::game::exclusive_owner_kind(0x0000000FEDCBA987ull, mem.at(synth::kActor),
                                            SyntheticMemory::read);
        check(stranger.read && stranger.kind == dsh::game::ExclusiveOwnerKind::other,
              "and an owner that is none of the four slots is `other`, which the receiver refuses rather than "
              "driving the wrong module",
              std::string(dsh::game::exclusive_owner_kind_text(stranger.kind)));
        const dsh::game::ExclusiveOwnerKindRead none =
            dsh::game::exclusive_owner_kind(0, 0, SyntheticMemory::read);
        check(none.read && none.kind == dsh::game::ExclusiveOwnerKind::none,
              "and no owner is `none` without reading anything -- 'no mode' is not a failed read");
    }
}

void test_file_digest() {
    section("file digest (v582: which file, exactly)");

    wchar_t dir[MAX_PATH] = {0};
    const DWORD dirLen = GetTempPathW(MAX_PATH, dir);
    check(dirLen != 0 && dirLen < MAX_PATH, "the temp directory is readable");

    const std::wstring stem = std::wstring(dir) + L"dsh-selftest-digest-" +
                              std::to_wstring(GetCurrentProcessId());
    const std::wstring path = stem + L".bin";

    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        bool wrote = false;
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            wrote = WriteFile(f, "abc", 3, &put, nullptr) != 0 && put == 3;
            CloseHandle(f);
        }
        check(wrote, "a three-byte file was written");

        const auto d = dsh::platform::file_sha256_prefix(path.c_str());
        check(d.ok, "the three-byte file hashes", std::string(d.error));
        check(std::strcmp(d.hex, "ba7816bf8f01cfea") == 0,
              "sha256(\"abc\") starts ba7816bf8f01cfea", std::string(d.hex));
        check(d.size == 3, "its size is reported as 3", std::to_string(d.size));
    }

    {

        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
        }
        const auto d = dsh::platform::file_sha256_prefix(path.c_str());
        check(d.ok, "an empty file hashes", std::string(d.error));
        check(std::strcmp(d.hex, "e3b0c44298fc1c14") == 0,
              "sha256(\"\") starts e3b0c44298fc1c14", std::string(d.hex));
        check(d.size == 0, "its size is reported as 0", std::to_string(d.size));
    }

    DeleteFileW(path.c_str());

    {
        const std::wstring missing = stem + L"-missing.bin";
        const auto d = dsh::platform::file_sha256_prefix(missing.c_str());
        check(!d.ok, "a missing file is NOT reported as ok", std::string(d.error));
        check(d.error[0] != '\0', "the failure names its stage", std::string(d.error));
        check(d.hex[0] == '\0', "and no hash is produced", std::string(d.hex));
    }
}

void test_item_wire() {
    std::printf("\n--- the item wire ---\n");
    std::string note;
    const std::string path = "Item_Potion_HealthPotion_C";
    const std::vector<std::uint8_t> payload = dsh::game::encode_item_use(path, note);
    check(!payload.empty(), "a plain item path encodes", note);

    std::string decoded;
    std::string decodeNote;
    check(dsh::game::decode_item_use(std::span<const std::uint8_t>(payload), decoded, decodeNote) &&
              decoded == path,
          "and decodes back to exactly itself", decodeNote);

    check(dsh::game::encode_item_use(std::string(), note).empty() && !note.empty(),
          "an empty path is refused BY NAME, not sent", note);

    const std::string tooLong(dsh::game::kMaxItemPath + 1, 'A');
    check(dsh::game::encode_item_use(tooLong, note).empty() && !note.empty(),
          "a path past the wire's bound is refused, never truncated", note);

    std::string notAscii = "Item_Bad_";
    notAscii.push_back(static_cast<char>(0xFF));
    check(dsh::game::encode_item_use(notAscii, note).empty() && !note.empty(),
          "a path that is not printable ASCII is refused", note);

    std::vector<std::uint8_t> wrongFormat = payload;
    wrongFormat[0] = 9;
    check(!dsh::game::decode_item_use(std::span<const std::uint8_t>(wrongFormat), decoded, decodeNote),
          "a payload from another format is refused", decodeNote);

    std::vector<std::uint8_t> trailing = payload;
    trailing.push_back(0x41);
    check(!dsh::game::decode_item_use(std::span<const std::uint8_t>(trailing), decoded, decodeNote),
          "trailing bytes are refused rather than ignored", decodeNote);

    check(!dsh::game::decode_item_use(std::span<const std::uint8_t>(), decoded, decodeNote),
          "an empty payload is refused", decodeNote);
}

void test_target_model() {
    section("targets: the pure scoring rules (v594)");
    const float eps = 1e-6f;

    const auto closes = [eps](float a, float b) { return std::fabs(a - b) <= eps; };

    check(closes(dsh::reflect::kSwitchMargin, 0.10f),
          "the switch margin is 0.10 of closeness -- 800 cm closer at the 8 km range");
    check(closes(dsh::reflect::kDecisionFloor, 0.10f),
          "and the floor is 0.10: below it nobody is close enough to be a target");
    check(closes(dsh::reflect::kTargetClosenessRangeCm, 8000.0f),
          "the range is 8000 cm, so a candidate past 80 m scores zero");

    check(closes(dsh::reflect::target_score({0.0f, false}), 1.0f), "at 0 cm a candidate scores exactly 1");
    check(closes(dsh::reflect::target_score({3000.0f, false}), 0.625f),
          "at 30 m of an 80 m range it scores 0.625, so distance is measured linearly");
    check(closes(dsh::reflect::target_score({8000.0f, false}), 0.0f), "at the range itself it scores 0");
    check(closes(dsh::reflect::target_score({20000.0f, false}), 0.0f),
          "past the range it is CLAMPED to 0 and does not go negative");
    check(closes(dsh::reflect::target_score({0.0f, true}), 1.0f + dsh::reflect::kTargetIncumbentBonus),
          "being the incumbent adds the bonus and nothing else");

    check(dsh::reflect::target_should_switch(0.60f, 0.45f),
          "a challenger 0.15 ahead clears a 0.10 margin");
    check(!dsh::reflect::target_should_switch(0.55f, 0.45f),
          "EXACTLY the margin does not clear it -- the comparison is strict, or the floor of two equal "
          "players would flip on float noise");
    check(!dsh::reflect::target_should_switch(0.45f, 0.45f),
          "and an equal score never switches");

    check(!dsh::reflect::target_keep_incumbent(0.14f, 0.05f),
          "558-1: an incumbent worth 0.05 is BELOW the floor, so a 0.14 challenger takes the enemy even "
          "though the margin would have kept it");
    check(dsh::reflect::target_keep_incumbent(0.14f, 0.20f),
          "and an incumbent worth 0.20 keeps it, because 0.14 does not clear 0.20 + 0.10");
    check(!dsh::reflect::target_keep_incumbent(0.90f, 0.10f),
          "an incumbent sitting exactly ON the floor is kept only while the margin holds -- 0.90 clears it");
}

void test_render_proxy_scan() {
    section("the renderer's copy of a primitive's transform: the scan, on a synthetic primitive");

    SyntheticMemory mem;
    if (!mem.ok()) {
        check(false, "a synthetic primitive could be allocated", "VirtualAlloc failed");
        return;
    }
    SyntheticMemory::set_active(&mem);

    constexpr std::size_t kComponent = 0x2000;
    constexpr std::size_t kProxy = 0x3000;
    constexpr std::size_t kSceneInfo = 0x4000;
    constexpr std::size_t kComponentProxySlot = 0x1F8;

    constexpr std::size_t kProxyMatrixOffset = 0x90;
    constexpr std::size_t kProxySceneInfoSlot = 0x10;

    const std::uintptr_t component = mem.at(kComponent);
    const std::uintptr_t proxy = mem.at(kProxy);
    const std::uintptr_t sceneInfo = mem.at(kSceneInfo);
    const float here[3] = {1234.5f, -678.25f, 90.0f};
    const float elsewhere[3] = {0.0f, 0.0f, 0.0f};

    mem.poke(kSceneInfo + 0x08, proxy);
    mem.poke(kProxy + kProxySceneInfoSlot, sceneInfo);
    const float matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f,
                              here[0], here[1], here[2], 1.0f};
    mem.poke_bytes(kProxy + kProxyMatrixOffset, matrix, sizeof(matrix));
    mem.poke(kComponent + kComponentProxySlot, proxy);
    dsh::reflect::RenderPrimitive located;
    const bool found = dsh::reflect::scan_render_primitive(component, here, SyntheticMemory::read, located);
    check(found, "the scan FINDS the proxy behind the component's own pointer", located.note);
    check(found && located.proxySlot == kComponentProxySlot,
          "and reports the offset it found it at, instead of assuming one", located.note);
    check(found && located.matrixOffset == kProxyMatrixOffset,
          "and the offset of the transform inside the proxy", located.note);
    check(found && located.matrixMatchesComponent,
          "and says the transform IS this component's own -- the healthy case");
    check(found && located.sceneInfo == sceneInfo && located.sceneInfoSlot == kProxySceneInfoSlot,
          "and finds the scene info through its back-pointer, so 'is it in a scene at all' is answerable",
          located.note);
    check(found && located.sceneInfoInner == 0x08,
          "and reports WHICH word of the scene info points back, so the next build reads the field instead of "
          "searching for it", located.note);

    mem.poke_byte(kProxy + 0x30, 2);
    mem.poke_byte(kProxy + 0x42, 0x04);
    mem.poke_byte(kProxy + 0x46, 0x08);
    mem.poke(kProxy + 0x120, 0x11110000);
    mem.poke(kProxy + 0x128, sceneInfo);
    mem.poke(kProxy + kProxyMatrixOffset + 0xD0, component);

    dsh::reflect::RenderPrimitive facts;
    const bool readFacts = dsh::reflect::read_render_primitive_at(component, kComponentProxySlot,
                                                                  kProxyMatrixOffset, kProxySceneInfoSlot,
                                                                  SyntheticMemory::read, facts);
    check(readFacts && facts.factsRead, "the proxy's own fields are read", facts.note);
    check(readFacts && facts.proxyMobility == 2, "Mobility at matrix-0x60 is the Mobility the proxy snapshotted");
    check(readFacts && facts.proxyStaticLighting, "bStaticLighting at matrix-0x4E bit 2 is read");
    check(readFacts && facts.proxyVfNeedsPrimitiveUniformBuffer,
          "bVFRequiresPrimitiveUniformBuffer at matrix-0x4A bit 3 is read");
    check(readFacts && facts.proxySceneInfo == sceneInfo && facts.proxySceneInfoPointsBack,
          "the scene info at matrix is read AND its back-pointer is verified against the proxy");
    check(readFacts && facts.proxyComponent == component,
          "ComponentForDebuggingOnly at matrix names the component, which is what proves the layout");

    dsh::reflect::RenderPrimitive reading;
    const bool readBack = dsh::reflect::read_render_primitive_at(
        component, located.proxySlot, located.matrixOffset, located.sceneInfoSlot, SyntheticMemory::read,
        reading);
    check(readBack && reading.translationCm[0] == here[0] && reading.translationCm[1] == here[1] &&
              reading.translationCm[2] == here[2],
          "the cheap read returns the same place, with no scanning and no ProcessEvent", reading.note);
    check(readBack && reading.sceneInfo == sceneInfo, "and the scene info is still named", reading.note);

    dsh::reflect::RenderPrimitive stale;
    const bool foundStale = dsh::reflect::scan_render_primitive(component, elsewhere, SyntheticMemory::read, stale);
    check(foundStale, "a component whose transform has gone STALE is still located -- the case this exists for",
          stale.note);
    check(foundStale && !stale.matrixMatchesComponent,
          "and it is reported as NOT matching, rather than being refused as 'no proxy'");
    check(foundStale && stale.translationCm[0] == here[0] && stale.translationCm[2] == here[2],
          "and the place it reports is the renderer's OLD one, which is the reading the round needs");

    mem.poke(kProxy + kProxySceneInfoSlot, 0);
    dsh::reflect::RenderPrimitive orphan;
    const bool readOrphan = dsh::reflect::read_render_primitive_at(component, kComponentProxySlot,
                                                                  kProxyMatrixOffset, kProxySceneInfoSlot,
                                                                  SyntheticMemory::read, orphan);
    check(readOrphan && orphan.sceneInfoSlot == kProxySceneInfoSlot && orphan.sceneInfo == 0,
          "a proxy whose scene-info word is NULL reads back as 'names NO scene', not as 'unknown'", orphan.note);

    dsh::reflect::RenderPrimitive nothing;
    const bool refused =
        dsh::reflect::scan_render_primitive(mem.at(0x5000), here, SyntheticMemory::read, nothing);
    check(!refused && !nothing.note.empty(),
          "a component holding no proxy is REFUSED with a reason, so an unreadable mesh cannot look healthy",
          nothing.note);
    dsh::reflect::RenderPrimitive plausible;
    check(!dsh::reflect::scan_render_primitive(0x10, here, SyntheticMemory::read, plausible),
          "and an implausible component address is refused before any read");
    check(!dsh::reflect::scan_render_primitive(component, here, nullptr, plausible),
          "and a null reader is refused rather than dereferenced");

    SyntheticMemory::set_active(nullptr);
}

int main() {
    std::printf("dsh-selftest -- offline conformance for dsh-net and dsh-sync\n");
    std::printf("(this binary never touches the game; that is the point of the layer split)\n");

    test_quantisation();
    test_interpolator();
    test_playback_clock();
    test_tick_grid();
    test_pose_stamp_grid();
    test_packet_envelope();

    std::printf("\n--- PHASE 1: session skeleton ---\n");
    test_session_handshake();
    test_session_clock_sync();
    test_session_timeout();
    test_session_budget_alarm();
    test_session_under_hostile_link();
    test_session_full_server();
    test_session_three_ways();
    test_session_relay();
    test_session_over_real_udp();
    test_loadout_manifest_chunking();
    test_machine_loadout_wire();
    test_log_truncation_is_marked();
    test_doll_stance_payload();
    test_doll_stance_class_chain();
    test_exclusive_mode();
    test_machine_movement_offsets();
    test_machine_movement_class_chain();
    test_machine_movement_input();
    test_machine_front_weapon();
    test_observed_watch();
    test_thruster_module_component_walk();
    test_discovery_addresses();
    test_peer_list();
    test_puppet_dolls();
    test_health_core();
    test_health_wire();
    test_player_health_wire();
    test_ammo_spawn_wire();
    test_ammo_copy_damage();
    test_item_wire();
    test_target_model();
    test_render_proxy_scan();
    test_file_digest();

    std::printf("\n========================================\n");
    std::printf("checks: %d failures: %d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
