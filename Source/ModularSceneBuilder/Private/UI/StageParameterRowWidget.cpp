// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterRowWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterRowWidget)

void UStageParameterRowWidget::OnInitializeControl()
{
	if (LabelText)
	{
		LabelText->SetText(Descriptor.DisplayName);
	}
	if (UnitsText)
	{
		UnitsText->SetText(Descriptor.Units);
	}
	if (GroupColorStrip)
	{
		GroupColorStrip->SetColorAndOpacity(GroupColor);
	}

	const bool bEditable = !Descriptor.bReadOnly;
	if (bEditable)
	{
		CreateMissingEditors();
	}
	if (ValueText)
	{
		ValueText->SetVisibility(bEditable && HasEditor() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	switch (Descriptor.GetType())
	{
	case EStageParameterType::Float:
	case EStageParameterType::Integer:
		if (ValueSpinBox)
		{
			ConfigureSpinBox(*ValueSpinBox, /*bUseRange*/ true);
			ValueSpinBox->SetIsEnabled(bEditable);
			ValueSpinBox->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleSpinValueChanged);
			ValueSpinBox->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleSpinValueCommitted);
			ValueSpinBox->OnEndSliderMovement.AddUniqueDynamic(this, &ThisClass::HandleSpinEndSliderMovement);
		}
		break;

	case EStageParameterType::Vector:
	case EStageParameterType::Rotator:
	case EStageParameterType::Color:
		for (USpinBox* Spin : { SpinX.Get(), SpinY.Get(), SpinZ.Get() })
		{
			if (Spin)
			{
				if (Descriptor.GetType() == EStageParameterType::Color)
				{
					ConfigurePercentSpinBox(*Spin);
				}
				else
				{
					ConfigureSpinBox(*Spin, /*bUseRange*/ false);
				}
				Spin->SetIsEnabled(bEditable);
				Spin->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinChanged);
				Spin->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinCommitted);
				Spin->OnEndSliderMovement.AddUniqueDynamic(this, &ThisClass::HandleSpinEndSliderMovement);
			}
		}
		break;

	case EStageParameterType::Bool:
		if (ValueCheckBox)
		{
			ValueCheckBox->SetIsEnabled(bEditable);
			ValueCheckBox->OnCheckStateChanged.AddUniqueDynamic(this, &ThisClass::HandleCheckStateChanged);
		}
		break;

	case EStageParameterType::Text:
		if (ValueTextBox)
		{
			ValueTextBox->SetIsReadOnly(!bEditable);
			ValueTextBox->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleTextCommitted);
		}
		break;

	case EStageParameterType::Enum:
		if (ValueComboBox)
		{
			ValueComboBox->ClearOptions();
			for (const FText& Option : Descriptor.Options)
			{
				ValueComboBox->AddOption(Option.ToString());
			}
			ValueComboBox->SetIsEnabled(bEditable && Descriptor.Options.Num() > 1);
			ValueComboBox->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleComboSelectionChanged);
		}
		break;
	}
}

USpinBox* UStageParameterRowWidget::CreateSpinBox(const TCHAR* Name)
{
	USpinBox* Spin = WidgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass(), FName(Name));
	AddToEditorSlot(*Spin, /*bFill*/ true);
	return Spin;
}

void UStageParameterRowWidget::AddToEditorSlot(UWidget& Widget, bool bFill)
{
	UPanelSlot* ChildSlot = EditorSlot->AddChild(&Widget);
	if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(ChildSlot))
	{
		BoxSlot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
		BoxSlot->SetVerticalAlignment(VAlign_Center);
		BoxSlot->SetPadding(FMargin(2.f, 0.f));
	}
}

bool UStageParameterRowWidget::HasEditor() const
{
	switch (Descriptor.GetType())
	{
	case EStageParameterType::Float:
	case EStageParameterType::Integer:	return ValueSpinBox != nullptr;
	case EStageParameterType::Vector:
	case EStageParameterType::Rotator:
	case EStageParameterType::Color:	return SpinX || SpinY || SpinZ;
	case EStageParameterType::Bool:		return ValueCheckBox != nullptr;
	case EStageParameterType::Text:		return ValueTextBox != nullptr;
	case EStageParameterType::Enum:		return ValueComboBox != nullptr;
	default:							return false;
	}
}

