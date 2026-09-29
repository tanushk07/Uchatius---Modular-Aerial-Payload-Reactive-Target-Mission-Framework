// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "MovableTargetComponent.h"
#include "TargetBehaviorComponent.h"
#include "DynamicPayloadSystemModule.h"
#include "PatrolAreaVolume.h"
#include "Kismet/KismetMathLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "TargetActor.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#endif

UMovableTargetComponent::UMovableTargetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UMovableTargetComponent::BeginPlay()
{
	Super::BeginPlay();

	// Capture the reset transform before the early return so stationary vehicles
	// retain their starting position.
	if (GetOwner())
	{
		StartTransform = GetOwner()->GetActorTransform();
	}

	AActor* Owner = GetOwner();
	if (Owner && Owner->GetRootComponent() &&
		Owner->GetRootComponent()->Mobility != EComponentMobility::Movable)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Warning,
				TEXT("[Movement] %s has non-Movable root — disabling MovableTargetComponent"),
				*Owner->GetName());
		}
#endif
		SetComponentTickEnabled(false);
		return;
	}

	CacheOwnerBounds();
	SmoothedZ = GetOwner()->GetActorLocation().Z;
	bZInitialized = false;
	DesiredMovementYaw = GetOwner()->GetActorRotation().Yaw;

	BehaviorComponent = GetOwner()->FindComponentByClass<UTargetBehaviorComponent>();
	if (BehaviorComponent)
	{
		SpeedMultiplier = BehaviorComponent->SpeedMultiplier;
		bCanMove = BehaviorComponent->bCanMove;
		BehaviorComponent->OnMovementCapabilityChanged.AddDynamic(
			this, &UMovableTargetComponent::HandleBehaviorUpdated
		);
	}

	if (PatrolSplineActor)
	{
		PatrolSpline = PatrolSplineActor->FindComponentByClass<USplineComponent>();
		if (PatrolSpline)
		{
			InitializeSplineMovement();
		}
	}

	if (MovementMode == ETargetMovementMode::ConvoyFollow)
	{
		ValidateConvoySetup();
		AutoChainDuplicateFollowers();
	}
}

void UMovableTargetComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bCanMove)
		return;

	switch (MovementMode)
	{
	case ETargetMovementMode::PatrolSpline:
		MoveAlongSpline(DeltaTime);
		break;
	case ETargetMovementMode::PatrolArea:
		MoveInPatrolArea(DeltaTime);
		break;
	case ETargetMovementMode::ConvoyFollow:
		MoveInConvoy(DeltaTime);
		break;
	case ETargetMovementMode::None:
	default:
		return;
	}

	if (bUseGroundAlignment)
	{
		AlignToGround(DeltaTime);
	}
}

/* ============================================================================
   SHARED ARC-STEERING
   ============================================================================ */

void UMovableTargetComponent::ArcSteerToward(float DeltaTime, const FVector& TargetLocation, float Speed, bool bApplyTurnPenalty)
{
	AActor* OwnerActor = GetOwner();
	FVector CurrentLocation = OwnerActor->GetActorLocation();
	FVector ToTarget = TargetLocation - CurrentLocation;
	ToTarget.Z = 0.f;
	float Distance = ToTarget.Size2D();

	if (Distance < 0.1f)
		return;

	FVector DesiredDirection = ToTarget.GetSafeNormal2D();
	FVector CurrentForward = OwnerActor->GetActorForwardVector();
	CurrentForward.Z = 0.f;
	CurrentForward.Normalize();

	float AngleToTarget = CalculateTurnAngle(CurrentForward, DesiredDirection);
	float TurnDirection = FVector::CrossProduct(CurrentForward, DesiredDirection).Z > 0.f ? 1.f : -1.f;

	float EffectiveSpeed;
	float TurningRadius;

	if (bApplyTurnPenalty)
	{
		float SpeedFactor = FMath::Clamp(1.f - (AngleToTarget / 180.f), 0.3f, 1.f);
		EffectiveSpeed = FMath::Max(Speed * SpeedFactor, MinPatrolSpeed);
		TurningRadius = GetTurningRadius();
	}
	else
	{
		EffectiveSpeed = Speed;
		float Omega = FMath::DegreesToRadians(YawTurnSpeed);
		TurningRadius = EffectiveSpeed / FMath::Max(Omega, 0.001f);
	}

	float MoveDistance = FMath::Min(EffectiveSpeed * DeltaTime, Distance);
	float Omega = EffectiveSpeed / FMath::Max(TurningRadius, 1.f);
	float MaxTurnThisFrame = Omega * DeltaTime;
	float DesiredTurnRad = FMath::DegreesToRadians(AngleToTarget);
	float ActualTurnRad = FMath::Min(MaxTurnThisFrame, DesiredTurnRad);

	FVector NewLocation;
	float NewYaw;

	if (Distance < GetArrivalTolerance())
	{
		NewLocation = CurrentLocation + DesiredDirection * MoveDistance;
		NewYaw = DesiredDirection.Rotation().Yaw;
	}
	else if (AngleToTarget > 0.5f)
	{
		float ArcMoveDistance = FMath::Min(EffectiveSpeed * DeltaTime, Distance);
		float ArcAngleRad = ArcMoveDistance / FMath::Max(TurningRadius, 1.f);

		FVector Right = OwnerActor->GetActorRightVector();
		FVector TurnCenter = CurrentLocation + (Right * TurnDirection * TurningRadius);

		float CurrentYaw = OwnerActor->GetActorRotation().Yaw;
		NewYaw = CurrentYaw + (FMath::RadiansToDegrees(ArcAngleRad) * TurnDirection);

		FVector FromCenter = CurrentLocation - TurnCenter;
		FQuat Rotation = FQuat(FVector::UpVector, ArcAngleRad * TurnDirection);
		FVector RotatedFromCenter = Rotation.RotateVector(FromCenter);
		NewLocation = TurnCenter + RotatedFromCenter;
	}
	else
	{
		NewYaw = OwnerActor->GetActorRotation().Yaw;
		NewLocation = CurrentLocation + CurrentForward * MoveDistance;
	}

	NewLocation.Z = CurrentLocation.Z;

	OwnerActor->SetActorLocation(NewLocation, false);
	DesiredMovementYaw = NewYaw;
}

