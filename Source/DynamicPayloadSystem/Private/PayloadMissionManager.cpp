// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "PayloadMissionManager.h"
#include "DamagableComponent.h"
#include "DynamicPayloadSystemModule.h"
#include "PayloadAttachmentComponent.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "Payload.h"
#include "Explosive.h"
#include "TargetActor.h"
#include "MissionLogReceiver.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/GameModeBase.h"
#include "Components/PrimitiveComponent.h"
#include "MovableTargetComponent.h"

APayloadMissionManager::APayloadMissionManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

void APayloadMissionManager::BeginPlay()
{
	Super::BeginPlay();
	if (!GetWorld())
		return;

	// Capture targets next tick so each actor has completed BeginPlay and saved
	// its InitialTransform, regardless of actor initialization order.
	GetWorld()->GetTimerManager().SetTimerForNextTick(
		this, &APayloadMissionManager::CaptureLevelTargets);

	if (bMissionModeEnabled)
	{
		RequestMissionStart();
	}
}

void APayloadMissionManager::RequestMissionStart()
{
	if (!bMissionModeEnabled)
		return;

	// A finished mission is stuck on Success/Failed and StartMission won't run
	// from there - that's how the second Play used to do nothing at all.
	// RetryMission already cleans everything up and restarts, so just use it.
	if (MissionState != EPayloadMissionState::NotStarted)
	{
		RetryMission();
		return;
	}

	HandleMissionStart();
}

void APayloadMissionManager::HandleMissionStart()
{
	if (!bMissionModeEnabled)
		return;

	// Find any actor with a PayloadAttachmentComponent that has a valid PayloadClass
	bool bHasValidPayload = false;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (UPayloadAttachmentComponent* PayloadComp = It->FindComponentByClass<UPayloadAttachmentComponent>())
		{
			if (PayloadComp->PayloadClass)
			{
				bHasValidPayload = true;
				break;
			}
		}
	}

	if (!bHasValidPayload)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Error, TEXT("[Mission] Cannot start - No PayloadClass assigned in PayloadAttachmentComponent!"));
		}
#endif
		return;
	}

	CountdownTimeRemaining = CountdownStartTime;

	// Send the first number right away, otherwise the UI sits blank for a second.
	OnMissionCountdown.Broadcast(CountdownTimeRemaining);

	GetWorld()->GetTimerManager().SetTimer(
		CountdownTimerHandle,
		this,
		&APayloadMissionManager::TickCountdown,
		1.0f,
		true
	);
}

void APayloadMissionManager::TickCountdown()
{
	CountdownTimeRemaining--;

	// Go on 0, not after it (a 3s countdown used to take 4).
	if (CountdownTimeRemaining <= 0)
	{
		GetWorld()->GetTimerManager().ClearTimer(CountdownTimerHandle);

		// No "0" broadcast - StartMission's state change is the "GO".
		StartMission();
		return;
	}

	OnMissionCountdown.Broadcast(CountdownTimeRemaining);
}

void APayloadMissionManager::StartMission()
{
	if (!bMissionModeEnabled)
		return;

	if (MissionState != EPayloadMissionState::NotStarted)
		return;

	AttemptsRemaining = MaxAttempts;

	MissionState = EPayloadMissionState::InProgress;
	OnMissionStateChanged.Broadcast(MissionState);
	RemainingTime = MissionDuration;

	// Reset now, not when Retry is pressed - the trucks keep driving during
	// the countdown, so an earlier reset looked like it never happened.
	ResetMissionWorld();

	TotalDamageInflicted = 0.f;
	TargetsDestroyedCount = 0;

	EmitMissionLog(TEXT("Mission Started"), ELogSeverity::Info);

	// Grab everything that's already here. Anything spawned later registers
	// itself from ATargetActor::BeginPlay.
	for (TActorIterator<ATargetActor> It(GetWorld()); It; ++It)
	{
		RegisterMissionTarget(*It);
	}
	InitialTargetCount = DamageableTargets.Num();

	// First run only - see CaptureMissionSnapshot.
	CaptureMissionSnapshot();

	GetWorld()->GetTimerManager().SetTimer(
		MissionTimerHandle,
		this,
		&APayloadMissionManager::TickMissionTimer,
		1.0f,
		true
	);

	// Give the player's drone its payload (see SpawnPayloadOnCarrier).
	SpawnPayloadOnCarrier();
}

