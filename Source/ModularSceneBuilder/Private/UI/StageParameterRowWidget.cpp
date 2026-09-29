// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterRowWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterRowWidget)

#define LOCTEXT_NAMESPACE "StageCraftParameterRow"

namespace
{
	FText FormatNumber(double Value, int32 FractionalDigits)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = FractionalDigits;
		Options.MaximumFractionalDigits = FractionalDigits;
		return FText::AsNumber(Value, &Options);
	}

	FText WithUnits(const FText& Value, const FText& Units)
	{
		return Units.IsEmpty() ? Value : FText::Format(LOCTEXT("ValueWithUnits", "{0} {1}"), Value, Units);
	}
}

void UStageParameterRowWidget::InitializeRow(const FStageParameterDescriptor& InDescriptor, const FLinearColor& InGroupColor)
{
	Descriptor = InDescriptor;
	TGuardValue<bool> Guard(bSuppressCommit, true);

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
		GroupColorStrip->SetColorAndOpacity(InGroupColor);
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
		}
		break;

	case EStageParameterType::Vector:
	case EStageParameterType::Rotator:
		for (USpinBox* Spin : { SpinX.Get(), SpinY.Get(), SpinZ.Get() })
		{
			if (Spin)
			{
				ConfigureSpinBox(*Spin, /*bUseRange*/ false);
				Spin->SetIsEnabled(bEditable);
				Spin->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinChanged);
				Spin->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinCommitted);
			}
		}
		break;

	case EStageParameterType::Color:
		// Interim editor: R/G/B in percent. A color wheel replaces this by calling CommitValue.
		for (USpinBox* Spin : { SpinX.Get(), SpinY.Get(), SpinZ.Get() })
		{
			if (Spin)
			{
				Spin->SetMinFractionalDigits(0);
				Spin->SetMaxFractionalDigits(1);
				Spin->SetMinValue(0.f);
				Spin->SetMaxValue(100.f);
				Spin->SetMinSliderValue(0.f);
				Spin->SetMaxSliderValue(100.f);
				Spin->SetIsEnabled(bEditable);
				Spin->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinChanged);
				Spin->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleComponentSpinCommitted);
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

	default:
		break;
	}

	BP_OnRowInitialized(Descriptor, InGroupColor);
	RefreshValue(Descriptor.Value);
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

	default:
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

void UStageParameterRowWidget::SetSpinValue(USpinBox* SpinBox, double StoredValue)
{
	if (SpinBox)
	{
		SpinBox->SetValue(static_cast<float>(StoredValue * Descriptor.DisplayScale));
	}
}

void UStageParameterRowWidget::RefreshValue(const FStageParameterValue& InValue)
{
	Descriptor.Value = InValue;
	TGuardValue<bool> Guard(bSuppressCommit, true);

	switch (InValue.Type)
	{
	case EStageParameterType::Float:
		SetSpinValue(ValueSpinBox, InValue.Float);
		break;
	case EStageParameterType::Integer:
		SetSpinValue(ValueSpinBox, InValue.Integer);
		break;
	case EStageParameterType::Vector:
		SetSpinValue(SpinX, InValue.Vector.X);
		SetSpinValue(SpinY, InValue.Vector.Y);
		SetSpinValue(SpinZ, InValue.Vector.Z);
		break;
	case EStageParameterType::Rotator:
		// Displayed as roll / pitch / yaw around X / Y / Z, matching the gizmo's rings.
		SetSpinValue(SpinX, InValue.Rotator.Roll);
		SetSpinValue(SpinY, InValue.Rotator.Pitch);
		SetSpinValue(SpinZ, InValue.Rotator.Yaw);
		break;
	case EStageParameterType::Bool:
		if (ValueCheckBox)
		{
			ValueCheckBox->SetIsChecked(InValue.bBool);
		}
		break;
	case EStageParameterType::Text:
		if (ValueTextBox && !ValueTextBox->HasKeyboardFocus())
		{
			ValueTextBox->SetText(InValue.Text);
		}
		break;
	case EStageParameterType::Color:
		if (ColorSwatch)
		{
			ColorSwatch->SetColorAndOpacity(FLinearColor(InValue.Color.R, InValue.Color.G, InValue.Color.B, 1.f));
		}
		if (SpinX) { SpinX->SetValue(InValue.Color.R * 100.f); }
		if (SpinY) { SpinY->SetValue(InValue.Color.G * 100.f); }
		if (SpinZ) { SpinZ->SetValue(InValue.Color.B * 100.f); }
		break;
	}

	if (ValueText)
	{
		ValueText->SetText(FormatValue(Descriptor, InValue));
	}

	BP_OnValueRefreshed(InValue);
}

void UStageParameterRowWidget::CommitValue(const FStageParameterValue& InValue)
{
	if (bSuppressCommit || Descriptor.bReadOnly)
	{
		return;
	}
	OnCommitted.ExecuteIfBound(Descriptor.Id, InValue);
}

void UStageParameterRowWidget::HandleSpinValueChanged(float InValue)
{
	const double Stored = InValue / Descriptor.DisplayScale;
	CommitValue(Descriptor.GetType() == EStageParameterType::Integer
		? FStageParameterValue::MakeInteger(FMath::RoundToInt(Stored))
		: FStageParameterValue::MakeFloat(Stored));
}

void UStageParameterRowWidget::HandleSpinValueCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	HandleSpinValueChanged(InValue);
}

void UStageParameterRowWidget::HandleComponentSpinChanged(float InValue)
{
	CommitComponents();
}

void UStageParameterRowWidget::HandleComponentSpinCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	CommitComponents();
}

void UStageParameterRowWidget::CommitComponents()
{
	// Unbound component boxes keep the last known value, so a row may show only some axes.
	auto Read = [this](const USpinBox* Spin, double Fallback)
	{
		return Spin ? Spin->GetValue() / Descriptor.DisplayScale : Fallback;
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
	}
}

FText UStageParameterRowWidget::FormatValue(const FStageParameterDescriptor& InDescriptor, const FStageParameterValue& InValue)
{
	const double Scale = InDescriptor.DisplayScale;
	switch (InValue.Type)
	{
	case EStageParameterType::Float:
		return WithUnits(FormatNumber(InValue.Float * Scale, 1), InDescriptor.Units);
	case EStageParameterType::Integer:
		return WithUnits(FText::AsNumber(InValue.Integer), InDescriptor.Units);
	case EStageParameterType::Bool:
		return InValue.bBool ? LOCTEXT("On", "On") : LOCTEXT("Off", "Off");
	case EStageParameterType::Vector:
		return WithUnits(FText::Format(LOCTEXT("Vector", "({0}, {1}, {2})"),
			FormatNumber(InValue.Vector.X * Scale, 0), FormatNumber(InValue.Vector.Y * Scale, 0), FormatNumber(InValue.Vector.Z * Scale, 0)), InDescriptor.Units);
	case EStageParameterType::Rotator:
		return WithUnits(FText::Format(LOCTEXT("Rotator", "({0}, {1}, {2})"),
			FormatNumber(InValue.Rotator.Roll, 1), FormatNumber(InValue.Rotator.Pitch, 1), FormatNumber(InValue.Rotator.Yaw, 1)), InDescriptor.Units);
	case EStageParameterType::Color:
		return FText::FromString(InValue.Color.ToFColor(/*bSRGB*/ true).ToHex().Left(6));
	case EStageParameterType::Text:
	default:
		return InValue.Text;
	}
}

#undef LOCTEXT_NAMESPACE
