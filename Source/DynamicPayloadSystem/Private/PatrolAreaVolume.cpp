// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "PatrolAreaVolume.h"
#include "Components/BoxComponent.h"

APatrolAreaVolume::APatrolAreaVolume()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	Bounds->SetupAttachment(Root);

	Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bounds->SetGenerateOverlapEvents(false);
}

FVector APatrolAreaVolume::GetRandomPointInArea() const
{
	if (!Bounds)
		return GetActorLocation();

	const FVector Origin = Bounds->GetComponentLocation();
	const FVector Extent = Bounds->GetScaledBoxExtent();

	// Keep points at the volume's Z coordinate. Ground vehicles ignore height,
	// and varying Z can produce unreachable points.
	return FVector(
		Origin.X + FMath::FRandRange(-Extent.X, Extent.X),
		Origin.Y + FMath::FRandRange(-Extent.Y, Extent.Y),
		Origin.Z
	);
}
