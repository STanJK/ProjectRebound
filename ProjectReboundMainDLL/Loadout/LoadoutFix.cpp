#include "LoadoutFix.h"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include "../SDK/ProjectBoundary_classes.hpp"
#include "../SDK/Engine_classes.hpp"
#include "../Libs/json.hpp"
#include "../Logging/LogManager.h"
#include "../API/APIInternal.h"
#include <string>
#include <sstream>
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace SDK;

// ======================================================
// Equip error swallow + display model refresh
// ======================================================
//
// UpdateRoleArchiveV2 returns {StatusCode:0/1}, metaserver saves correctly,
// but Native broadcasts OnEquipCharacterSlotDelegate(UnknowError=4) and the
// internal listener skips UPBShowRoomManager::SpawnInventory().
//
// We zero ONLY UnknowError (the spurious one) so the BP callbacks show "已装备",
// then directly call SpawnInventory() to refresh the 3D display actor.
// Real errors (QuotaNotEnough, IsLocked, …) are left intact so the game shows
// the correct "insufficient quota" / "locked" toast.

static bool  s_PendingSpawn = false;
static FName s_PendingCharID;
static FName s_PendingItemID;

void HandleEquipErrorSwallow(UObject* Object, UFunction* Function, void* Parms,
                              const std::string& funcName)
{
    if (funcName.find("OnEquipCharacterSlotComplete") != std::string::npos)
    {
        auto* P = static_cast<Params::PBDetailedWeaponDataWidget_OnEquipCharacterSlotComplete*>(Parms);
        if ((int)P->ErrorCode == (int)EPBEquipErrorCode::UnknowError)
        {
            ClientDebugLog("[LOADOUT-FIX] OnEquipSlotComplete"
                      + std::string("  Item=") + P->InItemID.ToString()
                      + "  Char=" + P->InCharacterID.ToString()
                      + "  Slot=" + std::to_string((int)P->InCharacterSlotType)
                      + "  Err=UnknowError -> 0 (metaserver saved OK)");
            P->ErrorCode = EPBEquipErrorCode::NoError;

            s_PendingSpawn = true;
            s_PendingCharID = P->InCharacterID;
            s_PendingItemID = P->InItemID;
        }
        else if ((int)P->ErrorCode != (int)EPBEquipErrorCode::NoError)
        {
            ClientDebugLog("[LOADOUT-FIX] OnEquipSlotComplete"
                      + std::string("  Item=") + P->InItemID.ToString()
                      + "  Err=" + std::to_string((int)P->ErrorCode) + " (passed through)");
        }
    }
    else if (funcName.find("K2_OnEquipComplete") != std::string::npos)
    {
        auto* P = static_cast<Params::PBCustomizeWidget_K2_OnEquipComplete*>(Parms);
        if ((int)P->ErrorCode == (int)EPBEquipErrorCode::UnknowError)
        {
            ClientDebugLog("[LOADOUT-FIX] K2_OnEquipComplete"
                      + std::string("  Item=") + P->InItemID.ToString()
                      + "  Err=UnknowError -> 0 (metaserver saved OK)");
            P->ErrorCode = EPBEquipErrorCode::NoError;
        }
        else if ((int)P->ErrorCode != (int)EPBEquipErrorCode::NoError)
        {
            ClientDebugLog("[LOADOUT-FIX] K2_OnEquipComplete"
                      + std::string("  Item=") + P->InItemID.ToString()
                      + "  Err=" + std::to_string((int)P->ErrorCode) + " (passed through)");
        }
    }
}

// ======================================================
// Player ID from UPBUserObject
// ======================================================

static std::string GetPlayerId()
{
    // TODO: get from UPBUserObject when it's initialized earlier
    return "76561198950613585";
}

// ======================================================
// HTTP GET helper
// ======================================================

