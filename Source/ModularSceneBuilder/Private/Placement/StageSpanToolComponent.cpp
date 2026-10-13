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

namespace StageSpanTool
{
	/** A second click closer than this (cm, horizontally) to the start point places nothing: there is no direction to draw in. */
	constexpr double MinRunDistance = 1.0;
}

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
	// A different item never continues the previous item's run.
	HidePreview();
	SetPhase(EStageSpanToolPhase::Inactive);
	RefreshActivation();
}

void UStageSpanToolComponent::RefreshActivation()
{
	const UStageSpanData* Span = GetArmedSpan();
	if (!PlacementTool || !PlacementTool->IsPlaceMode() || !Span)
	{
		HidePreview();
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

bool UStageSpanToolComponent::ComputeRun(const UStageSpanData& Span, const FHitResult& Hit, FTransform& OutRunTransform, FStageSpanLayout& OutLayout) const
{
	if (!Hit.bBlockingHit)
	{
		return false;
	}

	const StageSpanMath::FRules Rules = StageSpanMath::MakeRules(Span);
	if (Phase != EStageSpanToolPhase::PickEnd)
	{
		// Before the first click: one segment where the run would start, pointing the way the last run went.
		OutLayout = StageSpanMath::ComputeLayout(Rules, 0.0);
		OutRunTransform = FTransform(FRotator(0.0, LastYawDegrees, 0.0), ComputeSnappedPoint(Span, Hit));
		return OutLayout.IsValid();
	}

	const FVector End = StageSpanMath::ConstrainEndPoint(StartPoint, ComputeSnappedPoint(Span, Hit), Span.AngleSnapDegrees);
	OutLayout = StageSpanMath::ComputeLayout(Rules, FVector::Dist2D(StartPoint, End));
	OutRunTransform = StageSpanMath::MakeRunTransform(StartPoint, End, LastYawDegrees);
	return OutLayout.IsValid();
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
	FTransform RunTransform;
	FStageSpanLayout Layout;
	if (!IsToolActive() || !Span || !ComputeRun(*Span, Hit, RunTransform, Layout))
	{
		ClearTarget();
		return;
	}

	const EStagePlacementPreviewState State = EvaluateRun(*Span, Layout.SegmentCount);
	if (AStageSpanPreview* Ghost = GetOrCreatePreview())
	{
		Ghost->ShowRun(RunTransform, Layout, State);
	}

	FStageSpanToolStatus NewStatus = MakeIdleStatus();
	NewStatus.bHasRun = Phase == EStageSpanToolPhase::PickEnd;
	NewStatus.Length = NewStatus.bHasRun ? Layout.Length : 0.0;
	NewStatus.SegmentCount = NewStatus.bHasRun ? Layout.SegmentCount : 0;
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
	FTransform RunTransform;
	FStageSpanLayout Layout;
	if (!IsToolActive() || !Span || !SpawnSystem || !ComputeRun(*Span, Hit, RunTransform, Layout))
	{
		return nullptr;
	}

	if (Phase == EStageSpanToolPhase::PickStart)
	{
		StartPoint = RunTransform.GetLocation();
		SetPhase(EStageSpanToolPhase::PickEnd);
		return nullptr;
	}

	const FVector End = StageSpanMath::ConstrainEndPoint(StartPoint, ComputeSnappedPoint(*Span, Hit), Span->AngleSnapDegrees);
	if (FVector::Dist2D(StartPoint, End) < StageSpanTool::MinRunDistance)
	{
		return nullptr;
	}

	// The configured length is validated with the run's full segment count before the run finishes spawning.
	AModularBaseActor* Placed = SpawnSystem->SpawnConfiguredItem(Span, RunTransform, [&Layout](AModularBaseActor& Actor)
	{
		if (AStageSpanActor* Run = Cast<AStageSpanActor>(&Actor))
		{
			Run->InitializeSpanLength(Layout.Length);
		}
	});
	if (!Placed)
	{
		// Refused (reported by the validator's owner) or failed: keep the start so the user can try a shorter run.
		return nullptr;
	}
	UE_CLOG(!Placed->IsA<AStageSpanActor>(), LogStageCraft, Warning, TEXT("%s: span item %s spawns %s, which is not a span actor; its length was ignored."),
		*GetNameSafe(GetOwner()), *Span->GetName(), *Placed->GetClass()->GetName());

	// Chained: the run's real end (whole segments may stop short of the cursor) is where the next one starts.
	StartPoint += RunTransform.GetRotation().GetForwardVector() * Layout.Length;
	LastYawDegrees = RunTransform.Rotator().Yaw;
	// The placement used up part of the session's limits, so the next run's verdict is asked again.
	VerdictSegmentCount = INDEX_NONE;
	OnItemPlaced.Broadcast(Placed);
	return Placed;
}

bool UStageSpanToolComponent::CancelStart()
{
	if (Phase != EStageSpanToolPhase::PickEnd)
	{
		return false;
	}
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
	if (const UStageSpanData* Span = Phase != EStageSpanToolPhase::Inactive ? GetArmedSpan() : nullptr)
	{
		Idle.ItemName = Span->DisplayName;
	}
	return Idle;
}

void UStageSpanToolComponent::SetStatus(const FStageSpanToolStatus& NewStatus)
{
	// Cursor moves along the same whole-segment run change nothing the user can read; only real changes are announced.
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
