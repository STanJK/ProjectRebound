from __future__ import annotations

import argparse
import json
import queue
import secrets
import socket
import sys
import threading
import time
from dataclasses import dataclass
from urllib import error, parse, request


PUNCH_MAGIC = "PRB_PUNCH_V1"
RELAY_REGISTER = "PRB_RELAY_REGISTER_V1"
RELAY_REGISTERED = "PRB_RELAY_REGISTERED_V1"
TEST_MAGIC = "PRB_NAT_TEST_V1"


class ApiError(RuntimeError):
    pass


class ApiClient:
    def __init__(self, backend: str, access_token: str = "") -> None:
        self.backend = backend.rstrip("/")
        self.access_token = access_token

    def with_token(self, access_token: str) -> "ApiClient":
        return ApiClient(self.backend, access_token)

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


@dataclass(frozen=True)
class HostRoom:
    room_id: str
    host_token: str
    endpoint: str


@dataclass
class Peer:
    endpoint: tuple[str, int]
    ticket_id: str
    nonce: str


def log(message: str) -> None:
    print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}", flush=True)


def backend_host(backend: str) -> str:
    parsed = parse.urlparse(backend)
    if not parsed.hostname:
        raise ApiError(f"Invalid backend URL: {backend}")
    return parsed.hostname


def parse_endpoint(endpoint: str) -> tuple[str, int]:
    host, port = endpoint.rsplit(":", 1)
    return host, int(port)


def json_packet(payload: dict) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def decode_json_packet(data: bytes) -> dict | None:
    if not data.startswith(b"{"):
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def punch_packet(ticket_id: str, nonce: str, role: str) -> bytes:
    return json_packet({"type": PUNCH_MAGIC, "ticketId": ticket_id, "nonce": nonce, "role": role})


def test_ping(ticket_id: str, nonce: str, sequence: int) -> bytes:
    return json_packet(
        {
            "type": TEST_MAGIC,
            "kind": "ping",
            "ticketId": ticket_id,
            "nonce": nonce,
            "sequence": sequence,
            "sentAt": time.time(),
        }
    )


def test_pong(packet: dict) -> bytes:
    return json_packet(
        {
            "type": TEST_MAGIC,
            "kind": "pong",
            "ticketId": packet.get("ticketId"),
            "nonce": packet.get("nonce"),
            "sequence": packet.get("sequence"),
            "sentAt": packet.get("sentAt"),
            "receivedAt": time.time(),
        }
    )


def relay_register_packet(session_id: str, role: str, secret: str) -> bytes:
    return json_packet({"type": RELAY_REGISTER, "sessionId": session_id, "role": role, "secret": secret})


def login_guest(api: ApiClient, display_name: str) -> ApiClient:
    try:
        health = api.get("/health")
        log(f"backend health ok: {health}")
    except Exception as exc:
        raise ApiError(f"backend health check failed for {api.backend}: {exc}") from exc

    response = api.post(
        "/v1/auth/guest",
        {
            "displayName": display_name,
            "deviceToken": f"nat-test-{display_name}-{secrets.token_urlsafe(8)}",
        },
    )
    log(f"logged in as {response.get('displayName')} playerId={response.get('playerId')}")
    return api.with_token(response["accessToken"])


def register_binding(api: ApiClient, sock: socket.socket, local_port: int, role: str, room_id: str | None = None) -> dict:
    created = api.post("/v1/nat/bindings", {"localPort": local_port, "role": role, "roomId": room_id})
    udp_host = created.get("udpHost") or backend_host(api.backend)
    server = (udp_host, int(created["udpPort"]))
    payload = json_packet({"type": "nat-binding", "token": created["bindingToken"], "localPort": local_port})

    sock.settimeout(1)
    deadline = time.time() + 8
    while time.time() < deadline:
        sock.sendto(payload, server)
        try:
            data, source = sock.recvfrom(2048)
        except socket.timeout:
            continue
        packet = decode_json_packet(data)
        if packet and packet.get("token") == created["bindingToken"]:
            log(f"{role} rendezvous observed by {server[0]}:{server[1]} from {source[0]}:{source[1]}")
            break
    else:
        raise ApiError(f"{role} UDP rendezvous timed out. Sent to {server[0]}:{server[1]}.")

    confirmed = api.post(f"/v1/nat/bindings/{created['bindingToken']}/confirm")
    log(f"{role} public endpoint = {confirmed['publicIp']}:{confirmed['publicPort']}")
    return confirmed


