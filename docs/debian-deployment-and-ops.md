# ProjectRebound Debian Deployment and Operations

生成日期：2026-04-20

本文档描述如何把 `ProjectRebound.MatchServer` 骨干服务部署到 Debian 服务器，以及后续如何日常更新、回滚、备份和做真实联机验证。

## 1. 范围

部署对象：

- 后端：`Backend/ProjectRebound.MatchServer`
- 运行时：.NET 8 / ASP.NET Core / EF Core SQLite
- 反向代理：Nginx
- 进程守护：systemd
- 数据库：SQLite，放在 `/var/lib/projectrebound/projectrebound-matchserver.db`

不部署到 Linux 的部分：

- Windows Python GUI：`Desktop/ProjectRebound.Browser.Python`
- 游戏本体、server wrapper、payload

V1 网络模型优先使用玩家主机公网直连 / UDP 打洞。Linux 骨干服务负责身份、房间列表、UDP probe、匹配和心跳；当 P2P 打洞失败时，可启用最小 UDP relay 兜底转发游戏 UDP 包。

## 2. 前置假设

- Debian 12 或 Debian 13 x64 VPS。
- 服务器有 sudo 权限。
- 对外开放 TCP `80`；以后启用 HTTPS 时开放 TCP `443`。
- 如果启用 GUI 的 `Use UDP Proxy`，还需要对外开放 UDP `5001`，供 NAT rendezvous 使用。
- 如果启用最小 UDP relay 兜底，还需要对外开放 UDP `5002`。该端口会在 P2P 失败时转发游戏 UDP 包，服务器带宽会随玩家流量增长。
- 玩家当主机时，玩家机器需要开放并转发游戏 UDP 端口，例如 `7777/udp`。
- 当前 C++ `Payload` 更适合 HTTP `host:port` 上报。正式启用 HTTPS 前，建议先补 WinHTTP TLS 支持，或让游戏侧继续使用 `http://host:80`。

## 3. 服务器目录布局

```text
/opt/projectrebound/
  current -> /opt/projectrebound/releases/20260420-193000
  previous -> /opt/projectrebound/releases/20260420-181500
  releases/
    20260420-181500/
    20260420-193000/

/var/lib/projectrebound/
  projectrebound-matchserver.db
  projectrebound-matchserver.db-shm
  projectrebound-matchserver.db-wal

/var/backups/projectrebound/
  projectrebound-matchserver-20260420-193000.db
```

应用发布物放 `/opt/projectrebound/releases/<timestamp>`，`current` 指向当前版本。SQLite 数据库不放在发布目录里，这样更新应用不会误删数据。

## 4. 安装系统依赖

先安装常用工具：

```bash
sudo apt-get update
sudo apt-get install -y curl wget gpg unzip sqlite3 nginx ufw
```

添加 Microsoft 包源并安装 ASP.NET Core Runtime 8.0：

```bash
. /etc/os-release
wget "https://packages.microsoft.com/config/debian/${VERSION_ID}/packages-microsoft-prod.deb" -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
rm packages-microsoft-prod.deb

sudo apt-get update
sudo apt-get install -y aspnetcore-runtime-8.0
```

确认运行时：

```bash
dotnet --list-runtimes
```

如果出现 `E: Unable to locate package aspnetcore-runtime-8.0`，先确认系统版本和架构：

```bash
. /etc/os-release
echo "$ID $VERSION_ID"
dpkg --print-architecture
apt-cache policy | grep -i microsoft -A2
apt-cache search aspnetcore-runtime
```

常见原因：

- 没有成功安装 Microsoft Debian 包源。
- 安装包源后没有重新 `apt-get update`。
- 服务器不是 `amd64`。Microsoft Debian 包源中的 .NET 8 / .NET 9 只发布 x64 包；ARM64 机器不要用 APT 安装 .NET 8 runtime，改用 self-contained 发布。
- Debian 版本不是 12/13，`packages.microsoft.com/config/debian/${VERSION_ID}` 没有对应包源。

Debian 12/13 的包源修复命令：

```bash
. /etc/os-release
sudo apt-get update
sudo apt-get install -y gpg wget ca-certificates

rm -f packages-microsoft-prod.deb microsoft.asc microsoft.asc.gpg prod.list
wget "https://packages.microsoft.com/config/debian/${VERSION_ID}/packages-microsoft-prod.deb" -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
rm packages-microsoft-prod.deb

sudo apt-get update
apt-cache search aspnetcore-runtime
sudo apt-get install -y aspnetcore-runtime-8.0
```

