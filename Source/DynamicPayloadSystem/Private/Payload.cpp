// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "Payload.h"
#include "Explosive.h"
#include "DynamicPayloadSystemModule.h"
#include "Components/StaticMeshComponent.h"
#include "PayloadMissionManager.h"
#include "EngineUtils.h"
#include "DamagableComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

APayload::APayload()
{
	PrimaryActorTick.bCanEverTick = false;
	PayloadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PayloadMesh"));
	RootComponent = PayloadMesh;

	PayloadMesh->SetSimulatePhysics(false);
	PayloadMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PayloadMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
}

void APayload::BeginPlay()
{
	Super::BeginPlay();

	// Cache mission manager reference once
	TActorIterator<APayloadMissionManager> MissionManagerIt(GetWorld());
	if (MissionManagerIt)
	{
		CachedMissionManager = *MissionManagerIt;
	}
}

void APayload::Explode(const FVector& ExplosionLocation)
{
	if (bHasExploded)
		return;

	bHasExploded = true;

	UWorld* World = GetWorld();
	if (World)
	{
		World->GetTimerManager().ClearTimer(FuseTimerHandle);
	}

	if (ExplosionClass && World)
	{
		const FTransform SpawnTransform(FRotator::ZeroRotator, ExplosionLocation);

		AExplosive* Explosive = World->SpawnActorDeferred<AExplosive>(
			ExplosionClass,
			SpawnTransform,
			this,
			GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn
		);

		if (Explosive)
		{
			Explosive->bExplodeOnBeginPlay = true;
			UGameplayStatics::FinishSpawningActor(Explosive, SpawnTransform);
		}
	}

	if (CachedMissionManager)
	{
		CachedMissionManager->RespawnPayloadDelayed();
		CachedMissionManager->NotifyLastPayloadResolved();
	}

	Destroy();
}

void APayload::Arm()
{
	if (bIsArmed || bHasExploded)
		return;

	bIsArmed = true;

#if WITH_EDITOR
	if (bShowDebug)
	{
		UE_LOG(LogDynamicPayload, Warning, TEXT("Payload armed"));
	}
#endif

	if (bExplodeOnHit)
	{
		// Set it ourselves so hit events work even if someone changed the collision
		// profiles. PhysicsActor normally has it on, but I'd rather not count on that.
		PayloadMesh->SetNotifyRigidBodyCollision(true);
		PayloadMesh->OnComponentHit.AddDynamic(
			this, &APayload::OnPayloadHit
		);
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			FuseTimerHandle,
			this,
			&APayload::OnFuseExpired,
			FuseTime,
			false
		);
	}
}

void APayload::OnFuseExpired()
{
	if (bHasExploded)
		return;

#if WITH_EDITOR
	if (bShowDebug)
	{
		UE_LOG(LogDynamicPayload, Warning, TEXT("Payload fuse expired"));
	}
#endif

	Explode(GetActorLocation());
}

void APayload::OnPayloadHit(
	UPrimitiveComponent* HitComp,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector NormalImpulse,
	const FHitResult& Hit
)
{
	if (!bIsArmed || bHasExploded)
		return;

	if (!OtherActor || OtherActor == this)
		return;

	UDamagableComponent* DamageComp = OtherActor->FindComponentByClass<UDamagableComponent>();
	if (!DamageComp) return;

#if WITH_EDITOR
	if (bShowDebug)
	{
		UE_LOG(LogDynamicPayload, Warning, TEXT("Payload impact detected"));
	}
#endif

	Explode(Hit.ImpactPoint);
}