def heartbeat_loop(api: ApiClient, room_id: str, host_token: str, stop_event: threading.Event) -> None:
    while not stop_event.is_set():
        try:
            response = api.post(
                f"/v1/rooms/{room_id}/heartbeat",
                {"hostToken": host_token, "playerCount": 1, "serverState": "NatPunchTest"},
            )
            delay = max(2, min(int(response.get("nextHeartbeatSeconds", 5)), 10))
        except Exception as exc:
            log(f"heartbeat failed: {exc}")
            delay = 5
        stop_event.wait(delay)


def create_host_room(args: argparse.Namespace, api: ApiClient, binding_token: str) -> HostRoom:
    created = api.post(
        "/v1/rooms",
        {
            "probeId": None,
            "bindingToken": binding_token,
            "name": args.name,
            "region": args.region,
            "map": args.map,
            "mode": args.mode,
            "version": args.version,
            "maxPlayers": args.max_players,
        },
    )
    room = api.get(f"/v1/rooms/{created['roomId']}")
    log(f"room created: roomId={created['roomId']} endpoint={room['endpoint']}")
    print(f"ROOM_ID={created['roomId']}", flush=True)
    print(f"HOST_ENDPOINT={room['endpoint']}", flush=True)
    return HostRoom(created["roomId"], created["hostToken"], room["endpoint"])


def create_relay_allocation(
    api: ApiClient,
    room_id: str,
    role: str,
    host_token: str | None = None,
    join_ticket: str | None = None,
) -> dict:
    return api.post(
        "/v1/relay/allocations",
        {"roomId": room_id, "role": role, "hostToken": host_token, "joinTicket": join_ticket},
    )


def relay_endpoint(api: ApiClient, allocation: dict) -> tuple[str, int]:
    return (allocation.get("relayHost") or backend_host(api.backend), int(allocation["relayPort"]))


def run_host(args: argparse.Namespace, ready_queue: queue.Queue[HostRoom] | None = None, stop_event: threading.Event | None = None) -> int:
    stop = stop_event or threading.Event()
    api = login_guest(ApiClient(args.backend), args.display_name)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    sock.settimeout(0.05)

    binding = register_binding(api, sock, args.port, "host")
    room = create_host_room(args, api, binding["bindingToken"])
    if ready_queue is not None:
        ready_queue.put(room)

    threading.Thread(target=heartbeat_loop, args=(api, room.room_id, room.host_token, stop), daemon=True).start()
    relay_target: tuple[str, int] | None = None
    relay_packet = b""
    relay_registered = False
    if args.relay:
        relay = create_relay_allocation(api, room.room_id, "host", host_token=room.host_token)
        relay_target = relay_endpoint(api, relay)
        relay_packet = relay_register_packet(relay["sessionId"], "host", relay["secret"])
        log(f"host relay allocation: relay={relay_target[0]}:{relay_target[1]}")

    peers: dict[tuple[str, int], Peer] = {}
    nonce_to_peer: dict[str, Peer] = {}

    def poll_tickets() -> None:
        while not stop.is_set():
            try:
                query = parse.urlencode({"hostToken": room.host_token})
                result = api.get(f"/v1/rooms/{room.room_id}/punch-tickets?{query}")
                for item in result.get("items", []):
                    endpoint = parse_endpoint(item["clientEndpoint"])
                    peer = Peer(endpoint, item["ticketId"], item["nonce"])
                    peers[endpoint] = peer
                    nonce_to_peer[peer.nonce] = peer
            except Exception as exc:
                log(f"host ticket poll failed: {exc}")
            stop.wait(1)

    threading.Thread(target=poll_tickets, daemon=True).start()
    log(f"host listening on UDP {args.port}; waiting for punch tickets")

    started = time.time()
    last_punch = 0.0
    last_relay_register = 0.0
    last_stats = time.time()
    punch_rx = 0
    relay_rx = 0
    ping_rx = 0
    pong_tx = 0
    try:
        while not stop.is_set():
            if args.timeout and time.time() - started > args.timeout:
                log("host timeout reached")
                break

            now = time.time()
            if now - last_punch > 0.2:
                for peer in list(peers.values()):
                    sock.sendto(punch_packet(peer.ticket_id, peer.nonce, "host"), peer.endpoint)
                last_punch = now

            if relay_target is not None and now - last_relay_register > 2:
                sock.sendto(relay_packet, relay_target)
                last_relay_register = now

            try:
                data, source = sock.recvfrom(65535)
            except socket.timeout:
                if time.time() - last_stats > 5:
                    log(
                        f"host stats: peers={len(peers)} punch_rx={punch_rx} relay_registered={relay_registered} "
                        f"relay_rx={relay_rx} ping_rx={ping_rx} pong_tx={pong_tx}"
                    )
                    last_stats = time.time()
                continue

            packet = decode_json_packet(data)
            if not packet:
                continue

            if packet.get("type") == RELAY_REGISTERED:
                ok = bool(packet.get("ok"))
                if ok != relay_registered:
                    observed = ""
                    if ok and packet.get("observedIp") and packet.get("observedPort"):
                        observed = f" observed={packet['observedIp']}:{packet['observedPort']}"
                    log(f"host relay registration {'accepted' if ok else 'rejected'}{observed}")
                relay_registered = ok
                continue

            if packet.get("type") == PUNCH_MAGIC:
                punch_rx += 1
                nonce = str(packet.get("nonce", ""))
                peer = nonce_to_peer.get(nonce)
                if peer is not None and peer.endpoint != source:
                    peers.pop(peer.endpoint, None)
                    peer.endpoint = source
                    peers[source] = peer
                continue

            if packet.get("type") == TEST_MAGIC and packet.get("kind") == "ping":
                if relay_target is not None and source == relay_target:
                    relay_rx += 1
                ping_rx += 1
                sock.sendto(test_pong(packet), source)
                pong_tx += 1
                log(f"host echoed ping sequence={packet.get('sequence')} to {source[0]}:{source[1]}")
    finally:
        stop.set()
        sock.close()

    return 0


