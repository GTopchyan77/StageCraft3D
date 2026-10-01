// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageEncoderWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Rendering/DrawElements.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEncoderWidget)

namespace StageEncoder
{
	const FLinearColor StripBackground(0.018f, 0.018f, 0.022f, 1.f);
	const FLinearColor TextColor(0.82f, 0.82f, 0.85f, 1.f);
	const FLinearColor RingColor(0.12f, 0.12f, 0.14f, 1.f);

	// Dial sweep like a console encoder ring: 270 degrees, gap at the bottom. Angles in radians,
	// measured clockwise from 12 o'clock (Slate's Y axis points down).
	constexpr float SweepStart = -0.75f * UE_PI;
	constexpr float Sweep = 1.5f * UE_PI;
	constexpr int32 ArcSegments = 48;

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
}

void UStageEncoderWidget::ApplyValue(const FStageParameterValue& InValue, bool bForce)
{
	// The dial itself is painted from Descriptor.Value every frame; only the readout needs pushing.
	if (ValueText)
	{
		ValueText->SetText(FormatValue(Descriptor, InValue));
	}
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

	const FVector2D Center = Size * 0.5;
	const float Radius = static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.5 - 3.0);
	const float ValueAngle = SweepStart + Sweep * GetNormalizedValue();
	const float Dim = bParentEnabled && GetIsEnabled() ? 1.f : 0.4f;
	const FPaintGeometry PaintGeometry = DialGeometry.ToPaintGeometry();

	TArray<FVector2D> Ring;
	AppendArc(Ring, Center, Radius, SweepStart, SweepStart + Sweep);
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, Ring, ESlateDrawEffect::None, RingColor * Dim, /*bAntialias*/ true, 4.f);

	TArray<FVector2D> ValueArc;
	AppendArc(ValueArc, Center, Radius, SweepStart, ValueAngle);
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, ValueArc, ESlateDrawEffect::None, GroupColor * Dim, true, 4.f);

	const TArray<FVector2D> Pointer = { PointOnCircle(Center, Radius * 0.25f, ValueAngle), PointOnCircle(Center, Radius - 5.f, ValueAngle) };
	FSlateDrawElement::MakeLines(OutDrawElements, ++LayerId, PaintGeometry, Pointer, ESlateDrawEffect::None, FLinearColor::White * Dim, true, 2.f);

	return LayerId;
}

FReply UStageEncoderWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || Descriptor.bReadOnly || !Descriptor.HasRange())
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}

	bDragging = true;
	DragValue = ToDisplay(Descriptor.Value);
	return FReply::Handled().CaptureMouse(TakeWidget());
}

FReply UStageEncoderWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!bDragging || !HasMouseCapture())
	{
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	const FVector2D Delta = InMouseEvent.GetCursorDelta();
	const double PerPixel = (Max - Min) / PixelsPerFullRange * (InMouseEvent.IsShiftDown() ? 0.1 : 1.0);

	// Right and up both turn clockwise (Slate's Y grows downward).
	DragValue = FMath::Clamp(DragValue + (Delta.X - Delta.Y) * PerPixel, Min, Max);
	CommitValue(FromDisplay(DragValue));
	return FReply::Handled();
}

FReply UStageEncoderWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!bDragging || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	EndDrag();
	return FReply::Handled().ReleaseMouseCapture();
}

void UStageEncoderWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	EndDrag();
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
}

void UStageEncoderWidget::EndDrag()
{
	if (bDragging)
	{
		bDragging = false;
		FinishInteraction();
	}
}

void UStageEncoderWidget::Nudge(double DisplayDelta)
{
	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	CommitValue(FromDisplay(FMath::Clamp(ToDisplay(Descriptor.Value) + DisplayDelta, Min, Max)));
}

FReply UStageEncoderWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Descriptor.bReadOnly || !Descriptor.HasRange())
	{
		return Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
	}

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	double Step = Descriptor.Step > 0.0 ? Descriptor.Step : (Max - Min) * 0.01;
	if (InMouseEvent.IsShiftDown())
	{
		Step *= 0.1;
	}
	Nudge(Step * InMouseEvent.GetWheelDelta());
	// Handled: the wheel over an encoder must not also change camera fly speed.
	return FReply::Handled();
}
