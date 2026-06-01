#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/Kernel.h"
#include "util/BlockingQueue.h"

#include <cassert>
#include <string>
#include <thread>
#include <vector>

int main() {
    oscore::CommandDispatcher dispatcher;
    const auto command = dispatcher.parse("  create_pcb   worker 64  0 20 ");

    assert(command.name == "create_pcb");
    assert((command.arguments == std::vector<std::string>{"worker", "64", "0", "20"}));
    assert(command.rawLine == "  create_pcb   worker 64  0 20 ");

    oscore::BlockingQueue<int> queue;
    queue.push(7);
    queue.push(11);

    int value = 0;
    assert(queue.pop(value));
    assert(value == 7);
    assert(queue.pop(value));
    assert(value == 11);

    std::thread waitingThread([&queue] {
        int ignored = 0;
        assert(!queue.pop(ignored));
    });
    queue.shutdown();
    waitingThread.join();

    oscore::UserManager users;
    std::string message;
    assert(!users.registerUser("", "pw", message));
    assert(!users.registerUser("bad name", "pw", message));
    assert(!users.registerUser("alice", "", message));
    assert(users.registerUser("alice", "123456", message));
    assert(users.userExists("alice"));
    assert(!users.registerUser("alice", "999999", message));

    const auto exportedUsers = users.exportUsers();
    assert(exportedUsers.size() == 1);
    assert(exportedUsers.front().username == "alice");
    assert(exportedUsers.front().passwordHash != "123456");
    assert(!exportedUsers.front().salt.empty());

    assert(!users.login("alice", "wrong1", message));
    assert(!users.login("alice", "wrong2", message));
    assert(!users.login("alice", "wrong3", message));
    assert(!users.login("alice", "123456", message));
    assert(message.find("locked") != std::string::npos);

    assert(users.registerUser("bob", "abc", message));
    assert(!users.login("bob", "bad", message));
    assert(users.login("bob", "abc", message));
    assert(users.isLoggedIn());
    assert(users.currentUser() == "bob");
    assert(users.whoami() == "bob");
    assert(users.logout(message));
    assert(!users.isLoggedIn());
    assert(users.whoami() == "not logged in");

    oscore::Kernel kernel;
    kernel.start();

    const auto help = kernel.submitCommand("help");
    assert(help.success);
    assert(help.message.find("help") != std::string::npos);

    const auto status = kernel.submitCommand("status");
    assert(status.success);
    assert(status.message.find("Worker Thread: RUNNING") != std::string::npos);
    assert(status.message.find("Current User: <none>") != std::string::npos);

    const auto save = kernel.submitCommand("save");
    assert(save.success);
    assert(save.message == "[TODO] binary save will be implemented in persistence phase");

    const auto whoamiBeforeLogin = kernel.submitCommand("whoami");
    assert(whoamiBeforeLogin.success);
    assert(whoamiBeforeLogin.message == "not logged in");

    const auto registerCarol = kernel.submitCommand("register carol pass123");
    assert(registerCarol.success);

    const auto duplicateCarol = kernel.submitCommand("register carol other");
    assert(!duplicateCarol.success);

    const auto loginCarol = kernel.submitCommand("login carol pass123");
    assert(loginCarol.success);

    const auto whoamiAfterLogin = kernel.submitCommand("whoami");
    assert(whoamiAfterLogin.success);
    assert(whoamiAfterLogin.message == "carol");

    const auto statusWithUser = kernel.submitCommand("status");
    assert(statusWithUser.success);
    assert(statusWithUser.message.find("Current User: carol") != std::string::npos);

    const auto logoutCarol = kernel.submitCommand("logout");
    assert(logoutCarol.success);

    const auto unknown = kernel.submitCommand("unknown_command");
    assert(!unknown.success);
    assert(unknown.message.find("Unknown command") != std::string::npos);

    const auto exit = kernel.submitCommand("exit");
    assert(exit.success);
    assert(exit.shouldExit);
    kernel.stop();
    assert(!kernel.isWorkerRunning());

    return 0;
}