void APayloadMissionManager::RegisterMissionTarget(ATargetActor* Target)
{
	if (!Target || !Target->bIsMissionTarget)
		return;

	if (MissionState != EPayloadMissionState::InProgress)
		return;

	if (DamageableTargets.Contains(Target))
		return;

	UDamagableComponent* DC = Target->DamagableComponent;
	if (!DC)
		return;

	DamageableTargets.Add(Target);

	// Unbind first. A target can still be bound from the last mission, and
	// binding it twice counted every hit twice.
	DC->OnStructuralStateChanged.RemoveDynamic(
		this, &APayloadMissionManager::OnTargetStructuralStateChanged);
	DC->OnDamageTaken.RemoveDynamic(
		this, &APayloadMissionManager::OnTargetDamageTaken);

	DC->OnStructuralStateChanged.AddDynamic(
		this, &APayloadMissionManager::OnTargetStructuralStateChanged);
	DC->OnDamageTaken.AddDynamic(
		this, &APayloadMissionManager::OnTargetDamageTaken);

	DC->SetHighlightEnabled(true);
}

void APayloadMissionManager::TickMissionTimer()
{
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	if (bTimerFrozen)
		return;

	RemainingTime--;
	OnMissionTimeUpdated.Broadcast(RemainingTime);

	if (RemainingTime <= 0.f)
	{
		FailMission(TEXT("Time expired"));
	}
}

void APayloadMissionManager::FailMission(const FString& Reason)
{
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	bWaitingForLastPayload = false;
	bTimerFrozen = false;

	MissionState = EPayloadMissionState::Failed;
	OnMissionStateChanged.Broadcast(MissionState);
	OnMissionResolved.Broadcast(
		MissionState, InitialTargetCount, TargetsDestroyedCount, TotalDamageInflicted);

	GetWorld()->GetTimerManager().ClearTimer(MissionTimerHandle);

	EmitMissionLog(
		FString::Printf(TEXT("Mission Failed: %s"), *Reason),
		ELogSeverity::Critical
	);
	DisableAllTargetHighlights();

	if (bQuitGameOnFailure)
	{
		GetWorld()->GetTimerManager().SetTimer(
			QuitGameTimerHandle,
			this,
			&APayloadMissionManager::QuitGameDelayed,
			QuitDelaySeconds,
			false
		);
	}
}

void APayloadMissionManager::OnTargetStructuralStateChanged(EStructuralState NewState)
{
	if (NewState != EStructuralState::Destroyed)
		return;

	if (MissionState != EPayloadMissionState::InProgress)
		return;

	for (int32 i = DamageableTargets.Num() - 1; i >= 0; --i)
	{
		AActor* Actor = DamageableTargets[i];
		if (!Actor) { DamageableTargets.RemoveAt(i); continue; }

		UDamagableComponent* DC = Actor->FindComponentByClass<UDamagableComponent>();
		if (DC && DC->StructuralState == EStructuralState::Destroyed)
		{
			TargetsDestroyedCount++;
			DC->SetHighlightEnabled(false);

			// Keep destroyed targets visible for the rest of the mission when world
			// reset is enabled. Cancel the delayed hide; the actor remains available
			// for revival either way.
			if (bResetWorldOnRetry)
			{
				DC->CancelPendingDespawn();
			}

			DamageableTargets.RemoveAt(i);
		}
	}

	if (DamageableTargets.Num() == 0)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			this, &APayloadMissionManager::HandleAllTargetsDestroyed
		);
	}
}

void APayloadMissionManager::HandleAllTargetsDestroyed()
{
	EmitMissionLog(TEXT("All mission targets destroyed"), ELogSeverity::Info);

	bTimerFrozen = true;

	GetWorld()->GetTimerManager().ClearTimer(LastPayloadResolveTimerHandle);

	GetWorld()->GetTimerManager().SetTimer(
		LastPayloadResolveTimerHandle,
		this,
		&APayloadMissionManager::DeferredResolveLastPayload,
		LastPayloadStateChangeDelay,
		false
	);
}

void APayloadMissionManager::SucceedMission()
{
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	bWaitingForLastPayload = false;
	bTimerFrozen = false;
	MissionState = EPayloadMissionState::Success;
	OnMissionStateChanged.Broadcast(MissionState);
	OnMissionResolved.Broadcast(
		MissionState, InitialTargetCount, TargetsDestroyedCount, TotalDamageInflicted);

	GetWorld()->GetTimerManager().ClearTimer(MissionTimerHandle);

	EmitMissionLog(TEXT("Mission Successful"), ELogSeverity::Critical);
	DisableAllTargetHighlights();
}

void APayloadMissionManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (AActor* Actor : DamageableTargets)
	{
		if (Actor)
		{
			if (UDamagableComponent* DC = Actor->FindComponentByClass<UDamagableComponent>())
			{
				DC->OnStructuralStateChanged.RemoveDynamic(
					this,
					&APayloadMissionManager::OnTargetStructuralStateChanged
				);
				DC->OnDamageTaken.RemoveDynamic(
					this,
					&APayloadMissionManager::OnTargetDamageTaken
				);
			}
		}
	}

	// Highlights off BEFORE emptying the list, otherwise there's nothing to turn off.
	DisableAllTargetHighlights();
	DamageableTargets.Empty();

	if (UWorld* World = GetWorld())
	{
		FTimerManager& TM = World->GetTimerManager();
		// UE clears these when we're destroyed anyway, this is just belt and braces
		// (level streaming, world travel etc.).
		TM.ClearTimer(MissionTimerHandle);
		TM.ClearTimer(LastPayloadResolveTimerHandle);
		TM.ClearTimer(LastPayloadWatchdogHandle);
		TM.ClearTimer(CountdownTimerHandle);
		TM.ClearTimer(QuitGameTimerHandle);
		TM.ClearTimer(PayloadRespawnTimerHandle);
	}

	Super::EndPlay(EndPlayReason);
}

void APayloadMissionManager::NotifyAttemptConsumed()
{
	if (!bMissionModeEnabled)
		return;

	if (MissionState != EPayloadMissionState::InProgress)
		return;

	// Clamp at 0. Nothing breaks if it goes negative, but the UI would show
	// "-1 attempts".
	AttemptsRemaining = FMath::Max(AttemptsRemaining - 1, 0);

	EmitMissionLog(
		FString::Printf(TEXT("Attempt consumed. \nRemaining attempts: %d"), AttemptsRemaining),
		ELogSeverity::Warning
	);

	if (AttemptsRemaining <= 0 && DamageableTargets.Num() > 0)
	{
		EnterWaitingForLastPayload();
		return;
	}
	if (DamageableTargets.Num() == 0)
	{
		return;
	}
}

void APayloadMissionManager::RespawnPayloadDelayed()
{
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	if (DamageableTargets.Num() == 0)
		return;

	if (AttemptsRemaining <= 0 || bWaitingForLastPayload)
		return;

	// Give the drone its next payload (see SpawnPayloadOnCarrier).
	SpawnPayloadOnCarrier();
}

void APayloadMissionManager::NotifyLastPayloadResolved()
{
	if (!bWaitingForLastPayload)
		return;

	bTimerFrozen = true;

	if (DamageableTargets.Num() == 0)
	{
		// Nothing left to destroy, so resolve now instead of making the player
		// sit through the watchdog.
		DeferredResolveLastPayload();
		return;
	}

	GetWorld()->GetTimerManager().SetTimer(
		LastPayloadResolveTimerHandle,
		this,
		&APayloadMissionManager::DeferredResolveLastPayload,
		LastPayloadStateChangeDelay,
		false
	);
}

void APayloadMissionManager::DeferredResolveLastPayload()
{
	// The resolve timer and the watchdog can both land here. First one wins,
	// the second sees we're not InProgress anymore and bails.
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	// Resolving now, so kill anything else that could call back in here.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LastPayloadResolveTimerHandle);
		World->GetTimerManager().ClearTimer(LastPayloadWatchdogHandle);
	}

	if (DamageableTargets.Num() == 0)
	{
		SucceedMission();
	}
	else
	{
		const int32 TargetsLeft = DamageableTargets.Num();
		FailMission(
			FString::Printf(
				TEXT("\nAll attempts used.\n%d target%s remaining"),
				TargetsLeft,
				TargetsLeft == 1 ? TEXT("") : TEXT("s")
			)
		);
	}
}

