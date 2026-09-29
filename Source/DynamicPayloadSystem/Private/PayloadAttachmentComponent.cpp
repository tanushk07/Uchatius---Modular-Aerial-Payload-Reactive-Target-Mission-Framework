// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "PayloadAttachmentComponent.h"
#include "DynamicPayloadSystemModule.h"
#include "Payload.h"
#include "PayloadMissionManager.h"
#include "DamagableComponent.h"
#include "TargetActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

UPayloadAttachmentComponent::UPayloadAttachmentComponent()
{
	// Tick to recheck existing overlaps. Kamikaze mode may be enabled after
	// BeginPlay, so ticking remains enabled regardless of its initial value.
	PrimaryComponentTick.bCanEverTick = true;
}

void UPayloadAttachmentComponent::BeginPlay()
{
	Super::BeginPlay();

	// Cache the mission manager.
	TActorIterator<APayloadMissionManager> MissionManagerIt(GetWorld());
	if (MissionManagerIt)
	{
		CachedMissionManager = *MissionManagerIt;
	}

	// Bind regardless of the initial setting because the configuration screen
	// may enable kamikaze mode after BeginPlay.
	RefreshKamikazeBinding();

	if (bAutoSpawnOnBeginPlay && PayloadClass)
	{
		SpawnAndAttachPayload();
	}
}

void UPayloadAttachmentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Destroy the payload only while it remains attached to this actor.
	if (AttachedPayload)
	{
		AttachedPayload->Destroy();
		AttachedPayload = nullptr;
		CachedPayloadMass = 0.0f;
	}

	Super::EndPlay(EndPlayReason);
}

void UPayloadAttachmentComponent::SpawnAndAttachPayload()
{
	if (AttachedPayload || !PayloadClass)
		return;

	// Check mission mode.
	APayloadMissionManager* MissionManager = CachedMissionManager;

	if (MissionManager && MissionManager->IsMissionSystemEnabled())
	{
		if (!MissionManager->IsMissionModeActive())
			return;
	}

	UPrimitiveComponent* OwnerRoot = GetOwnerRootMesh();
	if (!OwnerRoot)
		return;

	FVector SpawnLocation = GetOwner()->GetActorLocation() +
		GetOwner()->GetActorRotation().RotateVector(AttachOffset);

	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.Instigator = Cast<APawn>(GetOwner());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AttachedPayload = GetWorld()->SpawnActor<APayload>(
		PayloadClass,
		SpawnLocation,
		GetOwner()->GetActorRotation(),
		Params
	);

	if (!AttachedPayload)
		return;

	// Use the configured mission fuse time when available.
	if (MissionManager)
	{
		AttachedPayload->FuseTime = MissionManager->ConfiguredFuseTime;
	}

	UStaticMeshComponent* PayloadMesh =
		Cast<UStaticMeshComponent>(AttachedPayload->GetRootComponent());

	if (!PayloadMesh)
	{
		AttachedPayload->Destroy();
		AttachedPayload = nullptr;
		return;
	}

	PayloadMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PayloadMesh->SetCollisionProfileName(TEXT("PhysicsActor"));

	// Temporarily enable physics to read the payload mass.
	PayloadMesh->SetSimulatePhysics(true);
	CachedPayloadMass = PayloadMesh->GetMass();

	if (CachedPayloadMass <= 0.0f)
	{
		CachedPayloadMass = 1.0f;
	}

	// Prevent collisions between the drone and its attached payload.
	OwnerRoot->IgnoreActorWhenMoving(AttachedPayload, true);
	PayloadMesh->IgnoreActorWhenMoving(GetOwner(), true);
	OwnerRoot->IgnoreComponentWhenMoving(PayloadMesh, true);
	PayloadMesh->IgnoreComponentWhenMoving(OwnerRoot, true);

	ECollisionChannel OwnerChannel = OwnerRoot->GetCollisionObjectType();
	PayloadMesh->SetCollisionResponseToChannel(OwnerChannel, ECR_Ignore);
	PayloadMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

	if (bEnableDanglingPhysics)
	{
		// DANGLING: The payload simulates under a physics constraint.
		PayloadMesh->SetEnableGravity(true);
		CreatePhysicsConstraint();
	}
	else
	{
		// KINEMATIC: attach without simulating physics.
		PayloadMesh->SetSimulatePhysics(false);
		PayloadMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		AttachedPayload->AttachToComponent(
			OwnerRoot,
			FAttachmentTransformRules::KeepWorldTransform
		);
	}

	// Notify listeners of the payload state change.
	OnPayloadStateChanged.Broadcast(true);
}

