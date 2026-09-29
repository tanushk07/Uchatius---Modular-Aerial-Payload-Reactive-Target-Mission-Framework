// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MovableTargetComponent.generated.h"

class APatrolAreaVolume;
class UTargetBehaviorComponent;
class USplineComponent;

UENUM(BlueprintType)
enum class ETargetMovementMode : uint8
{
	None,
	PatrolSpline,
	PatrolArea,
	ConvoyFollow
};

UENUM(BlueprintType)
enum class EPatrolAreaState : uint8
{
	Idle,
	Moving,
	Waiting
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DYNAMICPAYLOADSYSTEM_API UMovableTargetComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UMovableTargetComponent();

protected:
	virtual void BeginPlay() override;

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/* ========================================================================
	   MOVEMENT CONFIGURATION
	   ======================================================================== */

	UPROPERTY(EditAnywhere, Category = "Movement")
	float BaseSpeed = 300.f;

	UPROPERTY(EditAnywhere, Category = "Movement")
	ETargetMovementMode MovementMode = ETargetMovementMode::None;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float YawTurnSpeed = 120.f;

	/* ========================================================================
	   SPLINE PATROL
	   ======================================================================== */

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Movement|Spline")
	AActor* PatrolSplineActor = nullptr;

	/* ========================================================================
	   PATROL AREA (Realistic Turning)
	   ======================================================================== */

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Movement|PatrolArea")
	APatrolAreaVolume* PatrolArea = nullptr;

	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea")
	float ArrivalTolerance = 50.f;

	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea")
	float WaitTimeAtPoint = 2.f;

