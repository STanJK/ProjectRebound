from __future__ import annotations

import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from dataclasses import dataclass, asdict
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from urllib import error, parse, request


APP_DIR = Path(os.environ.get("APPDATA", str(Path.home()))) / "ProjectReboundBrowser"
CONFIG_PATH = APP_DIR / "config-python.json"
GUI_LOG_PATH = APP_DIR / "browser-launch.log"
LAUNCH_DIR = APP_DIR / "launchers"
HARD_CODED_BACKEND_URL = "http://43.240.193.246"
# 来源：ServerWrapper MapList（仅保留非 pveBug 项）与 SetMode 支持值
ROOM_MAP_OPTIONS = (
    "OSS",
    "MiniFarm",
    "Warehouse",
    "DataCenter",
    "CircularX",
)
ROOM_MODE_OPTIONS = ("pve", "pvp")


@dataclass
class AppConfig:
    backend_url: str = HARD_CODED_BACKEND_URL
    game_directory: str = ""
    display_name: str = os.environ.get("USERNAME", "Player")
    region: str = "CN"
    version: str = "dev"
    port: int = 7777
    access_token: str = ""
    room_name: str = "ProjectRebound Room"
    map_name: str = "Warehouse"
    mode: str = "pve"
    max_players: int = 8
    use_udp_proxy: bool = False
    proxy_client_port: int = 17777
    logic_server_url: str = "http://127.0.0.1:8000"


class ApiError(RuntimeError):
    pass


class ApiClient:
    def __init__(self) -> None:
        self.backend_url = HARD_CODED_BACKEND_URL
        self.access_token = ""

    def configure(self, backend_url: str, access_token: str) -> None:
        self.backend_url = backend_url.rstrip("/")
        self.access_token = access_token

    def login_guest(self, display_name: str, device_token: str | None) -> dict:
        return self.post("/v1/auth/guest", {"displayName": display_name, "deviceToken": device_token})

    def create_host_probe(self, port: int) -> dict:
        return self.post("/v1/host-probes", {"port": port})

    def confirm_host_probe(self, probe_id: str, nonce: str) -> dict:
        return self.post(f"/v1/host-probes/{probe_id}/confirm", {"nonce": nonce})

    def create_nat_binding(self, local_port: int, role: str, room_id: str | None = None) -> dict:
        return self.post("/v1/nat/bindings", {"localPort": local_port, "role": role, "roomId": room_id})

    def confirm_nat_binding(self, binding_token: str) -> dict:
        return self.post(f"/v1/nat/bindings/{binding_token}/confirm", {})

    def create_room(self, payload: dict) -> dict:
        return self.post("/v1/rooms", payload)

    def get_room(self, room_id: str) -> dict:
        return self.get(f"/v1/rooms/{room_id}")

    def list_rooms(self, region: str, version: str) -> dict:
        query = parse.urlencode({"region": region, "version": version})
        return self.get(f"/v1/rooms?{query}")

    def join_room(self, room_id: str, version: str) -> dict:
        return self.post(f"/v1/rooms/{room_id}/join", {"version": version})

    def create_punch_ticket(self, room_id: str, join_ticket: str, binding_token: str, client_local_endpoint: str) -> dict:
        return self.post(
            f"/v1/rooms/{room_id}/punch-tickets",
            {"joinTicket": join_ticket, "bindingToken": binding_token, "clientLocalEndpoint": client_local_endpoint},
        )

    def heartbeat_room(self, room_id: str, host_token: str, player_count: int, server_state: str) -> dict:
        return self.post(
            f"/v1/rooms/{room_id}/heartbeat",
            {"hostToken": host_token, "playerCount": player_count, "serverState": server_state},
        )

    def create_match_ticket(self, payload: dict) -> dict:
        return self.post("/v1/matchmaking/tickets", payload)

    def get_match_ticket(self, ticket_id: str) -> dict:
        return self.get(f"/v1/matchmaking/tickets/{ticket_id}")

    def get(self, path: str) -> dict:
        return self._send("GET", path)

    def post(self, path: str, payload: dict) -> dict:
        return self._send("POST", path, payload)

    def _send(self, method: str, path: str, payload: dict | None = None) -> dict:
        body = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.access_token:
            headers["Authorization"] = f"Bearer {self.access_token}"

        req = request.Request(f"{self.backend_url}{path}", data=body, headers=headers, method=method)
        append_gui_log(f"API {method} {path}")
        try:
            with request.urlopen(req, timeout=15) as response:
                raw = response.read().decode("utf-8")
                append_gui_log(f"API {method} {path} -> {response.status}")
                return json.loads(raw) if raw else {}
        except error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            append_gui_log(f"API {method} {path} -> HTTP {exc.code}: {raw}")
            try:
                details = json.loads(raw)
                if "code" in details and "message" in details:
                    raise ApiError(f"{details['code']} during {method} {path}: {details['message']}") from exc
                if "error" in details:
                    err = details["error"]
                    raise ApiError(f"{err.get('code', exc.code)} during {method} {path}: {err.get('message', raw)}") from exc
            except json.JSONDecodeError:
                pass
            raise ApiError(f"HTTP {exc.code} during {method} {path}: {raw}") from exc
        except error.URLError as exc:
            append_gui_log(f"API {method} {path} -> unreachable: {exc.reason}")
            raise ApiError(f"Backend is not reachable during {method} {path}: {exc.reason}") from exc