/* ============================================================================
   SPLINE PATROL MOVEMENT
   ============================================================================ */

void UMovableTargetComponent::InitializeSplineMovement()
{
	FVector ActorLocation = GetOwner()->GetActorLocation();
	float InputKey = PatrolSpline->FindInputKeyClosestToWorldLocation(ActorLocation);
	DistanceAlongSpline = PatrolSpline->GetDistanceAlongSplineAtSplineInputKey(InputKey);

	FVector SplinePoint = PatrolSpline->GetLocationAtDistanceAlongSpline(
		DistanceAlongSpline, ESplineCoordinateSpace::World);
	float DistToSpline = FVector::Dist2D(ActorLocation, SplinePoint);

	bOnSpline = (DistToSpline < GetArrivalTolerance());
	SplineBlendAlpha = bOnSpline ? 1.f : 0.f;
}

void UMovableTargetComponent::MoveAlongSpline(float DeltaTime)
{
	if (!PatrolSpline)
		return;

	const float Speed = BaseSpeed * SpeedMultiplier;
	const float SplineLength = PatrolSpline->GetSplineLength();

	if (!bOnSpline)
	{
		FVector CurrentLoc = GetOwner()->GetActorLocation();
		float NewInputKey = PatrolSpline->FindInputKeyClosestToWorldLocation(CurrentLoc);
		DistanceAlongSpline = PatrolSpline->GetDistanceAlongSplineAtSplineInputKey(NewInputKey);

		// Aim ahead along the path. Steering toward the nearest point can cause
		// oscillation when the vehicle cannot turn sharply enough to reach it.
		float AimDistance = DistanceAlongSpline + GetLookaheadDistance();
		if (PatrolSpline->IsClosedLoop() && SplineLength > KINDA_SMALL_NUMBER)
		{
			AimDistance = FMath::Fmod(AimDistance, SplineLength);
		}
		else
		{
			AimDistance = FMath::Min(AimDistance, SplineLength);
		}

		FVector SplineTarget = PatrolSpline->GetLocationAtDistanceAlongSpline(
			AimDistance, ESplineCoordinateSpace::World);

		// Increase speed gradually while joining the path.
		const float JoinRate = (Speed > CurrentSpeed) ? GetAccelRate() : GetBrakeRate();
		CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, Speed, DeltaTime, JoinRate);

		ArcSteerToward(DeltaTime, SplineTarget, FMath::Max(CurrentSpeed, MinPatrolSpeed));

		CurrentLoc = GetOwner()->GetActorLocation();
		NewInputKey = PatrolSpline->FindInputKeyClosestToWorldLocation(CurrentLoc);
		DistanceAlongSpline = PatrolSpline->GetDistanceAlongSplineAtSplineInputKey(NewInputKey);

		FVector SplinePoint = PatrolSpline->GetLocationAtDistanceAlongSpline(
			DistanceAlongSpline, ESplineCoordinateSpace::World);

		float DistToSpline = FVector::Dist2D(CurrentLoc, SplinePoint);

		if (DistToSpline < GetArrivalTolerance() * 2.f)
		{
			bOnSpline = true;
			SplineBlendAlpha = 0.f;
			FVector Tangent = PatrolSpline->GetDirectionAtDistanceAlongSpline(
				DistanceAlongSpline, ESplineCoordinateSpace::World);
			DesiredMovementYaw = Tangent.Rotation().Yaw;
		}
		return;
	}

	// On an open spline, the endpoint is a destination. Limit speed to the
	// available braking distance so the vehicle stops at the endpoint.
	float DesiredCruise = Speed;
	if (!PatrolSpline->IsClosedLoop())
	{
		const float Remaining = FMath::Max(SplineLength - DistanceAlongSpline, 0.f);
		DesiredCruise = FMath::Min(Speed, GetApproachSpeedLimit(Remaining));
	}

	const float CruiseRate = (DesiredCruise > CurrentSpeed) ? GetAccelRate() : GetBrakeRate();
	CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, DesiredCruise, DeltaTime, CruiseRate);

	DistanceAlongSpline += CurrentSpeed * DeltaTime;

	// Closed splines loop; open splines clamp at the endpoint.
	if (PatrolSpline->IsClosedLoop())
	{
		DistanceAlongSpline = FMath::Fmod(DistanceAlongSpline, SplineLength);
	}
	else
	{
		DistanceAlongSpline = FMath::Min(DistanceAlongSpline, SplineLength);
	}

	FVector TargetLocation = PatrolSpline->GetLocationAtDistanceAlongSpline(
		DistanceAlongSpline, ESplineCoordinateSpace::World
	);

	// Blend onto the path over approximately 0.3 seconds to avoid snapping.
	if (SplineBlendAlpha < 1.f)
	{
		SplineBlendAlpha = FMath::Clamp(SplineBlendAlpha + DeltaTime / 0.3f, 0.f, 1.f);
		TargetLocation = FMath::Lerp(GetOwner()->GetActorLocation(), TargetLocation, SplineBlendAlpha);
	}

	GetOwner()->SetActorLocation(TargetLocation);

	FVector Tangent = PatrolSpline->GetDirectionAtDistanceAlongSpline(
		DistanceAlongSpline, ESplineCoordinateSpace::World
	);
	DesiredMovementYaw = Tangent.Rotation().Yaw;
}

