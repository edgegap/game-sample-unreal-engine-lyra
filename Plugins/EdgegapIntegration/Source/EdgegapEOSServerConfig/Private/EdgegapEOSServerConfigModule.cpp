// Copyright Edgegap. All Rights Reserved.

#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogEdgegapEOSServerConfig, Log, All);

namespace
{
	const TCHAR* EOSSettingsSection = TEXT("/Script/OnlineSubsystemEOS.EOSSettings");

	/** Sets Key="Value" inside an artifact entry such as (ArtifactName="Lyra",ClientId="...",ClientSecret="..."). */
	bool SetArtifactField(FString& Entry, const FString& Key, const FString& Value)
	{
		const FString Prefix = Key + TEXT("=");
		int32 Start = Entry.Find(Prefix, ESearchCase::IgnoreCase);
		if (Start == INDEX_NONE)
		{
			return false;
		}
		Start += Prefix.Len();

		int32 End = Start;
		if (Entry.IsValidIndex(Start) && Entry[Start] == TEXT('"'))
		{
			End = Entry.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1);
			if (End == INDEX_NONE)
			{
				return false;
			}
			++End;
		}
		else
		{
			while (Entry.IsValidIndex(End) && Entry[End] != TEXT(',') && Entry[End] != TEXT(')'))
			{
				++End;
			}
		}

		Entry = Entry.Left(Start) + TEXT("\"") + Value + TEXT("\"") + Entry.Mid(End);
		return true;
	}
}

/**
 * Prepares the EOS settings of a dedicated server, in memory, before the online subsystem starts.
 *
 * - No player login: game clients sign in with Epic accounts, but a server has no player, and an Epic
 *   login attempt there would wait forever for a browser. The server creates sessions with client
 *   credentials only.
 * - Its own EOS client: the game clients' Peer2Peer client requires a user, so the server needs a client
 *   whose policy doesn't, and that secret must never ship to players. Edgegap injects it as
 *   EOS_SERVER_CLIENT_ID / EOS_SERVER_CLIENT_SECRET, swapped into the default artifact here.
 *
 * Loads at EarliestPossible: config exists, and the EOS online subsystem (PostConfigInit) doesn't yet.
 */
class FEdgegapEOSServerConfigModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!IsRunningDedicatedServer() || !GConfig)
		{
			return;
		}

		GConfig->SetBool(EOSSettingsSection, TEXT("bUseEAS"), false, GEngineIni);
		GConfig->SetBool(EOSSettingsSection, TEXT("bUseEOSConnect"), false, GEngineIni);
		GConfig->SetBool(EOSSettingsSection, TEXT("bEnableOverlay"), false, GEngineIni);
		GConfig->SetBool(EOSSettingsSection, TEXT("bEnableSocialOverlay"), false, GEngineIni);
		GConfig->SetBool(EOSSettingsSection, TEXT("bPreferPersistentAuth"), false, GEngineIni);

		const FString ClientId = FPlatformMisc::GetEnvironmentVariable(TEXT("EOS_SERVER_CLIENT_ID"));
		const FString ClientSecret = FPlatformMisc::GetEnvironmentVariable(TEXT("EOS_SERVER_CLIENT_SECRET"));
		if (ClientId.IsEmpty() || ClientSecret.IsEmpty())
		{
			UE_LOG(LogEdgegapEOSServerConfig, Log, TEXT("EOS_SERVER_CLIENT_ID/SECRET not set: the server uses the EOS client from config"));
			return;
		}

		FString DefaultArtifactName;
		GConfig->GetString(EOSSettingsSection, TEXT("DefaultArtifactName"), DefaultArtifactName, GEngineIni);

		TArray<FString> Artifacts;
		GConfig->GetArray(EOSSettingsSection, TEXT("Artifacts"), Artifacts, GEngineIni);

		const FString NameField = FString::Printf(TEXT("ArtifactName=\"%s\""), *DefaultArtifactName);
		for (FString& Artifact : Artifacts)
		{
			if (!DefaultArtifactName.IsEmpty() && !Artifact.Contains(NameField))
			{
				continue;
			}
			if (SetArtifactField(Artifact, TEXT("ClientId"), ClientId) && SetArtifactField(Artifact, TEXT("ClientSecret"), ClientSecret))
			{
				GConfig->SetArray(EOSSettingsSection, TEXT("Artifacts"), Artifacts, GEngineIni);
				// Never log the secret.
				UE_LOG(LogEdgegapEOSServerConfig, Log, TEXT("EOS artifact '%s' uses server client %s"), *DefaultArtifactName, *ClientId);
				return;
			}
		}

		UE_LOG(LogEdgegapEOSServerConfig, Warning, TEXT("No EOS artifact '%s' with ClientId/ClientSecret fields found in [%s]"), *DefaultArtifactName, EOSSettingsSection);
	}
};

IMPLEMENT_MODULE(FEdgegapEOSServerConfigModule, EdgegapEOSServerConfig)
