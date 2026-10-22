// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageSketchSurfaceWidget.h"

#include "Data/StageParameterTypes.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Placement/StageDeckMath.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSketchSurfaceWidget)

#define LOCTEXT_NAMESPACE "StageSketchSurfaceWidget"

namespace StageSketchSurface
{
	/** Freehand samples are taken at least this far apart, in pixels. */
	constexpr double SampleSpacing = 4.0;

	/** A click this close (px) to the first point closes a shape or truss loop; this close to a stroke picks it to erase. */
	constexpr double PickRadius = 10.0;

	constexpr double MinPixelsPerCm = 0.01;
	constexpr double MaxPixelsPerCm = 2.0;
	constexpr double DefaultPixelsPerCm = 0.2;
	constexpr double WheelZoomStep = 1.15;

	/** Grid lines closer than this (px) are not drawn. */
	constexpr double MinGridSpacing = 6.0;

	/** Every major line is this many cm apart (5 m). */
	constexpr double MajorGridStep = 500.0;

	constexpr float FixtureRadius = 6.f;
	constexpr int32 CircleSegments = 16;

	/** The imported photo shows through the grid and strokes, faintly enough not to hide them. */
	constexpr float ReferenceOpacity = 0.4f;

	FVector2f ToF(const FVector2D& Point)
	{
		return FVector2f(float(Point.X), float(Point.Y));
	}
}

void UStageSketchSurfaceWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	// Focusable so Enter / Esc / Backspace reach the canvas while drawing; it only takes focus on a click.
	SetIsFocusable(true);
	SetVisibility(ESlateVisibility::Visible);
	PixelsPerCm = StageSketchSurface::DefaultPixelsPerCm;
}

void UStageSketchSurfaceWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(GetWorld()))
	{
		BoundSketch = Sketch;
		Sketch->OnSketchChanged.AddUniqueDynamic(this, &ThisClass::HandleSketchChanged);
		Sketch->OnImportChanged.AddUniqueDynamic(this, &ThisClass::HandleImportChanged);
	}
	UpdateReferenceBrush();
}

void UStageSketchSurfaceWidget::NativeDestruct()
{
	if (UStageSketchSubsystem* Sketch = BoundSketch.Get())
	{
		Sketch->OnSketchChanged.RemoveDynamic(this, &ThisClass::HandleSketchChanged);
		Sketch->OnImportChanged.RemoveDynamic(this, &ThisClass::HandleImportChanged);
	}
	BoundSketch.Reset();
	ReferenceBrush.SetResourceObject(nullptr);
	Pending.Reset();
	Gesture = EGesture::None;
	Super::NativeDestruct();
}

UStageSketchSubsystem* UStageSketchSurfaceWidget::GetSketch() const
{
	return BoundSketch.Get();
}

const UStageCraftUITheme& UStageSketchSurfaceWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageSketchSurfaceWidget::HandleSketchChanged()
{
	HoverStroke = INDEX_NONE;
	Repaint();
}

void UStageSketchSurfaceWidget::HandleImportChanged()
{
	UpdateReferenceBrush();
	Repaint();
}

void UStageSketchSurfaceWidget::UpdateReferenceBrush()
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	UTexture2D* Photo = Sketch ? Sketch->GetReferenceImage() : nullptr;
	if (ReferenceBrush.GetResourceObject() != Photo)
	{
		ReferenceBrush.SetResourceObject(Photo);
		ReferenceBrush.DrawAs = ESlateBrushDrawType::Image;
		ReferenceBrush.ImageSize = Photo ? FVector2D(Photo->GetSizeX(), Photo->GetSizeY()) : FVector2D::ZeroVector;
	}
}

