// Interposes the Itanium C++ ABI's `__cxa_throw` so every C++ throw in the
// process records its stack before unwinding begins (see throw_site.h). This
// translation unit deliberately includes neither <cxxabi.h> nor <stacktrace>,
// so no other declaration of `__cxa_throw` is visible here.
//
// libiv_builder precedes libstdc++ in the global symbol search order of every
// executable that links it, so this definition also serves package libraries
// loaded later. `RTLD_NEXT` then finds the C++ runtime's definition.

#include <dlfcn.h>

#include <cstdlib>
#include <typeinfo>

namespace iv::details {
void capture_throw_site(void const* exception_object) noexcept;
} // namespace iv::details

extern "C" {
using IvCxaThrow = void (*)(void*, std::type_info*, void (*)(void*));

__attribute__((visibility("default"), noreturn)) void __cxa_throw(
    void* exception_object, std::type_info* type, void (*destructor)(void*))
{
    iv::details::capture_throw_site(exception_object);
    static auto const next = reinterpret_cast<IvCxaThrow>(
        dlsym(RTLD_NEXT, "__cxa_throw"));
    if (!next) std::abort();
    next(exception_object, type, destructor);
    __builtin_unreachable();
}
}
