// Copyright Epic Games, Inc. All Rights Reserved.

#include "Startup/SStageBrandedLoader.h"

#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UI/StageCraftWidgetStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "StageBrandedLoader"

namespace StageBrandedLoader
{
	constexpr float BarWidth = 320.f;
	constexpr float BarHeight = 3.f;
	/** Width of the moving highlight, as a share of the bar. */
	constexpr float SweepShare = 0.28f;
	/** One indeterminate sweep, in seconds. */
	constexpr double SweepPeriod = 1.6;
	/** How quickly the shown fraction catches up with the reported one (per second). */
	constexpr float EaseRate = 6.f;
}

/** The progress bar: a track, the filled part and a soft highlight. Animated from the paint time, so it needs no tick. */
class SStageLoaderBar : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SStageLoaderBar) {}
		SLATE_ATTRIBUTE(TOptional<float>, Progress)
		SLATE_ARGUMENT(FLinearColor, Accent)
		SLATE_ARGUMENT(FLinearColor, Track)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Progress = InArgs._Progress;
		Accent = InArgs._Accent;
		Track = InArgs._Track;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D(StageBrandedLoader::BarWidth, StageBrandedLoader::BarHeight);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		using namespace StageBrandedLoader;

		const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		const double Now = Args.GetCurrentTime();
		const float Opacity = InWidgetStyle.GetColorAndOpacityTint().A;

		auto DrawSpan = [&](float From, float To, const FLinearColor& Color, int32 Layer)
		{
			const float Left = FMath::Clamp(From, 0.f, 1.f) * Size.X;
			const float Right = FMath::Clamp(To, 0.f, 1.f) * Size.X;
			if (Right - Left < 0.5f)
			{
				return;
			}
			FSlateDrawElement::MakeBox(OutDrawElements, Layer,
				AllottedGeometry.ToPaintGeometry(FVector2f(Right - Left, Size.Y), FSlateLayoutTransform(FVector2f(Left, 0.f))),
				White, ESlateDrawEffect::None, Color.CopyWithNewOpacity(Color.A * Opacity));
		};

		DrawSpan(0.f, 1.f, Track, LayerId);

		const TOptional<float> Reported = Progress.Get();
		if (!Reported.IsSet())
		{
			// Indeterminate: one highlight sweeping left to right, entering and leaving off the ends.
			const float Phase = float(FMath::Fmod(Now, SweepPeriod) / SweepPeriod);
			const float Head = FMath::InterpEaseInOut(-SweepShare, 1.f + SweepShare, Phase, 2.f);
			DrawSpan(Head - SweepShare, Head, Accent, LayerId + 1);
			LastPaintTime = Now;
			return LayerId + 1;
		}

		// Determinate: ease the shown fraction towards the reported one; frame-rate independent.
		const float Target = FMath::Clamp(Reported.GetValue(), 0.f, 1.f);
		const float Delta = LastPaintTime > 0.0 ? float(FMath::Min(Now - LastPaintTime, 0.1)) : 0.f;
		Shown = Target < Shown ? Target : FMath::Lerp(Shown, Target, 1.f - FMath::Exp(-EaseRate * Delta));
		LastPaintTime = Now;

		DrawSpan(0.f, Shown, Accent, LayerId + 1);

		// A soft glint travelling along the filled part, so a long wait on one catalog still shows life.
		if (Shown > 0.02f)
		{
			const float Phase = float(FMath::Fmod(Now, SweepPeriod * 1.5) / (SweepPeriod * 1.5));
			const float Head = Phase * (Shown + SweepShare);
			DrawSpan(FMath::Max(Head - SweepShare * 0.5f, 0.f), FMath::Min(Head, Shown), FLinearColor(1.f, 1.f, 1.f, 0.35f), LayerId + 2);
		}
		return LayerId + 2;
	}

private:
	TAttribute<TOptional<float>> Progress;
	FLinearColor Accent;
	FLinearColor Track;

	mutable float Shown = 0.f;
	mutable double LastPaintTime = 0.0;
};

void SStageBrandedLoader::Construct(const FArguments& InArgs)
{
	using namespace StageBrandedLoader;

	// Class defaults: the authored look, available before any content asset has loaded.
	const UStageCraftUITheme& Theme = StageCraftWidgetStyle::ResolveTheme(nullptr);
	Background = Theme.WindowBackground.CopyWithNewOpacity(1.f);
	Accent = Theme.AccentColor;
	Track = Theme.Divider;
	TextPrimary = Theme.TextPrimary;
	TextSecondary = Theme.TextSecondary;

	FSlateFontInfo Wordmark = StageCraftWidgetStyle::MakeFont(30, TEXT("Bold"));
	Wordmark.LetterSpacing = 420;
	FSlateFontInfo Tagline = StageCraftWidgetStyle::MakeFont(9);
	Tagline.LetterSpacing = 300;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
		.BorderBackgroundColor(Background)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Wordmark", "STAGECRAFT"))
				.Font(Wordmark)
				.ColorAndOpacity(TextPrimary)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 6.f, 0.f, 28.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Tagline", "STAGE DESIGN  ·  LIGHTING  ·  SHOW"))
				.Font(Tagline)
				.ColorAndOpacity(Accent)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SStageLoaderBar)
				.Progress(InArgs._Progress)
				.Accent(Accent)
				.Track(Track)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 12.f, 0.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(BarWidth + 160.f)
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(InArgs._Status)
					.Font(StageCraftWidgetStyle::MakeFont(9))
					.ColorAndOpacity(TextSecondary)
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 14.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Visibility_Lambda([ShowContinue = InArgs._ShowContinue]() { return ShowContinue.Get() ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([OnContinue = InArgs._OnContinue]()
				{
					OnContinue.ExecuteIfBound();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Continue", "Continue"))
					.Font(StageCraftWidgetStyle::MakeFont(10, TEXT("Bold")))
				]
			]
		]
	];
}

void SStageBrandedLoader::FadeOut(float Seconds, FSimpleDelegate OnFaded)
{
	if (FadeSeconds > 0.f)
	{
		return; // already fading
	}
	FadeSeconds = FMath::Max(Seconds, 0.01f);
	FadeStart = FSlateApplication::Get().GetCurrentTime();
	FadeFinished = MoveTemp(OnFaded);
	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SStageBrandedLoader::TickFade));
}

EActiveTimerReturnType SStageBrandedLoader::TickFade(double CurrentTime, float DeltaTime)
{
	const float Alpha = FMath::Clamp(float((CurrentTime - FadeStart) / FadeSeconds), 0.f, 1.f);
	SetRenderOpacity(1.f - FMath::InterpEaseOut(0.f, 1.f, Alpha, 2.f));
	if (Alpha < 1.f)
	{
		return EActiveTimerReturnType::Continue;
	}
	// Copied first: the callback usually removes this widget.
	const FSimpleDelegate Finished = FadeFinished;
	FadeFinished.Unbind();
	Finished.ExecuteIfBound();
	return EActiveTimerReturnType::Stop;
}

#undef LOCTEXT_NAMESPACE
