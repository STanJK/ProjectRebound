# ProjectRebound v2 Implementation Status

更新时间：2026-04-21

## 已完成

- 后端项目：`Backend/ProjectRebound.MatchServer`
  - `.NET 8` Minimal API。
  - EF Core SQLite 持久化。
  - 匿名登录：`POST /v1/auth/guest`。
  - UDP host probe：`POST /v1/host-probes` 和 confirm。
  - 房间创建、列表、详情、加入、离开、心跳、start、end。
  - 快速匹配 ticket：创建、查询、取消。
  - 后台 matchmaking loop。
  - 后台 lifecycle cleanup。
  - 旧 `/server/status` 兼容层。
  - 保留 host migration 接口，返回 `501 NOT_IMPLEMENTED`。
  - UDP NAT rendezvous：`5001/udp`。
  - Punch ticket：host/client 交换公网 UDP endpoint。
  - 最小 UDP relay：`POST /v1/relay/allocations` + `5002/udp`，P2P 失败时兜底转发 UDP datagram。
  - SQLite `DateTimeOffset` 已转成 Unix milliseconds 存储，避免 SQLite provider 无法翻译 `ExpiresAt > now` 查询。

- 共享契约：`Shared/ProjectRebound.Contracts`
  - DTO 和 enum 已集中定义。

- Python GUI 原型：`Desktop/ProjectRebound.Browser.Python`
  - 标准库 `tkinter + urllib`，无需额外 pip 依赖。
  - 配置保存、匿名登录、刷新房间、创建房间、加入房间、快速匹配。
  - 复用现有 `-match=ip:port` 客户端直连。
  - 创建主机优先启动 `ProjectReboundServerWrapper.exe`。
  - `Use UDP Proxy` 模式已接入本地 host/client UDP proxy。
  - 创建主机时自动同步 `dxgi.dll`、`Payload.dll`、`ProjectReboundServerWrapper.exe` 到游戏 exe 目录。
  - 创建主机后 GUI 发送房间 heartbeat，防止 host 心跳链路未就绪时被后端误判 `host_lost`。
  - A/B 两机 GUI 创建/加入游戏链路已验证成功。

- C++ 接线
  - `ServerWrapper` 支持 `-online=`, `-roomid=`, `-hosttoken=`, `-map=`, `-mode=`, `-difficulty=`, `-servername=`, `-serverregion=`, `-port=`。
  - `ServerWrapper` 支持 `-gameexe=`，并在日志中输出 `Configured UDP port`。
  - `Payload` 支持 `-roomid=` 和 `-hosttoken=`。
  - `Payload` 有房间凭证时向 `/v1/rooms/{roomId}/heartbeat` 上报；否则继续 `/server/status`。
  - 客户端 `Payload` 支持 `-match=ip:port` 自动连接，并可用 `-debuglog` 生成 clientlog。

- 文档
  - `docs/backend-api-spec-v2.md`
  - `docs/debian-deployment-and-ops.md`
  - `docs/game-and-server-launch-flow.md`
  - `Tools/NatPunchTest/README.md`
  - `Desktop/ProjectRebound.Browser.Python/README.md`
  - 本状态文件。

## 已验证链路

- Debian 后端部署：`/health` 返回 `build = udp-relay-fallback-20260421`。
- `5001/udp` rendezvous 已验证。
- `5002/udp` relay 已验证：

```text
client relay registration accepted observed=...
PASS: received pong sequence=1 from 43.240.193.246:5002 ...
```

- 真实 A/B GUI 联机已验证：
  - A 机通过 Python GUI 创建玩家主机房。
  - A 机启动 fake login server、host UDP proxy、server wrapper、游戏客户端。
  - B 机通过 Python GUI 加入房间。
  - 在 P2P 不通时，client proxy 自动使用 relay fallback，成功进入游戏。

## 当前主线

- 当前桌面浏览器主线是 Python 原型。
- WPF 项目仍在 `Desktop/ProjectRebound.Browser`，但不是当前推荐运行入口。

## 待验证 / 未完成

- 快速匹配与 UDP proxy/relay 的完整联动仍未作为主路径验收。
- 未做主机迁移。
- 未做账号系统。
- 未做游戏内 `joinTicket` 校验。
- 未做数据库迁移；如果 schema 变化，当前建议删除旧 SQLite DB 后重建。
- 未做自动化 API 集成测试项目。

## 验证命令

最近一次检查结果：

- `dotnet build Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj --no-restore`：通过，0 warnings / 0 errors。
- `python -m py_compile Desktop\ProjectRebound.Browser.Python\project_rebound_browser.py Desktop\ProjectRebound.Browser.Python\project_rebound_udp_proxy.py Tools\NatPunchTest\nat_punch_test.py`：通过。
- `MSBuild Payload\Payload.vcxproj /p:Configuration=Release /p:Platform=x64 /m`：通过，0 warnings / 0 errors。
- `MSBuild ServerWrapper\ProjectReboundServerWrapper.slnx /p:Configuration=Release /p:Platform=x64 /m`：通过，0 warnings / 0 errors。
- 后端 smoke：临时 SQLite + 临时端口启动成功，`/health` 和 `POST /v1/auth/guest` 通过；后台 matchmaking 未再出现 `could not be translated` / `Matchmaking tick failed`。
- `Tools\NatPunchTest\run-host.bat ... --relay` + `run-client.bat ... --relay`：通过，relay ping/pong 成功。

后端构建：

```powershell
dotnet build Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj --no-restore
```

Python GUI 语法检查：

```powershell
python -m py_compile Desktop\ProjectRebound.Browser.Python\project_rebound_browser.py
```

Payload 构建：

```powershell
& 'G:\FXXKING SOFTWARES\VSC\MSBuild\Current\Bin\MSBuild.exe' Payload\Payload.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

ServerWrapper 构建：

```powershell
& 'G:\FXXKING SOFTWARES\VSC\MSBuild\Current\Bin\MSBuild.exe' ServerWrapper\ProjectReboundServerWrapper\ProjectReboundServerWrapper.slnx /p:Configuration=Release /p:Platform=x64 /m
```
