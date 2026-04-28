// ======================================================
//  LateJoinManager — 中途加入（Mid-Game Join）实现
// ======================================================
//
//  中途加入流程概述：
//    1. PostLogin 检测 → 判定为中途加入 → 注册到 LateJoinPlayers
//    2. 延迟 1s 发送 ClientStart 序列，让客户端"追上"比赛状态
//    3. 客户端选择角色 → 拦截 ServerConfirmRoleSelection → 状态推进
//    4. 每隔 2s 尝试生成 Pawn（最多 3 次，逐步回退）
//    5. 检测到非旁观者 Pawn → 强制 Possess + 通知客户端进入 Playing
//
//  与其他系统的关系：
//    - PlayerRespawnAllowedMap：与 Respawn/Death 系统共享，
//      LateJoin 设置为 true 允许重生，ClientBeKilled 设置为 false 阻止重生
//    - DidProcStartMatch：由匹配流程设置，LateJoin 只读取
//    - LoadoutManager：通过回调介入角色确认后的装备覆盖

#include "LateJoinManager.h"
#include "../SDK.hpp"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include <iostream>

using namespace SDK;

// =====================================================================
//  构造函数
// =====================================================================

LateJoinManager::LateJoinManager(
    const bool& InDidProcStartMatch,
    std::unordered_map<APBPlayerController*, bool>& InPlayerRespawnAllowedMap,
    FReportRoomStarted InReportRoomStarted
)
    : DidProcStartMatch(InDidProcStartMatch)
    , PlayerRespawnAllowedMap(InPlayerRespawnAllowedMap)
    , ReportRoomStarted(std::move(InReportRoomStarted))
{
}

// =====================================================================
//  公有接口 — Hook 层调用入口
// =====================================================================

// @brief PostLogin Hook 入口。
//  检测新连接玩家是否需要中途加入流程：
//  - 若比赛已开始或回合进行中 → 注册为中途加入玩家并返回 true
//  - 否则返回 false，由调用方执行正常首生逻辑
bool LateJoinManager::OnPostLogin(AGameMode* GameMode, APBPlayerController* PC)
{
    if (PC && IsLateJoinWindowOpen())
    {
        QueueLateJoinPlayer(PC);

        // 通知后端房间已启动（如中途加入玩家触发了首次上报）
        if (ReportRoomStarted)
            ReportRoomStarted();

        return true;
    }

    return false;
}

// @brief ProcessEvent Hook 入口。
//  拦截以下 RPC 并处理：
//    - CanPlayerSelectRole     → 对中途加入玩家强制返回 true
//    - CanSelectRole           → 对中途加入玩家强制返回 true
//    - ServerConfirmRoleSelection → 执行原函数 + 装备覆盖 + 推进状态机
//  返回 true 表示已完全拦截，调用方应 return 跳过原始 ProcessEvent。
bool LateJoinManager::OnProcessEvent(UObject* Object, const std::string& functionName, void* Parms)
{
    // ---- CanPlayerSelectRole（PBGameMode 级别）----
    // 中途加入玩家在回合进行中原本无法选择角色，此处强制放行
    if (functionName.contains("CanPlayerSelectRole"))
    {
        auto* RoleParms = (Params::PBGameMode_CanPlayerSelectRole*)Parms;
        if (RoleParms && IsLateJoinPlayer(RoleParms->Player))
        {
            RoleParms->ReturnValue = true;
            return true;    // 已拦截，跳过原始调用
        }
    }

    // ---- CanSelectRole（PBPlayerController 级别）----
    // 同上，对中途加入玩家强制允许
    if (functionName.contains("CanSelectRole"))
    {
        APBPlayerController* PBPlayerController = Object && Object->IsA(APBPlayerController::StaticClass())
            ? (APBPlayerController*)Object
            : nullptr;

        if (IsLateJoinPlayer(PBPlayerController))
        {
            auto* RoleParms = (Params::PBPlayerController_CanSelectRole*)Parms;
            if (RoleParms)
            {
                RoleParms->ReturnValue = true;
                return true;    // 已拦截
            }
        }
    }

    // 注意：ServerConfirmRoleSelection 不在此处处理。
    // 该 RPC 需要调用方先执行原始 ProcessEvent.call 再推进状态，
    // 因此由调用方在 Hook 体内使用 IsLateJoinPlayer() + OnRoleConfirmed() 处理。

    return false;   // 未拦截，调用方继续正常流程
}

// @brief ServerConfirmRoleSelection 后的状态推进
//  将中途加入玩家的状态从 PendingRoleSelection 推进到 RoleConfirmed，
//  并重置生成计时器。
void LateJoinManager::OnRoleConfirmed(APBPlayerController* PC)
{
    auto it = LateJoinPlayers.find(PC);
    if (it != LateJoinPlayers.end())
    {
        it->second.State = ELateJoinState::RoleConfirmed;
        it->second.ElapsedSeconds = 0.0f;
        it->second.SpawnAttempts = 0;
        std::cout << "[LATEJOIN] Role confirmed; scheduling single-player spawn." << std::endl;
    }
}

