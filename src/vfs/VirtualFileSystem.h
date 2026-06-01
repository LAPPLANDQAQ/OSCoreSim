#pragma once

#include <cstddef>

namespace oscore {

class VirtualFileSystem {
public:
    [[nodiscard]] std::size_t fileCount() const;
};

} // namespace oscore