void UStageSketchSurfaceWidget::PaintReference(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Size) const
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	if (!Sketch || !Sketch->IsReferenceVisible() || ReferenceBrush.GetResourceObject() != Sketch->GetReferenceImage())
	{
		return;
	}
	// Image up is plan +X and image right is plan +Y, like the canvas, so the photo is drawn upright between its plan corners.
	const FBox2D& Bounds = Sketch->GetReferenceBounds();
	const FVector2D TopLeft = WorldToLocal(FVector2D(Bounds.Max.X, Bounds.Min.Y), Size);
	const FVector2D BottomRight = WorldToLocal(FVector2D(Bounds.Min.X, Bounds.Max.Y), Size);
	const FVector2D Extent = BottomRight - TopLeft;
	if (Extent.X < 1.0 || Extent.Y < 1.0)
	{
		return;
	}
	FSlateDrawElement::MakeBox(Elements, Layer, Geometry.ToPaintGeometry(FVector2f(Extent), FSlateLayoutTransform(FVector2f(TopLeft))), &ReferenceBrush,
		ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, StageSketchSurface::ReferenceOpacity));
}

void UStageSketchSurfaceWidget::Repaint()
{
	Invalidate(EInvalidateWidgetReason::Paint);
}

void UStageSketchSurfaceWidget::SetTool(EStageSketchTool NewTool)
{
	if (Tool != NewTool)
	{
		CancelStroke();
		Tool = NewTool;
		HoverStroke = INDEX_NONE;
		Repaint();
	}
}

void UStageSketchSurfaceWidget::CancelStroke()
{
	Pending.Reset();
	if (Gesture == EGesture::Drawing)
	{
		Gesture = EGesture::None;
	}
	Repaint();
}

void UStageSketchSurfaceWidget::FrameSketch()
{
	using namespace StageSketchSurface;
	const UStageSketchSubsystem* Sketch = GetSketch();
	FBox2D Bounds(ForceInit);
	if (Sketch)
	{
		for (const FStageSketchStroke& Stroke : Sketch->GetStrokes())
		{
			for (const FVector2D& Point : Stroke.Points)
			{
				Bounds += Point;
			}
		}
		if (Sketch->IsReferenceVisible())
		{
			Bounds += Sketch->GetReferenceBounds();
		}
	}
	const FVector2D Size = GetCachedGeometry().GetLocalSize();
	if (!Bounds.bIsValid || Size.X < 1.0 || Size.Y < 1.0)
	{
		ViewCentre = FVector2D::ZeroVector;
		PixelsPerCm = DefaultPixelsPerCm;
	}
	else
	{
		// Plan X runs up the screen and plan Y across it; keep a 10 % margin.
		const FVector2D Extent = Bounds.GetSize();
		const double ScaleAcross = Size.X / FMath::Max(Extent.Y, 100.0);
		const double ScaleUp = Size.Y / FMath::Max(Extent.X, 100.0);
		ViewCentre = Bounds.GetCenter();
		PixelsPerCm = FMath::Clamp(0.9 * FMath::Min(ScaleAcross, ScaleUp), MinPixelsPerCm, MaxPixelsPerCm);
	}
	Repaint();
}

FVector2D UStageSketchSurfaceWidget::LocalToWorld(const FVector2D& Local, const FVector2D& Size) const
{
	const FVector2D Centre = Size * 0.5;
	return FVector2D(ViewCentre.X - (Local.Y - Centre.Y) / PixelsPerCm, ViewCentre.Y + (Local.X - Centre.X) / PixelsPerCm);
}

FVector2D UStageSketchSurfaceWidget::WorldToLocal(const FVector2D& World, const FVector2D& Size) const
{
	const FVector2D Centre = Size * 0.5;
	return FVector2D(Centre.X + (World.Y - ViewCentre.Y) * PixelsPerCm, Centre.Y - (World.X - ViewCentre.X) * PixelsPerCm);
}

