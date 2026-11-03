// Copyright Edgegap. All Rights Reserved.

#include "EdgegapSDKMatchmakingBackend.h"
#include "EdgegapCore.h"
#include "EdgegapMatchmakerSubsystem.h"
#include "EdgegapSessionSubsystem.h"
#include "EdgegapSettings.h"
#include "Engine/GameInstance.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(EdgegapSDKMatchmakingBackend)

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapSDKBridge, Log, All);

void UEdgegapSDKMatchmakingBackend::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Matchmaker = Collection.InitializeDependency<UEdgegapMatchmakerSubsystem>();
	Sessions = Collection.InitializeDependency<UEdgegapSessionSubsystem>();
	if (!Matchmaker || !Sessions)
	{
		UE_LOG(LogEdgegapSDKBridge, Warning, TEXT("Edgegap SDK or Lyra's Edgegap session subsystem is missing: keeping the built-in matchmaking client"));
		return;
	}

	UEdgegapSettings* Settings = GetMutableDefault<UEdgegapSettings>();
	// Lyra travels itself, so it can rejoin when a fresh server drops early joiners while it loads the match map.
	Settings->bTravelOnMatch = false;
	Settings->GamePortName = Sessions->PortName;
	if (Settings->MatchmakerUrl.IsEmpty())
	{
		Settings->MatchmakerUrl = Sessions->MatchmakerUrl;
	}
	if (Settings->MatchmakerToken.IsEmpty())
	{
		Settings->MatchmakerToken = Sessions->AuthToken;
	}

	Matchmaker->OnStatus.AddDynamic(this, &UEdgegapSDKMatchmakingBackend::HandleStatus);
	Matchmaker->OnMatchFound.AddDynamic(this, &UEdgegapSDKMatchmakingBackend::HandleMatchFound);
	Matchmaker->OnFailed.AddDynamic(this, &UEdgegapSDKMatchmakingBackend::HandleFailed);
	Sessions->SetMatchmakingBackend(this);
}

void UEdgegapSDKMatchmakingBackend::Deinitialize()
{
	if (Matchmaker)
	{
		Matchmaker->OnStatus.RemoveAll(this);
		Matchmaker->OnMatchFound.RemoveAll(this);
		Matchmaker->OnFailed.RemoveAll(this);
	}
	if (Sessions)
	{
		Sessions->SetMatchmakingBackend(nullptr);
	}
	Super::Deinitialize();
}

void UEdgegapSDKMatchmakingBackend::StartMatchmaking(const FString& Profile, bool bSendBeaconLatencies)
{
	// The SDK's own -EdgegapFindMatch= also reads Lyra's test flag (a playlist, not a profile): Lyra's search wins.
	if (Matchmaker->IsSearching())
	{
		Matchmaker->CancelMatch();
	}

	bActive = true;
	TArray<FString> MeasureLatencyInto;
	if (bSendBeaconLatencies)
	{
		MeasureLatencyInto.Add(TEXT("beacons"));
	}
	UE_LOG(LogEdgegapSDKBridge, Log, TEXT("Finding a match for profile %s with the Edgegap SDK"), *Profile);
	Matchmaker->FindMatch(Profile, FString(), MeasureLatencyInto);
}

void UEdgegapSDKMatchmakingBackend::CancelMatchmaking()
{
	bActive = false;
	Matchmaker->CancelMatch();
}

void UEdgegapSDKMatchmakingBackend::HandleStatus(const FString& Status)
{
	if (bActive)
	{
		Sessions->HandleBackendStatus(Status + TEXT("..."));
	}
}

void UEdgegapSDKMatchmakingBackend::HandleMatchFound(const FEdgegapMMTicketAssignmentResponse& Assignment, const FString& JoinUrl, const FString& TicketId)
{
	if (bActive)
	{
		bActive = false;
		Sessions->HandleBackendAssigned(JoinUrl, UEdgegapMatchmakerSubsystem::GetConnectAddress(Assignment));
	}
}

void UEdgegapSDKMatchmakingBackend::HandleFailed(const FEdgegapError& Error)
{
	if (bActive)
	{
		bActive = false;
		Sessions->HandleBackendFailed(Error.Message);
	}
}
