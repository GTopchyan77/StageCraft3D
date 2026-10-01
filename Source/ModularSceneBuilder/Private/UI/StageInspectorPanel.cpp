// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageInspectorPanel.h"

#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Interaction/InteractableInterface.h"
#include "Interaction/StageParameterInterface.h"
#include "ModularSceneBuilder.h"
#include "UI/StageParameterRowWidget.h"
#include "UI/StageParameterSectionWidget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageInspectorPanel)

void UStageInspectorPanel::ClearView()
{
	if (SectionContainer)
	{
		SectionContainer->ClearChildren();
	}
}

void UStageInspectorPanel::BuildView(UObject& Target, const TArray<FStageParameterSection>& Sections)
{
	if (!SectionContainer)
	{
		return;
	}
	if (!SectionWidgetClass)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: SectionWidgetClass is not set; the inspector cannot show anything."), *GetName());
		return;
	}

	for (const FStageParameterSection& Section : Sections)
	{
		if (Section.Parameters.IsEmpty())
		{
			continue;
		}

		const FLinearColor SectionColor = GetGroupColor(Section.FeatureGroup);
		UStageParameterSectionWidget* SectionWidget = CreateWidget<UStageParameterSectionWidget>(this, SectionWidgetClass);
		SectionWidget->InitializeSection(Section, SectionColor);
		SectionContainer->AddChild(SectionWidget);

		for (const FStageParameterDescriptor& Descriptor : Section.Parameters)
		{
			const TSubclassOf<UStageParameterRowWidget> RowClass = ChooseRowClass(Descriptor);
			if (!RowClass)
			{
				continue;
			}

			UStageParameterRowWidget* Row = CreateWidget<UStageParameterRowWidget>(this, RowClass);
			SectionWidget->AddRow(*Row);
			Row->InitializeControl(Descriptor, SectionColor);
			RegisterControl(*Row);
		}
	}
}

void UStageInspectorPanel::OnViewUpdated(UObject* Target)
{
	if (EmptyState)
	{
		EmptyState->SetVisibility(Target ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	RefreshHeader(Target);
}

void UStageInspectorPanel::OnParameterRefreshed(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Info_Label)
	{
		RefreshHeader(GetInspectedObject());
	}
}

TSubclassOf<UStageParameterRowWidget> UStageInspectorPanel::ChooseRowClass(const FStageParameterDescriptor& Descriptor) const
{
	if (Descriptor.bReadOnly && ReadOnlyRowWidgetClass)
	{
		return ReadOnlyRowWidgetClass;
	}
	if (const TSubclassOf<UStageParameterRowWidget>* RowClass = RowWidgetClasses.Find(Descriptor.GetType()))
	{
		if (*RowClass)
		{
			return *RowClass;
		}
	}
	return FallbackRowWidgetClass;
}

void UStageInspectorPanel::RefreshHeader(UObject* Target)
{
	if (TitleText)
	{
		FStageParameterValue Label;
		const bool bHasLabel = Target && IStageParameterInterface::Execute_GetParameterValue(Target, StageCraftTags::Param_Info_Label, Label);
		TitleText->SetText(bHasLabel ? Label.Text : (Target ? FText::FromString(Target->GetName()) : NSLOCTEXT("StageCraftInspector", "NoSelection", "No Selection")));
	}

	if (SubtitleText)
	{
		FText Subtitle;
		if (Target && Target->Implements<UInteractableInterface>())
		{
			const FStageItemInteractionDetails Details = IInteractableInterface::Execute_GetInteractionDetails(Target);
			Subtitle = FText::Format(NSLOCTEXT("StageCraftInspector", "SubtitleFormat", "{0} | {1}"),
				StaticEnum<EStageItemType>()->GetDisplayValueAsText(Details.ItemType), Details.DisplayName);
		}
		SubtitleText->SetText(Subtitle);
	}
}