FVector2D UStageSketchSurfaceWidget::PickPoint(const FVector2D& Local, const FVector2D& Size) const
{
	const FVector2D World = LocalToWorld(Local, Size);
	const UStageSketchSubsystem* Sketch = GetSketch();
	const bool bSnaps = Tool == EStageSketchTool::Shape || Tool == EStageSketchTool::Truss || Tool == EStageSketchTool::Fixture;
	if (bSnaps && Sketch && Sketch->GetSettings().bSnapToGrid)
	{
		return StageSketchMath::SnapToGrid(World, Sketch->GetSettings().GridStep);
	}
	return World;
}

EStageSketchStrokeKind UStageSketchSurfaceWidget::GetStrokeKind() const
{
	switch (Tool)
	{
	case EStageSketchTool::Freehand:	return EStageSketchStrokeKind::Freehand;
	case EStageSketchTool::Truss:		return EStageSketchStrokeKind::Truss;
	case EStageSketchTool::Fixture:		return EStageSketchStrokeKind::Fixture;
	default:							return EStageSketchStrokeKind::Shape;
	}
}

void UStageSketchSurfaceWidget::CommitStroke(EStageSketchStrokeKind Kind, bool bClosed)
{
	if (UStageSketchSubsystem* Sketch = GetSketch())
	{
		FStageSketchStroke Stroke;
		Stroke.Kind = Kind;
		Stroke.Points = Pending;
		Stroke.bClosed = bClosed;
		Stroke.DeckHeight = Sketch->GetSettings().DeckHeight;
		Sketch->AddStroke(Stroke);
	}
	Pending.Reset();
	Repaint();
}

void UStageSketchSurfaceWidget::FinishPointStroke()
{
	if (Tool == EStageSketchTool::Shape && Pending.Num() >= 3)
	{
		CommitStroke(EStageSketchStrokeKind::Shape, true);
	}
	else if (Tool == EStageSketchTool::Truss && Pending.Num() >= 2)
	{
		CommitStroke(EStageSketchStrokeKind::Truss, false);
	}
}

void UStageSketchSurfaceWidget::HandlePointClick(const FVector2D& Local, const FVector2D& Size)
{
	using namespace StageSketchSurface;
	if (Pending.Num() >= 3 && FVector2D::Distance(WorldToLocal(Pending[0], Size), Local) <= PickRadius)
	{
		CommitStroke(GetStrokeKind(), /*bClosed*/ true);
		return;
	}
	const FVector2D Point = PickPoint(Local, Size);
	if (Pending.IsEmpty() || FVector2D::Distance(Pending.Last(), Point) >= StageSketchMath::MinPointSpacing)
	{
		if (Pending.Num() < StageSketchMath::MaxStrokePoints)
		{
			Pending.Add(Point);
		}
	}
	Repaint();
}

FReply UStageSketchSurfaceWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	using namespace StageSketchSurface;
	const FVector2D Size = InGeometry.GetLocalSize();
	const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
	const TSharedRef<SWidget> Self = TakeWidget();

	if (InMouseEvent.GetEffectingButton() == EKeys::RightMouseButton || InMouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton)
	{
		Gesture = EGesture::Panning;
		LastPanLocal = Local;
		return FReply::Handled().CaptureMouse(Self);
	}
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	UStageSketchSubsystem* Sketch = GetSketch();
	switch (Tool)
	{
	case EStageSketchTool::Freehand:
		Pending.Reset();
		PenWorld = LocalToWorld(Local, Size);
		LastSampleLocal = Local;
		Pending.Add(PenWorld);
		Gesture = EGesture::Drawing;
		Repaint();
		return FReply::Handled().CaptureMouse(Self).SetUserFocus(Self);

	case EStageSketchTool::Shape:
	case EStageSketchTool::Truss:
		HandlePointClick(Local, Size);
		break;

	case EStageSketchTool::Fixture:
		Pending = { PickPoint(Local, Size) };
		CommitStroke(EStageSketchStrokeKind::Fixture, false);
		break;

	case EStageSketchTool::Erase:
		if (Sketch)
		{
			Sketch->RemoveStroke(Sketch->FindStrokeAt(LocalToWorld(Local, Size), PickRadius / PixelsPerCm));
		}
		break;
	}
	return FReply::Handled().SetUserFocus(Self);
}

