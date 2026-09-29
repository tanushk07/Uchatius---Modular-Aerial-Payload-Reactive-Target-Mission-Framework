// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EStructuralState.h"
#include "DamagableComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnStructuralStateChanged,
	EStructuralState,
	NewState
);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnDamageTaken,
	float, DamageAmount,
	float, RemainingHealth
);

UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class DYNAMICPAYLOADSYSTEM_API UDamagableComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDamagableComponent();

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnTakeAnyDamage(
		AActor* DamagedActor,
		float Damage,
		const class UDamageType* DamageType,
		class AController* InstigatedBy,
		AActor* DamageCauser
	);

	void UpdateStructuralState();

	/** Hide the actor after DestroyDelay while retaining its level-instance
	 *  configuration for mission resets. */
	void SoftDespawn();

	FTimerHandle DespawnTimerHandle;
	bool bSoftDespawned = false;

public:
	UPROPERTY(VisibleAnywhere, Category = "Health")
	float CurrentHealth;

	UPROPERTY(EditAnywhere, Category = "Health",
		meta = (ClampMin = "0.01",
			ToolTip = "Maximum health. Must be > 0 — at 0 the structural-state update short-circuits and the actor can never transition to Destroyed."))
	float MaxHealth = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity")
	FName DamageableTag;

	FString GetReadableName() const;
	void SetHighlightEnabled(bool bEnabled);

	/** Restore full health and the Intact state. Cancels pending despawn and
	 *  broadcasts only when the structural state changes. */
	UFUNCTION(BlueprintCallable, Category = "Damage")
	void Revive();

	/** Cancel the pending despawn timer. */
	void CancelPendingDespawn();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Damage")
	EStructuralState StructuralState = EStructuralState::Intact;

	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	float DamagedThreshold = 0.7f;

	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	float DestroyedThreshold = 0.25f;

	/** Seconds after destruction before the wreck is hidden and collision is disabled.
	 *  Zero disables automatic hiding. The actor remains available for resets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float DestroyDelay = 2.f;

	UPROPERTY(BlueprintAssignable, Category = "Damage")
	FOnStructuralStateChanged OnStructuralStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Damage")
	FOnDamageTaken OnDamageTaken;

	/** Enable damage and structural-state diagnostics in editor builds. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = false;
};
