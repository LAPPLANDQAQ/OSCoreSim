#include "persistence/SnapshotStore.h"

namespace oscore {

std::string SnapshotStore::defaultPath() const {
    return "data/os_state.bin";
}

} // namespace oscore
