#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/Kernel.h"
#include "memory/MemoryManager.h"
#include "persistence/SnapshotStore.h"
#include "process/ProcessManager.h"
#include "process/Scheduler.h"
#include "util/BlockingQueue.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

int main() {
    const std::filesystem::path testSnapshot = "data/unit_test_state.bin";
    const std::filesystem::path testSnapshotTmp = "data/unit_test_state.tmp";
    std::error_code cleanupError;
    std::filesystem::remove(testSnapshot, cleanupError);
    std::filesystem::remove(testSnapshotTmp, cleanupError);

    oscore::SnapshotStore store(testSnapshot.string());
    assert(store.defaultPath() == testSnapshot.string());

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

    oscore::ProcessManager schedProcesses;
    oscore::MemoryManager schedMemory;
    oscore::Scheduler scheduler;
    std::uint32_t schedAddr = 0;
    std::uint32_t schedPid = 0;
    assert(schedMemory.allocateForProcess("alice", 1, "p1", 64, schedAddr, message));
    assert(schedProcesses.createProcessWithMemory("alice", "p1", 64, schedAddr, 0, 5, std::nullopt, schedPid, message));
    assert(schedPid == 1);
    assert(schedMemory.allocateForProcess("alice", 2, "p2", 64, schedAddr, message));
    assert(schedProcesses.createProcessWithMemory("alice", "p2", 64, schedAddr, 8, 10, std::nullopt, schedPid, message));
    assert(schedPid == 2);
    const auto stepLog = scheduler.step("alice", schedProcesses, schedMemory);
    assert(stepLog.find("=== Scheduler Step ===") != std::string::npos);
    assert(stepLog.find("Selected PID=1") != std::string::npos);
    assert(stepLog.find("tick 1/2") != std::string::npos);
    assert(stepLog.find("Demote: Q0 -> Q1") != std::string::npos);
    assert(schedProcesses.showProcess("alice", 1).find("CPU Time: 2 / 5") != std::string::npos);
    assert(schedProcesses.readyQueueSnapshot("alice").find("Q1: 1") != std::string::npos);

    oscore::ProcessManager finishProcesses;
    oscore::MemoryManager finishMemory;
    assert(finishMemory.allocateForProcess("alice", 1, "short", 64, schedAddr, message));
    assert(finishProcesses.createProcessWithMemory("alice", "short", 64, schedAddr, 0, 1, std::nullopt, schedPid, message));
    const auto finishLog = scheduler.step("alice", finishProcesses, finishMemory);
    assert(finishLog.find("completed") != std::string::npos);
    assert(finishProcesses.listProcesses("alice").find("No process found") != std::string::npos);
    assert(finishMemory.usedMemoryKB() == 0);

    oscore::Kernel kernel(testSnapshot.string());
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
    assert(save.message.find("System state saved") != std::string::npos);
    assert(std::filesystem::exists(testSnapshot));

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

    const auto stepWithoutLogin = kernel.submitCommand("step");
    assert(!stepWithoutLogin.success);
    assert(stepWithoutLogin.message.find("requires login") != std::string::npos);

    const auto overviewWithoutLogin = kernel.submitCommand("overview");
    assert(!overviewWithoutLogin.success);
    assert(overviewWithoutLogin.message.find("login") != std::string::npos);

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
    auto invalidMemorySnapshot = kernel.exportSnapshot();
    for (auto& block : invalidMemorySnapshot.memoryBlocks) {
        if (block.type == oscore::MemoryBlockType::PROCESS && block.pid == 1) {
            ++block.start;
            break;
        }
    }
    std::string invalidSnapshotMessage;
    assert(!kernel.importSnapshot(invalidMemorySnapshot, invalidSnapshotMessage));
    assert(invalidSnapshotMessage.find("PCB memory") != std::string::npos);
    const auto readyq = kernel.submitCommand("readyq");
    assert(readyq.success);
    assert(readyq.message.find("Q0: 1") != std::string::npos);
    assert(readyq.message.find("Q1: 2") != std::string::npos);
    const auto schedStep = kernel.submitCommand("step");
    assert(schedStep.success);
    assert(schedStep.message.find("=== Scheduler Step ===") != std::string::npos);
    assert(schedStep.message.find("Selected PID=1") != std::string::npos);

    const auto overview = kernel.submitCommand("overview");
    assert(overview.success);
    assert(overview.message.find("System Overview") != std::string::npos);
    assert(overview.message.find("System Summary") != std::string::npos);
    assert(overview.message.find("Process Tree") != std::string::npos);
    assert(overview.message.find("Memory Map") != std::string::npos);
    assert(overview.message.find("MLFQ") != std::string::npos);
    assert(overview.message.find("dave") != std::string::npos);
    assert(overview.message.find("init") != std::string::npos);

    const auto writeFile = kernel.submitCommand("write_file note.txt hello operating system");
    assert(writeFile.success);
    const auto readFile = kernel.submitCommand("read_file note.txt");
    assert(readFile.success);
    assert(readFile.message.find("note.txt") != std::string::npos);
    assert(readFile.message.find("hello operating system") != std::string::npos);
    const auto listFiles = kernel.submitCommand("ls_file");
    assert(listFiles.success);
    assert(listFiles.message.find("note.txt") != std::string::npos);
    const auto touchFile = kernel.submitCommand("touch_file empty.txt");
    assert(touchFile.success);
    const auto listWithEmpty = kernel.submitCommand("ls_file");
    assert(listWithEmpty.success);
    assert(listWithEmpty.message.find("empty.txt") != std::string::npos);
    const auto removeEmpty = kernel.submitCommand("rm_file empty.txt");
    assert(removeEmpty.success);
    const auto listAfterRemove = kernel.submitCommand("ls_file");
    assert(listAfterRemove.success);
    assert(listAfterRemove.message.find("empty.txt") == std::string::npos);

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

    assert(kernel.submitCommand("create_pcb auto 64 0 3").success);
    const auto startSched = kernel.submitCommand("start_sched");
    assert(startSched.success);
    assert(startSched.message.find("started") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const auto autoStatus = kernel.submitCommand("status");
    assert(autoStatus.success);
    assert(autoStatus.message.find("Scheduler: RUNNING") != std::string::npos);
    const auto stopSched = kernel.submitCommand("stop_sched");
    assert(stopSched.success);
    assert(stopSched.message.find("stopped") != std::string::npos);
    assert(kernel.submitCommand("logout").success);

    assert(kernel.submitCommand("login dave pw").success);
    assert(kernel.submitCommand("create_pcb persisted 64 0 9").success);
    assert(kernel.submitCommand("set_alloc_algo BF").success);
    assert(kernel.submitCommand("write_file persisted.txt persistent vfs content").success);
    const auto saveSnapshot = kernel.submitCommand("save");
    assert(saveSnapshot.success);
    assert(saveSnapshot.message.find("Processes:") != std::string::npos);
    assert(kernel.submitCommand("create_pcb temp 64 0 5").success);
    assert(kernel.submitCommand("list_pcb").message.find("temp") != std::string::npos);
    assert(kernel.submitCommand("write_file temp_vfs.txt should disappear after load").success);
    const auto loadSnapshot = kernel.submitCommand("load");
    assert(loadSnapshot.success);
    assert(loadSnapshot.message.find("Please login again") != std::string::npos);
    assert(kernel.submitCommand("whoami").message == "not logged in");
    assert(kernel.submitCommand("login dave pw").success);
    const auto restoredList = kernel.submitCommand("list_pcb");
    assert(restoredList.success);
    assert(restoredList.message.find("temp") == std::string::npos);
    const auto restoredFiles = kernel.submitCommand("ls_file");
    assert(restoredFiles.success);
    assert(restoredFiles.message.find("persisted.txt") != std::string::npos);
    assert(restoredFiles.message.find("temp_vfs.txt") == std::string::npos);
    const auto restoredFile = kernel.submitCommand("read_file persisted.txt");
    assert(restoredFile.success);
    assert(restoredFile.message.find("persistent vfs content") != std::string::npos);
    const auto missingTempFile = kernel.submitCommand("read_file temp_vfs.txt");
    assert(missingTempFile.message.find("does not exist") != std::string::npos);
    assert(kernel.submitCommand("status").message.find("Snapshot File: data/unit_test_state.bin") != std::string::npos);
    assert(kernel.submitCommand("logout").success);

    oscore::Kernel reloadedKernel(testSnapshot.string());
    reloadedKernel.start();
    const auto reloadStatus = reloadedKernel.submitCommand("status");
    assert(reloadStatus.success);
    assert(reloadStatus.message.find("Auto Load: success") != std::string::npos);
    assert(reloadedKernel.submitCommand("whoami").message == "not logged in");
    assert(reloadedKernel.submitCommand("login dave pw").success);
    assert(reloadedKernel.submitCommand("list_pcb").message.find("persisted") != std::string::npos);
    const auto reloadedFiles = reloadedKernel.submitCommand("ls_file");
    assert(reloadedFiles.success);
    assert(reloadedFiles.message.find("persisted.txt") != std::string::npos);
    const auto reloadedFile = reloadedKernel.submitCommand("read_file persisted.txt");
    assert(reloadedFile.success);
    assert(reloadedFile.message.find("persistent vfs content") != std::string::npos);
    assert(reloadedKernel.submitCommand("status").message.find("Allocation Algorithm: BEST_FIT") != std::string::npos);
    reloadedKernel.stop();

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
