// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageEncoderWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Clipping.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEncoderWidget)

#define LOCTEXT_NAMESPACE "StageCraftEncoder"

namespace StageEncoder
{
	const FLinearColor StripBackground(0.018f, 0.018f, 0.022f, 1.f);
	const FLinearColor TextColor(0.82f, 0.82f, 0.85f, 1.f);
	const FLinearColor RingColor(0.12f, 0.12f, 0.14f, 1.f);
	const FLinearColor RingHoverColor(0.22f, 0.22f, 0.26f, 1.f);
	const FLinearColor PopupBackground(0.03f, 0.03f, 0.035f, 0.95f);

	// Dial sweep like a console encoder ring: 270 degrees, gap at the bottom. Angles in radians,
	// measured clockwise from 12 o'clock (Slate's Y axis points down).
	constexpr float SweepStart = -0.75f * UE_PI;
	constexpr float Sweep = 1.5f * UE_PI;
	constexpr int32 ArcSegments = 48;

	// Neighbouring strips paint from the same base layer as this one, so the popup needs headroom
	// above everything a strip draws to stay on top of them.
	constexpr int32 PopupLayerOffset = 100;
	constexpr float PopupGap = 4.f;
	const FVector2f PopupPadding(7.f, 3.f);

	FVector2D PointOnCircle(const FVector2D& Center, float Radius, float Angle)
	{
		return Center + FVector2D(FMath::Sin(Angle), -FMath::Cos(Angle)) * Radius;
	}

	void AppendArc(TArray<FVector2D>& Points, const FVector2D& Center, float Radius, float From, float To)
	{
		const int32 Segments = FMath::Max(2, FMath::CeilToInt(ArcSegments * FMath::Abs(To - From) / Sweep));
		for (int32 Index = 0; Index <= Segments; ++Index)
		{
			Points.Add(PointOnCircle(Center, Radius, FMath::Lerp(From, To, static_cast<float>(Index) / Segments)));
		}
	}

	void StyleText(UTextBlock& Text, int32 Size)
	{
		FSlateFontInfo Font = Text.GetFont();
		Font.Size = Size;
		Text.SetFont(Font);
		Text.SetColorAndOpacity(FSlateColor(TextColor));
		Text.SetJustification(ETextJustify::Center);
	}

	const FSlateBrush& PopupBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 4.f);
		return Brush;
	}
}

void UStageEncoderWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	SetVisibility(ESlateVisibility::Visible); // Mouse drag and wheel anywhere on the strip.

	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
	}
}

void UStageEncoderWidget::BuildDefaultTree()
{
	using namespace StageEncoder;

	USizeBox* Root = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("StripSize"));
	Root->SetWidthOverride(StripWidth);
	WidgetTree->RootWidget = Root;

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StripBackground"));
	Background->SetBrushColor(StripBackground);
	Background->SetPadding(FMargin(2.f, 5.f, 2.f, 4.f));
	Root->AddChild(Background);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StripColumn"));
	Background->SetContent(Column);

	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NameText"));
	StyleText(*NameText, 8);
	Column->AddChildToVerticalBox(NameText)->SetHorizontalAlignment(HAlign_Fill);

	USizeBox* DialBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DialSize"));
	DialBox->SetWidthOverride(DialSize);
	DialBox->SetHeightOverride(DialSize);
	UVerticalBoxSlot* DialSlot = Column->AddChildToVerticalBox(DialBox);
	DialSlot->SetHorizontalAlignment(HAlign_Center);
	DialSlot->SetPadding(FMargin(0.f, 4.f));

	DialArea = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), TEXT("DialArea"));
	DialBox->AddChild(DialArea);

	ValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ValueText"));
	StyleText(*ValueText, 8);
	Column->AddChildToVerticalBox(ValueText)->SetHorizontalAlignment(HAlign_Fill);
}

void UStageEncoderWidget::OnInitializeControl()
{
	if (NameText)
	{
		NameText->SetText(Descriptor.DisplayName);
	}
	SetIsEnabled(!Descriptor.bReadOnly);
	SetCursor(CanEdit() ? EMouseCursor::ResizeUpDown : EMouseCursor::Default); // Says "drag vertically".

	// A new parameter (rebuild, type swap) snaps the needle instead of sweeping from the old one.
	DisplayedNormalized = -1.f;
}

