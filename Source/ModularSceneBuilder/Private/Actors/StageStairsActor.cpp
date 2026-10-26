// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageStairsActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/StageStairsData.h"
#include "Engine/CollisionProfile.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStairsActor)

#define LOCTEXT_NAMESPACE "StageCraftStageStairsActor"

namespace StageStairsActor
{
	constexpr double CmToMetres = 0.01;
	constexpr int32 StepsSection = 0;
	constexpr int32 RailsSection = 1;
}

AStageStairsActor::AStageStairsActor()
{
	Surface = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Surface"));
	Surface->SetupAttachment(MeshComponent);
	Surface->SetMobility(EComponentMobility::Movable);
	// The same query-only collision as every stage item: selectable, a placement surface, no physics.
	Surface->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Surface->SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	// Item and placement traces are simple-collision queries; complex as simple lets them hit the solid steps.
	Surface->bUseComplexAsSimpleCollision = true;
}

UStageStairsData* AStageStairsActor::GetStairsData() const
{
	return Cast<UStageStairsData>(ItemData);
}

void AStageStairsActor::InitializeStairs(double InRise, double InWidth, bool bInHandrails)
{
	if (!ensureMsgf(!HasActorBegunPlay(), TEXT("%s: InitializeStairs is for stairs being spawned; edit placed stairs through their parameters."), *GetName()))
	{
		return;
	}
	// Stored as given; ApplyItemData sanitizes it once the item is known (spawning sets the item after this).
	Rise = InRise;
	Width = InWidth;
	bHandrails = bInHandrails;
	if (const UStageStairsData* Stairs = GetStairsData())
	{
		SanitizeShape(*Stairs);
		RebuildMesh();
	}
}

void AStageStairsActor::SanitizeShape(const UStageStairsData& Stairs)
{
	// A fresh actor (Library placement) has no size yet: the item's defaults, not a repair.
	if (Rise == 0.0 && Width == 0.0)
	{
		Rise = Stairs.GetDefaultRise();
		Width = Stairs.GetDefaultWidth();
		bHandrails = Stairs.bDefaultHandrails;
		return;
	}

	const auto Repair = [this](double& Value, double Min, double Max, double Default, const TCHAR* What)
	{
		if (FMath::IsFinite(Value) && Value >= Min && Value <= Max)
		{
			return;
		}
		const double Repaired = FMath::IsFinite(Value) && Value != 0.0 ? FMath::Clamp(Value, Min, Max) : Default;
		ReportStateRepair(FString::Printf(TEXT("stairs %s %s replaced by %.1f cm"), What, *LexToString(Value), Repaired));
		Value = Repaired;
	};
	Repair(Rise, Stairs.GetMinRise(), Stairs.GetMaxRise(), Stairs.GetDefaultRise(), TEXT("rise"));
	Repair(Width, Stairs.GetMinWidth(), Stairs.GetMaxWidth(), Stairs.GetDefaultWidth(), TEXT("width"));
}

void AStageStairsActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	if (const UStageStairsData* Stairs = Cast<UStageStairsData>(&Data))
	{
		// A swapped item has its own ranges; a loaded shape may be damaged. Both are sanitized here.
		SanitizeShape(*Stairs);
		Surface->SetMaterial(StageStairsActor::StepsSection, Stairs->StepMaterial.LoadSynchronous());
		Surface->SetMaterial(StageStairsActor::RailsSection, Stairs->HandrailMaterial.LoadSynchronous());
	}
	RebuildMesh();
}

