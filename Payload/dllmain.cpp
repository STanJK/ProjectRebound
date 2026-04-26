// ======================================================
//  SECTION 1 — INCLUDES & HEADERS
// ======================================================

#include <thread>
#include <Windows.h>
#include "SDK.hpp"
#include "NetDriverAccess.h"
#include "SDK/Engine_parameters.hpp"
#include "SDK/ProjectBoundary_parameters.hpp"
#include "safetyhook/safetyhook.hpp"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include "libreplicate.h"
#include "LateJoinManager.h"
#include <mutex>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <random>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace SDK;


// ======================================================
//  SECTION 2 — LOGGING SYSTEM
// ======================================================

// Helper function to get current timestamp for log file naming
std::string CurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::string LogFilePath;
std::mutex LogMutex;

// Initializes the logging system and output to wrapper
void Log(const std::string& msg)
{
    std::cout << msg << std::endl;
}

// ======================================================
//  SECTION 3 — GLOBAL VARIABLES AND FORWARD DECLARATIONS
// ======================================================

SafetyHookInline MessageBoxWHook;

uintptr_t BaseAddress = 0x0;

static LibReplicate* libReplicate;

bool listening = false;
bool amServer = false;

//Client logging to file
bool ClientDebugLogEnabled = false;
std::ofstream clientLogFile;

struct ServerConfig {
    std::wstring MapName;
    std::wstring FullModePath;
    unsigned int ExternalPort;
    unsigned int Port;
    bool IsPvE;
    int MinPlayersToStart;
    std::string ServerName;
    std::string ServerRegion;
};

//Central server ip
std::string OnlineBackendAddress = "";

//Room heartbeat credentials from the desktop browser/match server
std::string HostRoomId = "";
std::string HostToken = "";

//IP from the server browser
std::string MatchIP = "";

//Auto connect checks
bool LoginCompleted = false;
bool ReadyToAutoconnect = false;

static ServerConfig Config{};

void DebugLocateSubsystems();
void InitDebugConsole();
void DebugDumpSubsystemsToFile();
void ClientAutoDumpThread();
void HotkeyThread();

// ======================================================
//  SECTION 4 — UTILITY HELPERS
// ======================================================

std::vector<UObject*> getObjectsOfClass(UClass* theClass, bool includeDefault) {
    std::vector<UObject*> ret = std::vector<UObject*>();

    for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject() && !includeDefault)
            continue;

        if (Obj->IsA(theClass))
        {
            ret.push_back(Obj);
        }
    }

    return ret;
}

UObject* GetLastOfType(UClass* theClass, bool includeDefault) {
    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject() && !includeDefault)
            continue;

        if (Obj->IsA(theClass))
        {
            return Obj;
        }
    }

    return nullptr;
}
// Client log write
void ClientLog(const std::string& msg)
{
    // Always print to console
    std::cout << msg << std::endl;

    // If debug logging enabled, write to file
    if (ClientDebugLogEnabled && clientLogFile.is_open())
    {
        clientLogFile << msg << std::endl;
        clientLogFile.flush();
    }
}

//Force press space when autoconnect so it wont stuck to wait for player to press
void PressSpace()
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_SPACE;

    SendInput(1, &input, sizeof(INPUT));

    // Key up
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

//Get PlayerCount helper
int GetCurrentPlayerCount()
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return -1;

    APBGameState* GS = (APBGameState*)World->AuthorityGameMode->GameState;
    if (!GS)
        return -1;

    return GS->PlayerArray.Num();
}

nlohmann::json BuildServerStatusPayload()
{
    int playerCount = GetCurrentPlayerCount();

    std::string map = std::string(Config.MapName.begin(), Config.MapName.end());
    std::string mode = std::string(Config.FullModePath.begin(), Config.FullModePath.end());

    std::string state = "Unknown";

    // FIXED: Add proper null checks before dereferencing
    UWorld* World = UWorld::GetWorld();
    if (World && World->AuthorityGameMode && World->AuthorityGameMode->GameState)
    {
        APBGameState* GS = (APBGameState*)World->AuthorityGameMode->GameState;
        state = GS->RoundState.ToString();
    }

    nlohmann::json payload = {
        { "name",         Config.ServerName },
        { "region",       Config.ServerRegion },
        { "mode",         mode },
        { "map",          map },
        { "port",         Config.ExternalPort },
        { "playerCount",  playerCount },
        { "serverState",  state }
    };

    return payload;
}

std::string StripHttpScheme(const std::string& backend)
{
    const std::string http = "http://";
    const std::string https = "https://";

    if (backend.rfind(http, 0) == 0)
        return backend.substr(http.length());

    if (backend.rfind(https, 0) == 0)
        return backend.substr(https.length());

    return backend;
}

nlohmann::json BuildRoomHeartbeatPayload()
{
    int playerCount = GetCurrentPlayerCount();
    std::string state = "Unknown";

    UWorld* World = UWorld::GetWorld();
    if (World && World->AuthorityGameMode && World->AuthorityGameMode->GameState)
    {
        APBGameState* GS = (APBGameState*)World->AuthorityGameMode->GameState;
        state = GS->RoundState.ToString();
    }

    nlohmann::json payload = {
        { "hostToken",   HostToken },
        { "playerCount", playerCount },
        { "serverState", state }
    };

    return payload;
}

