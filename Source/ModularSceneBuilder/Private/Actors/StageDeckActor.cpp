// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageDeckActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/StageDeckData.h"
#include "Engine/CollisionProfile.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"
#include "Placement/StageDeckMath.h"
#include "ProceduralMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDeckActor)

#define LOCTEXT_NAMESPACE "StageCraftStageDeckActor"

namespace StageDeckActor
{
	constexpr double CmToMetres = 0.01;
	constexpr double Cm2ToSquareMetres = 0.0001;
	constexpr int32 TopSection = 0;
	constexpr int32 SkirtSection = 1;
}

AStageDeckActor::AStageDeckActor()
{
	Surface = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Surface"));
	Surface->SetupAttachment(MeshComponent);
	Surface->SetMobility(EComponentMobility::Movable);
	// The same query-only collision as every stage item: selectable, a placement surface, no physics.
	Surface->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Surface->SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	Surface->bUseComplexAsSimpleCollision = true;
}

UStageDeckData* AStageDeckActor::GetDeckData() const
{
	return Cast<UStageDeckData>(ItemData);
}

void AStageDeckActor::InitializeShape(TConstArrayView<FVector2D> LocalOutline, double Height)
{
	if (!ensureMsgf(!HasActorBegunPlay(), TEXT("%s: InitializeShape is for decks being spawned; edit placed decks through their parameters."), *GetName()))
	{
		return;
	}
	// Stored as given; ApplyItemData sanitizes it once the item is known (spawning sets the item after this).
	Outline = TArray<FVector2D>(LocalOutline);
	DeckHeight = Height;
	if (const UStageDeckData* Deck = GetDeckData())
	{
		SanitizeShape(*Deck);
		RebuildMesh();
	}
}

double AStageDeckActor::GetDeckArea() const
{
	return FMath::Abs(StageDeckMath::GetSignedArea(Outline));
}

int32 AStageDeckActor::GetQuantity() const
{
	const UStageDeckData* Deck = GetDeckData();
	const int32 Panels = Deck ? StageDeckMath::GetPanelCount(GetDeckArea(), Deck->GetPanelArea()) : 0;
	return Panels > 0 ? Panels : Super::GetQuantity();
}

void AStageDeckActor::SanitizeShape(const UStageDeckData& Deck)
{
	// A fresh actor (Library placement) has no outline and height yet: the item's defaults, not a repair.
	const bool bFresh = Outline.IsEmpty() && DeckHeight == 0.0;

	TArray<FVector2D> Clean;
	if (!StageDeckMath::SanitizeOutline(Outline, Clean))
	{
		if (!bFresh)
		{
			ReportStateRepair(FString::Printf(TEXT("unusable deck outline (%d corners) replaced by the default footprint"), Outline.Num()));
		}
		const FVector2D Size(FMath::Max(50.0, Deck.DefaultSize.X), FMath::Max(50.0, Deck.DefaultSize.Y));
		StageDeckMath::SanitizeOutline(StageDeckMath::MakeRectangle(Size.X, Size.Y), Clean);
	}
	else if (Clean.Num() != Outline.Num() && !bFresh)
	{
		ReportStateRepair(FString::Printf(TEXT("deck outline repaired (%d of %d corners kept)"), Clean.Num(), Outline.Num()));
	}
	Outline = MoveTemp(Clean);

	const double MinHeight = Deck.GetMinHeight();
	const double MaxHeight = Deck.GetMaxHeight();
	if (bFresh || DeckHeight == 0.0)
	{
		DeckHeight = Deck.GetDefaultHeight();
	}
	else if (!FMath::IsFinite(DeckHeight) || DeckHeight < MinHeight || DeckHeight > MaxHeight)
	{
		const double Repaired = FMath::IsFinite(DeckHeight) ? FMath::Clamp(DeckHeight, MinHeight, MaxHeight) : double(Deck.GetDefaultHeight());
		ReportStateRepair(FString::Printf(TEXT("deck height %s replaced by %.1f cm"), *LexToString(DeckHeight), Repaired));
		DeckHeight = Repaired;
	}
}

void AStageDeckActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	if (const UStageDeckData* Deck = Cast<UStageDeckData>(&Data))
	{
		// A swapped item has its own height range; a loaded shape may be damaged. Both are sanitized here.
		SanitizeShape(*Deck);
		Surface->SetMaterial(StageDeckActor::TopSection, Deck->SurfaceMaterial.LoadSynchronous());
		Surface->SetMaterial(StageDeckActor::SkirtSection, Deck->SkirtMaterial.LoadSynchronous());
	}
	RebuildMesh();
}

void AStageDeckActor::RebuildMesh()
{
	using namespace StageDeckActor;
	Surface->ClearAllMeshSections();
	BuiltTriangleCount = 0;

	const FStageDeckMesh Mesh = StageDeckMath::BuildMesh(Outline, DeckHeight);
	if (!Mesh.IsValid())
	{
		return;
	}
	const auto CreateSection = [this](int32 Index, const FStageDeckMesh::FSection& Section)
	{
		Surface->CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals, Section.UVs,
			TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision*/ true);
		BuiltTriangleCount += Section.Triangles.Num() / 3;
	};
	CreateSection(TopSection, Mesh.Top);
	CreateSection(SkirtSection, Mesh.Skirt);
	// Sections created by this build start without an overlay.
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageDeckActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Surface->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageDeckActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageDeckData* Deck = GetDeckData();
	if (!Deck)
	{
		return;
	}

	using namespace StageDeckActor;
	const FText Metres = LOCTEXT("Metres", "m");
	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Deck, LOCTEXT("DeckSection", "Deck"));
	Section.Add(StageCraftTags::Param_Deck_Height, LOCTEXT("Height", "Height"), FStageParameterValue::MakeFloat(DeckHeight))
		.Range(Deck->GetMinHeight(), Deck->GetMaxHeight())
		.Display(CmToMetres, Metres, FMath::Max(1.0, double(Deck->HeightStep)) * CmToMetres)
		.WithDefault(FStageParameterValue::MakeFloat(Deck->GetDefaultHeight()));
	Section.Add(StageCraftTags::Param_Deck_Area, LOCTEXT("Area", "Area"), FStageParameterValue::MakeFloat(GetDeckArea()))
		.Display(Cm2ToSquareMetres, LOCTEXT("SquareMetres", "m²")).ReadOnly();
	Section.Add(StageCraftTags::Param_Deck_Panels, LOCTEXT("Panels", "Panels"), FStageParameterValue::MakeInteger(GetQuantity())).ReadOnly();
	Section.Add(StageCraftTags::Param_Deck_Corners, LOCTEXT("Corners", "Corners"), FStageParameterValue::MakeInteger(Outline.Num())).ReadOnly();
}

bool AStageDeckActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Deck_Height)
	{
		OutValue = FStageParameterValue::MakeFloat(DeckHeight);
		return GetDeckData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Deck_Area)
	{
		OutValue = FStageParameterValue::MakeFloat(GetDeckArea());
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Deck_Panels)
	{
		OutValue = FStageParameterValue::MakeInteger(GetQuantity());
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Deck_Corners)
	{
		OutValue = FStageParameterValue::MakeInteger(Outline.Num());
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageDeckActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Deck_Height)
	{
		const UStageDeckData* Deck = GetDeckData();
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !Deck)
		{
			return false;
		}
		// Clamped like every write; the inspector reads the stored value back, so an out-of-range edit snaps to the limit.
		DeckHeight = FMath::Clamp(double(Value.Float), double(Deck->GetMinHeight()), double(Deck->GetMaxHeight()));
		RebuildMesh();
		NotifyParameterChanged(StageCraftTags::Param_Deck_Height);
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