void AStageStairsActor::RebuildMesh()
{
	Surface->ClearAllMeshSections();
	BuiltTriangleCount = 0;
	BuiltLayout = FStageStairsLayout();

	const UStageStairsData* Stairs = GetStairsData();
	if (!Stairs)
	{
		return;
	}
	BuiltLayout = StageProceduralShapes::LayoutStairs(Rise, Width, Stairs->TreadDepth, Stairs->MaxStepRise);
	const FStageShapeMesh Mesh = StageProceduralShapes::BuildStairsMesh(BuiltLayout, bHandrails);
	if (!Mesh.IsValid())
	{
		return;
	}
	for (int32 Index = 0; Index < Mesh.Sections.Num(); ++Index)
	{
		const FStageDeckMesh::FSection& Section = Mesh.Sections[Index];
		Surface->CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals, Section.UVs,
			TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision*/ true);
	}
	BuiltTriangleCount = Mesh.GetTriangleCount();
	// Sections created by this build start without an overlay.
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageStairsActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageStairsActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageStairsData* Stairs = GetStairsData();
	if (!Stairs)
	{
		return;
	}

	using namespace StageStairsActor;
	const FText Metres = LOCTEXT("Metres", "m");
	const double Step = FMath::Max(1.0, double(Stairs->SizeStep)) * CmToMetres;
	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Stairs, LOCTEXT("StairsSection", "Stairs"));
	Section.Add(StageCraftTags::Param_Stairs_Rise, LOCTEXT("Rise", "Rise"), FStageParameterValue::MakeFloat(Rise))
		.Range(Stairs->GetMinRise(), Stairs->GetMaxRise())
		.Display(CmToMetres, Metres, Step)
		.WithDefault(FStageParameterValue::MakeFloat(Stairs->GetDefaultRise()));
	Section.Add(StageCraftTags::Param_Stairs_Width, LOCTEXT("Width", "Width"), FStageParameterValue::MakeFloat(Width))
		.Range(Stairs->GetMinWidth(), Stairs->GetMaxWidth())
		.Display(CmToMetres, Metres, Step)
		.WithDefault(FStageParameterValue::MakeFloat(Stairs->GetDefaultWidth()));
	Section.Add(StageCraftTags::Param_Stairs_Handrails, LOCTEXT("Handrails", "Handrails"), FStageParameterValue::MakeBool(bHandrails))
		.WithDefault(FStageParameterValue::MakeBool(Stairs->bDefaultHandrails));
	Section.Add(StageCraftTags::Param_Stairs_Steps, LOCTEXT("Steps", "Steps"), FStageParameterValue::MakeInteger(BuiltLayout.Steps)).ReadOnly();
	Section.Add(StageCraftTags::Param_Stairs_StepRise, LOCTEXT("StepRise", "Step rise"), FStageParameterValue::MakeFloat(BuiltLayout.StepRise))
		.Display(CmToMetres, Metres).ReadOnly();
}

bool AStageStairsActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Stairs_Rise)
	{
		OutValue = FStageParameterValue::MakeFloat(Rise);
		return GetStairsData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Stairs_Width)
	{
		OutValue = FStageParameterValue::MakeFloat(Width);
		return GetStairsData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Stairs_Handrails)
	{
		OutValue = FStageParameterValue::MakeBool(bHandrails);
		return GetStairsData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Stairs_Steps)
	{
		OutValue = FStageParameterValue::MakeInteger(BuiltLayout.Steps);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Stairs_StepRise)
	{
		OutValue = FStageParameterValue::MakeFloat(BuiltLayout.StepRise);
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageStairsActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	const UStageStairsData* Stairs = GetStairsData();
	if (ParameterId == StageCraftTags::Param_Stairs_Rise || ParameterId == StageCraftTags::Param_Stairs_Width)
	{
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !Stairs)
		{
			return false;
		}
		// Clamped like every write; the inspector reads the stored value back, so an out-of-range edit snaps to the limit.
		if (ParameterId == StageCraftTags::Param_Stairs_Rise)
		{
			Rise = FMath::Clamp(double(Value.Float), double(Stairs->GetMinRise()), double(Stairs->GetMaxRise()));
		}
		else
		{
			Width = FMath::Clamp(double(Value.Float), double(Stairs->GetMinWidth()), double(Stairs->GetMaxWidth()));
		}
		RebuildMesh();
		NotifyParameterChanged(ParameterId);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Stairs_Handrails)
	{
		if (Value.Type != EStageParameterType::Bool || !Stairs)
		{
			return false;
		}
		bHandrails = Value.bBool;
		RebuildMesh();
		NotifyParameterChanged(ParameterId);
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
