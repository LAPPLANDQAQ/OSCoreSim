#include "app/ConsoleApp.h"
#include "ipc/InstanceGuard.h"

#include <clocale>
#include <iostream>
#include <windows.h>

namespace {

void configureConsoleEncoding() {
    // Windows 控制台默认代码页可能不是 UTF-8，会导致中文菜单输出乱码。
    // 源码已经通过 /utf-8 编译，这里把终端输入/输出代码页也切到 UTF-8。
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
}

} // namespace

int main() {
    configureConsoleEncoding();

    // 多实例选主：通过 Windows Named Mutex 判定当前实例是 MASTER 还是 CLIENT
    oscore::InstanceGuard guard;
    if (!guard.initialize()) {
        std::cerr << "[FATAL] Failed to initialize instance guard." << std::endl;
        return 1;
    }

    oscore::ConsoleApp app;
    return app.run(guard.role());
}