void UPayloadAttachmentComponent::CreatePhysicsConstraint()
{
	if (PayloadConstraint || !AttachedPayload)
		return;

	UPrimitiveComponent* OwnerRoot = GetOwnerRootMesh();
	UStaticMeshComponent* PayloadMesh =
		Cast<UStaticMeshComponent>(AttachedPayload->GetRootComponent());

	if (!OwnerRoot || !PayloadMesh)
		return;

	// Generate a unique name for each instance to avoid reusing an FName across
	// spawn and drop cycles.
	const FName UniqueName = MakeUniqueObjectName(
		GetOwner(), UPhysicsConstraintComponent::StaticClass(), TEXT("PayloadConstraint"));
	PayloadConstraint = NewObject<UPhysicsConstraintComponent>(GetOwner(), UniqueName);
	PayloadConstraint->RegisterComponent();
	PayloadConstraint->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepRelativeTransform);
	PayloadConstraint->SetRelativeLocation(FVector::ZeroVector);

	// Ball joint
	PayloadConstraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
	PayloadConstraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
	PayloadConstraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);

	// SwingAngleLimit controls how far it can swing:
	//   0      = locked (kinematic)
	//   1..89  = swings up to that many degrees each way
	//   90     = effectively unrestricted swing
	// Twist remains unconstrained.
	if (SwingAngleLimit > 0.f)
	{
		PayloadConstraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, SwingAngleLimit);
		PayloadConstraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, SwingAngleLimit);
	}
	else
	{
		PayloadConstraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0);
		PayloadConstraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
	}
	PayloadConstraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0);

	PayloadConstraint->SetConstrainedComponents(OwnerRoot, NAME_None, PayloadMesh, NAME_None);

	PayloadConstraint->ConstraintInstance.SetRefPosition(EConstraintFrame::Frame1, AttachOffset);
	PayloadConstraint->ConstraintInstance.SetRefPosition(EConstraintFrame::Frame2, FVector::ZeroVector);

	PayloadConstraint->SetDisableCollision(true);
}

void UPayloadAttachmentComponent::DestroyPhysicsConstraint()
{
	if (PayloadConstraint)
	{
		PayloadConstraint->BreakConstraint();
		PayloadConstraint->DestroyComponent();
		PayloadConstraint = nullptr;
	}
}

