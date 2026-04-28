// Network.h
#pragma once
#include <string>
#include "../Libs/json.hpp"

nlohmann::json BuildServerStatusPayload();
nlohmann::json BuildRoomHeartbeatPayload();
std::string StripHttpScheme(const std::string &backend);
void SendServerStatus(const std::string &backend);
void StartHeartbeatThread();