bool APayloadMissionManager::SpawnPayloadOnCarrier()
{
	UWorld* World = GetWorld();
	if (!World)
		return false;

	// Try the drone the player is actually flying first. Going back to the menu
	// and hitting Play can leave old drones lying around, and the payload used to
	// end up on one of those instead.
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* PlayerPawn = PC->GetPawn())
		{
			if (UPayloadAttachmentComponent* PawnComp =
				PlayerPawn->FindComponentByClass<UPayloadAttachmentComponent>())
			{
				PawnComp->SpawnAndAttachPayload();
				return true;
			}
		}
	}

	// No possessed carrier (AI-driven or headless): fall back to a world scan.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (UPayloadAttachmentComponent* PayloadComp =
			It->FindComponentByClass<UPayloadAttachmentComponent>())
		{
			PayloadComp->SpawnAndAttachPayload();
			return true;   // stop ONLY once we've found an actual carrier
		}
		// no break here: keep scanning until a carrier is found
	}

	return false;
}

void APayloadMissionManager::EnterWaitingForLastPayload()
{
	// The one way into "out of attempts, but a payload might still be falling".
	// Going through here means the watchdog always gets armed.
	// If we're already waiting, do nothing - otherwise every call would restart
	// the watchdog and the mission could be kept hanging forever.
	if (bWaitingForLastPayload)
	{
		return;
	}
	bWaitingForLastPayload = true;
	bTimerFrozen = true;
	StartLastPayloadWatchdog();
}

void APayloadMissionManager::StartLastPayloadWatchdog()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	World->GetTimerManager().ClearTimer(LastPayloadWatchdogHandle);
	World->GetTimerManager().SetTimer(
		LastPayloadWatchdogHandle,
		this,
		&APayloadMissionManager::OnLastPayloadWatchdogExpired,
		LastPayloadWatchdogTimeout,
		false
	);
}

void APayloadMissionManager::OnLastPayloadWatchdogExpired()
{
	// Already resolved the normal way? Nothing to do.
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	EmitMissionLog(
		TEXT("Last-payload watchdog expired - force-resolving mission"),
		ELogSeverity::Warning
	);

	// Same exit as a normal resolve.
	DeferredResolveLastPayload();
}

void APayloadMissionManager::EmitMissionLog(
	const FString& Message,
	ELogSeverity Severity
)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FGameLogEntry Log;
	Log.LogType = ELogType::Mission;
	Log.Severity = Severity;
	Log.Message = FText::FromString(Message);
	Log.TimeStamp = World->GetTimeSeconds();

	APlayerController* PC = World->GetFirstPlayerController();
	AHUD* HUD = PC ? PC->GetHUD() : nullptr;

	if (!HUD)
	{
		// No HUD yet — queue and flush when one becomes available.
		PendingMissionLogs.Add(Log);
		return;
	}

	if (!HUD->GetClass()->ImplementsInterface(UMissionLogReceiver::StaticClass()))
	{
		// The HUD doesn't implement the log interface, so drop the log (queuing it
		// would just grow forever). Warn once, in shipping builds too, or you'd never
		// know why the logs aren't showing up.
		if (!bWarnedAboutMissingInterface)
		{
			UE_LOG(LogDynamicPayload, Warning,
				TEXT("[Mission] HUD '%s' does not implement IMissionLogReceiver — mission logs will be dropped."),
				*HUD->GetClass()->GetName());
			bWarnedAboutMissingInterface = true;
		}
		PendingMissionLogs.Reset();
		return;
	}

	// Send the backlog first so everything shows up in order.
	for (const FGameLogEntry& Pending : PendingMissionLogs)
	{
		IMissionLogReceiver::Execute_PushGameLog(HUD, Pending);
	}
	PendingMissionLogs.Reset();

	IMissionLogReceiver::Execute_PushGameLog(HUD, Log);
}

void APayloadMissionManager::DisableAllTargetHighlights()
{
	for (AActor* Actor : DamageableTargets)
	{
		if (!Actor)
			continue;

		if (UDamagableComponent* DC =
			Actor->FindComponentByClass<UDamagableComponent>())
		{
			DC->SetHighlightEnabled(false);
		}
	}
}

void APayloadMissionManager::NotifyDroneDestroyed()
{
	if (!bMissionModeEnabled)
		return;

	if (MissionState != EPayloadMissionState::InProgress)
		return;

	// If the kamikaze took out the LAST target, that's a win - don't turn it
	// into a failure just because the drone blew up too. An empty list means
	// HandleAllTargetsDestroyed is already queued, so let that handle it.
	if (DamageableTargets.Num() == 0)
		return;

	FailMission(TEXT("Drone destroyed"));
}

