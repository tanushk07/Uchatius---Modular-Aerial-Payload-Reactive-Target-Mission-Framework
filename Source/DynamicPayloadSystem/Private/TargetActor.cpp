// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "TargetActor.h"
#include "DamagableComponent.h"
#include "Components/StaticMeshComponent.h"
#include "TargetBehaviorComponent.h"
#include "MovableTargetComponent.h"
#include "PayloadMissionManager.h"
#include "EngineUtils.h"

ATargetActor::ATargetActor()
{
	// No tick, Tick() is empty. Subclasses can turn it on if they need it.
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GroundFrame = CreateDefaultSubobject<USceneComponent>(TEXT("GroundFrame"));
	GroundFrame->SetupAttachment(Root);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(GroundFrame);

	Mesh->SetCollisionObjectType(ECC_WorldDynamic);
	Mesh->SetGenerateOverlapEvents(true);

	DamagableComponent =
		CreateDefaultSubobject<UDamagableComponent>(TEXT("DamagableComponent"));

	BehaviorComponent =
		CreateDefaultSubobject<UTargetBehaviorComponent>(TEXT("BehaviorComponent"));

	MovementComponent =
		CreateDefaultSubobject<UMovableTargetComponent>(TEXT("MovementComponent"));
}

void ATargetActor::BeginPlay()
{
	Super::BeginPlay();

	// Save the initial state used by mission resets.
	InitialTransform = GetActorTransform();
	bInitialActorTickEnabled = IsActorTickEnabled();
	if (Mesh)
	{
		InitialCollisionEnabled = Mesh->GetCollisionEnabled();
	}
	if (MovementComponent)
	{
		bInitialMovementTickEnabled = MovementComponent->IsComponentTickEnabled();
	}

	if (!IntactMesh && Mesh)
	{
		IntactMesh = Mesh->GetStaticMesh();
	}

	if (Mesh)
	{
		Mesh->bRenderCustomDepth = bIsMissionTarget;
		Mesh->MarkRenderStateDirty();
	}

	if (DamagableComponent)
	{
		DamagableComponent->OnStructuralStateChanged.AddDynamic(
			this,
			&ATargetActor::HandleStructuralStateChanged
		);
	}

	// Register targets spawned during an active mission. StartMission collects
	// targets spawned before the mission begins.
	if (bIsMissionTarget)
	{
		TActorIterator<APayloadMissionManager> MissionManagerIt(GetWorld());
		if (MissionManagerIt)
		{
			MissionManagerIt->RegisterMissionTarget(this);
		}
	}
}

void ATargetActor::HandleStructuralStateChanged(EStructuralState NewState)
{
	if (!Mesh) return;

	UStaticMesh* NewMesh = nullptr;

	switch (NewState)
	{
	case EStructuralState::Intact:
		NewMesh = IntactMesh;
		break;
	case EStructuralState::Damaged:
		NewMesh = DamagedMesh;
		break;
	case EStructuralState::Destroyed:
		NewMesh = DestroyedMesh;
		break;
	}

	if (NewMesh)
	{
		Mesh->SetStaticMesh(NewMesh);
		Mesh->bRenderCustomDepth = bIsMissionTarget;
		Mesh->MarkRenderStateDirty();

		if (MovementComponent)
		{
			MovementComponent->CacheOwnerBounds();
		}
	}

	if (NewState == EStructuralState::Destroyed)
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->bRenderCustomDepth = false;
		Mesh->MarkRenderStateDirty();

		if (MovementComponent)
		{
			MovementComponent->SetComponentTickEnabled(false);
		}
		SetActorTickEnabled(false);
	}
	else
	{
		// Restore collision, rendering, and movement after the target is revived.
		Mesh->SetCollisionEnabled(InitialCollisionEnabled);
		Mesh->bRenderCustomDepth = bIsMissionTarget;
		Mesh->MarkRenderStateDirty();

		if (MovementComponent)
		{
			MovementComponent->SetComponentTickEnabled(bInitialMovementTickEnabled);
		}
		SetActorTickEnabled(bInitialActorTickEnabled);
	}
}
