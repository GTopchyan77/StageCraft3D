// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageAudienceActor.h"

#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Data/StageAudienceData.h"
#include "Interaction/StageCraftCollision.h"
#include "People/StageFigureActors.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudienceActor)

#define LOCTEXT_NAMESPACE "StageCraftAudienceActor"

namespace StageAudienceActor
{
	constexpr double CmToMetres = 0.01;
	/** Half the height of the floor pick box, in cm. */
	constexpr double PickHalfHeight = 1.0;
	/** The inspector's size step: half a metre. */
	constexpr double SizeStep = 50.0;
	constexpr double DensityStep = 0.25;

	TArray<FText> MoodNames()
	{
		return { LOCTEXT("Calm", "Calm"), LOCTEXT("Dancing", "Dancing"), LOCTEXT("Cheering", "Cheering") };
	}

	int32 NewSeed()
	{
		// Never 0, which means "not chosen yet".
		return FMath::Max(1, FMath::Rand());
	}
}

AStageAudienceActor::AStageAudienceActor()
{
	Crowd = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("Crowd"));
	Crowd->SetupAttachment(MeshComponent);
	StageFigureActors::ConfigureFigureComponent(*Crowd, EStageFigureCollision::None);
	// Thousands of people: no shadows, no distance fields or ray tracing; the stage lights still light them.
	Crowd->SetCastShadow(false);
	Crowd->SetAffectDistanceFieldLighting(false);
	Crowd->SetVisibleInRayTracing(false);
	Crowd->bAffectDynamicIndirectLighting = false;

	PickArea = CreateDefaultSubobject<UBoxComponent>(TEXT("PickArea"));
	PickArea->SetupAttachment(MeshComponent);
	PickArea->SetMobility(EComponentMobility::Movable);
	PickArea->SetCanEverAffectNavigation(false);
	PickArea->SetGenerateOverlapEvents(false);
	PickArea->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickArea->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickArea->SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	PickArea->SetHiddenInGame(true);
	PickArea->SetBoxExtent(FVector(1.0, 1.0, StageAudienceActor::PickHalfHeight), /*bUpdateOverlaps*/ false);
}

UStageAudienceData* AStageAudienceActor::GetAudienceData() const
{
	return Cast<UStageAudienceData>(ItemData);
}

int32 AStageAudienceActor::GetPeopleCount() const
{
	return Crowd->GetInstanceCount();
}

void AStageAudienceActor::PrepareAsDuplicate()
{
	Super::PrepareAsDuplicate();
	// A copy is a different crowd of the same size: the same people standing twice would show.
	Seed = StageAudienceActor::NewSeed();
}

void AStageAudienceActor::SanitizeState(const UStageAudienceData& Audience)
{
	const bool bFresh = AreaSize.IsZero() && Density == 0.0 && Mood == EStageCrowdMood::Count;
	const double MinSize = Audience.GetMinSize();
	const double MaxSize = Audience.GetMaxSize();
	const double MinDensity = Audience.GetMinDensity();
	const double MaxDensity = Audience.GetMaxDensity();

	if (bFresh)
	{
		AreaSize = FVector2D(FMath::Clamp(Audience.DefaultSize.X, MinSize, MaxSize), FMath::Clamp(Audience.DefaultSize.Y, MinSize, MaxSize));
		Density = FMath::Clamp(Audience.DefaultDensity, MinDensity, MaxDensity);
		Mood = Audience.DefaultMood < EStageCrowdMood::Count ? Audience.DefaultMood : EStageCrowdMood::Dancing;
	}
	else
	{
		const auto RepairSize = [&](double& Value, const TCHAR* Name)
		{
			if (!FMath::IsFinite(Value) || Value < MinSize || Value > MaxSize)
			{
				const double Repaired = FMath::IsFinite(Value) ? FMath::Clamp(Value, MinSize, MaxSize) : FMath::Clamp(1000.0, MinSize, MaxSize);
				ReportStateRepair(FString::Printf(TEXT("audience %s %s replaced by %.0f cm"), Name, *LexToString(Value), Repaired));
				Value = Repaired;
			}
		};
		RepairSize(AreaSize.X, TEXT("width"));
		RepairSize(AreaSize.Y, TEXT("depth"));
		if (!FMath::IsFinite(Density) || Density < MinDensity || Density > MaxDensity)
		{
			const double Repaired = FMath::IsFinite(Density) ? FMath::Clamp(Density, MinDensity, MaxDensity) : FMath::Clamp(Audience.DefaultDensity, MinDensity, MaxDensity);
			ReportStateRepair(FString::Printf(TEXT("audience density %s replaced by %.2f"), *LexToString(Density), Repaired));
			Density = Repaired;
		}
		if (Mood >= EStageCrowdMood::Count)
		{
			ReportStateRepair(FString::Printf(TEXT("audience mood %d replaced by Dancing"), static_cast<int32>(Mood)));
			Mood = EStageCrowdMood::Dancing;
		}
	}
	if (Seed == 0)
	{
		Seed = StageAudienceActor::NewSeed();
	}
}

void AStageAudienceActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);
	if (const UStageAudienceData* Audience = Cast<UStageAudienceData>(&Data))
	{
		SanitizeState(*Audience);
		Crowd->SetCullDistances(0, FMath::Max(0, FMath::RoundToInt32(Audience->CullDistance)));
	}
	RebuildCrowd();
}

