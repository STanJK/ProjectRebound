# ProjectRebound Backend API Spec v2

生成日期：2026-04-20

目标：在保留 v1 专服心跳兼容层的同时，提供“玩家主机房间/匹配”骨干服务。V2 只负责匿名身份、房间、匹配、UDP 可达性探测和连接信息分发；每一局仍由某个玩家机器作为主机承载游戏。

当前实现：

- 后端：`.NET 8 / ASP.NET Core Minimal API / EF Core SQLite`
- 桌面浏览器 GUI：`Python 3.11 + tkinter` 快速原型，路径为 `Desktop/ProjectRebound.Browser.Python`
- 保留的 WPF 原型：`Desktop/ProjectRebound.Browser`，不再作为当前主线
- 已验证：A/B 两机通过 Python GUI 创建/加入游戏成功；P2P 不通时最小 UDP relay 可兜底。

## 1. 约定

- 协议：HTTP/1.1 JSON；公网部署建议放在 HTTPS 反代后。
- 基础路径：`/v1`
- 时间：API 使用 ISO 8601 UTC；SQLite 内部把 `DateTimeOffset` 转成 Unix milliseconds 存储，避免 SQLite provider 无法翻译时间比较查询。
- 认证：除 `GET /v1/rooms`、`GET /v1/rooms/{roomId}`、`GET /v1/servers` 和旧 `/server/status` 外，写操作使用 `Authorization: Bearer <player_token>`。
- 网络模型：V1 优先公网直连 / UDP 打洞；主机掉线则本局结束；主机迁移仍保留为未来接口。
- 实验性 UDP Proxy：当前原型增加了本地 UDP 小代理路径。服务器提供 UDP rendezvous、punch ticket 信令，并在 P2P 失败时提供最小 UDP relay 兜底；GUI 勾选 `Use UDP Proxy` 后启用。

## 2. 最小闭环

1. GUI 调 `POST /v1/auth/guest` 获取匿名玩家 token。
2. 创建房间或快速匹配前，GUI 临时监听本机 UDP 端口，并调 `POST /v1/host-probes`。
3. 后端向请求来源公网 IP 的指定 UDP 端口发送 `nonce`。
4. GUI 收到 UDP `nonce` 后调 `POST /v1/host-probes/{probeId}/confirm`。
5. GUI 调 `POST /v1/rooms` 创建房间，获得 `roomId` 和 `hostToken`。
6. GUI 先确保本地 fake login server 可达，再启动 host：`ProjectReboundServerWrapper.exe -online=<backend_host:port> -roomid=<roomId> -hosttoken=<hostToken> ...`
7. Payload 优先发 `/v1/rooms/{roomId}/heartbeat`；旧 `/server/status` 带 `roomId/hostToken` 时也映射到房间心跳。
8. 浏览器用 `GET /v1/rooms` 展示房间。
9. 玩家加入时调 `POST /v1/rooms/{roomId}/join`，拿到 `connect: "ip:port"` 后，GUI 会先按可用批处理流程启动本地 fake login server：`BoundaryMetaServer-main/index.js`，等待 `http://127.0.0.1:8000` 可达，再启动客户端：`ProjectBoundarySteam-Win64-Shipping.exe -LogicServerURL=http://127.0.0.1:8000 -match=ip:port -debuglog`。
10. `Use UDP Proxy` 模式下，客户端实际连接本地 proxy：`-match=127.0.0.1:<Client Proxy>`；proxy 优先 P2P，失败后使用 `5002/udp` relay。

## 3. 数据模型

- `Player`: `playerId`, `displayName`, `deviceTokenHash`, `createdAt`, `lastSeenAt`, `status`
- `HostProbe`: `probeId`, `playerId`, `publicIp`, `port`, `nonce`, `status`, `expiresAt`
- `Room`: `roomId`, `hostPlayerId`, `hostProbeId`, `hostTokenHash`, `name`, `region`, `map`, `mode`, `version`, `endpoint`, `port`, `maxPlayers`, `playerCount`, `serverState`, `state`, `createdAt`, `lastSeenAt`, `endedReason`
- `RoomPlayer`: `roomPlayerId`, `roomId`, `playerId`, `joinTicketHash`, `status`, `joinedAt`, `expiresAt`
- `MatchTicket`: `ticketId`, `playerId`, `region`, `map`, `mode`, `version`, `canHost`, `probeId`, `state`, `assignedRoomId`, `hostTokenPlain`, `joinTicketPlain`, `failureReason`, `expiresAt`
- `LegacyServer`: legacy `/server/status` 兼容记录