static std::string HttpGet(const std::string& host, int port, const std::string& path)
{
    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSession = WinHttpOpen(L"BoundaryDLL/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::stringstream ss;
    DWORD bytesRead = 0;
    char buf[4096];
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0)
    {
        buf[bytesRead] = 0;
        ss << buf;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ss.str();
}

// ======================================================
// Loadout fetch from metaserver REST API (step 1 — log only)
// ======================================================

void LoadoutFix_FetchAndLog()
{
    std::string playerId = GetPlayerId();
    if (playerId.empty())
    {
        ClientDebugLog("[LOADOUT-FIX] GetPlayerId: NOT FOUND");
        return;
    }
    ClientDebugLog("[LOADOUT-FIX] PlayerId=" + playerId);

    // (moved to ForceRefresh — needs armory to be loaded)

    std::string url = "/api/loadout/" + playerId;
    std::string body = HttpGet("127.0.0.1", 8000, url);
    if (body.empty())
    {
        ClientDebugLog("[LOADOUT-FIX] HTTP GET failed or empty response");
        return;
    }

    ClientDebugLog("[LOADOUT-FIX] HTTP body (" + std::to_string(body.size()) + " bytes): " + body.substr(0, 500));

    try
    {
        auto json = nlohmann::json::parse(body);
        ClientDebugLog("[LOADOUT-FIX] JSON parsed. Top keys: " + [&]() {
            std::string keys;
            for (auto& [k, v] : json.items()) { if (!keys.empty()) keys += ", "; keys += k; }
            return keys;
        }());
        if (json.contains("roles"))
        {
            for (auto& [roleId, roleData] : json["roles"].items())
            {
                std::string pw = roleData.value("primaryWeapon", "None");
                std::string sw = roleData.value("secondaryWeapon", "None");
                std::string lp = roleData.value("leftPylon", "None");
                std::string rp = roleData.value("rightPylon", "None");
                std::string mm = roleData.value("mobilityModule", "None");
                std::string mw = roleData.value("meleeWeapon", "None");
                ClientDebugLog("[LOADOUT-FIX]   " + roleId
                          + "  PW=" + pw + "  SW=" + sw
                          + "  LP=" + lp + "  RP=" + rp
                          + "  MM=" + mm + "  MW=" + mw);
            }
        }
    }
    catch (std::exception& e)
    {
        ClientDebugLog(std::string("[LOADOUT-FIX] JSON parse error: ") + e.what());
    }
}

// ======================================================
// Called AFTER ProcessEvent (BP callback has run).
// ======================================================

static FName ToFName(const std::string& s)
{
    std::wstring ws(s.begin(), s.end());
    FString fStr(ws.c_str());
    return UKismetStringLibrary::Conv_StringToName(fStr);
}

// ======================================================
// Full armory refresh state (JSON fetched in hotkey thread,
// actual spawn deferred to main thread via FlushRefresh)
// ======================================================

static nlohmann::json s_FullLoadoutJson;
static bool              s_FullRefreshPending = false;

// Called from hotkey thread (F8) — HTTP GET only, no ProcessEvent
void LoadoutFix_ForceRefresh()
{
    // Log a DisplayChar RoleConfig address for x64dbg write-breakpoint
    {
        auto chars = getObjectsOfClass(APBDisplayCharacter::StaticClass(), false);
        if (!chars.empty())
            ClientDebugLog("[LOADOUT-FIX] RoleConfig addr=0x"
                      + std::to_string(reinterpret_cast<uintptr_t>(
                          &static_cast<APBDisplayCharacter*>(chars[0])->RoleConfig)));
    }

    ClientDebugLog("[LOADOUT-FIX] ForceRefresh: fetching...");

    std::string playerId = GetPlayerId();
    std::string body = HttpGet("127.0.0.1", 8000, "/api/loadout/" + playerId);
    if (body.empty()) { ClientDebugLog("[LOADOUT-FIX] ForceRefresh: HTTP failed"); return; }

    try { s_FullLoadoutJson = nlohmann::json::parse(body); }
    catch (...) { ClientDebugLog("[LOADOUT-FIX] ForceRefresh: JSON parse failed"); return; }

    if (!s_FullLoadoutJson.contains("roles"))
    { ClientDebugLog("[LOADOUT-FIX] ForceRefresh: no roles in JSON"); return; }

    s_FullRefreshPending = true;
    ClientDebugLog("[LOADOUT-FIX] ForceRefresh: JSON ready, pending main-thread spawn");
}

void LoadoutFix_FlushRefresh()
{
    // --- Full refresh (from F8 hotkey) ---
    if (s_FullRefreshPending)
    {
        s_FullRefreshPending = false;

        auto mgrs = getObjectsOfClass(UPBShowRoomManager::StaticClass(), false);
        if (mgrs.empty()) return;
        auto* ShowRoom = static_cast<UPBShowRoomManager*>(mgrs[0]);

        // 1. Destroy all cached display actors (so SpawnInventory creates fresh ones)
        ShowRoom->DestroyAllDisplayActor();

        // 2. Re-spawn character bodies (may be at origin — state machine positions them later)
        ShowRoom->SpawnCharacters();

        // 3. Find the re-spawned characters, patch their RoleConfig, and spawn equipment
        auto charActors = getObjectsOfClass(APBDisplayCharacter::StaticClass(), false);
        ClientDebugLog("[LOADOUT-FIX] Found " + std::to_string(charActors.size()) + " display characters");

        for (auto* obj : charActors)
        {
            auto* Char = static_cast<APBDisplayCharacter*>(obj);
            std::string charName = Char->RoleConfig.CharacterID.ToString();
            if (!s_FullLoadoutJson["roles"].contains(charName)) continue;
            auto& rd = s_FullLoadoutJson["roles"][charName];

            ClientDebugLog("[LOADOUT-FIX] Patching RoleConfig for " + charName);

            Char->RoleConfig.FirstWeaponPartData.WeaponID
                = ToFName(rd.value("primaryWeapon", "None"));
            Char->RoleConfig.SecondWeaponPartData.WeaponID
                = ToFName(rd.value("secondaryWeapon", "None"));
            Char->RoleConfig.LeftLauncherData.ID
                = ToFName(rd.value("leftPylon", "None"));
            Char->RoleConfig.RightLauncherData.ID
                = ToFName(rd.value("rightPylon", "None"));
            Char->RoleConfig.MeleeWeaponData.ID
                = ToFName(rd.value("meleeWeapon", "None"));
            Char->RoleConfig.MobilityModuleData.MobilityModuleID
                = ToFName(rd.value("mobilityModule", "None"));

            // Spawn equipment from the new RoleConfig (cache is empty, so fresh actors)
            FName cID = ToFName(charName);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.FirstWeaponPartData.WeaponID);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.SecondWeaponPartData.WeaponID);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.LeftLauncherData.ID);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.RightLauncherData.ID);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.MeleeWeaponData.ID);
            ShowRoom->SpawnInventory(cID, Char->RoleConfig.MobilityModuleData.MobilityModuleID);

            Char->K2_RefreshDisplayActor();
            Char->K2_FinishRefreshDisplayActor();
        }

        // Update UI slot labels (per-character)
        auto panels = getObjectsOfClass(UPBPanelCSTM_EditCharacterSlot::StaticClass(), false);
        for (auto* obj : panels)
        {
            auto* panel = static_cast<UPBPanelCSTM_EditCharacterSlot*>(obj);
            std::string charName = panel->EditingCharacterID.ToString();
            ClientDebugLog("[LOADOUT-FIX]   Panel char=" + charName + " curEquipped=" + panel->EquippedInventoryID.ToString());
            if (s_FullLoadoutJson["roles"].contains(charName))
            {
                auto& rd = s_FullLoadoutJson["roles"][charName];
                panel->EquippedInventoryID = ToFName(rd.value("primaryWeapon", "None"));
            }
        }

        ClientDebugLog("[LOADOUT-FIX] Full refresh done");
    }

    // --- Single-item equip refresh ---
    if (!s_PendingSpawn)
        return;
    s_PendingSpawn = false;

    auto mgrs2 = getObjectsOfClass(UPBShowRoomManager::StaticClass(), false);
    if (mgrs2.empty())
        return;

    auto* ShowRoom = static_cast<UPBShowRoomManager*>(mgrs2[0]);
    ClientDebugLog("[LOADOUT-FIX] SpawnInventory  Char=" + s_PendingCharID.ToString()
              + "  Item=" + s_PendingItemID.ToString());
    ShowRoom->SpawnInventory(s_PendingCharID, s_PendingItemID);

    auto panels2 = getObjectsOfClass(UPBPanelCSTM_EditCharacterSlot::StaticClass(), false);
    for (auto* obj : panels2)
    {
        static_cast<UPBPanelCSTM_EditCharacterSlot*>(obj)->EquippedInventoryID = s_PendingItemID;
    }
}
