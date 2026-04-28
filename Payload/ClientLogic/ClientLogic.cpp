// ClientLogic.cpp
#include "ClientLogic.h"
#include "../Config/Config.h"
#include "../Debug/Debug.h"
#include "../Utility/Utility.h"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include "../Libs/json.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <Windows.h>

using namespace SDK;

bool LoginCompleted = false;
bool ReadyToAutoconnect = false;

// ======================================================
//  SECTION 14 — CLIENT LOGIC
// ======================================================

void InitClientArmory()
{
    for (UObject *obj : getObjectsOfClass(UPBArmoryManager::StaticClass(), false))
    {
        UPBArmoryManager *DefaultConfig = (UPBArmoryManager *)obj;

        std::ifstream items("DT_ItemType.json");
        nlohmann::json itemJson = nlohmann::json::parse(items);

        for (auto &[ItemId, _] : itemJson[0]["Rows"].items())
        {
            std::string aString = std::string(ItemId.c_str());
            std::wstring wString = std::wstring(aString.begin(), aString.end());

            if (DefaultConfig->DefaultConfig)
                DefaultConfig->DefaultConfig->OwnedItems.Add(UKismetStringLibrary::Conv_StringToName(wString.c_str()));

            FPBItem item{};
            item.ID = UKismetStringLibrary::Conv_StringToName(wString.c_str());
            item.Count = 1;
            item.bIsNew = false;

            DefaultConfig->Armorys.OwnedItems.Add(item);
        }
    }
}

void ConnectToMatch()
{
    UPBGameInstance *GameInstance =
        (UPBGameInstance *)UWorld::GetWorld()->OwningGameInstance;

    GameInstance->ShowLoadingScreen(false, true);

    UPBLocalPlayer *LocalPlayer =
        (UPBLocalPlayer *)(UWorld::GetWorld()->OwningGameInstance->LocalPlayers[0]);

    LocalPlayer->GoToRange(0.0f);

    UKismetSystemLibrary::ExecuteConsoleCommand(
        UWorld::GetWorld(), L"travel 127.0.0.1", nullptr);

    GameInstance->ShowLoadingScreen(true, true);
}

void AutoConnectToMatchFromCmdline()
{
    std::thread([]()
                {
                    // Wait for world
                    while (!UWorld::GetWorld())
                        Sleep(100);

                    // Wait for GameInstance
                    while (!UWorld::GetWorld()->OwningGameInstance)
                        Sleep(100);

                    // Wait for LocalPlayer
                    while (UWorld::GetWorld()->OwningGameInstance->LocalPlayers.Num() == 0)
                        Sleep(100);

                    // Wait for login complete
                    while (!LoginCompleted)
                        Sleep(100);

                    // Delay to avoid main menu overriding the range transition
                    Sleep(2000);

                    // Enter Shooting Range
                    auto *GI = UWorld::GetWorld()->OwningGameInstance;
                    UPBLocalPlayer *LP = (UPBLocalPlayer *)GI->LocalPlayers[0];

                    if (LP)
                    {
                        ClientLog("[CLIENT] Auto-enter Shooting Range...");
                        LP->GoToRange(0.0f);
                    }

                    // Give travel a moment to initialize
                    Sleep(1000);

                    ReadyToAutoconnect = true;

                    // Wait for flag
                    while (!ReadyToAutoconnect)
                        Sleep(100);

                    Sleep(200);

                    // Connect to match
                    std::wstring wcmd = L"open " + std::wstring(MatchIP.begin(), MatchIP.end());
                    ClientLog("[CLIENT] Auto-connecting to match: " + MatchIP);

                    UKismetSystemLibrary::ExecuteConsoleCommand(
                        UWorld::GetWorld(),
                        wcmd.c_str(),
                        nullptr);
                })
        .detach();
}