FReply UStageSketchSurfaceWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Gesture == EGesture::Panning && InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		Gesture = EGesture::None;
		return FReply::Handled().ReleaseMouseCapture();
	}
	if (Gesture == EGesture::Drawing && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		Gesture = EGesture::None;
		CommitStroke(EStageSketchStrokeKind::Freehand, false);
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply UStageSketchSurfaceWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	using namespace StageSketchSurface;
	const FVector2D Size = InGeometry.GetLocalSize();
	const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());

	if (Gesture == EGesture::Panning)
	{
		const FVector2D Delta = Local - LastPanLocal;
		LastPanLocal = Local;
		// Dragging right moves the plan right (+Y), dragging up moves it up (+X).
		ViewCentre += FVector2D(Delta.Y, -Delta.X) / PixelsPerCm;
	}
	else if (Gesture == EGesture::Drawing)
	{
		const UStageSketchSubsystem* Sketch = GetSketch();
		PenWorld = StageSketchMath::Stabilize(PenWorld, LocalToWorld(Local, Size), Sketch ? Sketch->GetSettings().Smoothing : 0.f);
		if (FVector2D::Distance(Local, LastSampleLocal) >= SampleSpacing && Pending.Num() < StageSketchMath::MaxStrokePoints)
		{
			LastSampleLocal = Local;
			Pending.Add(PenWorld);
		}
	}

	Hover = PickPoint(Local, Size);
	if (Tool == EStageSketchTool::Erase)
	{
		const UStageSketchSubsystem* Sketch = GetSketch();
		HoverStroke = Sketch ? Sketch->FindStrokeAt(LocalToWorld(Local, Size), PickRadius / PixelsPerCm) : INDEX_NONE;
	}
	Repaint();
	return FReply::Handled();
}

FReply UStageSketchSurfaceWidget::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// The first click of the pair already added the point under the cursor.
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && (Tool == EStageSketchTool::Shape || Tool == EStageSketchTool::Truss))
	{
		FinishPointStroke();
		return FReply::Handled();
	}
	return NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UStageSketchSurfaceWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	using namespace StageSketchSurface;
	const FVector2D Size = InGeometry.GetLocalSize();
	const FVector2D Local = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
	// The plan point under the cursor stays under it.
	const FVector2D Anchor = LocalToWorld(Local, Size);
	PixelsPerCm = FMath::Clamp(PixelsPerCm * FMath::Pow(WheelZoomStep, InMouseEvent.GetWheelDelta()), MinPixelsPerCm, MaxPixelsPerCm);
	const FVector2D Moved = LocalToWorld(Local, Size);
	ViewCentre += Anchor - Moved;
	Repaint();
	return FReply::Handled();
}

void UStageSketchSurfaceWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	if (Gesture == EGesture::None)
	{
		Hover.Reset();
		HoverStroke = INDEX_NONE;
		Repaint();
	}
}

void UStageSketchSurfaceWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
	// A drag cut short (window lost focus) keeps nothing half-drawn.
	if (Gesture == EGesture::Drawing)
	{
		Pending.Reset();
	}
	Gesture = EGesture::None;
	Repaint();
}

