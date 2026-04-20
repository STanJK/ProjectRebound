from __future__ import annotations

import json
import os
import queue
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


@dataclass
class AppConfig:
    backend_url: str = "http://127.0.0.1:5000"
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
        self.backend_url = "http://127.0.0.1:5000"
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
        try:
            with request.urlopen(req, timeout=15) as response:
                raw = response.read().decode("utf-8")
                return json.loads(raw) if raw else {}
        except error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            try:
                details = json.loads(raw)
                if "code" in details and "message" in details:
                    raise ApiError(f"{details['code']}: {details['message']}") from exc
                if "error" in details:
                    err = details["error"]
                    raise ApiError(f"{err.get('code', exc.code)}: {err.get('message', raw)}") from exc
            except json.JSONDecodeError:
                pass
            raise ApiError(f"HTTP {exc.code}: {raw}") from exc
        except error.URLError as exc:
            raise ApiError(f"Backend is not reachable: {exc.reason}") from exc


def load_config() -> AppConfig:
    if not CONFIG_PATH.exists():
        return AppConfig()
    with CONFIG_PATH.open("r", encoding="utf-8") as file:
        data = json.load(file)
    defaults = asdict(AppConfig())
    defaults.update(data)
    return AppConfig(**defaults)


def save_config(config: AppConfig) -> None:
    APP_DIR.mkdir(parents=True, exist_ok=True)
    with CONFIG_PATH.open("w", encoding="utf-8") as file:
        json.dump(asdict(config), file, indent=2)


def append_gui_log(message: str) -> None:
    APP_DIR.mkdir(parents=True, exist_ok=True)
    with GUI_LOG_PATH.open("a", encoding="utf-8") as file:
        file.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}\n")


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


def backend_for_game(backend_url: str) -> str:
    parsed = parse.urlparse(backend_url)
    if parsed.hostname:
        if parsed.port:
            return f"{parsed.hostname}:{parsed.port}"
        return parsed.hostname
    return backend_url.replace("http://", "").replace("https://", "").rstrip("/")


def sanitize_arg(value: str) -> str:
    return value.replace(" ", "_").replace("\t", "_")


class BrowserApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ProjectRebound Browser - Python Prototype")
        self.geometry("1120x700")
        self.minsize(980, 620)

        self.config_data = load_config()
        self.api = ApiClient()
        self.api.configure(self.config_data.backend_url, self.config_data.access_token)
        self.ui_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.selected_room: dict | None = None
        self.rooms: list[dict] = []

        self._build_ui()
        self.after(100, self._drain_queue)
        self.run_background(self.initialize)

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)

        settings = ttk.LabelFrame(root, text="Settings", padding=8)
        settings.pack(fill=tk.X)

        self.backend_var = tk.StringVar(value=self.config_data.backend_url)
        self.game_dir_var = tk.StringVar(value=self.config_data.game_directory)
        self.display_name_var = tk.StringVar(value=self.config_data.display_name)
        self.region_var = tk.StringVar(value=self.config_data.region)
        self.version_var = tk.StringVar(value=self.config_data.version)

        self._field(settings, "Backend", self.backend_var, 0, 0, width=42)
        self._field(settings, "Display", self.display_name_var, 0, 2, width=20)
        self._field(settings, "Region", self.region_var, 0, 4, width=10)
        self._field(settings, "Version", self.version_var, 0, 6, width=10)
        self._field(settings, "Game Dir", self.game_dir_var, 1, 0, width=64)
        ttk.Button(settings, text="Browse", command=self.browse_game_dir).grid(row=1, column=2, sticky="ew", padx=4, pady=4)
        ttk.Button(settings, text="Save / Login", command=lambda: self.run_background(self.save_and_login)).grid(row=1, column=3, sticky="ew", padx=4, pady=4)

        room_box = ttk.LabelFrame(root, text="Room", padding=8)
        room_box.pack(fill=tk.X, pady=(10, 0))

        self.room_name_var = tk.StringVar(value=self.config_data.room_name)
        self.map_var = tk.StringVar(value=self.config_data.map_name)
        self.mode_var = tk.StringVar(value=self.config_data.mode)
        self.port_var = tk.IntVar(value=self.config_data.port)
        self.max_players_var = tk.IntVar(value=self.config_data.max_players)
        self.use_proxy_var = tk.BooleanVar(value=self.config_data.use_udp_proxy)
        self.proxy_client_port_var = tk.IntVar(value=self.config_data.proxy_client_port)
        self.logic_server_url_var = tk.StringVar(value=self.config_data.logic_server_url)

        self._field(room_box, "Name", self.room_name_var, 0, 0, width=24)
        self._field(room_box, "Map", self.map_var, 0, 2, width=16)
        self._field(room_box, "Mode", self.mode_var, 0, 4, width=10)
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

    def _field(self, parent: ttk.Frame, label: str, variable: tk.Variable, row: int, column: int, width: int) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky="w", padx=(0, 4), pady=4)
        ttk.Entry(parent, textvariable=variable, width=width).grid(row=row, column=column + 1, sticky="ew", padx=(0, 8), pady=4)

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

    def read_form(self) -> AppConfig:
        return AppConfig(
            backend_url=self.backend_var.get().strip() or "http://127.0.0.1:5000",
            game_directory=self.game_dir_var.get().strip(),
            display_name=self.display_name_var.get().strip() or "Player",
            region=self.region_var.get().strip() or "CN",
            version=self.version_var.get().strip() or "dev",
            port=int(self.port_var.get()),
            access_token=self.config_data.access_token,
            room_name=self.room_name_var.get().strip() or "ProjectRebound Room",
            map_name=self.map_var.get().strip() or "Warehouse",
            mode=self.mode_var.get().strip() or "pve",
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
        join = self.api.join_room(self.selected_room["roomId"], self.config_data.version)
        if self.config_data.use_udp_proxy:
            self.start_client_proxy(self.selected_room["roomId"], join["joinTicket"])
            connect = f"127.0.0.1:{self.config_data.proxy_client_port}"
            self.start_client(connect)
            self.set_status(f"Launching client through local proxy {connect}.")
        else:
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
        args = [
            exe,
            f"-LogicServerURL={self.config_data.logic_server_url}",
            f"-match={connect}",
        ]
        append_gui_log("Launching client: " + subprocess.list2cmdline(args))
        subprocess.Popen(args, cwd=str(Path(exe).parent), creationflags=self.client_creation_flags())

    def start_client_proxy(self, room_id: str, join_ticket: str) -> None:
        proxy = Path(__file__).with_name("project_rebound_udp_proxy.py")
        args = [
            sys.executable,
            str(proxy),
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
        subprocess.Popen(args, cwd=str(proxy.parent), creationflags=self.creation_flags())
        time.sleep(1)

    def start_host(self, room: dict, host_token: str) -> None:
        wrapper = find_file(self.config_data.game_directory, "ProjectReboundServerWrapper.exe")
        backend = backend_for_game(self.config_data.backend_url)
        game_port = self.config_data.port + 1 if self.config_data.use_udp_proxy else int(room.get("port", self.config_data.port))
        if self.config_data.use_udp_proxy:
            self.start_host_proxy(room["roomId"], host_token, game_port)
        if wrapper:
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
            append_gui_log("Launching wrapper: " + subprocess.list2cmdline(args))
            subprocess.Popen(args, cwd=str(Path(wrapper).parent), creationflags=self.creation_flags())
            return

        exe = find_file(self.config_data.game_directory, "ProjectBoundarySteam-Win64-Shipping.exe")
        if not exe:
            raise RuntimeError("Neither ProjectReboundServerWrapper.exe nor ProjectBoundarySteam-Win64-Shipping.exe was found.")
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

    def start_host_proxy(self, room_id: str, host_token: str, game_port: int) -> None:
        proxy = Path(__file__).with_name("project_rebound_udp_proxy.py")
        args = [
            sys.executable,
            str(proxy),
            "host",
            "--backend",
            self.config_data.backend_url,
            "--room-id",
            room_id,
            "--host-token",
            host_token,
            "--public-port",
            str(self.config_data.port),
            "--game-port",
            str(game_port),
        ]
        subprocess.Popen(args, cwd=str(proxy.parent), creationflags=self.creation_flags())
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


if __name__ == "__main__":
    app = BrowserApp()
    app.mainloop()
