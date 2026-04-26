// ======================================================
//  PROJECT BOUNDARY SERVER WRAPPER – PUBLIC INTERFACE
// ======================================================

#pragma once
#include <string>
#include <atomic>

// --- External log callback type ---
using LogCallback = void(*)(const std::string& msg);

// --- Core lifecycle ---
void InitWrapperCore();                     // load config, open log file, init command system
void StartConsoleInput();                   // launch stdin reader thread (CLI only)
void RunWrapperLoop();                      // start server & wait for shutdown signal
void WrapperMain();                         // combined CLI entry: InitCore + Input + RunLoop

//Server control
void LaunchServer();
void RestartServer();
void KillServer();

//Command execution (shared by CLI & GUI)
void ExecuteConsoleCommand(const std::string& line);

//External log hook
void SetExternalLogCallback(LogCallback callback);

//Global shutdown flag (set by UI or signal)
extern std::atomic<bool> g_WrapperShuttingDown;