FReply UStageSketchSurfaceWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Enter)
	{
		FinishPointStroke();
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		CancelStroke();
		return FReply::Handled();
	}
	if (Key == EKeys::BackSpace)
	{
		if (!Pending.IsEmpty() && Gesture == EGesture::None)
		{
			Pending.Pop();
			Repaint();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

// --- Painting ---

int32 UStageSketchSurfaceWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace StageSketchSurface;
	const UStageCraftUITheme& Palette = GetTheme();
	const FVector2D Size = AllottedGeometry.GetLocalSize();

	// A plain white brush tinted with the theme: a rounded-box brush painted directly ignores its fill colour here.
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), StageCraftWidgetStyle::AppBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, Palette.WindowBackground);
	PaintReference(AllottedGeometry, OutDrawElements, LayerId + 1, Size);
	PaintGrid(AllottedGeometry, OutDrawElements, LayerId + 2, Size);

	const int32 StrokeLayer = LayerId + 3;
	if (const UStageSketchSubsystem* Sketch = GetSketch())
	{
		const TArray<FStageSketchStroke>& Strokes = Sketch->GetStrokes();
		for (int32 Index = 0; Index < Strokes.Num(); ++Index)
		{
			PaintStroke(AllottedGeometry, OutDrawElements, StrokeLayer, Size, Strokes[Index], Index == HoverStroke);
		}
	}

	// The stroke in progress, with a rubber band to the cursor for clicked tools.
	const int32 PendingLayer = StrokeLayer + 2;
	if (!Pending.IsEmpty())
	{
		TArray<FVector2f> Line;
		for (const FVector2D& Point : Pending)
		{
			Line.Add(ToF(WorldToLocal(Point, Size)));
		}
		const bool bClicked = Tool == EStageSketchTool::Shape || Tool == EStageSketchTool::Truss;
		if (bClicked && Hover.IsSet())
		{
			Line.Add(ToF(WorldToLocal(Hover.GetValue(), Size)));
		}
		FSlateDrawElement::MakeLines(OutDrawElements, PendingLayer, AllottedGeometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, Palette.SelectionColor, true, 2.f);
		if (bClicked)
		{
			for (const FVector2D& Point : Pending)
			{
				PaintMarker(AllottedGeometry, OutDrawElements, PendingLayer, WorldToLocal(Point, Size), 3.f, Palette.SelectionColor);
			}
			// The first point lights up when a click there would close the stroke.
			if (Pending.Num() >= 3 && Hover.IsSet() && FVector2D::Distance(WorldToLocal(Pending[0], Size), WorldToLocal(Hover.GetValue(), Size)) <= PickRadius)
			{
				PaintMarker(AllottedGeometry, OutDrawElements, PendingLayer, WorldToLocal(Pending[0], Size), 7.f, Palette.AccentColor);
			}
		}
	}

	// The snapped cursor.
	if (Hover.IsSet() && Gesture != EGesture::Panning && Tool != EStageSketchTool::Erase)
	{
		const FVector2D At = WorldToLocal(Hover.GetValue(), Size);
		const FLinearColor Cross = StageCraftWidgetStyle::WithAlpha(Palette.TextPrimary, 0.6f);
		FSlateDrawElement::MakeLines(OutDrawElements, PendingLayer, AllottedGeometry.ToPaintGeometry(), TArray<FVector2f>{ ToF(At - FVector2D(6, 0)), ToF(At + FVector2D(6, 0)) },
			ESlateDrawEffect::None, Cross, true, 1.f);
		FSlateDrawElement::MakeLines(OutDrawElements, PendingLayer, AllottedGeometry.ToPaintGeometry(), TArray<FVector2f>{ ToF(At - FVector2D(0, 6)), ToF(At + FVector2D(0, 6)) },
			ESlateDrawEffect::None, Cross, true, 1.f);
		const FVector2D Plan = Hover.GetValue();
		FSlateDrawElement::MakeText(OutDrawElements, PendingLayer, AllottedGeometry.ToPaintGeometry(FVector2f(160.f, 14.f), FSlateLayoutTransform(ToF(At + FVector2D(10, 8)))),
			FString::Printf(TEXT("X %.2f  Y %.2f m"), Plan.X * 0.01, Plan.Y * 0.01), StageCraftWidgetStyle::MakeFont(7), ESlateDrawEffect::None, Palette.TextSecondary);
	}

	// Orientation and scale.
	const FString Caption = FString::Printf(TEXT("TOP VIEW   up +X, right +Y   grid %.0f cm"), GetSketch() ? GetSketch()->GetSettings().GridStep : 0.f);
	FSlateDrawElement::MakeText(OutDrawElements, PendingLayer, AllottedGeometry.ToPaintGeometry(FVector2f(400.f, 14.f), FSlateLayoutTransform(FVector2f(8.f, 6.f))),
		Caption, StageCraftWidgetStyle::MakeFont(7, TEXT("Bold")), ESlateDrawEffect::None, Palette.TextDisabled);

	return Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, PendingLayer + 1, InWidgetStyle, bParentEnabled);
}

