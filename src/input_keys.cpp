#include "input_keys.hpp"

#include <cctype>
#include <cstring>

namespace vko::input {
namespace {

// evdev: linux/input-event-codes.h. keysym: X11/keysymdef.h.
constexpr KeyInfo kTable[] = {
    {Key::F1, "F1", 59, 0xffbe},   {Key::F2, "F2", 60, 0xffbf},   {Key::F3, "F3", 61, 0xffc0},
    {Key::F4, "F4", 62, 0xffc1},   {Key::F5, "F5", 63, 0xffc2},   {Key::F6, "F6", 64, 0xffc3},
    {Key::F7, "F7", 65, 0xffc4},   {Key::F8, "F8", 66, 0xffc5},   {Key::F9, "F9", 67, 0xffc6},
    {Key::F10, "F10", 68, 0xffc7}, {Key::F11, "F11", 87, 0xffc8}, {Key::F12, "F12", 88, 0xffc9},
    {Key::Insert, "Insert", 110, 0xff63},     {Key::Delete, "Delete", 111, 0xffff},
    {Key::Home, "Home", 102, 0xff50},         {Key::End, "End", 107, 0xff57},
    {Key::PageUp, "PageUp", 104, 0xff55},     {Key::PageDown, "PageDown", 109, 0xff56},
    {Key::Pause, "Pause", 119, 0xff13},       {Key::ScrollLock, "ScrollLock", 70, 0xff14},
};

bool iequals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

}  // namespace

const KeyInfo* key_table(size_t* count) {
    if (count) *count = sizeof kTable / sizeof kTable[0];
    return kTable;
}

Key key_from_evdev(uint32_t code) {
    for (const auto& e : kTable)
        if (e.evdev == code) return e.key;
    return Key::None;
}

const char* key_name(Key k) {
    for (const auto& e : kTable)
        if (e.key == k) return e.name;
    return "none";
}

bool parse_key(const char* name, Key* out) {
    if (!name || !out) return false;
    if (iequals(name, "none") || iequals(name, "off") || iequals(name, "disabled")) { *out = Key::None; return true; }
    for (const auto& e : kTable)
        if (iequals(name, e.name)) { *out = e.key; return true; }
    return false;
}

}  // namespace vko::input