/* ============================================================================
   PATROL AREA MOVEMENT
   ============================================================================ */

void UMovableTargetComponent::MoveInPatrolArea(float DeltaTime)
{
	if (!PatrolArea)
		return;

	const float Speed = BaseSpeed * SpeedMultiplier;

	switch (PatrolState)
	{
	case EPatrolAreaState::Idle:
	{
		AActor* Owner = GetOwner();
		FVector Forward2D = Owner->GetActorForwardVector();
		Forward2D.Z = 0.f;
		Forward2D.Normalize();
		FVector VehicleLoc = Owner->GetActorLocation();

		float EffectiveMinDist = (GetMinWaypointDistance() > 0.f)
			? GetMinWaypointDistance()
			: GetTurningRadius() * 2.f;

		bool bFoundGoodPoint = false;
		for (int32 i = 0; i < 10; ++i)
		{
			FVector Candidate = PatrolArea->GetRandomPointInArea();
			Candidate.Z = VehicleLoc.Z;

			float DistToCandidate = FVector::Dist2D(VehicleLoc, Candidate);
			if (DistToCandidate < EffectiveMinDist)
				continue;

			FVector ToCandidate = (Candidate - VehicleLoc).GetSafeNormal2D();
			float Dot = FVector::DotProduct(Forward2D, ToCandidate);
			if (Dot > 0.f)
			{
				CurrentDestination = Candidate;
				bFoundGoodPoint = true;
				break;
			}
			if (DistToCandidate > EffectiveMinDist * 2.f)
			{
				CurrentDestination = Candidate;
				bFoundGoodPoint = true;
				break;
			}
		}
		if (!bFoundGoodPoint)
		{
			CurrentDestination = PatrolArea->GetRandomPointInArea();
			CurrentDestination.Z = VehicleLoc.Z;
		}

		OrbitTimer = 0.f;
		LastDistanceToTarget = FVector::Dist2D(VehicleLoc, CurrentDestination);

		PatrolState = EPatrolAreaState::Moving;
		break;
	}

	case EPatrolAreaState::Moving:
#if WITH_EDITOR
		if (bShowDebug)
		{
			DrawDebugSphere(GetWorld(), CurrentDestination, 60.f, 12, FColor::Magenta, false, 0.f);
		}
#endif
		UpdatePatrolMovement(DeltaTime, Speed);
		break;

	case EPatrolAreaState::Waiting:
		WaitTimeRemaining -= DeltaTime;
		if (WaitTimeRemaining <= 0.f)
		{
			PatrolState = EPatrolAreaState::Idle;
		}
		break;
	}
}

void UMovableTargetComponent::UpdatePatrolMovement(float DeltaTime, float Speed)
{
	FVector CurrentLocation = GetOwner()->GetActorLocation();
	float Distance = FVector::Dist2D(CurrentLocation, CurrentDestination);

	if (Distance <= GetArrivalTolerance())
	{
		PatrolState = EPatrolAreaState::Waiting;
		WaitTimeRemaining = WaitTimeAtPoint;
		CurrentSpeed = 0.f;          // pull away from rest next time
		return;
	}

	OrbitTimer += DeltaTime;
	if (OrbitTimer >= OrbitTimeoutDuration)
	{
		float Progress = LastDistanceToTarget - Distance;
		if (Progress < LastDistanceToTarget * 0.2f)
		{
			PatrolState = EPatrolAreaState::Idle;
			return;
		}
		OrbitTimer = 0.f;
		LastDistanceToTarget = Distance;
	}

	// Reduce speed while approaching the waypoint.
	const float ApproachLimit = GetApproachSpeedLimit(Distance - GetArrivalTolerance());
	const float DesiredSpeed = FMath::Min(Speed, ApproachLimit);
	const float Rate = (DesiredSpeed > CurrentSpeed) ? GetAccelRate() : GetBrakeRate();
	CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, DesiredSpeed, DeltaTime, Rate);

	ArcSteerToward(DeltaTime, CurrentDestination, CurrentSpeed);
}

float UMovableTargetComponent::CalculateTurnAngle(const FVector& CurrentDir, const FVector& TargetDir) const
{
	float DotProduct = FVector::DotProduct(CurrentDir, TargetDir);
	float AngleRadians = FMath::Acos(FMath::Clamp(DotProduct, -1.f, 1.f));
	return FMath::RadiansToDegrees(AngleRadians);
}

/* ============================================================================
   CONVOY MOVEMENT
   ============================================================================ */

