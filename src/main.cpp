#include "app/ConsoleApp.h"
#include "ipc/InstanceGuard.h"

#include <iostream>

int main() {
    // 多实例选主：通过 Windows Named Mutex 判定当前实例是 MASTER 还是 CLIENT
    oscore::InstanceGuard guard;
    if (!guard.initialize()) {
        std::cerr << "[FATAL] Failed to initialize instance guard." << std::endl;
        return 1;
    }

    oscore::ConsoleApp app;
    return app.run(guard.role());
}
