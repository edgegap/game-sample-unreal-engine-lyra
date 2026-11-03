// Copyright Edgegap. All Rights Reserved.

#include "EdgegapSessionSubsystem.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "CommonUserTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(EdgegapSessionSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapMatchmaker, Log, All);

namespace
{
	/** Round trip of a TCP handshake to the beacon, in ms, or a negative value if it didn't answer in time. */
	float MeasureTcpPingMs(const FString& Ip, int32 Port, float TimeoutSeconds)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		bool bValidIp = false;
		Address->SetIp(*Ip, bValidIp);
		Address->SetPort(Port);
		if (!bValidIp)
		{
			return -1.f;
		}

		FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("EdgegapBeaconPing"), Address->GetProtocolType());
		if (!Socket)
		{
			return -1.f;
		}

		float PingMs = -1.f;
		Socket->SetNonBlocking(true);
		const double Start = FPlatformTime::Seconds();
		Socket->Connect(*Address);
		if (Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(TimeoutSeconds))
			&& Socket->GetConnectionState() == SCS_Connected)
		{
			PingMs = static_cast<float>((FPlatformTime::Seconds() - Start) * 1000.0);
		}
		Socket->Close();
		Sockets->DestroySocket(Socket);
		return PingMs;
	}

	struct FBeacon
	{
		FString City;
		FString Ip;
		int32 TcpPort = 0;
		float PingMs = -1.f;
	};

	UEdgegapSessionSubsystem* GetSessionSubsystem(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UEdgegapSessionSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs FindMatchCommand(
		TEXT("Edgegap.FindMatch"), TEXT("Queue with the Edgegap matchmaker. Optional argument: playlist asset name, e.g. DA_Expanse_TDM."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UEdgegapSessionSubsystem* Sessions = GetSessionSubsystem(World))
			{
				Sessions->FindMatch(Args.Num() > 0 ? Args[0] : FString());
			}
		}));

	FAutoConsoleCommandWithWorld CancelMatchCommand(
		TEXT("Edgegap.CancelMatch"), TEXT("Leave the Edgegap matchmaking queue."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UEdgegapSessionSubsystem* Sessions = GetSessionSubsystem(World))
			{
				Sessions->CancelMatch();
			}
		}));
}

UEdgegapSessionSubsystem::UEdgegapSessionSubsystem()
{
	// Edgegap servers are dedicated servers advertised as EOS sessions: player-hosted lobbies don't apply,
	// and the reservation beacon would need a second port on every deployment.
	bUseLobbiesDefault = false;
	bUseLobbiesVoiceChatDefault = false;
	bUseBeacons = false;
}

void UEdgegapSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!IsRunningDedicatedServer())
	{
		PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UEdgegapSessionSubsystem::OnPostLoadMapForMatchmaking);
	}
}

void UEdgegapSessionSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	HideStatusOverlay();

	// Don't leave a ticket behind when the game closes mid-search, or it can still start a server.
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		DeleteTicket();
	}
	StopPolling();
	Super::Deinitialize();
}

void UEdgegapSessionSubsystem::HostSession(APlayerController* HostingPlayer, UCommonSession_HostSessionRequest* Request)
{
	if (bEnableMatchmaking && !IsRunningDedicatedServer() && Request && Request->OnlineMode == ECommonSessionOnlineMode::Online)
	{
		UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Hosting %s on Edgegap instead of this machine"), *Request->ModeNameForAdvertisement);
		FindMatch(Request->ModeNameForAdvertisement);
		return;
	}

	Super::HostSession(HostingPlayer, Request);
}

FString UEdgegapSessionSubsystem::GetProfileForPlaylist(const FString& Playlist) const
{
	for (const FEdgegapPlaylistProfile& Entry : PlaylistProfiles)
	{
		if (Entry.Playlist.Equals(Playlist, ESearchCase::IgnoreCase))
		{
			return Entry.Profile;
		}
	}
	return DefaultProfile;
}

FString UEdgegapSessionSubsystem::GetPlaylistForProfile(const FString& Profile) const
{
	for (const FEdgegapPlaylistProfile& Entry : PlaylistProfiles)
	{
		if (Entry.Profile.Equals(Profile, ESearchCase::IgnoreCase))
		{
			return Entry.Playlist;
		}
	}
	return FString();
}

