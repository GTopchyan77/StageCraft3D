// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageArcTrussActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/StageArcTrussData.h"
#include "Engine/CollisionProfile.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageArcTrussActor)

#define LOCTEXT_NAMESPACE "StageCraftStageArcTrussActor"

namespace StageArcTrussActor
{
	constexpr double CmToMetres = 0.01;
	constexpr int32 TrussSection = 0;
}

AStageArcTrussActor::AStageArcTrussActor()
{
	Surface = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Surface"));
	Surface->SetupAttachment(MeshComponent);
	Surface->SetMobility(EComponentMobility::Movable);
	// The same query-only collision as every stage item: selectable, a placement surface, no physics.
	Surface->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Surface->SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	// Simple collision is the per-section hulls (SetCollisionConvexMeshes), not the thin tubes.
	Surface->bUseComplexAsSimpleCollision = false;
}

UStageArcTrussData* AStageArcTrussActor::GetArcData() const
{
	return Cast<UStageArcTrussData>(ItemData);
}

void AStageArcTrussActor::InitializeArc(double InRadius, double InAngleDegrees)
{
	if (!ensureMsgf(!HasActorBegunPlay(), TEXT("%s: InitializeArc is for arcs being spawned; edit placed arcs through their parameters."), *GetName()))
	{
		return;
	}
	// Stored as given; ApplyItemData sanitizes it once the item is known (spawning sets the item after this).
	Radius = InRadius;
	AngleDegrees = InAngleDegrees;
	if (const UStageArcTrussData* Arc = GetArcData())
	{
		SanitizeShape(*Arc);
		RebuildMesh();
	}
}

int32 AStageArcTrussActor::GetQuantity() const
{
	return BuiltLayout.IsValid() ? BuiltLayout.Sections : Super::GetQuantity();
}

int32 AStageArcTrussActor::PredictQuantity(const FGameplayTag& ParameterId, const FStageParameterValue& Value) const
{
	const UStageArcTrussData* Arc = GetArcData();
	if (Arc && Value.Type == EStageParameterType::Float && FMath::IsFinite(Value.Float))
	{
		if (ParameterId == StageCraftTags::Param_Arc_Radius)
		{
			return ComputeLayout(*Arc, Value.Float, AngleDegrees).Sections;
		}
		if (ParameterId == StageCraftTags::Param_Arc_Angle)
		{
			return ComputeLayout(*Arc, Radius, Value.Float).Sections;
		}
	}
	return Super::PredictQuantity(ParameterId, Value);
}

FStageArcTrussLayout AStageArcTrussActor::ComputeLayout(const UStageArcTrussData& Arc, double InRadius, double InAngleDegrees) const
{
	// The same clamps as a write, so a prediction matches what the write would build.
	const double CleanRadius = FMath::Clamp(InRadius, double(Arc.GetMinRadius()), double(Arc.GetMaxRadius()));
	const double CleanAngle = FMath::Clamp(InAngleDegrees, double(Arc.GetMinAngle()), double(Arc.GetMaxAngle()));
	return StageProceduralShapes::LayoutArcTruss(CleanRadius, CleanAngle, Arc.ProfileWidth, Arc.SectionLength);
}

void AStageArcTrussActor::SanitizeShape(const UStageArcTrussData& Arc)
{
	// A fresh actor (Library placement) has no size yet: the item's defaults, not a repair.
	if (Radius == 0.0 && AngleDegrees == 0.0)
	{
		Radius = Arc.GetDefaultRadius();
		AngleDegrees = Arc.GetDefaultAngle();
		return;
	}

	const auto Repair = [this](double& Value, double Min, double Max, double Default, const TCHAR* What)
	{
		if (FMath::IsFinite(Value) && Value >= Min && Value <= Max)
		{
			return;
		}
		const double Repaired = FMath::IsFinite(Value) && Value != 0.0 ? FMath::Clamp(Value, Min, Max) : Default;
		ReportStateRepair(FString::Printf(TEXT("curved truss %s %s replaced by %.1f"), What, *LexToString(Value), Repaired));
		Value = Repaired;
	};
	Repair(Radius, Arc.GetMinRadius(), Arc.GetMaxRadius(), Arc.GetDefaultRadius(), TEXT("radius"));
	Repair(AngleDegrees, Arc.GetMinAngle(), Arc.GetMaxAngle(), Arc.GetDefaultAngle(), TEXT("angle"));
}

void AStageArcTrussActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	if (const UStageArcTrussData* Arc = Cast<UStageArcTrussData>(&Data))
	{
		// A swapped item has its own ranges; a loaded shape may be damaged. Both are sanitized here.
		SanitizeShape(*Arc);
		Surface->SetMaterial(StageArcTrussActor::TrussSection, Arc->TrussMaterial.LoadSynchronous());
	}
	RebuildMesh();
}

void AStageArcTrussActor::RebuildMesh()
{
	Surface->ClearAllMeshSections();
	Surface->ClearCollisionConvexMeshes();
	BuiltTriangleCount = 0;
	BuiltLayout = FStageArcTrussLayout();

	const UStageArcTrussData* Arc = GetArcData();
	if (!Arc)
	{
		return;
	}
	BuiltLayout = ComputeLayout(*Arc, Radius, AngleDegrees);
	const FStageShapeMesh Mesh = StageProceduralShapes::BuildArcTrussMesh(BuiltLayout, Arc->ChordDiameter, Arc->BraceDiameter);
	if (!Mesh.IsValid())
	{
		return;
	}
	const FStageDeckMesh::FSection& Section = Mesh.Sections[StageArcTrussActor::TrussSection];
	Surface->CreateMeshSection(StageArcTrussActor::TrussSection, Section.Vertices, Section.Triangles, Section.Normals, Section.UVs,
		TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);
	Surface->SetCollisionConvexMeshes(StageProceduralShapes::BuildArcTrussCollision(BuiltLayout));
	BuiltTriangleCount = Mesh.GetTriangleCount();
	// Sections created by this build start without an overlay.
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageArcTrussActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageArcTrussActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageArcTrussData* Arc = GetArcData();
	if (!Arc)
	{
		return;
	}

	using namespace StageArcTrussActor;
	const FText Metres = LOCTEXT("Metres", "m");
	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_ArcTruss, LOCTEXT("ArcSection", "Curved truss"));
	Section.Add(StageCraftTags::Param_Arc_Radius, LOCTEXT("Radius", "Radius"), FStageParameterValue::MakeFloat(Radius))
		.Range(Arc->GetMinRadius(), Arc->GetMaxRadius())
		.Display(CmToMetres, Metres, FMath::Max(1.0, double(Arc->RadiusStep)) * CmToMetres)
		.WithDefault(FStageParameterValue::MakeFloat(Arc->GetDefaultRadius()));
	Section.Add(StageCraftTags::Param_Arc_Angle, LOCTEXT("Angle", "Angle"), FStageParameterValue::MakeFloat(AngleDegrees))
		.Range(Arc->GetMinAngle(), Arc->GetMaxAngle())
		.Display(1.0, LOCTEXT("Degrees", "°"), 5.0)
		.WithDefault(FStageParameterValue::MakeFloat(Arc->GetDefaultAngle()));
	Section.Add(StageCraftTags::Param_Arc_Length, LOCTEXT("Length", "Length"), FStageParameterValue::MakeFloat(BuiltLayout.ArcLength))
		.Display(CmToMetres, Metres).ReadOnly();
	Section.Add(StageCraftTags::Param_Arc_Sections, LOCTEXT("Sections", "Sections"), FStageParameterValue::MakeInteger(GetQuantity())).ReadOnly();
}

bool AStageArcTrussActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Arc_Radius)
	{
		OutValue = FStageParameterValue::MakeFloat(Radius);
		return GetArcData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Arc_Angle)
	{
		OutValue = FStageParameterValue::MakeFloat(AngleDegrees);
		return GetArcData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Arc_Length)
	{
		OutValue = FStageParameterValue::MakeFloat(BuiltLayout.ArcLength);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Arc_Sections)
	{
		OutValue = FStageParameterValue::MakeInteger(GetQuantity());
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageArcTrussActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Arc_Radius || ParameterId == StageCraftTags::Param_Arc_Angle)
	{
		const UStageArcTrussData* Arc = GetArcData();
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !Arc)
		{
			return false;
		}
		// Clamped like every write; the inspector reads the stored value back, so an out-of-range edit snaps to the limit.
		if (ParameterId == StageCraftTags::Param_Arc_Radius)
		{
			Radius = FMath::Clamp(double(Value.Float), double(Arc->GetMinRadius()), double(Arc->GetMaxRadius()));
		}
		else
		{
			AngleDegrees = FMath::Clamp(double(Value.Float), double(Arc->GetMinAngle()), double(Arc->GetMaxAngle()));
		}
		RebuildMesh();
		NotifyParameterChanged(ParameterId);
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