void UMovableTargetComponent::MoveInConvoy(float DeltaTime)
{
	if (!ConvoyLeader)
		return;

	USplineComponent* RootSpline = nullptr;
	float RootSplineDistance = 0.f;

	if (!FindRootSpline(RootSpline, RootSplineDistance))
		return;
	float CumulativeDistance = CalculateCumulativeFollowDistance();

	float SplineLength = RootSpline->GetSplineLength();

	// Limit the following gap to the route length. On short loops, a larger gap
	// can wrap followers onto the leader.
	if (SplineLength > KINDA_SMALL_NUMBER)
	{
		CumulativeDistance = FMath::Min(CumulativeDistance, SplineLength * 0.8f);
	}

	float DesiredDistance = RootSplineDistance - CumulativeDistance;
	const bool bLoop = RootSpline->IsClosedLoop() && SplineLength > KINDA_SMALL_NUMBER;

	// Wrap the follower distance across the loop seam instead of clamping it to
	// zero, which would send it to the spline start on each lap.
	if (bLoop)
	{
		DesiredDistance = FMath::Fmod(DesiredDistance, SplineLength);
		if (DesiredDistance < 0.f)
		{
			DesiredDistance += SplineLength;
		}
	}
	else
	{
		DesiredDistance = FMath::Clamp(DesiredDistance, 0.f, SplineLength);
	}

	// Measure the leader's actual speed along the spline. Using BaseSpeed here
	// causes followers to overshoot when the leader slows or stops.
	float LeaderSplineSpeed = 0.f;
	if (bConvoyInitialized)
	{
		float Delta = RootSplineDistance - PrevRootSplineDistance;

		// Adjust for seam crossings to avoid a one-frame negative speed spike
		// that would trigger hard braking.
		if (bLoop)
		{
			if (Delta > SplineLength * 0.5f)
			{
				Delta -= SplineLength;
			}
			else if (Delta < -SplineLength * 0.5f)
			{
				Delta += SplineLength;
			}
		}

		LeaderSplineSpeed = Delta / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	}
	PrevRootSplineDistance = RootSplineDistance;

	if (!bConvoyInitialized)
	{
		InitializeConvoyPosition(RootSpline, DesiredDistance);
		return;
	}

	UpdateConvoyMovement(DeltaTime, RootSpline, DesiredDistance, SplineLength, LeaderSplineSpeed);
}

bool UMovableTargetComponent::FindRootSpline(USplineComponent*& OutSpline, float& OutDistance)
{
	UMovableTargetComponent* Current = ConvoyLeader->FindComponentByClass<UMovableTargetComponent>();

	// Use a visited set to prevent infinite traversal of circular convoy chains.
	TSet<const UMovableTargetComponent*> Visited;

	while (Current)
	{
		if (Visited.Contains(Current))
		{
#if WITH_EDITOR
			if (bShowDebug)
			{
				UE_LOG(LogDynamicPayload, Error,
					TEXT("[Convoy] Circular ConvoyLeader chain detected on %s"),
					*GetOwner()->GetName());
			}
#endif
			return false;
		}
		Visited.Add(Current);

		if (Current->MovementMode == ETargetMovementMode::PatrolSpline && Current->PatrolSpline)
		{
			OutSpline = Current->PatrolSpline;
			OutDistance = Current->GetDistanceAlongSpline();
			return true;
		}

		if (Current->MovementMode == ETargetMovementMode::ConvoyFollow && Current->ConvoyLeader)
		{
			Current = Current->ConvoyLeader->FindComponentByClass<UMovableTargetComponent>();
		}
		else
		{
			break;
		}
	}

	return false;
}

float UMovableTargetComponent::CalculateCumulativeFollowDistance() const
{
	float Total = 0.f;
	const UMovableTargetComponent* Current = this;

	// Prevent traversal of circular convoy chains.
	TSet<const UMovableTargetComponent*> Visited;

	while (Current && Current->MovementMode == ETargetMovementMode::ConvoyFollow)
	{
		if (Visited.Contains(Current))
		{
			break;
		}
		Visited.Add(Current);

		Total += Current->GetFollowDistance();

		if (Current->ConvoyLeader)
		{
			UMovableTargetComponent* LeaderComp =
				Current->ConvoyLeader->FindComponentByClass<UMovableTargetComponent>();

			if (LeaderComp && LeaderComp->MovementMode == ETargetMovementMode::PatrolSpline)
				break;

			Current = LeaderComp;
		}
		else
		{
			break;
		}
	}

	return Total;
}

void UMovableTargetComponent::InitializeConvoyPosition(USplineComponent* Spline, float DesiredDistance)
{
	FVector CurrentLocation = GetOwner()->GetActorLocation();
	ConvoyDistanceAlongSpline = DesiredDistance;

	FVector SplinePoint = Spline->GetLocationAtDistanceAlongSpline(
		DesiredDistance, ESplineCoordinateSpace::World);
	float DistToSpline = FVector::Dist2D(CurrentLocation, SplinePoint);

	bConvoyOnSpline = (DistToSpline < GetArrivalTolerance());
	ConvoyBlendAlpha = bConvoyOnSpline ? 1.f : 0.f;
	bConvoyInitialized = true;
}