void UEdgegapSessionSubsystem::OnPostLoadMapForMatchmaking(UWorld* LoadedWorld)
{
	if (!LoadedWorld || LoadedWorld->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	// Connected to a server: the overlay belongs to the menus only.
	if (LoadedWorld->GetNetMode() != NM_Standalone)
	{
		// A fresh server still on its boot map travels to the match map next, so that doesn't count as joined yet.
		if (LoadedWorld->GetOutermost()->GetName() != TEXT("/Engine/Maps/Entry"))
		{
			AssignedAddress.Reset();
			if (MatchmakingState == EEdgegapMatchmakingState::Connecting)
			{
				SetMatchmakingState(EEdgegapMatchmakingState::Idle, FString());
			}
		}
		HideStatusOverlay();
		return;
	}

	// Back on a standalone map before reaching the match: the server dropped us while it traveled, or wasn't ready.
	if (!AssignedAddress.IsEmpty())
	{
		if (FPlatformTime::Seconds() - AssignedTime < JoinRetryWindowSeconds && JoinAttempts < MaxJoinAttempts)
		{
			SetMatchmakingState(EEdgegapMatchmakingState::Connecting, TEXT("Rejoining the Edgegap server..."));
			GetGameInstance()->GetTimerManager().SetTimer(RejoinTimer, this, &UEdgegapSessionSubsystem::TravelToAssignedServer, JoinRetryDelaySeconds, false);
		}
		else
		{
			AssignedAddress.Reset();
			Fail(TEXT("Could not join the Edgegap server"));
		}
	}
	else if (MatchmakingState != EEdgegapMatchmakingState::Idle)
	{
		// LoadMap removed viewport widgets: put the overlay back.
		ShowStatusOverlay();
	}

	// -EdgegapFindMatch[=<Playlist>] queues once at startup, for unattended tests with several clients.
	const bool bAutoQueue = FParse::Param(FCommandLine::Get(), TEXT("EdgegapFindMatch"))
		|| FCString::Strifind(FCommandLine::Get(), TEXT("-EdgegapFindMatch=")) != nullptr;
	if (!bAutoQueued && bAutoQueue)
	{
		bAutoQueued = true;
		FString Playlist;
		FParse::Value(FCommandLine::Get(), TEXT("EdgegapFindMatch="), Playlist);

		// Let Lyra's front end finish loading its experience before traveling away from it.
		FTimerHandle Unused;
		TWeakObjectPtr<UEdgegapSessionSubsystem> WeakThis(this);
		GetGameInstance()->GetTimerManager().SetTimer(Unused, FTimerDelegate::CreateLambda([WeakThis, Playlist]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->FindMatch(Playlist);
			}
		}), 5.f, false);
	}
}

