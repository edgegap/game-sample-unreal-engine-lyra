// Copyright Edgegap. All Rights Reserved.

#pragma once

#include "UObject/Interface.h"
#include "EdgegapMatchmakingBackend.generated.h"

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UEdgegapMatchmakingBackend : public UInterface
{
	GENERATED_BODY()
};

/**
 * Another matchmaking client for UEdgegapSessionSubsystem, e.g. the Edgegap SDK (EdgegapSDKBridge plugin).
 *
 * Register it with UEdgegapSessionSubsystem::SetMatchmakingBackend. The session subsystem keeps Lyra's
 * hosting flow, status overlay, travel and rejoin; the backend only gets the player a server, then reports
 * back with HandleBackendStatus, HandleBackendAssigned or HandleBackendFailed.
 */
class EDGEGAPINTEGRATION_API IEdgegapMatchmakingBackend
{
	GENERATED_BODY()

public:
	/** Queue for a match of this matchmaker profile. */
	virtual void StartMatchmaking(const FString& Profile, bool bSendBeaconLatencies) = 0;

	/** Leave the queue. No more reports are expected after this. */
	virtual void CancelMatchmaking() = 0;
};
