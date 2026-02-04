// dllmain.cpp : Defines the entry point for the DLL application.
#include <thread>
#include <Windows.h>
#include "SDK.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/ProjectBoundary_parameters.hpp"
#include "safetyhook/safetyhook.hpp"
#include "json.hpp"
#include <iostream>
#include <fstream>

#include "libreplicate.h"

using namespace SDK;

static LibReplicate* libReplicate;

uintptr_t BaseAddress = 0x0;

bool listening = false;

bool amServer = false;

struct ServerConfig {
    std::wstring MapName;
    std::wstring FullModePath;
    unsigned int Port;
    bool NumPlayersToStartAt;
};

static ServerConfig Config{};

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

void EnableUnrealConsole() {
    SDK::UInputSettings::GetDefaultObj()->ConsoleKeys[0].KeyName = SDK::UKismetStringLibrary::Conv_StringToName(L"F2");

    /* Creates a new UObject of class-type specified by Engine->ConsoleClass */
    SDK::UObject* NewObject = SDK::UGameplayStatics::SpawnObject(UEngine::GetEngine()->ConsoleClass, UEngine::GetEngine()->GameViewport);

    /* The Object we created is a subclass of UConsole, so this cast is **safe**. */
    UEngine::GetEngine()->GameViewport->ViewportConsole = static_cast<SDK::UConsole*>(NewObject);

    std::cout << "[DEBUG] Unreal Console => F2" << std::endl;
}

SafetyHookInline TickFlush = {};

std::vector<APlayerController*> playerControllersPossessed = std::vector<APlayerController*>();

void TickFlushHook(UNetDriver* NetDriver, float DeltaTime) {
    if (listening && NetDriver && UWorld::GetWorld()) {
        UWorld::GetWorld()->NetDriver = NetDriver;
        NetDriver->World = UWorld::GetWorld();

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

        /*
        for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
        {
            SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

            if (!Obj)
                continue;

            if (Obj->HasTypeFlag(EClassCastFlags::PlayerController) && !Obj->IsDefaultObject()) {
                PlayerControllers.push_back((void*)Obj);
                continue;
            }

            if (Obj->HasTypeFlag(EClassCastFlags::Actor) && !((AActor*)Obj)->bActorIsBeingDestroyed && !Obj->IsDefaultObject() && ((AActor*)Obj)->RemoteRole != ENetRole::ROLE_None) {
                LibReplicate::FActorInfo ActorInfo = LibReplicate::FActorInfo((void*)Obj, ((AActor*)Obj)->bNetTemporary);

                ActorInfos.push_back(ActorInfo);
            }
        }
        */

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
            //std::cout << "TRYING TO REPLICATE" << std::endl;
            if (NetDriver) {
                libReplicate->CallFromTickFlushHook(ActorInfos, PlayerControllerInfos, CastConnections, ActorName, NetDriver);
                *(int*)(&NetDriver + 0x420) = *(int*)(&NetDriver + 0x420) + 1;
            }
        }
    }

    if (!((APBGameState*)(UWorld::GetWorld()->AuthorityGameMode->GameState))->IsRoundInProgress()) {
        for (UNetConnection* pc : NetDriver->ClientConnections) {
            if (pc->PlayerController && pc->PlayerController->Pawn)
                pc->PlayerController->Possess(pc->PlayerController->Pawn);
        }
    }

    if (GetAsyncKeyState(VK_F7) && amServer) {
        /*
        ((APBGameState*)((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->PBGameState)->PlayerJoinsGameMatch((APBPlayerState*)GetLastOfType(APBPlayerState::StaticClass(), false));
        ((APBGameState*)((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->PBGameState)->K2_StartRoleSelection();
        */
        //((UPBWorldManagerV2*)GetLastOfType(UPBWorldManagerV2::StaticClass(), false))->FinishLoadSubLevel();
        /*
        ((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->ModeRuleSetting.bAllowQuickRespawn = true;
        ((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->ModeRuleSetting.QuickRespawnCoolDownTime = 0.1f;
        ((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->ModeRuleSetting.RespawnWaveIntervalTime = 1;
                */
        ((APBGameMode*)UWorld::GetWorld()->AuthorityGameMode)->StartMatch();


        while (GetAsyncKeyState(VK_F7)) {

        }
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
                if (((APBPlayerController*)Obj)->CanSelectRole()) {
                    ((APBPlayerController*)Obj)->ClientSelectRole();
                }
                else {
                    std::cout << "CANT SELECT ROLE WEE WOO WEE WOO" << std::endl;
                }
            }
        }

        while (GetAsyncKeyState(VK_F8)) {

        }
    }

    if (GetAsyncKeyState(VK_F9) && amServer) {
        for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
        {
            SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

            if (!Obj)
                continue;

            if (Obj->IsDefaultObject())
                continue;

            if (Obj->IsA(APBPlayerController::StaticClass()))
            {
                ((APBPlayerController*)Obj)->K2_PawnReplicatedPossess();
                ((APBPlayerController*)Obj)->NotifyStopKillCamera();
                ((APBPlayerController*)Obj)->StopKillCamera();
                ((APBPlayerController*)Obj)->StopThirdPersonCamera();
            }
        }

        while (GetAsyncKeyState(VK_F9)) {

        }
    }

    if (GetAsyncKeyState(VK_F10) && amServer) {
        for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; i--)
        {
            SDK::UObject* Obj = SDK::UObject::GObjects->GetByIndex(i);

            if (!Obj)
                continue;

            if (Obj->IsDefaultObject())
                continue;

            if (Obj->IsA(APBPlayerController::StaticClass()))
            {
                std::cout << Obj->GetFullName() << std::endl;
                ((APBPlayerController*)Obj)->Possess(((APBPlayerController*)Obj)->Pawn);
            }
        }

        while (GetAsyncKeyState(VK_F10)) {

        }
    }

    return TickFlush.call(NetDriver, DeltaTime);
}

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

