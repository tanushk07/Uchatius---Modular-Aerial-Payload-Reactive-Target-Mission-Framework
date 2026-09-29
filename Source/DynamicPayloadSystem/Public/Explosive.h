// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Chaos/ChaosEngineInterface.h"
#include "Explosive.generated.h"

class UNiagaraComponent;
class USoundBase;
class UCameraShakeBase;
class UDamageType;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnExplosionTriggered,
	FVector, ExplosionLocation,
	float, ExplosionRadius
);

UCLASS()
class DYNAMICPAYLOADSYSTEM_API AExplosive : public AActor
{
	GENERATED_BODY()

public:
	AExplosive();

protected:
	virtual void BeginPlay() override;

	/** Prevent duplicate detonation; Explode() is BlueprintCallable. */
	bool bHasDetonated = false;

	/* ================= Root & FX ================= */

	UPROPERTY(VisibleAnywhere, Category = "Explosion")
	USceneComponent* Root;

	UPROPERTY(VisibleAnywhere, Category = "Explosion")
	UNiagaraComponent* ExplosionVFX;

	UPROPERTY(EditAnywhere, Category = "Explosion")
	USoundBase* ExplosionSound;

	/* ================= Core Logic ================= */

	UFUNCTION(BlueprintCallable, Category = "Explosion")
	void Explode();

	TArray<FOverlapResult> ScanForTargets() const;
	void ApplyPhysicsImpulses(const TArray<FOverlapResult>& Overlaps);
	void ApplyDamageToActors(const TArray<FOverlapResult>& Overlaps);
	void PlayExplosionEffects();
	void DrawExplosionDebug() const;

	float CalculateFinalDamage(AActor* Victim, const FVector& ExplosionPos);
	float ComputeDirectionalFactor(const FVector& ToTarget) const;
	/** Trace to TargetPoint, ignoring this actor and an optional additional actor. */
	bool HasLineOfSight(const FVector& TargetPoint, const AActor* IgnoreActor = nullptr) const;

	/* ================= Unit Conversion ================= */

	FORCEINLINE float MetersToUU(float Meters) const
	{
		return Meters * 100.f;
	}

public:

	/** Has this one already gone off? */
	bool HasDetonated() const { return bHasDetonated; }

	/* ================= Explosion Parameters (SI) ================= */

	UPROPERTY(EditAnywhere, Category = "Explosion|SI")
	float ExplosionRadius_m = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Explosion|SI")
	float ExplosionImpulse_Ns = 3000.0f;

	UPROPERTY(EditAnywhere, Category = "Explosion")
	TEnumAsByte<ERadialImpulseFalloff> Falloff = ERadialImpulseFalloff::RIF_Linear;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Explosion|SI",
		meta = (ClampMin = "0.1",
			ToolTip = "Seconds the actor remains after detonation. Set this longer than the Niagara effect duration."))
	float DestroyDelay_s = 3.0f;

	/** Detonate on BeginPlay. Disabled by default for level-placed explosives;
	 *  APayload enables it for spawned instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Explosion")
	bool bExplodeOnBeginPlay = false;

	/* ================= Damage Parameters (SI) ================= */

	UPROPERTY(EditAnywhere, Category = "Damage|SI")
	float MaxDamage = 10.0f;

	/** Damage below this threshold is not applied. */
	UPROPERTY(EditAnywhere, Category = "Damage|SI",
		meta = (ClampMin = "0.0"))
	float MinDamageToApply = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Damage|SI")
	float InnerRadius_m = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Damage|SI")
	float OuterRadius_m = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Damage")
	TSubclassOf<UDamageType> DamageType;

	/* ================= Camera Shake ================= */

	/** Optional camera shake, played automatically. */
	UPROPERTY(EditAnywhere, Category = "Explosion|Camera")
	TSubclassOf<UCameraShakeBase> CameraShake;

	UPROPERTY(EditAnywhere, Category = "Explosion|Camera")
	float MaxShakeRadius = 2000.f;

	/* ================= Debug ================= */

	/** Debug draw for the blast radii (editor only). */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = false;

	/* ================= Events ================= */

	/** Broadcast when detonation is triggered. */
	UPROPERTY(BlueprintAssignable, Category = "Explosion")
	FOnExplosionTriggered OnExplosionTriggered;
};
