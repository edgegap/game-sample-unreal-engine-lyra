// Copyright Edgegap. All Rights Reserved.

#include "EdgegapServerSubsystem.h"
#include "EdgegapSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Misc/CommandLine.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(EdgegapServerSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapServer, Log, All);

namespace
{
	/** Matchmaker variables are JSON values: strip the quotes around strings. */
	FString GetJsonStringEnv(const TCHAR* Name)
	{
		FString Value = FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		return Value;
	}
}

bool UEdgegapServerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return IsRunningDedicatedServer();
}

bool UEdgegapServerSubsystem::IsMatchmakerDeployment()
{
	// The matchmaker injects MM_* variables into the deployments it starts.
	return !FPlatformMisc::GetEnvironmentVariable(TEXT("MM_MATCH_ID")).IsEmpty()
		|| !FPlatformMisc::GetEnvironmentVariable(TEXT("MM_TICKET_IDS")).IsEmpty();
}

void UEdgegapServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	BindSessionPortFix();

	if (!IsMatchmakerDeployment())
	{
		UE_LOG(LogEdgegapServer, Log, TEXT("Not started by the Edgegap matchmaker: the server will not stop itself"));
		return;
	}

	UE_LOG(LogEdgegapServer, Log, TEXT("Started by the Edgegap matchmaker (match %s): the server stops itself when the match is empty"),
		*GetJsonStringEnv(TEXT("MM_MATCH_ID")));
	ApplyMatchmakerPlaylist();

	PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &UEdgegapServerSubsystem::OnPostLogin);
	LogoutHandle = FGameModeEvents::GameModeLogoutEvent.AddUObject(this, &UEdgegapServerSubsystem::OnLogout);

	TWeakObjectPtr<UEdgegapServerSubsystem> WeakThis(this);
	GetGameInstance()->GetTimerManager().SetTimer(StopTimer, FTimerDelegate::CreateLambda([WeakThis]()
	{
		if (WeakThis.IsValid())
		{
			WeakThis->StopDeployment(TEXT("no player joined"));
		}
	}), NoPlayerTimeoutSeconds, false);
}

void UEdgegapServerSubsystem::Deinitialize()
{
	FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
	FGameModeEvents::GameModeLogoutEvent.Remove(LogoutHandle);
	if (IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get())
	{
		if (IOnlineSessionPtr Sessions = OnlineSubsystem->GetSessionInterface())
		{
			Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionHandle);
		}
	}
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(StopTimer);
	}
	Super::Deinitialize();
}

void UEdgegapServerSubsystem::ApplyMatchmakerPlaylist()
{
	// A playlist given on the command line wins.
	FString Existing;
	if (FParse::Value(FCommandLine::Get(), TEXT("Playlist="), Existing) || FParse::Value(FCommandLine::Get(), TEXT("UserExperience="), Existing))
	{
		return;
	}

	const FString Profile = GetJsonStringEnv(TEXT("MM_MATCH_PROFILE"));
	const FString Playlist = GetDefault<UEdgegapSessionSubsystem>()->GetPlaylistForProfile(Profile);
	if (Playlist.IsEmpty())
	{
		UE_LOG(LogEdgegapServer, Log, TEXT("No playlist mapped to matchmaker profile '%s': hosting Lyra's default playlist"), *Profile);
		return;
	}

	// ALyraGameMode::HostDedicatedServerMatch reads -Playlist= once the front end map has loaded.
	FCommandLine::Append(*FString::Printf(TEXT(" -Playlist=%s"), *Playlist));
	UE_LOG(LogEdgegapServer, Log, TEXT("Matchmaker profile '%s' hosts playlist %s"), *Profile, *Playlist);
}

void UEdgegapServerSubsystem::BindSessionPortFix()
{
	const FString PortVariable = FString::Printf(TEXT("ARBITRIUM_PORT_%s_EXTERNAL"), *PortName.ToUpper());
	ExternalPort = FCString::Atoi(*FPlatformMisc::GetEnvironmentVariable(*PortVariable));
	if (ExternalPort <= 0)
	{
		return;
	}

	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	IOnlineSessionPtr Sessions = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!Sessions.IsValid())
	{
		return;
	}

	CreateSessionHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &UEdgegapServerSubsystem::OnSessionCreated));
}

void UEdgegapServerSubsystem::OnSessionCreated(FName SessionName, bool bWasSuccessful)
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	IOnlineSessionPtr Sessions = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	FOnlineSessionSettings* Settings = Sessions.IsValid() ? Sessions->GetSessionSettings(SessionName) : nullptr;
	if (!bWasSuccessful || !Settings)
	{
		return;
	}

	// The online subsystem advertises the port the server listens on (7777); players must use the one
	// Edgegap exposes. Lyra's later UpdateSession calls reuse these settings, so this sticks.
	FOnlineSessionSettings Updated = *Settings;
	Updated.Set(FName(TEXT("port")), ExternalPort, EOnlineDataAdvertisementType::ViaOnlineService);
	Sessions->UpdateSession(SessionName, Updated, true);
	UE_LOG(LogEdgegapServer, Log, TEXT("Session %s advertises Edgegap port %d"), *SessionName.ToString(), ExternalPort);
}

void UEdgegapServerSubsystem::OnPostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	GetGameInstance()->GetTimerManager().ClearTimer(StopTimer);
}

void UEdgegapServerSubsystem::OnLogout(AGameModeBase* GameMode, AController* Exiting)
{
	// The leaving controller still counts at this point, so check again once it is gone.
	GetGameInstance()->GetTimerManager().SetTimer(StopTimer, this, &UEdgegapServerSubsystem::CheckEmptyServer, EmptyServerGraceSeconds, false);
}

void UEdgegapServerSubsystem::CheckEmptyServer()
{
	UWorld* World = GetGameInstance()->GetWorld();
	AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
	// Lyra fills matches with bots: count human players only.
	if (GameMode && GameMode->GetNumPlayers() == 0)
	{
		StopDeployment(TEXT("all players left"));
	}
}

void UEdgegapServerSubsystem::StopDeployment(const FString& Reason)
{
	if (bStopRequested)
	{
		return;
	}

	// Injected by Edgegap into every deployment. Never log the token.
	const FString DeleteUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("ARBITRIUM_DELETE_URL"));
	const FString DeleteToken = FPlatformMisc::GetEnvironmentVariable(TEXT("ARBITRIUM_DELETE_TOKEN"));
	if (DeleteUrl.IsEmpty() || DeleteToken.IsEmpty())
	{
		UE_LOG(LogEdgegapServer, Warning, TEXT("Would stop (%s), but ARBITRIUM_DELETE_URL/TOKEN are missing"), *Reason);
		return;
	}

	bStopRequested = true;
	UE_LOG(LogEdgegapServer, Log, TEXT("Stopping deployment: %s"), *Reason);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(DeleteUrl);
	Request->SetVerb(TEXT("DELETE"));
	Request->SetHeader(TEXT("Authorization"), DeleteToken);
	Request->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
		UE_LOG(LogEdgegapServer, Log, TEXT("Self-stop request returned %d"), Code);

		// On success Edgegap sends SIGTERM shortly. If the request failed, exit anyway so the match ends.
		if (!bConnected || Code < 200 || Code >= 300)
		{
			FPlatformMisc::RequestExit(false);
		}
	});
	Request->ProcessRequest();
}
