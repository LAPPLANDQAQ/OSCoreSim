#pragma once

#include <string>

namespace oscore {

class SnapshotStore {
public:
    [[nodiscard]] std::string defaultPath() const;
};

} // namespace oscore