void UStageSketchSurfaceWidget::PaintGrid(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Size) const
{
	using namespace StageSketchSurface;
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageSketchSubsystem* Sketch = GetSketch();
	const double Step = Sketch ? Sketch->GetSettings().GridStep : 50.0;

	// The visible plan rectangle: X runs up the screen, Y across it.
	const FVector2D TopLeft = LocalToWorld(FVector2D::ZeroVector, Size);
	const FVector2D BottomRight = LocalToWorld(Size, Size);
	const double MinX = BottomRight.X, MaxX = TopLeft.X;
	const double MinY = TopLeft.Y, MaxY = BottomRight.Y;

	const auto DrawLines = [&](double LineStep, const FLinearColor& Color, float Thickness)
	{
		if (LineStep * PixelsPerCm < MinGridSpacing)
		{
			return;
		}
		for (double Y = FMath::CeilToDouble(MinY / LineStep) * LineStep; Y <= MaxY; Y += LineStep)
		{
			const double ScreenX = WorldToLocal(FVector2D(0.0, Y), Size).X;
			FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), TArray<FVector2f>{ FVector2f(float(ScreenX), 0.f), FVector2f(float(ScreenX), float(Size.Y)) },
				ESlateDrawEffect::None, Color, false, Thickness);
		}
		for (double X = FMath::CeilToDouble(MinX / LineStep) * LineStep; X <= MaxX; X += LineStep)
		{
			const double ScreenY = WorldToLocal(FVector2D(X, 0.0), Size).Y;
			FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), TArray<FVector2f>{ FVector2f(0.f, float(ScreenY)), FVector2f(float(Size.X), float(ScreenY)) },
				ESlateDrawEffect::None, Color, false, Thickness);
		}
	};
	DrawLines(Step, StageCraftWidgetStyle::WithAlpha(Palette.Divider, 0.45f), 1.f);
	DrawLines(MajorGridStep, Palette.Divider, 1.f);

	// The plan axes through the origin, in the gizmo's axis colours.
	const FVector2D Origin = WorldToLocal(FVector2D::ZeroVector, Size);
	FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), TArray<FVector2f>{ FVector2f(float(Origin.X), 0.f), FVector2f(float(Origin.X), float(Size.Y)) },
		ESlateDrawEffect::None, StageCraftWidgetStyle::WithAlpha(Palette.AxisXColor, 0.5f), false, 1.f);
	FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), TArray<FVector2f>{ FVector2f(0.f, float(Origin.Y)), FVector2f(float(Size.X), float(Origin.Y)) },
		ESlateDrawEffect::None, StageCraftWidgetStyle::WithAlpha(Palette.AxisYColor, 0.5f), false, 1.f);
}

