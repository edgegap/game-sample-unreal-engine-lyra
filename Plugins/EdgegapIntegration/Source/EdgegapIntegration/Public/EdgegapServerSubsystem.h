// Copyright Edgegap. All Rights Reserved.

#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "EdgegapServerSubsystem.generated.h"

class AController;
class AGameModeBase;
class APlayerController;

/**
 * Dedicated server side of the Edgegap integration. Only created on dedicated servers.
 *
 * - Playlist: a server started by the Edgegap matchmaker hosts the playlist mapped to its profile
 *   (MM_MATCH_PROFILE, see PlaylistProfiles on UEdgegapSessionSubsystem), through Lyra's own
 *   dedicated server flow (ALyraGameMode::HostDedicatedServerMatch reads -Playlist=).
 * - EOS session: Edgegap maps the game port to a different external port. The EOS session would
 *   advertise the internal one, so the session's port is updated to ARBITRIUM_PORT_<PORT>_EXTERNAL.
 *   EOS fills in the public IP itself.
 * - Lifecycle: a matchmaker-started server stops its deployment once the match is empty, or if
 *   nobody joins in time, so idle servers don't keep billing. Servers deployed by hand are left alone.
 *
 * Optional tuning in DefaultEngine.ini under [/Script/EdgegapIntegration.EdgegapServerSubsystem].
 */
UCLASS(Config=Engine)
class EDGEGAPINTEGRATION_API UEdgegapServerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Begin USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem interface

	/** True when the Edgegap matchmaker started this server. */
	static bool IsMatchmakerDeployment();

	/** Asks Edgegap to stop this deployment. Edgegap then sends SIGTERM, which Unreal handles as a clean exit. */
	void StopDeployment(const FString& Reason);

	/** Seconds a fresh server waits for its first player before stopping itself. */
	UPROPERTY(Config)
	float NoPlayerTimeoutSeconds = 300.f;

	/** Seconds the server stays up once empty, so a matched player who is still connecting can join. */
	UPROPERTY(Config)
	float EmptyServerGraceSeconds = 15.f;

	/** Port name on the Edgegap app version whose external port EOS sessions advertise. */
	UPROPERTY(Config)
	FString PortName = TEXT("gameport");

private:
	void ApplyMatchmakerPlaylist();
	void BindSessionPortFix();
	void OnSessionCreated(FName SessionName, bool bWasSuccessful);
	void OnPostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);
	void OnLogout(AGameModeBase* GameMode, AController* Exiting);
	void CheckEmptyServer();

	FTimerHandle StopTimer;
	FDelegateHandle PostLoginHandle;
	FDelegateHandle LogoutHandle;
	FDelegateHandle CreateSessionHandle;
	int32 ExternalPort = 0;
	bool bStopRequested = false;
};