void UPayloadAttachmentComponent::DetachPayload()
{
	if (!AttachedPayload)
		return;

	UPrimitiveComponent* OwnerRoot = GetOwnerRootMesh();
	UStaticMeshComponent* PayloadMesh =
		Cast<UStaticMeshComponent>(AttachedPayload->GetRootComponent());

	if (!OwnerRoot || !PayloadMesh)
		return;

	// Restore collision settings changed during attachment before detaching.
	OwnerRoot->IgnoreActorWhenMoving(AttachedPayload, false);
	PayloadMesh->IgnoreActorWhenMoving(GetOwner(), false);
	OwnerRoot->IgnoreComponentWhenMoving(PayloadMesh, false);
	PayloadMesh->IgnoreComponentWhenMoving(OwnerRoot, false);

	ECollisionChannel OwnerChannel = OwnerRoot->GetCollisionObjectType();
	PayloadMesh->SetCollisionResponseToChannel(OwnerChannel, ECR_Block);
	// Keep camera collision ignored so the payload does not obstruct the view.

	if (bEnableDanglingPhysics)
	{
		DestroyPhysicsConstraint();

		if (bTransferVelocityOnDetach)
		{
			FVector OwnerVelocity = OwnerRoot->GetComponentVelocity();
			FVector CurrentVelocity = PayloadMesh->GetComponentVelocity();
			if (CurrentVelocity.SizeSquared() < OwnerVelocity.SizeSquared())
			{
				PayloadMesh->SetPhysicsLinearVelocity(OwnerVelocity, true);
			}
		}
	}
	else
	{
		// Kinematic: detach and turn physics on
		FVector OwnerVelocity = OwnerRoot->GetComponentVelocity();
		AttachedPayload->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		PayloadMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		PayloadMesh->SetSimulatePhysics(true);
		PayloadMesh->SetEnableGravity(true);
		PayloadMesh->SetPhysicsLinearVelocity(OwnerVelocity);
	}

	APayload* DetachedPayload = AttachedPayload;

	AttachedPayload = nullptr;
	CachedPayloadMass = 0.0f;

	DetachedPayload->Arm();

	// Notify listeners that the payload was detached.
	OnPayloadStateChanged.Broadcast(false);

	// Notify the mission manager that the payload was dropped.
	if (CachedMissionManager)
	{
		CachedMissionManager->NotifyAttemptConsumed();
	}
}

FVector UPayloadAttachmentComponent::GetPayloadWorldPosition() const
{
	if (!AttachedPayload)
	{
		return GetOwner()->GetActorLocation() +
			GetOwner()->GetActorRotation().RotateVector(AttachOffset);
	}

	return AttachedPayload->GetActorLocation();
}

FVector UPayloadAttachmentComponent::GetPayloadLocalOffset() const
{
	if (!AttachedPayload || !GetOwner())
	{
		return AttachOffset;
	}

	FVector WorldOffset = AttachedPayload->GetActorLocation() - GetOwner()->GetActorLocation();
	return GetOwner()->GetActorRotation().UnrotateVector(WorldOffset);
}

void UPayloadAttachmentComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Recheck existing overlaps each tick. BeginOverlap fires only on entry, when
	// the vehicle may still be below the kamikaze speed threshold.
	if (!bKamikazeMode || !AttachedPayload || !KamikazeTriggerMesh)
		return;

	// Check speed before querying overlapping components.
	const UPrimitiveComponent* OwnerRoot = GetOwnerRootMesh();
	if (OwnerRoot &&
		OwnerRoot->GetComponentVelocity().Size() / 100.0f < MinKamikazeSpeed_ms)
	{
		return;
	}

	TArray<UPrimitiveComponent*> Overlaps;
	KamikazeTriggerMesh->GetOverlappingComponents(Overlaps);
	for (UPrimitiveComponent* Other : Overlaps)
	{
		if (!Other)
			continue;

		// Detonation may destroy the owner; stop processing overlaps immediately.
		if (TryKamikazeDetonate(Other->GetOwner(), Other))
			return;
	}
}