void UEdgegapSessionSubsystem::FindMatch(const FString& Playlist)
{
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		return;
	}

	SearchPlaylist = Playlist;
	SearchProfile = GetProfileForPlaylist(Playlist);
	TicketId.Reset();
	AssignedAddress.Reset();
	++SearchId;

	if (IEdgegapMatchmakingBackend* Client = Backend.Get())
	{
		if (SearchProfile.IsEmpty())
		{
			Fail(FString::Printf(TEXT("No Edgegap matchmaker profile for playlist '%s' and no DefaultProfile"), *Playlist));
			return;
		}
		NotifySessionInformationUpdated(ECommonSessionInformationState::Matchmaking, Playlist);
		SetMatchmakingState(EEdgegapMatchmakingState::Searching, TEXT("Joining the Edgegap queue..."));
		Client->StartMatchmaking(SearchProfile, bSendBeaconLatencies);
		return;
	}

	// Catch unset or placeholder values here, with a clear message, instead of a confusing HTTP error later.
	// An unquoted URL in the ini arrives cut to "https:", which also fails this check.
	const bool bUrlLooksReal = MatchmakerUrl.StartsWith(TEXT("https://")) && MatchmakerUrl.Len() > 12 && !MatchmakerUrl.Contains(TEXT("YOUR_"));
	const bool bTokenLooksReal = AuthToken.Len() >= 16 && !AuthToken.Contains(TEXT("YOUR_")) && !AuthToken.Contains(TEXT("<"));
	if (!bUrlLooksReal || !bTokenLooksReal || SearchProfile.IsEmpty())
	{
		Fail(!bUrlLooksReal ? TEXT("Set MatchmakerUrl (quoted, https://...) from the Edgegap dashboard")
			: !bTokenLooksReal ? TEXT("Set AuthToken from the Edgegap dashboard")
			: FString::Printf(TEXT("No Edgegap matchmaker profile for playlist '%s' and no DefaultProfile"), *Playlist));
		return;
	}

	NotifySessionInformationUpdated(ECommonSessionInformationState::Matchmaking, Playlist);

	if (!bSendBeaconLatencies)
	{
		CreateTicket({});
		return;
	}

	// Beacons are rescaled in real time, so fetch the list again for every search.
	SetMatchmakingState(EEdgegapMatchmakingState::Searching, TEXT("Measuring latency to Edgegap..."));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), TEXT("/locations/beacons"));
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapSessionSubsystem::OnBeaconsReceived, SearchId);
	Request->ProcessRequest();
}

void UEdgegapSessionSubsystem::CancelMatch()
{
	if (MatchmakingState != EEdgegapMatchmakingState::Searching)
	{
		return;
	}
	++SearchId;
	if (IEdgegapMatchmakingBackend* Client = Backend.Get())
	{
		Client->CancelMatchmaking();
	}
	DeleteTicket();
	StopPolling();
	NotifySessionInformationUpdated(ECommonSessionInformationState::OutOfGame);
	SetMatchmakingState(EEdgegapMatchmakingState::Idle, TEXT("Cancelled"));
}

void UEdgegapSessionSubsystem::SetMatchmakingBackend(IEdgegapMatchmakingBackend* InBackend)
{
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		CancelMatch();
	}
	Backend = InBackend;
	UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Matchmaking client: %s"), InBackend ? *Backend.GetObject()->GetClass()->GetName() : TEXT("built-in"));
}

void UEdgegapSessionSubsystem::HandleBackendStatus(const FString& StatusText)
{
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		SetMatchmakingState(EEdgegapMatchmakingState::Searching, StatusText);
	}
}

void UEdgegapSessionSubsystem::HandleBackendAssigned(const FString& JoinUrl, const FString& ServerAddress)
{
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		JoinAssignedServer(JoinUrl, ServerAddress);
	}
}

void UEdgegapSessionSubsystem::HandleBackendFailed(const FString& Reason)
{
	if (MatchmakingState == EEdgegapMatchmakingState::Searching)
	{
		Fail(Reason);
	}
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UEdgegapSessionSubsystem::MakeRequest(const FString& Verb, const FString& Path) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	FString Url = MatchmakerUrl;
	Url.RemoveFromEnd(TEXT("/"));
	Request->SetURL(Url + Path);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Authorization"), AuthToken);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	return Request;
}

