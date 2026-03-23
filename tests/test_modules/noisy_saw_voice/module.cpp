#include "module/module.h"
#include "modules/noisy_saw.h"

namespace {
    iv::TypeErasedNode noisy_saw_voice(iv::ModuleContext const&)
    {
        return iv::modules::noisy_saw();
    }
}

IV_EXPORT_MODULE("iv.test.noisy_saw_voice", noisy_saw_voice);
