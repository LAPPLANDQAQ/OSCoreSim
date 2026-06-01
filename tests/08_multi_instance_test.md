# P8 多实例 Named Pipe IPC 验收测试
# 需要同时打开两个终端窗口

# === 窗口 A (Master) ===
# 启动: .\build\Release\os_sim.exe
# 预期: 显示 Role: MASTER
# 执行:
register bob abc
login bob abc
create_pcb init 64 0 20
overview

# === 窗口 B (Client) ===
# 启动: .\build\Release\os_sim.exe (保持窗口 A 运行)
# 预期: 显示 Role: CLIENT, Connected to Kernel Master through Named Pipe
# 执行:
whoami
# 预期: bob
overview
# 预期: 看到 init 进程
create_pcb shell 128 5 12 1
# 预期: 进程创建成功

# === 窗口 A ===
overview
# 预期: 看到 shell 进程 (由 Client 创建)

# === 窗口 B ===
start_sched
stop_sched

# === 窗口 A ===
# 预期: 调度器正常运行，无死锁

# === 窗口 B ===
exit
# 预期: Client 退出

# === 窗口 A ===
overview
# 预期: Master 正常运行
save
exit
# 预期: 正常保存退出