def run_client(args: argparse.Namespace) -> int:
    api = login_guest(ApiClient(args.backend), args.display_name)

    join = api.post(f"/v1/rooms/{args.room_id}/join", {"version": args.version})
    log(f"join reserved: connect={join['connect']}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    sock.settimeout(0.05)

    binding = register_binding(api, sock, args.port, "client", args.room_id)
    ticket = api.post(
        f"/v1/rooms/{args.room_id}/punch-tickets",
        {
            "joinTicket": join["joinTicket"],
            "bindingToken": binding["bindingToken"],
            "clientLocalEndpoint": f"127.0.0.1:{args.port}",
        },
    )

    host_endpoint = parse_endpoint(ticket["hostEndpoint"])
    ticket_id = ticket["ticketId"]
    nonce = ticket["nonce"]
    relay_target: tuple[str, int] | None = None
    relay_packet = b""
    relay_registered = False
    if args.relay:
        relay = create_relay_allocation(api, args.room_id, "client", join_ticket=join["joinTicket"])
        relay_target = relay_endpoint(api, relay)
        relay_packet = relay_register_packet(relay["sessionId"], "client", relay["secret"])
        log(f"client relay allocation: relay={relay_target[0]}:{relay_target[1]}")
    log(f"punch ticket created: ticketId={ticket_id} hostEndpoint={ticket['hostEndpoint']}")

    started = time.time()
    last_punch = 0.0
    last_ping = 0.0
    last_relay_register = 0.0
    last_stats = time.time()
    sequence = 0
    punch_rx = 0
    relay_rx = 0
    ping_tx = 0
    while time.time() - started <= args.timeout:
        now = time.time()
        if now - last_punch > 0.2:
            sock.sendto(punch_packet(ticket_id, nonce, "client"), host_endpoint)
            last_punch = now

        if relay_target is not None and now - last_relay_register > 2:
            sock.sendto(relay_packet, relay_target)
            last_relay_register = now

        if now - last_ping > 0.5 and (relay_target is None or relay_registered):
            sequence += 1
            payload = test_ping(ticket_id, nonce, sequence)
            if relay_target is not None:
                sock.sendto(payload, relay_target)
            else:
                sock.sendto(payload, host_endpoint)
            ping_tx += 1
            last_ping = now

        if now - last_stats > 5:
            log(
                f"client stats: host={host_endpoint[0]}:{host_endpoint[1]} "
                f"punch_rx={punch_rx} relay_registered={relay_registered} relay_rx={relay_rx} ping_tx={ping_tx}"
            )
            last_stats = now

        try:
            data, source = sock.recvfrom(65535)
        except socket.timeout:
            continue

        packet = decode_json_packet(data)
        if not packet:
            continue

        if packet.get("type") == RELAY_REGISTERED:
            ok = bool(packet.get("ok"))
            if ok != relay_registered:
                observed = ""
                if ok and packet.get("observedIp") and packet.get("observedPort"):
                    observed = f" observed={packet['observedIp']}:{packet['observedPort']}"
                log(f"client relay registration {'accepted' if ok else 'rejected'}{observed}")
            relay_registered = ok
            continue

        if packet.get("type") == PUNCH_MAGIC:
            punch_rx += 1
            host_endpoint = source
            continue

        if packet.get("type") == TEST_MAGIC and packet.get("kind") == "pong":
            if relay_target is not None and source == relay_target:
                relay_rx += 1
            elapsed_ms = int((time.time() - float(packet.get("sentAt", time.time()))) * 1000)
            log(
                f"PASS: received pong sequence={packet.get('sequence')} "
                f"from {source[0]}:{source[1]} after {elapsed_ms}ms "
                f"(punch_rx={punch_rx}, relay_rx={relay_rx}, ping_tx={ping_tx})"
            )
            try:
                api.post(f"/v1/rooms/{args.room_id}/punch-tickets/{ticket_id}/complete", {})
            except Exception as exc:
                log(f"ticket complete call failed: {exc}")
            sock.close()
            return 0

    sock.close()
    log(
        f"FAIL: no pong within {args.timeout}s (punch_rx={punch_rx}, relay_registered={relay_registered}, "
        f"relay_rx={relay_rx}, ping_tx={ping_tx}). "
        "Backend rendezvous succeeded, but UDP data did not complete."
    )
    return 2


def run_loopback(args: argparse.Namespace) -> int:
    ready: queue.Queue[HostRoom] = queue.Queue()
    stop = threading.Event()
    host_args = argparse.Namespace(**vars(args))
    host_args.port = args.host_port
    host_args.display_name = "NatTestHost"
    host_thread = threading.Thread(target=run_host, args=(host_args, ready, stop), daemon=True)
    host_thread.start()

    try:
        room = ready.get(timeout=15)
    except queue.Empty:
        stop.set()
        log("FAIL: host did not create a room")
        return 2

    client_args = argparse.Namespace(**vars(args))
    client_args.port = args.client_port
    client_args.room_id = room.room_id
    client_args.display_name = "NatTestClient"
    result = run_client(client_args)
    stop.set()
    host_thread.join(timeout=3)
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ProjectRebound NAT punch smoke tests")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(target: argparse.ArgumentParser, default_timeout: int = 60) -> None:
        target.add_argument("--backend", default="http://127.0.0.1:5000")
        target.add_argument("--version", default="dev")
        target.add_argument("--timeout", type=int, default=default_timeout)

    host = sub.add_parser("host", help="Run the host side on machine A")
    add_common(host, default_timeout=0)
    host.add_argument("--port", type=int, default=27777)
    host.add_argument("--display-name", default="NatTestHost")
    host.add_argument("--name", default="Nat Punch Test")
    host.add_argument("--region", default="CN")
    host.add_argument("--map", default="Warehouse")
    host.add_argument("--mode", default="pve")
    host.add_argument("--max-players", type=int, default=8)
    host.add_argument("--relay", action="store_true", help="Register with UDP relay and answer relay pings")

    client = sub.add_parser("client", help="Run the client side on machine B")
    add_common(client)
    client.add_argument("--room-id", required=True)
    client.add_argument("--port", type=int, default=27778)
    client.add_argument("--display-name", default="NatTestClient")
    client.add_argument("--relay", action="store_true", help="Send test ping through UDP relay instead of direct P2P")

    loopback = sub.add_parser("loopback", help="Run host and client in one process for local smoke testing")
    add_common(loopback)
    loopback.add_argument("--host-port", type=int, default=27777)
    loopback.add_argument("--client-port", type=int, default=27778)
    loopback.add_argument("--name", default="Nat Punch Loopback")
    loopback.add_argument("--region", default="CN")
    loopback.add_argument("--map", default="Warehouse")
    loopback.add_argument("--mode", default="pve")
    loopback.add_argument("--max-players", type=int, default=8)
    loopback.add_argument("--relay", action="store_true", help="Send loopback test ping through UDP relay")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        if args.command == "host":
            return run_host(args)
        if args.command == "client":
            return run_client(args)
        if args.command == "loopback":
            return run_loopback(args)
    except KeyboardInterrupt:
        log("interrupted")
        return 130
    except Exception as exc:
        log(f"ERROR: {exc}")
        return 1
    parser.error(f"unknown command: {args.command}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
