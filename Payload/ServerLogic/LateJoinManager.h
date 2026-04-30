#pragma once

// ======================================================
//  LateJoinManager — 中途加入（Mid-Game Join）状态机
// ======================================================
//
//  职责：
//    管理比赛进行中新连接玩家从"进入"到"可玩"的完整生命周期，
//    包括角色选择、Pawn 生成、客户端状态同步。
//
//  使用方式：
//    1. 构造实例，注入外部依赖（见构造函数参数）
//    2. 在 PostLogin Hook 中调用 OnPostLogin()
//    3. 在 ProcessEvent Hook 中调用 OnProcessEvent()
//    4. 在 TickFlush Hook 中调用 Tick()
//
//  设计原则：
//    - 所有 LateJoin 内部状态完全私有封装
//    - 外部共享状态（PlayerRespawnAllowedMap、DidProcStartMatch）
//      通过引用注入，不获取所有权
//    - Hook 层仅需一行调用，拦截逻辑由本类内部处理

#include <unordered_map>
#include <functional>
#include <string>

// Forward declarations — 与 SDK 命名空间保持一致
namespace SDK
{
    class APBPlayerController;
    class APawn;
    class APBGameState;
    class APBGameMode;
    class AGameMode;
    class UObject;
}

class LateJoinManager
{
public:
    // ------------------------------------------------------------------
    //  嵌套类型 — 中途加入状态机
    // ------------------------------------------------------------------

    // @brief 中途加入玩家所处的阶段
    enum class ELateJoinState
    {
        PendingRoleSelection,   // 等待客户端选择角色
        RoleConfirmed,          // 角色已确认，准备生成可玩 Pawn
        Spawned,                // 已成功生成可玩 Pawn（终态，将移除）
        TimedOut                // 超时放弃（终态，将移除）
    };

    // @brief 单个中途加入玩家的跟踪信息
    struct FLateJoinInfo
    {
        ELateJoinState State = ELateJoinState::PendingRoleSelection;
        float ElapsedSeconds = 0.0f;    // 当前阶段已持续时间（秒）
        int   SpawnAttempts   = 0;      // 已尝试生成的次数（最多 3 次）
        bool  ClientStartSent = false;   // 是否已发送 ClientStart 序列
        bool  bIsInitialJoin  = false;   // 是否为初始加入（比赛未开始时连接）
    };

    // ------------------------------------------------------------------
    //  回调类型定义
    // ------------------------------------------------------------------

    // @brief 用于通知外部系统房间已启动（如后端心跳上报）
    using FReportRoomStarted = std::function<void()>;

    // ------------------------------------------------------------------
    //  构造 / 初始化
    // ------------------------------------------------------------------

    // @param InDidProcStartMatch        引用 — 比赛是否已调用过 StartMatch
    // @param InPlayerRespawnAllowedMap  引用 — 玩家重生许可表（与 Respawn 系统共享）
    // @param InReportRoomStarted        回调 — 通知后端房间已启动
    LateJoinManager(
        const bool& InDidProcStartMatch,
        std::unordered_map<SDK::APBPlayerController*, bool>& InPlayerRespawnAllowedMap,
        FReportRoomStarted InReportRoomStarted = nullptr
    );

    // ------------------------------------------------------------------
    //  公有接口 — Hook 层调用入口
    // ------------------------------------------------------------------

    // @brief PostLogin Hook 中调用。检测是否为中途加入并注册玩家。
    // @param GameMode  当前 GameMode
    // @param PC        新连接的 PlayerController
    // @return true 表示已作为中途加入处理，调用方应跳过正常首生逻辑
    bool OnPostLogin(SDK::AGameMode* GameMode, SDK::APBPlayerController* PC);

    // @brief PostLogin Hook 中调用。将初始加入玩家注册到与中途加入一致的流程。
    //        初始加入也会经过 ClientStart 序列与延迟 Pawn 生成路径，
    //        以统一客户端状态推进与 UI 生命周期。
    // @param GameMode  当前 GameMode
    // @param PC        新连接的 PlayerController
    void QueueInitialJoinPlayer(SDK::AGameMode* GameMode, SDK::APBPlayerController* PC);

