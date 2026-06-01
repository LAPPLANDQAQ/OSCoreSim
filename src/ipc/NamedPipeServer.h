#pragma once

namespace oscore {

class NamedPipeServer {
public:
    [[nodiscard]] bool available() const;
};

} // namespace oscore
