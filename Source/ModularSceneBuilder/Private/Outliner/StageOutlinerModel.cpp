// Copyright Epic Games, Inc. All Rights Reserved.

#include "Outliner/StageOutlinerModel.h"

#include "Algo/Count.h"
#include "Data/StageItemTypes.h"
#include "Misc/ComparisonUtility.h"
#include "UI/StageItemLibraryPanel.h"

namespace StageOutliner
{
	int32 GetGroupOrder(const FGameplayTag& GroupCategory)
	{
		const FGameplayTag Order[] =
		{
			StageCraftTags::Category_Stage, StageCraftTags::Category_Truss, StageCraftTags::Category_Rigging, StageCraftTags::Category_Lighting,
			StageCraftTags::Category_Audio, StageCraftTags::Category_Screen, StageCraftTags::Category_Cable, StageCraftTags::Category_Prop,
		};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Order); ++Index)
		{
			if (Order[Index] == GroupCategory)
			{
				return Index;
			}
		}
		return GroupCategory.IsValid() ? UE_ARRAY_COUNT(Order) : UE_ARRAY_COUNT(Order) + 1;
	}

	namespace
	{
		bool IsGroupBefore(const FStageOutlinerGroup& A, const FStageOutlinerGroup& B)
		{
			const int32 OrderA = GetGroupOrder(A.Category);
			const int32 OrderB = GetGroupOrder(B.Category);
			return OrderA != OrderB ? OrderA < OrderB : A.Label.CompareTo(B.Label) < 0;
		}

		bool IsItemBefore(const FStageOutlinerItem& A, const FStageOutlinerItem& B)
		{
			const int32 ByLabel = UE::ComparisonUtility::CompareNaturalOrder(A.Label.ToString(), B.Label.ToString());
			return ByLabel != 0 ? ByLabel < 0 : A.InstanceId < B.InstanceId;
		}

		EStageOutlinerFlagState Summarize(int32 SetCount, int32 Total)
		{
			return SetCount == 0 ? EStageOutlinerFlagState::None : (SetCount == Total ? EStageOutlinerFlagState::All : EStageOutlinerFlagState::Some);
		}
	}

	FGameplayTag GetGroupCategory(const FGameplayTag& Category)
	{
		if (!Category.IsValid() || Category == StageCraftTags::Category || !Category.MatchesTag(StageCraftTags::Category))
		{
			return FGameplayTag();
		}

		FGameplayTag Current = Category;
		FGameplayTag Parent = Current.RequestDirectParent();
		while (Parent.IsValid() && Parent != StageCraftTags::Category)
		{
			Current = Parent;
			Parent = Current.RequestDirectParent();
		}
		return Parent == StageCraftTags::Category ? Current : FGameplayTag();
	}

	FText MakeGroupLabel(const FGameplayTag& GroupCategory)
	{
		return StageItemLibrary::MakeCategoryLabel(GroupCategory);
	}

	TArray<FStageOutlinerGroup> BuildGroups(TArray<FStageOutlinerItem> Items)
	{
		TArray<FStageOutlinerGroup> Groups;
		for (FStageOutlinerItem& Item : Items)
		{
			const FGameplayTag GroupCategory = GetGroupCategory(Item.Category);
			FStageOutlinerGroup* Group = Groups.FindByPredicate([&GroupCategory](const FStageOutlinerGroup& Candidate) { return Candidate.Category == GroupCategory; });
			if (!Group)
			{
				Group = &Groups.AddDefaulted_GetRef();
				Group->Category = GroupCategory;
				Group->Label = MakeGroupLabel(GroupCategory);
			}
			Group->Items.Add(MoveTemp(Item));
		}

		Groups.Sort(&IsGroupBefore);
		for (FStageOutlinerGroup& Group : Groups)
		{
			Group.Items.Sort(&IsItemBefore);
			const int32 HiddenCount = Algo::CountIf(Group.Items, [](const FStageOutlinerItem& Item) { return Item.bHidden; });
			const int32 LockedCount = Algo::CountIf(Group.Items, [](const FStageOutlinerItem& Item) { return Item.bLocked; });
			Group.Hidden = Summarize(HiddenCount, Group.Items.Num());
			Group.Locked = Summarize(LockedCount, Group.Items.Num());
		}
		return Groups;
	}

	bool MatchesFilter(const FStageOutlinerItem& Item, const FText& GroupLabel, const FString& Query)
	{
		const FString Name = FString::Printf(TEXT("%s %s"), *Item.Label.ToString(), *Item.ModelName.ToString());
		return StageItemLibrary::MatchesSearch(Name, GroupLabel.ToString(), Query);
	}
}