void UMovableTargetComponent::UpdateConvoyMovement(float DeltaTime, USplineComponent* Spline,
	float DesiredDistance, float SplineLength, float LeaderSplineSpeed)
{
	const float Speed = BaseSpeed * SpeedMultiplier;

	float ForwardClearance = GetForwardClearance();
	float CollisionBrake = 1.f;
	const float MinFollow = GetMinConvoyFollowDistance();
	if (ForwardClearance < MinFollow)
	{
		CollisionBrake = FMath::Clamp(ForwardClearance / FMath::Max(MinFollow, 1.f), 0.f, 1.f);
	}

	if (!bConvoyOnSpline)
	{
		FVector LeaderPos = ConvoyLeader->GetActorLocation();
		FVector CurrentLoc = GetOwner()->GetActorLocation();
		float DistToLeader = FVector::Dist2D(CurrentLoc, LeaderPos);

		float DistError = DistToLeader - GetFollowDistance();
		float ApproachSpeed = FMath::Clamp(
			LeaderSplineSpeed + DistError * ConvoyFollowStiffness,
			0.f,
			Speed * ConvoyMaxSpeedMultiplier
		);

		if (ApproachSpeed > 1.f)
		{
			ArcSteerToward(DeltaTime, LeaderPos, ApproachSpeed * CollisionBrake, false);
		}
		else
		{
			FVector ToLeader = (LeaderPos - CurrentLoc).GetSafeNormal2D();
			if (!ToLeader.IsNearlyZero())
			{
				DesiredMovementYaw = ToLeader.Rotation().Yaw;
			}
		}

		float NearestKey = Spline->FindInputKeyClosestToWorldLocation(GetOwner()->GetActorLocation());
		float NearestDist = Spline->GetDistanceAlongSplineAtSplineInputKey(NearestKey);
		FVector NearestSplinePoint = Spline->GetLocationAtDistanceAlongSpline(
			NearestDist, ESplineCoordinateSpace::World);

		float DistToSpline = FVector::Dist2D(GetOwner()->GetActorLocation(), NearestSplinePoint);

		if (DistToSpline < GetArrivalTolerance() * 2.f && DesiredDistance > GetArrivalTolerance())
		{
			bConvoyOnSpline = true;
			ConvoyBlendAlpha = 0.f;
			ConvoyDistanceAlongSpline = NearestDist;
			FVector Tangent = Spline->GetDirectionAtDistanceAlongSpline(
				NearestDist, ESplineCoordinateSpace::World);
			DesiredMovementYaw = Tangent.Rotation().Yaw;
		}
		return;
	}

	const bool bLoopPath = Spline->IsClosedLoop() && SplineLength > KINDA_SMALL_NUMBER;

	float DistanceGap = DesiredDistance - ConvoyDistanceAlongSpline;

	// Use the shortest path around the loop.
	if (bLoopPath)
	{
		if (DistanceGap > SplineLength * 0.5f)
		{
			DistanceGap -= SplineLength;
		}
		else if (DistanceGap < -SplineLength * 0.5f)
		{
			DistanceGap += SplineLength;
		}
	}

	const float FollowDist = GetFollowDistance();
	float ClampedGap = FMath::Clamp(DistanceGap, -FollowDist, FollowDist);

	// The derivative term damps oscillation around the target gap.
	float GapRate = 0.f;
	if (bGapErrorInitialized)
	{
		GapRate = (ClampedGap - PrevGapError) / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	}
	PrevGapError = ClampedGap;
	bGapErrorInitialized = true;

	// Match the leader's speed and apply a proportional correction for the gap.
	// When the leader stops and the gap is correct, the target speed is zero.
	const float MaxSpeed = Speed * ConvoyMaxSpeedMultiplier;

	// Limit the gain to available speed headroom to avoid saturation and
	// oscillation.
	const float Headroom = FMath::Max(MaxSpeed - FMath::Abs(LeaderSplineSpeed), 1.f);
	const float EffectiveKp = FMath::Min(ConvoyFollowStiffness,
		Headroom / FMath::Max(FollowDist, 1.f));

	// Limit gap-closing speed by braking distance so the follower slows as it
	// approaches the target gap.
	const float ApproachCap = GetApproachSpeedLimit(FMath::Abs(ClampedGap));
	float Correction = FMath::Sign(ClampedGap)
		* FMath::Min(FMath::Abs(ClampedGap) * EffectiveKp, ApproachCap);

	// Apply additional damping to reduce residual jitter.
	Correction += GapRate * ConvoyDamping;

	// Keep the clamp bounds valid when the leader is reversing and its speed is
	// negative.
	const float DownAuthority = FMath::Max(LeaderSplineSpeed + ConvoyRecoverySpeed, 1.f);
	Correction = FMath::Clamp(Correction, -DownAuthority, Headroom);

	// Allow negative correction so a follower can increase the gap when too close.
	const float DesiredSpeed = FMath::Clamp(
		LeaderSplineSpeed + Correction,
		-ConvoyRecoverySpeed,
		MaxSpeed
	);

	const float ConvoyRate = (DesiredSpeed > CurrentSpeed) ? GetAccelRate() : GetBrakeRate();
	CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, DesiredSpeed, DeltaTime, ConvoyRate);

	const float TargetSpeed = CurrentSpeed;

	// Apply forward-clearance braking to avoid collisions with actors not
	// represented by the spline spacing.
	ConvoyDistanceAlongSpline += TargetSpeed * CollisionBrake * DeltaTime;

	if (bLoopPath)
	{
		ConvoyDistanceAlongSpline = FMath::Fmod(ConvoyDistanceAlongSpline, SplineLength);
		if (ConvoyDistanceAlongSpline < 0.f)
		{
			ConvoyDistanceAlongSpline += SplineLength;
		}
	}
	else
	{
		ConvoyDistanceAlongSpline = FMath::Clamp(ConvoyDistanceAlongSpline, 0.f, SplineLength);
	}

	FVector SplineLocation = Spline->GetLocationAtDistanceAlongSpline(
		ConvoyDistanceAlongSpline, ESplineCoordinateSpace::World
	);
	FVector Tangent = Spline->GetDirectionAtDistanceAlongSpline(
		ConvoyDistanceAlongSpline, ESplineCoordinateSpace::World
	);

	if (ConvoyBlendAlpha < 1.f)
	{
		ConvoyBlendAlpha = FMath::Clamp(ConvoyBlendAlpha + DeltaTime / 0.3f, 0.f, 1.f);
		SplineLocation = FMath::Lerp(GetOwner()->GetActorLocation(), SplineLocation, ConvoyBlendAlpha);
	}

	GetOwner()->SetActorLocation(SplineLocation);
	DesiredMovementYaw = Tangent.Rotation().Yaw;

#if WITH_EDITOR
	if (bShowDebug)
	{
		DrawConvoyDebug(Spline, DesiredDistance, GetOwner()->GetActorLocation(), Tangent, DistanceGap);
	}
#endif
}