void UEdgegapSessionSubsystem::OnBeaconsReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected, int32 ForSearch)
{
	if (MatchmakingState != EEdgegapMatchmakingState::Searching || ForSearch != SearchId)
	{
		return;
	}
	if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		Fail(TEXT("Could not reach the Edgegap matchmaker"));
		return;
	}

	TSharedPtr<FJsonObject> Json;
	const TArray<TSharedPtr<FJsonValue>>* BeaconValues = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) || !Json.IsValid()
		|| !Json->TryGetArrayField(TEXT("beacons"), BeaconValues))
	{
		Fail(TEXT("Unexpected beacon list"));
		return;
	}

	TArray<FBeacon> Beacons;
	for (const TSharedPtr<FJsonValue>& Value : *BeaconValues)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		const TSharedPtr<FJsonObject>* Location = nullptr;
		FBeacon Beacon;
		if (Object.IsValid() && Object->TryGetObjectField(TEXT("location"), Location)
			&& (*Location)->TryGetStringField(TEXT("city"), Beacon.City)
			&& Object->TryGetStringField(TEXT("public_ip"), Beacon.Ip)
			&& Object->TryGetNumberField(TEXT("tcp_port"), Beacon.TcpPort))
		{
			Beacons.Add(Beacon);
		}
	}
	if (Beacons.IsEmpty())
	{
		Fail(TEXT("No Edgegap beacons available"));
		return;
	}

	// Ping every beacon in parallel off the game thread, keeping the best of three handshakes each.
	const float Timeout = BeaconPingTimeoutSeconds;
	TWeakObjectPtr<UEdgegapSessionSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Beacons = MoveTemp(Beacons), Timeout, ForSearch]() mutable
	{
		ParallelFor(Beacons.Num(), [&Beacons, Timeout](int32 Index)
		{
			FBeacon& Beacon = Beacons[Index];
			for (int32 Sample = 0; Sample < 3; ++Sample)
			{
				const float PingMs = MeasureTcpPingMs(Beacon.Ip, Beacon.TcpPort, Timeout);
				if (PingMs >= 0.f && (Beacon.PingMs < 0.f || PingMs < Beacon.PingMs))
				{
					Beacon.PingMs = PingMs;
				}
			}
		});

		TMap<FString, float> Latencies;
		for (const FBeacon& Beacon : Beacons)
		{
			if (Beacon.PingMs >= 0.f)
			{
				Latencies.Add(Beacon.City, Beacon.PingMs);
			}
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Latencies = MoveTemp(Latencies), ForSearch]()
		{
			UEdgegapSessionSubsystem* This = WeakThis.Get();
			if (!This || This->MatchmakingState != EEdgegapMatchmakingState::Searching || ForSearch != This->SearchId)
			{
				return;
			}
			if (Latencies.IsEmpty())
			{
				This->Fail(TEXT("Could not reach any Edgegap beacon"));
				return;
			}
			This->CreateTicket(Latencies);
		});
	});
}

void UEdgegapSessionSubsystem::CreateTicket(const TMap<FString, float>& BeaconLatencies)
{
	SetMatchmakingState(EEdgegapMatchmakingState::Searching, TEXT("Creating matchmaking ticket..."));

	TSharedRef<FJsonObject> Attributes = MakeShared<FJsonObject>();
	if (bSendBeaconLatencies)
	{
		TSharedRef<FJsonObject> Beacons = MakeShared<FJsonObject>();
		for (const TPair<FString, float>& Latency : BeaconLatencies)
		{
			Beacons->SetNumberField(Latency.Key, FMath::RoundToFloat(Latency.Value * 10.f) / 10.f);
			UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Beacon %s: %.1f ms"), *Latency.Key, Latency.Value);
		}
		Attributes->SetObjectField(TEXT("beacons"), Beacons);
	}

	// player_ip is left out so the matchmaker uses the address the request comes from.
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("profile"), SearchProfile);
	Body->SetObjectField(TEXT("attributes"), Attributes);
	FString BodyText;
	FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&BodyText));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), TEXT("/tickets"));
	Request->SetContentAsString(BodyText);
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapSessionSubsystem::OnTicketCreated);
	Request->ProcessRequest();
}

void UEdgegapSessionSubsystem::OnTicketCreated(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
{
	if (MatchmakingState != EEdgegapMatchmakingState::Searching)
	{
		return;
	}
	if (!bConnected || !Response.IsValid())
	{
		Fail(TEXT("Could not reach the Edgegap matchmaker"));
		return;
	}
	if (Response->GetResponseCode() != 201)
	{
		Fail(FString::Printf(TEXT("Ticket refused (%d): %s"), Response->GetResponseCode(), *Response->GetContentAsString()));
		return;
	}

	TSharedPtr<FJsonObject> Ticket;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Ticket) || !Ticket.IsValid()
		|| !Ticket->TryGetStringField(TEXT("id"), TicketId))
	{
		Fail(TEXT("Unexpected ticket response"));
		return;
	}

	UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Matchmaking ticket %s created for profile %s"), *TicketId, *SearchProfile);
	SetMatchmakingState(EEdgegapMatchmakingState::Searching, TEXT("Searching for players..."));
	GetGameInstance()->GetTimerManager().SetTimer(PollTimer, this, &UEdgegapSessionSubsystem::PollTicket, PollIntervalSeconds, true);
}