void APayloadMissionManager::NotifyKamikazeTriggered()
{
	if (!bMissionModeEnabled)
		return;

	if (MissionState != EPayloadMissionState::InProgress)
		return;

	AttemptsRemaining = FMath::Max(AttemptsRemaining - 1, 0);

	EmitMissionLog(
		FString::Printf(TEXT("Kamikaze triggered.\nAttempt consumed. Remaining: %d"), AttemptsRemaining),
		ELogSeverity::Warning
	);

	// Only wait if the player is really out of attempts AND something is still
	// standing. This used to trigger on every kamikaze and froze the mission
	// even with attempts to spare.
	if (AttemptsRemaining <= 0 && DamageableTargets.Num() > 0)
	{
		// Arm the resolve timer here. Relying on the payload's Explode() to call
		// back at the right moment is what caused the old hang. The watchdog is
		// the backup; this is the normal, faster path.
		EnterWaitingForLastPayload();

		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(LastPayloadResolveTimerHandle);
			World->GetTimerManager().SetTimer(
				LastPayloadResolveTimerHandle,
				this,
				&APayloadMissionManager::DeferredResolveLastPayload,
				LastPayloadStateChangeDelay,
				false
			);
		}
	}
}

void APayloadMissionManager::CaptureMissionSnapshot()
{
	if (bMissionSnapshotCaptured)
		return;

	// Level target snapshots are normally captured one tick after BeginPlay.
	CaptureLevelTargets();

	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				PlayerRestartTransform = Pawn->GetActorTransform();
				CapturedPlayerPawnClass = Pawn->GetClass();

				// Remember what the config screen gave it, so the respawned drone matches.
				if (const UPayloadAttachmentComponent* Attach =
					Pawn->FindComponentByClass<UPayloadAttachmentComponent>())
				{
					CapturedPayloadClass = Attach->PayloadClass;
					bCapturedKamikazeMode = Attach->bKamikazeMode;
				}
			}
		}
	}

	bMissionSnapshotCaptured = true;
}

void APayloadMissionManager::CaptureLevelTargets()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	// Bind after the player controller becomes available so possession changes
	// can be observed in both Free Play and Mission Play.
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		PC->OnPossessedPawnChanged.AddUniqueDynamic(
			this, &APayloadMissionManager::OnPlayerPossessedPawnChanged);

		// Save a fallback restart transform for levels without a PlayerStart.
		if (!bMissionSnapshotCaptured && PC->GetPawn())
		{
			PlayerRestartTransform = PC->GetPawn()->GetActorTransform();
		}
	}

	if (bTargetSnapshotCaptured)
		return;

	MissionTargetSnapshots.Reset();
	for (TActorIterator<ATargetActor> It(World); It; ++It)
	{
		ATargetActor* Target = *It;
		if (!IsValid(Target))
			continue;

		FMissionTargetSnapshot Snapshot;
		Snapshot.TargetClass = Target->GetClass();
		Snapshot.LiveActor = Target;

		// Preserve the level placement using the transform recorded at BeginPlay.
		Snapshot.SpawnTransform = Target->GetInitialTransform();

		MissionTargetSnapshots.Add(Snapshot);
	}

	bTargetSnapshotCaptured = true;

#if WITH_EDITOR
	if (bShowDebug)
	{
		UE_LOG(LogDynamicPayload, Display,
			TEXT("[Mission] Recorded %d level targets for resets"), MissionTargetSnapshots.Num());
	}
#endif
}

void APayloadMissionManager::OnPlayerPossessedPawnChanged(APawn* OldPawn, APawn* NewPawn)
{
	// Ignore pawn unpossession and mission-managed respawn events.
	if (!NewPawn || bResettingPlayer)
		return;

	// Defer handling until the next tick because the menu possesses the pawn
	// before updating bMissionModeEnabled and calling RequestMissionStart.
	GetWorld()->GetTimerManager().SetTimerForNextTick(
		this, &APayloadMissionManager::HandleNewSessionStarted);
}

void APayloadMissionManager::HandleNewSessionStarted()
{
	// Clear the previous mission state and timers when entering Free Play from
	// a running mission, countdown, or completed mission.
	if (!bMissionModeEnabled)
	{
		if (MissionState != EPayloadMissionState::NotStarted
			|| GetWorld()->GetTimerManager().IsTimerActive(CountdownTimerHandle))
		{
			TeardownMission();
		}
	}
	// StartMission resets the world after a countdown. Preserve world state for
	// an active mission and reset targets for other new sessions.
	else if (MissionState == EPayloadMissionState::InProgress)
	{
		return;
	}

	ResetTargetsToStart();
}