SafetyHookInline NotifyAboutSomeShitImTooTiredToLookup = {};

__int64 NotifyAcceptingConnectionHook(UObject* obj) {
    return 1;
}

SafetyHookInline NotifyControlMessage = {};

char NotifyControlMessageHook(unsigned __int64 ScuffedShit, __int64 a2, uint8_t a3, __int64 a4) {
    UWorld::GetWorld()->NetDriver = (UIpNetDriver*)GetLastOfType(UIpNetDriver::StaticClass(), false);

    return NotifyControlMessage.call<char>(ScuffedShit, a2, a3, a4);
}

SafetyHookInline ViewportShit = {};

char ViewportShitHook(__int64 a1, float a2) {
    return 1;
}

//__int64 *__fastcall sub_141561A60(__int64 a1, __int64 *a2, unsigned __int64 a3, unsigned __int8 a4)

int NumPlayersSelectedRole = 0;

SafetyHookInline ProcessEvent;

bool canStartMatch = false;

bool AllowedToRespawn = false;

std::unordered_map<APBPlayerController*, bool> PlayerRespawnAllowedMap{};

void ProcessEventHook(UObject* Object, UFunction* Function, void* Parms) {
    if (Function->GetFullName().contains("QuickRespawn")) {
        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = true;
    }

    if (Function->GetFullName().contains("ServerRestartPlayer") ) {
        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        if (PlayerRespawnAllowedMap.contains(PBPlayerController) && PlayerRespawnAllowedMap[PBPlayerController] == false) {
            std::cout << "Denied restart!" << std::endl;
            return;
        }
    }

    //ReceivePossess

    if (Function->GetFullName().contains("ServerConfirmRoleSelection")) {
        NumPlayersSelectedRole++;

        if (!canStartMatch && NumPlayersSelectedRole >= Config.NumPlayersToStartAt) {
            canStartMatch = true;
        }
    }


    if (Function->GetFullName().contains("ReadyToMatchIntro_WaitingToStart")) {
        if (!canStartMatch) {
            return;
        }
    }

    if (Function->GetFullName().contains("ClientBeKilled")) {
        std::cout << "Intercepted Player Kill!" << std::endl;

        APBPlayerController* PBPlayerController = (APBPlayerController*)Object;

        PlayerRespawnAllowedMap[PBPlayerController] = false;
        //std::cout << "[PE] " << Object->GetFullName() << " - " << Function->GetFullName() << std::endl;
    }

    if (Function->GetFullName().contains("PlayerCanRestart")) {
        std::cout << "YEPPERS WE CAN RESTART" << std::endl;
        ((Params::GameModeBase_PlayerCanRestart*)Parms)->ReturnValue = ((AGameModeBase*)Object)->HasMatchStarted();
        /*
        if (!((AGameModeBase*)Object)->HasMatchStarted()) {
            if (!((APBPlayerController*)((Params::GameModeBase_PlayerCanRestart*)Parms)->Player)->bShowSelectRole) {
                ((APBPlayerController*)((Params::GameModeBase_PlayerCanRestart*)Parms)->Player)->ClientSelectRole();
            }
        }
        */
        return;
    }

    return ProcessEvent.call(Object, Function, Parms);
}

