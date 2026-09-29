// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EStructuralState.h"
#include "TargetActor.generated.h"

class UDamagableComponent;
class UTargetBehaviorComponent;
class UMovableTargetComponent;

UCLASS()
class DYNAMICPAYLOADSYSTEM_API ATargetActor : public AActor
{
	GENERATED_BODY()

public:
	ATargetActor();

protected:
	virtual void BeginPlay() override;

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USceneComponent* Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UDamagableComponent* DamagableComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UTargetBehaviorComponent* BehaviorComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UMovableTargetComponent* MovementComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USceneComponent* GroundFrame;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage|Visuals")
	UStaticMesh* IntactMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage|Visuals")
	UStaticMesh* DamagedMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage|Visuals")
	UStaticMesh* DestroyedMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission")
	bool bIsMissionTarget = true;

	/** Initial transform recorded at BeginPlay and used by mission resets. */
	UFUNCTION(BlueprintPure, Category = "Mission")
	FTransform GetInitialTransform() const { return InitialTransform; }

protected:
	UFUNCTION()
	void HandleStructuralStateChanged(EStructuralState NewState);

	FTransform InitialTransform;

	/** Saved at BeginPlay so revival restores the actor's configured settings. */
	TEnumAsByte<ECollisionEnabled::Type> InitialCollisionEnabled = ECollisionEnabled::QueryAndPhysics;
	bool bInitialActorTickEnabled = false;
	bool bInitialMovementTickEnabled = true;
};