void APayloadMissionManager::ResetPlayerToStart()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
		return;

	TGuardValue<bool> ResettingGuard(bResettingPlayer, true);

	// Use the PlayerStart if there is one, otherwise the saved transform.
	FTransform Destination = PlayerRestartTransform;
	TActorIterator<APlayerStart> PlayerStartIt(World);
	if (PlayerStartIt)
	{
		Destination = PlayerStartIt->GetActorTransform();
	}

	APawn* OldPawn = PC->GetPawn();

	// Prefer the saved class - the old pawn might not exist anymore.
	TSubclassOf<APawn> PawnClass = CapturedPlayerPawnClass;
	if (!PawnClass && OldPawn)
	{
		PawnClass = OldPawn->GetClass();
	}
	if (!PawnClass)
	{
		if (const AGameModeBase* GameMode = World->GetAuthGameMode())
		{
			PawnClass = GameMode->DefaultPawnClass;
		}
	}

	if (!PawnClass)
	{
		// Nothing to rebuild from. Move whatever is there and bail.
		if (OldPawn)
		{
			if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(OldPawn->GetRootComponent()))
			{
				if (Prim->IsSimulatingPhysics())
				{
					Prim->SetPhysicsLinearVelocity(FVector::ZeroVector);
					Prim->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
				}
			}
			OldPawn->TeleportTo(Destination.GetLocation(), Destination.Rotator(), false, true);
		}
		return;
	}

	// Keep the payload setup from the current drone if there is one,
	// otherwise use what was saved at mission start.
	TSubclassOf<APayload> PayloadClassToApply = CapturedPayloadClass;
	bool bKamikazeToApply = bCapturedKamikazeMode;
	if (OldPawn)
	{
		if (const UPayloadAttachmentComponent* OldAttach =
			OldPawn->FindComponentByClass<UPayloadAttachmentComponent>())
		{
			if (OldAttach->PayloadClass)
			{
				PayloadClassToApply = OldAttach->PayloadClass;
			}
			bKamikazeToApply = OldAttach->bKamikazeMode;
		}
	}

	// Spawn a new drone instead of teleporting. After a kamikaze there's
	// nothing left to teleport, and a fresh one has no leftover velocity or
	// half-attached payload either.
	if (OldPawn)
	{
		PC->UnPossess();
		OldPawn->Destroy();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = PC;

	APawn* NewPawn = World->SpawnActor<APawn>(
		PawnClass, Destination.GetLocation(), Destination.Rotator(), SpawnParams);

	if (!NewPawn)
	{
		UE_LOG(LogDynamicPayload, Error,
			TEXT("[Mission] Could not respawn the player pawn (%s); the player is left without one."),
			*PawnClass->GetName());
		return;
	}

	if (UPayloadAttachmentComponent* NewAttach =
		NewPawn->FindComponentByClass<UPayloadAttachmentComponent>())
	{
		if (PayloadClassToApply)
		{
			NewAttach->PayloadClass = PayloadClassToApply;
		}
		NewAttach->bKamikazeMode = bKamikazeToApply;
	}

	PC->Possess(NewPawn);
}

void APayloadMissionManager::ResetMissionWorld()
{
	if (!bResetWorldOnRetry)
		return;

	ResetTargetsToStart();

	if (bResetPlayerOnRetry)
	{
		ResetPlayerToStart();
	}
}

void APayloadMissionManager::ClearLeftoversFromLastAttempt()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	// Blasts from the last attempt. Their sound is attached to them, so this is
	// also what stops a bang that was cut off by the pause from finishing after
	// the retry. Only ones that already went off - placed explosives stay.
	for (TActorIterator<AExplosive> It(World); It; ++It)
	{
		if (It->HasDetonated())
		{
			It->Destroy();
		}
	}

	// Same story for camera shakes: they live on the player's camera manager,
	// not on the explosive, so they just pause with the game and carry on
	// shaking after the retry. Kill them outright.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->StopAllCameraShakes(/*bImmediately=*/ true);
			}
		}
	}

	// Payloads still falling. Left alone they'd land after the unpause and hit
	// the freshly revived targets. Ones a drone is still holding are kept.
	TSet<const APayload*> Held;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (const UPayloadAttachmentComponent* Attach =
			It->FindComponentByClass<UPayloadAttachmentComponent>())
		{
			if (const APayload* P = Attach->GetAttachedPayload())
			{
				Held.Add(P);
			}
		}
	}
	for (TActorIterator<APayload> It(World); It; ++It)
	{
		if (!Held.Contains(*It))
		{
			It->Destroy();
		}
	}
}