// @brief 每帧驱动中途加入状态机。
//  由 TickFlush Hook 调用，遍历所有已注册的中途加入玩家，
//  根据当前阶段推进状态或执行超时清理。
void LateJoinManager::Tick(float DeltaTime)
{
    for (auto it = LateJoinPlayers.begin(); it != LateJoinPlayers.end();)
    {
        APBPlayerController* PC = it->first;
        FLateJoinInfo& Info = it->second;

        // 无效 PC → 清理
        if (!PC)
        {
            it = LateJoinPlayers.erase(it);
            continue;
        }

        Info.ElapsedSeconds += DeltaTime;

        // ---- 检测生成成功 ----
        // 如果角色已确认且已尝试过生成，且现在拥有非旁观者 Pawn，则完成
        if (Info.State == ELateJoinState::RoleConfirmed
            && Info.SpawnAttempts > 0
            && HasPlayableLateJoinPawn(PC))
        {
            FinalizeLateJoinSpawn(PC);
            Info.State = ELateJoinState::Spawned;
            std::cout << "[LATEJOIN] Spawn complete for late join player." << std::endl;
            it = LateJoinPlayers.erase(it);
            continue;
        }

        // ---- Phase 1: 等待角色选择 ----
        if (Info.State == ELateJoinState::PendingRoleSelection)
        {
            // 延迟 1s 后发送 ClientStart 序列（等待连接稳定）
            if (!Info.ClientStartSent && Info.ElapsedSeconds >= CLIENT_START_DELAY_SEC)
            {
                SendLateJoinClientStart(PC);
                Info.ClientStartSent = true;
                Info.ElapsedSeconds = 0.0f;
            }
            // 超时 30s 未选择角色 → 放弃
            else if (Info.ClientStartSent && Info.ElapsedSeconds >= ROLE_SELECTION_TIMEOUT)
            {
                Info.State = ELateJoinState::TimedOut;
                std::cout << "[LATEJOIN] Timed out waiting for role selection." << std::endl;
            }
        }
        // ---- Phase 2: 角色已确认，尝试生成 Pawn ----
        else if (Info.State == ELateJoinState::RoleConfirmed)
        {
            // 首次立即尝试，之后每 2s 重试
            if (Info.SpawnAttempts == 0 || Info.ElapsedSeconds >= SPAWN_RETRY_INTERVAL)
            {
                if (Info.SpawnAttempts < MAX_SPAWN_ATTEMPTS)
                {
                    RequestLateJoinSpawn(PC, Info);
                }
                else
                {
                    Info.State = ELateJoinState::TimedOut;
                    std::cout << "[LATEJOIN] Timed out spawning late join player." << std::endl;
                }
            }
        }

        // ---- 终态清理 ----
        if (Info.State == ELateJoinState::TimedOut)
        {
            it = LateJoinPlayers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// @brief 查询指定 PC 是否为中途加入玩家
bool LateJoinManager::IsLateJoinPlayer(APBPlayerController* PC) const
{
    return PC && LateJoinPlayers.find(PC) != LateJoinPlayers.end();
}

// @brief 查询中途加入窗口是否开放
//  条件：比赛已调用 StartMatch（DidProcStartMatch）或回合正在进行中
bool LateJoinManager::IsLateJoinWindowOpen() const
{
    return DidProcStartMatch || IsRoundCurrentlyInProgress();
}

// =====================================================================
//  私有方法 — 状态查询
// =====================================================================

APBGameState* LateJoinManager::GetPBGameState() const
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode || !World->AuthorityGameMode->GameState)
        return nullptr;

    return (APBGameState*)World->AuthorityGameMode->GameState;
}

APBGameMode* LateJoinManager::GetPBGameMode() const
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return nullptr;

    return (APBGameMode*)World->AuthorityGameMode;
}

bool LateJoinManager::IsRoundCurrentlyInProgress() const
{
    APBGameState* GameState = GetPBGameState();
    return GameState && GameState->IsRoundInProgress();
}

// @brief 判断 Pawn 是否为旁观者（SpectatorPawn）
bool LateJoinManager::IsSpectatorPawn(APawn* Pawn)
{
    return Pawn && Pawn->IsA(ASpectatorPawn::StaticClass());
}

// @brief 判断玩家是否拥有可玩的（非旁观者）Pawn
bool LateJoinManager::HasPlayableLateJoinPawn(APBPlayerController* PC)
{
    return PC && PC->Pawn && !IsSpectatorPawn(PC->Pawn);
}

// =====================================================================
//  私有方法 — 状态机动作
// =====================================================================

// @brief 将玩家注册为中途加入，初始化跟踪信息
void LateJoinManager::QueueLateJoinPlayer(APBPlayerController* PC)
{
    if (!PC)
        return;

    LateJoinPlayers[PC] = FLateJoinInfo{};
    PlayerRespawnAllowedMap[PC] = true;
    std::cout << "[LATEJOIN] Queued player for in-progress join: " << PC->GetFullName() << std::endl;
}

