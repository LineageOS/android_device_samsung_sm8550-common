/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace samsung::ril {

inline constexpr char kCallStateProperty[] = "vendor.calls.state";
// Bits follow RIL_CallState, never call indexes or subscription IDs.
inline constexpr unsigned kActive = 1 << 0;
inline constexpr unsigned kHolding = 1 << 1;
inline constexpr unsigned kDialing = 1 << 2;
inline constexpr unsigned kAlerting = 1 << 3;
inline constexpr unsigned kIncoming = 1 << 4;
inline constexpr unsigned kWaiting = 1 << 5;

struct CallSnapshot {
    uint64_t generation = 0;  // Changes whenever the RIL process initializes.
    uint64_t sequence = 0;
    std::array<unsigned, 2> slots{};

    std::string encode() const {
        return "1:" + std::to_string(generation) + ":" + std::to_string(sequence) + ":" +
               std::to_string(slots[0]) + ":" + std::to_string(slots[1]);
    }

    static bool parse(std::string_view text, CallSnapshot* result) {
        if (text.substr(0, 2) != "1:") return false;
        text.remove_prefix(2);
        uint64_t fields[4]{};
        for (size_t i = 0; i < 4; ++i) {
            size_t end = text.find(':');
            if ((end == std::string_view::npos) != (i == 3)) return false;
            auto field = text.substr(0, end);
            auto parsed = std::from_chars(field.data(), field.data() + field.size(), fields[i]);
            if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) return false;
            if (i != 3) text.remove_prefix(end + 1);
        }
        if (fields[0] == 0 || fields[2] > 63 || fields[3] > 63) return false;
        *result = {fields[0], fields[1], {unsigned(fields[2]), unsigned(fields[3])}};
        return true;
    }

    // Ringing/waiting calls never start a voice session. An answered incoming
    // call becomes ACTIVE. Keep an owned, held slot until it ends or another
    // slot gains a foreground call; do not mistake a held call for a new one.
    int selectSlot(int current) const {
        constexpr unsigned foreground = kActive | kDialing | kAlerting;
        if (current >= 0 && current < 2 && (slots[current] & foreground)) return current;
        for (int slot = 0; slot < 2; ++slot) {
            if (slots[slot] & foreground) return slot;
        }
        if (current >= 0 && current < 2 && (slots[current] & kHolding)) return current;
        return -1;
    }
};

}  // namespace samsung::ril