void APayloadMissionManager::ResetTargetsToStart()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	// Capture targets on demand if reset runs before the deferred startup capture.
	CaptureLevelTargets();

	ClearLeftoversFromLastAttempt();

	for (FMissionTargetSnapshot& Snapshot : MissionTargetSnapshots)
	{
		AActor* Actor = Snapshot.LiveActor.Get();

		if (!IsValid(Actor))
		{
			// Shouldn't really happen anymore, since destroyed targets are only hidden.
			// If something else deleted it (BP Destroy, level streaming) we respawn it
			// from its class - but that loses the level-placed setup (spline, convoy
			// leader, movement mode) and it won't drive, hence the warning.
			if (!Snapshot.TargetClass)
				continue;

			UE_LOG(LogDynamicPayload, Warning,
				TEXT("[Mission] Target '%s' left play and had to be rebuilt from its class. "
					 "Level-instance movement settings (spline, convoy leader, mode) are lost "
					 "and it will not move."),
				*Snapshot.TargetClass->GetName());

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			Actor = World->SpawnActor<AActor>(
				Snapshot.TargetClass, Snapshot.SpawnTransform, SpawnParams);

			Snapshot.LiveActor = Actor;
			if (!Actor)
				continue;
		}
		else if (UDamagableComponent* DC = Actor->FindComponentByClass<UDamagableComponent>())
		{
			DC->Revive();
		}

		// Put movers back on their route (this also clears their movement state).
		if (UMovableTargetComponent* MC = Actor->FindComponentByClass<UMovableTargetComponent>())
		{
			MC->ResetToStart();
		}
		else
		{
			// Restore static targets displaced by an explosion.
			Actor->SetActorTransform(Snapshot.SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
}

void APayloadMissionManager::RetryMission()
{
	TeardownMission();

	// Reset after teardown removes delegate bindings. Revive broadcasts state
	// changes that should not be handled as events from the new mission.
	ResetMissionWorld();

	HandleMissionStart();
}

void APayloadMissionManager::TeardownMission()
{
	GetWorld()->GetTimerManager().ClearTimer(MissionTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(QuitGameTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(LastPayloadResolveTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(LastPayloadWatchdogHandle);

	bWaitingForLastPayload = false;
	bTimerFrozen = false;
	MissionState = EPayloadMissionState::NotStarted;
	OnMissionStateChanged.Broadcast(MissionState);

	RemainingTime = MissionDuration;
	AttemptsRemaining = MaxAttempts;
	TotalDamageInflicted = 0.f;
	TargetsDestroyedCount = 0;
	InitialTargetCount = 0;

	// Unbind from every recorded target, not just DamageableTargets - dead
	// targets were already removed from that list and would stay bound.
	auto UnbindFrom = [this](AActor* Actor)
	{
		if (!IsValid(Actor))
			return;

		if (UDamagableComponent* DC = Actor->FindComponentByClass<UDamagableComponent>())
		{
			DC->OnStructuralStateChanged.RemoveDynamic(
				this,
				&APayloadMissionManager::OnTargetStructuralStateChanged
			);
			DC->OnDamageTaken.RemoveDynamic(
				this,
				&APayloadMissionManager::OnTargetDamageTaken
			);
		}
	};

	for (AActor* Actor : DamageableTargets)
	{
		UnbindFrom(Actor);
	}
	for (const FMissionTargetSnapshot& Snapshot : MissionTargetSnapshots)
	{
		UnbindFrom(Snapshot.LiveActor.Get());
	}
	DisableAllTargetHighlights();
	DamageableTargets.Empty();

	// Clear any pre-HUD logs queued during the failed mission so they don't
	// flush into the HUD at retry start.
	PendingMissionLogs.Reset();
}

void APayloadMissionManager::QuitGameDelayed()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC)
	{
		PC->ConsoleCommand(TEXT("quit"));
	}
}

void APayloadMissionManager::OnTargetDamageTaken(float DamageAmount, float RemainingHealth)
{
	if (MissionState != EPayloadMissionState::InProgress)
		return;

	TotalDamageInflicted += DamageAmount;
}
