#include "kernel/CommandDispatcher.h"
#include "util/BlockingQueue.h"

#include <cassert>
#include <string>
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

    assert(queue.pop() == 7);
    assert(queue.pop() == 11);

    return 0;
}
