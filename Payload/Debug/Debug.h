// Debug.h
#pragma once
#include <string>
#include <fstream>
#include <mutex>

extern std::mutex LogMutex;
extern std::string LogFilePath;
extern bool ClientDebugLogEnabled;
extern std::ofstream clientLogFile;

std::string CurrentTimestamp();
void Log(const std::string &msg);
void ClientLog(const std::string &msg);
void InitDebugConsole();
void EnableUnrealConsole();
void DebugLocateSubsystems();
void DebugDumpSubsystemsToFile();
void DebugDumpWeaponPartsToFile();
void HotkeyThread();
void ClientAutoDumpThread();