# ProjectRebound 后端 API 规范 v1

> 生成日期：2026-04-20  
> 目标：提供可玩服务端所需的完整后端接口规范，并兼容当前客户端实现。

## 1. 总体约定

- 协议：HTTP/1.1（建议同时支持 HTTPS）
- 编码：UTF-8
- Content-Type：application/json
- 时间格式：ISO 8601 UTC（例如 `2026-04-20T08:30:00Z`）
- 推荐基础路径：`/v1`
- **兼容路径（必须保留）**：`/server/status`

### 1.1 认证建议

- 公共查询接口：可匿名
- 服务器写接口：`Authorization: Bearer <server_token>`（兼容阶段可先放开）
- 管理接口：`Authorization: Bearer <admin_token>`

---

## 2. 数据模型

## 2.1 ServerStatusPayload（与现有代码兼容）

```json
{
  "name": "CN-1",
  "region": "CN",
  "mode": "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Normal.BP_PBGameMode_Rush_PVE_Normal_C",
  "map": "Warehouse",
  "port": 7777,
  "playerCount": 3,
  "serverState": "RoundInProgress"
}
```

字段约束：

- `name`: string, 1..64
- `region`: string, 1..16（如 CN、SEA）
- `mode`: string, 1..256
- `map`: string, 1..64
- `port`: integer, 1..65535
- `playerCount`: integer, 0..128
- `serverState`: string, 1..64

服务端补充字段（后端写入）：

- `serverId`: string (UUID)
- `endpoint`: string (`ip:port`)
- `lastSeenAt`: datetime
- `status`: enum(`online`,`stale`,`offline`,`draining`)
- `ttlSeconds`: integer（建议 15）
- `version`: string（可选）

---

## 3. 必须实现（兼容层）

## 3.1 POST /server/status（必须）

用途：接收游戏服心跳/状态上报（当前客户端唯一会调用的后端接口）。

### Request

- Header: `Content-Type: application/json`
- Body: `ServerStatusPayload`

### Response

建议始终快速返回 200：

```json
{
  "ok": true,
  "serverId": "c4b0f3fb-73b9-4df9-b7f0-fce70dbcf98a",
  "nextHeartbeatSeconds": 5
}
```

### 处理规则

1. 按来源地址 `remote_ip + payload.port` 或显式 `serverId` 去重。
2. 更新状态并刷新 `lastSeenAt`。
3. 若 `now - lastSeenAt > ttlSeconds`，置为 `stale/offline`。
4. 幂等：同一实例重复上报不创建重复记录。

### 错误码

- 400：字段非法
- 413：请求体过大
- 429：超限
- 500：内部错误

---

## 4. 推荐完整接口（可玩服列表/匹配）

## 4.1 GET /v1/servers

用途：服务器列表（大厅/网页/启动器）。

Query 参数：

- `region` (optional)
- `map` (optional)
- `mode` (optional)
- `status` (optional, default `online`)
- `page` (default 1)
- `pageSize` (default 20, max 100)

Response 200：

```json
{
  "items": [
    {
      "serverId": "c4b0f3fb-73b9-4df9-b7f0-fce70dbcf98a",
      "name": "CN-1",
      "region": "CN",
      "mode": "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Normal.BP_PBGameMode_Rush_PVE_Normal_C",
      "map": "Warehouse",
      "endpoint": "1.2.3.4:7777",
      "port": 7777,
      "playerCount": 3,
      "serverState": "RoundInProgress",
      "status": "online",
      "lastSeenAt": "2026-04-20T08:30:00Z"
    }
  ],
  "page": 1,
  "pageSize": 20,
  "total": 1
}
```

## 4.2 GET /v1/servers/{serverId}

用途：单服详情。  
Response 200：单项服务器对象。

## 4.3 POST /v1/match/resolve

用途：把选中的服务器解析为客户端可直连地址（用于生成 `-match=ip:port`）。

Request：

```json
{
  "serverId": "c4b0f3fb-73b9-4df9-b7f0-fce70dbcf98a"
}
```

Response 200：

```json
{
  "connect": "1.2.3.4:7777",
  "expiresAt": "2026-04-20T08:35:00Z"
}
```

## 4.4 GET /v1/regions

用途：前端筛选项。  
Response：

```json
{
  "items": ["CN", "SEA", "EU", "NA"]
}
```

---

## 5. 服务器生命周期接口（推荐）

## 5.1 POST /v1/server/register（可选）

用途：服务器首次注册，返回 `serverId` + token。

## 5.2 POST /v1/server/status（推荐主端点）

与兼容端点同语义。新客户端建议走此路径。

## 5.3 POST /v1/server/shutdown（可选）

用途：优雅下线，立即标记 `offline`。

---

## 6. 管理接口（推荐）

## 6.1 GET /v1/admin/servers

用途：查看全部状态（含 stale/offline）。

## 6.2 POST /v1/admin/servers/{serverId}/force-offline

用途：手动摘除异常服。

---

## 7. 统一错误格式

所有 4xx/5xx 返回：

```json
{
  "error": {
    "code": "VALIDATION_ERROR",
    "message": "port must be between 1 and 65535",
    "details": [
      { "field": "port", "reason": "out_of_range" }
    ],
    "requestId": "8b9f2d24f23b4dfb"
  }
}
```

---

## 8. 状态机与超时策略

- 上报周期：约每 5 秒 1 次
- 建议阈值：
  - `online`: 0~15 秒内有心跳
  - `stale`: 15~45 秒无心跳
  - `offline`: >45 秒无心跳
- 清理策略：`offline` 超过 N 分钟可归档

---

## 9. 限流与安全建议

- `POST /server/status`: 每实例 2 req/s（突发 10）
- 公共查询：IP 级限流（如 60 req/min）
- 可选签名：`X-Signature: HMAC-SHA256(body, secret)`
- 日志脱敏：不记录 token/签名明文

---

## 10. 最小可玩闭环（结论）

最少需要：

1. `POST /server/status`（兼容路径）
2. `GET /v1/servers`（供玩家看到服务器）
3. 心跳 TTL 过期剔除逻辑

建议补充：

4. `POST /v1/match/resolve`（稳定输出连接地址）

---

## 11. 与当前客户端代码的兼容说明

当前客户端仅直接调用：

- `POST /server/status`

且上报触发条件是带 `-online=host:port` 参数运行。

建议后端第一阶段先保证兼容端点可用，再逐步引导到 `/v1/server/status`。