//Send Message to Backend HTTP Helper
void SendServerStatus(const std::string& backend)
{
    bool useRoomHeartbeat = !HostRoomId.empty() && !HostToken.empty();
    nlohmann::json payload = useRoomHeartbeat ? BuildRoomHeartbeatPayload() : BuildServerStatusPayload();
    if (!useRoomHeartbeat && !HostRoomId.empty())
    {
        payload["roomId"] = HostRoomId;
        payload["hostToken"] = HostToken;
    }

    std::string body = payload.dump();
    std::string cleanBackend = StripHttpScheme(backend);

    size_t slash = cleanBackend.find('/');
    if (slash != std::string::npos)
        cleanBackend = cleanBackend.substr(0, slash);

    size_t colon = cleanBackend.find(':');
    if (colon == std::string::npos)
    {
        std::cout << "[ONLINE] Invalid backend address format." << std::endl;
        return;
    }

    std::string host = cleanBackend.substr(0, colon);
    std::string port = cleanBackend.substr(colon + 1);
    std::string path = useRoomHeartbeat
        ? "/v1/rooms/" + HostRoomId + "/heartbeat"
        : "/server/status";

    HINTERNET hSession = WinHttpOpen(L"BoundaryDLL/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
        return;

    std::wstring whost(host.begin(), host.end());
    INTERNET_PORT wport = (INTERNET_PORT)std::stoi(port);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), wport, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        std::wstring(path.begin(), path.end()).c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    BOOL bResults = WinHttpSendRequest(
        hRequest,
        L"Content-Type: application/json",
        -1,
        (LPVOID)body.c_str(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (bResults)
        WinHttpReceiveResponse(hRequest, NULL);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::cout << "[ONLINE] Sent " << path << ": " << body << std::endl;
}

// ======================================================
//  SECTION 5 — UNREAL ENGINE HELPERS
// ======================================================

void EnableUnrealConsole() {
    SDK::UInputSettings::GetDefaultObj()->ConsoleKeys[0].KeyName =
        SDK::UKismetStringLibrary::Conv_StringToName(L"F2");

    /* Creates a new UObject of class-type specified by Engine->ConsoleClass */
    SDK::UObject* NewObject =
        SDK::UGameplayStatics::SpawnObject(
            UEngine::GetEngine()->ConsoleClass,
            UEngine::GetEngine()->GameViewport
        );

    /* The Object we created is a subclass of UConsole, so this cast is **safe**. */
    UEngine::GetEngine()->GameViewport->ViewportConsole =
        static_cast<SDK::UConsole*>(NewObject);

    ClientLog("[DEBUG] Unreal Console => F2");
}

void ConnectToMatch() {
    UPBGameInstance* GameInstance =
        (UPBGameInstance*)UWorld::GetWorld()->OwningGameInstance;

    GameInstance->ShowLoadingScreen(false, true);

    UPBLocalPlayer* LocalPlayer =
        (UPBLocalPlayer*)(UWorld::GetWorld()->OwningGameInstance->LocalPlayers[0]);

    LocalPlayer->GoToRange(0.0f);

    UKismetSystemLibrary::ExecuteConsoleCommand(
        UWorld::GetWorld(), L"travel 127.0.0.1", nullptr
    );

    GameInstance->ShowLoadingScreen(true, true);
}


// ======================================================
//  SECTION 6 — REPLICATION SYSTEM GLOBALS
// ======================================================

std::vector<APlayerController*> playerControllersPossessed = std::vector<APlayerController*>();

int NumPlayersJoined = 0;
float PlayerJoinTimerSelectFuck = -1.0f;
bool DidProcFlow = false;
float StartMatchTimer = -1.0f;
int NumPlayersSelectedRole = 0;
bool DidProcStartMatch = false;
bool canStartMatch = false;
int NumExpectedPlayers = -1;
float MatchStartCountdown = -1.0f;

std::unordered_map<APBPlayerController*, bool> PlayerRespawnAllowedMap{};
std::unordered_set<APBPlayerController*> PlayersConfirmedRole{};
std::unordered_set<APBPlayerController*> PendingNameUpdatePlayers{};
std::unordered_set<APBPlayerController*> AppliedNameUpdatePlayers{};
float PendingNameApplyAccumulator = 0.0f;

// LateJoinManager instance
// Constructed later in MainThread after dependencies are ready
static LateJoinManager* gLateJoinManager = nullptr;

// Helpers used by TickFlushHook and LateJoinManager

APBGameState* GetPBGameState()
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode || !World->AuthorityGameMode->GameState)
        return nullptr;

    return (APBGameState*)World->AuthorityGameMode->GameState;
}

APBGameMode* GetPBGameMode()
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return nullptr;

    return (APBGameMode*)World->AuthorityGameMode;
}

bool IsRoundCurrentlyInProgress()
{
    APBGameState* GameState = GetPBGameState();
    return GameState && GameState->IsRoundInProgress();
}

void QueuePendingPlayerNameUpdate(APBPlayerController* PlayerController)
{
    if (!PlayerController)
        return;

    if (AppliedNameUpdatePlayers.find(PlayerController) == AppliedNameUpdatePlayers.end())
    {
        PendingNameUpdatePlayers.insert(PlayerController);
    }
}

void ApplyPendingPlayerNameUpdates(const char* reason)
{
    if (PendingNameUpdatePlayers.empty())
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return;

    AGameMode* GameMode = (AGameMode*)World->AuthorityGameMode;
    if (!GameMode)
        return;

    std::vector<APBPlayerController*> toApply;
    toApply.reserve(PendingNameUpdatePlayers.size());
    for (APBPlayerController* playerController : PendingNameUpdatePlayers)
    {
        if (playerController)
            toApply.push_back(playerController);
    }

    for (APBPlayerController* playerController : toApply)
    {
        if (!playerController || !playerController->PlayerState)
            continue;

        FString playerName = playerController->PlayerState->GetPlayerName();
        std::string nameStr = playerName.ToString();

        if (nameStr.empty() || nameStr == "UserName")
            continue;

        GameMode->ChangeName(playerController, playerName, true);

        if (playerController->PBPlayerState)
        {
            playerController->PBPlayerState->OnCustomPlayerNameChanged();
        }

        PendingNameUpdatePlayers.erase(playerController);
        AppliedNameUpdatePlayers.insert(playerController);

        std::cout << "[NAME] Applied delayed name update(" << reason << "): " << nameStr << std::endl;
    }
}
// ======================================================
//  SECTION 7 — HOOK DETOURS (ENGINE HOOKS)
// ======================================================

SafetyHookInline TickFlush = {};

