# ProjectRebound NAT Punch Test Scripts

These scripts test the match server's UDP rendezvous and punch-ticket flow without
starting the game.

They use only Python's standard library.

## Local smoke test

Run this on one machine against a running match server:

```powershell
Tools\NatPunchTest\run-loopback.bat --backend http://127.0.0.1:5000
```

A passing run ends with:

```text
PASS: received pong ...
```

This validates the HTTP API, UDP rendezvous port, room creation, room join, punch
ticket creation, and UDP packet exchange in a local environment. It does not prove
that two different NATs can reach each other.

## Two-machine test

On machine A:

```powershell
Tools\NatPunchTest\run-host.bat --backend http://YOUR_SERVER:5000 --port 27777
```

Keep the host process open. By default, the host side does not time out. It
prints a room id:

```text
ROOM_ID=...
```

On machine B:

```powershell
Tools\NatPunchTest\run-client.bat --backend http://YOUR_SERVER:5000 --room-id ROOM_ID_FROM_A --port 27778
```

A passing client run prints:

```text
PASS: received pong ...
```

## Relay fallback test

Use `--relay` on both sides to test the server UDP relay path instead of direct
peer UDP:

Machine A:

```powershell
Tools\NatPunchTest\run-host.bat --backend http://YOUR_SERVER --port 27777 --relay
```

Machine B:

```powershell
Tools\NatPunchTest\run-client.bat --backend http://YOUR_SERVER --room-id ROOM_ID_FROM_A --port 27778 --relay
```

This requires UDP `5002` to be open on the Debian server and cloud security
group. A passing run still ends with:

```text
PASS: received pong ...
```

Verified example:

```text
client relay registration accepted observed=...
PASS: received pong sequence=1 from 43.240.193.246:5002 ...
```

## Expected failure meanings

- `UDP rendezvous timed out`: UDP `5001` did not round-trip between the test
  machine and the match server. Check Debian firewall, cloud security group, and
  Windows outbound firewall.
- `HTTP 409 ... NAT_BINDING_NOT_READY`: the backend did not observe the UDP
  rendezvous packet for that binding.
- `FAIL: no pong`: HTTP rendezvous and punch-ticket creation worked, but direct
  peer UDP did not complete. This can mean symmetric NAT, carrier NAT, firewall,
  or endpoint-dependent filtering.
- `--relay` still fails with `FAIL: no pong`: UDP `5002` is blocked, the relay
  service is not running, or one side did not register with the relay.

## Useful server checks

```bash
sudo ss -lunp | grep 5001
sudo ss -lunp | grep 5002
sudo journalctl -u projectrebound-matchserver -f
sudo tcpdump -ni any "udp port 5001 or udp port 5002"
```
