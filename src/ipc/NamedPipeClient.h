#pragma once

namespace oscore {

class NamedPipeClient {
public:
    [[nodiscard]] bool available() const;
};

} // namespace oscore