// @brief 向中途加入客户端发送"比赛已开始"的完整通知序列
//  模拟正常比赛启动时的 RPC 序列，让客户端 UI 状态追上
void LateJoinManager::SendLateJoinClientStart(APBPlayerController* PC)
{
    if (!PC)
        return;

    std::cout << "[LATEJOIN] Sending in-progress match state and role selection." << std::endl;
    PC->ClientStartOnlineGame();
    PC->ClientMatchHasStarted();
    PC->ClientRoundHasStarted();
    PC->NotifyGameStarted();
    PC->ClientSelectRole();
}

// @brief 准备重生前的清理工作
//  清除旁观者状态、解锁输入、释放旁观者 Pawn
void LateJoinManager::PrepareLateJoinRespawn(APBPlayerController* PC)
{
    if (!PC)
        return;

    PlayerRespawnAllowedMap[PC] = true;
    PC->ServerSetSpectatorWaiting(false);
    PC->ClientSetSpectatorWaiting(false);
    PC->SetIgnoreMoveInput(false);
    PC->SetIgnoreLookInput(false);
    PC->ClientIgnoreMoveInput(false);
    PC->ClientIgnoreLookInput(false);

    // 如果当前是旁观者 Pawn → 退出观察模式并释放
    if (PC->Pawn && IsSpectatorPawn(PC->Pawn))
    {
        std::cout << "[LATEJOIN] Clearing spectator pawn before playable spawn: "
            << PC->Pawn->GetFullName() << std::endl;
        PC->ExitObserverState();
        PC->UnPossess();
    }
}

// @brief 生成成功后的最终化操作
//  强制 Possess、通知客户端进入 Playing 状态、确认占有
void LateJoinManager::FinalizeLateJoinSpawn(APBPlayerController* PC)
{
    if (!HasPlayableLateJoinPawn(PC))
        return;

    // 确保重生许可和输入解锁（防御性重置）
    PlayerRespawnAllowedMap[PC] = true;
    PC->ServerSetSpectatorWaiting(false);
    PC->ClientSetSpectatorWaiting(false);
    PC->SetIgnoreMoveInput(false);
    PC->SetIgnoreLookInput(false);
    PC->ClientIgnoreMoveInput(false);
    PC->ClientIgnoreLookInput(false);
    PC->ExitObserverState();

    // 强制 Possess（如果 Pawn 的 Controller 不是当前 PC）
    if (PC->Pawn)
    {
        if (PC->Pawn->Controller != (AController*)PC)
        {
            std::cout << "[LATEJOIN] Forcing possess on spawned pawn: "
                << PC->Pawn->GetFullName() << std::endl;
            PC->Possess(PC->Pawn);
        }

        PC->Pawn->ForceNetUpdate();
    }

    // 向客户端发送完整的"已就绪"通知序列
    PC->ForceNetUpdate();
    PC->ClientReadyAtStartSpot();
    PC->NotifyGameStarted();
    PC->ClientGotoState(UKismetStringLibrary::Conv_StringToName(L"Playing"));
    PC->ClientRestart(PC->Pawn);
    PC->ClientRetryClientRestart(PC->Pawn);
    PC->ServerAcknowledgePossession(PC->Pawn);

    std::cout << "[LATEJOIN] Finalized playable possession: "
        << PC->Pawn->GetFullName() << std::endl;
}

// @brief 执行生成尝试（3 级回退策略）
//  Attempt 0: RestartPlayers — 标准引擎生成路径
//  Attempt 1: ServerQuickRespawn — 快速重生
//  Attempt 2: ServerSuicide — 自杀触发重生（最后手段）
void LateJoinManager::RequestLateJoinSpawn(APBPlayerController* PC, FLateJoinInfo& Info)
{
    if (!PC)
        return;

    PrepareLateJoinRespawn(PC);
    PlayerRespawnAllowedMap[PC] = true;

    APBGameMode* GameMode = GetPBGameMode();

    if (Info.SpawnAttempts == 0 && GameMode)
    {
        // 第 1 次：通过 GameMode 的标准 RestartPlayers 生成
        TArray<AController*> Controllers{};
        Controllers.Add((AController*)PC);
        std::cout << "[LATEJOIN] RestartPlayers for late join player." << std::endl;
        GameMode->RestartPlayers(Controllers);
    }
    else if (Info.SpawnAttempts == 1)
    {
        // 第 2 次：RestartPlayers 未生效，尝试 ServerQuickRespawn
        std::cout << "[LATEJOIN] RestartPlayers did not produce a pawn; trying ServerQuickRespawn." << std::endl;
        PC->ServerQuickRespawn();
    }
    else if (Info.SpawnAttempts == 2)
    {
        // 第 3 次：QuickRespawn 也未生效，用 ServerSuicide 触发重生链
        std::cout << "[LATEJOIN] Quick respawn did not produce a pawn; trying ServerSuicide fallback." << std::endl;
        PC->ServerSuicide(0);
    }

    Info.SpawnAttempts++;
    Info.ElapsedSeconds = 0.0f;
}