float UMovableTargetComponent::GetForwardClearance()
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	if (!OwnerActor || !World)
	{
		return GetConvoyForwardTraceRange();
	}

	FVector Start = OwnerActor->GetActorLocation() + FVector(0, 0, 100.f);

	FVector Forward = OwnerActor->GetActorForwardVector();
	Forward.Z = 0.f;
	Forward.Normalize();

	FVector End = Start + Forward * GetConvoyForwardTraceRange();

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(OwnerActor);

	// Refresh the follower list at the configured interval instead of scanning
	// every world actor for each follower on every frame.
	const float Now = World->GetTimeSeconds();
	if (Now >= CachedConvoyFollowersValidUntil)
	{
		CachedConvoyFollowers.Reset();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (*It != OwnerActor && It->FindComponentByClass<UMovableTargetComponent>())
			{
				CachedConvoyFollowers.Add(*It);
			}
		}
		CachedConvoyFollowersValidUntil = Now + ConvoyFollowerCacheRefreshSeconds;
	}

	for (const TWeakObjectPtr<AActor>& WeakActor : CachedConvoyFollowers)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			Params.AddIgnoredActor(Actor);
		}
	}

	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			DrawDebugLine(World, Start, Hit.ImpactPoint, FColor::Orange, false, 0.f, 0, 3.f);
		}
#endif
		return Hit.Distance;
	}

	return GetConvoyForwardTraceRange();
}

void UMovableTargetComponent::DrawConvoyDebug(USplineComponent* Spline, float DesiredDistance,
	const FVector& CurrentLoc, const FVector& Tangent, float Gap)
{
#if WITH_EDITOR
	DrawDebugLine(GetWorld(), CurrentLoc, CurrentLoc + Tangent * 200.f,
		FColor::Cyan, false, 0.f, 0, 5.f);

	FVector DesiredLocation = Spline->GetLocationAtDistanceAlongSpline(
		DesiredDistance, ESplineCoordinateSpace::World
	);

	DrawDebugSphere(GetWorld(), DesiredLocation, 50.f, 12, FColor::Green, false, 0.f);
	DrawDebugSphere(GetWorld(), CurrentLoc, 40.f, 12, FColor::Yellow, false, 0.f);

	DrawDebugLine(GetWorld(), CurrentLoc, DesiredLocation,
		Gap > 0.f ? FColor::Red : FColor::Blue, false, 0.f, 0, 2.f);
#endif
}

/* ============================================================================
   GROUND ALIGNMENT
   ============================================================================ */

void UMovableTargetComponent::RefreshDerivedGeometry()
{
	ResolvedVehicleLength    = FMath::Max(CachedLongitudinalExtent * 2.f, 100.f);
	ResolvedFollowDistance   = ResolvedVehicleLength * FollowGapInLengths;
	ResolvedTurningRadius    = ResolvedVehicleLength * TurningRadiusInLengths;
	ResolvedArrivalTolerance = ResolvedVehicleLength * ArrivalToleranceInLengths;
	ResolvedAxleOffset       = ResolvedVehicleLength * AxleOffsetInLengths;
}

/** Return the configured value when overridden or before mesh geometry is
 *  available. */

float UMovableTargetComponent::GetFollowDistance() const
{
	if (bUseAbsoluteGeometry || ResolvedFollowDistance <= KINDA_SMALL_NUMBER)
		return FollowDistance;
	return ResolvedFollowDistance;
}

float UMovableTargetComponent::GetTurningRadius() const
{
	if (bUseAbsoluteGeometry || ResolvedTurningRadius <= KINDA_SMALL_NUMBER)
		return MinTurningRadius;
	return ResolvedTurningRadius;
}

float UMovableTargetComponent::GetArrivalTolerance() const
{
	if (bUseAbsoluteGeometry || ResolvedArrivalTolerance <= KINDA_SMALL_NUMBER)
		return ArrivalTolerance;
	return ResolvedArrivalTolerance;
}

float UMovableTargetComponent::GetMinWaypointDistance() const
{
	if (bUseAbsoluteGeometry || ResolvedVehicleLength <= KINDA_SMALL_NUMBER)
		return MinWaypointDistance;
	return ResolvedVehicleLength * MinWaypointInLengths;
}

float UMovableTargetComponent::GetAxleOffset() const
{
	if (bUseAbsoluteGeometry || ResolvedAxleOffset <= KINDA_SMALL_NUMBER)
		return FMath::Max(FrontTraceOffset, RearTraceOffset);
	return ResolvedAxleOffset;
}

float UMovableTargetComponent::GetMinConvoyFollowDistance() const
{
	if (bUseAbsoluteGeometry)
		return MinConvoyFollowDistance;
	return GetFollowDistance() * 0.6f;
}

float UMovableTargetComponent::GetConvoyForwardTraceRange() const
{
	if (bUseAbsoluteGeometry)
		return ConvoyForwardTraceRange;
	return GetFollowDistance() * 1.6f;
}

float UMovableTargetComponent::GetAccelRate() const
{
	const float Target = FMath::Max(BaseSpeed * SpeedMultiplier, 1.f);
	return Target / FMath::Max(SpeedRampTime, 0.05f);
}

float UMovableTargetComponent::GetBrakeRate() const
{
	return GetAccelRate() * FMath::Max(BrakeRampMultiplier, 1.f);
}

float UMovableTargetComponent::GetLookaheadDistance() const
{
	return FMath::Max(GetVehicleLength(), CurrentSpeed * LookaheadSeconds);
}

float UMovableTargetComponent::GetApproachSpeedLimit(float Distance) const
{
	return FMath::Sqrt(2.f * GetBrakeRate() * FMath::Max(Distance, 0.f));
}

