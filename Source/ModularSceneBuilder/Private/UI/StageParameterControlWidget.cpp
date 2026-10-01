// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterControlWidget.h"

#include "Components/Widget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterControlWidget)

#define LOCTEXT_NAMESPACE "StageCraftParameterControl"

namespace StageParameterControl
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
		if (Units.IsEmpty())
		{
			return Value;
		}
		// Degrees attach to the number ("45.0°"); every other unit is spaced ("75.0 %", "12 dB").
		return Units.ToString() == TEXT("°")
			? FText::Format(LOCTEXT("ValueWithDegrees", "{0}{1}"), Value, Units)
			: FText::Format(LOCTEXT("ValueWithUnits", "{0} {1}"), Value, Units);
	}
}

void UStageParameterControlWidget::InitializeControl(const FStageParameterDescriptor& InDescriptor, const FLinearColor& InGroupColor)
{
	Descriptor = InDescriptor;
	GroupColor = InGroupColor;
	{
		TGuardValue<bool> Guard(bSuppressCommit, true);
		OnInitializeControl();
	}
	BP_OnControlInitialized(Descriptor, GroupColor);
	ApplyValueGuarded(/*bForce*/ true);
}

void UStageParameterControlWidget::RefreshValue(const FStageParameterValue& InValue)
{
	Descriptor.Value = InValue;
	ApplyValueGuarded(/*bForce*/ false);
}

void UStageParameterControlWidget::FinishInteraction()
{
	ApplyValueGuarded(/*bForce*/ true);
}

void UStageParameterControlWidget::ApplyValueGuarded(bool bForce)
{
	{
		TGuardValue<bool> Guard(bSuppressCommit, true);
		ApplyValue(Descriptor.Value, bForce);
	}
	BP_OnValueRefreshed(Descriptor.Value);
}

void UStageParameterControlWidget::CommitValue(const FStageParameterValue& InValue)
{
	if (bSuppressCommit || Descriptor.bReadOnly)
	{
		return;
	}
	OnCommitted.ExecuteIfBound(Descriptor.Id, InValue);
}

bool UStageParameterControlWidget::IsUserInteracting(const UWidget* Widget)
{
	return Widget && (Widget->HasMouseCapture() || Widget->HasKeyboardFocus() || Widget->HasFocusedDescendants());
}

double UStageParameterControlWidget::ToDisplay(const FStageParameterValue& InValue) const
{
	switch (InValue.Type)
	{
	case EStageParameterType::Float:	return InValue.Float * Descriptor.DisplayScale;
	case EStageParameterType::Integer:	return InValue.Integer * Descriptor.DisplayScale;
	default:							return 0.0;
	}
}

FStageParameterValue UStageParameterControlWidget::FromDisplay(double DisplayValue) const
{
	const double Stored = DisplayValue / (Descriptor.DisplayScale != 0.0 ? Descriptor.DisplayScale : 1.0);
	return Descriptor.GetType() == EStageParameterType::Integer
		? FStageParameterValue::MakeInteger(FMath::RoundToInt(Stored))
		: FStageParameterValue::MakeFloat(Stored);
}

FText UStageParameterControlWidget::FormatValue(const FStageParameterDescriptor& InDescriptor, const FStageParameterValue& InValue)
{
	using namespace StageParameterControl;

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
	case EStageParameterType::Enum:
		return InDescriptor.Options.IsValidIndex(InValue.Integer) ? InDescriptor.Options[InValue.Integer] : FText::GetEmpty();
	case EStageParameterType::Text:
	default:
		return InValue.Text;
	}
}

#undef LOCTEXT_NAMESPACE