void UPayloadAttachmentComponent::RefreshKamikazeBinding()
{
	if (bKamikazeBindingDone)
		return;

	AActor* Owner = GetOwner();
	if (!Owner)
		return;

	if (!KamikazeTriggerMeshName.IsNone())
	{
		TArray<UActorComponent*> Children;
		Owner->GetComponents(UPrimitiveComponent::StaticClass(), Children);
		for (UActorComponent* Child : Children)
		{
			if (Child->GetFName() == KamikazeTriggerMeshName)
			{
				KamikazeTriggerMesh = Cast<UPrimitiveComponent>(Child);
				break;
			}
		}
	}

	// Fall back to the root body if the configured trigger mesh is not found.
	if (!KamikazeTriggerMesh)
	{
		KamikazeTriggerMesh = GetOwnerRootMesh();
	}

	if (!KamikazeTriggerMesh)
	{
		UE_LOG(LogDynamicPayload, Warning,
			TEXT("[Payload] %s has no usable kamikaze trigger mesh ('%s' not found and no root primitive); kamikaze cannot fire."),
			*Owner->GetName(), *KamikazeTriggerMeshName.ToString());
		return;
	}

	KamikazeTriggerMesh->SetGenerateOverlapEvents(true);

	// Enable hit notifications for simulating bodies.
	KamikazeTriggerMesh->SetNotifyRigidBodyCollision(true);

	// Skeletal meshes simulate through their physics asset bodies, not their own
	// BodyInstance, so the call above doesn't reach them. The drone root is a
	// skeletal mesh, so without this the hit events never come.
	if (USkeletalMeshComponent* SkeletalTrigger = Cast<USkeletalMeshComponent>(KamikazeTriggerMesh))
	{
		SkeletalTrigger->SetAllBodiesNotifyRigidBodyCollision(true);
	}

	KamikazeTriggerMesh->OnComponentBeginOverlap.AddDynamic(
		this, &UPayloadAttachmentComponent::OnKamikazeOverlap);
	KamikazeTriggerMesh->OnComponentHit.AddDynamic(
		this, &UPayloadAttachmentComponent::OnKamikazeHit);

	bKamikazeBindingDone = true;
}

void UPayloadAttachmentComponent::OnKamikazeOverlap(
	UPrimitiveComponent* OverlappedComp,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	TryKamikazeDetonate(OtherActor, OtherComp);
}

void UPayloadAttachmentComponent::OnKamikazeHit(
	UPrimitiveComponent* HitComp,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	TryKamikazeDetonate(OtherActor, OtherComp);
}

bool UPayloadAttachmentComponent::TryKamikazeDetonate(
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp)
{
	if (!OtherActor || OtherActor == GetOwner())
		return false;

	// Ignore query-only components such as health widgets, selection volumes,
	// and audio ranges. Only physical target components should trigger detonation.
	// Apply the same solidity check to overlap and hit events.
	if (OtherComp)
	{
		const ECollisionEnabled::Type Solidity = OtherComp->GetCollisionEnabled();
		if (Solidity != ECollisionEnabled::QueryAndPhysics &&
			Solidity != ECollisionEnabled::PhysicsOnly)
		{
			return false;
		}
	}

	if (AttachedPayload && OtherActor == AttachedPayload)
		return false;

	if (!bKamikazeMode)
		return false;

	if (!AttachedPayload)
		return false;

	// Check the minimum kamikaze speed.
	UPrimitiveComponent* OwnerRoot = GetOwnerRootMesh();
	if (OwnerRoot)
	{
		const float Speed_ms = OwnerRoot->GetComponentVelocity().Size() / 100.0f;
		if (Speed_ms < MinKamikazeSpeed_ms)
			return false;
	}

	UDamagableComponent* DamageComp = OtherActor->FindComponentByClass<UDamagableComponent>();
	if (!DamageComp)
		return false;

	ATargetActor* Target = Cast<ATargetActor>(OtherActor);
	if (Target && !Target->bIsMissionTarget)
		return false;

	DestroyPhysicsConstraint();

	// Notify the mission manager.
	if (CachedMissionManager)
	{
		CachedMissionManager->NotifyKamikazeTriggered();
	}

	const FVector ExplosionLocation = KamikazeTriggerMesh ?
		KamikazeTriggerMesh->GetComponentLocation() : GetOwner()->GetActorLocation();
	AttachedPayload->Explode(ExplosionLocation);
	AttachedPayload = nullptr;
	CachedPayloadMass = 0.0f;

	// Notify listeners that the payload has been removed.
	OnPayloadStateChanged.Broadcast(false);

	// Notify the mission manager after the explosion so destroyed targets are
	// processed before the drone-destruction result is evaluated.
	if (CachedMissionManager)
	{
		CachedMissionManager->NotifyDroneDestroyed();
	}

	GetOwner()->Destroy();
	return true;
}

UPrimitiveComponent* UPayloadAttachmentComponent::GetOwnerRootMesh() const
{
	if (!GetOwner())
		return nullptr;

	return Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
}