void UMovableTargetComponent::ResetToStart()
{
	AActor* Owner = GetOwner();
	if (!Owner)
		return;

	// Use TeleportPhysics to avoid swept collisions while restoring the actor.
	Owner->SetActorTransform(StartTransform, false, nullptr, ETeleportType::TeleportPhysics);

	// --- path progress ---
	DistanceAlongSpline = 0.f;
	bOnSpline = false;
	ConvoyDistanceAlongSpline = 0.f;
	bConvoyInitialized = false;
	bConvoyOnSpline = false;
	PrevRootSplineDistance = 0.f;

	// --- patrol state machine ---
	PatrolState = EPatrolAreaState::Idle;
	CurrentDestination = FVector::ZeroVector;
	WaitTimeRemaining = 0.f;
	OrbitTimer = 0.f;
	LastDistanceToTarget = 0.f;

	// Reset controller history to avoid applying stale errors on the next tick.
	CurrentSpeed = 0.f;
	PrevGapError = 0.f;
	bGapErrorInitialized = false;
	SplineBlendAlpha = 1.f;
	ConvoyBlendAlpha = 1.f;

	// --- ground conforming ---
	SmoothedZ = StartTransform.GetLocation().Z;
	bZInitialized = false;
	DesiredMovementYaw = StartTransform.Rotator().Yaw;

	// Force the follower list to be rebuilt.
	CachedConvoyFollowersValidUntil = -1.f;

	SetComponentTickEnabled(true);

	// Recalculate the actor's position along the path.
	if (PatrolSpline)
	{
		InitializeSplineMovement();
	}
}

void UMovableTargetComponent::CacheOwnerBounds()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
		return;

	if (ATargetActor* TargetActor = Cast<ATargetActor>(OwnerActor))
	{
		if (TargetActor->Mesh && TargetActor->Mesh->GetStaticMesh())
		{
			FBoxSphereBounds LocalBounds = TargetActor->Mesh->GetStaticMesh()->GetBounds();
			FVector Scale = TargetActor->Mesh->GetComponentScale();
			float ScaledX = LocalBounds.BoxExtent.X * Scale.X;
			float ScaledY = LocalBounds.BoxExtent.Y * Scale.Y;

			CachedLongitudinalExtent = FMath::Max(FMath::Max(ScaledX, ScaledY), 50.f);
			CachedLateralExtent = FMath::Max(FMath::Min(ScaledX, ScaledY), 30.f);

			RefreshDerivedGeometry();
			return;
		}
	}

	FBox ActorBounds = OwnerActor->GetComponentsBoundingBox(true);
	FVector Extents = ActorBounds.GetExtent();
	CachedLongitudinalExtent = FMath::Max(FMath::Max(Extents.X, Extents.Y), 50.f);
	CachedLateralExtent = FMath::Max(FMath::Min(Extents.X, Extents.Y), 30.f);

	RefreshDerivedGeometry();
}

void UMovableTargetComponent::AlignToGround(float DeltaTime)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
		return;

	FVector ActorLocation = OwnerActor->GetActorLocation();
	FVector Forward = OwnerActor->GetActorForwardVector();
	FVector Right = OwnerActor->GetActorRightVector();

	FHitResult FrontHit, RearHit, LeftHit, RightHit;

	const float AxleOffset = GetAxleOffset();
	FVector FrontOffset = Forward * AxleOffset;
	FVector RearOffset = -Forward * AxleOffset;
	FVector LeftOffset = -Right * CachedLateralExtent;
	FVector RightOffset = Right * CachedLateralExtent;

	bool bFront = TraceGround(ActorLocation, FrontOffset, FrontHit);
	bool bRear = TraceGround(ActorLocation, RearOffset, RearHit);
	bool bLeft = TraceGround(ActorLocation, LeftOffset, LeftHit);
	bool bRight = TraceGround(ActorLocation, RightOffset, RightHit);

	if (!bFront || !bRear)
		return;

	float SumZ = FrontHit.ImpactPoint.Z + RearHit.ImpactPoint.Z;
	int HitCount = 2;

	if (bLeft)
	{
		SumZ += LeftHit.ImpactPoint.Z;
		HitCount++;
	}
	if (bRight)
	{
		SumZ += RightHit.ImpactPoint.Z;
		HitCount++;
	}

	float TargetZ = (SumZ / HitCount) + GroundOffset;

	if (!bZInitialized || FMath::Abs(SmoothedZ - TargetZ) > 500.f)
	{
		SmoothedZ = TargetZ;
		bZInitialized = true;
	}
	else
	{
		SmoothedZ = FMath::FInterpTo(SmoothedZ, TargetZ, DeltaTime, GroundAlignInterpSpeed);
	}

	ActorLocation.Z = SmoothedZ;
	OwnerActor->SetActorLocation(ActorLocation, false, nullptr, ETeleportType::TeleportPhysics);

	const FRotator CurrentRot = OwnerActor->GetActorRotation();
	const float SmoothedYaw = FMath::FixedTurn(CurrentRot.Yaw, DesiredMovementYaw, YawTurnSpeed * DeltaTime);

	// TargetActors tilt their GroundFrame child for slopes, so the root needs
	// only yaw. Preserve pitch and roll for actors without a GroundFrame.
	if (Cast<ATargetActor>(OwnerActor))
	{
		OwnerActor->SetActorRotation(FRotator(0.f, SmoothedYaw, 0.f));
	}
	else
	{
		OwnerActor->SetActorRotation(FRotator(CurrentRot.Pitch, SmoothedYaw, CurrentRot.Roll));
	}

	if (bLeft && bRight)
	{
		ApplyGroundAlignment(FrontHit, RearHit, LeftHit, RightHit, DeltaTime);
	}
	else
	{
		FHitResult DummyLeft = FrontHit;
		FHitResult DummyRight = FrontHit;
		ApplyGroundAlignment(FrontHit, RearHit, DummyLeft, DummyRight, DeltaTime);
	}
}

