// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EStructuralState.h"
#include "TargetBehaviorComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DYNAMICPAYLOADSYSTEM_API UTargetBehaviorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTargetBehaviorComponent();

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleStructuralStateChanged(EStructuralState NewState);

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Behavior")
	float SpeedMultiplier = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Behavior")
	bool bCanMove = true;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
		FOnMovementCapabilityChanged,
		float, SpeedMultiplier,
		bool, bCanMove
	);

	UPROPERTY(BlueprintAssignable)
	FOnMovementCapabilityChanged OnMovementCapabilityChanged;

	/** Log behavior changes in editor builds. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = false;
};
