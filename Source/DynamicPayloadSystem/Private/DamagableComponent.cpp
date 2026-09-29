// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "DamagableComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DynamicPayloadSystemModule.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "TimerManager.h"

UDamagableComponent::UDamagableComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDamagableComponent::BeginPlay()
{
	Super::BeginPlay();

	// ClampMin only works in the editor - a BP can still set MaxHealth to 0
	// at runtime. Bump it back up so the state logic keeps working.
	if (MaxHealth <= 0.f)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Warning,
				TEXT("[Damagable] %s: MaxHealth was %.2f; clamped to 1.0 to avoid softlock"),
				*GetOwner()->GetName(), MaxHealth);
		}
#endif
		MaxHealth = 1.f;
	}

	CurrentHealth = MaxHealth;

	AActor* Owner = GetOwner();
	if (!Owner) return;
	Owner->OnTakeAnyDamage.AddDynamic(
		this,
		&UDamagableComponent::OnTakeAnyDamage
	);
}

FString UDamagableComponent::GetReadableName() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
		return TEXT("INVALID");

	if (!DamageableTag.IsNone())
		return DamageableTag.ToString();

	return Owner->GetName();
}

void UDamagableComponent::OnTakeAnyDamage(
	AActor* DamagedActor,
	float Damage,
	const UDamageType* DamageTypeRef,
	AController* InstigatedBy,
	AActor* DamageCauser
)
{
	if (Damage <= 0.f)
		return;

	if (StructuralState == EStructuralState::Destroyed)
		return;

	CurrentHealth = FMath::Max(CurrentHealth - Damage, 0.f);
	OnDamageTaken.Broadcast(Damage, CurrentHealth);
	UpdateStructuralState();
}

void UDamagableComponent::SetHighlightEnabled(bool bEnabled)
{
	AActor* Owner = GetOwner();
	if (!Owner)
		return;

	TArray<UPrimitiveComponent*> PrimComponents;
	Owner->GetComponents<UPrimitiveComponent>(PrimComponents);

	for (UPrimitiveComponent* Comp : PrimComponents)
	{
		if (!Comp)
			continue;

		Comp->SetRenderCustomDepth(bEnabled);
		Comp->SetCustomDepthStencilValue(1);
	}
}

void UDamagableComponent::CancelPendingDespawn()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DespawnTimerHandle);
	}
}

void UDamagableComponent::SoftDespawn()
{
	AActor* Owner = GetOwner();
	if (!Owner || StructuralState != EStructuralState::Destroyed)
		return;

	Owner->SetActorHiddenInGame(true);
	Owner->SetActorEnableCollision(false);
	bSoftDespawned = true;
}

void UDamagableComponent::Revive()
{
	// Cancel pending despawn before restoring the target to prevent the timer
	// from hiding it again after revival.
	CancelPendingDespawn();

	if (AActor* Owner = GetOwner())
	{
		// In case a Blueprint set a lifespan on it too.
		Owner->SetLifeSpan(0.f);

		if (bSoftDespawned)
		{
			Owner->SetActorHiddenInGame(false);
			Owner->SetActorEnableCollision(true);
			bSoftDespawned = false;
		}
	}

	CurrentHealth = MaxHealth;

	const EStructuralState Previous = StructuralState;
	StructuralState = EStructuralState::Intact;

	// Only fire on a real change. Listeners swap meshes/collision on this, and
	// doing that for every untouched target on each retry is pointless.
	if (Previous != StructuralState)
	{
		OnStructuralStateChanged.Broadcast(StructuralState);
	}
}

void UDamagableComponent::UpdateStructuralState()
{
	// BeginPlay fixes a zero MaxHealth, but BP can still set it to 0 later.
	if (MaxHealth <= 0.f)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Warning,
				TEXT("[Damagable] %s: MaxHealth was set to <= 0 at runtime; clamped to 1.0"),
				GetOwner() ? *GetOwner()->GetName() : TEXT("(no owner)"));
		}
#endif
		MaxHealth = 1.f;
	}

	const float HealthRatio = CurrentHealth / MaxHealth;

	EStructuralState NewState;

	if (HealthRatio <= DestroyedThreshold)
	{
		NewState = EStructuralState::Destroyed;
	}
	else if (HealthRatio <= DamagedThreshold)
	{
		NewState = EStructuralState::Damaged;
	}
	else
	{
		NewState = EStructuralState::Intact;
	}

	if (NewState != StructuralState)
	{
		StructuralState = NewState;

		// Hide the wreck after DestroyDelay while keeping the actor alive so a
		// reset can restore its level-instance configuration. A zero delay leaves
		// the wreck visible. Arm the timer before broadcasting so listeners can
		// cancel it.
		if (NewState == EStructuralState::Destroyed && DestroyDelay > 0.f)
		{
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					DespawnTimerHandle, this, &UDamagableComponent::SoftDespawn,
					DestroyDelay, false);
			}
		}

		OnStructuralStateChanged.Broadcast(StructuralState);
	}

#if WITH_EDITOR
	if (bShowDebug)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			UE_LOG(LogDynamicPayload, Display,
				TEXT("[%s] StructuralState -> %s (Health: %.1f / %.1f)"),
				*Owner->GetName(),
				*UEnum::GetValueAsString(StructuralState),
				CurrentHealth,
				MaxHealth
			);
		}
	}
#endif
}
