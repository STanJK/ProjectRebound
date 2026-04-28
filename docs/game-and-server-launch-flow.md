# ProjectRebound 当前游戏与服务器启动流程

生成日期：2026-04-21

本文总结当前已经形成的 Windows GUI + Linux 骨干服务 + 玩家主机房间启动链路。目标是让 A 机创建主机房、A 自己进入战局、B 机通过房间浏览器加入，并在 P2P 打洞失败时自动使用最小 UDP relay 兜底。

## 0. 当前验证状态

截至 2026-04-21，以下链路已经实测成功：

- Debian 后端返回 `build = udp-relay-fallback-20260421`。
- `5001/udp` rendezvous 成功。
- `5002/udp` relay 成功，`NatPunchTest --relay` 出现：

```text
client relay registration accepted observed=...
PASS: received pong sequence=1 from 43.240.193.246:5002 ...
```

- A 机使用 Python GUI 创建房间并进入游戏。
- B 机使用 Python GUI 加入房间并进入游戏。
- 在 P2P 打洞不通时，client proxy 自动启用 relay fallback 后可加入游戏。

## 1. 组件职责

- Linux 骨干服务：`Backend/ProjectRebound.MatchServer`
  - 提供匿名登录、房间列表、join ticket、host heartbeat、NAT rendezvous、punch ticket、最小 UDP relay。
  - HTTP 入口通常由 Nginx 暴露为 `http://SERVER_IP/`。
  - Kestrel 本机监听 `127.0.0.1:5000`，不直接暴露公网。

- Windows 桌面浏览器 GUI：`Desktop/ProjectRebound.Browser.Python/project_rebound_browser.py`
  - 配置游戏目录、后端地址、地区、端口、版本。
  - 创建房间、刷新房间、加入房间。
  - 生成并启动 host/client bat。
  - 自动同步 `dxgi.dll`、`Payload.dll`、`ProjectReboundServerWrapper.exe` 到游戏 exe 目录。
  - 创建主机房后额外发 GUI host heartbeat，避免 Payload 心跳失败时房间被误判 `host_lost`。

- 本地 UDP 小代理：`Desktop/ProjectRebound.Browser.Python/project_rebound_udp_proxy.py`
  - host 模式：监听玩家公网端口，如 `7777`，转发到本机游戏服务端端口，如 `7778`。
  - client 模式：监听本机 client proxy 端口，如 `17777`，游戏客户端连接 `127.0.0.1:17777`。
  - 优先 P2P 打洞；client 8 秒内收不到 host punch 时自动切换到服务器 UDP relay。

- Fake login server：`BoundaryMetaServer-main/index.js`
  - 本机监听 `http://127.0.0.1:8000`。
  - 游戏客户端必须带 `-LogicServerURL=http://127.0.0.1:8000`。

- Server wrapper：`ProjectReboundServerWrapper.exe`
  - 启动游戏服务端进程。
  - 传入 `-online=`, `-roomid=`, `-hosttoken=`, `-map=`, `-mode=`, `-port=` 等参数。
  - 日志写入游戏目录下 `logs/log-*.txt`。

- Payload：`dxgi.dll` 加载 `Payload.dll`
  - 服务端模式：根据 wrapper 参数启动监听、上报 heartbeat。
  - 客户端模式：识别 `-match=ip:port`，进入游戏后执行连接。
  - `-debuglog` 会生成 `clientlogs/clientlog-*.txt`。

## 2. Linux 骨干服务要求

部署后先确认：

```bash
curl -fsS http://127.0.0.1:5000/health
curl -fsS http://YOUR_SERVER_IP/health
```

当前版本应包含：

```json
"build": "udp-relay-fallback-20260421"
```

需要开放端口：

```text
80/tcp    HTTP API, Nginx
5001/udp  NAT rendezvous
5002/udp  UDP relay fallback
```

Debian 防火墙：

```bash
sudo ufw allow 80/tcp
sudo ufw allow 5001/udp
sudo ufw allow 5002/udp
sudo ufw status
```

云厂商安全组也必须放行 `5001/udp` 和 `5002/udp`。

确认 UDP 服务监听：

```bash
sudo ss -lunp | grep 5001
sudo ss -lunp | grep 5002
sudo journalctl -u projectrebound-matchserver -f
```

期望日志：

```text
UDP rendezvous service listening on 0.0.0.0:5001
UDP relay service listening on 0.0.0.0:5002
```

GUI 和测试脚本建议使用 Nginx 入口：

```text
http://YOUR_SERVER_IP
```

不要填：

```text
http://YOUR_SERVER_IP:5000
```

除非你明确把 Kestrel `5000` 暴露到了公网。

## 3. Windows GUI 推荐配置

在 GUI 中：

```text
Backend       http://YOUR_SERVER_IP
Game Dir      D:\steam\steamapps\common\Boundary
Region        CN
Version       dev
Port          7777
Use UDP Proxy checked
Client Proxy  17777
Logic URL     http://127.0.0.1:8000
```

