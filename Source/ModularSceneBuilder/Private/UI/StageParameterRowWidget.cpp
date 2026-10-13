// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageParameterRowWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "ModularSceneBuilder.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageWidgetTree.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterRowWidget)

void UStageParameterRowWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	ApplyColumnGrid();
}

void UStageParameterRowWidget::ApplyColumnGrid()
{
	using namespace StageCraftWidgetStyle;

	// A column is fixed by the size box it sits in: the Blueprint's own, or one put around it here.
	const auto FixColumnWidth = [this](UWidget& Column, float Width)
	{
		USizeBox* Box = Cast<USizeBox>(Column.GetParent());
		if (!Box)
		{
			Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			if (!StageWidgetTree::WrapInPlace(Column, *Box))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("%s: %s has no parent panel; the inspector column grid cannot hold it."), *GetClass()->GetName(), *Column.GetName());
				return;
			}
		}
		Box->SetWidthOverride(Width);
		Box->SetMinDesiredWidth(Width);
		Box->SetMaxDesiredWidth(Width);
	};

	if (LabelText)
	{
		LabelText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		LabelText->SetClipping(EWidgetClipping::ClipToBounds);
		FixColumnWidth(*LabelText, InspectorLabelWidth);
	}
	if (UnitsText)
	{
		FixColumnWidth(*UnitsText, InspectorUnitsWidth);
	}
	if (ValueText)
	{
		ValueText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		ValueText->SetClipping(EWidgetClipping::ClipToBounds);
		// Read-only values start where the text inside a field starts, not at the field's outline.
		const float Inset = FieldTextInset + FieldOutlineWidth;
		if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(ValueText->Slot))
		{
			const FMargin SlotPadding = BoxSlot->GetPadding();
			BoxSlot->SetPadding(FMargin(Inset, SlotPadding.Top, SlotPadding.Right, SlotPadding.Bottom));
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ValueText->Slot))
		{
			const FMargin SlotPadding = OverlaySlot->GetPadding();
			OverlaySlot->SetPadding(FMargin(Inset, SlotPadding.Top, SlotPadding.Right, SlotPadding.Bottom));
		}
	}
}

