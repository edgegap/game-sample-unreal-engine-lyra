// Copyright Edgegap. All Rights Reserved.

#pragma once

#include "CommonSessionSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "EdgegapSessionSubsystem.generated.h"

class SWidget;

UENUM(BlueprintType)
enum class EEdgegapMatchmakingState : uint8
{
	Idle,
	Searching,
	Connecting,
	Failed
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEdgegapMatchmakingStatusChanged, EEdgegapMatchmakingState, State, const FString&, StatusText);

/** Maps a Lyra playlist (user-facing experience asset name) to an Edgegap matchmaker profile. */
USTRUCT()
struct FEdgegapPlaylistProfile
{
	GENERATED_BODY()

	/** Primary asset name of the ULyraUserFacingExperienceDefinition, e.g. DA_Expanse_TDM. */
	UPROPERTY()
	FString Playlist;

	/** Profile name in the Edgegap matchmaker config. */
	UPROPERTY()
	FString Profile;
};

/**
 * Lyra's session subsystem with Edgegap hosting.
 *
 * Hosting an online game (Start a Game, or Quick Play when no session is found) no longer makes this
 * player the listen server: it sends a ticket to the Edgegap matchmaker for the playlist's profile,
 * waits for Edgegap to start a dedicated server, then travels to it. The server registers an EOS session,
 * so Browse and Quick Play find it and other players join it through EOS like any other session.
 *
 * Offline and LAN hosting, Browse, Join and invites are unchanged. Replaces UCommonSessionSubsystem
 * automatically (CommonUser only creates the base class when no subclass exists).
 *
 * Configured in DefaultEngine.ini / UserEngine.ini under [/Script/EdgegapIntegration.EdgegapSessionSubsystem].
 */
UCLASS(Config=Engine)
class EDGEGAPINTEGRATION_API UEdgegapSessionSubsystem : public UCommonSessionSubsystem
{
	GENERATED_BODY()

public:
	UEdgegapSessionSubsystem();

	// Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem interface

	// Begin UCommonSessionSubsystem interface
	virtual void HostSession(APlayerController* HostingPlayer, UCommonSession_HostSessionRequest* Request) override;
	// End UCommonSessionSubsystem interface

	/** Queues for a match of the given playlist (empty: the default profile). Does nothing while already searching. */
	UFUNCTION(BlueprintCallable, Category = "Edgegap")
	void FindMatch(const FString& Playlist);

	/** Deletes the current ticket and stops searching. */
	UFUNCTION(BlueprintCallable, Category = "Edgegap")
	void CancelMatch();

	UFUNCTION(BlueprintPure, Category = "Edgegap")
	EEdgegapMatchmakingState GetMatchmakingState() const { return MatchmakingState; }

	UFUNCTION(BlueprintPure, Category = "Edgegap")
	FString GetMatchmakingStatusText() const { return MatchmakingStatusText; }

	/** Fires on every matchmaking state or status text change, for a UMG menu. */
	UPROPERTY(BlueprintAssignable, Category = "Edgegap")
	FEdgegapMatchmakingStatusChanged OnMatchmakingStatusChanged;

	/** Matchmaker profile for a playlist, or DefaultProfile. */
	FString GetProfileForPlaylist(const FString& Playlist) const;

	/** Playlist for a matchmaker profile, or empty (the server then hosts Lyra's default playlist). */
	FString GetPlaylistForProfile(const FString& Profile) const;

	/** Routes online hosting through the Edgegap matchmaker. Off: Lyra's original listen-server hosting. */
	UPROPERTY(Config)
	bool bEnableMatchmaking = false;

	/** Matchmaker API URL from the dashboard. Keep it quoted in the ini: unquoted, the parser cuts it at "//". */
	UPROPERTY(Config)
	FString MatchmakerUrl;

	/** Matchmaker auth token from the dashboard. Safe to ship in clients: it grants no Edgegap API access. */
	UPROPERTY(Config)
	FString AuthToken;

	/** Profile used when a playlist has no entry in PlaylistProfiles. */
	UPROPERTY(Config)
	FString DefaultProfile;

	UPROPERTY(Config)
	TArray<FEdgegapPlaylistProfile> PlaylistProfiles;

	/** Port name on the Edgegap app version. If the version has a single port, that port is used anyway. */
	UPROPERTY(Config)
	FString PortName = TEXT("gameport");

	UPROPERTY(Config)
	float PollIntervalSeconds = 2.f;

	/**
	 * Sends the player's latency to each Edgegap beacon with the ticket, for a "latencies" rule in the profile.
	 * Must match the profile: the matchmaker rejects attributes it doesn't expect.
	 */
	UPROPERTY(Config)
	bool bSendBeaconLatencies = true;

	/** Timeout for one ping to one beacon. Unreachable beacons are left out of the ticket. */
	UPROPERTY(Config)
	float BeaconPingTimeoutSeconds = 1.f;

	/** Shows matchmaking progress and a Cancel button on top of Lyra's menus while searching. */
	UPROPERTY(Config)
	bool bShowStatusOverlay = true;

	/**
	 * A fresh Lyra server boots on an empty map, then travels to the playlist's map, which drops anyone
	 * who connected in between. Within this window after the assignment, a dropped player rejoins.
	 */
	UPROPERTY(Config)
	float JoinRetryWindowSeconds = 60.f;

	UPROPERTY(Config)
	int32 MaxJoinAttempts = 5;

	UPROPERTY(Config)
	float JoinRetryDelaySeconds = 3.f;

private:
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Verb, const FString& Path) const;
	void OnBeaconsReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected, int32 ForSearch);
	void CreateTicket(const TMap<FString, float>& BeaconLatencies);
	void OnTicketCreated(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected);
	void PollTicket();
	void OnTicketPolled(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected);
	void DeleteTicket();
	void StopPolling();
	void Fail(const FString& Reason);
	void SetMatchmakingState(EEdgegapMatchmakingState NewState, const FString& NewStatusText);

	void OnPostLoadMapForMatchmaking(UWorld* LoadedWorld);
	void TravelToAssignedServer();
	void ShowStatusOverlay();
	void HideStatusOverlay();

	EEdgegapMatchmakingState MatchmakingState = EEdgegapMatchmakingState::Idle;
	FString MatchmakingStatusText;
	FString TicketId;
	/** Server the last ticket was assigned to, kept until the player reaches its match map. */
	FString AssignedAddress;
	double AssignedTime = 0.0;
	int32 JoinAttempts = 0;
	FTimerHandle RejoinTimer;
	FString SearchProfile;
	FString SearchPlaylist;
	/** Increments on every FindMatch and CancelMatch, so late results from an old search are ignored. */
	int32 SearchId = 0;
	FTimerHandle PollTimer;
	bool bPollInFlight = false;
	bool bAutoQueued = false;
	FDelegateHandle PostLoadMapHandle;
	TSharedPtr<SWidget> StatusOverlay;
};