`Game Dir` 可以指向 Boundary 根目录，GUI 会递归查找：

```text
ProjectBoundarySteam-Win64-Shipping.exe
BoundaryMetaServer-main
node.exe
ProjectReboundServerWrapper.exe
```

创建/加入前，GUI 会把仓库构建产物同步到游戏 exe 目录：

```text
dxgi.dll
Payload.dll
ProjectReboundServerWrapper.exe
```

如果 wrapper 正在运行，复制可能失败。此时先关闭旧的 wrapper、游戏、host launcher、UDP proxy 窗口。

## 4. A 机创建房间流程

GUI 点击 `Create` 后，在 `Use UDP Proxy` 模式下执行：

1. 匿名登录：

```text
POST /v1/auth/guest
```

2. 创建 host NAT binding：

```text
POST /v1/nat/bindings
```

3. GUI 从同一个 UDP socket 向服务器 `5001/udp` 发送 rendezvous 包。

4. 确认 binding：

```text
POST /v1/nat/bindings/{bindingToken}/confirm
```

5. 创建房间：

```text
POST /v1/rooms
```

请求使用 `bindingToken`，不使用传统 `probeId`。

6. GUI 获取 `roomId` 和 `hostToken` 后启动 host 链路。

7. GUI 启动独立 heartbeat 线程：

```text
POST /v1/rooms/{roomId}/heartbeat
```

每 5 秒左右一次。GUI 日志应看到：

```text
Starting GUI host heartbeat for room ...
GUI host heartbeat is active for room ...
```

## 5. A 机 host bat 流程

GUI 生成：

```text
%APPDATA%\ProjectReboundBrowser\launchers\launch-host.bat
```

bat 的关键步骤：

1. `pushd` 到游戏 exe 目录。

2. 启动或复用 fake login server：

```bat
start /B /D "BoundaryMetaServer-main" ..\nodejs\node.exe index.js
```

等待 `http://127.0.0.1:8000` 可用。

3. 启动 host UDP proxy：

```text
project_rebound_udp_proxy.py host
```

参数含义：

```text
--backend      http://YOUR_SERVER_IP
--access-token  <playerToken>
--room-id      <roomId>
--host-token   <hostToken>
--public-port  7777
--game-port    7778
```

端口分工：

```text
7777  host UDP proxy 对外监听
7778  本机游戏服务端监听
```

4. 启动 server wrapper：

```text
ProjectReboundServerWrapper.exe
  -online=YOUR_SERVER_IP:80
  -roomid=<roomId>
  -hosttoken=<hostToken>
  -map=Warehouse
  -mode=pve
  -servername=ProjectRebound_Room
  -serverregion=CN
  -port=7778
  -gameexe=<Win64>\ProjectBoundarySteam-Win64-Shipping.exe
```

注意：UDP proxy 模式下 wrapper 必须使用 `-port=7778`。如果 wrapper 报：

```text
ERROR: UDP Port 7777 is already in use!
```

说明运行的是旧 wrapper，或没有正确吃到 `-port=7778`。

5. bat 不再固定等待 20 秒，而是扫描最新 wrapper 日志，直到出现：

```text
Server is now listening
Heartbeat received
[HEARTBEAT]
```

然后才启动 A 机自己的游戏客户端。

6. 启动 A 机游戏客户端：

```text
ProjectBoundarySteam-Win64-Shipping.exe
  -LogicServerURL=http://127.0.0.1:8000
  -match=127.0.0.1:7778
  -debuglog
```

A 自己连接本机游戏服务端 `127.0.0.1:7778`，不经过 host UDP proxy。

## 6. A 机成功日志检查点

Wrapper 日志：

```text
logs/log-*.txt
```

期望看到：

```text
[Launcher] Wrapper started.
[Launcher] Configured UDP port: 7778
[SERVER] Online backend: YOUR_SERVER_IP:80
[SERVER] Host room id: ...
[SERVER] Host token received.
[SERVER] Port: 7778
[SERVER] Server is now listening.
[Launcher] Heartbeat received
```

Host UDP proxy 日志：

```text
%APPDATA%\ProjectReboundBrowser\udp-proxy-host.log
```

期望看到：

```text
host proxy listening on UDP 7777, forwarding to 127.0.0.1:7778, relay=YOUR_SERVER_IP:5002
host proxy stats: peers=0 punch_rx=0 relay_registered=True ...
```

当 B 加入后，`peers` 应变为 `1`。如果 P2P 不通但 relay 生效，`relay_rx` 会增长。

A 客户端日志：

```text
clientlogs/clientlog-*.txt
```

期望看到：

```text
[CLIENT] Auto-connecting to match: 127.0.0.1:7778
```

## 7. B 机加入房间流程

GUI 点击 `Join` 后：

1. 预留房间位置：

```text
POST /v1/rooms/{roomId}/join
```

返回：

```text
connect     host公网endpoint
joinTicket  短期票据
```

2. 启动 client UDP proxy：

```text
project_rebound_udp_proxy.py client
```

参数含义：

