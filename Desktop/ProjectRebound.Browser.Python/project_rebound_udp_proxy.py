from __future__ import annotations

import argparse
import json
import os
import socket
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from urllib import error, parse, request


MAGIC = "PRB_PUNCH_V1"
RELAY_REGISTER = "PRB_RELAY_REGISTER_V1"
RELAY_REGISTERED = "PRB_RELAY_REGISTERED_V1"
LOG_PATH: Path | None = None


def setup_log(role: str) -> None:
    global LOG_PATH
    app_dir = Path(os.environ.get("APPDATA", str(Path.home()))) / "ProjectReboundBrowser"
    app_dir.mkdir(parents=True, exist_ok=True)
    LOG_PATH = app_dir / f"udp-proxy-{role}.log"
    LOG_PATH.write_text("", encoding="utf-8")


def log(message: str) -> None:
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"{stamp} {message}"
    print(line, flush=True)
    if LOG_PATH is not None:
        with LOG_PATH.open("a", encoding="utf-8") as file:
            file.write(line + "\n")


class ApiError(RuntimeError):
    pass


class ApiClient:
    def __init__(self, backend: str, access_token: str = "") -> None:
        self.backend = backend.rstrip("/")
        self.access_token = access_token

    def get(self, path: str) -> dict:
        return self._send("GET", path)

    def post(self, path: str, payload: dict | None = None) -> dict:
        return self._send("POST", path, payload)

    def _send(self, method: str, path: str, payload: dict | None = None) -> dict:
        body = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.access_token:
            headers["Authorization"] = f"Bearer {self.access_token}"

        req = request.Request(f"{self.backend}{path}", data=body, headers=headers, method=method)
        try:
            with request.urlopen(req, timeout=10) as response:
                raw = response.read().decode("utf-8")
                return json.loads(raw) if raw else {}
        except error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            raise ApiError(f"HTTP {exc.code} during {method} {path}: {raw}") from exc
        except error.URLError as exc:
            raise ApiError(f"Backend is not reachable during {method} {path}: {exc.reason}") from exc


def parse_endpoint(endpoint: str) -> tuple[str, int]:
    host, port = endpoint.rsplit(":", 1)
    return host, int(port)


def backend_host(backend: str) -> str:
    parsed = parse.urlparse(backend)
    if not parsed.hostname:
        raise ApiError(f"Invalid backend URL: {backend}")
    return parsed.hostname


def punch_packet(ticket_id: str, nonce: str, role: str) -> bytes:
    return json.dumps({"type": MAGIC, "ticketId": ticket_id, "nonce": nonce, "role": role}).encode("utf-8")


def is_punch_packet(data: bytes) -> bool:
    if not data.startswith(b"{"):
        return False
    try:
        return json.loads(data.decode("utf-8")).get("type") == MAGIC
    except (UnicodeDecodeError, json.JSONDecodeError):
        return False


def parse_json_packet(data: bytes) -> dict | None:
    if not data.startswith(b"{"):
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def relay_register_packet(session_id: str, role: str, secret: str) -> bytes:
    return json.dumps(
        {"type": RELAY_REGISTER, "sessionId": session_id, "role": role, "secret": secret}
    ).encode("utf-8")


def create_relay_allocation(
    api: ApiClient,
    role: str,
    room_id: str,
    host_token: str | None = None,
    join_ticket: str | None = None,
) -> dict:
    return api.post(
        "/v1/relay/allocations",
        {"roomId": room_id, "role": role, "hostToken": host_token, "joinTicket": join_ticket},
    )


def register_binding(api: ApiClient, sock: socket.socket, local_port: int, role: str, room_id: str | None = None) -> dict:
    created = api.post("/v1/nat/bindings", {"localPort": local_port, "role": role, "roomId": room_id})
    server = (created.get("udpHost") or backend_host(api.backend), int(created["udpPort"]))
    packet = json.dumps({"type": "nat-binding", "token": created["bindingToken"], "localPort": local_port}).encode("utf-8")
    sock.settimeout(1)
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
            break
    else:
        raise ApiError(f"UDP rendezvous timed out. Sent to {server[0]}:{server[1]}.")

    return api.post(f"/v1/nat/bindings/{created['bindingToken']}/confirm")