void UStageParameterRowWidget::OnInitializeControl()
{
	if (LabelText)
	{
		LabelText->SetText(Descriptor.DisplayName);
		// The label column cuts long names; the full name stays one hover away.
		LabelText->SetToolTipText(Descriptor.DisplayName);
	}
	if (UnitsText)
	{
		// Read-only rows show FormatValue, which already ends in the units ("24.5 kg"). The column stays, so fields still line up.
		UnitsText->SetText(Descriptor.bReadOnly ? FText::GetEmpty() : Descriptor.Units);
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
	if (EditorSlot && EditorSlot->GetChildrenCount() == 0)
	{
		// An empty slot would still take its share of the row and push a read-only value off the editors' left edge.
		EditorSlot->SetVisibility(ESlateVisibility::Collapsed);
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

void UStageParameterRowWidget::SetTheme(const UStageCraftUITheme* InTheme)
{
	Theme = InTheme;
}

const UStageCraftUITheme& UStageParameterRowWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

USpinBox* UStageParameterRowWidget::ConstructNumericField(const TCHAR* Name)
{
	USpinBox* Spin = WidgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass(), FName(Name));
	// Styled before it joins the live row: a widget added to a built panel builds its Slate widget at that moment.
	Spin->SetWidgetStyle(StageCraftWidgetStyle::MakeNumericField(GetTheme()));
	Spin->SetFont(StageCraftWidgetStyle::MakeFont(9));
	Spin->SetForegroundColor(GetTheme().TextPrimary);
	Spin->SetMinDesiredWidth(0.f);
	return Spin;
}

USpinBox* UStageParameterRowWidget::CreateSpinBox(const TCHAR* Name)
{
	USpinBox* Spin = ConstructNumericField(Name);
	AddToEditorSlot(WrapInFieldBox(*Spin), /*bFill*/ true);
	return Spin;
}

USpinBox* UStageParameterRowWidget::CreateComponentSpinBox(const TCHAR* Name, const FLinearColor& TagColor)
{
	USpinBox* Spin = ConstructNumericField(Name);

	// The Unreal Editor convention: a short coloured bar at the left edge names the component. A rounded box brush has no
	// size of its own, so the bar is given one; the slot stretches it to the field's height.
	FSlateBrush TagBrush = StageCraftWidgetStyle::MakeBox(FLinearColor::White, 1.f);
	TagBrush.ImageSize = FVector2D(3.0, 12.0);
	UImage* Tag = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	Tag->SetBrush(TagBrush);
	Tag->SetColorAndOpacity(TagColor);

	UOverlay* Field = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	if (UOverlaySlot* SpinSlot = Field->AddChildToOverlay(Spin))
	{
		SpinSlot->SetHorizontalAlignment(HAlign_Fill);
		SpinSlot->SetVerticalAlignment(VAlign_Fill);
	}
	if (UOverlaySlot* TagSlot = Field->AddChildToOverlay(Tag))
	{
		TagSlot->SetHorizontalAlignment(HAlign_Left);
		TagSlot->SetVerticalAlignment(VAlign_Fill);
		TagSlot->SetPadding(FMargin(1.f, 5.f, 0.f, 5.f));
	}
	AddToEditorSlot(WrapInFieldBox(*Field), /*bFill*/ true);
	return Spin;
}

UImage* UStageParameterRowWidget::CreateColorSwatch()
{
	UImage* Swatch = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ColorSwatch"));
	// Sized by its brush: a rounded box brush has no size of its own.
	const double Size = StageCraftWidgetStyle::FieldHeight - 4.f;
	FSlateBrush SwatchBrush = StageCraftWidgetStyle::MakeBox(FLinearColor::White, StageCraftWidgetStyle::CornerRadius);
	SwatchBrush.ImageSize = FVector2D(Size, Size);
	Swatch->SetBrush(SwatchBrush);
	AddToEditorSlot(*Swatch, /*bFill*/ false);
	return Swatch;
}

UWidget& UStageParameterRowWidget::WrapInFieldBox(UWidget& Editor)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
	Box->SetContent(&Editor);
	return *Box;
}

void UStageParameterRowWidget::AddToEditorSlot(UWidget& Widget, bool bFill)
{
	// Gaps go only between editors: the first one starts flush with the label column, where read-only values start too.
	const bool bFirstEditor = EditorSlot->GetChildrenCount() == 0;
	UPanelSlot* ChildSlot = EditorSlot->AddChild(&Widget);
	if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(ChildSlot))
	{
		BoxSlot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
		BoxSlot->SetVerticalAlignment(VAlign_Center);
		BoxSlot->SetPadding(FMargin(bFirstEditor ? 0.f : 4.f, 0.f, 0.f, 0.f));
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
			ColorSwatch = CreateColorSwatch();
		}
		[[fallthrough]]; // Color also uses the three component boxes.
	case EStageParameterType::Vector:
	case EStageParameterType::Rotator:
		SpinX = CreateComponentSpinBox(TEXT("SpinX"), GetTheme().AxisXColor);
		SpinY = CreateComponentSpinBox(TEXT("SpinY"), GetTheme().AxisYColor);
		SpinZ = CreateComponentSpinBox(TEXT("SpinZ"), GetTheme().AxisZColor);
		break;

	case EStageParameterType::Bool:
		ValueCheckBox = WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(), TEXT("ValueCheckBox"));
		ValueCheckBox->SetWidgetStyle(StageCraftWidgetStyle::MakeCheckBox(GetTheme()));
		AddToEditorSlot(*ValueCheckBox, /*bFill*/ false);
		break;

	case EStageParameterType::Text:
	{
		ValueTextBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("ValueTextBox"));
		FEditableTextBoxStyle TextStyle = StageCraftWidgetStyle::MakeInputField(GetTheme());
		TextStyle.SetPadding(FMargin(StageCraftWidgetStyle::FieldTextInset, 2.f));
		ValueTextBox->SetWidgetStyle(TextStyle);
		AddToEditorSlot(WrapInFieldBox(*ValueTextBox), /*bFill*/ true);
		break;
	}

	case EStageParameterType::Enum:
	{
		UStageThemedComboBox* Combo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass(), TEXT("ValueComboBox"));
		Combo->ApplyTheme(GetTheme());
		ValueComboBox = Combo;
		AddToEditorSlot(WrapInFieldBox(*Combo), /*bFill*/ true);
		break;
	}
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
		const FText Formatted = FormatValue(Descriptor, InValue);
		ValueText->SetText(Formatted);
		// A long read-only value is cut with an ellipsis; its tooltip shows it whole.
		ValueText->SetToolTipText(Formatted);
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