```text
--backend       http://YOUR_SERVER_IP
--access-token  <playerToken>
--room-id       <roomId>
--join-ticket   <joinTicket>
--listen-port   17777
```

3. client proxy 创建 NAT binding：

```text
POST /v1/nat/bindings
```

4. client proxy 从同一个 UDP socket 向服务器 `5001/udp` 做 rendezvous。

5. client proxy 创建 punch ticket：

```text
POST /v1/rooms/{roomId}/punch-tickets
```

6. client proxy 分配 relay：

```text
POST /v1/relay/allocations
```

7. client proxy 同时尝试：

```text
P2P punch -> host endpoint
relay register -> 5002/udp
```

8. 如果 8 秒内没有收到 host punch：

```text
client proxy enabling UDP relay fallback; no peer punch was received in time
```

之后本地游戏 UDP 包同时/改走 relay。

9. B 机游戏客户端启动：

```text
ProjectBoundarySteam-Win64-Shipping.exe
  -LogicServerURL=http://127.0.0.1:8000
  -match=127.0.0.1:17777
  -debuglog
```

## 8. B 机成功日志检查点

Client UDP proxy 日志：

```text
%APPDATA%\ProjectReboundBrowser\udp-proxy-client.log
```

P2P 成功时：

```text
client proxy stats: host=<hostIp:port> punch_rx>0 relay_registered=True relay_active=False ...
```

P2P 失败、relay 兜底时：

```text
client proxy enabling UDP relay fallback; no peer punch was received in time
client proxy stats: ... relay_registered=True relay_active=True relay_rx>0 ...
```

B 客户端日志：

```text
clientlogs/clientlog-*.txt
```

期望看到：

```text
[CLIENT] Auto-connecting to match: 127.0.0.1:17777
```

## 9. NAT/Relay 单独测试

不用启动游戏，可以先测网络链路。

P2P 测试：

A 机：

```powershell
Tools\NatPunchTest\run-host.bat --backend http://YOUR_SERVER_IP --port 27777
```

B 机：

```powershell
Tools\NatPunchTest\run-client.bat --backend http://YOUR_SERVER_IP --room-id ROOM_ID_FROM_A --port 27778
```

Relay 测试：

A 机：

```powershell
Tools\NatPunchTest\run-host.bat --backend http://YOUR_SERVER_IP --port 27777 --relay
```

B 机：

```powershell
Tools\NatPunchTest\run-client.bat --backend http://YOUR_SERVER_IP --room-id ROOM_ID_FROM_A --port 27778 --relay
```

成功：

```text
PASS: received pong ...
```

如果不带 `--relay` 失败，但带 `--relay` 成功，说明 P2P 被 NAT/防火墙挡住，relay 可兜底。

## 10. 常见问题

### HTTP 502

如果测试脚本或 GUI 访问：

```text
http://YOUR_SERVER_IP:5000
```

出现 502，通常是访问了错误入口。按当前 Debian/Nginx 部署，应使用：

```text
http://YOUR_SERVER_IP
```

### UDP Port 7777 is already in use

在 UDP proxy 模式下，`7777` 被 host proxy 占用是正常的；wrapper 不应该再用 `7777`。

检查 wrapper 日志：

```text
Configured UDP port: 7778
```

如果仍然报 `UDP Port 7777 is already in use`：

1. 确认 GUI 勾选了 `Use UDP Proxy`。
2. 确认运行的是新版 `ProjectReboundServerWrapper.exe`。
3. 关闭旧 wrapper/host proxy/游戏窗口后重试。

查占用：

```powershell
netstat -ano -p udp | findstr :7777
taskkill /PID <PID> /F
```

### A 自己进靶场后断线

先看 wrapper 是否真的 ready：

```text
[SERVER] Server is now listening.
```

再看 A 客户端是否连接：

```text
[CLIENT] Auto-connecting to match: 127.0.0.1:7778
```

如果客户端启动早于服务端监听，使用当前新版 GUI 重新生成 host bat；新版 bat 会等待 wrapper 日志 readiness。

### B 进入靶场后断线

按顺序看：

1. B 的 `udp-proxy-client.log`
2. A 的 `udp-proxy-host.log`
3. 服务器 `journalctl`

关键字段：

```text
punch_rx
relay_registered
relay_active
relay_rx
game_from_local
game_from_host
game_from_peer
game_to_peer
```

如果 `relay_registered=False`，检查服务器 `5002/udp`。

如果 `relay_registered=True` 且 `relay_rx=0`，检查云安全组、防火墙或 relay 注册是否被 NAT 改端口后丢失。

## 11. 当前约束

- 主机掉线则房间结束，不做主机迁移。
- Relay 是最小 UDP datagram 转发，不解析游戏协议，不做带宽控制。
- Relay 兜底会消耗服务器上下行带宽，只应在 P2P 失败时使用。
- 快速匹配的 UDP proxy/relay 链路仍是原型，主要手动创建/加入房间验证。
- `joinTicket` 当前只用于后端占位、relay/punch 授权和审计，不在游戏内强校验。