void UStageParameterRowWidget::CreateMissingEditors()
{
	if (!EditorSlot || !WidgetTree || HasEditor())
	{
		return;
	}

	switch (Descriptor.GetType())
	{
	case EStageParameterType::Float:
	case EStageParameterType::Integer:
		ValueSpinBox = CreateSpinBox(TEXT("ValueSpinBox"));
		break;

	case EStageParameterType::Color:
		if (!ColorSwatch)
		{
			ColorSwatch = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ColorSwatch"));
			ColorSwatch->SetDesiredSizeOverride(FVector2D(18.0, 18.0));
			AddToEditorSlot(*ColorSwatch, /*bFill*/ false);
		}
		[[fallthrough]]; // Color also uses the three component boxes.
	case EStageParameterType::Vector:
	case EStageParameterType::Rotator:
		SpinX = CreateSpinBox(TEXT("SpinX"));
		SpinY = CreateSpinBox(TEXT("SpinY"));
		SpinZ = CreateSpinBox(TEXT("SpinZ"));
		break;

	case EStageParameterType::Bool:
		ValueCheckBox = WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(), TEXT("ValueCheckBox"));
		AddToEditorSlot(*ValueCheckBox, /*bFill*/ false);
		break;

	case EStageParameterType::Text:
		ValueTextBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("ValueTextBox"));
		AddToEditorSlot(*ValueTextBox, /*bFill*/ true);
		break;

	case EStageParameterType::Enum:
		ValueComboBox = WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass(), TEXT("ValueComboBox"));
		AddToEditorSlot(*ValueComboBox, /*bFill*/ true);
		break;
	}
}

void UStageParameterRowWidget::ConfigureSpinBox(USpinBox& SpinBox, bool bUseRange) const
{
	const bool bInteger = Descriptor.GetType() == EStageParameterType::Integer;
	const double Scale = Descriptor.DisplayScale;

	SpinBox.SetMinFractionalDigits(bInteger ? 0 : 1);
	SpinBox.SetMaxFractionalDigits(bInteger ? 0 : 2);
	if (bInteger)
	{
		SpinBox.SetDelta(1.f);
		SpinBox.SetAlwaysUsesDeltaSnap(true);
	}
	else if (Descriptor.Step > 0.0)
	{
		SpinBox.SetDelta(static_cast<float>(Descriptor.Step));
	}

	if (bUseRange && Descriptor.HasRange())
	{
		const float DisplayMin = static_cast<float>(Descriptor.Min * Scale);
		const float DisplayMax = static_cast<float>(Descriptor.Max * Scale);
		SpinBox.SetMinValue(DisplayMin);
		SpinBox.SetMaxValue(DisplayMax);
		SpinBox.SetMinSliderValue(DisplayMin);
		SpinBox.SetMaxSliderValue(DisplayMax);
	}
}

void UStageParameterRowWidget::ConfigurePercentSpinBox(USpinBox& SpinBox) const
{
	SpinBox.SetMinFractionalDigits(0);
	SpinBox.SetMaxFractionalDigits(1);
	SpinBox.SetMinValue(0.f);
	SpinBox.SetMaxValue(100.f);
	SpinBox.SetMinSliderValue(0.f);
	SpinBox.SetMaxSliderValue(100.f);
}

void UStageParameterRowWidget::SetSpinValue(USpinBox* SpinBox, double DisplayValue, bool bForce)
{
	if (SpinBox && (bForce || !IsUserInteracting(SpinBox)))
	{
		SpinBox->SetValue(static_cast<float>(DisplayValue));
	}
}