@dataclass
class Peer:
    endpoint: tuple[str, int]
    ticket_id: str
    nonce: str


def run_host(args: argparse.Namespace) -> None:
    api = ApiClient(args.backend, args.access_token)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.public_port))
    sock.settimeout(0.05)

    game_endpoint = ("127.0.0.1", args.game_port)
    peers: dict[tuple[str, int], Peer] = {}
    last_peer: tuple[str, int] | None = None
    stop = threading.Event()
    relay = create_relay_allocation(api, "host", args.room_id, host_token=args.host_token)
    relay_endpoint = (relay.get("relayHost") or backend_host(args.backend), int(relay["relayPort"]))
    relay_packet = relay_register_packet(relay["sessionId"], "host", relay["secret"])
    relay_registered = False
    log(f"host relay allocation received: session={relay['sessionId']} relay={relay_endpoint[0]}:{relay_endpoint[1]}")

    def poll() -> None:
        while not stop.is_set():
            try:
                query = parse.urlencode({"hostToken": args.host_token})
                result = api.get(f"/v1/rooms/{args.room_id}/punch-tickets?{query}")
                for item in result.get("items", []):
                    endpoint = parse_endpoint(item["clientEndpoint"])
                    peers[endpoint] = Peer(endpoint, item["ticketId"], item["nonce"])
            except Exception as exc:
                log(f"host proxy poll failed: {exc}")
            time.sleep(1)

    threading.Thread(target=poll, daemon=True).start()
    log(
        f"host proxy listening on UDP {args.public_port}, forwarding to {game_endpoint[0]}:{game_endpoint[1]}, "
        f"relay={relay_endpoint[0]}:{relay_endpoint[1]}"
    )
    try:
        last_punch = 0.0
        last_relay_register = 0.0
        last_stats = time.time()
        punch_rx = 0
        relay_rx = 0
        game_rx = 0
        game_tx = 0
        while True:
            now = time.time()
            if now - last_punch > 0.2:
                for peer in list(peers.values()):
                    sock.sendto(punch_packet(peer.ticket_id, peer.nonce, "host"), peer.endpoint)
                last_punch = now

            if now - last_relay_register > 2:
                sock.sendto(relay_packet, relay_endpoint)
                last_relay_register = now

            try:
                data, source = sock.recvfrom(65535)
            except socket.timeout:
                if time.time() - last_stats > 5:
                    log(
                        f"host proxy stats: peers={len(peers)} punch_rx={punch_rx} relay_registered={relay_registered} "
                        f"relay_rx={relay_rx} game_from_peer={game_rx} game_to_peer={game_tx}"
                    )
                    last_stats = time.time()
                continue

            packet = parse_json_packet(data)
            if packet and packet.get("type") == RELAY_REGISTERED:
                ok = bool(packet.get("ok"))
                if ok != relay_registered:
                    log(f"host relay registration {'accepted' if ok else 'rejected'}")
                relay_registered = ok
                continue

            if is_punch_packet(data):
                punch_rx += 1
                if source in peers:
                    last_peer = source
                continue

            if source[0] in {"127.0.0.1", "::1"} or source[0].startswith("127."):
                targets = [last_peer] if last_peer else list(peers.keys())
                for target in targets:
                    if target:
                        sock.sendto(data, target)
                        game_tx += 1
            else:
                if source == relay_endpoint:
                    relay_rx += 1
                    last_peer = source
                else:
                    last_peer = source
                    peers.setdefault(source, Peer(source, "", ""))
                sock.sendto(data, game_endpoint)
                game_rx += 1
    finally:
        stop.set()
        sock.close()


