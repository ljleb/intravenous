#include <meta>

class NonStructuralConfig {
public:
    constexpr explicit NonStructuralConfig(int channels)
        : channels_(channels)
    {
    }

private:
    int channels_;
};

// This is intentionally a negative compiler probe. GCC 16.2 rejects it
// because reflect_constant currently requires the value's type to be
// structural; the private member makes NonStructuralConfig non-structural.
constexpr auto reflection = std::meta::reflect_constant(NonStructuralConfig { 6 });
