#include "ipc/NamedPipeClient.h"

namespace oscore {

bool NamedPipeClient::available() const {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

} // namespace oscore