def run_client(args: argparse.Namespace) -> None:
    api = ApiClient(args.backend, args.access_token)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.listen_port))
    binding = register_binding(api, sock, args.listen_port, "client", args.room_id)
    local_endpoint = f"127.0.0.1:{args.listen_port}"
    ticket = api.post(
        f"/v1/rooms/{args.room_id}/punch-tickets",
        {
            "joinTicket": args.join_ticket,
            "bindingToken": binding["bindingToken"],
            "clientLocalEndpoint": local_endpoint,
        },
    )
    host_endpoint = parse_endpoint(ticket["hostEndpoint"])
    nonce = ticket["nonce"]
    ticket_id = ticket["ticketId"]
    relay = create_relay_allocation(api, "client", args.room_id, join_ticket=args.join_ticket)
    relay_endpoint = (relay.get("relayHost") or backend_host(args.backend), int(relay["relayPort"]))
    relay_packet = relay_register_packet(relay["sessionId"], "client", relay["secret"])
    relay_registered = False
    relay_active = False
    log(f"client relay allocation received: session={relay['sessionId']} relay={relay_endpoint[0]}:{relay_endpoint[1]}")
    game_endpoint: tuple[str, int] | None = None
    sock.settimeout(0.05)
    log(
        f"client proxy listening on 127.0.0.1:{args.listen_port}, punching {ticket['hostEndpoint']}, "
        f"relay={relay_endpoint[0]}:{relay_endpoint[1]}"
    )

    started = time.time()
    last_punch = 0.0
    last_relay_register = 0.0
    last_stats = time.time()
    punch_rx = 0
    relay_rx = 0
    game_from_local = 0
    game_from_host = 0
    while True:
        now = time.time()
        if now - last_punch > 0.2:
            sock.sendto(punch_packet(ticket_id, nonce, "client"), host_endpoint)
            last_punch = now

        if now - last_relay_register > 2:
            sock.sendto(relay_packet, relay_endpoint)
            last_relay_register = now

        if not relay_active and punch_rx == 0 and now - started > args.relay_fallback_after:
            relay_active = True
            log("client proxy enabling UDP relay fallback; no peer punch was received in time")

        try:
            data, source = sock.recvfrom(65535)
        except socket.timeout:
            if time.time() - last_stats > 5:
                log(
                    f"client proxy stats: host={host_endpoint[0]}:{host_endpoint[1]} "
                    f"punch_rx={punch_rx} relay_registered={relay_registered} relay_active={relay_active} "
                    f"relay_rx={relay_rx} game_from_local={game_from_local} game_from_host={game_from_host}"
                )
                last_stats = time.time()
            continue

        packet = parse_json_packet(data)
        if packet and packet.get("type") == RELAY_REGISTERED:
            ok = bool(packet.get("ok"))
            if ok != relay_registered:
                log(f"client relay registration {'accepted' if ok else 'rejected'}")
            relay_registered = ok
            continue

        if is_punch_packet(data):
            if source == relay_endpoint:
                continue
            host_endpoint = source
            punch_rx += 1
            if relay_active:
                relay_active = False
                log("client proxy disabling UDP relay fallback; peer punch is now working")
            continue

        if source[0] in {"127.0.0.1", "::1"} or source[0].startswith("127."):
            game_endpoint = source
            sock.sendto(data, host_endpoint)
            if relay_registered and relay_active:
                sock.sendto(data, relay_endpoint)
            game_from_local += 1
        elif source == relay_endpoint and game_endpoint is not None:
            sock.sendto(data, game_endpoint)
            relay_rx += 1
            game_from_host += 1
        elif game_endpoint is not None:
            sock.sendto(data, game_endpoint)
            game_from_host += 1


def main() -> None:
    parser = argparse.ArgumentParser(description="ProjectRebound experimental UDP punch proxy")
    sub = parser.add_subparsers(dest="mode", required=True)

    host = sub.add_parser("host")
    host.add_argument("--backend", required=True)
    host.add_argument("--access-token", required=True)
    host.add_argument("--room-id", required=True)
    host.add_argument("--host-token", required=True)
    host.add_argument("--public-port", type=int, required=True)
    host.add_argument("--game-port", type=int, required=True)

    client = sub.add_parser("client")
    client.add_argument("--backend", required=True)
    client.add_argument("--access-token", required=True)
    client.add_argument("--room-id", required=True)
    client.add_argument("--join-ticket", required=True)
    client.add_argument("--listen-port", type=int, required=True)
    client.add_argument("--relay-fallback-after", type=float, default=8.0)

    args = parser.parse_args()
    setup_log(args.mode)
    try:
        if args.mode == "host":
            run_host(args)
        else:
            run_client(args)
    except Exception as exc:
        log(f"fatal proxy error: {exc}")
        raise


if __name__ == "__main__":
    main()
