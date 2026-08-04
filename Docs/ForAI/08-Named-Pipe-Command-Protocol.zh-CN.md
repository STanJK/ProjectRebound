# ProjectRebound 命名管道命令协议

[English](command-framework.md) | 简体中文

`ExternalCommandPipe` 是 launcher 与游戏进程内 Payload 之间的本地命令通道。
Payload 是单实例管道服务端。launcher 为每次游戏运行生成高熵管道名，通过 Windows
Wrapper 将 `-pipe=<name>` 传给游戏，并以同一 Windows 用户、同一会话连接
`\\.\pipe\<name>`。

实现被拆成三个边界：

- `ProjectReboundMainDLL/API/CommandProtocol.*`：可移植的帧解析、编码和比赛目标校验；
- `ProjectReboundMainDLL/API/ExternalCommandPipe.*`：Windows 命名管道安全、Overlapped
  I/O、生命周期、分发和响应；
- `ProjectReboundMainDLL/Client/AutoConnect.*` 与 `ProjectReboundMainDLL/Hooking/ClientHooks.cpp`：线程安全的加入队列
  和游戏线程执行。

所有权、锁规则、调用流、剩余约束和完整测试矩阵见
[函数—结构—代码分析指南](command-framework-code-analysis.zh-CN.md)。

## 传输与帧格式

服务端使用消息模式、双向、单活动实例的命名管道。每条应用消息是一行 UTF-8 文本：

```text
<command>\t<json-object>\n
```

规则如下：

- 命令与 JSON 之间是一个 Tab，帧以 LF 结束；输入也兼容 CRLF；
- 命令长度为 1–32 字节，只允许小写 ASCII 字母、数字、`_` 和 `-`；
- JSON 不能为空且必须是对象；
- 包含 LF 在内的完整线上帧最大为 64 KiB；
- 可选 `request_id` 必须是 1–128 字节的非空字符串；该请求的响应会原样回显；
- 畸形输入会得到 `error`；同一连接累计三个协议错误后会被服务端断开。

示例：

```text
ping\t{"request_id":"health-17"}\n
pong\t{"request_id":"health-17"}\n
```

## 命令

| 请求 | 必需字段 | 成功响应 | 含义 |
| --- | --- | --- | --- |
| `ping` | 无 | `pong` | 检查通道是否存活 |
| `join` | `ip`：字符串 | `join_ack` | 接受一个游戏线程比赛跳转 |
| `debug` | 由回调定义 | `debug_ack` | 执行注册到 Payload 的调试处理器 |
| `server_status` | 无 | `server_status_ack` | 为 launcher 所有的签名心跳读取非秘密专服运行状态 |

`server_status` 仅在宿主入口注册 `ServerStatusCallback` 后可用，约定响应字段为
`state`、非负 `player_count` 和诊断字段 `round_state`。当前 StanJK 客户端入口没有注册
这个可选回调，因此会返回 `unavailable`。注册令牌、节点私钥、运行令牌、证书、CSR 和签名
都禁止出现在该请求或响应中。

`join.ip` 表示完整目标，不只是 IP。允许 `host:port`、`IPv4:port` 和
`[IPv6]:port`；端口必须在 1–65535 之间，完整目标最多 512 字节。构造 Unreal
控制台命令前会拒绝空白、控制字符、控制台分隔符和未加方括号的 IPv6。

`join.token` 可选，最大 4096 字节，目前是预留字段：Payload 会把它传给回调，但不使用
它做认证。

成功响应表示目标已经通过校验并进入队列：

```text
join_ack\t{"request_id":"join-42","status":"accepted"}\n
```

它**不表示**远端游戏连接已经完成。如果已有跳转待处理，服务端返回错误码 `busy`，不会
给出假成功的 `join_ack`。

调试回调可以返回任意 JSON 值。非对象结果会包装成 `{"result": ...}`，保证所有响应仍
符合“JSON 对象载荷”规则。

## 错误响应

错误结构稳定：

```text
error\t{"code":"invalid_target","message":"port must be between 1 and 65535","request_id":"join-42"}\n
```

当前错误码包括 `empty_frame`、`frame_too_large`、`missing_delimiter`、
`invalid_command`、`missing_json`、`invalid_json`、`invalid_json_type`、
`invalid_request_id`、`invalid_request`、`invalid_target`、`unknown_command`、
`unavailable`、`busy` 和 `internal_error`。

客户端应按 `code` 分支，不应依赖面向人的 `message` 文案。

## 生命周期、并发与超时

- `Start()` 校验名称、构造当前用户 ACL、创建手动复位停止事件并启动一个监听线程；
- 连接、读取和写入都使用带事件的 `OVERLAPPED`。取消或超时后一定会先通过
  `GetOverlappedResult` 等到操作进入终态，再释放事件、缓冲区或栈内存；
- 默认空闲读超时为 30 秒，写超时为 5 秒；配置为 0 表示不超时；
- `SendResponse()` 串行化写入，可从其他线程调用；
- `Stop()` 发出停止信号、取消当前 I/O 并确定性 join，绝不 detach 捕获框架对象的线程；
- 断线、超时和可恢复传输错误会回到新的 `CreateNamedPipeW`/连接周期。

`join` 回调在监听线程执行，但不做任何 Unreal 操作，只校验并排队目标。客户端
`ProcessEvent` Hook 在登录事件记录游戏线程 ID，后续只允许该确切线程泵送排队的跳转，
并保留登录后、进入靶场后的稳定等待阶段。

## 安全边界

管道使用以下限制：

- DACL 只向 Payload 进程用户 SID 授予读写；
- `PIPE_REJECT_REMOTE_CLIENTS` 拒绝远程客户端；
- `FILE_FLAG_FIRST_PIPE_INSTANCE` 在名称已被占用时失败关闭；
- 连接后校验客户端 PID 所属 Windows 会话和用户 SID；
- 句柄不可继承，管道名只允许受限 ASCII 字符。

这些措施能隔离其他本地用户和远程客户端，但不等同于启动器的密码学认证。同一用户、
同一会话中的其他进程如果得知管道名，仍可能连接。启动器因此应生成不可猜测的每次运行
名称，不传长期秘密，并继续把每个载荷字段视为不可信输入。

## 关闭与验证

正常进程退出由 Windows 回收进程生命周期内的框架。需要显式卸载 DLL 的宿主必须先在
普通线程调用导出的 `ShutdownPayloadCommandFramework()`，不能从 `DllMain` 或命令回调
调用。

CI 会在 Ubuntu 运行可移植协议测试，并在 Windows 运行真实命名管道集成测试。同一套
Windows 测试可在本机运行：

```powershell
cmake -S ProjectReboundMainDLL/Tests -B build/command-pipe-tests
cmake --build build/command-pipe-tests --config Release
ctest --test-dir build/command-pipe-tests -C Release --output-on-failure
```

修改协议时，必须在同一变更中更新协议实现、所有启动器消费者、本文两个语言版本和测试。