void UStageEncoderWidget::ApplyValue(const FStageParameterValue& InValue, bool bForce)
{
	// The dial is painted every frame; only the readout needs pushing. It is never "being typed
	// into", so it always shows the live value, also during a drag.
	if (ValueText)
	{
		ValueText->SetText(FormatValue(Descriptor, InValue));
	}
}

bool UStageEncoderWidget::CanEdit() const
{
	const EStageParameterType Type = Descriptor.GetType();
	return !Descriptor.bReadOnly && Descriptor.HasRange() && (Type == EStageParameterType::Float || Type == EStageParameterType::Integer);
}

void UStageEncoderWidget::GetDisplayRange(double& OutMin, double& OutMax) const
{
	OutMin = Descriptor.Min * Descriptor.DisplayScale;
	OutMax = Descriptor.Max * Descriptor.DisplayScale;
	if (OutMin > OutMax)
	{
		Swap(OutMin, OutMax);
	}
}

float UStageEncoderWidget::GetNormalizedValue() const
{
	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	return Max > Min ? static_cast<float>(FMath::Clamp((ToDisplay(Descriptor.Value) - Min) / (Max - Min), 0.0, 1.0)) : 0.f;
}

FText UStageEncoderWidget::GetPopupText() const
{
	const FText Value = FormatValue(Descriptor, Descriptor.Value);
	if (IsDragging() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetModifierKeys().IsShiftDown())
	{
		return FText::Format(LOCTEXT("PopupFine", "{0}: {1}  fine"), Descriptor.DisplayName, Value);
	}
	return FText::Format(LOCTEXT("Popup", "{0}: {1}"), Descriptor.DisplayName, Value);
}

void UStageEncoderWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Exponential easing, alpha = 1 - e^(-k dt): identical feel at any frame rate.
	const float Target = GetNormalizedValue();
	if (DisplayedNormalized < 0.f || NeedleSmoothing <= 0.f || FMath::IsNearlyEqual(DisplayedNormalized, Target, 1.e-4f))
	{
		DisplayedNormalized = Target;
	}
	else
	{
		DisplayedNormalized = FMath::Lerp(DisplayedNormalized, Target, 1.f - FMath::Exp(-NeedleSmoothing * InDeltaTime));
	}

	PopupTimeRemaining = FMath::Max(0.f, PopupTimeRemaining - InDeltaTime);
}

int32 UStageEncoderWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace StageEncoder;

	LayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	// Paint over DialArea when it exists, else over the whole widget.
	const FGeometry DialGeometry = DialArea ? DialArea->GetPaintSpaceGeometry() : AllottedGeometry;
	const FVector2D Size = DialGeometry.GetLocalSize();
	if (Size.X < 4.0 || Size.Y < 4.0)
	{
		return LayerId;
	}

	const bool bActive = IsDragging();
	const bool bHighlighted = bActive || (bHovered && CanEdit());
	const FVector2D Center = Size * 0.5;
	const float Radius = static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.5 - 3.0);
	const float Normalized = DisplayedNormalized >= 0.f ? DisplayedNormalized : GetNormalizedValue();
	const float ValueAngle = SweepStart + Sweep * Normalized;
	const float Dim = bParentEnabled && GetIsEnabled() ? 1.f : 0.4f;
	const FPaintGeometry PaintGeometry = DialGeometry.ToPaintGeometry();

	TArray<FVector2D> Ring;
	AppendArc(Ring, Center, Radius, SweepStart, SweepStart + Sweep);
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, Ring, ESlateDrawEffect::None,
		(bHighlighted ? RingHoverColor : RingColor) * Dim, /*bAntialias*/ true, 4.f);

	TArray<FVector2D> ValueArc;
	AppendArc(ValueArc, Center, Radius, SweepStart, ValueAngle);
	if (bActive)
	{
		// Soft halo under the arc while dragging.
		FLinearColor Halo = GroupColor;
		Halo.A = 0.3f;
		FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, ValueArc, ESlateDrawEffect::None, Halo * Dim, true, 9.f);
	}
	const FLinearColor ArcColor = bHighlighted ? FMath::Lerp(GroupColor, FLinearColor::White, 0.25f) : GroupColor;
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, ValueArc, ESlateDrawEffect::None, ArcColor * Dim, true, bActive ? 5.f : 4.f);

	const TArray<FVector2D> Pointer = { PointOnCircle(Center, Radius * 0.25f, ValueAngle), PointOnCircle(Center, Radius - 5.f, ValueAngle) };
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, Pointer, ESlateDrawEffect::None, FLinearColor::White * Dim, true, bActive ? 2.5f : 2.f);

	if (IsPopupVisible())
	{
		PaintPopup(AllottedGeometry, DialGeometry, OutDrawElements, LayerId);
	}

	return LayerId;
}