SafetyHookInline HudFunctionThatCrashesTheGame;

__int64 HudFunctionThatCrashesTheGameHook(__int64 a1, __int64 a2) {
    return 0;
}

SafetyHookInline ProcessEventClient;

void ProcessEventHookClient(UObject* Object, UFunction* Function, void* Parms) {
    if (Function->GetFullName().contains("HasItem")) {
        std::cout << "[PE] " << Object->GetFullName() << " - " << Function->GetFullName() << std::endl;
    }

    return ProcessEventClient.call(Object, Function, Parms);
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

SafetyHookInline ClientDeathCrash;

__int64 ClientDeathCrashHook(__int64 a1) {
    return 0;
}

SafetyHookInline ObjectNeedsLoad;

char ObjectNeedsLoadHook(UObject* a1) {
    return 1;
}

SafetyHookInline ActorNeedsLoad;

char ActorNeedsLoadHook(UObject* a1) {
    return 1;
}

void InitServerHooks() {
    NotifyActorDestroyed = safetyhook::create_inline((void*)(BaseAddress + 0x33403E0), NotifyActorDestroyedHook);
    NotifyAcceptingConnection = safetyhook::create_inline((void*)(BaseAddress + 0x36CDC90), NotifyAcceptingConnectionHook);
    NotifyControlMessage = safetyhook::create_inline((void*)(BaseAddress + 0x36CDCE0), NotifyControlMessageHook);
    TickFlush = safetyhook::create_inline((void*)(BaseAddress + 0x33E05F0), TickFlushHook);
    ProcessEvent = safetyhook::create_inline((void*)(BaseAddress + 0x1BCBE40), ProcessEventHook);
    ObjectNeedsLoad = safetyhook::create_inline((void*)(BaseAddress + 0x1B7B710), ObjectNeedsLoadHook);
    ActorNeedsLoad = safetyhook::create_inline((void*)(BaseAddress + 0x3124E70), ActorNeedsLoadHook);

    //GameEngineTick = safetyhook::create_inline((void*)(BaseAddress + 0x350b3a0), GameEngineTickHook);
    //HudFunctionThatCrashesTheGame = safetyhook::create_inline((void*)(BaseAddress + 0x180B060), HudFunctionThatCrashesTheGameHook);

    //NotifyAboutSomeShitImTooTiredToLookup = safetyhook::create_inline((void*)(BaseAddress + 0x91D770), NotifyAcceptingConnectionHook);
    //ViewportShit = safetyhook::create_inline((void*)(BaseAddress + 0x33E0420), ViewportShitHook);
}

void InitClientHook() {
    ProcessEventClient = safetyhook::create_inline((void*)(BaseAddress + 0x1BCBE40), ProcessEventHookClient);
    ClientDeathCrash = safetyhook::create_inline((void*)(BaseAddress + 0x16abe10), ClientDeathCrashHook);
}

void LoadConfig() {
    Config.FullModePath = L"/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Easy.BP_PBGameMode_Rush_PVE_Easy_C";
    Config.MapName = L"DataCenter";
    Config.NumPlayersToStartAt = 1;
    Config.Port = 7777;
}

bool hidden = false;

const wchar_t* LocalURL = L"http://127.0.0.1:8000\0";

void MainThread() {
    AllocConsole();
    FILE* Dummy;
    freopen_s(&Dummy, "CONOUT$", "w", stdout);
    freopen_s(&Dummy, "CONIN$", "r", stdin);

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

    if (amServer) {
        InitServerHooks();

        LoadConfig();

        std::wstring cmd = L"open " + Config.MapName + L"?game=" + Config.FullModePath;

        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), cmd.c_str(), nullptr); //

        Sleep(8 * 1000);

        while (!UWorld::GetWorld()) {

        }

        libReplicate = new LibReplicate(LibReplicate::EReplicationMode::Minimal, (void*)(BaseAddress + 0x91AEB0), (void*)(BaseAddress + 0x33A66D0), (void*)(BaseAddress + 0x31F44F0), (void*)(BaseAddress + 0x31F0070), (void*)(BaseAddress + 0x18F1810), (void*)(BaseAddress + 0x18E5490), (void*)(BaseAddress + 0x36CDCE0), (void*)(BaseAddress + 0x366ADB0), (void*)(BaseAddress + 0x31DA270), (void*)(BaseAddress + 0x33DF330), (void*)(BaseAddress + 0x2fefbd0), (void*)(BaseAddress + 0x3506320));
    
        UEngine* Engine = UEngine::GetEngine();
        UWorld* World = UWorld::GetWorld();

        FName name = UKismetStringLibrary::Conv_StringToName(L"GameNetDriver");

        libReplicate->CreateNetDriver(Engine, World, &name);

        UIpNetDriver* NetDriver = (UIpNetDriver*)GetLastOfType(UIpNetDriver::StaticClass(), false);

        std::cout << NetDriver->GetFullName() << std::endl;
        
        World->NetDriver = NetDriver;

        libReplicate->Listen((void*)NetDriver, (void*)World, LibReplicate::EJoinMode::Open, Config.Port);

        World->NetDriver = NetDriver;

        listening = true;
    }
    else {
        EnableUnrealConsole();

        InitClientHook();

        //*(const wchar_t***)(BaseAddress + 0x5C63C88) = &LocalURL;


        for (UObject* obj : getObjectsOfClass(UPBArmoryManager::StaticClass(), false)) {
            UPBArmoryManager* DefaultConfig = (UPBArmoryManager*)obj;

            std::ifstream items("DT_ItemType.json");
            nlohmann::json itemJson = nlohmann::json::parse(items);

            for (auto& [ItemId, _] : itemJson[0]["Rows"].items()) {
                std::string aString = std::string(ItemId.c_str());

                std::wstring wString = std::wstring(aString.begin(), aString.end());
                if(DefaultConfig->DefaultConfig)
                    DefaultConfig->DefaultConfig->OwnedItems.Add(UKismetStringLibrary::Conv_StringToName(wString.c_str()));

                FPBItem item{};

                item.ID = UKismetStringLibrary::Conv_StringToName(wString.c_str());
                item.Count = 1;
                item.bIsNew = false;

                DefaultConfig->Armorys.OwnedItems.Add(item);
            }
        }

        Sleep(5 * 1000);

        //UCommonActivatableWidget* widget = nullptr;
        //reinterpret_cast<UPBMainMenuManager_BP_C*>(getObjectsOfClass(UPBMainMenuManager_BP_C::StaticClass(), false).back())->GetTopMenuWidget(&widget);
        //widget->SetVisibility(ESlateVisibility::Hidden);
        //widget->DeactivateWidget();

        //reinterpret_cast<UUMG_MainMenuLayout_C*>(getObjectsOfClass(UUMG_MainMenuLayout_C::StaticClass(), false).back())->OnJoinGame();

        //UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 127.0.0.1", nullptr);

        //UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 127.0.0.1", nullptr);
    }
}

BOOL APIENTRY DllMain( HMODULE hModule,
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

