/// @file RtpPlacement.cpp

#include "RtpPlacement.h"
#include <algorithm>
#include <cstdlib>

namespace AES67 {

RtpPlacement::RtpPlacement(Config config, NetworkTimeMapping& mapping) noexcept
    : config_(config)
    , mapping_(mapping)
{
}

void RtpPlacement::reset() noexcept {
    haveSource_ = false;
    ssrc_ = 0;
    privateAdjust_ = 0;
    followsShared_ = true;
    candidateCount_ = 0;
}

int64_t RtpPlacement::unwrapNear(uint32_t networkTimestamp, int64_t reference) noexcept {
    const auto delta = static_cast<int32_t>(networkTimestamp - static_cast<uint32_t>(reference));
    return reference + delta;
}

int64_t RtpPlacement::positionOf(uint32_t networkTimestamp, int64_t localArrival) const noexcept {
    const int64_t offset = mapping_.offset() + privateAdjust_;
    return unwrapNear(networkTimestamp, localArrival - offset) + offset;
}

int64_t RtpPlacement::marginOf(int64_t position, uint32_t frames, int64_t localArrival) const noexcept {
    // How far the packet's end lands ahead of the read point when it arrives
    return position + frames - (localArrival - config_.linkOffsetFrames);
}

bool RtpPlacement::inWindow(int64_t margin) const noexcept {
    return margin > 0 && margin <= 2 * config_.linkOffsetFrames;
}

RtpPlacement::Result RtpPlacement::place(uint32_t rtpTimestamp, uint32_t ssrc, uint32_t frames,
                                         int64_t localArrival) noexcept {
    const uint32_t networkTimestamp = rtpTimestamp - config_.mediaClockOffset;
    const int64_t arrivalAnchor = localArrival - frames;  // a re-anchored packet ends at its arrival

    if (!haveSource_) {
        // First packet: anchor the shared mapping if no stream has, then
        // follow it if this stream's timestamps fit, else anchor privately
        haveSource_ = true;
        ssrc_ = ssrc;
        privateAdjust_ = 0;
        followsShared_ = true;
        mapping_.anchorIfUnset(arrivalAnchor - static_cast<int64_t>(networkTimestamp));
        int64_t position = positionOf(networkTimestamp, localArrival);
        if (!inWindow(marginOf(position, frames, localArrival))) {
            privateAdjust_ = arrivalAnchor - position;
            followsShared_ = false;
            position = arrivalAnchor;
        }
        return {Verdict::Accepted, position, false};
    }

    int64_t position = positionOf(networkTimestamp, localArrival);
    const int64_t margin = marginOf(position, frames, localArrival);
    if (ssrc == ssrc_ && inWindow(margin)) {
        candidateCount_ = 0;
        return {Verdict::Accepted, position, false};
    }

    // Another source, or a packet the window cannot hold. Count consecutive
    // packets from the same candidate whose timestamps advance plausibly and
    // whose margins agree (a catch-up burst's margins grow by a packet each).
    const auto advance = static_cast<int32_t>(networkTimestamp - candidateTimestamp_);
    const int64_t marginTolerance = config_.linkOffsetFrames / 4;
    const bool continuesCandidate = candidateCount_ > 0 && ssrc == candidateSsrc_ && advance > 0 &&
                                    advance <= 16 * static_cast<int32_t>(frames) &&
                                    std::llabs(margin - candidateFirstMargin_) <= marginTolerance;
    if (continuesCandidate) {
        ++candidateCount_;
        candidateFrames_ += frames;
    } else {
        candidateCount_ = 1;
        candidateFrames_ = frames;
        candidateFirstMargin_ = margin;
    }
    candidateSsrc_ = ssrc;
    candidateTimestamp_ = networkTimestamp;

    const bool confirmed = candidateCount_ >= kSourceSwitchPackets &&
                           candidateFrames_ >= std::max(config_.sourceSwitchFrames, config_.linkOffsetFrames / 2);
    if (!confirmed) {
        if (ssrc != ssrc_) {
            ++foreignDrops_;
            return {Verdict::Foreign, position, false};
        }
        if (margin <= 0) {
            ++lateDrops_;
            return {Verdict::Late, position, false};
        }
        ++earlyDrops_;
        return {Verdict::Early, position, false};
    }

    // Confirmed: follow this source from now on
    candidateCount_ = 0;
    const bool sameSource = ssrc == ssrc_;
    ssrc_ = ssrc;
    if (inWindow(margin)) {
        // New SSRC on the same timeline (e.g. a PTP-locked sender restarted)
        return {Verdict::Accepted, position, false};
    }

    ++reanchors_;
    if (sameSource && followsShared_) {
        // Drift between the local clock and network time: move the shared
        // mapping so every stream on it stays aligned
        mapping_.reanchor(mapping_.offset() + (arrivalAnchor - position));
    } else {
        // This stream's timeline changed on its own (new session offset or an
        // unrelated clock): adjust only this stream
        privateAdjust_ += arrivalAnchor - position;
        followsShared_ = false;
    }
    return {Verdict::Accepted, arrivalAnchor, true};
}

} // namespace AES67
