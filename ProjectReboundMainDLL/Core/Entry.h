#pragma once

// DLL entry point.
// DllMain() spawns MainThread() on DLL_PROCESS_ATTACH.
// MainThread() owns the full server/client initialization sequence.

void MainThread();

// Explicit unloaders must call this from an owner thread before unloading the
// DLL. It must not be called from the command-pipe listener thread.
extern "C" __declspec(dllexport) void ShutdownPayloadCommandFramework();
