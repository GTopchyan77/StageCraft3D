// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSpanToolComponent.h"

#include "Actors/StageSpanActor.h"
#include "Data/StageSpanData.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementMath.h"
#include "Placement/StageSpanPreview.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanToolComponent)

UStageSpanToolComponent::UStageSpanToolComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	PreviewClass = AStageSpanPreview::StaticClass();
}

void UStageSpanToolComponent::BeginPlay()
{
	Super::BeginPlay();

	PlacementTool = GetOwner()->FindComponentByClass<UStagePlacementToolComponent>();
	SpawnSystem = GetOwner()->FindComponentByClass<USpawnSystemComponent>();
	if (!ensureMsgf(PlacementTool && SpawnSystem, TEXT("%s: UStageSpanToolComponent needs UStagePlacementToolComponent and USpawnSystemComponent on the same actor; spans cannot be drawn."),
		*GetNameSafe(GetOwner())))
	{
		return;
	}

	PlacementTool->OnEditModeChanged.AddUniqueDynamic(this, &ThisClass::HandleEditModeChanged);
	PlacementTool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
	RefreshActivation();
}

void UStageSpanToolComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlacementTool)
	{
		PlacementTool->OnEditModeChanged.RemoveDynamic(this, &ThisClass::HandleEditModeChanged);
		PlacementTool->OnArmedItemChanged.RemoveDynamic(this, &ThisClass::HandleArmedItemChanged);
		PlacementTool = nullptr;
	}
	if (Preview)
	{
		Preview->Destroy();
		Preview = nullptr;
	}
	PreviewEvaluator.Unbind();
	Super::EndPlay(EndPlayReason);
}

void UStageSpanToolComponent::HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode)
{
	RefreshActivation();
}

void UStageSpanToolComponent::HandleArmedItemChanged(UBaseItemData* ArmedItem)
{
	// A different item never continues the previous item's path.
	HidePreview();
	PendingPoints.Reset();
	SetPhase(EStageSpanToolPhase::Inactive);
	RefreshActivation();
}

void UStageSpanToolComponent::RefreshActivation()
{
	const UStageSpanData* Span = GetArmedSpan();
	if (!PlacementTool || !PlacementTool->IsPlaceMode() || !Span)
	{
		HidePreview();
		PendingPoints.Reset();
		SetPhase(EStageSpanToolPhase::Inactive);
		return;
	}

	if (Preview)
	{
		Preview->SetItem(Span);
	}
	if (Phase == EStageSpanToolPhase::Inactive)
	{
		SetPhase(EStageSpanToolPhase::PickStart);
	}
}

UStageSpanData* UStageSpanToolComponent::GetArmedSpan() const
{
	return PlacementTool ? Cast<UStageSpanData>(PlacementTool->GetArmedItem()) : nullptr;
}

FVector UStageSpanToolComponent::ComputeSnappedPoint(const UStageSpanData& Span, const FHitResult& Hit) const
{
	return StagePlacementMath::ComputePlacementTransform(Span.PlacementRules, Hit.ImpactPoint, Hit.ImpactNormal).GetLocation();
}

FVector UStageSpanToolComponent::ComputeNextPoint(const UStageSpanData& Span, const FHitResult& Hit) const
{
	return StageSpanMath::ConstrainPathPoint(StageSpanMath::MakeRules(Span), PendingPoints.Last(), ComputeSnappedPoint(Span, Hit), Span.AngleSnapDegrees);
}

bool UStageSpanToolComponent::IsNearPoint(const FVector& World, const FVector& Target) const
{
	return FVector::Dist2D(World, Target) <= PointPickRadius;
}

