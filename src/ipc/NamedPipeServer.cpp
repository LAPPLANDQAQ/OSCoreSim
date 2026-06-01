#include "ipc/NamedPipeServer.h"

namespace oscore {

bool NamedPipeServer::available() const {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

} // namespace oscore