    // @brief ProcessEvent Hook 中调用。拦截角色选择相关的 RPC。
    //        仅处理 CanPlayerSelectRole / CanSelectRole（强制返回 true）。
    //        ServerConfirmRoleSelection 需要调用方先执行原函数再用 OnRoleConfirmed 推进状态，
    //        因为此处无法访问 SafetyHook 的原始调用。
    // @param Object       ProcessEvent 的目标对象
    // @param functionName 函数全名（用于匹配）
    // @param Parms        参数缓冲区（可能被修改）
    // @return true 表示已拦截并处理，调用方应 return 跳过原始 ProcessEvent
    bool OnProcessEvent(SDK::UObject* Object, const std::string& functionName, void* Parms);

    // @brief ServerConfirmRoleSelection 拦截后的状态推进。
    //        调用方应先检查 IsLateJoinPlayer()，若为 true 则：
    //          1. 执行原始 ProcessEvent.call(Object, Function, Parms)
    //          2. 调用 LoadoutManager::OnServerProcessEventPost
    //          3. 调用本方法
    //          4. return 跳过正常计数逻辑
    // @param PC 角色确认的 PlayerController
    void OnRoleConfirmed(SDK::APBPlayerController* PC);

    // @brief TickFlush Hook 中调用。驱动状态机每帧更新。
    // @param DeltaTime 帧间隔时间（秒）
    void Tick(float DeltaTime);

    // @brief 查询指定 PC 是否为中途加入玩家（供其他系统判断）
    bool IsLateJoinPlayer(SDK::APBPlayerController* PC) const;

    // @brief 查询指定 PC 是否为初始加入玩家（注册到延迟生成流程但比赛未开始时连接）
    bool IsInitialJoinPlayer(SDK::APBPlayerController* PC) const;

    // @brief 查询中途加入窗口是否开放（比赛已开始或回合进行中）
    bool IsLateJoinWindowOpen() const;

private:
    // ------------------------------------------------------------------
    //  私有类型 — 客户端状态同步配置
    // ------------------------------------------------------------------

    struct FClientSyncOptions
    {
        bool SendStartOnlineGame = false;
        bool SendMatchHasStarted = false;
        bool SendRoundHasStarted = false;
        bool SendNotifyGameStarted = false;
        bool SendClientSelectRole = false;
        bool SendReadyAtStartSpot = false;
        bool SendGotoPlaying = false;
        bool SendRestartAndAcknowledge = false;
    };

    // ------------------------------------------------------------------
    //  外部依赖（引用，不拥有）
    // ------------------------------------------------------------------

    const bool& DidProcStartMatch;                                                      // 比赛是否已启动
    std::unordered_map<SDK::APBPlayerController*, bool>& PlayerRespawnAllowedMap;       // 重生许可表
    FReportRoomStarted        ReportRoomStarted;                                        // 后端上报回调

    // ------------------------------------------------------------------
    //  内部状态
    // ------------------------------------------------------------------

    std::unordered_map<SDK::APBPlayerController*, FLateJoinInfo> LateJoinPlayers;

    // ------------------------------------------------------------------
    //  可配置常量 — 未来可提取为配置项
    // ------------------------------------------------------------------

    static constexpr float CLIENT_START_DELAY_SEC   = 1.0f;   // 连接后延迟多久发送 ClientStart
    static constexpr float ROLE_SELECTION_TIMEOUT   = 30.0f;  // 等待角色选择超时
    static constexpr float SPAWN_RETRY_INTERVAL     = 2.0f;   // 生成重试间隔
    static constexpr int   MAX_SPAWN_ATTEMPTS       = 3;       // 最大生成尝试次数

    // ------------------------------------------------------------------
    //  私有方法 — 状态查询
    // ------------------------------------------------------------------

    SDK::APBGameState* GetPBGameState() const;
    SDK::APBGameMode*  GetPBGameMode() const;
    bool          IsRoundCurrentlyInProgress() const;
    static bool   IsSpectatorPawn(SDK::APawn* Pawn);
    static bool   HasPlayableLateJoinPawn(SDK::APBPlayerController* PC);

    // ------------------------------------------------------------------
    //  私有方法 — 状态机动作
    // ------------------------------------------------------------------

    void QueueLateJoinPlayer(SDK::APBPlayerController* PC);
    void SyncClientJoinState(SDK::APBPlayerController* PC, const FClientSyncOptions& Options);
    void SendLateJoinClientStart(SDK::APBPlayerController* PC);
    void PrepareLateJoinRespawn(SDK::APBPlayerController* PC);
    void FinalizeLateJoinSpawn(SDK::APBPlayerController* PC);
    void RequestLateJoinSpawn(SDK::APBPlayerController* PC, FLateJoinInfo& Info);
};