void TickFlushHook(UNetDriver* NetDriver, float DeltaTime) {
    if (listening && NetDriver && UWorld::GetWorld()) {
        NetDriverAccess::Observe(NetDriver, UWorld::GetWorld(), NetDriverAccess::Source::HookArgument);

        if (PlayerJoinTimerSelectFuck > 0.0f) {
            PlayerJoinTimerSelectFuck -= DeltaTime;

            if (PlayerJoinTimerSelectFuck <= 0.0f) {

                for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
                {
                    SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

                    if (!Obj)
                        continue;

                    if (Obj->IsDefaultObject())
                        continue;

                    if (Obj->IsA(APBPlayerController::StaticClass()))
                    {
                        if (((APBPlayerController*)Obj)->CanSelectRole()) {
                            std::cout << "Selecting role..." << std::endl;
                            ((APBPlayerController*)Obj)->ClientSelectRole();
                        }
                        else {
                            std::cout << "CANT SELECT ROLE WEE WOO WEE WOO" << std::endl;
                        }
                    }
                }

            }
        }

        std::vector<LibReplicate::FActorInfo> ActorInfos = std::vector<LibReplicate::FActorInfo>();
        std::vector<UNetConnection*> Connections = std::vector<UNetConnection*>();
        std::vector<void*> PlayerControllers = std::vector<void*>();

        for (UNetConnection* Connection : NetDriver->ClientConnections) {
            if (Connection->OwningActor) {
                Connection->ViewTarget = Connection->PlayerController ? Connection->PlayerController->GetViewTarget() : Connection->OwningActor;
                Connections.push_back(Connection);
            }
        }

        for (int i = 0; i < UWorld::GetWorld()->Levels.Num(); i++) {
            ULevel* Level = UWorld::GetWorld()->Levels[i];

            if (Level) {
                for (int j = 0; j < Level->Actors.Num(); j++) {
                    AActor* actor = Level->Actors[j];

                    if (!actor)
                        continue;

                    if (actor->RemoteRole == ENetRole::ROLE_None)
                        continue;

                    if (!actor->bReplicates)
                        continue;

                    if (actor->bActorIsBeingDestroyed)
                        continue;

                    if (actor->Class == APlayerController_BP_C::StaticClass()) {
                        PlayerControllers.push_back((void*)actor);
                        if (((APlayerController*)actor)->Character && ((APlayerController*)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass())) {
                            ((UCharacterMovementComponent*)(((APlayerController*)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass())))->bIgnoreClientMovementErrorChecksAndCorrection = true;
                            ((UCharacterMovementComponent*)(((APlayerController*)actor)->Character->GetComponentByClass(UCharacterMovementComponent::StaticClass())))->bServerAcceptClientAuthoritativePosition = true;
                        }
                        continue;
                    }

                    ActorInfos.push_back(LibReplicate::FActorInfo(actor, actor->bNetTemporary));
                }
            }
        }

        std::vector<LibReplicate::FPlayerControllerInfo> PlayerControllerInfos = std::vector<LibReplicate::FPlayerControllerInfo>();

        for (void* PlayerController : PlayerControllers) {
            for (UNetConnection* Connection : Connections) {
                if (Connection->PlayerController == PlayerController) {
                    PlayerControllerInfos.push_back(LibReplicate::FPlayerControllerInfo(Connection, PlayerController));
                    break;
                }
            }
        }

        std::vector<void*> CastConnections = std::vector<void*>();

        for (UNetConnection* Connection : Connections) {
            CastConnections.push_back((void*)Connection);
        }

        static FName* ActorName = nullptr;

        if (!ActorName) {
            ActorName = new FName();
            ActorName->ComparisonIndex = UKismetStringLibrary::Conv_StringToName(L"Actor").ComparisonIndex;
            ActorName->Number = UKismetStringLibrary::Conv_StringToName(L"Actor").Number;
        }

        if (ActorInfos.size() > 0 && CastConnections.size() > 0) {
            if (NetDriver) {
                libReplicate->CallFromTickFlushHook(ActorInfos, PlayerControllerInfos, CastConnections, ActorName, NetDriver);

                int* counter = reinterpret_cast<int*>(reinterpret_cast<char*>(NetDriver) + 0x420);
                *counter = *counter + 1;
            }
        }

        // Drive LateJoin state machine
        if (gLateJoinManager)
            gLateJoinManager->Tick(DeltaTime);
    }

    APBGameState* CurrentGameState = GetPBGameState();
    if (CurrentGameState && !CurrentGameState->IsRoundInProgress()) {
        if (CurrentGameState->RoundState.ToString().contains("InvalidState")) {

            if (NumPlayersJoined >= Config.MinPlayersToStart) {
                if (!DidProcFlow) {
                    if (MatchStartCountdown == -1.0f) {
                        MatchStartCountdown = 30.0f;

                        NumExpectedPlayers = NumPlayersJoined;
                    }
                    else {
                        MatchStartCountdown -= DeltaTime;

                        if (NumExpectedPlayers > NumPlayersJoined) {
                            NumExpectedPlayers = NumPlayersJoined;

                            MatchStartCountdown += 15.0f;
                        }

                        if (MatchStartCountdown <= 0.0f) {
                            DidProcFlow = true;

                            std::cout << "All players connected, beginning role selection flow!" << std::endl;

                            PlayerJoinTimerSelectFuck = 5.0f;

                            NumExpectedPlayers = NumPlayersJoined;
                            NumPlayersSelectedRole = 0;
                            canStartMatch = false;
                            StartMatchTimer = -1.0f;
                            PlayersConfirmedRole.clear();
                            PendingNameUpdatePlayers.clear();
                            AppliedNameUpdatePlayers.clear();
                        }
                    }
                }
            }
        }

        if (CurrentGameState->RoundState.ToString().contains("CountdownToStart")) {

            for (UNetConnection* pc : NetDriver->ClientConnections) {
                if (pc->PlayerController && pc->PlayerController->Pawn)
                    pc->PlayerController->Possess(pc->PlayerController->Pawn);
            }
        }
    }

    if (canStartMatch && !DidProcStartMatch) {
        DidProcStartMatch = true;

        ((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->StartMatch();
    }

    if (GetAsyncKeyState(VK_F8) && amServer) {
        for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
        {
            SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

            if (!Obj)
                continue;

            if (Obj->IsDefaultObject())
                continue;

            if (Obj->IsA(APBPlayerController::StaticClass()))
            {
                ((APBPlayerController*)Obj)->ServerSuicide(0);
            }
        }

        while (GetAsyncKeyState(VK_F8)) {

        }
    }

    return TickFlush.call(NetDriver, DeltaTime);
}


// ======================================================
//  SECTION 8 — HOOK DETOURS (GAMEPLAY HOOKS)
// ======================================================

SafetyHookInline NotifyActorDestroyed = {};

bool NotifyActorDestroyedHook(UWorld* World, AActor* Actor, bool SomeShit, bool SomeShit2) {
    bool ret = NotifyActorDestroyed.call<bool>(World, Actor, SomeShit, SomeShit2);

    if (listening) {
        LibReplicate::FActorInfo ActorInfo = LibReplicate::FActorInfo((void*)Actor, Actor->bNetTemporary);

        libReplicate->CallWhenActorDestroyed(ActorInfo);
    }

    return ret;
}

SafetyHookInline NotifyAcceptingConnection = {};

__int64 NotifyAcceptingConnectionHook(UObject* obj) {
    return 1;
}

SafetyHookInline NotifyControlMessage = {};

char NotifyControlMessageHook(unsigned __int64 ScuffedShit, __int64 a2, uint8_t a3, __int64 a4) {
    if (UWorld* World = UWorld::GetWorld()) {
        if (UNetDriver* ActiveNetDriver = NetDriverAccess::Resolve()) {
            NetDriverAccess::Observe(ActiveNetDriver, World, NetDriverAccess::Source::Cached);
        }
    }

    return NotifyControlMessage.call<char>(ScuffedShit, a2, a3, a4);
}

SafetyHookInline ProcessEvent;

void ProcessEventHook(UObject* Object, UFunction* Function, void* Parms) {
    const std::string functionName = Function ? std::string(Function->GetFullName()) : "";

    if (functionName.contains("QuickRespawn")) {
        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = true;
    }

    if (functionName.contains("ServerRestartPlayer")) {
        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        if (PlayerRespawnAllowedMap.contains(PBPlayerController) && PlayerRespawnAllowedMap[PBPlayerController] == false) {
            std::cout << "Denied restart!" << std::endl;
            return;
        }
    }

    // LateJoin: role-selection interception (CanPlayerSelectRole / CanSelectRole)
    if (gLateJoinManager && gLateJoinManager->OnProcessEvent(Object, functionName, Parms))
    {
        // Already handled by LateJoinManager
        return;
    }

    // LateJoin: ServerConfirmRoleSelection
    // Must call original ProcessEvent first, then advance LateJoin state
    if (functionName.contains("ServerConfirmRoleSelection")) {
        APBPlayerController* PBPlayerController = Object && Object->IsA(APBPlayerController::StaticClass())
            ? (APBPlayerController*)Object
            : nullptr;
        QueuePendingPlayerNameUpdate(PBPlayerController);

        // Late join player: execute original + advance state, skip match-start counting
        if (gLateJoinManager && gLateJoinManager->IsLateJoinPlayer(PBPlayerController)) {
            // Execute original function first
            ProcessEvent.call(Object, Function, Parms);
            // Advance LateJoin state to RoleConfirmed
            gLateJoinManager->OnRoleConfirmed(PBPlayerController);
            return;
        }

        // Initial join player in deferred-spawn flow: execute original + advance state,
        // but ALSO count towards match start (unlike late join)
        if (gLateJoinManager && gLateJoinManager->IsInitialJoinPlayer(PBPlayerController)) {
            ProcessEvent.call(Object, Function, Parms);
            // Advance deferred-spawn state to RoleConfirmed
            gLateJoinManager->OnRoleConfirmed(PBPlayerController);

            // Still count for match start
            if (PBPlayerController) {
                const auto [_, inserted] = PlayersConfirmedRole.insert(PBPlayerController);
                if (inserted) {
                    NumPlayersSelectedRole = static_cast<int>(PlayersConfirmedRole.size());
                    std::cout << "[MATCH] Role confirmed by (initial-join) "
                        << PBPlayerController->GetFullName()
                        << " (" << NumPlayersSelectedRole << "/" << NumExpectedPlayers << ")" << std::endl;
                }

                if (!canStartMatch && NumExpectedPlayers > 0 && NumPlayersSelectedRole >= NumExpectedPlayers) {
                    canStartMatch = true;
                    StartMatchTimer = -1.0f;
                }
            }
            return;
        }

        if (PBPlayerController) {
            const auto [_, inserted] = PlayersConfirmedRole.insert(PBPlayerController);
            if (inserted) {
                NumPlayersSelectedRole = static_cast<int>(PlayersConfirmedRole.size());
                std::cout << "[MATCH] Role confirmed by "
                    << PBPlayerController->GetFullName()
                    << " (" << NumPlayersSelectedRole << "/" << NumExpectedPlayers << ")" << std::endl;
            }
            else {
                std::cout << "[MATCH] Ignoring duplicate role confirmation from "
                    << PBPlayerController->GetFullName() << std::endl;
            }

        if (!canStartMatch && NumExpectedPlayers > 0 && NumPlayersSelectedRole >= NumExpectedPlayers) {
            canStartMatch = true;
            StartMatchTimer = -1.0f;
        }
    }

    if (functionName.contains("ReadyToMatchIntro_WaitingToStart")) {
        ApplyPendingPlayerNameUpdates("ReadyToMatchIntro_WaitingToStart");
        if (!canStartMatch) {
            return;
        }
    }

    if (functionName.contains("ClientBeKilled")) {
        std::cout << "Intercepted Player Kill!" << std::endl;

        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = false;
    }

    if (functionName.contains("PlayerCanRestart")) {
        ((Params::GameModeBase_PlayerCanRestart*)Parms)->ReturnValue =
            ((AGameModeBase*)Object)->HasMatchStarted();
        return;
    }

    return ProcessEvent.call(Object, Function, Parms);
}

SafetyHookInline PostLoginHook;

void* PostLogin(AGameMode* GameMode, APBPlayerController* PC)
{
    void* Ret = PostLoginHook.call<void*>(GameMode, PC);

    NumPlayersJoined++;

    std::cout << "Player Connected!" << std::endl;

    // LateJoin detection
    if (gLateJoinManager && gLateJoinManager->OnPostLogin(GameMode, PC))
    {
        // Handled as LateJoin player; skip normal first-life flow
        return Ret;
    }

    // Initial join: defer Pawn creation to LateJoinManager's delayed-spawn flow.
    // This ensures weapons are created AFTER role confirmation (when pre-order
    // inventory is already applied), fixing the loadout switching issue.
    if (gLateJoinManager)
    {
        gLateJoinManager->QueueInitialJoinPlayer(GameMode, PC);
        return Ret;
    }

    // Fallback: if LateJoinManager is not available, use the old immediate-respawn path
    if (PC && PC->Pawn)
    {
        PC->ServerSuicide(0);   // triggers respawn
    }

    return Ret;
}

SafetyHookInline OnFireWeaponHook;

void* OnFireWeapon(APBWeapon* Weapon) {
    if ((uintptr_t)_ReturnAddress() - BaseAddress != 0x1608B31) {
        return nullptr;
    }
    else {
        return OnFireWeaponHook.call<void*>(Weapon);
    }
}


// ======================================================
//  SECTION 9 — HOOK DETOURS (CLIENT HOOKS)
// ======================================================

SafetyHookInline ProcessEventClient;

void ProcessEventHookClient(UObject* Object, UFunction* Function, void* Parms) {
    // TEMP LOGIN DEBUG DUMP (GameInstance only)
    //if (Object && Object->IsA(UPBGameInstance::StaticClass()))
    //{
    //    std::string fn = Function->GetFullName();
    //        std::cout << "[LOGIN-DUMP] GI :: " << fn << std::endl;
    //}
    //Froce space to login
    if (Function->GetFullName().contains("UMG_EnterGame_C.Construct"))
    {
        ClientLog("[LOGIN] EnterGame Construct forcing SPACE");

        std::thread([]()
            {
                Sleep(1000); // small delay so widget is fully active
                PressSpace();
            }).detach();
    }
    if (Function->GetFullName().contains("UMG_EnterGame_C.BP_OnActivated"))
    {
        ClientLog("[LOGIN] EnterGame Activated forcing SPACE");

        std::thread([]()
            {
                Sleep(1000);
                PressSpace();
            }).detach();
    }
    // Detect login complete via MainMenuBase Construct
    if (Function->GetFullName().contains("UMG_MainMenuBase_C.Construct"))
    {
        LoginCompleted = true;
    }
    if (Function->GetFullName().contains("OnConnectMatchServerTimeOut")) {
        ClientLog("[PE] " + std::string(Object->GetFullName()) + " - " + std::string(Function->GetFullName()));

        ConnectToMatch();
    }

    return ProcessEventClient.call(Object, Function, Parms);
}

SafetyHookInline ClientDeathCrash;

__int64 ClientDeathCrashHook(__int64 a1) {
    return 0;
}


// ======================================================
//  SECTION 10 — HOOK DETOURS (MISC HOOKS)
// ======================================================

SafetyHookInline ObjectNeedsLoad;

char ObjectNeedsLoadHook(UObject* a1) {
    return 1;
}

SafetyHookInline ActorNeedsLoad;

char ActorNeedsLoadHook(UObject* a1) {
    return 1;
}

int WINAPI MessageBoxW_Detour(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    if (lpText && wcsstr(lpText, L"Roboto"))
    {
        return IDOK;
    }
    return MessageBoxWHook.call<int>(hWnd, lpText, lpCaption, uType);
}

SafetyHookInline HudFunctionThatCrashesTheGame;

__int64 HudFunctionThatCrashesTheGameHook(__int64 a1, __int64 a2) {
    return 0;
}

SafetyHookInline GameEngineTick;

__int64 GameEngineTickHook(APlayerController* a1,
    float a2,
    __int64 a3,
    __int64 a4) {

    static bool flip = true;

    flip = !flip;

    if (flip) {
        std::cout << "NO TICKY" << std::endl;
        return 0;
    }

    return GameEngineTick.call<__int64>(a1, a2, a3, a4);
}

SafetyHookInline IsDedicatedServerHook;

bool IsDedicatedServer(void* WorldContextOrSomething) {
    return true;
}

SafetyHookInline IsServerHook;

bool IsServer(void* WorldContextOrSomething) {
    return true;
}

SafetyHookInline IsStandaloneHook;

bool IsStandalone(void* WorldContextOrSomething) {
    return false;
}
// ======================================================
//  SECTION 11 — HOOK INITIALIZATION
// ======================================================

void InitMessageBoxHook()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;

    void* addr = GetProcAddress(user32, "MessageBoxW");
    if (!addr) return;

    MessageBoxWHook = safetyhook::create_inline(addr, MessageBoxW_Detour);
}

void InitServerHooks() {
    NotifyActorDestroyed = safetyhook::create_inline((void*)(BaseAddress + 0x33403E0), NotifyActorDestroyedHook);
    NotifyAcceptingConnection = safetyhook::create_inline((void*)(BaseAddress + 0x36CDC90), NotifyAcceptingConnectionHook);
    NotifyControlMessage = safetyhook::create_inline((void*)(BaseAddress + 0x36CDCE0), NotifyControlMessageHook);
    TickFlush = safetyhook::create_inline((void*)(BaseAddress + 0x33E05F0), TickFlushHook);
    ProcessEvent = safetyhook::create_inline((void*)(BaseAddress + 0x1BCBE40), ProcessEventHook);
    ObjectNeedsLoad = safetyhook::create_inline((void*)(BaseAddress + 0x1B7B710), ObjectNeedsLoadHook);
    ActorNeedsLoad = safetyhook::create_inline((void*)(BaseAddress + 0x3124E70), ActorNeedsLoadHook);
    OnFireWeaponHook = safetyhook::create_inline((void*)(BaseAddress + 0x1610500), OnFireWeapon);
    PostLoginHook = safetyhook::create_inline((void*)(BaseAddress + 0x32903B0), PostLogin);
    IsDedicatedServerHook = safetyhook::create_inline((void*)(BaseAddress + 0x33266F0), IsDedicatedServer);
    IsServerHook = safetyhook::create_inline((void*)(BaseAddress + 0x3326C60), IsServer);
    IsStandaloneHook = safetyhook::create_inline((void*)(BaseAddress + 0x3326CE0), IsStandalone);
}

void InitClientHook() {
    ProcessEventClient = safetyhook::create_inline((void*)(BaseAddress + 0x1BCBE40), ProcessEventHookClient);
    ClientDeathCrash = safetyhook::create_inline((void*)(BaseAddress + 0x16abe10), ClientDeathCrashHook);
}


// ======================================================
//  SECTION 12 — SERVER CONFIGURATION
// ======================================================
// Set up the dll to get values from the wrapper
std::string GetCmdValue(const std::string& key)
{
    std::string cmd = GetCommandLineA();
    size_t pos = cmd.find(key);
    if (pos == std::string::npos)
        return "";

    pos += key.length();
    size_t end = cmd.find(" ", pos);
    if (end == std::string::npos)
        end = cmd.length();

    return cmd.substr(pos, end - pos);
}

void LoadConfig()
{
    std::string cmd = GetCommandLineA();

    // PvE flag
    Config.IsPvE = cmd.find("-pve") != std::string::npos;

    // Map
    std::string mapArg = GetCmdValue("-map=");
    if (!mapArg.empty())
    {
        Config.MapName = std::wstring(mapArg.begin(), mapArg.end());
    }
    else
    {
        // fallback to something safe
        Config.MapName = L"Warehouse";
    }

    // Mode
    std::string modeArg = GetCmdValue("-mode=");
    if (!modeArg.empty())
    {
        Config.FullModePath = std::wstring(modeArg.begin(), modeArg.end());
    }
    else
    {
        // fallback based on PvE
        Config.FullModePath = Config.IsPvE
            ? L"/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Hard.BP_PBGameMode_Rush_PVE_Hard_C"
            : L"/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C";
    }

    // Port
    std::string portArg = GetCmdValue("-port=");
    if (!portArg.empty())
    {
        Config.Port = std::stoi(portArg);
    }
    else
    {
        Config.Port = 7777;
    }

    // External port
    std::string externalArg = GetCmdValue("-external=");
    if (!externalArg.empty())
    {
        Config.ExternalPort = std::stoi(externalArg);
    }
    else
    {
        Config.ExternalPort = Config.Port;  // default same as internal
    }

    Log("[SERVER] External port: " + std::to_string(Config.ExternalPort));


    //Name
    std::string serverNameArg = GetCmdValue("-servername=");
    if (!serverNameArg.empty())
    {
        Config.ServerName = serverNameArg;
        Log("[SERVER] Server name: " + serverNameArg);
    }
    //Region
    std::string serverRegionArg = GetCmdValue("-serverregion=");
    if (!serverRegionArg.empty())
    {
        Config.ServerRegion = serverRegionArg;
        Log("[SERVER] Server region: " + serverRegionArg);
    }
    // Min players (still used in TickFlush)
    Config.MinPlayersToStart = Config.IsPvE ? 1 : 2;

    // Online check if contact central server
    std::string onlineArg = GetCmdValue("-online=");
    if (!onlineArg.empty())
    {
        OnlineBackendAddress = onlineArg;
        std::cout << "[SERVER] Online backend: " << OnlineBackendAddress << std::endl;
    }

    std::string roomIdArg = GetCmdValue("-roomid=");
    if (!roomIdArg.empty())
    {
        HostRoomId = roomIdArg;
        Log("[SERVER] Host room id: " + HostRoomId);
    }

    std::string hostTokenArg = GetCmdValue("-hosttoken=");
    if (!hostTokenArg.empty())
    {
        HostToken = hostTokenArg;
        Log("[SERVER] Host token received.");
    }
}

void LoadClientConfig()
{
    std::string matchArg = GetCmdValue("-match=");
    if (!matchArg.empty())
    {
        MatchIP = matchArg;
        ClientLog("[CLIENT] Auto-match target: " + MatchIP);
    }

    // NEW: debug log flag
    if (std::string(GetCommandLineA()).find("-debuglog") != std::string::npos)
    {
        ClientDebugLogEnabled = true;
    }
}

// ======================================================
//  SECTION 13 — SERVER STARTUP AND COMMAND RELATED LOGIC
// ======================================================

void StartServer()
{
    Log("[SERVER] Starting server...");

    LoadConfig();

    Log("[SERVER] Map loaded: " + std::string(Config.MapName.begin(), Config.MapName.end()));
    Log("[SERVER] Mode: " + std::string(Config.FullModePath.begin(), Config.FullModePath.end()));
    Log("[SERVER] Port: " + std::to_string(Config.Port));

    std::wstring openCmd = L"open " + Config.MapName + L"?game=" + Config.FullModePath;
    Log("[SERVER] Executing open command");

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), openCmd.c_str(), nullptr);

    Log("[SERVER] Waiting for world to load...");
    Sleep(8000);

    UEngine* Engine = UEngine::GetEngine();
    UWorld* World = UWorld::GetWorld();

    if (!World)
    {
        Log("[ERROR] World is NULL after map load!");
        return;
    }

    Log("[SERVER] Forcing streaming levels to load...");

    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
    {
        SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

        if (!Obj)
            continue;

        if (Obj->IsDefaultObject())
            continue;

        if (Obj->IsA(ULevelStreaming::StaticClass()))
        {
            ULevelStreaming* LS = (ULevelStreaming*)Obj;

            LS->SetShouldBeLoaded(true);
            LS->SetShouldBeVisible(true);

            Log("[SERVER] Streaming level loaded: " + std::string(Obj->GetFullName()));
        }
    }

    if (!libReplicate)
    {
        Log("[ERROR] libReplicate is null before CreateNetDriver!");
        return;
    }

    Log("[SERVER] Creating NetDriver...");
    FName name = UKismetStringLibrary::Conv_StringToName(L"GameNetDriver");
    libReplicate->CreateNetDriver(Engine, World, &name);

    UIpNetDriver* NetDriver = reinterpret_cast<UIpNetDriver*>(NetDriverAccess::Resolve());

    if (!NetDriver)
    {
        Log("[ERROR] NetDriver not found after CreateNetDriver!");
        return;
    }

    NetDriverAccess::Observe(NetDriver, World, NetDriverAccess::Source::ObjectScan);
    Log("[SERVER] NetDriver created successfully.");

    Log("[SERVER] Calling Listen()...");
    libReplicate->Listen(NetDriver, World, LibReplicate::EJoinMode::Open, Config.Port);
    NetDriverAccess::Observe(NetDriver, World, NetDriverAccess::Source::World);

    NetDriverAccess::Snapshot snapshot{};
    if (NetDriverAccess::TryGetSnapshot(snapshot, false)) {
        Log("[SERVER] NetDriver exposed via source: " + std::string(NetDriverAccess::ToString(snapshot.LastSource)));
    }

    listening = true;

    Log("[SERVER] Server is now listening.");
}
// ======================================================
//  SECTION 14 — CLIENT LOGIC
// ======================================================