UStageSpanToolComponent::FCandidatePath UStageSpanToolComponent::MakePath(const UStageSpanData& Span, TConstArrayView<FVector> WorldPoints, bool bClosed) const
{
	FCandidatePath Candidate;
	if (WorldPoints.IsEmpty())
	{
		return Candidate;
	}

	// Placed at the first vertex and facing along the first leg, so a one-leg path is exactly the old straight run.
	const FVector Second = WorldPoints.Num() > 1 ? WorldPoints[1] : WorldPoints[0];
	Candidate.Transform = StageSpanMath::MakeRunTransform(WorldPoints[0], Second, LastYawDegrees);

	TArray<FVector, TInlineAllocator<16>> Local;
	Local.Reserve(WorldPoints.Num());
	for (const FVector& Point : WorldPoints)
	{
		Local.Add(Candidate.Transform.InverseTransformPosition(Point));
	}
	Candidate.Layout = StageSpanMath::ComputePathLayout(StageSpanMath::MakeRules(Span), Local, bClosed, 0.0);
	return Candidate;
}

bool UStageSpanToolComponent::ComputeCandidate(const UStageSpanData& Span, const FHitResult& Hit, FCandidatePath& OutCandidate) const
{
	if (!Hit.bBlockingHit)
	{
		return false;
	}

	const FVector Cursor = ComputeSnappedPoint(Span, Hit);
	if (Phase != EStageSpanToolPhase::PickEnd || PendingPoints.IsEmpty())
	{
		// Before the first click: one segment where the path would start, pointing the way the last path went.
		const FVector Direction = FRotator(0.0, LastYawDegrees, 0.0).Vector();
		OutCandidate = MakePath(Span, { Cursor, Cursor + Direction * Span.SegmentLength }, false);
		return OutCandidate.Layout.IsValid();
	}

	// On the start point of a path that can close: the loop the click would make.
	if (PendingPoints.Num() >= 3 && IsNearPoint(Cursor, PendingPoints[0]))
	{
		FCandidatePath Loop = MakePath(Span, PendingPoints, true);
		Loop.bClosing = Loop.Layout.bClosed;
		// Not closable: the open path as drawn, so the click on the start point visibly does nothing.
		Loop.bCloseBlocked = !Loop.Layout.bClosed;
		OutCandidate = MoveTemp(Loop);
		return OutCandidate.Layout.IsValid();
	}

	TArray<FVector, TInlineAllocator<16>> Points(PendingPoints);
	const FVector Next = ComputeNextPoint(Span, Hit);
	if (FVector::Dist(Next, PendingPoints.Last()) >= StageSpanMath::MinLegLength)
	{
		Points.Add(Next);
	}
	OutCandidate = MakePath(Span, Points, false);
	return OutCandidate.Layout.IsValid();
}

EStagePlacementPreviewState UStageSpanToolComponent::EvaluateRun(const UStageSpanData& Span, int32 SegmentCount)
{
	if (SegmentCount != VerdictSegmentCount)
	{
		VerdictSegmentCount = SegmentCount;
		const bool bAllowed = !PreviewEvaluator.IsBound() || PreviewEvaluator.Execute(Span, SegmentCount).IsSuccess();
		Verdict = bAllowed ? EStagePlacementPreviewState::Valid : EStagePlacementPreviewState::Refused;
	}
	return Verdict;
}

