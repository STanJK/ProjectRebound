#pragma once

#include <string>

void InitClientArmory();

// Producers may call this from any thread. Unreal calls are performed later by
// PumpPendingClientCommands on the ProcessEvent game thread.
[[nodiscard]] bool QueueConnectToMatch(const std::string& target);
void ConnectToMatch();
void AutoConnectToMatchFromCmdline();
[[nodiscard]] bool NotifyClientLoginCompleted();
void PumpPendingClientCommands();
