// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterViewWidget.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Engine/World.h"
#include "Interaction/StageParameterInterface.h"
#include "Player/ModularPlayerController.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageParameterControlWidget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterViewWidget)

void UStageParameterViewWidget::NativeConstruct()
{
	Super::NativeConstruct();

	USelectionComponent* Selection = nullptr;
	if (bFollowSelection)
	{
		const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer());
		Selection = Controller ? Controller->GetSelection() : nullptr;
		if (Selection)
		{
			Selection->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionChanged);
			BoundSelection = Selection;
		}
	}

	if (bFollowSelection)
	{
		Inspect(Selection ? Selection->GetSelectedActor() : nullptr);
	}
	else
	{
		OnViewUpdated(InspectedObject.Get());
	}
}

void UStageParameterViewWidget::NativeDestruct()
{
	if (USelectionComponent* Selection = BoundSelection.Get())
	{
		Selection->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSelectionChanged);
	}
	BoundSelection.Reset();
	bRebuildPending = false;
	Inspect(nullptr);

	Super::NativeDestruct();
}

void UStageParameterViewWidget::HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection)
{
	Inspect(NewSelection);
}

void UStageParameterViewWidget::Inspect(UObject* Target)
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

void UStageParameterViewWidget::UnbindTarget()
{
	if (AModularBaseActor* Actor = BoundActor.Get())
	{
		Actor->OnParameterChanged.RemoveDynamic(this, &ThisClass::HandleParameterChanged);
	}
	BoundActor.Reset();
}

void UStageParameterViewWidget::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (bRebuildPending || !World)
	{
		return;
	}

	bRebuildPending = true;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		if (bRebuildPending)
		{
			Rebuild();
		}
	}));
}

void UStageParameterViewWidget::Rebuild()
{
	bRebuildPending = false;

	UnregisterControls();
	ClearView();

	if (UObject* Target = InspectedObject.Get())
	{
		const TArray<FStageParameterSection> Sections = IStageParameterInterface::Execute_GetParameterSections(Target);
		BuildView(*Target, Sections);
	}

	OnViewUpdated(InspectedObject.Get());
}

void UStageParameterViewWidget::RegisterControl(UStageParameterControlWidget& Control)
{
	Control.OnCommitted.BindUObject(this, &ThisClass::HandleControlCommitted);
	Controls.Add(&Control);
}

void UStageParameterViewWidget::UnregisterControls()
{
	for (UStageParameterControlWidget* Control : Controls)
	{
		if (Control)
		{
			Control->OnCommitted.Unbind();
		}
	}
	Controls.Reset();
}

UStageParameterControlWidget* UStageParameterViewWidget::FindControl(FGameplayTag ParameterId) const
{
	for (UStageParameterControlWidget* Control : Controls)
	{
		if (Control && Control->GetParameterId() == ParameterId)
		{
			return Control;
		}
	}
	return nullptr;
}

bool UStageParameterViewWidget::CommitParameter(FGameplayTag ParameterId, const FStageParameterValue& Value)
{
	UObject* Target = InspectedObject.Get();
	if (!Target)
	{
		return false;
	}

	const bool bAccepted = IStageParameterInterface::Execute_SetParameterValue(Target, ParameterId, Value);

	// Read back even when a change event already fired: an edit that clamps to the current value
	// fires nothing, and the control must not keep showing the out-of-range number the user typed.
	if (InspectedObject.Get() == Target)
	{
		RefreshControls(ParameterId);
	}
	return bAccepted;
}

void UStageParameterViewWidget::HandleControlCommitted(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	CommitParameter(ParameterId, Value);
}

void UStageParameterViewWidget::RefreshControls(const FGameplayTag& ParameterId)
{
	UObject* Target = InspectedObject.Get();
	FStageParameterValue Value;
	if (!Target || !IStageParameterInterface::Execute_GetParameterValue(Target, ParameterId, Value))
	{
		return;
	}

	// Copy: a control's refresh hook could, in principle, trigger a rebuild that resets Controls.
	const TArray<TObjectPtr<UStageParameterControlWidget>> Snapshot = Controls;
	for (UStageParameterControlWidget* Control : Snapshot)
	{
		if (Control && Control->GetParameterId() == ParameterId)
		{
			Control->RefreshValue(Value);
		}
	}
	OnParameterRefreshed(ParameterId, Value);
}

void UStageParameterViewWidget::HandleParameterChanged(AModularBaseActor* Actor, FGameplayTag ParameterId)
{
	if (Actor != InspectedObject.Get())
	{
		return;
	}

	if (!ParameterId.IsValid())
	{
		// Structure changed (e.g. item type swapped from this very panel's dropdown): never rebuild
		// inside the callback chain of a widget that is about to be destroyed.
		RequestRebuild();
		return;
	}

	RefreshControls(ParameterId);
}

FLinearColor UStageParameterViewWidget::GetGroupColor(const FGameplayTag& FeatureGroup) const
{
	return Theme ? Theme->GetFeatureGroupColor(FeatureGroup) : FLinearColor(0.85f, 0.55f, 0.1f);
}