void UEdgegapSessionSubsystem::PollTicket()
{
	if (bPollInFlight || TicketId.IsEmpty())
	{
		return;
	}
	bPollInFlight = true;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), TEXT("/tickets/") + TicketId);
	Request->OnProcessRequestComplete().BindUObject(this, &UEdgegapSessionSubsystem::OnTicketPolled);
	Request->ProcessRequest();
}

void UEdgegapSessionSubsystem::OnTicketPolled(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
{
	bPollInFlight = false;
	if (MatchmakingState != EEdgegapMatchmakingState::Searching)
	{
		return;
	}
	if (!bConnected || !Response.IsValid())
	{
		// Network blip: the timer polls again.
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code == 404)
	{
		Fail(TEXT("Ticket expired, try again"));
		return;
	}
	if (Code != 200)
	{
		// 429 and 5xx are retried on the next tick.
		UE_LOG(LogEdgegapMatchmaker, Warning, TEXT("Ticket poll returned %d: %s"), Code, *Response->GetContentAsString());
		return;
	}

	TSharedPtr<FJsonObject> Ticket;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Ticket) || !Ticket.IsValid())
	{
		return;
	}

	const FString Status = Ticket->GetStringField(TEXT("status"));
	if (Status == TEXT("CANCELLED"))
	{
		Fail(TEXT("No match found in time, try again"));
		return;
	}
	if (Status == TEXT("MATCH_FOUND"))
	{
		SetMatchmakingState(EEdgegapMatchmakingState::Searching, TEXT("Match found, starting an Edgegap server..."));
		return;
	}
	if (Status != TEXT("HOST_ASSIGNED"))
	{
		return;
	}

	const TSharedPtr<FJsonObject>* Assignment = nullptr;
	const TSharedPtr<FJsonObject>* Ports = nullptr;
	if (!Ticket->TryGetObjectField(TEXT("assignment"), Assignment) || !(*Assignment)->TryGetObjectField(TEXT("ports"), Ports))
	{
		return;
	}

	// Use the configured port name, or the only port if the version has just one.
	TSharedPtr<FJsonObject> Port;
	const TSharedPtr<FJsonObject>* Named = nullptr;
	if ((*Ports)->TryGetObjectField(PortName, Named))
	{
		Port = *Named;
	}
	else if ((*Ports)->Values.Num() == 1)
	{
		Port = (*Ports)->Values.CreateConstIterator().Value()->AsObject();
	}
	if (!Port.IsValid())
	{
		Fail(FString::Printf(TEXT("No port named '%s' on the assigned server"), *PortName));
		return;
	}

	const FString Address = FString::Printf(TEXT("%s:%d"),
		*(*Assignment)->GetStringField(TEXT("public_ip")), Port->GetIntegerField(TEXT("external")));

	StopPolling();
	UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Ticket %s assigned to %s"), *TicketId, *Address);

	// The ticket goes with the join, for servers that only let matched players in (the Edgegap SDK's ticket check).
	JoinAssignedServer(FString::Printf(TEXT("%s?edgegap_mm_ticket_id=%s"), *Address, *TicketId), Address);
}

void UEdgegapSessionSubsystem::JoinAssignedServer(const FString& JoinUrl, const FString& ServerAddress)
{
	SetMatchmakingState(EEdgegapMatchmakingState::Connecting, FString::Printf(TEXT("Joining Edgegap server %s..."), *ServerAddress));

	AssignedAddress = JoinUrl;
	AssignedTime = FPlatformTime::Seconds();
	JoinAttempts = 0;
	TravelToAssignedServer();
}