void InitClientArmory()
{
    for (UObject* obj : getObjectsOfClass(UPBArmoryManager::StaticClass(), false)) {
        UPBArmoryManager* DefaultConfig = (UPBArmoryManager*)obj;

        std::ifstream items("DT_ItemType.json");
        nlohmann::json itemJson = nlohmann::json::parse(items);

        for (auto& [ItemId, _] : itemJson[0]["Rows"].items()) {
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
            auto* GI = UWorld::GetWorld()->OwningGameInstance;
            UPBLocalPlayer* LP = (UPBLocalPlayer*)GI->LocalPlayers[0];

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
                nullptr
            );

        }).detach();
}

// ======================================================
//  SECTION 15 — MAIN THREAD (ENTRY LOGIC)
// ======================================================

void MainThread()
{
    ClientLog("[BOOT] DLL injected, starting...");
    try
    {
        //Calms down the ui font missing panic
        InitMessageBoxHook();

        BaseAddress = (uintptr_t)GetModuleHandleA(nullptr);

        UC::FMemory::Init((void*)(BaseAddress + 0x18f4350));

        if (std::string(GetCommandLineA()).contains("-server")) {
            amServer = true;
        }

        while (!UWorld::GetWorld()) {
            if (amServer) {
                *(__int8*)(BaseAddress + 0x5ce2404) = 0;
                *(__int8*)(BaseAddress + 0x5ce2405) = 1;
            }
        }

        //DebugLocateSubsystems();
        //DebugDumpSubsystemsToFile();

        if (amServer)
        {
            InitServerHooks();
            Log("[SERVER] Hooks installed.");

            // Wait for world
            Log("[SERVER] Waiting for UWorld...");
            while (!UWorld::GetWorld())
                Sleep(10);
            Log("[SERVER] UWorld is ready.");

            //Initialize LibReplicate exactly like original code
            libReplicate = new LibReplicate(
                LibReplicate::EReplicationMode::Minimal,
                (void*)(BaseAddress + 0x91AEB0),
                (void*)(BaseAddress + 0x33A66D0),
                (void*)(BaseAddress + 0x31F44F0),
                (void*)(BaseAddress + 0x31F0070),
                (void*)(BaseAddress + 0x18F1810),
                (void*)(BaseAddress + 0x18E5490),
                (void*)(BaseAddress + 0x36CDCE0),
                (void*)(BaseAddress + 0x366ADB0),
                (void*)(BaseAddress + 0x31DA270),
                (void*)(BaseAddress + 0x33DF330),
                (void*)(BaseAddress + 0x2fefbd0),
                (void*)(BaseAddress + 0x3506320)
            );
            Log("[SERVER] LibReplicate initialized.");

            // Initialize LateJoinManager
            gLateJoinManager = new LateJoinManager(
                DidProcStartMatch,
                PlayerRespawnAllowedMap,
                nullptr  // ReportRoomStarted callback — can be wired later
            );
            Log("[SERVER] LateJoinManager initialized.");

            StartServer();

            // Heartbeat thread (game + backend)
            std::thread([]() {
                // Wait until Gamestate is Valid
                while (!UWorld::GetWorld() ||
                    !UWorld::GetWorld()->AuthorityGameMode ||
                    !UWorld::GetWorld()->AuthorityGameMode->GameState)
                {
                    Sleep(100);
                }
                while (true)
                {
                    int pc = GetCurrentPlayerCount();
                    std::cout << "[HEARTBEAT] PlayerCount = " << pc << std::endl;

                    if (!OnlineBackendAddress.empty())
                    {
                        SendServerStatus(OnlineBackendAddress);
                    }

                    Sleep(5000);
                }
                }).detach();
        }

        else {
            //We're client
            LoadClientConfig();
            // Initialize client debug log
            if (ClientDebugLogEnabled)
            {
                std::filesystem::create_directory("clientlogs");

                std::string path = "clientlogs/clientlog-" + CurrentTimestamp() + ".txt";
                clientLogFile.open(path, std::ios::app);

                std::cout << "[CLIENT] Debug logging enabled: " << path << std::endl;
            }
            InitDebugConsole();
            EnableUnrealConsole();

            InitClientHook();

            //*(const wchar_t***)(BaseAddress + 0x5C63C88) = &LocalURL;
            //auto dump below
            //std::thread(ClientAutoDumpThread).detach();
            //Init Hotkey Check 
            std::thread(HotkeyThread).detach();

            InitClientArmory();
            if (!MatchIP.empty())
            {
                AutoConnectToMatchFromCmdline();
            }
            /*
            Sleep(10 * 1000);

            UCommonActivatableWidget* widget = nullptr;
            reinterpret_cast<UPBMainMenuManager_BP_C*>(getObjectsOfClass(UPBMainMenuManager_BP_C::StaticClass(), false).back())->GetTopMenuWidget(&widget);
            widget->SetVisibility(ESlateVisibility::Hidden);
            widget->DeactivateWidget();

            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 73.130.167.222", nullptr);
            */

            //UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 127.0.0.1", nullptr);
        }
    }
    catch (...)
    {
        std::cout << "[ERROR] Unhandled exception in MainThread!" << std::endl;
        std::cout << "Press ENTER to exit..." << std::endl;
        std::cin.get();
    }
}


