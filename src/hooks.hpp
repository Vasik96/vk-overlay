// hooks.hpp - inline-hook plumbing on top of safetyhook.
//
// BACKGROUND: how an inline (a.k.a. "detour") hook works on x86_64 Linux
// ----------------------------------------------------------------------
//  1. The first N bytes (>= 5 for a rel32 jmp, 14 for an absolute one) of the target
//     function are copied into an executable "trampoline" buffer. Every RIP-relative
//     operand and relative branch in the copied bytes is re-encoded (safetyhook uses the
//     Zydis disassembler for this) so the stolen instructions still work from the new
//     address.
//  2. The trampoline ends with a jump back to target+N, so calling the trampoline == "call
//     the original function".
//  3. The target's first bytes are overwritten with a jump to our detour. Code pages are
//     normally r-x, so safetyhook temporarily mprotect(2)s the page to rwx, patches,
//     then restores the old protection.
//     !! On Linux safetyhook does NOT suspend other threads (its trap_threads() only
//     !! flips page protections and fix_ip() is an empty stub, unlike the Windows backend).
//     !! Patching is therefore only race-free while the process is still single-threaded,
//     !! i.e. from the ELF constructor under LD_PRELOAD. Never patch a running multi-threaded
//     !! process with this code, and never un-patch while other threads may be in the target.
//
// WHY A SLOT TABLE?
// ----------------------------------------------------------------------
// The Vulkan loader (libvulkan.so.1) exports *trampoline* functions such as
// vkQueuePresentKHR. Those jump through the dispatchable object's dispatch table into
// the layer chain / ICD. Applications reach the real work in two different ways:
//     (a) calling the exported loader symbol          -> loader trampoline address
//     (b) vkGetDeviceProcAddr(device, "vkQueue...")   -> often the ICD's function directly
// To be robust we hook *every distinct address* that a name can resolve to. A detour has
// no way of learning "which address was I reached through", so each address gets its own
// detour function, instantiated from a template with a compile-time slot index.
// A thread_local re-entrancy guard in each handler makes the nested case (loader
// trampoline -> ICD function, both hooked) process the call exactly once.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <utility>

#include <safetyhook.hpp>

#include "log.hpp"

namespace vko {

// Tag requirements:
//   using Fn = R (VKAPI_PTR*)(Args...);                     (a PFN_vk* type)
//   static R handle(Fn original, Args... args);             (the actual hook body)
template <class Tag, class Sig>
struct HookSlots;

template <class Tag, class R, class... A>
struct HookSlots<Tag, R (VKAPI_PTR*)(A...)> {
    using Fn = R (VKAPI_PTR*)(A...);
    static constexpr std::size_t kMax = 4;

    // Returns true if `target` is now (or already was) hooked in one of the slots.
    static bool install(void* target, const char* what) {
        if (!target) return false;
        std::lock_guard lk(mu());
        for (std::size_t i = 0; i < used(); ++i)
            if (targets()[i] == target) return true;  // already hooked via another name
        if (used() >= kMax) {
            VKO_LOG("hook slots exhausted for %s (target %p) - not hooked", what, target);
            return false;
        }
        const std::size_t i = used();
        auto r = safetyhook::InlineHook::create(target, detours()[i]);
        if (!r) {
            VKO_LOG("safetyhook failed to hook %s at %p (error %d)", what, target,
                    static_cast<int>(r.error().type));
            return false;
        }
        hooks()[i] = std::move(*r);
        targets()[i] = target;
        ++used();
        VKO_VLOG("hooked %s at %p (slot %zu)", what, target, i);
        return true;
    }

    // Restores the original bytes of every hook in this family.
    static void remove_all() {
        std::lock_guard lk(mu());
        for (std::size_t i = 0; i < used(); ++i) {
            hooks()[i].reset();  // safetyhook: stops threads, restores bytes, frees trampoline
            targets()[i] = nullptr;
        }
        used() = 0;
    }

private:
    template <std::size_t I>
    static R VKAPI_PTR detour(A... a) {
        return Tag::handle(hooks()[I].template original<Fn>(), a...);
    }

    template <std::size_t... I>
    static auto make_detours(std::index_sequence<I...>) {
        return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&detour<I>)...};
    }

    // Function-local statics (not static data members) keep this header-only and make the
    // construction order independent of the shared object's static-init order.
    // Heap-allocated and intentionally leaked: a static std::array would be destroyed during
    // process exit, and InlineHook's destructor un-patches the target and frees the
    // trampoline while other threads may still be executing through it (use-after-free).
    static auto& hooks() { static auto* h = new std::array<safetyhook::InlineHook, kMax>{}; return *h; }
    static auto& targets() { static std::array<void*, kMax> t{}; return t; }
    static std::size_t& used() { static std::size_t u = 0; return u; }
    static std::mutex& mu() { static std::mutex m; return m; }
    static const auto& detours() {
        static const auto d = make_detours(std::make_index_sequence<kMax>{});
        return d;
    }
};

}  // namespace vko