void UStageEncoderWidget::PaintPopup(const FGeometry& AllottedGeometry, const FGeometry& DialGeometry, FSlateWindowElementList& OutDrawElements, int32 LayerId) const
{
	using namespace StageEncoder;

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	const FString Text = GetPopupText().ToString();
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FVector2f TextSize = FVector2f(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font));
	const FVector2f PopupSize = TextSize + PopupPadding * 2.f;

	// Centered on the dial, just above the strip (the bank sits at the bottom of the screen).
	const FVector2f DialCenter = FVector2f(AllottedGeometry.AbsoluteToLocal(DialGeometry.LocalToAbsolute(DialGeometry.GetLocalSize() * 0.5f)));
	const FVector2f Offset(DialCenter.X - PopupSize.X * 0.5f, -PopupSize.Y - PopupGap);
	const FGeometry PopupGeometry = AllottedGeometry.MakeChild(PopupSize, FSlateLayoutTransform(Offset));

	// Escape the ancestors' clip rects (the bank's scroll box would cut the popup off) by clipping to
	// the popup alone, without intersecting the parent zone.
	FSlateClippingZone PopupZone(PopupGeometry);
	PopupZone.SetShouldIntersectParent(false);
	OutDrawElements.PushClip(PopupZone);

	const int32 PopupLayer = LayerId + PopupLayerOffset;
	FSlateDrawElement::MakeBox(OutDrawElements, PopupLayer, PopupGeometry.ToPaintGeometry(), &PopupBrush(), ESlateDrawEffect::None, GroupColor);

	const FGeometry InnerGeometry = PopupGeometry.MakeChild(PopupSize - FVector2f(2.f, 2.f), FSlateLayoutTransform(FVector2f(1.f, 1.f)));
	FSlateDrawElement::MakeBox(OutDrawElements, PopupLayer + 1, InnerGeometry.ToPaintGeometry(), &PopupBrush(), ESlateDrawEffect::None, PopupBackground);

	const FGeometry TextGeometry = PopupGeometry.MakeChild(TextSize, FSlateLayoutTransform(PopupPadding));
	FSlateDrawElement::MakeText(OutDrawElements, PopupLayer + 2, TextGeometry.ToPaintGeometry(), Text, Font, ESlateDrawEffect::None, FLinearColor::White);

	OutDrawElements.PopClip();
}

FReply UStageEncoderWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !CanEdit())
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}

	// Nothing changes yet: a press is a click until it travels past the drag threshold.
	DragState = EDragState::Pending;
	PendingTravel = FVector2D::ZeroVector;
	return FReply::Handled().CaptureMouse(TakeWidget());
}

