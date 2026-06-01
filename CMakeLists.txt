cmake_minimum_required(VERSION 3.20)

project(OSCoreSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

set(OS_SIM_CORE_SOURCES
    src/kernel/Kernel.cpp
    src/kernel/CommandDispatcher.cpp
    src/auth/UserManager.cpp
    src/process/ProcessManager.cpp
    src/process/Scheduler.cpp
    src/memory/MemoryManager.cpp
    src/persistence/SnapshotStore.cpp
)

set(OS_SIM_SOURCES
    src/app/ConsoleApp.cpp
    src/ipc/InstanceGuard.cpp
    ${OS_SIM_CORE_SOURCES}
    src/view/OverviewRenderer.cpp
    src/ipc/NamedPipeServer.cpp
    src/ipc/NamedPipeClient.cpp
    src/vfs/VirtualFileSystem.cpp
)

add_executable(os_sim
    src/main.cpp
    ${OS_SIM_SOURCES}
)

target_include_directories(os_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(os_sim_tests
    tests/unit_tests.cpp
    src/view/OverviewRenderer.cpp
    src/vfs/VirtualFileSystem.cpp
    ${OS_SIM_CORE_SOURCES}
)

target_include_directories(os_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(os_sim PRIVATE /utf-8)
    target_compile_options(os_sim_tests PRIVATE /utf-8)
endif()

if(BUILD_TESTING)
    add_test(NAME os_sim_tests COMMAND os_sim_tests)
endif()
