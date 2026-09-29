// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "Explosive.h"
#include "DamagableComponent.h"
#include "DynamicPayloadSystemModule.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "Camera/CameraShakeBase.h"
#include "Kismet/GameplayStatics.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#endif
#include "Engine/World.h"
#include "Engine/OverlapResult.h"

AExplosive::AExplosive()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	ExplosionVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("ExplosionFX"));
	ExplosionVFX->SetupAttachment(RootComponent);
	// Niagara components auto-activate by default. Keep the effect inactive
	// until detonation.
	ExplosionVFX->SetAutoActivate(false);
}

void AExplosive::BeginPlay()
{
	Super::BeginPlay();

	if (!bExplodeOnBeginPlay)
	{
		return;
	}

#if WITH_EDITOR
	if (bShowDebug)
	{
		DrawExplosionDebug();
	}
#endif
	Explode();
}

void AExplosive::Explode()
{
	// Prevent duplicate detonations from BeginPlay, Blueprint, or damage events.
	if (bHasDetonated)
	{
		return;
	}
	bHasDetonated = true;

	const FVector ExplosionLocation = GetActorLocation();
#if WITH_EDITOR
	if (bShowDebug)
	{
		UE_LOG(LogDynamicPayload, Warning, TEXT("=== EXPLOSION TRIGGERED at %s ==="), *ExplosionLocation.ToString());
	}
#endif

	TArray<FOverlapResult> Overlaps = ScanForTargets();

	if (Overlaps.Num() > 0)
	{
		ApplyPhysicsImpulses(Overlaps);
		ApplyDamageToActors(Overlaps);
	}

	PlayExplosionEffects();

	// The sound is attached to us and dies with us, so stick around until it's
	// done - otherwise a short DestroyDelay would cut the bang off.
	float LifeSpan = DestroyDelay_s;
	if (ExplosionSound)
	{
		// Looping sounds report a huge duration; don't hang around for those.
		const float SoundLength = ExplosionSound->GetDuration();
		if (SoundLength < 60.f)
		{
			LifeSpan = FMath::Max(LifeSpan, SoundLength + 0.1f);
		}
	}
	SetLifeSpan(LifeSpan);
}

TArray<FOverlapResult> AExplosive::ScanForTargets() const
{
	TArray<FOverlapResult> LocalOverlaps;
	UWorld* World = GetWorld();
	if (!World) return LocalOverlaps;

	FCollisionShape Sphere =
		FCollisionShape::MakeSphere(MetersToUU(FMath::Max(ExplosionRadius_m, OuterRadius_m)));

	FCollisionObjectQueryParams Params;
	Params.AddObjectTypesToQuery(ECC_PhysicsBody);
	Params.AddObjectTypesToQuery(ECC_Pawn);
	Params.AddObjectTypesToQuery(ECC_WorldDynamic);
	// Include WorldStatic so bunkers/cover with a DamagableComponent get hit too.
	Params.AddObjectTypesToQuery(ECC_WorldStatic);

	World->OverlapMultiByObjectType(
		LocalOverlaps,
		GetActorLocation(),
		FQuat::Identity,
		Params,
		Sphere
	);

	return LocalOverlaps;
}

void AExplosive::ApplyPhysicsImpulses(const TArray<FOverlapResult>& Overlaps)
{
	for (const FOverlapResult& Result : Overlaps)
	{
		UPrimitiveComponent* Comp = Result.GetComponent();
		if (Comp && Comp->IsSimulatingPhysics())
		{
			Comp->AddRadialImpulse(
				GetActorLocation(),
				MetersToUU(ExplosionRadius_m),
				ExplosionImpulse_Ns,
				Falloff,
				true
			);
		}
	}
}

void AExplosive::ApplyDamageToActors(const TArray<FOverlapResult>& Overlaps)
{
	TSet<AActor*> ProcessedActors;

	for (const FOverlapResult& Result : Overlaps)
	{
		AActor* HitActor = Result.GetActor();
		if (!HitActor || HitActor == this || ProcessedActors.Contains(HitActor))
			continue;

		UDamagableComponent* DamageComp =
			HitActor->FindComponentByClass<UDamagableComponent>();
		if (!DamageComp) continue;

		const float FinalDamage =
			CalculateFinalDamage(HitActor, GetActorLocation());

		if (FinalDamage > MinDamageToApply)
		{
			// Pass the instigator controller for kill attribution and scoring.
			UGameplayStatics::ApplyDamage(
				HitActor,
				FinalDamage,
				GetInstigatorController(),
				this,
				DamageType
			);
		}

		ProcessedActors.Add(HitActor);
	}
}

