#include "app/ConsoleApp.h"
#include "ipc/InstanceGuard.h"

#include <clocale>
#include <iostream>
#include <windows.h>

namespace {

// 将控制台输入、输出和 C 运行时区域设置统一调整为 UTF-8。
void configureConsoleEncoding() {
    // Windows 控制台默认代码页可能不是 UTF-8，会导致中文菜单输出乱码。
    // 源码已经通过 /utf-8 编译，这里把终端输入/输出代码页也切到 UTF-8。
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
}

} // namespace

int main() {
    // 程序入口首先修正控制台编码，后续中文菜单、提示和错误信息才能正常显示。
    configureConsoleEncoding();

    // 多实例选主：通过 Windows Named Mutex 判定当前实例是 MASTER 还是 CLIENT
    oscore::InstanceGuard guard;
    // initialize() 会创建或打开全局互斥量；失败时无法可靠判断主/从角色，必须直接退出。
    if (!guard.initialize()) {
        std::cerr << "[FATAL] Failed to initialize instance guard." << std::endl;
        return 1;
    }

    // ConsoleApp 接收 InstanceGuard 判定出的角色，并进入对应的 MASTER/CLIENT 控制台循环。
    oscore::ConsoleApp app;
    return app.run(guard.role());
}
