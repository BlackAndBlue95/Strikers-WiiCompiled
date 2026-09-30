#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
        "Strikers-WiiCompiled",
    };
    return descriptor;
}

} // namespace RuntimeProduct
