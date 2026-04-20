# ProjectRebound Python Browser Prototype

This is a dependency-free Python 3.11 + tkinter prototype for the room browser.

## Run

```powershell
python Desktop\ProjectRebound.Browser.Python\project_rebound_browser.py
```

or double-click:

```text
Desktop\ProjectRebound.Browser.Python\run_browser.bat
```

## Notes

- Start the backend first: `dotnet run --project Backend\ProjectRebound.MatchServer\ProjectRebound.MatchServer.csproj`
- Config is saved to `%APPDATA%\ProjectReboundBrowser\config-python.json`.
- Create Room and Quick Match require the selected UDP port to be reachable from the backend.
- Join launches the game with `-LogicServerURL=http://127.0.0.1:8000 -match=ip:port` by default. `Logic URL` can be changed in the GUI.
- Client game launch intentionally does not use `CREATE_NEW_CONSOLE`; proxy and wrapper still open their own consoles for logs.
- Create Room launches `ProjectReboundServerWrapper.exe` when it can find it under the configured game directory.
- This Python prototype is the current recommended GUI path; the WPF prototype remains in the repo but is not the active target.

## Experimental UDP Proxy

`project_rebound_udp_proxy.py` is an experimental local UDP punch proxy.

When `Use UDP Proxy` is checked in the GUI:

- Host side keeps the public room port for the proxy, for example `7777`.
- Host game/server wrapper is launched on `Port + 1`, for example `7778`.
- Client side starts a local proxy on `Client Proxy`, for example `17777`.
- Client game is launched with `-match=127.0.0.1:17777`.
- The backend only exchanges NAT binding and punch ticket metadata; it does not relay game packets.

The backend must expose UDP rendezvous port `5001/udp` when this mode is used.

This mode is a prototype. It can help with cone / port-restricted NAT cases, but it does not guarantee symmetric NAT or strict CGNAT traversal.
