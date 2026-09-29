// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FGameLogEntry.h"
#include "ELogType.h"
#include "ELogSeverity.h"
#include "EStructuralState.h"
#include "GameFramework/Actor.h"
#include "PayloadMissionManager.generated.h"

UENUM(BlueprintType)
enum class EPayloadMissionState : uint8
{
	NotStarted,
	InProgress,
	Success,
	Failed
};

/** A target as it was before anyone started shooting at it.
 *  The class and start transform are kept so a reset can put it back. */
USTRUCT()
struct FMissionTargetSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<AActor> TargetClass;

	UPROPERTY()
	FTransform SpawnTransform = FTransform::Identity;

	/** Weak so a dead target isn't kept alive just by being referenced here. */
	UPROPERTY()
	TWeakObjectPtr<AActor> LiveActor;
};

UCLASS()
class DYNAMICPAYLOADSYSTEM_API APayloadMissionManager : public AActor
{
	GENERATED_BODY()

public:
	APayloadMissionManager();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	float MissionDuration = 30.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mission")
	float RemainingTime = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Mission")
	EPayloadMissionState MissionState = EPayloadMissionState::NotStarted;

	FTimerHandle MissionTimerHandle;
	void StartMission();
	void TickMissionTimer();
	void FailMission(const FString& Reason);

	UPROPERTY()
	TArray<AActor*> DamageableTargets;

	UFUNCTION()
	void OnTargetStructuralStateChanged(EStructuralState NewState);

	void SucceedMission();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	int32 MaxAttempts = 1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mission")
	int32 AttemptsRemaining = 0;

	bool bWaitingForLastPayload = false;
	bool bTimerFrozen = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mission|Summary")
	int32 InitialTargetCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mission|Summary")
	int32 TargetsDestroyedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mission|Summary")
	float TotalDamageInflicted = 0.f;

	UFUNCTION()
	void OnTargetDamageTaken(float DamageAmount, float RemainingHealth);

	void EmitMissionLog(const FString& Message, ELogSeverity Severity);
	void HandleAllTargetsDestroyed();

	UPROPERTY()
	TArray<FGameLogEntry> PendingMissionLogs;

	/** Every level ATargetActor, captured once after BeginPlay and reused for
	 *  subsequent resets. */
	UPROPERTY()
	TArray<FMissionTargetSnapshot> MissionTargetSnapshots;

	bool bTargetSnapshotCaptured = false;

	/** Capture level targets once. Subsequent calls have no effect. */
	void CaptureLevelTargets();

	/** Revive snapshotted targets and restore their initial transforms. */
	void ResetTargetsToStart();

	/** Removes blasts and falling payloads left over from the last attempt. */
	void ClearLeftoversFromLastAttempt();

	/** Clear mission state, timers, and target bindings without resetting the world. */
	void TeardownMission();

	/** Handle possession of a new pawn, which may indicate a new game session. */
	UFUNCTION()
	void OnPlayerPossessedPawnChanged(APawn* OldPawn, APawn* NewPawn);

	/** Run one tick after possession, once the game mode flag has been updated. */
	void HandleNewSessionStarted();

	/** Prevent the manager's pawn reset from being handled as a new session. */
	bool bResettingPlayer = false;

	UPROPERTY()
	FTransform PlayerRestartTransform = FTransform::Identity;

	/** Kamikaze deletes the drone, so on retry there's nothing to teleport.
	 *  Keeping the class means a new one can be spawned. */
	UPROPERTY()
	TSubclassOf<APawn> CapturedPlayerPawnClass;

	/** Whatever the config screen set up, so the respawned drone gets it too. */
	UPROPERTY()
	TSubclassOf<class APayload> CapturedPayloadClass;

	bool bCapturedKamikazeMode = false;

	bool bMissionSnapshotCaptured = false;

	/** Records the starting line-up. No-op once it has run. */
	void CaptureMissionSnapshot();

	/** Teleports the player pawn home and clears its physics state. */
	void ResetPlayerToStart();