// ======================================================
//  SECTION 16 — DLL ENTRY POINT
// ======================================================

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        std::thread t(MainThread);

        t.detach();
    }

    return TRUE;
}

// ======================================================
//  SECTION 17 — LOADOUT SYSTEM TEST
// ======================================================

void DebugLocateSubsystems()
{
    std::cout << "\nLocating Subsystems\n";

    //Armory Manager
    auto armories = getObjectsOfClass(UPBArmoryManager::StaticClass(), false);
    if (!armories.empty())
        std::cout << "[FOUND] UPBArmoryManager at " << armories.back() << std::endl;
    else
        std::cout << "[MISSING] UPBArmoryManager" << std::endl;

    //Field Mod Manager
    auto fieldMods = getObjectsOfClass(UPBFieldModManager::StaticClass(), false);
    if (!fieldMods.empty())
        std::cout << "[FOUND] UPBFieldModManager at " << fieldMods.back() << std::endl;
    else
        std::cout << "[MISSING] UPBFieldModManager" << std::endl;

    //Weapon Part Manager
    auto partMgrs = getObjectsOfClass(UPBWeaponPartManager::StaticClass(), false);
    if (!partMgrs.empty())
        std::cout << "[FOUND] UPBWeaponPartManager at " << partMgrs.back() << std::endl;
    else
        std::cout << "[MISSING] UPBWeaponPartManager" << std::endl;

    std::cout << "END PHASE 1.1\n\n";
}

