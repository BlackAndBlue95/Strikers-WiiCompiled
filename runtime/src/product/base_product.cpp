#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{"Strikers Recharged"};
    return descriptor;
}

} // namespace RuntimeProduct
