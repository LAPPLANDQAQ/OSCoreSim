#include "kernel/Kernel.h"

#include <exception>
#include <future>
#include <sstream>
#include <utility>

namespace oscore {

Kernel::~Kernel() {
    stop();
}

void Kernel::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (started_) {
        return;
    }

    // 每次启动前重置队列，保证旧的 shutdown 状态不会影响本次工作线程。
    requestQueue_.reset();
    stopping_ = false;
    workerRunning_ = false;
    started_ = true;
    workerThread_ = std::thread(&Kernel::workerLoop, this);
}

void Kernel::stop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ && !workerThread_.joinable()) {
            return;
        }
        stopping_ = true;
    }

    requestQueue_.shutdown();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    started_ = false;
    workerRunning_ = false;
}

CommandResponse Kernel::submitCommand(
    const std::string& rawLine,
    const std::string& username,
    CommandSource source) {
    auto promise = std::make_shared<std::promise<CommandResponse>>();
    auto future = promise->get_future();

    CommandRequest request;
    request.rawLine = rawLine;
    request.username = username;
    request.source = source;
    request.promise = std::move(promise);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ || stopping_) {
            return {false, "Kernel is not running.", false};
        }
        request.id = nextRequestId_++;
    }

    try {
        requestQueue_.push(std::move(request));
    } catch (const std::exception& ex) {
        std::ostringstream output;
        output << "Failed to submit command: " << ex.what();
        return {false, output.str(), false};
    }

    return future.get();
}

bool Kernel::isWorkerRunning() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return workerRunning_;
}

bool Kernel::isLoggedIn() const {
    return userManager_.isLoggedIn();
}

std::string Kernel::currentUser() const {
    return userManager_.currentUser();
}

void Kernel::workerLoop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        workerRunning_ = true;
    }

    CommandRequest request;
    while (requestQueue_.pop(request)) {
        auto response = executeRequest(request);
        if (request.promise) {
            request.promise->set_value(response);
        }

        if (response.shouldExit) {
            // exit/quit 由后台线程确认后触发队列关闭，前台随后调用 stop() 完成 join。
            requestQueue_.shutdown();
            break;
        }
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    workerRunning_ = false;
}

CommandResponse Kernel::executeRequest(const CommandRequest& request) {
    try {
        const auto command = dispatcher_.parse(request.rawLine);
        const CommandContext context{
            request.id,
            userManager_.currentUser(),
            request.source,
            isWorkerRunning()
        };
        return dispatcher_.dispatch(command, context, userManager_);
    } catch (const std::exception& ex) {
        std::ostringstream output;
        output << "Command execution failed: " << ex.what();
        return {false, output.str(), false};
    }
}

} // namespace oscore