bool UMovableTargetComponent::TraceGround(const FVector& Origin, const FVector& Offset, FHitResult& OutHit)
{
	FVector TraceStart = Origin + Offset + FVector(0, 0, TraceHeight);
	FVector TraceEnd = Origin + Offset - FVector(0, 0, TraceHeight);

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(GetOwner());

	// Trace by object type so only ground is considered. A WorldStatic channel
	// trace can also hit WorldDynamic actors and cause vehicles to stack.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	bool bHit = GetWorld()->LineTraceSingleByObjectType(
		OutHit, TraceStart, TraceEnd, ObjectParams, Params
	);

#if WITH_EDITOR
	if (bShowDebug)
	{
		FColor TraceColor = bHit ? FColor::Green : FColor::Red;
		DrawDebugLine(GetWorld(), TraceStart, bHit ? OutHit.ImpactPoint : TraceEnd,
			TraceColor, false, 0.f, 0, 2.f);
		if (bHit)
		{
			DrawDebugSphere(GetWorld(), OutHit.ImpactPoint, 10.f, 4, FColor::Yellow, false, 0.f);
		}
	}
#endif

	return bHit;
}

void UMovableTargetComponent::ApplyGroundAlignment(
	const FHitResult& FrontHit, const FHitResult& RearHit,
	const FHitResult& LeftHit, const FHitResult& RightHit, float DeltaTime)
{
	float PitchDelta = FrontHit.ImpactPoint.Z - RearHit.ImpactPoint.Z;
	float PitchDistance = GetAxleOffset() * 2.f;
	float Pitch = FMath::RadiansToDegrees(FMath::Atan2(PitchDelta, PitchDistance));

	float RollDelta = LeftHit.ImpactPoint.Z - RightHit.ImpactPoint.Z;
	float RollDistance = CachedLateralExtent * 2.f;
	float Roll = FMath::RadiansToDegrees(FMath::Atan2(RollDelta, FMath::Max(RollDistance, 1.f)));

	if (ATargetActor* TargetActor = Cast<ATargetActor>(GetOwner()))
	{
		if (USceneComponent* GroundFrame = TargetActor->GroundFrame)
		{
			FRotator Current = GroundFrame->GetRelativeRotation();
			FRotator Target(Pitch, 0.f, Roll);
			GroundFrame->SetRelativeRotation(
				FMath::RInterpTo(Current, Target, DeltaTime, GroundAlignInterpSpeed)
			);
		}
	}
}

/* ============================================================================
   UTILITIES
   ============================================================================ */

void UMovableTargetComponent::HandleBehaviorUpdated(float NewSpeedMultiplier, bool bNewCanMove)
{
	SpeedMultiplier = NewSpeedMultiplier;
	bCanMove = bNewCanMove;
}

USplineComponent* UMovableTargetComponent::ResolveActiveSpline() const
{
	// Use iterative traversal with a visited set to guard against cycles.
	const UMovableTargetComponent* Current = this;
	TSet<const UMovableTargetComponent*> Visited;

	while (Current)
	{
		if (Visited.Contains(Current))
		{
			return nullptr;
		}
		Visited.Add(Current);

		if (Current->PatrolSpline)
		{
			return Current->PatrolSpline;
		}

		if (Current->MovementMode == ETargetMovementMode::ConvoyFollow && Current->ConvoyLeader)
		{
			Current = Current->ConvoyLeader->FindComponentByClass<UMovableTargetComponent>();
		}
		else
		{
			break;
		}
	}

	return nullptr;
}

void UMovableTargetComponent::ValidateConvoySetup()
{
	AActor* OwnerActor = GetOwner();

	if (!ConvoyLeader)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Error,
				TEXT("[Convoy] %s: No ConvoyLeader assigned"), *OwnerActor->GetName());
		}
#endif
		return;
	}

	if (ConvoyLeader == OwnerActor)
	{
#if WITH_EDITOR
		if (bShowDebug)
		{
			UE_LOG(LogDynamicPayload, Error,
				TEXT("[Convoy] %s: Cannot follow itself"), *OwnerActor->GetName());
		}
#endif
		ConvoyLeader = nullptr;
		return;
	}

	if (PatrolSplineActor || PatrolArea)
	{
		PatrolSplineActor = nullptr;
		PatrolSpline = nullptr;
		PatrolArea = nullptr;
	}
}

void UMovableTargetComponent::AutoChainDuplicateFollowers()
{
	if (!ConvoyLeader || MovementMode != ETargetMovementMode::ConvoyFollow)
		return;

	AActor* OwnerActor = GetOwner();

	TArray<AActor*> SameLeaderFollowers;

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* OtherActor = *It;
		if (OtherActor == OwnerActor)
			continue;

		UMovableTargetComponent* OtherMove = OtherActor->FindComponentByClass<UMovableTargetComponent>();
		if (OtherMove &&
			OtherMove->MovementMode == ETargetMovementMode::ConvoyFollow &&
			OtherMove->ConvoyLeader == ConvoyLeader)
		{
			SameLeaderFollowers.Add(OtherActor);
		}
	}

	if (SameLeaderFollowers.Num() == 0)
		return;

	FVector LeaderLoc = ConvoyLeader->GetActorLocation();
	SameLeaderFollowers.Add(OwnerActor);

	SameLeaderFollowers.Sort([&LeaderLoc](const AActor& A, const AActor& B)
	{
		return FVector::DistSquared(A.GetActorLocation(), LeaderLoc)
			< FVector::DistSquared(B.GetActorLocation(), LeaderLoc);
	});

	for (int32 i = 1; i < SameLeaderFollowers.Num(); i++)
	{
		UMovableTargetComponent* ThisMove =
			SameLeaderFollowers[i]->FindComponentByClass<UMovableTargetComponent>();
		AActor* PreviousInChain = SameLeaderFollowers[i - 1];

		if (ThisMove && ThisMove->ConvoyLeader != PreviousInChain)
		{
			ThisMove->ConvoyLeader = PreviousInChain;
		}
	}
}
