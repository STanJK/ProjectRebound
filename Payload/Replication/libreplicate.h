#pragma once

#include <vector>
#include <mutex>

class LibReplicate {
public:
	enum EReplicationMode {
		Minimal
	};

	enum EJoinMode {
		Open,
		Closed
	};

private:
	struct TArray {
		void* Data;
		unsigned int ArraySize;
		unsigned int ArrayMax;
	};

	struct FString {
		const wchar_t* Data;
		unsigned int ArraySize;
		unsigned int ArrayMax;
	};

	struct FURL {
		FString Protocol;
		FString Host;
		int Port;
		int Valid;
		FString Map;
		FString RedirectURL;
		TArray Op;
		FString Portal;
	};

	struct FName {
		unsigned int ComparisonIndex;
		unsigned int Number;

		FName(unsigned int ComparisonIndex) {
			this->ComparisonIndex = ComparisonIndex;
			this->Number = 0;
		}

		FName() {
			this->ComparisonIndex = 0;
			this->Number = 0;
		}
	};

private:
	typedef void UNetDriver;
	typedef void UActorChannel;
	typedef void AActor;
	typedef void UNetConnection;
	typedef void UEngine;
	typedef void UWorld;

private:
	typedef bool (*InitListen)(UNetDriver* NetDriver, void* InNotify, FURL* URL, bool bReuseAddressAndPort, FString* Error);
	typedef void* (*FMemoryMalloc)(size_t Size, size_t Alignment);
	typedef void (*FMemoryFree)(void* Mem);
	typedef void (*SetChannelActor)(UActorChannel* Channel, AActor* InActor, unsigned int Flags);
	typedef UActorChannel* (*CreateChannel)(UNetConnection* Connection, FName* ChName, unsigned int Flags, int ChIndex);
	typedef uint8_t (*ReplicateActor)(UActorChannel* Channel);
	typedef void (*OrigNotifyControlMessage)(UNetConnection* Connection, uint8_t MessageType, void* Bunch);
	typedef bool (*CreateNamedNetDriver)(UEngine* Engine, UWorld* World, void* NetDriverName, void* NetDriverDefinition);
	typedef bool (*NotifyAcceptingConnection)(void* Notify);
	typedef void (*NotifyAcceptedConnection)(void* Notify, UNetConnection* Connection);
	typedef bool (*NotifyAcceptingChannel)(void* Notify, void* Channel);
	typedef void (*SetWorld)(UNetDriver* NetDriver, UWorld* World);
	typedef void (*UActorChannelClose)(UActorChannel* Channel, unsigned int flags);
	typedef void (*CallPreReplication)(AActor* Actor, UNetDriver* NetDriver);
	typedef void (*SendClientAdjustment)(AActor* PlayerController);

private:
	EReplicationMode ReplicationMode;
	EJoinMode JoinMode;
	std::vector<std::pair<UNetConnection*, std::vector<std::pair<UActorChannel*, AActor*>>>>* Channels;
	std::vector<std::pair<UNetConnection*, std::vector<AActor*>>>* SentTemporaries;
	std::vector<UActorChannel*>* ChannelsToClose;
	std::mutex ChannelsToCloseMutex;

	FMemoryMalloc FMemoryMallocFuncPtr;
	FMemoryFree FMemoryFreeFuncPtr;
	SetChannelActor SetChannelActorFuncPtr;
	CreateChannel CreateChannelFuncPtr;
	ReplicateActor ReplicateActorFuncPtr;
	OrigNotifyControlMessage OrigNotifyControlMessageFuncPtr;
	CreateNamedNetDriver CreateNamedNetDriverFuncPtr;
	SetWorld SetWorldFuncPtr;
	InitListen InitListenFuncPtr;
	UActorChannelClose ActorChannelCloseFuncPtr;
	CallPreReplication CallPreReplicationFuncPtr;
	SendClientAdjustment SendClientAdjustmentFuncPtr;

public:
	struct FActorInfo {
		void* ActorPtr;
		bool bNetTemporary;

		FActorInfo(void* ActorPtr, bool bNetTemporary) {
			this->ActorPtr = ActorPtr;
			this->bNetTemporary = bNetTemporary;
		};
	};

	struct FPlayerControllerInfo {
		UNetConnection* OwningConnection;
		AActor* PlayerController;

		FPlayerControllerInfo(UNetConnection* OwningConnection, AActor* PlayerController) {
			this->OwningConnection = OwningConnection;
			this->PlayerController = PlayerController;
		}
	};

private:
	static bool CanJoinGame();

	static bool JustReturnOne();

	static void DoNothing();

private:
	UActorChannel* GetChannelForActor(UNetConnection* Connection, AActor* Actor);

	bool HaveWeSentThisTemporaryActor(UNetConnection* Connection, AActor* Actor);

	void AddActorChannelToChannels(UNetConnection* Connection, UActorChannel* ActorChannel, AActor* Actor);

private:
	struct FNetworkNotify {
		NotifyAcceptingConnection NotifyAcceptingConnectionFuncPtr;
		NotifyAcceptedConnection NotifyAcceptedConnectionFuncPtr;
		NotifyAcceptingChannel NotifyAcceptingChannelFuncPtr;
		OrigNotifyControlMessage NotifyControlMessageFuncPtr;

		FNetworkNotify(void* JoinFunc, void* DoNothingFunc, void* ReturnOneFunc, void* OrigNotifyControlMessageFuncPtr) {
			this->NotifyAcceptingChannelFuncPtr = (NotifyAcceptingChannel)ReturnOneFunc;
			this->NotifyControlMessageFuncPtr = (OrigNotifyControlMessage)OrigNotifyControlMessageFuncPtr;
			this->NotifyAcceptedConnectionFuncPtr = (NotifyAcceptedConnection)DoNothingFunc;
			this->NotifyAcceptingConnectionFuncPtr = (NotifyAcceptingConnection)JoinFunc;
		}
	};

public:
	LibReplicate(EReplicationMode ReplicationMode, void* InitListenFuncPtr, void* CreateChannelFuncPtr, void* SetChannelActorFuncPtr, void* ReplicateActorFuncPtr, void* FMemoryMallocFuncPtr, void* FMemoryFreeFuncPtr, void* OrigNotifyControlMessageFuncPtr, void* CreateNamedNetDriverFuncPtr, void* ActorChannelCloseFuncPtr, void* SetWorldFuncPtr, void* CallPreReplicationFuncPtr, void* SendClientAdjustmentFuncPtr);

	void CreateNetDriver(void* Engine, void* World, void* NetDriverName);

	void Listen(void* NetDriver, void* World, EJoinMode InitialJoinMode, int Port);

	void SetJoinMode(EJoinMode NewJoinMode);

	void CallFromTickFlushHook(std::vector<FActorInfo>& Actors, std::vector<FPlayerControllerInfo>& PlayerControllers, std::vector<UNetConnection*>& Connections, void* ActorChannelName, UNetDriver* NetDriver);

	void CallWhenActorDestroyed(FActorInfo& Actor);
};