def load_config() -> AppConfig:
    if not CONFIG_PATH.exists():
        return AppConfig()
    with CONFIG_PATH.open("r", encoding="utf-8") as file:
        data = json.load(file)
    defaults = asdict(AppConfig())
    defaults.update(data)
    defaults["backend_url"] = HARD_CODED_BACKEND_URL
    map_name = str(defaults.get("map_name") or ROOM_MAP_OPTIONS[0])
    mode = str(defaults.get("mode") or ROOM_MODE_OPTIONS[0])
    defaults["map_name"] = map_name if map_name in ROOM_MAP_OPTIONS else ROOM_MAP_OPTIONS[0]
    defaults["mode"] = mode if mode in ROOM_MODE_OPTIONS else ROOM_MODE_OPTIONS[0]
    return AppConfig(**defaults)


def save_config(config: AppConfig) -> None:
    APP_DIR.mkdir(parents=True, exist_ok=True)
    with CONFIG_PATH.open("w", encoding="utf-8") as file:
        json.dump(asdict(config), file, indent=2)


def append_gui_log(message: str) -> None:
    APP_DIR.mkdir(parents=True, exist_ok=True)
    with GUI_LOG_PATH.open("a", encoding="utf-8") as file:
        file.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}\n")


def quote_bat(value: str | Path) -> str:
    return '"' + str(value).replace('"', '""') + '"'


def write_batch(name: str, lines: list[str]) -> Path:
    LAUNCH_DIR.mkdir(parents=True, exist_ok=True)
    path = LAUNCH_DIR / name
    path.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    append_gui_log(f"Wrote launcher batch: {path}")
    return path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def runtime_base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def runtime_artifacts_dir() -> Path:
    return runtime_base_dir() / "runtime"


def find_file(root: str, file_name: str) -> str | None:
    if not root or not os.path.isdir(root):
        return None
    direct = Path(root) / file_name
    if direct.exists():
        return str(direct)
    for path, _, files in os.walk(root):
        if file_name in files:
                return str(Path(path) / file_name)
    return None


def candidate_roots(root: str) -> list[Path]:
    if not root or not os.path.isdir(root):
        return []
    start = Path(root).resolve()
    roots: list[Path] = []
    for path in [start, *list(start.parents)[:5]]:
        if path not in roots:
            roots.append(path)
    return roots


def find_file_near(root: str, file_name: str) -> str | None:
    for base in candidate_roots(root):
        direct = base / file_name
        if direct.exists():
            return str(direct)
        nested_node = base / "nodejs" / file_name
        if nested_node.exists():
            return str(nested_node)
    return find_file(root, file_name)


def find_directory(root: str, directory_name: str) -> str | None:
    if not root or not os.path.isdir(root):
        return None
    direct = Path(root) / directory_name
    if direct.is_dir():
        return str(direct)
    for path, dirs, _ in os.walk(root):
        if directory_name in dirs:
            return str(Path(path) / directory_name)
    return None


def find_directory_near(root: str, directory_name: str) -> str | None:
    for base in candidate_roots(root):
        direct = base / directory_name
        if direct.is_dir():
            return str(direct)
    return find_directory(root, directory_name)


def logic_server_endpoint(logic_server_url: str) -> tuple[str, int] | None:
    parsed = parse.urlparse(logic_server_url)
    if not parsed.hostname:
        return None
    if parsed.port:
        return parsed.hostname, parsed.port
    return parsed.hostname, 443 if parsed.scheme == "https" else 80


def is_local_host(host: str) -> bool:
    return host.lower() in {"localhost", "127.0.0.1", "::1"}


