#pragma once

// ======================================================
//  CommandFramework — 运行时指令框架（长连接 + JSON 协议 + 看门狗）
// ======================================================
//
//  职责：
//    管理外部启动器（Python 浏览器）与游戏 DLL 之间的持久命名管道通信。
//    采用 <CMD>\t<JSON>\n 文本行协议，支持多命令交互、心跳保活、
//    接收端超时断线重建。
//
//  协议：
//    PROTOCOL_DELIM  = '\t' （命令与 JSON 参数分隔符）
//    PROTOCOL_NEWLINE = '\n' （每行终止符）
//
//    命令清单（方向：L = 启动器, D = DLL）
//      L → D  ping      心跳请求
//      D → L  pong      心跳响应
//      L → D  join      请求连接目标比赛（ip + token）
//      D → L  join_ack  确认收到 join
//      D → L  error     解析错误 / 未知命令
//
//  使用方式：
//    1. 构造实例，SetPipeName() + SetJoinCallback() + SetLogCallback()
//    2. Start() 启动监听线程
//    3. 启动器侧通过 PipeClient 连接同名管道并发送命令
//    4. Stop() 安全停止（设置停止标志 → 取消 I/O → join 线程）
//
//  设计原则：
//    - Overlapped I/O 替代同步 ReadFile + CancelSynchronousIo，看门狗
//      由 WaitForSingleObject 超时自然实现，无需独立看门狗线程
//    - 所有管道写入受 writeMutex 保护，回调均在监听线程内同步执行
//    - 连接期间断线自动重建管道等待新客户端（单实例模式）
//    - 回调通过 std::function 注入，不依赖具体实现

#include <windows.h>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include "../Libs/json.hpp"

class CommandFramework
{
public:
    // ------------------------------------------------------------------
    //  回调类型定义
    // ------------------------------------------------------------------

    // @brief 收到 join 命令时调用。不会在锁内执行。
    // @param ip    目标比赛 IP:PORT
    // @param token 鉴权令牌（预留）
    using JoinCallback = std::function<void(const std::string& ip, const std::string& token)>;

    // @brief 内部日志回调，所有模块内部事件均通过此回调输出
    // @param msg 日志消息（不含换行符）
    using LogCallback = std::function<void(const std::string& msg)>;

    // ------------------------------------------------------------------
    //  构造 / 析构
    // ------------------------------------------------------------------

    CommandFramework();
    ~CommandFramework();

    // ------------------------------------------------------------------
    //  配置 — 必须在 Start() 前调用
    // ------------------------------------------------------------------

    // @brief 设置管道名称（不含 \\\\.\pipe\\ 前缀）
    // @param name 管道名称，如 "ProjectRebound_A1B2C3D4"
    void SetPipeName(const std::string& name);

    // @brief 设置读超时时间。启动器在此时间内未发送任何数据时断线。
    // @param timeoutMs 超时毫秒数（默认 30000）
    void SetWatchdogTimeout(DWORD timeoutMs);

    // @brief 注入 join 命令回调
    void SetJoinCallback(JoinCallback cb);

    // @brief 注入日志回调
    void SetLogCallback(LogCallback cb);

    // @brief 注入 debug 命令回调。收到 debug 命令时调用，传入 JSON 参数，
    //        回调返回的 JSON 通过 SendResponse("debug_ack", ...) 回复。
    // @param cb 回调签名：nlohmann::json(const nlohmann::json&)
    using DebugCallback = std::function<nlohmann::json(const nlohmann::json&)>;
    void SetDebugCallback(DebugCallback cb);

    // ------------------------------------------------------------------
    //  生命周期
    // ------------------------------------------------------------------

    // @brief 启动监听线程，创建命名管道并等待启动器连接。
    // @return true=成功启动, false=已在运行或管道名为空
    bool Start();

    // @brief 安全停止。设置停止标志 → 取消当前 I/O → 等待线程退出（上限 5s）。
    //        超过 5s 未退出则 detach() 交由操作系统回收。
    void Stop();

    // ------------------------------------------------------------------
    //  协议常量
    // ------------------------------------------------------------------

    static constexpr char PROTOCOL_DELIM    = '\t';
    static constexpr char PROTOCOL_NEWLINE  = '\n';

    // ------------------------------------------------------------------
    //  回写能力 — 向已连接启动器发送响应
    // ------------------------------------------------------------------

    // @brief 向当前连接的管道写入一行命令响应。
    //        无连接时静默丢弃。调用线程安全。
    // @param cmd  命令标识（如 "pong", "join_ack", "error"）
    // @param args JSON 参数体
    void SendResponse(const std::string& cmd, const nlohmann::json& args);

private:
    // ------------------------------------------------------------------
    //  监听线程（单线程，Overlapped I/O）
    // ------------------------------------------------------------------

    // @brief 主循环：CreateNamedPipe → ConnectNamedPipe（overlapped）→
    //        ReadFile（overlapped）→ 拆行分发 → 断线重建管道
    void ListenerLoop();

    // ------------------------------------------------------------------
    //  协议解析
    // ------------------------------------------------------------------

    // @brief 按 PROTOCOL_DELIM 拆分为 CMD + JSON，解析 JSON 后调用 Dispatch()
    void ParseAndDispatch(const std::string& line);

    // @brief 按 CMD 分发到对应处理逻辑
    void Dispatch(const std::string& cmd, const nlohmann::json& args);

    // ------------------------------------------------------------------
    //  内部工具
    // ------------------------------------------------------------------

    // @brief 通过注入的日志回调输出（若未设置则静默）
    void Log(const std::string& msg);

    // ------------------------------------------------------------------
    //  配置
    // ------------------------------------------------------------------

    std::string pipeName;            // 完整管道路径（含 \\.\pipe\ 前缀）
    DWORD       watchdogTimeoutMs;   // 读超时（毫秒），默认 30000

    // ------------------------------------------------------------------
    //  运行时状态
    // ------------------------------------------------------------------

    std::atomic<bool> running;       // 监听循环运行标志
    HANDLE            hCurrentPipe;  // 当前连接的管道句柄（INVALID_HANDLE_VALUE = 无连接）
    std::mutex        writeMutex;    // 保护 hCurrentPipe 读写 + WriteFile 互斥

    // ------------------------------------------------------------------
    //  线程
    // ------------------------------------------------------------------

    std::thread listenerThread;

    // ------------------------------------------------------------------
    //  回调
    // ------------------------------------------------------------------

    JoinCallback onJoin;    // join 命令回调
    LogCallback  onLog;     // 日志回调
    DebugCallback onDebug;  // debug 命令回调

    // ------------------------------------------------------------------
    //  安全描述符 — NULL DACL 允许任意进程连接
    // ------------------------------------------------------------------

    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    bool                saInitialized;
};
