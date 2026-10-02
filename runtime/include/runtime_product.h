#pragma once

#include <string_view>

namespace RuntimeProduct {

struct Descriptor {
    std::string_view displayName;
};

// Defined by the executable's own source (src/product/base_product.cpp), not by the shared runtime
// library.
const Descriptor& Active() noexcept;

} // namespace RuntimeProduct