def is_tcp_open(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except OSError:
        return False


def backend_for_game(backend_url: str) -> str:
    parsed = parse.urlparse(backend_url)
    if parsed.hostname:
        if parsed.port:
            return f"{parsed.hostname}:{parsed.port}"
        if parsed.scheme == "http":
            return f"{parsed.hostname}:80"
        if parsed.scheme == "https":
            return f"{parsed.hostname}:443"
        return parsed.hostname
    return backend_url.replace("http://", "").replace("https://", "").rstrip("/")


def sanitize_arg(value: str) -> str:
    return value.replace(" ", "_").replace("\t", "_")


def game_exe_dir(game_directory: str) -> Path | None:
    exe = find_file(game_directory, "ProjectBoundarySteam-Win64-Shipping.exe")
    return Path(exe).parent if exe else None


def latest_existing(paths: list[Path]) -> Path | None:
    existing = [path for path in paths if path.exists()]
    if not existing:
        return None
    return max(existing, key=lambda path: path.stat().st_mtime)


def proxy_launcher() -> tuple[list[str], Path]:
    if getattr(sys, "frozen", False):
        base = runtime_base_dir()
        candidates = [
            base / "ProjectReboundUdpProxy.exe",
            base / "project_rebound_udp_proxy.exe",
        ]
        proxy_exe = next((path for path in candidates if path.exists()), None)
        if proxy_exe is None:
            raise RuntimeError(
                "project_rebound_udp_proxy 可执行文件未找到。请确认分发目录包含 ProjectReboundUdpProxy.exe。"
            )
        return [str(proxy_exe)], proxy_exe.parent

    proxy_script = Path(__file__).with_name("project_rebound_udp_proxy.py")
    if not proxy_script.exists():
        raise RuntimeError("project_rebound_udp_proxy.py was not found next to project_rebound_browser.py.")
    return [sys.executable, str(proxy_script)], proxy_script.parent


class BrowserApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ProjectRebound Browser - Python Prototype")
        self.geometry("1120x700")
        self.minsize(980, 620)

        self.config_data = load_config()
        self.config_data.backend_url = HARD_CODED_BACKEND_URL
        self.api = ApiClient()
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        self.ui_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.selected_room: dict | None = None
        self.rooms: list[dict] = []
        self.host_heartbeat_stop: threading.Event | None = None
        self.host_heartbeat_thread: threading.Thread | None = None

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(100, self._drain_queue)
        self.run_background(self.initialize)

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)

        settings = ttk.LabelFrame(root, text="Settings", padding=8)
        settings.pack(fill=tk.X)

        self.game_dir_var = tk.StringVar(value=self.config_data.game_directory)
        self.display_name_var = tk.StringVar(value=self.config_data.display_name)
        self.region_var = tk.StringVar(value=self.config_data.region)
        self.version_var = tk.StringVar(value=self.config_data.version)

        self._field(settings, "Display", self.display_name_var, 0, 0, width=20)
        self._field(settings, "Region", self.region_var, 0, 2, width=10)
        self._field(settings, "Version", self.version_var, 0, 4, width=10)
        self._field(settings, "Game Dir", self.game_dir_var, 1, 0, width=64)
        ttk.Button(settings, text="Browse", command=self.browse_game_dir).grid(row=1, column=2, sticky="ew", padx=4, pady=4)
        ttk.Button(settings, text="Save / Login", command=lambda: self.run_background(self.save_and_login)).grid(row=1, column=3, sticky="ew", padx=4, pady=4)

        room_box = ttk.LabelFrame(root, text="Room", padding=8)
        room_box.pack(fill=tk.X, pady=(10, 0))

        self.room_name_var = tk.StringVar(value=self.config_data.room_name)
        current_map = self.config_data.map_name if self.config_data.map_name in ROOM_MAP_OPTIONS else ROOM_MAP_OPTIONS[0]
        current_mode = self.config_data.mode if self.config_data.mode in ROOM_MODE_OPTIONS else ROOM_MODE_OPTIONS[0]
        self.map_var = tk.StringVar(value=current_map)
        self.mode_var = tk.StringVar(value=current_mode)
        self.port_var = tk.IntVar(value=self.config_data.port)
        self.max_players_var = tk.IntVar(value=self.config_data.max_players)
        self.use_proxy_var = tk.BooleanVar(value=self.config_data.use_udp_proxy)
        self.proxy_client_port_var = tk.IntVar(value=self.config_data.proxy_client_port)
        self.logic_server_url_var = tk.StringVar(value=self.config_data.logic_server_url)

        self._field(room_box, "Name", self.room_name_var, 0, 0, width=24)
        self._combo_field(room_box, "Map", self.map_var, list(ROOM_MAP_OPTIONS), 0, 2, width=14)
        self._combo_field(room_box, "Mode", self.mode_var, list(ROOM_MODE_OPTIONS), 0, 4, width=10)
        self._field(room_box, "Port", self.port_var, 0, 6, width=8)
        self._field(room_box, "Max", self.max_players_var, 0, 8, width=8)
        ttk.Checkbutton(room_box, text="Use UDP Proxy", variable=self.use_proxy_var).grid(row=1, column=0, sticky="w", padx=4, pady=4)
        self._field(room_box, "Client Proxy", self.proxy_client_port_var, 1, 2, width=8)
        self._field(room_box, "Logic URL", self.logic_server_url_var, 1, 4, width=32)

        buttons = ttk.Frame(room_box)
        buttons.grid(row=0, column=10, sticky="e", padx=(12, 0))
        ttk.Button(buttons, text="Refresh", command=lambda: self.run_background(self.refresh_rooms)).pack(side=tk.LEFT, padx=4)
        ttk.Button(buttons, text="Create", command=lambda: self.run_background(self.create_room)).pack(side=tk.LEFT, padx=4)
        ttk.Button(buttons, text="Join", command=lambda: self.run_background(self.join_selected_room)).pack(side=tk.LEFT, padx=4)
        ttk.Button(buttons, text="Quick Match", command=lambda: self.run_background(self.quick_match)).pack(side=tk.LEFT, padx=4)

        columns = ("name", "region", "map", "mode", "players", "max", "state", "endpoint", "last")
        self.tree = ttk.Treeview(root, columns=columns, show="headings", selectmode="browse")
        self.tree.pack(fill=tk.BOTH, expand=True, pady=(10, 0))
        headings = {
            "name": ("Name", 220),
            "region": ("Region", 80),
            "map": ("Map", 130),
            "mode": ("Mode", 120),
            "players": ("Players", 80),
            "max": ("Max", 60),
            "state": ("State", 100),
            "endpoint": ("Endpoint", 160),
            "last": ("Last Seen", 170),
        }
        for column, (title, width) in headings.items():
            self.tree.heading(column, text=title)
            self.tree.column(column, width=width, anchor=tk.W)
        self.tree.bind("<<TreeviewSelect>>", self.on_room_selected)

        self.status_var = tk.StringVar(value="Ready.")
        ttk.Label(root, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W, padding=6).pack(fill=tk.X, pady=(10, 0))

    def _field(self, parent: tk.Misc, label: str, variable: tk.Variable, row: int, column: int, width: int) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky="w", padx=(0, 4), pady=4)
        ttk.Entry(parent, textvariable=variable, width=width).grid(row=row, column=column + 1, sticky="ew", padx=(0, 8), pady=4)

    def _combo_field(self, parent: tk.Misc, label: str, variable: tk.StringVar, values: list[str], row: int, column: int, width: int) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky="w", padx=(0, 4), pady=4)
        ttk.Combobox(parent, textvariable=variable, values=values, state="readonly", width=width).grid(
            row=row,
            column=column + 1,
            sticky="ew",
            padx=(0, 8),
            pady=4,
        )

    def _drain_queue(self) -> None:
        try:
            while True:
                kind, payload = self.ui_queue.get_nowait()
                if kind == "status":
                    self.status_var.set(str(payload))
                elif kind == "rooms":
                    self._render_rooms(payload)  # type: ignore[arg-type]
                elif kind == "error":
                    self.status_var.set(str(payload))
                    messagebox.showerror("ProjectRebound Browser", str(payload))
        except queue.Empty:
            pass
        self.after(100, self._drain_queue)

    def set_status(self, text: str) -> None:
        self.ui_queue.put(("status", text))

    def run_background(self, func) -> None:
        def worker() -> None:
            try:
                func()
            except Exception as exc:
                self.ui_queue.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def on_close(self) -> None:
        self.stop_host_heartbeat()
        self.destroy()

    def read_form(self) -> AppConfig:
        selected_map = self.map_var.get().strip()
        selected_mode = self.mode_var.get().strip()
        return AppConfig(
            backend_url=HARD_CODED_BACKEND_URL,
            game_directory=self.game_dir_var.get().strip(),
            display_name=self.display_name_var.get().strip() or "Player",
            region=self.region_var.get().strip() or "CN",
            version=self.version_var.get().strip() or "dev",
            port=int(self.port_var.get()),
            access_token=self.config_data.access_token,
            room_name=self.room_name_var.get().strip() or "ProjectRebound Room",
            map_name=selected_map if selected_map in ROOM_MAP_OPTIONS else ROOM_MAP_OPTIONS[0],
            mode=selected_mode if selected_mode in ROOM_MODE_OPTIONS else ROOM_MODE_OPTIONS[0],
            max_players=int(self.max_players_var.get()),
            use_udp_proxy=bool(self.use_proxy_var.get()),
            proxy_client_port=int(self.proxy_client_port_var.get()),
            logic_server_url=self.logic_server_url_var.get().strip() or "http://127.0.0.1:8000",
        )

    def initialize(self) -> None:
        self.save_and_login()
        self.refresh_rooms()

    def save_and_login(self) -> None:
        self.config_data = self.read_form()
        self.api.configure(self.config_data.backend_url, "")
        self.set_status("Logging in...")
        auth = self.api.login_guest(
            self.config_data.display_name,
            self.config_data.access_token if self.config_data.access_token else None,
        )
        self.config_data.access_token = auth["accessToken"]
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        save_config(self.config_data)
        self.set_status(f"Logged in as {auth.get('displayName', self.config_data.display_name)}.")

    def refresh_rooms(self) -> None:
        self.config_data = self.read_form()
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        self.set_status("Refreshing rooms...")
        result = self.api.list_rooms(self.config_data.region, self.config_data.version)
        self.ui_queue.put(("rooms", result.get("items", [])))
        self.set_status(f"Loaded {len(result.get('items', []))} rooms.")

    def create_room(self) -> None:
        self.ensure_ready_for_launch()
        probe = None
        binding = None
        if self.config_data.use_udp_proxy:
            self.set_status("Registering host UDP proxy binding...")
            binding = self.run_nat_binding(self.config_data.port, "host")
        else:
            self.set_status("Probing host UDP port...")
            probe = self.run_host_probe()
        self.set_status("Creating room...")
        created = self.api.create_room(
            {
                "probeId": probe["probeId"] if probe else None,
                "bindingToken": binding["bindingToken"] if binding else None,
                "name": self.config_data.room_name,
                "region": self.config_data.region,
                "map": self.config_data.map_name,
                "mode": self.config_data.mode,
                "version": self.config_data.version,
                "maxPlayers": self.config_data.max_players,
            }
        )
        room = self.api.get_room(created["roomId"])
        self.start_host(room, created["hostToken"])
        self.set_status(f"Created room {room.get('name')} and launched host.")
        self.refresh_rooms()

    def join_selected_room(self) -> None:
        self.ensure_ready_for_launch()
        if not self.selected_room:
            raise RuntimeError("Select a room first.")
        self.set_status("Reserving room slot...")
        join = self.api.join_room(self.selected_room["roomId"], self.config_data.version)
        if self.config_data.use_udp_proxy:
            self.set_status("Starting local UDP proxy...")
            self.start_client_proxy(self.selected_room["roomId"], join["joinTicket"])
            connect = f"127.0.0.1:{self.config_data.proxy_client_port}"
            self.set_status("Launching game client...")
            self.start_client(connect)
            self.set_status(f"Launching client through local proxy {connect}.")
        else:
            self.set_status("Launching game client...")
            self.start_client(join["connect"])
            self.set_status(f"Launching client for {join['connect']}.")

    def quick_match(self) -> None:
        self.ensure_ready_for_launch()
        if self.config_data.use_udp_proxy:
            raise RuntimeError("Quick Match with UDP Proxy is not wired yet. Create a proxy room first, then join it from another client.")
        self.set_status("Probing host UDP port for quick match...")
        probe = self.run_host_probe()
        ticket = self.api.create_match_ticket(
            {
                "region": self.config_data.region,
                "map": self.config_data.map_name,
                "mode": self.config_data.mode,
                "version": self.config_data.version,
                "canHost": True,
                "probeId": probe["probeId"],
                "roomName": self.config_data.room_name,
                "maxPlayers": self.config_data.max_players,
            }
        )
        ticket_id = ticket["ticketId"]
        for _ in range(70):
            time.sleep(2)
            state = self.api.get_match_ticket(ticket_id)
            self.set_status(f"Matchmaking: {state.get('state')}.")
            if state.get("state") == "HostAssigned" and state.get("room") and state.get("hostToken"):
                self.start_host(state["room"], state["hostToken"])
                self.set_status(f"You are host for {state['room'].get('name')}.")
                self.refresh_rooms()
                return
            if state.get("state") == "Matched" and state.get("connect"):
                self.start_client(state["connect"])
                self.set_status(f"Matched. Launching client for {state['connect']}.")
                return
            if state.get("state") in {"Failed", "Canceled", "Expired"}:
                self.set_status(f"Matchmaking ended: {state.get('failureReason') or state.get('state')}.")
                return
        self.set_status("Matchmaking timed out.")

    def run_host_probe(self) -> dict:
        self.config_data = self.read_form()
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        port = self.config_data.port
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("", port))
            sock.settimeout(15)
            probe = self.api.create_host_probe(port)
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout as exc:
                target = f"{probe.get('publicIp', 'unknown')}:{probe.get('port', port)}"
                raise RuntimeError(
                    f"UDP probe timed out. Backend sent the probe to {target}. "
                    "Check VPN/proxy, firewall, CGNAT, and port forwarding."
                ) from exc
        nonce = data.decode("utf-8", errors="replace")
        if nonce != probe["nonce"]:
            raise RuntimeError("UDP probe nonce mismatch.")
        self.api.confirm_host_probe(probe["probeId"], nonce)
        return probe

    def run_nat_binding(self, local_port: int, role: str) -> dict:
        self.config_data = self.read_form()
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("", local_port))
            sock.settimeout(1)
            created = self.api.create_nat_binding(local_port, role)
            server = (created.get("udpHost") or parse.urlparse(self.config_data.backend_url).hostname, int(created["udpPort"]))
            packet = json.dumps({"type": "nat-binding", "token": created["bindingToken"], "localPort": local_port}).encode("utf-8")
            deadline = time.time() + 8
            while time.time() < deadline:
                sock.sendto(packet, server)
                try:
                    data, _ = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                try:
                    response = json.loads(data.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if response.get("token") == created["bindingToken"]:
                    return self.api.confirm_nat_binding(created["bindingToken"])
        raise RuntimeError(f"UDP rendezvous timed out. Sent to {server[0]}:{server[1]}. Check server UDP 5001, cloud security group, and local firewall.")

    def start_client(self, connect: str) -> None:
        exe = find_file(self.config_data.game_directory, "ProjectBoundarySteam-Win64-Shipping.exe")
        if not exe:
            raise RuntimeError("ProjectBoundarySteam-Win64-Shipping.exe was not found under the game directory.")
        self.ensure_payload_files(Path(exe).parent)
        self.launch_client_via_batch(exe, connect)

    def launch_client_via_batch(self, exe: str, connect: str) -> None:
        backend_dir = find_directory_near(self.config_data.game_directory, "BoundaryMetaServer-main")
        node = find_file_near(self.config_data.game_directory, "node.exe")
        if not backend_dir or not node:
            raise RuntimeError("nodejs/node.exe or BoundaryMetaServer-main was not found under the game directory.")

        lines = [
            "@echo off",
            "title Project Rebound Client Launcher",
            "echo [Launcher] Starting fake login server...",
        ]
        endpoint = logic_server_endpoint(self.config_data.logic_server_url)
        if endpoint and is_local_host(endpoint[0]) and is_tcp_open(endpoint[0], endpoint[1]):
            lines.append(f"echo [Launcher] Fake login server already reachable at {endpoint[0]}:{endpoint[1]}.")
        else:
            lines.extend([
                f'start "" /B /D {quote_bat(backend_dir)} {quote_bat(node)} ' + quote_bat("index.js"),
                "echo [Launcher] Waiting for login server to initialize...",
                "timeout /t 5 >nul",
            ])

        lines.extend([
            "if not exist " + quote_bat(Path(exe).parent / "dxgi.dll") + " echo [Launcher] WARNING: dxgi.dll is missing next to the game exe.",
            "if not exist " + quote_bat(Path(exe).parent / "Payload.dll") + " echo [Launcher] WARNING: Payload.dll is missing next to the game exe.",
            "echo [Launcher] Launching game client...",
            (
                f"start \"\" /D {quote_bat(Path(exe).parent)} {quote_bat(exe)} "
                f"-LogicServerURL={self.config_data.logic_server_url} -match={connect} -debuglog"
            ),
            "echo.",
            "echo [Launcher] Client launch requested.",
            "echo This window can be closed after the game has started.",
        ])

        batch = write_batch("launch-client.bat", lines)
        append_gui_log(f"Launching client batch: {batch}")
        subprocess.Popen(["cmd.exe", "/c", str(batch)], creationflags=self.creation_flags())

    def ensure_fake_login_server(self) -> None:
        endpoint = logic_server_endpoint(self.config_data.logic_server_url)
        if endpoint is None:
            append_gui_log(f"Logic URL is not parseable, skipping fake login autostart: {self.config_data.logic_server_url}")
            return

        host, port = endpoint
        if not is_local_host(host):
            append_gui_log(f"Logic URL is not local, skipping fake login autostart: {self.config_data.logic_server_url}")
            return

        if is_tcp_open(host, port):
            append_gui_log(f"Fake login server already reachable at {host}:{port}")
            return

        node = find_file_near(self.config_data.game_directory, "node.exe")
        backend_dir = find_directory_near(self.config_data.game_directory, "BoundaryMetaServer-main")
        if not node or not backend_dir:
            raise RuntimeError(
                "Local fake login server is not running, and nodejs/node.exe or "
                "BoundaryMetaServer-main was not found under the game directory."
            )

        args = [node, "index.js"]
        append_gui_log("Starting fake login server: " + subprocess.list2cmdline(args) + f" cwd={backend_dir}")
        subprocess.Popen(
            args,
            cwd=backend_dir,
            creationflags=self.login_server_creation_flags(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + 6
        while time.time() < deadline:
            if is_tcp_open(host, port):
                append_gui_log(f"Fake login server is ready at {host}:{port}")
                return
            time.sleep(0.25)

        raise RuntimeError(f"Fake login server did not become reachable at {host}:{port}.")

    def start_client_proxy(self, room_id: str, join_ticket: str) -> None:
        launcher, launcher_cwd = proxy_launcher()
        args = [
            *launcher,
            "client",
            "--backend",
            self.config_data.backend_url,
            "--access-token",
            self.config_data.access_token,
            "--room-id",
            room_id,
            "--join-ticket",
            join_ticket,
            "--listen-port",
            str(self.config_data.proxy_client_port),
        ]
        subprocess.Popen(args, cwd=str(launcher_cwd), creationflags=self.creation_flags())
        time.sleep(1)

    def start_host(self, room: dict, host_token: str) -> None:
        exe_dir = game_exe_dir(self.config_data.game_directory)
        if exe_dir:
            self.ensure_payload_files(exe_dir)
            self.ensure_wrapper_file(exe_dir)
        wrapper = str(exe_dir / "ProjectReboundServerWrapper.exe") if exe_dir and (exe_dir / "ProjectReboundServerWrapper.exe").exists() else find_file(self.config_data.game_directory, "ProjectReboundServerWrapper.exe")
        backend = backend_for_game(self.config_data.backend_url)
        game_port = self.config_data.port + 1 if self.config_data.use_udp_proxy else int(room.get("port", self.config_data.port))
        if wrapper:
            args = self.wrapper_args(wrapper, room, host_token, backend, game_port, exe_dir)
            if self.config_data.use_udp_proxy:
                self.launch_host_via_batch(args, room["roomId"], host_token, game_port, exe_dir or Path(wrapper).parent)
                self.start_host_heartbeat(room, host_token)
                return
            self.ensure_fake_login_server()
            wrapper_cwd = str(exe_dir or Path(wrapper).parent)
            append_gui_log("Launching wrapper: " + subprocess.list2cmdline(args) + f" cwd={wrapper_cwd}")
            subprocess.Popen(args, cwd=wrapper_cwd, creationflags=self.creation_flags())
            self.start_host_heartbeat(room, host_token)
            return

        exe = find_file(self.config_data.game_directory, "ProjectBoundarySteam-Win64-Shipping.exe")
        if not exe:
            raise RuntimeError("Neither ProjectReboundServerWrapper.exe nor ProjectBoundarySteam-Win64-Shipping.exe was found.")
        self.ensure_fake_login_server()
        args = [
            exe,
            "-log",
            "-server",
            "-nullrhi",
            f"-online={backend}",
            f"-roomid={room['roomId']}",
            f"-hosttoken={host_token}",
            f"-map={room.get('map', self.config_data.map_name)}",
            f"-mode={self.mode_to_path(room.get('mode', self.config_data.mode))}",
            f"-servername={sanitize_arg(room.get('name', self.config_data.room_name))}",
            f"-serverregion={room.get('region', self.config_data.region)}",
            f"-port={game_port}",
        ]
        if room.get("mode", self.config_data.mode).lower() == "pve":
            args.append("-pve")
        append_gui_log("Launching server exe: " + subprocess.list2cmdline(args))
        subprocess.Popen(args, cwd=str(Path(exe).parent), creationflags=self.creation_flags())
        self.start_host_heartbeat(room, host_token)

    def stop_host_heartbeat(self) -> None:
        if self.host_heartbeat_stop is not None:
            self.host_heartbeat_stop.set()
        self.host_heartbeat_stop = None
        self.host_heartbeat_thread = None

    def start_host_heartbeat(self, room: dict, host_token: str) -> None:
        self.stop_host_heartbeat()

        stop_event = threading.Event()
        self.host_heartbeat_stop = stop_event
        room_id = str(room["roomId"])
        backend_url = self.config_data.backend_url
        player_count = max(1, int(room.get("playerCount") or 1))

        def worker() -> None:
            api = ApiClient()
            api.configure(backend_url, "")
            append_gui_log(f"Starting GUI host heartbeat for room {room_id}")
            success_logged = False
            while not stop_event.is_set():
                delay = 5
                try:
                    response = api.heartbeat_room(room_id, host_token, player_count, "Hosting")
                    delay = int(response.get("nextHeartbeatSeconds", 5))
                    if not success_logged:
                        append_gui_log(f"GUI host heartbeat is active for room {room_id}")
                        success_logged = True
                except Exception as exc:
                    append_gui_log(f"GUI host heartbeat failed for room {room_id}: {exc}")
                    if "ROOM_ENDED" in str(exc) or "NOT_FOUND" in str(exc):
                        break
                delay = max(2, min(delay, 10))
                stop_event.wait(delay)
            append_gui_log(f"GUI host heartbeat stopped for room {room_id}")

        thread = threading.Thread(target=worker, daemon=True)
        self.host_heartbeat_thread = thread
        thread.start()

    def wrapper_args(self, wrapper: str, room: dict, host_token: str, backend: str, game_port: int, exe_dir: Path | None) -> list[str]:
        args = [
            wrapper,
            f"-online={backend}",
            f"-roomid={room['roomId']}",
            f"-hosttoken={host_token}",
            f"-map={room.get('map', self.config_data.map_name)}",
            f"-mode={room.get('mode', self.config_data.mode)}",
            f"-servername={sanitize_arg(room.get('name', self.config_data.room_name))}",
            f"-serverregion={room.get('region', self.config_data.region)}",
            f"-port={game_port}",
        ]
        if exe_dir:
            args.append(f"-gameexe={exe_dir / 'ProjectBoundarySteam-Win64-Shipping.exe'}")
        return args

    def launch_host_via_batch(self, wrapper_args: list[str], room_id: str, host_token: str, game_port: int, wrapper_cwd: Path) -> None:
        backend_dir = find_directory_near(self.config_data.game_directory, "BoundaryMetaServer-main")
        node = find_file_near(self.config_data.game_directory, "node.exe")
        if not backend_dir or not node:
            raise RuntimeError("nodejs/node.exe or BoundaryMetaServer-main was not found under the game directory.")

        launcher, launcher_cwd = proxy_launcher()
        proxy_args = [
            *launcher,
            "host",
            "--backend",
            self.config_data.backend_url,
            "--access-token",
            self.config_data.access_token,
            "--room-id",
            room_id,
            "--host-token",
            host_token,
            "--public-port",
            str(self.config_data.port),
            "--game-port",
            str(game_port),
        ]

        game_exe = wrapper_cwd / "ProjectBoundarySteam-Win64-Shipping.exe"
        wrapper_exe = wrapper_cwd / "ProjectReboundServerWrapper.exe"
        wrapper_tail = subprocess.list2cmdline(wrapper_args[1:])
        readiness_command = (
            "powershell -NoProfile -ExecutionPolicy Bypass -Command "
            "\"$started=Get-Date; "
            "$deadline=$started.AddSeconds(90); "
            "$ready=$false; "
            "while((Get-Date) -lt $deadline){ "
            "$log=Get-ChildItem -Path 'logs\\log-*.txt' -ErrorAction SilentlyContinue | "
            "Where-Object { $_.LastWriteTime -ge $started } | "
            "Sort-Object LastWriteTime -Descending | Select-Object -First 1; "
            "if($log){ "
            "$text=Get-Content -Path $log.FullName -Tail 100 -ErrorAction SilentlyContinue; "
            "if($text -match 'Server is now listening|Heartbeat received|\\[HEARTBEAT\\]'){ $ready=$true; break } "
            "} "
            "Start-Sleep -Seconds 2 "
            "}; "
            "if($ready){ Write-Host '[Launcher] Server readiness confirmed.'; exit 0 } "
            "Write-Host '[Launcher] ERROR: server did not become ready within 90 seconds.'; exit 1\""
        )

        lines = [
            "@echo off",
            "title Project Rebound Host Launcher",
            "pushd " + quote_bat(wrapper_cwd),
            "echo [Launcher] Starting fake login server...",
        ]
        endpoint = logic_server_endpoint(self.config_data.logic_server_url)
        if endpoint and is_local_host(endpoint[0]) and is_tcp_open(endpoint[0], endpoint[1]):
            lines.append(f"echo [Launcher] Fake login server already reachable at {endpoint[0]}:{endpoint[1]}.")
        else:
            lines.extend([
                f'start "" /B /D {quote_bat(backend_dir)} {quote_bat(node)} ' + quote_bat("index.js"),
                "echo [Launcher] Waiting for login server to initialize...",
                "timeout /t 5 >nul",
            ])

        lines.extend([
            "if not exist dxgi.dll echo [Launcher] WARNING: dxgi.dll is missing next to the game exe.",
            "if not exist Payload.dll echo [Launcher] WARNING: Payload.dll is missing next to the game exe.",
            "echo [Launcher] Starting UDP proxy...",
            "start \"Project Rebound Host UDP Proxy\" /MIN " + subprocess.list2cmdline(proxy_args),
            "echo [Launcher] Waiting for proxy to initialize...",
            "timeout /t 2 >nul",
            f"echo [Launcher] Starting dedicated server wrapper on UDP {game_port}...",
            "start \"\" /MIN " + quote_bat(wrapper_exe) + (" " + wrapper_tail if wrapper_tail else ""),
            "echo [Launcher] Waiting for game server to listen...",
            readiness_command,
            "if errorlevel 1 goto server_not_ready",
            "echo [Launcher] Launching game client...",
            "start \"\" "
            + quote_bat(game_exe)
            + f" -LogicServerURL={self.config_data.logic_server_url} -match=127.0.0.1:{game_port} -debuglog",
            "goto launcher_done",
            ":server_not_ready",
            "echo [Launcher] Client was not launched because the dedicated server was not ready.",
            "echo [Launcher] Check the newest logs\\log-*.txt for map load or port errors.",
            ":launcher_done",
            "echo.",
            "echo [Launcher] All systems running. DO NOT CLOSE THIS WINDOW.",
            "echo Closing this window may shut down child consoles depending on Windows settings.",
            "echo.",
            "pause >nul",
            "popd",
        ])

        batch = write_batch("launch-host.bat", lines)
        append_gui_log(f"Launching host batch: {batch}")
        subprocess.Popen(["cmd.exe", "/c", str(batch)], creationflags=self.creation_flags())

    def ensure_payload_files(self, exe_dir: Path) -> None:
        root = repo_root()
        sources = {
            "dxgi.dll": latest_existing([
                runtime_artifacts_dir() / "dxgi.dll",
                root / "dxgi" / "x64" / "Release" / "dxgi.dll",
                root / "dxgi" / "dxgi" / "x64" / "Release" / "dxgi.dll",
            ]),
            "Payload.dll": latest_existing([
                runtime_artifacts_dir() / "Payload.dll",
                root / "Payload" / "x64" / "Release" / "Payload.dll",
                root / "Payload" / "Payload" / "x64" / "Release" / "Payload.dll",
            ]),
        }

        missing = [name for name, source in sources.items() if source is None]
        if missing:
            raise RuntimeError("Built payload files were not found: " + ", ".join(missing))

        for name, source in sources.items():
            assert source is not None
            target = exe_dir / name
            should_copy = not target.exists() or source.stat().st_mtime > target.stat().st_mtime or source.stat().st_size != target.stat().st_size
            if should_copy:
                shutil.copy2(source, target)
                append_gui_log(f"Copied {source} -> {target}")
            else:
                append_gui_log(f"Payload file already current: {target}")

    def ensure_wrapper_file(self, exe_dir: Path) -> None:
        root = repo_root()
        source = latest_existing([
            runtime_artifacts_dir() / "ProjectReboundServerWrapper.exe",
            root / "ServerWrapper" / "ProjectReboundServerWrapper" / "x64" / "Release" / "ProjectReboundServerWrapper.exe",
            root / "ServerWrapper" / "ProjectReboundServerWrapper" / "ProjectReboundServerWrapper" / "x64" / "Release" / "ProjectReboundServerWrapper.exe",
        ])
        if source is None:
            append_gui_log("Built ProjectReboundServerWrapper.exe was not found; using existing wrapper from game directory.")
            return

        target = exe_dir / "ProjectReboundServerWrapper.exe"
        should_copy = not target.exists() or source.stat().st_mtime > target.stat().st_mtime or source.stat().st_size != target.stat().st_size
        if should_copy:
            try:
                shutil.copy2(source, target)
            except PermissionError as exc:
                raise RuntimeError(
                    "Could not update ProjectReboundServerWrapper.exe. "
                    "Close existing wrapper/server/game launcher windows and try again."
                ) from exc
            append_gui_log(f"Copied {source} -> {target}")
        else:
            append_gui_log(f"Wrapper already current: {target}")

    def start_host_proxy(self, room_id: str, host_token: str, game_port: int) -> None:
        launcher, launcher_cwd = proxy_launcher()
        args = [
            *launcher,
            "host",
            "--backend",
            self.config_data.backend_url,
            "--access-token",
            self.config_data.access_token,
            "--room-id",
            room_id,
            "--host-token",
            host_token,
            "--public-port",
            str(self.config_data.port),
            "--game-port",
            str(game_port),
        ]
        subprocess.Popen(args, cwd=str(launcher_cwd), creationflags=self.creation_flags())
        time.sleep(1)

    def ensure_ready_for_launch(self) -> None:
        self.config_data = self.read_form()
        if not self.config_data.access_token:
            self.save_and_login()
        if not self.config_data.game_directory or not os.path.isdir(self.config_data.game_directory):
            raise RuntimeError("Set a valid game directory first.")
        save_config(self.config_data)

    def browse_game_dir(self) -> None:
        selected = filedialog.askdirectory(
            title="Select Project Boundary game directory",
            initialdir=self.game_dir_var.get() or os.getcwd(),
        )
        if selected:
            self.game_dir_var.set(selected)

    def on_room_selected(self, _event) -> None:
        selected = self.tree.selection()
        if not selected:
            self.selected_room = None
            return
        index = int(selected[0])
        self.selected_room = self.rooms[index]

    def _render_rooms(self, rooms: list[dict]) -> None:
        self.rooms = rooms
        self.selected_room = None
        for item in self.tree.get_children():
            self.tree.delete(item)
        for index, room in enumerate(rooms):
            self.tree.insert(
                "",
                tk.END,
                iid=str(index),
                values=(
                    room.get("name", ""),
                    room.get("region", ""),
                    room.get("map", ""),
                    room.get("mode", ""),
                    room.get("playerCount", 0),
                    room.get("maxPlayers", 0),
                    room.get("state", ""),
                    room.get("endpoint", ""),
                    room.get("lastSeenAt", ""),
                ),
            )

    @staticmethod
    def mode_to_path(mode: str) -> str:
        if mode.lower() == "pvp":
            return "/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C"
        return "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Normal.BP_PBGameMode_Rush_PVE_Normal_C"

    @staticmethod
    def creation_flags() -> int:
        return getattr(subprocess, "CREATE_NEW_CONSOLE", 0)

    @staticmethod
    def client_creation_flags() -> int:
        return 0

    @staticmethod
    def login_server_creation_flags() -> int:
        return getattr(subprocess, "CREATE_NO_WINDOW", 0)


if __name__ == "__main__":
    app = BrowserApp()
    app.mainloop()