void UEdgegapSessionSubsystem::TravelToAssignedServer()
{
	APlayerController* PC = GetGameInstance()->GetFirstLocalPlayerController();
	if (!PC || AssignedAddress.IsEmpty())
	{
		AssignedAddress.Reset();
		Fail(TEXT("No local player to travel"));
		return;
	}

	++JoinAttempts;
	UE_LOG(LogEdgegapMatchmaker, Log, TEXT("Joining %s (attempt %d)"), *AssignedAddress, JoinAttempts);

	FString URL = AssignedAddress;
	OnPreClientTravelEvent.Broadcast(URL);
	PC->ClientTravel(URL, TRAVEL_Absolute);
}

void UEdgegapSessionSubsystem::DeleteTicket()
{
	if (TicketId.IsEmpty())
	{
		return;
	}
	// Fire and forget: the ticket also expires on its own.
	MakeRequest(TEXT("DELETE"), TEXT("/tickets/") + TicketId)->ProcessRequest();
	TicketId.Reset();
}

void UEdgegapSessionSubsystem::StopPolling()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(PollTimer);
	}
	bPollInFlight = false;
}

void UEdgegapSessionSubsystem::Fail(const FString& Reason)
{
	UE_LOG(LogEdgegapMatchmaker, Warning, TEXT("Matchmaking failed: %s"), *Reason);
	StopPolling();
	TicketId.Reset();
	SetMatchmakingState(EEdgegapMatchmakingState::Failed, Reason);

	// Lyra's hosting screen waits for a create-session result: report the failure like a failed host.
	FOnlineResultInformation Result;
	Result.bWasSuccessful = false;
	Result.ErrorId = TEXT("Edgegap.Matchmaking");
	Result.ErrorText = FText::FromString(Reason);
	NotifyCreateSessionComplete(Result);
	NotifySessionInformationUpdated(ECommonSessionInformationState::OutOfGame);
}

void UEdgegapSessionSubsystem::SetMatchmakingState(EEdgegapMatchmakingState NewState, const FString& NewStatusText)
{
	MatchmakingState = NewState;
	MatchmakingStatusText = NewStatusText;
	OnMatchmakingStatusChanged.Broadcast(MatchmakingState, MatchmakingStatusText);

	if (NewState == EEdgegapMatchmakingState::Idle)
	{
		HideStatusOverlay();
	}
	else if (!StatusOverlay.IsValid())
	{
		ShowStatusOverlay();
	}
}

void UEdgegapSessionSubsystem::ShowStatusOverlay()
{
	HideStatusOverlay();
	if (!bShowStatusOverlay || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	TWeakObjectPtr<UEdgegapSessionSubsystem> WeakThis(this);

	// Plain Slate keeps the plugin free of .uasset files; a UMG widget can bind OnMatchmakingStatusChanged instead.
	StatusOverlay =
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, 40.f))
		[
			SNew(SBorder)
			.Padding(FMargin(20.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([WeakThis]() { return FText::FromString(WeakThis.IsValid() ? WeakThis->GetMatchmakingStatusText() : FString()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(FMargin(0.f, 10.f, 0.f, 0.f))
				[
					SNew(SButton)
					.Visibility_Lambda([WeakThis]()
					{
						const bool bShow = WeakThis.IsValid() && WeakThis->GetMatchmakingState() != EEdgegapMatchmakingState::Connecting;
						return bShow ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([WeakThis]()
					{
						const bool bSearching = WeakThis.IsValid() && WeakThis->GetMatchmakingState() == EEdgegapMatchmakingState::Searching;
						return FText::FromString(bSearching ? TEXT("Cancel") : TEXT("Close"));
					})
					.OnClicked_Lambda([WeakThis]()
					{
						if (WeakThis.IsValid())
						{
							if (WeakThis->GetMatchmakingState() == EEdgegapMatchmakingState::Searching)
							{
								WeakThis->CancelMatch();
							}
							else
							{
								WeakThis->SetMatchmakingState(EEdgegapMatchmakingState::Idle, FString());
							}
						}
						return FReply::Handled();
					})
				]
			]
		];

	GEngine->GameViewport->AddViewportWidgetContent(StatusOverlay.ToSharedRef(), 1000);
}

void UEdgegapSessionSubsystem::HideStatusOverlay()
{
	if (StatusOverlay.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(StatusOverlay.ToSharedRef());
	}
	StatusOverlay.Reset();
}
