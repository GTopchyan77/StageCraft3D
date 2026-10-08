// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSnappingComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Data/BaseItemData.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"
#include "Subsystems/StageSessionSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSnappingComponent)

namespace StageSnapping
{
	// Keeps guides readable when the camera is very close.
	constexpr double MinGuideThickness = 0.5;
}

UStageSnappingComponent::UStageSnappingComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageSnappingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndMove();
	PlacementNeighbours.Empty();
	Super::EndPlay(EndPlayReason);
}

bool UStageSnappingComponent::IsSnapToItemsEnabled() const
{
	// Without the StageCraft settings class there is no stored preference; snapping is the default.
	const UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	return !Settings || Settings->IsSnapToItemsEnabled();
}

void UStageSnappingComponent::SetSnapToItemsEnabled(bool bEnabled)
{
	UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	if (!Settings)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: GameUserSettingsClassName is not StageCraftUserSettings; the snap preference cannot be stored."), *GetNameSafe(GetOwner()));
		return;
	}

	if (Settings->SetSnapToItemsEnabled(bEnabled))
	{
		// A rare, explicit toggle: saved at once, unlike the debounced volume sliders.
		Settings->SaveSettings();
		UE_LOG(LogStageCraft, Log, TEXT("Snap to items %s."), bEnabled ? TEXT("on") : TEXT("off"));
	}
}

FStageSnapOutcome UStageSnappingComponent::SnapPlacement(const UBaseItemData& Item, const FTransform& FreeTransform, const FTransform& GridTransform)
{
	FStageSnapOutcome Outcome;
	Outcome.Location = GridTransform.GetLocation();

	// The ghost shows Item's Mesh; items whose visuals come from a Blueprint have nothing to measure before they exist.
	const UStaticMesh* Mesh = Item.Mesh.Get();
	if (!IsSnapToItemsEnabled() || !Mesh)
	{
		LastSnapSignature = 0;
		return Outcome;
	}

	const FBox FreeBox = Mesh->GetBoundingBox().TransformBy(FreeTransform);
	GatherNeighbourBounds({}, PlacementNeighbours);
	const StageSnapMath::FSnapResult Snap = StageSnapMath::ComputeSnap(FreeBox, PlacementNeighbours, SnapDistance, StageSnapMath::HorizontalAxes);
	NoteEngagement(Snap);
	if (!Snap.AnySnapped())
	{
		return Outcome;
	}

	Outcome.Location = StageSnapMath::MergeWithGrid(FreeTransform.GetLocation(), GridTransform.GetLocation(), Snap);
	Outcome.bSnapped = true;
	AddGuides(Snap, FreeBox.ShiftBy(Outcome.Location - FreeTransform.GetLocation()), PlacementNeighbours, Outcome.Guides);
	return Outcome;
}

void UStageSnappingComponent::BeginMove(const AActor& Moving, TConstArrayView<const AActor*> MovingWith)
{
	MovingActor = &Moving;
	MoveStartLocation = Moving.GetActorLocation();
	MoveStartBounds = GetItemBounds(Moving);

	TArray<const AActor*, TInlineAllocator<16>> Excluded(MovingWith);
	Excluded.Add(&Moving);
	GatherNeighbourBounds(Excluded, MoveNeighbours);
	LastSnapSignature = 0;
}

FStageSnapOutcome UStageSnappingComponent::SnapMove(const FVector& ProposedLocation, uint8 AxisMask)
{
	FStageSnapOutcome Outcome;
	Outcome.Location = ProposedLocation;
	if (!IsSnapToItemsEnabled() || !MovingActor.IsValid() || !MoveStartBounds.IsValid)
	{
		return Outcome;
	}

	// Translation only: the box at the proposal is the start box shifted by the same amount.
	const FBox ProposedBox = MoveStartBounds.ShiftBy(ProposedLocation - MoveStartLocation);
	const StageSnapMath::FSnapResult Snap = StageSnapMath::ComputeSnap(ProposedBox, MoveNeighbours, SnapDistance, AxisMask);
	NoteEngagement(Snap);
	if (!Snap.AnySnapped())
	{
		return Outcome;
	}

	const FVector Offset = Snap.GetOffset();
	Outcome.Location = ProposedLocation + Offset;
	Outcome.bSnapped = true;
	AddGuides(Snap, ProposedBox.ShiftBy(Offset), MoveNeighbours, Outcome.Guides);
	return Outcome;
}

void UStageSnappingComponent::EndMove()
{
	MovingActor.Reset();
	MoveStartBounds = FBox(ForceInit);
	MoveNeighbours.Reset();
	LastSnapSignature = 0;
}

void UStageSnappingComponent::GatherNeighbourBounds(TConstArrayView<const AActor*> Exclude, TArray<FBox>& OutBounds) const
{
	OutBounds.Reset();
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!Session)
	{
		return;
	}

	for (const AModularBaseActor* Item : Session->GetPlacedItems())
	{
		if (Item && !Exclude.Contains(Item) && !Item->IsActorBeingDestroyed())
		{
			const FBox Bounds = GetItemBounds(*Item);
			if (Bounds.IsValid)
			{
				OutBounds.Add(Bounds);
			}
		}
	}
}

void UStageSnappingComponent::AddGuides(const StageSnapMath::FSnapResult& Snap, const FBox& SnappedBox, TConstArrayView<FBox> Neighbours, FStageSnapGuideList& OutGuides) const
{
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const StageSnapMath::FAxisSnap& AxisSnap = Snap.Axes[Axis];
		if (!AxisSnap.bSnapped || !Neighbours.IsValidIndex(AxisSnap.Neighbour))
		{
			continue;
		}

		FStageSnapGuide& Guide = OutGuides.AddDefaulted_GetRef();
		StageSnapMath::MakeGuide(Axis, AxisSnap.Plane, SnappedBox, Neighbours[AxisSnap.Neighbour], Guide.Start, Guide.End);
		Guide.Thickness = ComputeGuideThickness(0.5 * (Guide.Start + Guide.End));
	}
}

void UStageSnappingComponent::NoteEngagement(const StageSnapMath::FSnapResult& Snap)
{
	// Only a change to a different snap is news; holding a snap while the cursor moves is not.
	const uint32 Signature = Snap.GetSignature();
	if (Signature != 0 && Signature != LastSnapSignature)
	{
		OnSnapEngaged.Broadcast();
	}
	LastSnapSignature = Signature;
}

double UStageSnappingComponent::ComputeGuideThickness(const FVector& At) const
{
	const APlayerController* Viewer = Cast<APlayerController>(GetOwner());
	const double Distance = Viewer && Viewer->PlayerCameraManager
		? FVector::Dist(Viewer->PlayerCameraManager->GetCameraLocation(), At)
		: 0.0;
	return FMath::Max(Distance * GuideThicknessPerCm, StageSnapping::MinGuideThickness);
}

FBox UStageSnappingComponent::GetItemBounds(const AActor& Item)
{
	return Item.GetComponentsBoundingBox(/*bNonColliding*/ false);
}