void UStageParameterRowWidget::ApplyValue(const FStageParameterValue& InValue, bool bForce)
{
	const double Scale = Descriptor.DisplayScale;

	switch (InValue.Type)
	{
	case EStageParameterType::Float:
	case EStageParameterType::Integer:
		SetSpinValue(ValueSpinBox, ToDisplay(InValue), bForce);
		break;
	case EStageParameterType::Vector:
		SetSpinValue(SpinX, InValue.Vector.X * Scale, bForce);
		SetSpinValue(SpinY, InValue.Vector.Y * Scale, bForce);
		SetSpinValue(SpinZ, InValue.Vector.Z * Scale, bForce);
		break;
	case EStageParameterType::Rotator:
		// Displayed as roll / pitch / yaw around X / Y / Z, matching the gizmo's rings.
		SetSpinValue(SpinX, InValue.Rotator.Roll * Scale, bForce);
		SetSpinValue(SpinY, InValue.Rotator.Pitch * Scale, bForce);
		SetSpinValue(SpinZ, InValue.Rotator.Yaw * Scale, bForce);
		break;
	case EStageParameterType::Color:
		if (ColorSwatch)
		{
			ColorSwatch->SetColorAndOpacity(FLinearColor(InValue.Color.R, InValue.Color.G, InValue.Color.B, 1.f));
		}
		SetSpinValue(SpinX, InValue.Color.R * 100.0, bForce);
		SetSpinValue(SpinY, InValue.Color.G * 100.0, bForce);
		SetSpinValue(SpinZ, InValue.Color.B * 100.0, bForce);
		break;
	case EStageParameterType::Bool:
		if (ValueCheckBox)
		{
			ValueCheckBox->SetIsChecked(InValue.bBool);
		}
		break;
	case EStageParameterType::Text:
		if (ValueTextBox && (bForce || !IsUserInteracting(ValueTextBox)))
		{
			ValueTextBox->SetText(InValue.Text);
		}
		break;
	case EStageParameterType::Enum:
		// An open dropdown is left alone; the selection it is about to make wins.
		if (ValueComboBox && (bForce || !ValueComboBox->IsOpen()) && ValueComboBox->GetSelectedIndex() != InValue.Integer)
		{
			ValueComboBox->SetSelectedIndex(InValue.Integer);
		}
		break;
	}

	if (ValueText)
	{
		ValueText->SetText(FormatValue(Descriptor, InValue));
	}
}

void UStageParameterRowWidget::HandleSpinValueChanged(float InValue)
{
	CommitValue(FromDisplay(InValue));
}

void UStageParameterRowWidget::HandleSpinValueCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	HandleSpinValueChanged(InValue);
	FinishInteraction();
}

void UStageParameterRowWidget::HandleSpinEndSliderMovement(float InValue)
{
	FinishInteraction();
}

void UStageParameterRowWidget::HandleComponentSpinChanged(float InValue)
{
	CommitComponents();
}

void UStageParameterRowWidget::HandleComponentSpinCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	CommitComponents();
	FinishInteraction();
}

void UStageParameterRowWidget::CommitComponents()
{
	// Unbound component boxes keep the last known value, so a row may show only some axes.
	const double Scale = Descriptor.DisplayScale != 0.0 ? Descriptor.DisplayScale : 1.0;
	auto Read = [Scale](const USpinBox* Spin, double Fallback)
	{
		return Spin ? Spin->GetValue() / Scale : Fallback;
	};

	if (Descriptor.GetType() == EStageParameterType::Vector)
	{
		const FVector& Current = Descriptor.Value.Vector;
		CommitValue(FStageParameterValue::MakeVector(FVector(Read(SpinX, Current.X), Read(SpinY, Current.Y), Read(SpinZ, Current.Z))));
	}
	else if (Descriptor.GetType() == EStageParameterType::Rotator)
	{
		const FRotator& Current = Descriptor.Value.Rotator;
		CommitValue(FStageParameterValue::MakeRotator(FRotator(Read(SpinY, Current.Pitch), Read(SpinZ, Current.Yaw), Read(SpinX, Current.Roll))));
	}
	else if (Descriptor.GetType() == EStageParameterType::Color)
	{
		const FLinearColor& Current = Descriptor.Value.Color;
		auto ReadPercent = [](const USpinBox* Spin, float Fallback) { return Spin ? Spin->GetValue() / 100.f : Fallback; };
		CommitValue(FStageParameterValue::MakeColor(FLinearColor(ReadPercent(SpinX, Current.R), ReadPercent(SpinY, Current.G), ReadPercent(SpinZ, Current.B), 1.f)));
	}
}

void UStageParameterRowWidget::HandleCheckStateChanged(bool bIsChecked)
{
	CommitValue(FStageParameterValue::MakeBool(bIsChecked));
}

void UStageParameterRowWidget::HandleTextCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnCleared)
	{
		CommitValue(FStageParameterValue::MakeText(InText));
		FinishInteraction();
	}
}

void UStageParameterRowWidget::HandleComboSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	// Direct = set from code (ApplyValue); only user picks are edits.
	if (SelectionType != ESelectInfo::Direct && ValueComboBox)
	{
		CommitValue(FStageParameterValue::MakeEnum(ValueComboBox->GetSelectedIndex()));
	}
}
