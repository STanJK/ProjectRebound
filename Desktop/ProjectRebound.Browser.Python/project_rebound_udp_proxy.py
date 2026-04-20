from __future__ import annotations

import argparse
import json
import socket
import threading
import time
from dataclasses import dataclass
from urllib import error, parse, request


MAGIC = "PRB_PUNCH_V1"


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
            raise ApiError(f"HTTP {exc.code}: {raw}") from exc
        except error.URLError as exc:
            raise ApiError(f"Backend is not reachable: {exc.reason}") from exc


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
    api = ApiClient(args.backend)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.public_port))
    sock.settimeout(0.05)

    game_endpoint = ("127.0.0.1", args.game_port)
    peers: dict[tuple[str, int], Peer] = {}
    last_peer: tuple[str, int] | None = None
    stop = threading.Event()

    def poll() -> None:
        while not stop.is_set():
            try:
                query = parse.urlencode({"hostToken": args.host_token})
                result = api.get(f"/v1/rooms/{args.room_id}/punch-tickets?{query}")
                for item in result.get("items", []):
                    endpoint = parse_endpoint(item["clientEndpoint"])
                    peers[endpoint] = Peer(endpoint, item["ticketId"], item["nonce"])
            except Exception as exc:
                print(f"host proxy poll failed: {exc}", flush=True)
            time.sleep(1)

    threading.Thread(target=poll, daemon=True).start()
    print(f"host proxy listening on UDP {args.public_port}, forwarding to {game_endpoint[0]}:{game_endpoint[1]}", flush=True)
    try:
        last_punch = 0.0
        while True:
            now = time.time()
            if now - last_punch > 0.2:
                for peer in list(peers.values()):
                    sock.sendto(punch_packet(peer.ticket_id, peer.nonce, "host"), peer.endpoint)
                last_punch = now

            try:
                data, source = sock.recvfrom(65535)
            except socket.timeout:
                continue

            if is_punch_packet(data):
                if source in peers:
                    last_peer = source
                continue

            if source[0] in {"127.0.0.1", "::1"} or source[0].startswith("127."):
                targets = [last_peer] if last_peer else list(peers.keys())
                for target in targets:
                    if target:
                        sock.sendto(data, target)
            else:
                last_peer = source
                peers.setdefault(source, Peer(source, "", ""))
                sock.sendto(data, game_endpoint)
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
    game_endpoint: tuple[str, int] | None = None
    sock.settimeout(0.05)
    print(f"client proxy listening on 127.0.0.1:{args.listen_port}, punching {ticket['hostEndpoint']}", flush=True)

    last_punch = 0.0
    while True:
        now = time.time()
        if now - last_punch > 0.2:
            sock.sendto(punch_packet(ticket_id, nonce, "client"), host_endpoint)
            last_punch = now

        try:
            data, source = sock.recvfrom(65535)
        except socket.timeout:
            continue

        if is_punch_packet(data):
            host_endpoint = source
            continue

        if source[0] in {"127.0.0.1", "::1"} or source[0].startswith("127."):
            game_endpoint = source
            sock.sendto(data, host_endpoint)
        elif game_endpoint is not None:
            sock.sendto(data, game_endpoint)


def main() -> None:
    parser = argparse.ArgumentParser(description="ProjectRebound experimental UDP punch proxy")
    sub = parser.add_subparsers(dest="mode", required=True)

    host = sub.add_parser("host")
    host.add_argument("--backend", required=True)
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

    args = parser.parse_args()
    if args.mode == "host":
        run_host(args)
    else:
        run_client(args)


if __name__ == "__main__":
    main()