void InitDebugConsole()
{
    AllocConsole();

    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);

    // Redirect stdout manually
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);

    std::wcout.clear();
    std::cout.clear();

    std::cout << "[DEBUG] Console initialized" << std::endl;
}

void DebugDumpSubsystemsToFile()
{
    std::ofstream out("subsystems_dump.txt", std::ios::trunc);
    if (!out.is_open())
        return;

    out << "=== SUBSYSTEM DUMP ===\n\n";

    // ----------------------------------------------------
    // 1) Armory Manager
    // ----------------------------------------------------
    auto armories = getObjectsOfClass(UPBArmoryManager::StaticClass(), false);
    if (!armories.empty())
    {
        UPBArmoryManager* Armory = (UPBArmoryManager*)armories.back();
        out << "[UPBArmoryManager] " << Armory << "\n";

        out << "  Armorys.OwnedItems:\n";
        for (int i = 0; i < Armory->Armorys.OwnedItems.Num(); ++i)
        {
            const FPBItem& item = Armory->Armorys.OwnedItems[i];
            std::string id = item.ID.ToString();

            out << "    [" << i << "] ID=" << id
                << " Count=" << item.Count
                << " bIsNew=" << (item.bIsNew ? "true" : "false") << "\n";
        }
        out << "\n";
    }
    else
    {
        out << "[MISSING] UPBArmoryManager\n\n";
    }

    // ----------------------------------------------------
    // 2) Field Mod Manager
    // ----------------------------------------------------
    auto fieldMods = getObjectsOfClass(UPBFieldModManager::StaticClass(), false);
    if (!fieldMods.empty())
    {
        UPBFieldModManager* FieldMod = (UPBFieldModManager*)fieldMods.back();
        out << "[UPBFieldModManager] " << FieldMod << "\n";

        out << "  CharacterPreOrderingInventoryConfigs:\n";
        for (auto& pair : FieldMod->CharacterPreOrderingInventoryConfigs)
        {
            // Correct SDK access: Key() and Value()
            std::string roleId = pair.Key().ToString();
            const FPBInventoryNetworkConfig& cfg = pair.Value();

            out << "    RoleID=" << roleId << "\n";

            for (int i = 0; i < cfg.CharacterSlots.Num(); ++i)
            {
                int slot = (int)cfg.CharacterSlots[i];
                std::string itemId = "";

                if (i < cfg.InventoryItems.Num())
                    itemId = cfg.InventoryItems[i].ToString();

                out << "      Slot[" << i << "] Type=" << slot
                    << " Item=" << itemId << "\n";
            }

            out << "\n";
        }
        out << "\n";
    }
    else
    {
        out << "[MISSING] UPBFieldModManager\n\n";
    }

    // ----------------------------------------------------
    // 3) Weapon Part Manager
    // ----------------------------------------------------
    auto partMgrs = getObjectsOfClass(UPBWeaponPartManager::StaticClass(), false);
    if (!partMgrs.empty())
    {
        UPBWeaponPartManager* PartMgr = (UPBWeaponPartManager*)partMgrs.back();
        out << "[UPBWeaponPartManager] " << PartMgr << "\n";

        out << "  WeaponSlotMap (keys only):\n";
        for (auto& pair : PartMgr->WeaponSlotMap)
        {
            // Correct SDK access: Key() and Value()
            APBWeapon* weapon = pair.Key();
            std::string name = weapon ? weapon->GetFullName() : "NULL";

            out << "    Weapon=" << name << "\n";
        }

        out << "\n";
    }
    else
    {
        out << "[MISSING] UPBWeaponPartManager\n\n";
    }

    out << "=== END SUBSYSTEM DUMP ===\n";
    out.close();
}

