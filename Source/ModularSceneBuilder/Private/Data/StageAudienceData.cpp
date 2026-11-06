// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageAudienceData.h"

#include "Actors/StageAudienceActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudienceData)

#define LOCTEXT_NAMESPACE "StageCraftAudienceData"

namespace StagePeopleData
{
	constexpr double MinimumSize = 100.0;
	constexpr double MinimumDensity = 0.05;

	double Finite(double Value, double Fallback)
	{
		return FMath::IsFinite(Value) ? Value : Fallback;
	}
}

// --- Audience ---

UStageAudienceData::UStageAudienceData()
{
	ItemType = EStageItemType::Prop;
	ActorClass = AStageAudienceActor::StaticClass();
	CategoryTag = StageCraftTags::Category_People_Audience;
	PlacementRules.GridSize = FVector(50.0, 50.0, 0.0);
}

double UStageAudienceData::GetMinSize() const
{
	using namespace StagePeopleData;
	return FMath::Max(MinimumSize, FMath::Min(Finite(MinSize, MinimumSize), Finite(MaxSize, MinimumSize)));
}

double UStageAudienceData::GetMaxSize() const
{
	using namespace StagePeopleData;
	return FMath::Max(GetMinSize(), FMath::Max(Finite(MinSize, MinimumSize), Finite(MaxSize, MinimumSize)));
}

double UStageAudienceData::GetMinDensity() const
{
	using namespace StagePeopleData;
	return FMath::Clamp(FMath::Min(Finite(MinDensity, MinimumDensity), Finite(MaxDensity, MinimumDensity)), MinimumDensity, StageAudienceMath::MaxDensity);
}

double UStageAudienceData::GetMaxDensity() const
{
	using namespace StagePeopleData;
	return FMath::Clamp(FMath::Max(Finite(MinDensity, MinimumDensity), Finite(MaxDensity, MinimumDensity)), GetMinDensity(), StageAudienceMath::MaxDensity);
}

#if WITH_EDITOR

EDataValidationResult UStageAudienceData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (MinSize > MaxSize || MinDensity > MaxDensity)
	{
		Context.AddError(FText::Format(LOCTEXT("Ranges", "{0}: a Min value is above its Max."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (FigureMaterial.IsNull())
	{
		Context.AddError(FText::Format(LOCTEXT("AudienceMaterial", "{0}: Figure Material is required (M_StageFigure)."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (TopColors.IsEmpty())
	{
		Context.AddWarning(FText::Format(LOCTEXT("NoColours", "{0}: no Top Colors; everyone wears grey."), AssetName));
	}
	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStageAudienceActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotAudience", "{0}: ActorClass must be a StageAudienceActor."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