FReply UStageEncoderWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (DragState == EDragState::None || !HasMouseCapture())
	{
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}

	const FVector2D Delta = InMouseEvent.GetCursorDelta();

	if (DragState == EDragState::Pending)
	{
		PendingTravel += Delta;
		if (PendingTravel.Size() < FSlateApplication::Get().GetDragTriggerDistance())
		{
			return FReply::Handled();
		}

		// The threshold travel is deliberately not applied: the drag starts from the exact current
		// value, so crossing the threshold never jumps it.
		DragState = EDragState::Dragging;
		double Min = 0.0, Max = 1.0;
		GetDisplayRange(Min, Max);
		DragValue = FMath::Clamp(ToDisplay(Descriptor.Value), Min, Max);
		// The OS cursor itself, not the (possibly fractional) event position, so it returns pixel-exact.
		const FVector2D CursorPosition = FSlateApplication::Get().GetCursorPos();
		DragAnchorScreenPosition = FIntPoint(FMath::RoundToInt(CursorPosition.X), FMath::RoundToInt(CursorPosition.Y));
		PopupTimeRemaining = 0.f;

		// High-precision mode needs a capture request in the same reply. Re-capturing releases our
		// own capture first, which reports a capture loss that must not end this drag.
		bIgnoreNextCaptureLost = true;
		const TSharedRef<SWidget> Self = TakeWidget();
		return FReply::Handled().CaptureMouse(Self).UseHighPrecisionMouseMovement(Self);
	}

	bIgnoreNextCaptureLost = false;

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	const double PerPixel = (Max - Min) / PixelsPerFullRange * (InMouseEvent.IsShiftDown() ? FineScale : 1.0);

	// Up increases (Slate's Y grows downward). The accumulator is clamped every step, so pushing
	// past a limit builds no dead zone and the first move back responds at once.
	const double NewValue = FMath::Clamp(DragValue - Delta.Y * PerPixel, Min, Max);
	if (NewValue != DragValue)
	{
		DragValue = NewValue;
		CommitValue(FromDisplay(DragValue));
	}
	return FReply::Handled();
}

FReply UStageEncoderWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (DragState == EDragState::None || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	return EndInteraction(/*bReleaseCapture*/ true);
}

FReply UStageEncoderWidget::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !ResetToDefault())
	{
		return Super::NativeOnMouseButtonDoubleClick(InGeometry, InMouseEvent);
	}
	return FReply::Handled();
}

void UStageEncoderWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	if (bIgnoreNextCaptureLost)
	{
		bIgnoreNextCaptureLost = false;
	}
	else
	{
		EndInteraction(/*bReleaseCapture*/ false);
	}
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
}

FReply UStageEncoderWidget::EndInteraction(bool bReleaseCapture)
{
	const bool bWasDragging = DragState == EDragState::Dragging;
	const bool bWasActive = DragState != EDragState::None;
	DragState = EDragState::None;
	bIgnoreNextCaptureLost = false;

	if (bWasDragging)
	{
		ShowPopupBriefly(); // Let the final value read for a moment after release.
		FinishInteraction();
	}

	if (!bReleaseCapture || !bWasActive)
	{
		return FReply::Handled();
	}

	FReply Reply = FReply::Handled().ReleaseMouseCapture();
	if (bWasDragging)
	{
		// The cursor was hidden and parked during the drag: show it again exactly where it vanished.
		Reply.SetMousePos(DragAnchorScreenPosition);
	}
	return Reply;
}

bool UStageEncoderWidget::ResetToDefault()
{
	if (!CanEdit())
	{
		return false;
	}

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	const double Default = Descriptor.bHasDefault ? ToDisplay(Descriptor.DefaultValue) : 0.0;
	CommitValue(FromDisplay(FMath::Clamp(Default, Min, Max)));
	ShowPopupBriefly();
	return true;
}

void UStageEncoderWidget::ShowPopupBriefly()
{
	PopupTimeRemaining = PopupLingerTime;
}

void UStageEncoderWidget::Nudge(double DisplayDelta)
{
	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	CommitValue(FromDisplay(FMath::Clamp(ToDisplay(Descriptor.Value) + DisplayDelta, Min, Max)));
	ShowPopupBriefly();
}

FReply UStageEncoderWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!CanEdit() || IsDragging())
	{
		// Handled during a drag too, so the wheel cannot fight the drag accumulator.
		return IsDragging() ? FReply::Handled() : Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
	}

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	double Step = Descriptor.Step > 0.0 ? Descriptor.Step : (Max - Min) * 0.01;
	if (InMouseEvent.IsShiftDown())
	{
		Step *= FineScale;
	}
	Nudge(Step * InMouseEvent.GetWheelDelta());
	// Handled: the wheel over an encoder must not also change camera fly speed.
	return FReply::Handled();
}

void UStageEncoderWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);
	bHovered = true;
}

void UStageEncoderWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	bHovered = false;
}

#undef LOCTEXT_NAMESPACE