void DebugDumpWeaponPartsToFile()
{
    std::ofstream out("weapon_parts_dump.txt", std::ios::trunc);
    if (!out.is_open())
        return;

    out << "=== WEAPON PARTS DUMP ===\n\n";

    auto partMgrs = getObjectsOfClass(UPBWeaponPartManager::StaticClass(), false);
    if (partMgrs.empty())
    {
        out << "[MISSING] UPBWeaponPartManager\n";
        return;
    }

    UPBWeaponPartManager* PartMgr = (UPBWeaponPartManager*)partMgrs.back();
    out << "[UPBWeaponPartManager] " << PartMgr << "\n\n";

    out << "WeaponSlotMap:\n";

    for (auto& pair : PartMgr->WeaponSlotMap)
    {
        APBWeapon* weapon = pair.Key();   // <-- FIXED
        FWeaponSlotPartInfo info = pair.Value(); // <-- FIXED

        std::string weaponName = weapon ? weapon->GetFullName() : "NULL";
        out << "  Weapon=" << weaponName << "\n";

        // Iterate TMap<EPBPartSlotType, UPartDataHolderComponent*>
        for (auto& kvp : info.TypePartMap)
        {
            EPBPartSlotType slotType = kvp.Key();  // <-- FIXED
            UPartDataHolderComponent* holder = kvp.Value(); // <-- FIXED

            std::string partId = "NONE";
            if (holder)
            {
                FName id = holder->GetPartID();
                partId = id.ToString();
            }

            out << "    SlotType=" << (int)slotType
                << " PartID=" << partId << "\n";
        }

        out << "\n";
    }

    out << "=== END WEAPON PARTS DUMP ===\n";
    out.close();
}

//hotkey dump
void HotkeyThread()
{
    while (true)
    {
        // F5 pressed
        if (GetAsyncKeyState(VK_F5) & 0x8000)
        {
            DebugDumpSubsystemsToFile();
            ClientLog("[CLIENT] Auto-enter Shooting Range...");
            DebugDumpWeaponPartsToFile();
            ClientLog("[CLIENT] Auto-enter Shooting Range...");
            // simple debounce so it doesn't spam while held
            Sleep(300);
        }

        // F9 pressed
        if (GetAsyncKeyState(VK_F9) & 0x8000)
        {
            UPBLocalPlayer* LP = nullptr;
            auto* GI = UWorld::GetWorld()->OwningGameInstance;

            if (GI && GI->LocalPlayers.Num() > 0)
            {
                LP = (UPBLocalPlayer*)GI->LocalPlayers[0];
                if (LP)
                {
                    ClientLog("[CLIENT] Auto-enter Shooting Range...");
                    LP->GoToRange(0.0f);
                }
            }
            Sleep(300);
        }
        Sleep(10);
    }
}

