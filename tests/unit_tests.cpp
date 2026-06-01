#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/Kernel.h"
#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"
#include "util/BlockingQueue.h"

#include <cassert>
#include <optional>
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

    oscore::ProcessManager processes;
    assert(processes.createProcess("alice", "init", 64, 0, 20, std::nullopt, message));
    assert(message.find("PID=1") != std::string::npos);
    assert(processes.createProcess("alice", "shell", 128, 5, 12, 1, message));
    assert(processes.createProcess("alice", "worker", 32, 8, 8, 2, message));
    assert(processes.createProcess("bob", "bob_init", 64, 0, 10, std::nullopt, message));
    assert(!processes.createProcess("bob", "bad_child", 64, 0, 10, 1, message));

    const auto aliceList = processes.listProcesses("alice");
    assert(aliceList.find("init") != std::string::npos);
    assert(aliceList.find("bob_init") == std::string::npos);
    assert(processes.showProcess("alice", 2).find("Name: shell") != std::string::npos);
    assert(processes.showProcess("bob", 2).find("access denied") != std::string::npos);
    assert(processes.readyQueueSnapshot("alice").find("Q0: 1") != std::string::npos);
    assert(processes.readyQueueSnapshot("alice").find("Q1: 2") != std::string::npos);
    assert(processes.readyQueueSnapshot("alice").find("Q2: 3") != std::string::npos);

    assert(processes.blockProcess("alice", 2, message));
    assert(processes.readyQueueSnapshot("alice").find("Q1: empty") != std::string::npos);
    assert(processes.wakeupProcess("alice", 2, message));
    assert(processes.suspendProcess("alice", 3, message));
    assert(processes.readyQueueSnapshot("alice").find("Q2: empty") != std::string::npos);
    assert(processes.resumeProcess("alice", 3, message));
    assert(processes.reniceProcess("alice", 3, 2, message));
    assert(message.find("8(Q2) -> 2(Q0)") != std::string::npos);
    assert(processes.processTree("alice").find("worker(3)") != std::string::npos);
    assert(processes.killProcess("alice", 1, message));
    assert(message.find("1") != std::string::npos);
    assert(message.find("2") != std::string::npos);
    assert(message.find("3") != std::string::npos);
    assert(processes.listProcesses("alice").find("No process found") != std::string::npos);
    assert(processes.listProcesses("bob").find("bob_init") != std::string::npos);

    oscore::MemoryManager memory;
    std::uint32_t addr = 0;
    assert(memory.allocateManual("alice", 100, addr, message));
    assert(addr == 0);
    assert(memory.allocateManual("alice", 80, addr, message));
    assert(addr == 100);
    assert(memory.freeByAddress("alice", 0, message));
    assert(memory.setAlgorithm("BF", message));
    assert(memory.allocateManual("alice", 50, addr, message));
    assert(addr == 0);
    assert(memory.setAlgorithm("WF", message));
    assert(memory.allocateManual("alice", 30, addr, message));
    assert(addr == 180);
    assert(!memory.freeByAddress("bob", 50, message));
    assert(memory.showMemory("alice").find("KERNEL") != std::string::npos);
    assert(memory.memoryStat().find("External Fragmentation") != std::string::npos);

    std::uint32_t processAddr = 0;
    assert(memory.allocateForProcess("alice", 42, "worker", 64, processAddr, message));
    assert(memory.swapOutProcess("alice", 42, message));
    assert(message.find("Released 64KB") != std::string::npos);

    oscore::MemoryManager compactMemory;
    assert(compactMemory.allocateForProcess("alice", 1, "p1", 64, addr, message));
    assert(addr == 0);
    assert(compactMemory.allocateForProcess("alice", 2, "p2", 128, addr, message));
    assert(addr == 64);
    assert(compactMemory.allocateManual("alice", 32, addr, message));
    assert(addr == 192);
    assert(compactMemory.freeByPid("alice", 1, message));
    const auto compactResult = compactMemory.compact();
    assert(compactResult.success);
    assert(compactResult.pidNewStart.at(2) == 0);

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

    const auto createWithoutLogin = kernel.submitCommand("create_pcb test 64 0 10");
    assert(!createWithoutLogin.success);
    assert(createWithoutLogin.message.find("requires login") != std::string::npos);

    const auto showMemWithoutLogin = kernel.submitCommand("show_mem");
    assert(!showMemWithoutLogin.success);
    assert(showMemWithoutLogin.message.find("requires login") != std::string::npos);

    assert(kernel.submitCommand("register dave pw").success);
    assert(kernel.submitCommand("login dave pw").success);
    const auto initialMem = kernel.submitCommand("show_mem");
    assert(initialMem.success);
    assert(initialMem.message.find("FREE") != std::string::npos);
    const auto manualAlloc = kernel.submitCommand("alloc 100");
    assert(manualAlloc.success);
    assert(manualAlloc.message.find("start=0KB") != std::string::npos);
    const auto createInit = kernel.submitCommand("create_pcb init 64 0 20");
    assert(createInit.success);
    assert(createInit.message.find("PID=1") != std::string::npos);
    assert(kernel.submitCommand("show_pcb 1").message.find("Memory: start=100KB") != std::string::npos);
    assert(kernel.submitCommand("create_pcb shell 128 5 12 1").success);
    const auto memAfterProcesses = kernel.submitCommand("show_mem");
    assert(memAfterProcesses.success);
    assert(memAfterProcesses.message.find("PROCESS") != std::string::npos);
    const auto readyq = kernel.submitCommand("readyq");
    assert(readyq.success);
    assert(readyq.message.find("Q0: 1") != std::string::npos);
    assert(readyq.message.find("Q1: 2") != std::string::npos);
    const auto blockShell = kernel.submitCommand("block_pcb 2");
    assert(blockShell.success);
    assert(kernel.submitCommand("wakeup_pcb 2").success);
    assert(kernel.submitCommand("renice 2 1").success);
    const auto swapShell = kernel.submitCommand("swap_out 2");
    assert(swapShell.success);
    assert(swapShell.message.find("swapped out") != std::string::npos);
    assert(kernel.submitCommand("show_pcb 2").message.find("Swapped Out: true") != std::string::npos);
    const auto tree = kernel.submitCommand("ptree");
    assert(tree.success);
    assert(tree.message.find("shell(2)") != std::string::npos);
    assert(kernel.submitCommand("pgfault 1").message.find("[PAGE FAULT] PID=1") != std::string::npos);
    assert(kernel.submitCommand("kill_pcb 1").success);
    assert(kernel.submitCommand("show_mem").message.find("FREE") != std::string::npos);
    assert(kernel.submitCommand("logout").success);

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