float AExplosive::CalculateFinalDamage(AActor* Victim, const FVector& ExplosionPos)
{
	FVector ClosestPoint = Victim->GetActorLocation();
	UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(
		Victim->GetRootComponent()
	);
	if (!PrimComp)
	{
		PrimComp = Victim->FindComponentByClass<UPrimitiveComponent>();
	}
	if (PrimComp)
	{
		PrimComp->GetClosestPointOnCollision(ExplosionPos, ClosestPoint);
	}

	const float Distance = FVector::Dist(ExplosionPos, ClosestPoint);

	const float InnerUU = MetersToUU(InnerRadius_m);
	const float OuterUU = MetersToUU(OuterRadius_m);

	float DistanceFactor = 1.f;
	if (Distance > InnerUU)
	{
		if (OuterUU <= InnerUU)
		{
			DistanceFactor = 0.f;
		}
		else
		{
			DistanceFactor = 1.f - FMath::Clamp(
				(Distance - InnerUU) / (OuterUU - InnerUU),
				0.f, 1.f
			);
		}
	}

	const FVector ToTarget = ClosestPoint - ExplosionPos;
	const float DirectionalFactor = ComputeDirectionalFactor(ToTarget);
	// Ignore the victim so the trace does not hit its own collision and reduce
	// clear-line-of-sight damage to 0.3x.
	const float VisibilityFactor = HasLineOfSight(ClosestPoint, Victim) ? 1.f : 0.3f;

	const float BlastDamage =
		MaxDamage * DistanceFactor * VisibilityFactor;

	const float ShrapnelDamage =
		MaxDamage * 0.4f * DirectionalFactor * VisibilityFactor;

	return BlastDamage + ShrapnelDamage;
}

void AExplosive::PlayExplosionEffects()
{
	// Attached, not fire-and-forget. A loose one-shot can't be stopped, so if
	// the game paused mid-bang and you hit Retry, it just finished playing
	// after the unpause. Attached to us, it goes away when we do.
	if (ExplosionSound)
	{
		UGameplayStatics::SpawnSoundAttached(
			ExplosionSound,
			GetRootComponent(),
			NAME_None,
			FVector::ZeroVector,
			EAttachLocation::KeepRelativeOffset,
			/*bStopWhenAttachedToDestroyed=*/ true
		);
	}
	if (ExplosionVFX && ExplosionVFX->GetAsset())
	{
		ExplosionVFX->Activate(true);
	}

	// Play the assigned camera shake, if any.
	if (CameraShake)
	{
		UGameplayStatics::PlayWorldCameraShake(
			this,
			CameraShake,
			GetActorLocation(),
			0.f,
			MaxShakeRadius
		);
	}

	// Notify listeners of the explosion.
	OnExplosionTriggered.Broadcast(GetActorLocation(), ExplosionRadius_m);
}

float AExplosive::ComputeDirectionalFactor(const FVector& ToTarget) const
{
	const FVector Forward = GetActorForwardVector();
	const FVector Dir = ToTarget.GetSafeNormal();
	const float Dot = FVector::DotProduct(Forward, Dir);
	return FMath::Lerp(0.85f, 1.0f, (Dot + 1.f) * 0.5f);
}

bool AExplosive::HasLineOfSight(const FVector& TargetPoint, const AActor* IgnoreActor) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		// Treat the target as visible when no world is available, such as during teardown.
		return true;
	}

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	if (IgnoreActor)
	{
		Params.AddIgnoredActor(IgnoreActor);
	}

	const bool bBlocked = World->LineTraceSingleByChannel(
		Hit,
		GetActorLocation(),
		TargetPoint,
		ECC_Visibility,
		Params
	);

	return !bBlocked;
}

void AExplosive::DrawExplosionDebug() const
{
#if WITH_EDITOR
	if (!bShowDebug) return;

	UWorld* World = GetWorld();
	if (!World) return;

	const FVector Center = GetActorLocation();

	DrawDebugSphere(World, Center, MetersToUU(ExplosionRadius_m), 32, FColor::Red, false, 2.0f);
	DrawDebugSphere(World, Center, MetersToUU(InnerRadius_m), 32, FColor::Green, false, 2.0f);
	DrawDebugSphere(World, Center, MetersToUU(OuterRadius_m), 32, FColor::Yellow, false, 2.0f);
#endif
}