如果 `dpkg --print-architecture` 不是 `amd64`，推荐在开发机发布 self-contained 版本：

```powershell
dotnet publish Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true -o publish\matchserver-linux-x64
```

对应 systemd 的 `ExecStart` 改为：

```ini
ExecStart=/opt/projectrebound/current/ProjectRebound.MatchServer --urls http://127.0.0.1:5000
```

上传后别忘了赋予执行权限：

```bash
sudo chmod +x /opt/projectrebound/current/ProjectRebound.MatchServer
```

## 5. 创建运行用户

```bash
sudo useradd --system --home /var/lib/projectrebound --create-home --shell /usr/sbin/nologin projectrebound
sudo mkdir -p /opt/projectrebound/releases /var/lib/projectrebound /var/backups/projectrebound
sudo chown -R projectrebound:projectrebound /opt/projectrebound /var/lib/projectrebound /var/backups/projectrebound
```

## 6. 本机发布

在 Windows 开发机的仓库根目录运行：

```powershell
dotnet publish Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj -c Release -o publish\matchserver
```

上传到服务器：

```powershell
scp -r publish\matchserver user@YOUR_SERVER:/tmp/projectrebound-matchserver
```

如果想生成不依赖服务器 .NET Runtime 的版本：

```powershell
dotnet publish Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true -o publish\matchserver-linux-x64
```

常规部署建议先使用 framework-dependent 发布，因为文件更小，更新更快。

## 7. 首次部署发布物

在 Debian 服务器上：

```bash
RELEASE="$(date +%Y%m%d-%H%M%S)"
sudo mkdir -p "/opt/projectrebound/releases/${RELEASE}"
sudo cp -a /tmp/projectrebound-matchserver/. "/opt/projectrebound/releases/${RELEASE}/"
sudo chown -R projectrebound:projectrebound "/opt/projectrebound/releases/${RELEASE}"
sudo ln -sfn "/opt/projectrebound/releases/${RELEASE}" /opt/projectrebound/current
```

## 8. systemd 服务

创建服务文件：

```bash
sudo nano /etc/systemd/system/projectrebound-matchserver.service
```

内容：

```ini
[Unit]
Description=ProjectRebound Match Server
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/opt/projectrebound/current
ExecStart=/usr/bin/dotnet /opt/projectrebound/current/ProjectRebound.MatchServer.dll --urls http://127.0.0.1:5000
Restart=always
RestartSec=5
KillSignal=SIGINT
SyslogIdentifier=projectrebound-matchserver
User=projectrebound
Environment=ASPNETCORE_ENVIRONMENT=Production
Environment="ConnectionStrings__MatchServer=Data Source=/var/lib/projectrebound/projectrebound-matchserver.db"

[Install]
WantedBy=multi-user.target
```

