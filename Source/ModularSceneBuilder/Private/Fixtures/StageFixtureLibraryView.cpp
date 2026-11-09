// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageFixtureLibraryView.h"

#include "Data/BaseItemData.h"
#include "Data/LightingFixtureData.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "StageFixtureLibraryView"

namespace StageFixtureLibraryView
{
	namespace
	{
		FText JoinNames(const TArray<FString>& Names, int32 Total)
		{
			FString Joined = FString::Join(Names, TEXT(", "));
			if (Total > Names.Num())
			{
				Joined += FString::Printf(TEXT(" +%d"), Total - Names.Num());
			}
			return FText::FromString(Joined);
		}

		FText TypeName(const FString& Manufacturer, const FString& Name)
		{
			return FText::FromString(Manufacturer.IsEmpty() ? Name : Manufacturer + TEXT(" ") + Name);
		}

		FModeRow DescribeMode(const FStageFixtureModeSummary& Mode)
		{
			FModeRow Row;
			Row.bUsable = Mode.bUsable;
			Row.Name = FText::Format(LOCTEXT("ModeName", "{0} ({1} ch)"), FText::FromString(Mode.Name), FText::AsNumber(Mode.Footprint));
			if (!Mode.bUsable)
			{
				Row.Detail = FText::Format(LOCTEXT("ModeNotAdded", "Not added: {0}"), FText::FromString(Mode.Reason));
				return Row;
			}
			Row.Detail = FText::Format(LOCTEXT("ModeCounts", "{0} mapped · {1} approximated · {2} at default"),
				FText::AsNumber(Mode.Mapped), FText::AsNumber(Mode.Approximated), FText::AsNumber(Mode.Defaulted));
			if (Mode.Approximated > 0)
			{
				Row.Notes.Add(FText::Format(LOCTEXT("ApproximatedNames", "Approximated: {0}"), JoinNames(Mode.ApproximatedNames, Mode.Approximated)));
			}
			if (Mode.Defaulted > 0)
			{
				Row.Notes.Add(FText::Format(LOCTEXT("DefaultNames", "At default: {0}"), JoinNames(Mode.DefaultedNames, Mode.Defaulted)));
			}
			return Row;
		}
	}

	FReviewText DescribeReview(const FStageFixtureImportReview& Review)
	{
		FReviewText Text;
		const FText FileName = FText::FromString(FPaths::GetCleanFilename(Review.SourceFile));
		const FText Type = TypeName(Review.Manufacturer, Review.Name);
		Text.Title = Review.Name.IsEmpty() ? FileName : Type;
		Text.Source = Review.Manufacturer.IsEmpty() ? FileName
			: FText::Format(LOCTEXT("Source", "Manufacturer: {0} · {1}"), FText::FromString(Review.Manufacturer), FileName);
		Text.bCanConfirm = Review.bCanImport && Review.CountUsableModes() > 0;
		Text.ConfirmLabel = Review.bReplaces ? LOCTEXT("Replace", "REPLACE") : LOCTEXT("Add", "ADD");

		const int32 Usable = Review.CountUsableModes();
		if (!Text.bCanConfirm)
		{
			Text.Summary = LOCTEXT("CannotAdd", "This file cannot be added to the Library.");
		}
		else if (Review.bReplaces)
		{
			Text.Summary = FText::Format(LOCTEXT("Replaces", "Replaces {0} in the Library: {1} {1}|plural(one=mode,other=modes). Placed fixtures move to the mode with the same name."),
				Type, FText::AsNumber(Usable));
		}
		else
		{
			Text.Summary = FText::Format(LOCTEXT("Adds", "Adds {0} {0}|plural(one=mode,other=modes) to the Library. Each mode is a fixture you can place and patch."), FText::AsNumber(Usable));
		}

		for (const FStageFixtureModeSummary& Mode : Review.Modes)
		{
			Text.Modes.Add(DescribeMode(Mode));
		}
		for (const FString& Warning : Review.Warnings)
		{
			Text.Warnings.Add(FText::FromString(Warning));
		}
		for (const FString& Error : Review.Errors)
		{
			Text.Errors.Add(FText::FromString(Error));
		}
		return Text;
	}

	TArray<FCard> MakeCards(TConstArrayView<UBaseItemData*> Catalog, TConstArrayView<FStageFixtureLibraryType> Types)
	{
		TMap<const UBaseItemData*, int32> TypeOfMode;
		for (int32 Index = 0; Index < Types.Num(); ++Index)
		{
			for (const ULightingFixtureData* Mode : Types[Index].Modes)
			{
				if (Mode)
				{
					TypeOfMode.Add(Mode, Index);
				}
			}
		}

		TArray<FCard> Cards;
		Cards.Reserve(Catalog.Num());
		TSet<int32> Listed;
		for (UBaseItemData* Item : Catalog)
		{
			if (!Item)
			{
				continue;
			}
			const int32* TypeIndex = TypeOfMode.Find(Item);
			if (!TypeIndex)
			{
				FCard& Card = Cards.AddDefaulted_GetRef();
				Card.Item = Item;
				continue;
			}
			if (Listed.Contains(*TypeIndex))
			{
				continue;
			}
			Listed.Add(*TypeIndex);
			const FStageFixtureLibraryType& Type = Types[*TypeIndex];
			ULightingFixtureData* First = nullptr;
			int32 ModeCount = 0;
			for (ULightingFixtureData* Mode : Type.Modes)
			{
				if (Mode)
				{
					First = First ? First : Mode;
					++ModeCount;
				}
			}
			FCard& Card = Cards.AddDefaulted_GetRef();
			Card.Item = First;
			Card.FixtureTypeId = Type.FixtureTypeId;
			Card.ModeCount = ModeCount;
			Card.Name = TypeName(First->Specs.Manufacturer.ToString(), First->SourceTypeName);
			Card.Detail = Type.bSessionOnly
				? FText::Format(LOCTEXT("SessionCardDetail", "GDTF · {0} {0}|plural(one=mode,other=modes) · this session"), FText::AsNumber(ModeCount))
				: FText::Format(LOCTEXT("CardDetail", "GDTF · {0} {0}|plural(one=mode,other=modes)"), FText::AsNumber(ModeCount));
		}
		return Cards;
	}
}

#undef LOCTEXT_NAMESPACE