void AStageAudienceActor::RebuildCrowd()
{
	const UStageAudienceData* Audience = GetAudienceData();
	if (!Audience || !Crowd->IsRegistered())
	{
		Crowd->ClearInstances();
		return;
	}

	// The pick box covers the floor of the area: the area runs back from its front edge along +X.
	PickArea->SetRelativeLocation(FVector(AreaSize.Y * 0.5, 0.0, StageAudienceActor::PickHalfHeight));
	PickArea->SetBoxExtent(FVector(AreaSize.Y * 0.5, AreaSize.X * 0.5, StageAudienceActor::PickHalfHeight), /*bUpdateOverlaps*/ false);

	const int32 Count = StageAudienceMath::GetPeopleCount(AreaSize.X, AreaSize.Y, Density, Audience->MaxPeople);
	TArray<StageAudienceMath::FPerson> People;
	StageAudienceMath::PlacePeople(AreaSize.X, AreaSize.Y, Count, Mood, Seed, Audience->TopColors, People);
	StageFigureActors::ShowPeople(*Crowd, People, Audience->FigureMaterial.LoadSynchronous());
}

void AStageAudienceActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);
	const UStageAudienceData* Audience = GetAudienceData();
	if (!Audience)
	{
		return;
	}

	using namespace StageAudienceActor;
	const FText Metres = LOCTEXT("Metres", "m");
	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Audience, LOCTEXT("AudienceSection", "Audience"));
	Section.Add(StageCraftTags::Param_Audience_Width, LOCTEXT("Width", "Width"), FStageParameterValue::MakeFloat(AreaSize.X))
		.Range(Audience->GetMinSize(), Audience->GetMaxSize())
		.Display(CmToMetres, Metres, SizeStep * CmToMetres)
		.WithDefault(FStageParameterValue::MakeFloat(Audience->DefaultSize.X));
	Section.Add(StageCraftTags::Param_Audience_Depth, LOCTEXT("Depth", "Depth"), FStageParameterValue::MakeFloat(AreaSize.Y))
		.Range(Audience->GetMinSize(), Audience->GetMaxSize())
		.Display(CmToMetres, Metres, SizeStep * CmToMetres)
		.WithDefault(FStageParameterValue::MakeFloat(Audience->DefaultSize.Y));
	Section.Add(StageCraftTags::Param_Audience_Density, LOCTEXT("Density", "Density"), FStageParameterValue::MakeFloat(Density))
		.Range(Audience->GetMinDensity(), Audience->GetMaxDensity())
		.Display(1.0, LOCTEXT("PerSquareMetre", "/m²"), DensityStep)
		.WithDefault(FStageParameterValue::MakeFloat(Audience->DefaultDensity));
	FStageParameterDescriptor& MoodRow = Section.Add(StageCraftTags::Param_Audience_Mood, LOCTEXT("Mood", "Mood"),
		FStageParameterValue::MakeEnum(static_cast<int32>(Mood)));
	MoodRow.Options = MoodNames();
	Section.Add(StageCraftTags::Param_Audience_People, LOCTEXT("People", "People"), FStageParameterValue::MakeInteger(GetPeopleCount())).ReadOnly();
}

bool AStageAudienceActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Audience_Width)
	{
		OutValue = FStageParameterValue::MakeFloat(AreaSize.X);
		return GetAudienceData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Audience_Depth)
	{
		OutValue = FStageParameterValue::MakeFloat(AreaSize.Y);
		return GetAudienceData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Audience_Density)
	{
		OutValue = FStageParameterValue::MakeFloat(Density);
		return GetAudienceData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Audience_Mood)
	{
		OutValue = FStageParameterValue::MakeEnum(static_cast<int32>(Mood));
		return GetAudienceData() != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Audience_People)
	{
		OutValue = FStageParameterValue::MakeInteger(GetPeopleCount());
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageAudienceActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	const UStageAudienceData* Audience = GetAudienceData();
	const bool bSize = ParameterId == StageCraftTags::Param_Audience_Width || ParameterId == StageCraftTags::Param_Audience_Depth;
	if (bSize || ParameterId == StageCraftTags::Param_Audience_Density)
	{
		if (!Audience || Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float))
		{
			return false;
		}
		// Clamped like every write; the inspector reads the stored value back, so an out-of-range edit snaps to the limit.
		if (ParameterId == StageCraftTags::Param_Audience_Width)
		{
			AreaSize.X = FMath::Clamp(double(Value.Float), Audience->GetMinSize(), Audience->GetMaxSize());
		}
		else if (ParameterId == StageCraftTags::Param_Audience_Depth)
		{
			AreaSize.Y = FMath::Clamp(double(Value.Float), Audience->GetMinSize(), Audience->GetMaxSize());
		}
		else
		{
			Density = FMath::Clamp(double(Value.Float), Audience->GetMinDensity(), Audience->GetMaxDensity());
		}
	}
	else if (ParameterId == StageCraftTags::Param_Audience_Mood)
	{
		if (!Audience || Value.Type != EStageParameterType::Enum || Value.Integer < 0 || Value.Integer >= static_cast<int32>(EStageCrowdMood::Count))
		{
			return false;
		}
		Mood = static_cast<EStageCrowdMood>(Value.Integer);
	}
	else
	{
		return Super::WriteParameter(ParameterId, Value);
	}
	RebuildCrowd();
	NotifyParameterChanged(ParameterId);
	NotifyParameterChanged(StageCraftTags::Param_Audience_People);
	return true;
}

#undef LOCTEXT_NAMESPACE