启动：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now projectrebound-matchserver
sudo systemctl status projectrebound-matchserver
```

看实时日志：

```bash
sudo journalctl -u projectrebound-matchserver -f
```

本机健康检查：

```bash
curl -fsS http://127.0.0.1:5000/health
```

注意：`ConnectionStrings__MatchServer` 的值里有 `Data Source` 空格，systemd service 文件里必须给整个环境变量赋值加双引号。否则应用可能只读到 `Data`，启动时报：

```text
Format of the initialization string does not conform to specification starting at index 0.
```

## 9. Nginx 反向代理

创建站点：

```bash
sudo nano /etc/nginx/sites-available/projectrebound
```

内容：

```nginx
server {
    listen 80;
    server_name YOUR_DOMAIN_OR_SERVER_IP;

    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

启用：

```bash
sudo ln -s /etc/nginx/sites-available/projectrebound /etc/nginx/sites-enabled/projectrebound
sudo nginx -t
sudo systemctl reload nginx
```

外部健康检查：

```bash
curl -fsS http://YOUR_DOMAIN_OR_SERVER_IP/health
```

`X-Forwarded-For` 很重要。后端创建 host probe 时会根据请求来源 IP 推断玩家公网 IP，Nginx 必须把真实来源 IP 传给后端。

## 10. 防火墙

```bash
sudo ufw allow OpenSSH
sudo ufw allow 80/tcp
sudo ufw allow 5001/udp
sudo ufw allow 5002/udp
sudo ufw enable
sudo ufw status
```

以后启用 HTTPS：

```bash
sudo ufw allow 443/tcp
```

不要把 Kestrel 的 `5000` 端口暴露到公网。systemd 已经让后端只监听 `127.0.0.1:5000`。

如果不使用 UDP Proxy，可以不开放 UDP `5001` 和 `5002`。如果使用 UDP Proxy，Nginx 不处理 UDP `5001/5002`，这两个 UDP 端口由 `ProjectRebound.MatchServer` 的后台 service 直接监听。`5001` 用于 rendezvous；`5002` 用于 P2P 失败后的最小 relay 兜底。

## 11. 冒烟测试

健康检查：

```bash
curl -fsS http://YOUR_DOMAIN_OR_SERVER_IP/health
```

匿名登录：

```bash
curl -sS -X POST http://YOUR_DOMAIN_OR_SERVER_IP/v1/auth/guest \
  -H "Content-Type: application/json" \
  -d '{"displayName":"Smoke","deviceToken":null}'
```

房间列表：

```bash
curl -sS "http://YOUR_DOMAIN_OR_SERVER_IP/v1/rooms?region=CN&version=dev"
```

旧心跳兼容路径：

```bash
curl -sS -X POST http://YOUR_DOMAIN_OR_SERVER_IP/server/status \
  -H "Content-Type: application/json" \
  -d '{"name":"legacy-smoke","endpoint":"127.0.0.1:7777","map":"test","mode":"test","version":"dev","playerCount":0,"maxPlayers":4}'
```

如果这些都返回 HTTP 200，说明 Nginx、Kestrel、SQLite 初始化和基本 API 都通了。

## 12. 真实联机验证

当前已验证结果（2026-04-21）：

- A 机 Python GUI 创建玩家主机房并进入游戏成功。
- B 机 Python GUI 加入 A 机房间并进入游戏成功。
- 实测环境中 P2P 打洞可能不通，但 UDP `5002` relay 兜底可成功。
- `Tools\NatPunchTest --relay` 已验证 relay ping/pong：

```text
client relay registration accepted observed=...
PASS: received pong sequence=1 from 43.240.193.246:5002 ...
```

准备两台 Windows 机器，最好不在同一局域网：

- A：玩家主机。
- B：玩家客户端。
- 两台都运行 `Desktop/ProjectRebound.Browser.Python/run_browser.bat`。
- Backend URL 都填 `http://YOUR_DOMAIN_OR_SERVER_IP`。
- Region、Version 保持一致。

不要在 GUI 里填 `http://YOUR_DOMAIN_OR_SERVER_IP:5000`。按本文档部署时，公网入口是 Nginx 的 `80` 端口；Kestrel 的 `5000` 端口只监听服务器本机 `127.0.0.1`。

A 机器：

1. Windows 防火墙允许游戏 UDP 端口，例如 `7777/udp`。
2. 如果 A 在路由器后面，转发 `UDP 7777` 到 A 的局域网 IP。
3. GUI 点创建房间。
4. 期望：UDP probe 成功，房间创建成功，服务端房间列表出现该房间。

实验性 UDP Proxy 模式：

1. 两台 GUI 都勾选 `Use UDP Proxy`。
2. A 的 `Port` 仍填公网代理端口，例如 `7777`。
3. A 创建房间时，GUI 会先向服务器 UDP `5001` 做 rendezvous，然后启动本地 `project_rebound_udp_proxy.py host`。
4. A 的游戏服务端会被启动在 `Port + 1`，例如 `7778`；公网 `7777` 由 proxy 监听。
5. B 加入房间时，GUI 会启动本地 `project_rebound_udp_proxy.py client`，并把游戏客户端启动为 `-LogicServerURL=http://127.0.0.1:8000 -match=127.0.0.1:<Client Proxy>`。
6. 服务器先交换双方公网 UDP endpoint 和 punch ticket，双方优先尝试 P2P 打洞。
7. 如果 client proxy 在短时间内没有收到 host punch 包，会自动切到服务器 UDP `5002` relay 兜底。
8. 对于端口受限 NAT，client proxy 收到 host punch 包后会把后续游戏包发往实际收到的 host endpoint，而不是只依赖 rendezvous 阶段观察到的 endpoint。

当前 UDP Proxy 是快速原型，主要验证受限 NAT 下的直连打洞链路。对于 symmetric NAT / 严格 CGNAT，P2P 可能失败，此时会使用最小 UDP relay 兜底；relay 会消耗服务器上下行带宽。

B 机器：

1. GUI 刷新房间列表。
2. 选择 A 的房间并加入。
3. 期望：GUI 调 `/v1/rooms/{roomId}/join`，拿到 `connect = ip:port`，启动游戏并传入 `-match=ip:port`。

快速匹配：

1. A 点快速匹配并允许当主机。
2. B 点快速匹配。
3. 期望：B 被分配到 A 的房间，或者后端选择可当主机的 ticket 创建房间。

主机掉线：

1. A 创建房间后关闭 wrapper 或游戏进程。
2. 约 45 秒内房间应变为 ended。
3. B 刷新后不应再能加入该房间。
4. 服务端日志不应出现连续异常。

查看服务端日志：

```bash
sudo journalctl -u projectrebound-matchserver -f
```

如果 A 使用直连 probe 模式创建房间时卡在 UDP probe，优先检查 A 的公网 UDP 可达性和端口转发。当前推荐的 `Use UDP Proxy` 模式已经提供 NAT rendezvous，并可在 P2P 失败时用 UDP `5002` relay 兜底。

## 13. 日常应用更新

开发机发布并上传：

```powershell
dotnet publish Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj -c Release -o publish\matchserver
# 注意：IP 后面必须有冒号 : 否则会拷贝到本地文件夹
scp -r publish\matchserver user@YOUR_SERVER:/tmp/projectrebound-matchserver-next
```

服务器切换版本：

```bash
set -e

RELEASE="$(date +%Y%m%d-%H%M%S)"
CURRENT="$(readlink -f /opt/projectrebound/current || true)"

if [ ! -d "/tmp/projectrebound-matchserver-next" ]; then
  echo "Error: /tmp/projectrebound-matchserver-next not found. Did scp fail?"
  exit 1
fi

sudo mkdir -p "/opt/projectrebound/releases/${RELEASE}"
sudo cp -a /tmp/projectrebound-matchserver-next/. "/opt/projectrebound/releases/${RELEASE}/"
sudo chown -R projectrebound:projectrebound "/opt/projectrebound/releases/${RELEASE}"

if [ -n "${CURRENT}" ]; then
  sudo ln -sfn "${CURRENT}" /opt/projectrebound/previous
fi

sudo ln -sfn "/opt/projectrebound/releases/${RELEASE}" /opt/projectrebound/current
sudo systemctl restart projectrebound-matchserver
sudo systemctl status projectrebound-matchserver
curl -fsS http://127.0.0.1:5000/health
```

外部再测：

```bash
curl -fsS http://YOUR_DOMAIN_OR_SERVER_IP/health
curl -sS "http://YOUR_DOMAIN_OR_SERVER_IP/v1/rooms"
```

更新期间已有房间会受影响，因为当前是单实例内存后台服务加 SQLite。正式服更新建议先公告维护窗口。

## 14. 回滚

如果更新后冒烟测试失败：

```bash
PREVIOUS="$(readlink -f /opt/projectrebound/previous)"
sudo ln -sfn "${PREVIOUS}" /opt/projectrebound/current
sudo systemctl restart projectrebound-matchserver
sudo systemctl status projectrebound-matchserver
curl -fsS http://127.0.0.1:5000/health
```

回滚应用不会自动回滚 SQLite 数据库。如果新版本引入了数据库结构变更，必须先备份数据库，并准备对应的数据库回滚策略。当前实现使用 `EnsureCreated`，没有 EF migrations；变更实体结构时要特别谨慎。

## 15. 数据库备份

安装 `sqlite3` 后，可以在线备份 SQLite：

```bash
BACKUP="/var/backups/projectrebound/projectrebound-matchserver-$(date +%Y%m%d-%H%M%S).db"
sudo -u projectrebound sqlite3 /var/lib/projectrebound/projectrebound-matchserver.db ".backup '${BACKUP}'"
sudo ls -lh "${BACKUP}"
```

不要只复制 `.db` 文件而忽略 `.db-wal` 和 `.db-shm`。当前 SQLite 开启 WAL 时，直接复制单个 `.db` 文件可能拿到不完整数据。优先使用 `.backup`。

建议：

- 每次应用更新前备份一次。
- 每天定时备份一次。
- 至少保留最近 7 天。
- 在另一台机器上定期恢复验证。

## 16. 日常运维命令

查看状态：

```bash
sudo systemctl status projectrebound-matchserver
```

实时日志：

```bash
sudo journalctl -u projectrebound-matchserver -f
```

最近 200 行日志：

```bash
sudo journalctl -u projectrebound-matchserver -n 200 --no-pager
```

重启后端：

```bash
sudo systemctl restart projectrebound-matchserver
```

检查 Nginx 配置：

```bash
sudo nginx -t
```

重载 Nginx：

```bash
sudo systemctl reload nginx
```

查看监听端口：

```bash
ss -lntup
```

查看数据库大小：

```bash
sudo du -h /var/lib/projectrebound/projectrebound-matchserver.db*
```

## 17. 系统和 .NET Runtime 更新

常规系统更新：

```bash
sudo apt-get update
sudo apt-get upgrade
sudo systemctl restart projectrebound-matchserver
```

只更新 ASP.NET Core Runtime：

```bash
sudo apt-get update
sudo apt-get install --only-upgrade aspnetcore-runtime-8.0
sudo systemctl restart projectrebound-matchserver
```

更新后确认：

```bash
dotnet --list-runtimes
curl -fsS http://127.0.0.1:5000/health
```

## 18. 常见故障

### 后端启动失败

检查：

```bash
sudo journalctl -u projectrebound-matchserver -n 200 --no-pager
dotnet --list-runtimes
sudo ls -lah /opt/projectrebound/current
sudo ls -lah /var/lib/projectrebound
```

常见原因：

- 没安装 `aspnetcore-runtime-8.0`。
- `/var/lib/projectrebound` 没有写权限。
- 发布目录缺文件。
- 旧 SQLite 文件来自不兼容结构。

### 外网访问失败

检查：

```bash
curl -fsS http://127.0.0.1:5000/health
curl -fsS http://YOUR_DOMAIN_OR_SERVER_IP/health
sudo nginx -t
sudo ufw status
```

如果本机 `127.0.0.1:5000` 通，外网不通，优先看 Nginx、UFW、云厂商安全组和 DNS。

### curl /health 正常但 Python GUI 显示 502

优先检查 GUI 的 Backend URL。按本文档部署时应填写：

```text
http://YOUR_DOMAIN_OR_SERVER_IP
```

不要填写：

```text
http://YOUR_DOMAIN_OR_SERVER_IP:5000
```

然后在服务器上分别测试 GUI 会用到的 API：

```bash
curl -fsS http://127.0.0.1:5000/health
curl -fsS http://YOUR_DOMAIN_OR_SERVER_IP/health
curl -sS -i -X POST http://YOUR_DOMAIN_OR_SERVER_IP/v1/auth/guest \
  -H "Content-Type: application/json" \
  -d '{"displayName":"GuiSmoke","deviceToken":null}'
curl -sS -i "http://YOUR_DOMAIN_OR_SERVER_IP/v1/rooms?region=CN&version=dev"
```

如果 `/health` 正常但 `POST /v1/auth/guest` 返回 502，查看后端是否在请求时崩溃：

```bash
sudo journalctl -u projectrebound-matchserver -n 120 --no-pager
```

如果服务器 curl 全部正常而 GUI 仍显示 502，检查 Windows 上 `%APPDATA%/ProjectReboundBrowser/config-python.json` 里的 `backend_url` 是否仍是旧地址或带 `:5000` 的地址。

### UDP probe 失败

这通常不是 Debian 服务器防火墙问题。后端只是向玩家主机公网 IP 和端口发 UDP nonce。

先查后端实际把 probe 发往哪里：

```bash
sudo sqlite3 /var/lib/projectrebound/projectrebound-matchserver.db \
"select publicip, port, status, expiresat from hostprobes order by expiresat desc limit 5;"
```

如果 `publicip` 是 `127.0.0.1` 或服务器内网地址，检查 Nginx 的 `X-Forwarded-For`。如果 `publicip` 是公网地址但 GUI 仍然超时，说明服务器发出的 UDP 没有到达玩家主机。

检查玩家主机：

- Windows 防火墙是否允许游戏 UDP 端口。
- 路由器是否转发 UDP 端口到正确 LAN IP。
- 玩家是否处在运营商 CGNAT 后面。
- GUI 中端口是否和路由器转发端口一致。
- 是否开了 VPN、代理、加速器或公司网关。后端会向 HTTP 请求来源 IP 发 UDP；如果 HTTP 走代理出口，UDP 会被发到代理 IP，而不是玩家电脑。

在玩家 Windows 机器上查看当前公网出口：

```powershell
(Invoke-WebRequest -UseBasicParsing https://api.ipify.org).Content
```

这个 IP 应该和数据库里的 `publicip` 一致。如果不一致，先关闭 VPN/代理/加速器，或者让 GUI 的后端访问不走代理。

### UDP Proxy rendezvous 失败

如果 GUI 勾选 `Use UDP Proxy` 后提示 `UDP rendezvous timed out`，检查服务器 UDP `5001`：

```bash
sudo ss -lunp | grep 5001
sudo ufw status
sudo journalctl -u projectrebound-matchserver -n 120 --no-pager
```

云厂商安全组也要放行 `5001/udp`。Nginx 不代理这个 UDP 端口。

期望能看到类似：

```text
UNCONN 0 0 0.0.0.0:5001 0.0.0.0:* users:(("dotnet",pid=...,fd=...))
```

日志里也应有：

```text
UDP rendezvous service listening on 0.0.0.0:5001
```

如果 `ss` 看不到 `5001`：

- 确认服务器已部署包含 UDP Proxy 的新版后端。
- 确认 `appsettings.json` 或环境变量中 `MatchServer:UdpRendezvousPort` 是 `5001`。
- 重启服务：`sudo systemctl restart projectrebound-matchserver`。

如果 `ss` 能看到 `5001`，但 GUI 仍超时：

- 放行 Debian 防火墙：`sudo ufw allow 5001/udp`。
- 放行云厂商安全组 / 防火墙的 `5001/udp` 入站。
- 确认 GUI 报错里显示的目标是你的服务器公网 IP 或域名，而不是 `127.0.0.1`、内网 IP 或旧地址。
- 临时关闭 Windows 防火墙或安全软件测试出站 UDP。

可以在服务器上抓包确认 UDP 是否到达：

```bash
sudo tcpdump -ni any udp port 5001
```

如果抓不到包，问题在服务器外侧防火墙、云安全组、路由或 Windows 出站网络。如果抓得到包但 GUI 没有响应，查看后端日志是否有 `UDP rendezvous packet failed`。

### UDP Relay 兜底失败

如果 P2P 打洞失败，GUI 的 UDP proxy 会自动尝试服务器 relay。确认服务器 UDP `5002` 已监听并放行：

```bash
sudo ss -lunp | grep 5002
sudo ufw allow 5002/udp
sudo tcpdump -ni any udp port 5002
```

日志里应能看到：

```text
UDP relay service listening on 0.0.0.0:5002
```

云厂商安全组也要放行 `5002/udp`。Nginx 不代理这个 UDP 端口。

可以用仓库里的脚本单独验证 relay：

```powershell
Tools\NatPunchTest\run-host.bat --backend http://YOUR_DOMAIN_OR_SERVER_IP --port 27777 --relay
Tools\NatPunchTest\run-client.bat --backend http://YOUR_DOMAIN_OR_SERVER_IP --room-id ROOM_ID_FROM_HOST --port 27778 --relay
```

看到 `PASS: received pong` 表示 relay 控制面和 UDP 转发面都可用。

### 房间创建成功但别人连不上

检查：

- `/v1/rooms` 返回的 `connect` 是否是公网 IP 和正确端口。
- 客户端是否实际启动了 `-match=ip:port`。
- 主机游戏进程是否真的监听 UDP 端口。
- 同一个局域网内测试可能被 NAT loopback 干扰，建议用不同网络测试。

## 19. 参考资料

- Microsoft Learn: Install .NET on Debian: https://learn.microsoft.com/en-us/dotnet/core/install/linux-debian
- Microsoft Learn: Host ASP.NET Core on Linux with Nginx: https://learn.microsoft.com/aspnet/core/host-and-deploy/linux-nginx
- Microsoft Learn: dotnet publish: https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-publish
- Debian Wiki: systemd documentation: https://wiki.debian.org/systemd/documentation
