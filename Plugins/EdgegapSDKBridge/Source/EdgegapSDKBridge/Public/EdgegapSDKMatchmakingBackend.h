// Copyright Edgegap. All Rights Reserved.

#pragma once

#include "EdgegapMatchmakingBackend.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "EdgegapSDKMatchmakingBackend.generated.h"

class UEdgegapMatchmakerSubsystem;
class UEdgegapSessionSubsystem;
struct FEdgegapError;
struct FEdgegapMMTicketAssignmentResponse;

/**
 * Runs Lyra's Edgegap matchmaking through the Edgegap SDK's matchmaker client (UEdgegapMatchmakerSubsystem).
 *
 * UEdgegapSessionSubsystem keeps Lyra's side: hosting requests, playlist -> profile mapping, the status overlay,
 * travel and rejoin. The SDK measures the beacons, queues a solo group and waits for the server; its join URL
 * carries the player's ticket, which the SDK's server module checks (EdgegapServer, Require Matchmaker Ticket).
 *
 * The SDK reads its matchmaker URL and token from -EdgegapMMUrl= / EDGEGAP_MM_URL or Project Settings > Edgegap.
 * When those are empty it uses MatchmakerUrl and AuthToken from the EdgegapSessionSubsystem settings, so one
 * configuration works for both clients.
 */
UCLASS()
class EDGEGAPSDKBRIDGE_API UEdgegapSDKMatchmakingBackend : public UGameInstanceSubsystem, public IEdgegapMatchmakingBackend
{
	GENERATED_BODY()

public:
	// Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem interface

	// Begin IEdgegapMatchmakingBackend interface
	virtual void StartMatchmaking(const FString& Profile, bool bSendBeaconLatencies) override;
	virtual void CancelMatchmaking() override;
	// End IEdgegapMatchmakingBackend interface

private:
	UFUNCTION()
	void HandleStatus(const FString& Status);

	UFUNCTION()
	void HandleMatchFound(const FEdgegapMMTicketAssignmentResponse& Assignment, const FString& JoinUrl, const FString& TicketId);

	UFUNCTION()
	void HandleFailed(const FEdgegapError& Error);

	UPROPERTY()
	TObjectPtr<UEdgegapMatchmakerSubsystem> Matchmaker;

	UPROPERTY()
	TObjectPtr<UEdgegapSessionSubsystem> Sessions;

	/** True while Lyra waits on a search this backend started. The SDK also searches on its own console commands. */
	bool bActive = false;
};
