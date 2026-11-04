// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/SStageEffectPreview.h"

#include "Rendering/DrawElements.h"
#include "Show/StageEffects.h"
#include "Styling/CoreStyle.h"

namespace StageEffectPreview
{
	constexpr int32 CurvePoints = 96;
	/** More fixtures than this show as the first ones only: the dots would merge anyway. */
	constexpr int32 MaxDots = 128;
	constexpr float DotSize = 5.f;
	constexpr float Margin = 6.f;

	bool IsPath(const FStageEffect& Effect)
	{
		return Effect.Target == EStageEffectTarget::Position
			&& (Effect.Waveform == EStageEffectWaveform::Circle || Effect.Waveform == EStageEffectWaveform::Figure8);
	}
}

void SStageEffectPreview::Construct(const FArguments& InArgs)
{
	SetCanTick(false);
}

void SStageEffectPreview::SetStateSource(FGetState InGetState)
{
	GetState = MoveTemp(InGetState);
	if (GetState && !AnimateTimer.IsValid())
	{
		AnimateTimer = RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SStageEffectPreview::Animate));
	}
}

EActiveTimerReturnType SStageEffectPreview::Animate(double InCurrentTime, float InDeltaTime)
{
	if (!GetState)
	{
		AnimateTimer.Reset();
		return EActiveTimerReturnType::Stop;
	}
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Continue;
}

int32 SStageEffectPreview::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace StageEffectPreview;
	const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), White, ESlateDrawEffect::None, Colors.Background);

	const FVector2f Origin(Margin, Margin);
	const FVector2f Area = FVector2f::Max(Size - FVector2f(2.f * Margin), FVector2f(1.f));
	const auto Line = [&](TArray<FVector2f> Points, const FLinearColor& Color, float Thickness)
	{
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(), MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	};
	// The middle line and the top and bottom of the swing.
	Line({ Origin + FVector2f(0.f, Area.Y * 0.5f), Origin + FVector2f(Area.X, Area.Y * 0.5f) }, Colors.Grid, 1.f);
	Line({ Origin, Origin + FVector2f(Area.X, 0.f) }, Colors.Grid, 1.f);
	Line({ Origin + FVector2f(0.f, Area.Y), Origin + Area }, Colors.Grid, 1.f);

	FStageEffect Effect;
	int32 Count = 0;
	double Cycles = 0.0;
	if (!GetState || !GetState(Effect, Count, Cycles) || Count <= 0)
	{
		return LayerId + 2;
	}

	const bool bPath = IsPath(Effect);
	// Unit (0..1, up) across one cycle; a path maps pan across and tilt up, both -1..1, in a centred square so a circle stays round.
	const auto ToScreen = [&](float X, float Y) { return Origin + FVector2f(X * Area.X, (1.f - Y) * Area.Y); };
	const float Side = FMath::Min(Area.X, Area.Y);
	const FVector2f PathOrigin = Origin + (Area - FVector2f(Side)) * 0.5f;
	const auto PathPoint = [&](const StageEffects::FSample& Sample)
	{
		return PathOrigin + FVector2f((0.5f + 0.5f * Sample.Pan) * Side, (0.5f - 0.5f * Sample.Tilt) * Side);
	};

	if (Effect.Waveform != EStageEffectWaveform::Random)
	{
		TArray<FVector2f> Curve;
		Curve.Reserve(CurvePoints + 1);
		for (int32 Point = 0; Point <= CurvePoints; ++Point)
		{
			// The last point sits just before the wrap, so a ramp is not drawn back down across the box.
			const double Position = FMath::Min(static_cast<double>(Point) / CurvePoints, 0.9999);
			const StageEffects::FSample Sample = StageEffects::SampleWave(Effect.Waveform, Position, Effect.Duty, 0);
			Curve.Add(bPath ? PathPoint(Sample) : ToScreen(static_cast<float>(Position), Sample.Unit));
		}
		Line(MoveTemp(Curve), Colors.Curve, 1.5f);
	}

	const int32 Dots = FMath::Min(Count, MaxDots);
	for (int32 Index = 0; Index < Dots; ++Index)
	{
		const StageEffects::FSample Sample = StageEffects::Evaluate(Effect, Index, Count, Cycles);
		FVector2f Center;
		if (bPath)
		{
			Center = PathPoint(Sample);
		}
		else
		{
			const double Position = StageEffects::GetWavePosition(Effect, Index, Count, Cycles);
			Center = ToScreen(static_cast<float>(Position - FMath::FloorToDouble(Position)), Sample.Unit);
		}
		// The first fixture brighter, so the direction of travel is visible.
		const FLinearColor Color = Index == 0 ? Colors.Dot : Colors.Dot.CopyWithNewOpacity(0.6f);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(FVector2f(DotSize), FSlateLayoutTransform(Center - FVector2f(DotSize * 0.5f))), White, ESlateDrawEffect::None, Color);
	}
	return LayerId + 3;
}
