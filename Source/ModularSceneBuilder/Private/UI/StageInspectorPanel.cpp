// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageInspectorPanel.h"

#include "Actors/ModularBaseActor.h"
#include "Components/PanelWidget.h"
#include "Components/SelectionComponent.h"
#include "Components/TextBlock.h"
#include "Interaction/InteractableInterface.h"
#include "Interaction/StageParameterInterface.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageParameterRowWidget.h"
#include "UI/StageParameterSectionWidget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageInspectorPanel)

void UStageInspectorPanel::NativeConstruct()
{
	Super::NativeConstruct();

	const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer());
	USelectionComponent* Selection = Controller ? Controller->GetSelection() : nullptr;
	if (Selection)
	{
		Selection->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionChanged);
		BoundSelection = Selection;
	}

	Inspect(Selection ? Selection->GetSelectedActor() : nullptr);
}

void UStageInspectorPanel::NativeDestruct()
{
	if (USelectionComponent* Selection = BoundSelection.Get())
	{
		Selection->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSelectionChanged);
	}
	BoundSelection.Reset();
	Inspect(nullptr);

	Super::NativeDestruct();
}

void UStageInspectorPanel::HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection)
{
	Inspect(NewSelection);
}

void UStageInspectorPanel::Inspect(UObject* Target)
{
	if (Target && !Target->Implements<UStageParameterInterface>())
	{
		Target = nullptr;
	}
	if (Target && Target == InspectedObject.Get())
	{
		return;
	}

	UnbindTarget();
	InspectedObject = Target;

	// Only AModularBaseActor exposes a change event; other implementers are shown as a snapshot.
	if (AModularBaseActor* Actor = Cast<AModularBaseActor>(Target))
	{
		Actor->OnParameterChanged.AddUniqueDynamic(this, &ThisClass::HandleParameterChanged);
		BoundActor = Actor;
	}

	Rebuild();
	BP_OnInspectedObjectChanged(Target);
}

void UStageInspectorPanel::UnbindTarget()
{
	if (AModularBaseActor* Actor = BoundActor.Get())
	{
		Actor->OnParameterChanged.RemoveDynamic(this, &ThisClass::HandleParameterChanged);
	}
	BoundActor.Reset();
}

void UStageInspectorPanel::ClearRows()
{
	for (const TPair<FGameplayTag, TObjectPtr<UStageParameterRowWidget>>& Pair : RowsById)
	{
		if (Pair.Value)
		{
			Pair.Value->OnCommitted.Unbind();
		}
	}
	RowsById.Reset();

	if (SectionContainer)
	{
		SectionContainer->ClearChildren();
	}
}

void UStageInspectorPanel::Rebuild()
{
	ClearRows();

	UObject* Target = InspectedObject.Get();
	if (EmptyState)
	{
		EmptyState->SetVisibility(Target ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	RefreshHeader();

	if (!Target || !SectionContainer)
	{
		return;
	}
	if (!SectionWidgetClass)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: SectionWidgetClass is not set; the inspector cannot show anything."), *GetName());
		return;
	}

	const TArray<FStageParameterSection> Sections = IStageParameterInterface::Execute_GetParameterSections(Target);
	for (const FStageParameterSection& Section : Sections)
	{
		if (Section.Parameters.IsEmpty())
		{
			continue;
		}

		const FLinearColor GroupColor = Theme ? Theme->GetFeatureGroupColor(Section.FeatureGroup) : FLinearColor::White;
		UStageParameterSectionWidget* SectionWidget = CreateWidget<UStageParameterSectionWidget>(this, SectionWidgetClass);
		SectionWidget->InitializeSection(Section, GroupColor);
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
			Row->InitializeRow(Descriptor, GroupColor);
			Row->OnCommitted.BindUObject(this, &ThisClass::HandleRowCommitted);
			RowsById.Add(Descriptor.Id, Row);
		}
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

void UStageInspectorPanel::RefreshHeader()
{
	UObject* Target = InspectedObject.Get();

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

void UStageInspectorPanel::HandleRowCommitted(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	UObject* Target = InspectedObject.Get();
	if (!Target)
	{
		return;
	}

	// Rejected or clamped edits snap the row back to what the object actually holds.
	IStageParameterInterface::Execute_SetParameterValue(Target, ParameterId, Value);

	FStageParameterValue Actual;
	if (IStageParameterInterface::Execute_GetParameterValue(Target, ParameterId, Actual))
	{
		if (UStageParameterRowWidget* Row = RowsById.FindRef(ParameterId))
		{
			Row->RefreshValue(Actual);
		}
	}
}

void UStageInspectorPanel::HandleParameterChanged(AModularBaseActor* Actor, FGameplayTag ParameterId)
{
	if (Actor != InspectedObject.Get())
	{
		return;
	}

	if (!ParameterId.IsValid())
	{
		Rebuild();
		return;
	}

	if (ParameterId == StageCraftTags::Param_Info_Label)
	{
		RefreshHeader();
	}

	UStageParameterRowWidget* Row = RowsById.FindRef(ParameterId);
	FStageParameterValue Value;
	if (Row && IStageParameterInterface::Execute_GetParameterValue(Actor, ParameterId, Value))
	{
		Row->RefreshValue(Value);
	}
}