	/** Minimum turning radius in cm. Typical values: 500 for a car, 800 for a truck, 1200 for a semi. */
	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea",
		meta = (ClampMin = "100.0"))
	float MinTurningRadius = 800.f;

	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea",
		meta = (ClampMin = "10.0"))
	float MinPatrolSpeed = 40.f;

	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea",
		meta = (ClampMin = "0.0"))
	float MinWaypointDistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Movement|PatrolArea",
		meta = (ClampMin = "1.0"))
	float OrbitTimeoutDuration = 8.f;

	/* ========================================================================
	   CONVOY FOLLOWING (Distance-Maintaining)
	   ======================================================================== */

	UPROPERTY(EditInstanceOnly, Category = "Movement|Convoy")
	AActor* ConvoyLeader = nullptr;

	UPROPERTY(EditAnywhere, Category = "Movement|Convoy")
	float FollowDistance = 500.f;

	UPROPERTY(EditAnywhere, Category = "Movement|Convoy",
		meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float ConvoyFollowStiffness = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Convoy",
		meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float ConvoyMaxSpeedMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Movement|Convoy",
		meta = (ClampMin = "50.0"))
	float MinConvoyFollowDistance = 200.f;

	UPROPERTY(EditAnywhere, Category = "Movement|Convoy")
	float ConvoyForwardTraceRange = 600.f;

	/* ========================================================================
	   AUTO GEOMETRY  (mesh-relative sizing)

	   Every distance below is derived from the owner's mesh bounds, so a
	   convoy behaves the same whether it is built from hatchbacks or tanks.
	   Ratios are expressed in "vehicle lengths"; the resolved centimetre
	   resolved values are shown read-only below.
	   Tick bUseAbsoluteGeometry to fall back to typing raw centimetres.
	   ======================================================================== */

	UPROPERTY(EditAnywhere, Category = "Geometry")
	bool bUseAbsoluteGeometry = false;

	UPROPERTY(EditAnywhere, Category = "Geometry",
		meta = (EditCondition = "!bUseAbsoluteGeometry", ClampMin = "0.5"))
	float FollowGapInLengths = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Geometry",
		meta = (EditCondition = "!bUseAbsoluteGeometry", ClampMin = "0.3"))
	float TurningRadiusInLengths = 1.4f;

	UPROPERTY(EditAnywhere, Category = "Geometry",
		meta = (EditCondition = "!bUseAbsoluteGeometry", ClampMin = "0.05"))
	float ArrivalToleranceInLengths = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Geometry",
		meta = (EditCondition = "!bUseAbsoluteGeometry", ClampMin = "0.5"))
	float MinWaypointInLengths = 2.5f;

	/** Where the front/rear ground traces go, as a fraction of vehicle length.
	 *  0.32 is about a real wheelbase. Also used for pitch on slopes. */
	UPROPERTY(EditAnywhere, Category = "Geometry",
		meta = (EditCondition = "!bUseAbsoluteGeometry", ClampMin = "0.1", ClampMax = "0.5"))
	float AxleOffsetInLengths = 0.32f;

	/* --- resolved values: read-only, refreshed on spawn and on mesh swap --- */

	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float ResolvedVehicleLength = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float ResolvedFollowDistance = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float ResolvedTurningRadius = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float ResolvedArrivalTolerance = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float ResolvedAxleOffset = 0.f;

	/* ========================================================================
	   CONVOY CONTROL
	   ======================================================================== */

	/** Damping for convoy gap control. A value near 0.6 typically reduces
	 *  oscillation without overshoot. */
	UPROPERTY(EditAnywhere, Category = "Movement|Convoy",
		meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float ConvoyDamping = 0.6f;

	/** Maximum reverse speed used by a follower to increase its gap from the leader. */
	UPROPERTY(EditAnywhere, Category = "Movement|Convoy", meta = (ClampMin = "0.0"))
	float ConvoyRecoverySpeed = 120.f;

	/* ========================================================================
	   DRIVE FEEL

	   A vehicle ramps its speed instead of snapping to it, and judges how fast
	   it may approach something by how hard it can brake. Both settings are
	   times and ratios, so they are independent of vehicle size AND of
	   BaseSpeed - a tank and a hatchback using the same numbers both feel
	   driven rather than dragged along a spline.
	   ======================================================================== */

	/** Seconds to get from standstill to BaseSpeed. */
	UPROPERTY(EditAnywhere, Category = "Movement|Drive Feel", meta = (ClampMin = "0.05"))
	float SpeedRampTime = 2.5f;

	/** Braking acceleration multiplier; higher values apply stronger braking. */
	UPROPERTY(EditAnywhere, Category = "Movement|Drive Feel",
		meta = (ClampMin = "1.0", ClampMax = "5.0"))
	float BrakeRampMultiplier = 1.8f;

	/** Steering look-ahead time in seconds. Higher speeds increase look-ahead
	 *  distance for smoother cornering. */
	UPROPERTY(EditAnywhere, Category = "Movement|Drive Feel", meta = (ClampMin = "0.1"))
	float LookaheadSeconds = 0.8f;

	/** Current speed, interpolated toward the mode's target speed. */
	UPROPERTY(VisibleAnywhere, Category = "Geometry|Resolved")
	float CurrentSpeed = 0.f;

	/* ========================================================================
	   GEOMETRY ACCESSORS
	   Always read sizing through these - never the raw fields, which are only
	   meaningful when bUseAbsoluteGeometry is set.
	   ======================================================================== */

	UFUNCTION(BlueprintPure, Category = "Geometry")
	float GetVehicleLength() const { return FMath::Max(ResolvedVehicleLength, 100.f); }

	float GetFollowDistance() const;
	float GetTurningRadius() const;
	float GetArrivalTolerance() const;
	float GetMinWaypointDistance() const;
	float GetAxleOffset() const;
	float GetMinConvoyFollowDistance() const;
	float GetConvoyForwardTraceRange() const;

	/** Acceleration in cm/s^2 (from BaseSpeed and SpeedRampTime). */
	float GetAccelRate() const;

	/** Braking in cm/s^2. */
	float GetBrakeRate() const;

	/** Steering look-ahead: at least one car length, more when moving. */
	float GetLookaheadDistance() const;

	/** Maximum speed from which the vehicle can stop within Distance,
	 *  calculated as sqrt(2 * braking acceleration * distance). */
	float GetApproachSpeedLimit(float Distance) const;

	/** Restore the starting transform and clear movement state, including spline
	 *  progress, convoy gap, blend state, and ground height. */
	UFUNCTION(BlueprintCallable, Category = "Movement")
	void ResetToStart();

	/** Recompute all the Resolved* values from the mesh bounds.
	 *  Runs on mesh swap too, so a damaged mesh gets re-measured. */
	void RefreshDerivedGeometry();

	/* ========================================================================
	   GROUND ALIGNMENT
	   ======================================================================== */

	UPROPERTY(EditAnywhere, Category = "Grounding")
	bool bUseGroundAlignment = true;

	UPROPERTY(EditAnywhere, Category = "Grounding")
	float GroundOffset = 0.f;

	UPROPERTY(EditAnywhere, Category = "Grounding")
	float GroundAlignInterpSpeed = 6.f;

	UPROPERTY(EditAnywhere, Category = "Grounding")
	float FrontTraceOffset = 200.f;

	UPROPERTY(EditAnywhere, Category = "Grounding")
	float RearTraceOffset = 200.f;

	UPROPERTY(EditAnywhere, Category = "Grounding")
	float TraceHeight = 1000.f;

	/* ========================================================================
	   DEBUG
	   ======================================================================== */

	/** Debug drawing for movement/convoy/ground (editor only). */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = false;

	/* ========================================================================
	   PUBLIC API
	   ======================================================================== */

	UFUNCTION(BlueprintCallable, Category = "Movement")
	float GetDistanceAlongSpline() const { return DistanceAlongSpline; }

	UFUNCTION(BlueprintCallable, Category = "Movement")
	USplineComponent* ResolveActiveSpline() const;

	/** Re-read the mesh bounds. Call after swapping the mesh. */
	void CacheOwnerBounds();

private:
	/* ========================================================================
	   INTERNAL STATE
	   ======================================================================== */

	UPROPERTY()
	UTargetBehaviorComponent* BehaviorComponent;

	float SpeedMultiplier = 1.0f;
	bool bCanMove = true;

	// UPROPERTY allows the reference to be cleared if the spline is destroyed,
	// for example when its level is unloaded.
	UPROPERTY()
	USplineComponent* PatrolSpline = nullptr;
	float DistanceAlongSpline = 0.f;
	bool bOnSpline = false;

	EPatrolAreaState PatrolState = EPatrolAreaState::Idle;
	FVector CurrentDestination = FVector::ZeroVector;
	float WaitTimeRemaining = 0.f;
	float OrbitTimer = 0.f;
	float LastDistanceToTarget = 0.f;

	float ConvoyDistanceAlongSpline = 0.f;
	bool bConvoyInitialized = false;
	bool bConvoyOnSpline = false;
	float PrevRootSplineDistance = 0.f;
	float DesiredMovementYaw = 0.f;

	float CachedLateralExtent = 100.f;
	float CachedLongitudinalExtent = 200.f;

	/** Actor transform captured at BeginPlay and used by ResetToStart(). */
	FTransform StartTransform;

	// convoy PD state
	float PrevGapError = 0.f;
	bool bGapErrorInitialized = false;

	// capture blending: 1 = fully on the path, <1 = easing on
	float SplineBlendAlpha = 1.f;
	float ConvoyBlendAlpha = 1.f;
	float SmoothedZ = 0.f;
	bool bZInitialized = false;

	/* ========================================================================
	   MOVEMENT IMPLEMENTATION
	   ======================================================================== */

	void ArcSteerToward(float DeltaTime, const FVector& TargetLocation, float Speed, bool bApplyTurnPenalty = true);

	void InitializeSplineMovement();
	void MoveAlongSpline(float DeltaTime);

	void MoveInPatrolArea(float DeltaTime);
	void UpdatePatrolMovement(float DeltaTime, float Speed);
	float CalculateTurnAngle(const FVector& CurrentDir, const FVector& TargetDir) const;

	void MoveInConvoy(float DeltaTime);
	bool FindRootSpline(USplineComponent*& OutSpline, float& OutDistance);
	float CalculateCumulativeFollowDistance() const;
	void InitializeConvoyPosition(USplineComponent* Spline, float DesiredDistance);
	void UpdateConvoyMovement(float DeltaTime, USplineComponent* Spline,
	float DesiredDistance, float SplineLength, float LeaderSplineSpeed);
	void DrawConvoyDebug(USplineComponent* Spline, float DesiredDistance,
		const FVector& CurrentLoc, const FVector& Tangent, float Gap);
	float GetForwardClearance();

	/** Other actors with a MovableTargetComponent, refreshed at the configured
	 *  interval to avoid scanning the world every frame. New followers are
	 *  detected at the next refresh. */
	TArray<TWeakObjectPtr<AActor>> CachedConvoyFollowers;
	float CachedConvoyFollowersValidUntil = -1.f;
	static constexpr float ConvoyFollowerCacheRefreshSeconds = 2.f;

	void AlignToGround(float DeltaTime);
	bool TraceGround(const FVector& Origin, const FVector& Offset, FHitResult& OutHit);
	void ApplyGroundAlignment(const FHitResult& FrontHit, const FHitResult& RearHit,
		const FHitResult& LeftHit, const FHitResult& RightHit, float DeltaTime);

	UFUNCTION()
	void HandleBehaviorUpdated(float NewSpeedMultiplier, bool bNewCanMove);

	void ValidateConvoySetup();
	void AutoChainDuplicateFollowers();
};