void UStageSpanToolComponent::UpdateTarget(const FHitResult& Hit)
{
	const UStageSpanData* Span = GetArmedSpan();
	FCandidatePath Candidate;
	if (!IsToolActive() || !Span || !ComputeCandidate(*Span, Hit, Candidate))
	{
		ClearTarget();
		return;
	}

	// More points than the layout kept means the item's segment cap cut the path: the click would be refused.
	const bool bOnStart = Candidate.bClosing || Candidate.bCloseBlocked;
	const int32 ExpectedPoints = PendingPoints.Num() + (Phase == EStageSpanToolPhase::PickEnd && !bOnStart ? 1 : 0);
	const bool bTruncated = Phase == EStageSpanToolPhase::PickEnd && !bOnStart && Candidate.Layout.Points.Num() < ExpectedPoints
		&& FVector::Dist(ComputeNextPoint(*Span, Hit), PendingPoints.Last()) >= StageSpanMath::MinLegLength;

	const EStagePlacementPreviewState State = bTruncated || Candidate.bCloseBlocked ? EStagePlacementPreviewState::Refused : EvaluateRun(*Span, Candidate.Layout.GetSegmentCount());
	if (AStageSpanPreview* Ghost = GetOrCreatePreview())
	{
		Ghost->ShowPath(Candidate.Transform, Candidate.Layout, State);
	}

	FStageSpanToolStatus NewStatus = MakeIdleStatus();
	NewStatus.bHasRun = Phase == EStageSpanToolPhase::PickEnd;
	NewStatus.Length = NewStatus.bHasRun ? Candidate.Layout.GetLength() : 0.0;
	NewStatus.SegmentCount = NewStatus.bHasRun ? Candidate.Layout.GetSegmentCount() : 0;
	NewStatus.LegCount = NewStatus.bHasRun ? Candidate.Layout.Legs.Num() : 0;
	NewStatus.bClosing = Candidate.bClosing;
	NewStatus.bCloseBlocked = Candidate.bCloseBlocked;
	NewStatus.bRefused = State == EStagePlacementPreviewState::Refused;
	SetStatus(NewStatus);
}

void UStageSpanToolComponent::ClearTarget()
{
	HidePreview();
	SetStatus(MakeIdleStatus());
}

AModularBaseActor* UStageSpanToolComponent::HandleClick(const FHitResult& Hit)
{
	UStageSpanData* Span = GetArmedSpan();
	if (!IsToolActive() || !Span || !SpawnSystem || !Hit.bBlockingHit)
	{
		return nullptr;
	}

	const FVector Cursor = ComputeSnappedPoint(*Span, Hit);
	if (Phase == EStageSpanToolPhase::PickStart)
	{
		PendingPoints = { Cursor };
		SetPhase(EStageSpanToolPhase::PickEnd);
		return nullptr;
	}

	// A click on the start point closes the loop (or, when it cannot close, does nothing: never a stray point next to the start);
	// a second click on the last point (a double-click) finishes the path.
	if (PendingPoints.Num() >= 3 && IsNearPoint(Cursor, PendingPoints[0]))
	{
		return MakePath(*Span, PendingPoints, true).Layout.bClosed ? PlacePendingPath(/*bClosed*/ true) : nullptr;
	}
	if (PendingPoints.Num() >= 2 && IsNearPoint(Cursor, PendingPoints.Last()))
	{
		return PlacePendingPath(/*bClosed*/ false);
	}

	const FVector Next = ComputeNextPoint(*Span, Hit);
	if (FVector::Dist(Next, PendingPoints.Last()) < StageSpanMath::MinLegLength)
	{
		return nullptr;
	}

	TArray<FVector, TInlineAllocator<16>> Points(PendingPoints);
	Points.Add(Next);
	if (MakePath(*Span, Points, false).Layout.Points.Num() < Points.Num())
	{
		// The item's segment cap (or point cap) would cut the new leg off; the ghost already shows it refused.
		UE_LOG(LogStageCraft, Verbose, TEXT("%s: span path point refused: %s allows no more segments."), *GetNameSafe(GetOwner()), *Span->GetName());
		return nullptr;
	}

	PendingPoints.Add(Next);
	VerdictSegmentCount = INDEX_NONE;
	SetStatus(MakeIdleStatus());
	return nullptr;
}

AModularBaseActor* UStageSpanToolComponent::FinishPath()
{
	if (Phase != EStageSpanToolPhase::PickEnd || PendingPoints.Num() < 2)
	{
		return nullptr;
	}
	return PlacePendingPath(/*bClosed*/ false);
}