	/** Warn-once flag for the HUD interface check. It used to be a static bool,
	 *  so the warning only ever showed in the first PIE session. */
	bool bWarnedAboutMissingInterface = false;

public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
		FOnMissionTimeUpdated,
		float, RemainingTime
	);

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
		FOnMissionStateChanged,
		EPayloadMissionState, NewState
	);

	/** Fires once per second of the pre-mission countdown (3, 2, 1...).
	 *  There's no 0 - for "GO" listen to OnMissionStateChanged, which fires as
	 *  the mission starts. */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
		FOnMissionCountdown,
		int32, SecondsRemaining
	);

	/** Fires once when the mission ends, win or lose, with the stats for a results screen. */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
		FOnMissionResolved,
		EPayloadMissionState, FinalState,
		int32, InitialTargets,
		int32, TargetsDestroyed,
		float, TotalDamage
	);

	UPROPERTY(BlueprintAssignable, Category = "Mission")
	FOnMissionTimeUpdated OnMissionTimeUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Mission")
	FOnMissionStateChanged OnMissionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Mission")
	FOnMissionResolved OnMissionResolved;

	UPROPERTY(BlueprintAssignable, Category = "Mission")
	FOnMissionCountdown OnMissionCountdown;

	UFUNCTION(BlueprintCallable, Category = "Mission")
	void NotifyAttemptConsumed();

	/** For targets spawned after the mission started. Calling it twice is fine,
	 *  and it does nothing if no mission is running. */
	UFUNCTION(BlueprintCallable, Category = "Mission")
	void RegisterMissionTarget(class ATargetActor* Target);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission Mode")
	bool bMissionModeEnabled = false;

	bool IsMissionModeActive() const { return MissionState == EPayloadMissionState::InProgress; }
	bool IsMissionSystemEnabled() const { return bMissionModeEnabled; }
	bool IsMissionActive() const { return bMissionModeEnabled && MissionState == EPayloadMissionState::InProgress; }

	void DisableAllTargetHighlights();

	UPROPERTY(EditAnywhere, Category = "Mission")
	bool bQuitGameOnFailure = true;

	UFUNCTION()
	void NotifyDroneDestroyed();

	void NotifyKamikazeTriggered();

	FTimerHandle CountdownTimerHandle;

	/** Seconds of countdown before the mission clock starts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mission")
	int32 CountdownStartTime = 3;
	int32 CountdownTimeRemaining;

	void HandleMissionStart();
	void TickCountdown();

	UFUNCTION(BlueprintCallable, Category = "Mission")
	void RetryMission();

	/** Puts everything back: targets revived at their start, vehicles back on
	 *  their route, and a fresh drone at the PlayerStart. Safe to call twice. */
	UFUNCTION(BlueprintCallable, Category = "Mission")
	void ResetMissionWorld();

	/** Set false to make Retry reset only the clock and counters, leaving the
	 *  world as the player left it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	bool bResetWorldOnRetry = true;

	/** Also return the player pawn to the PlayerStart on a world reset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	bool bResetPlayerOnRetry = true;

	UPROPERTY(EditAnywhere, Category = "Mission")
	float QuitDelaySeconds = 3.0f;
	FTimerHandle QuitGameTimerHandle;
	UFUNCTION()
	void QuitGameDelayed();

	UFUNCTION(BlueprintCallable, Category = "Mission")
	void RequestMissionStart();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	float PayloadRespawnDelay = 2.0f;
	FTimerHandle PayloadRespawnTimerHandle;
	void RespawnPayloadDelayed();

	void NotifyLastPayloadResolved();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	float LastPayloadStateChangeDelay = 3.0f;
	FTimerHandle LastPayloadResolveTimerHandle;
	void DeferredResolveLastPayload();

	/** Safety net, in seconds. If the last payload disappears without reporting
	 *  back (destroyed from BP, drone died, level unloaded) the mission would wait
	 *  forever. After this long it resolves anyway. Keep it longer than a
	 *  payload's fall plus fuse time, or it will cut in early. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission",
		meta = (ClampMin = "1.0"))
	float LastPayloadWatchdogTimeout = 10.0f;
	FTimerHandle LastPayloadWatchdogHandle;

	/** Gives a payload to whoever is carrying - the player's drone first,
	 *  otherwise the first actor with a PayloadAttachmentComponent.
	 *  @return false if there was nobody to give it to. */
	bool SpawnPayloadOnCarrier();

	/** Out of attempts but a payload might still be falling - wait for it. */
	void EnterWaitingForLastPayload();

	/** Arms (or re-arms) the watchdog timer. */
	void StartLastPayloadWatchdog();

	/** Fired only if nothing resolved the waiting state in time. */
	UFUNCTION()
	void OnLastPayloadWatchdogExpired();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Training")
	float ConfiguredFuseTime = 3.0f;

	/** Toggle debug logging for mission manager diagnostics (editor only). */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = false;
};