void UStageSketchSurfaceWidget::PaintStroke(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Size,
	const FStageSketchStroke& Stroke, bool bHighlighted) const
{
	using namespace StageSketchSurface;
	const UStageCraftUITheme& Palette = GetTheme();
	if (Stroke.Points.IsEmpty())
	{
		return;
	}

	TArray<FVector2D> Local;
	Local.Reserve(Stroke.Points.Num());
	for (const FVector2D& Point : Stroke.Points)
	{
		Local.Add(WorldToLocal(Point, Size));
	}

	const EStageSketchElement Element = Stroke.GetElement();
	if (Element == EStageSketchElement::Fixture)
	{
		PaintMarker(Geometry, Elements, Layer, Local[0], FixtureRadius, bHighlighted ? Palette.WarningColor : Palette.GetFeatureGroupColor(StageCraftTags::FeatureGroup_Dimmer));
		return;
	}

	const FLinearColor Color = bHighlighted ? Palette.WarningColor
		: (Element == EStageSketchElement::Deck ? Palette.GetFeatureGroupColor(StageCraftTags::FeatureGroup_Deck) : Palette.AccentColor);
	if (Element == EStageSketchElement::Deck)
	{
		PaintFill(Geometry, Elements, Layer, Local, StageCraftWidgetStyle::WithAlpha(Color, 0.25f));
	}

	TArray<FVector2f> Line;
	Line.Reserve(Local.Num() + 1);
	for (const FVector2D& Point : Local)
	{
		Line.Add(ToF(Point));
	}
	if (Stroke.bClosed)
	{
		Line.Add(ToF(Local[0]));
	}
	FSlateDrawElement::MakeLines(Elements, Layer + 1, Geometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, Color, true, Element == EStageSketchElement::Truss ? 3.f : 2.f);

	if (Element == EStageSketchElement::Deck)
	{
		const FVector2D Label = StageDeckMath::GetCentroid(Local);
		FSlateDrawElement::MakeText(Elements, Layer + 1, Geometry.ToPaintGeometry(FVector2f(80.f, 14.f), FSlateLayoutTransform(ToF(Label - FVector2D(18, 7)))),
			FString::Printf(TEXT("%.2f m"), Stroke.DeckHeight * 0.01), StageCraftWidgetStyle::MakeFont(7, TEXT("Bold")), ESlateDrawEffect::None, Palette.TextPrimary);
	}
	else
	{
		for (const FVector2D& Point : Local)
		{
			PaintMarker(Geometry, Elements, Layer + 1, Point, 2.5f, Color);
		}
	}
}

void UStageSketchSurfaceWidget::PaintFill(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, TConstArrayView<FVector2D> LocalPolygon, const FLinearColor& Color) const
{
	TArray<int32> Triangles;
	if (!StageDeckMath::Triangulate(LocalPolygon, Triangles))
	{
		return; // A self-crossing outline is drawn as lines only; the generator will skip it.
	}
	const FSlateBrush* White = StageCraftWidgetStyle::AppBrush(TEXT("WhiteBrush"));
	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White);
	const FSlateRenderTransform& Transform = Geometry.GetAccumulatedRenderTransform();
	const FColor Tint = Color.ToFColor(true);

	TArray<FSlateVertex> Vertices;
	Vertices.Reserve(LocalPolygon.Num());
	for (const FVector2D& Point : LocalPolygon)
	{
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, StageSketchSurface::ToF(Point), FVector2f(0.5f, 0.5f), FVector2f(1.f, 1.f), Tint));
	}
	TArray<SlateIndex> Indices;
	Indices.Reserve(Triangles.Num());
	for (const int32 Index : Triangles)
	{
		Indices.Add(SlateIndex(Index));
	}
	FSlateDrawElement::MakeCustomVerts(Elements, Layer, Handle, Vertices, Indices, nullptr, 0, 0);
}

void UStageSketchSurfaceWidget::PaintMarker(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer, const FVector2D& Local, float Radius, const FLinearColor& Color) const
{
	TArray<FVector2f> Circle;
	Circle.Reserve(StageSketchSurface::CircleSegments + 1);
	for (int32 Segment = 0; Segment <= StageSketchSurface::CircleSegments; ++Segment)
	{
		const double Angle = 2.0 * UE_DOUBLE_PI * Segment / StageSketchSurface::CircleSegments;
		Circle.Add(StageSketchSurface::ToF(Local + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius));
	}
	FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), Circle, ESlateDrawEffect::None, Color, true, 2.f);
}

#undef LOCTEXT_NAMESPACE
