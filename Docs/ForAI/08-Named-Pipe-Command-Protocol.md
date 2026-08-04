# ProjectRebound named-pipe command protocol

English | [简体中文](command-framework.zh-CN.md)

`ExternalCommandPipe` is the local command channel between the launcher and the
in-process game Payload. The Payload is the single-instance pipe server. The launcher
creates a high-entropy pipe name for one game run, passes it through the Windows Wrapper
as `-pipe=<name>`, and connects to `\\.\pipe\<name>` as the same Windows user and in the
same session.

The implementation is split into three boundaries:

- `ProjectReboundMainDLL/API/CommandProtocol.*`: portable framing, parsing, encoding, and
  match-target validation;
- `ProjectReboundMainDLL/API/ExternalCommandPipe.*`: Windows named-pipe security, Overlapped
  I/O, lifecycle, dispatch, and responses;
- `ProjectReboundMainDLL/Client/AutoConnect.*` plus `ProjectReboundMainDLL/Hooking/ClientHooks.cpp`: thread-safe join
  queue and game-thread execution.

See the [function/structure/code analysis guide](command-framework-code-analysis.md)
for ownership, lock rules, call flows, residual constraints, and the full test matrix.

## Transport and frame format

The server uses a message-mode, duplex named pipe with one active instance. Every
application message is one UTF-8 line:

```text
<command>\t<json-object>\n
```

Rules:

- the delimiter is one Tab and the terminator is LF; CRLF input is accepted;
- the command is 1–32 bytes and contains only lowercase ASCII letters, digits, `_`, or
  `-`;
- the JSON payload is required and must be an object;
- the complete wire frame, including LF, is at most 64 KiB;
- an optional `request_id` must be a non-empty string of at most 128 bytes; every
  response to that request echoes it;
- malformed input produces an `error` response. Three protocol errors on one
  connection cause the server to disconnect that client.

Example exchange:

```text
ping\t{"request_id":"health-17"}\n
pong\t{"request_id":"health-17"}\n
```

## Commands

| Request | Required fields | Success response | Meaning |
| --- | --- | --- | --- |
| `ping` | none | `pong` | Checks that the channel is live |
| `join` | `ip`: string | `join_ack` | Accepts a game-thread match transition |
| `debug` | callback-defined | `debug_ack` | Runs the registered Payload debug handler |
| `server_status` | none | `server_status_ack` | Reads non-secret Dedicated Server runtime state for launcher-owned signed heartbeats |

`server_status` is available only when the embedding entry point registers a
`ServerStatusCallback`. Its response convention is `state`, non-negative
`player_count`, and diagnostic `round_state`. The current StanJK client entry point does
not register this optional callback, so it returns `unavailable`. Registration tokens,
private keys, runtime tokens, certificates, CSRs, and signatures are prohibited from
this request and response.

`join.ip` is the complete target, not only an IP address. Accepted forms are
`host:port`, `IPv4:port`, and `[IPv6]:port`; the port range is 1–65535 and the complete
target is at most 512 bytes. Whitespace, control characters, console separators, and
unbracketed IPv6 are rejected before an Unreal console command is built.

`join.token` is optional and limited to 4096 bytes. It is currently reserved: the
Payload passes it to the callback but does not authenticate it.

A successful acknowledgement means that the target was validated and queued:

```text
join_ack\t{"request_id":"join-42","status":"accepted"}\n
```

It does **not** mean that the remote game connection has completed. If another
transition is pending, the server returns an `error` with code `busy` instead of a
false-positive acknowledgement.

The debug callback may return any JSON value. Non-object results are wrapped as
`{"result": ...}` so every response still obeys the object-payload rule.

## Error response

Errors use a stable shape:

```text
error\t{"code":"invalid_target","message":"port must be between 1 and 65535","request_id":"join-42"}\n
```

Current codes include `empty_frame`, `frame_too_large`, `missing_delimiter`,
`invalid_command`, `missing_json`, `invalid_json`, `invalid_json_type`,
`invalid_request_id`, `invalid_request`, `invalid_target`, `unknown_command`,
`unavailable`, `busy`, and `internal_error`.

Clients must branch on `code`, not on the human-readable `message`.

## Lifecycle, concurrency, and timeouts

- `Start()` validates the name, builds the current-user ACL, creates a manual-reset stop
  event, and starts one listener thread.
- Connect, read, and write operations use event-backed `OVERLAPPED` structures. A
  canceled or timed-out operation is always drained with `GetOverlappedResult` before
  its event, buffer, or stack storage is released.
- The default idle read timeout is 30 seconds and the default write timeout is 5
  seconds. A configured value of zero means no timeout.
- `SendResponse()` is serialized and may be called from other threads.
- `Stop()` signals the stop event, cancels current I/O, joins deterministically, and
  never detaches a thread that captures the framework object.
- Disconnect, timeout, and recoverable transport errors return the listener to a fresh
  `CreateNamedPipeW`/connect cycle.

The `join` callback runs on the listener thread but performs no Unreal work. It only
validates/queues a target. The client `ProcessEvent` hook records the game-thread ID at
login and pumps the queued transition only on that exact thread, with the existing
login/range settle delays.

## Security boundary

The pipe is created with:

- a DACL granting read/write only to the Payload process user's SID;
- `PIPE_REJECT_REMOTE_CLIENTS`;
- `FILE_FLAG_FIRST_PIPE_INSTANCE` to fail closed on a pre-existing name;
- a post-connect check that the client PID belongs to the same Windows session and the
  same user SID;
- non-inheritable handles and a restricted ASCII pipe-name grammar.

This protects against other local users and remote clients, but it is not cryptographic
launcher attestation. Another process running as the same user in the same session can
still connect if it learns the pipe name. Launchers should therefore generate an
unguessable per-run name, avoid sending long-lived secrets, and treat every payload
field as untrusted.

## Shutdown and verification

Process termination lets Windows reclaim the process-lifetime framework. A host that
explicitly unloads the DLL must first call the exported
`ShutdownPayloadCommandFramework()` function from a normal thread, never from
`DllMain` or a command callback.

CI runs portable protocol tests on Ubuntu and real named-pipe integration tests on
Windows. The same Windows suite can be run locally:

```powershell
cmake -S ProjectReboundMainDLL/Tests -B build/command-pipe-tests
cmake --build build/command-pipe-tests --config Release
ctest --test-dir build/command-pipe-tests -C Release --output-on-failure
```

When changing the contract, update the protocol implementation, every consuming
launcher, both language versions of this document, and the tests in the same change.
