// input_keys.hpp - the ONE place that knows how platform key codes map to overlay keys.
//
// Every backend (X11 polling, Wayland listener hook, anything added later) translates its
// native codes into `Key` through this table and reports plain facts ("F12 went down").
// What a key MEANS (cycle the overlay mode, open settings, ...) is decided exclusively by
// the controller in input.cpp. Adding a key or a backend therefore never changes behaviour
// on one platform only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vko::input {

enum class Key : uint8_t {
    None = 0,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock,
    Count
};
static_assert(static_cast<unsigned>(Key::Count) <= 32, "KeyMask is a 32-bit set");

using KeyMask = uint32_t;  // bit i set <=> Key(i) is in the set
constexpr KeyMask key_bit(Key k) { return k == Key::None ? 0u : (1u << static_cast<unsigned>(k)); }

struct KeyInfo {
    Key key;
    const char* name;      // as accepted by VKOVERLAY_KEY_* (case-insensitive)
    uint32_t evdev;        // Linux input-event-codes.h value (Wayland delivers these)
    unsigned long keysym;  // X11 keysym (resolved to a keycode through the live keymap)
};

const KeyInfo* key_table(size_t* count);  // every key the overlay can observe
Key key_from_evdev(uint32_t code);        // Key::None if the code is not in the table
const char* key_name(Key k);              // "none" for Key::None

// Parses a key name. "none"/"off"/"disabled" -> Key::None and returns true (binding disabled).
// Returns false for unknown names, leaving *out untouched.
bool parse_key(const char* name, Key* out);

}  // namespace vko::input