AModularBaseActor* UStageSpanToolComponent::PlacePendingPath(bool bClosed)
{
	UStageSpanData* Span = GetArmedSpan();
	if (!Span || !SpawnSystem)
	{
		return nullptr;
	}

	const FCandidatePath Path = MakePath(*Span, PendingPoints, bClosed);
	if (!Path.Layout.IsValid())
	{
		return nullptr;
	}

	// The whole path is validated with its full segment count before it finishes spawning.
	AModularBaseActor* Placed = SpawnSystem->SpawnConfiguredItem(Span, Path.Transform, [&Path](AModularBaseActor& Actor)
	{
		if (AStageSpanActor* Run = Cast<AStageSpanActor>(&Actor))
		{
			Run->InitializePath(Path.Layout.Points, Path.Layout.bClosed);
		}
	});
	if (!Placed)
	{
		// Refused (reported by the validator's owner) or failed: keep the points so the user can shorten the path.
		return nullptr;
	}
	UE_CLOG(!Placed->IsA<AStageSpanActor>(), LogStageCraft, Warning, TEXT("%s: span item %s spawns %s, which is not a span actor; its path was ignored."),
		*GetNameSafe(GetOwner()), *Span->GetName(), *Placed->GetClass()->GetName());

	// The next path's start marker points the way this one ended.
	const int32 LastLeg = Path.Layout.Legs.Num() - 1;
	LastYawDegrees = Path.Transform.TransformVector(Path.Layout.GetLegEnd(LastLeg) - Path.Layout.GetLegStart(LastLeg)).Rotation().Yaw;
	PendingPoints.Reset();
	SetPhase(EStageSpanToolPhase::PickStart);
	OnItemPlaced.Broadcast(Placed);
	return Placed;
}

bool UStageSpanToolComponent::RemoveLastPoint()
{
	if (Phase != EStageSpanToolPhase::PickEnd || PendingPoints.IsEmpty())
	{
		return false;
	}
	PendingPoints.Pop();
	VerdictSegmentCount = INDEX_NONE;
	if (PendingPoints.IsEmpty())
	{
		SetPhase(EStageSpanToolPhase::PickStart);
	}
	else
	{
		SetStatus(MakeIdleStatus());
	}
	return true;
}

bool UStageSpanToolComponent::CancelPath()
{
	if (Phase != EStageSpanToolPhase::PickEnd)
	{
		return false;
	}
	PendingPoints.Reset();
	SetPhase(EStageSpanToolPhase::PickStart);
	return true;
}

void UStageSpanToolComponent::SetPhase(EStageSpanToolPhase NewPhase)
{
	if (Phase == NewPhase)
	{
		return;
	}

	UE_LOG(LogStageCraft, Verbose, TEXT("%s: span tool %s -> %s."), *GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Phase), *UEnum::GetValueAsString(NewPhase));
	Phase = NewPhase;
	VerdictSegmentCount = INDEX_NONE;
	SetStatus(MakeIdleStatus());
}

FStageSpanToolStatus UStageSpanToolComponent::MakeIdleStatus() const
{
	FStageSpanToolStatus Idle;
	Idle.Phase = Phase;
	Idle.PointCount = PendingPoints.Num();
	if (const UStageSpanData* Span = Phase != EStageSpanToolPhase::Inactive ? GetArmedSpan() : nullptr)
	{
		Idle.ItemName = Span->DisplayName;
	}
	return Idle;
}

void UStageSpanToolComponent::SetStatus(const FStageSpanToolStatus& NewStatus)
{
	// Cursor moves that keep the same path change nothing the user can read; only real changes are announced.
	if (!(NewStatus == Status))
	{
		Status = NewStatus;
		OnStatusChanged.Broadcast(Status);
	}
}

void UStageSpanToolComponent::HidePreview()
{
	if (Preview)
	{
		Preview->HidePreview();
	}
}

AStageSpanPreview* UStageSpanToolComponent::GetOrCreatePreview()
{
	if (Preview || !PreviewClass)
	{
		return Preview;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = GetOwner();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;
	Preview = World->SpawnActor<AStageSpanPreview>(PreviewClass, FTransform::Identity, SpawnParams);
	if (Preview)
	{
		Preview->SetItem(GetArmedSpan());
	}
	return Preview;
}