## 4. API

### POST /v1/nat/bindings

创建一次 UDP rendezvous 绑定。客户端随后必须从同一个 UDP socket 向后端 UDP rendezvous 端口发送包含 `bindingToken` 的 JSON 包，后端据此记录观察到的公网 UDP endpoint。

Request:

```json
{ "localPort": 7777, "role": "host", "roomId": null }
```

Response:

```json
{
  "bindingToken": "token",
  "udpHost": "match.example.com",
  "udpPort": 5001,
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

### POST /v1/nat/bindings/{bindingToken}/confirm

确认后端已经观察到 UDP 包，返回公网 UDP endpoint。

```json
{
  "bindingToken": "token",
  "publicIp": "1.2.3.4",
  "publicPort": 53000,
  "localPort": 7777,
  "role": "host",
  "roomId": null,
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

### POST /v1/rooms/{roomId}/punch-tickets

加入者拿到 `joinTicket` 后创建 punch ticket。后端把 client NAT binding 和 room host endpoint 组成一次打洞会话。

Request:

```json
{
  "joinTicket": "join-token",
  "bindingToken": "client-nat-binding-token",
  "clientLocalEndpoint": "127.0.0.1:17777"
}
```

Response:

```json
{
  "ticketId": "uuid",
  "state": "Pending",
  "nonce": "token",
  "hostEndpoint": "1.2.3.4:7777",
  "hostLocalEndpoint": null,
  "clientEndpoint": "5.6.7.8:53000",
  "clientLocalEndpoint": "127.0.0.1:17777",
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

### GET /v1/rooms/{roomId}/punch-tickets?hostToken=...

主机 proxy 轮询待打洞的 client endpoint。

### POST /v1/rooms/{roomId}/punch-tickets/{ticketId}/complete

预留完成上报接口。当前 proxy 原型主要靠持续 punch 和 UDP 转发工作，完成上报不作为连接前置条件。

### POST /v1/relay/allocations

为本地 UDP proxy 分配最小 relay 凭据。host 侧使用 `hostToken`；client 侧必须先 `join` 房间并使用新鲜 `joinTicket`。随后 proxy 从同一个 UDP socket 向 `relayHost:relayPort` 发送注册包：

```json
{
  "type": "PRB_RELAY_REGISTER_V1",
  "sessionId": "uuid",
  "role": "host",
  "secret": "secret"
}
```

Request:

```json
{
  "roomId": "uuid",
  "role": "client",
  "hostToken": null,
  "joinTicket": "join-token"
}
```

Response:

```json
{
  "sessionId": "uuid",
  "role": "client",
  "secret": "secret",
  "relayHost": "match.example.com",
  "relayPort": 5002,
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

Relay 只转发 UDP datagram：host 发来的包转发给同房间已注册 client；client 发来的包转发给已注册 host。Relay 不解析游戏协议，也不负责主机迁移。

### POST /v1/auth/guest

匿名登录或续期设备 token。

Request:

```json
{ "displayName": "Player", "deviceToken": null }
```

Response:

```json
{ "playerId": "uuid", "displayName": "Player", "accessToken": "token" }
```

### POST /v1/host-probes

创建 UDP 可达性探测。后端会向请求来源公网 IP 的指定 UDP 端口发送 `nonce`。

Request:

```json
{ "port": 7777 }
```

Response:

```json
{
  "probeId": "uuid",
  "publicIp": "1.2.3.4",
  "port": 7777,
  "nonce": "token",
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

### POST /v1/host-probes/{probeId}/confirm

GUI 收到 UDP `nonce` 后确认。只有成功且未过期的 probe 可以创建房间或成为快速匹配主机。

```json
{ "nonce": "received-nonce" }
```

### POST /v1/rooms

创建玩家主机房间。

```json
{
  "probeId": "uuid",
  "name": "CN Room",
  "region": "CN",
  "map": "Warehouse",
  "mode": "pve",
  "version": "dev",
  "maxPlayers": 8
}
```

Response:

```json
{ "roomId": "uuid", "hostToken": "secret", "heartbeatSeconds": 5 }
```

### GET /v1/rooms

房间列表。支持 `region`, `map`, `mode`, `version`, `state`, `page`, `pageSize`。

默认只返回 `Open` 和 `Starting` 房间。

### GET /v1/rooms/{roomId}

返回单个房间详情。

### POST /v1/rooms/{roomId}/join

预留加入位置，返回现有客户端可直接使用的地址。

```json
{ "version": "dev" }
```

Response:

```json
{ "connect": "1.2.3.4:7777", "joinTicket": "token", "expiresAt": "2026-04-20T08:35:00Z" }
```

### POST /v1/rooms/{roomId}/leave

释放加入占位。

```json
{ "joinTicket": "token" }
```

### POST /v1/rooms/{roomId}/heartbeat

主机心跳。

```json
{ "hostToken": "secret", "playerCount": 2, "serverState": "RoundInProgress" }
```

### POST /v1/rooms/{roomId}/start

用 `hostToken` 把房间标记为 `InGame`。

### POST /v1/rooms/{roomId}/end

用 `hostToken` 把房间标记为 `Ended`，`endedReason = "host_ended"`。

### POST /v1/matchmaking/tickets

快速匹配排队。V1 优先匹配已有 `Open` 房间；没有房间时，从 `canHost=true` 且 probe 成功的 ticket 中选主机。

```json
{
  "region": "CN",
  "map": "Warehouse",
  "mode": "pve",
  "version": "dev",
  "canHost": true,
  "probeId": "uuid",
  "roomName": "Quick Match",
  "maxPlayers": 8
}
```

### GET /v1/matchmaking/tickets/{ticketId}

轮询匹配状态：`Waiting`, `HostAssigned`, `Matched`, `Failed`, `Canceled`, `Expired`。

### DELETE /v1/matchmaking/tickets/{ticketId}

取消仍在等待中的匹配 ticket。

### POST /server/status

兼容 v1：

- 无 `roomId/hostToken`：记录 legacy server 状态。
- 有 `roomId/hostToken`：映射到对应房间心跳。

### 保留接口

- `POST /v1/rooms/{roomId}/host-migration/*`：V1 返回 `501 NOT_IMPLEMENTED`。
- `/v1/relay/allocations` 已实现最小 UDP relay 分配；复杂 TURN/多区域 relay 仍保留给后续版本。

## 5. 生命周期

- 心跳建议：5 秒。
- 15 秒无心跳：视为 stale；当前实现不单独暴露 stale 状态。
- 45 秒无心跳：房间进入 `Ended`，`endedReason = "host_lost"`。
- 主机掉线不迁移；后续版本可在保留接口上扩展。
- 后台任务会清理过期 probe、join ticket、match ticket 和超过保留时间的 ended/expired 房间。

## 6. Python GUI 原型

路径：`Desktop/ProjectRebound.Browser.Python`

运行：

```powershell
python Desktop\ProjectRebound.Browser.Python\project_rebound_browser.py
```

或双击：

```text
Desktop\ProjectRebound.Browser.Python\run_browser.bat
```

配置保存到：

```text
%APPDATA%\ProjectReboundBrowser\config-python.json
```

支持：

- 设置后端地址、游戏目录、显示名、地区、版本、端口、地图、模式、人数。
- 登录匿名设备 token。
- 刷新房间列表。
- UDP host probe。
- 创建房间并启动 `ProjectReboundServerWrapper.exe`。
- 加入房间并启动 `ProjectBoundarySteam-Win64-Shipping.exe -match=<ip:port>`。
- 快速匹配：成为主机则启动 wrapper，匹配到房间则启动客户端。

## 7. 运行顺序

1. 启动后端：

```powershell
dotnet run --project Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj
```

2. 启动 Python GUI：

```powershell
python Desktop\ProjectRebound.Browser.Python\project_rebound_browser.py
```

3. 在 GUI 中配置游戏目录。
4. 先点 `Refresh` 验证后端连接。
5. 再测试 `Create` 或 `Quick Match`。

## 8. 注意事项

- 如果已有旧版 `projectrebound-matchserver.db` 是在 DateTimeOffset 转换修复前创建的，建议删除旧 DB 后重新启动后端，因为当前实现使用 `EnsureCreated`，没有迁移脚本。
- `Create` 和 `Quick Match` 需要后端能访问 GUI 所在机器的 UDP 端口；本机测试时后端和 GUI 在同一机器通常可通过，公网部署需要端口转发/防火墙允许。
- `joinTicket` 当前只用于后端占位和审计，暂不做游戏内强校验。
