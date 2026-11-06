// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageUIAnchors.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Containers/Ticker.h"
#include "UI/StageAudioPanel.h"
#include "UI/StageCuePanel.h"
#include "UI/StageEffectsPanel.h"
#include "UI/StageGroupsPanel.h"
#include "UI/StagePalettesPanel.h"
#include "UI/StagePatchPanel.h"
#include "UI/StagePlaybackPanel.h"
#include "UI/StageProblemsPanel.h"
#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageUIAnchors)

void FStageAnchorWidgets::Add(FName ControlId, UWidget* Widget)
{
	if (Widget)
	{
		Widgets.Add(ControlId, Widget);
	}
}

UWidget* FStageAnchorWidgets::Find(FName ControlId) const
{
	const TWeakObjectPtr<UWidget>* Widget = Widgets.Find(ControlId);
	return Widget ? Widget->Get() : nullptr;
}

namespace StageUIAnchors
{
	namespace
	{
		/** The widget that contains Widget: its panel, or for the root of a nested user widget, that user widget. */
		UWidget* GetContainer(const UWidget& Widget)
		{
			if (UPanelWidget* Parent = Widget.GetParent())
			{
				return Parent;
			}
			const UWidgetTree* Tree = Cast<UWidgetTree>(Widget.GetOuter());
			return Tree ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
		}

		struct FPulse
		{
			TWeakObjectPtr<UWidget> Widget;
			double StartSeconds = 0.0;
			float OriginalOpacity = 1.f;
			FWidgetTransform OriginalTransform;
			/** Compact controls (a button, a fader) also swell a little; a whole row only blinks, so it never spills over the panel. */
			bool bSwell = false;
		};

		/** Widest control that swells while it pulses (slate units). */
		constexpr float MaxSwellWidth = 400.f;

		/** Pulses in progress; the ticker runs only while there is one. Game thread only. */
		TArray<FPulse> Pulses;
		FTSTicker::FDelegateHandle PulseTicker;

		bool TickPulses(float /*DeltaSeconds*/)
		{
			const double Now = FPlatformTime::Seconds();
			for (int32 Index = Pulses.Num() - 1; Index >= 0; --Index)
			{
				FPulse& Pulse = Pulses[Index];
				UWidget* Widget = Pulse.Widget.Get();
				const float Elapsed = static_cast<float>(Now - Pulse.StartSeconds);
				if (!Widget || Elapsed >= PulseSeconds)
				{
					if (Widget)
					{
						Widget->SetRenderOpacity(Pulse.OriginalOpacity);
						Widget->SetRenderTransform(Pulse.OriginalTransform);
					}
					Pulses.RemoveAtSwap(Index);
					continue;
				}
				// Three blinks down to 20 %, ending as it was.
				const float Dip = FMath::Abs(FMath::Sin(Elapsed / PulseSeconds * 3.f * UE_PI));
				Widget->SetRenderOpacity(Pulse.OriginalOpacity * (1.f - 0.8f * Dip));
				if (Pulse.bSwell)
				{
					FWidgetTransform Swollen = Pulse.OriginalTransform;
					Swollen.Scale = Pulse.OriginalTransform.Scale * (1.f + 0.12f * Dip);
					Widget->SetRenderTransform(Swollen);
				}
			}
			if (Pulses.IsEmpty())
			{
				PulseTicker.Reset();
				return false;
			}
			return true;
		}
	}

	TArray<FTable> GetTables()
	{
		return {
			{ StageCraftTags::Panel_Playback, UStagePlaybackPanel::GetAnchors() },
			{ StageCraftTags::Panel_Cues, UStageCuePanel::GetAnchors() },
			{ StageCraftTags::Panel_Effects, UStageEffectsPanel::GetAnchors() },
			{ StageCraftTags::Panel_Audio, UStageAudioPanel::GetAnchors() },
			{ StageCraftTags::Panel_Patch, UStagePatchPanel::GetAnchors() },
			{ StageCraftTags::Panel_Groups, UStageGroupsPanel::GetAnchors() },
			{ StageCraftTags::Panel_Palettes, UStagePalettesPanel::GetAnchors() },
			{ StageCraftTags::Panel_Problems, UStageProblemsPanel::GetAnchors() },
		};
	}

	bool GetPlaceTarget(EStageDiagnosticPlace Place, FGameplayTag& OutPanel, FName& OutControl)
	{
		struct FTarget
		{
			EStageDiagnosticPlace Place;
			FGameplayTag Panel;
			const TCHAR* Control;
		};
		// Every control named here is an anchor of that panel (StageCraft.Show.Diagnostics checks it).
		const FTarget Targets[] = {
			{ EStageDiagnosticPlace::Patch, StageCraftTags::Panel_Patch, TEXT("PatchSheet") },
			{ EStageDiagnosticPlace::DMXIn, StageCraftTags::Panel_Patch, TEXT("DMXIn") },
			{ EStageDiagnosticPlace::DMXOut, StageCraftTags::Panel_Patch, TEXT("DMXOut") },
			{ EStageDiagnosticPlace::Cues, StageCraftTags::Panel_Cues, TEXT("CueList") },
			{ EStageDiagnosticPlace::ControlSource, StageCraftTags::Panel_Cues, TEXT("Internal") },
			{ EStageDiagnosticPlace::Playback, StageCraftTags::Panel_Playback, TEXT("Fader") },
			{ EStageDiagnosticPlace::GrandMaster, StageCraftTags::Panel_Playback, TEXT("GrandMaster") },
			{ EStageDiagnosticPlace::Blackout, StageCraftTags::Panel_Playback, TEXT("Blackout") },
			{ EStageDiagnosticPlace::Audio, StageCraftTags::Panel_Audio, TEXT("SourceFile") },
			{ EStageDiagnosticPlace::AudioKill, StageCraftTags::Panel_Playback, TEXT("AudioKill") },
			{ EStageDiagnosticPlace::Programmer, StageCraftTags::Panel_Groups, TEXT("ClearProgrammer") },
			{ EStageDiagnosticPlace::Effects, StageCraftTags::Panel_Effects, TEXT("StopAll") },
		};
		for (const FTarget& Target : Targets)
		{
			if (Target.Place == Place)
			{
				OutPanel = Target.Panel;
				OutControl = Target.Control;
				return true;
			}
		}
		return false;
	}

	const FStageUIAnchor* Find(const FGameplayTag& Panel, FName ControlId)
	{
		for (const FTable& Table : GetTables())
		{
			if (Table.Panel == Panel)
			{
				return Table.Anchors.FindByPredicate([ControlId](const FStageUIAnchor& Anchor) { return Anchor.ControlId == ControlId; });
			}
		}
		return nullptr;
	}

	const FStageUIAnchor& Get(TConstArrayView<FStageUIAnchor> Anchors, FName ControlId)
	{
		if (const FStageUIAnchor* Anchor = Anchors.FindByPredicate([ControlId](const FStageUIAnchor& Candidate) { return Candidate.ControlId == ControlId; }))
		{
			return *Anchor;
		}
		ensureMsgf(false, TEXT("No UI anchor named %s in this panel's table."), *ControlId.ToString());
		static const FStageUIAnchor Empty;
		return Empty;
	}

	bool IsShown(const UWidget& Widget)
	{
		for (const UWidget* Current = &Widget; Current; Current = GetContainer(*Current))
		{
			if (!Current->IsVisible())
			{
				return false;
			}
		}
		return true;
	}

	void BringIntoView(UWidget& Widget)
	{
		for (UWidget* Container = GetContainer(Widget); Container; Container = GetContainer(*Container))
		{
			if (UScrollBox* Scroll = Cast<UScrollBox>(Container))
			{
				Scroll->ScrollWidgetIntoView(&Widget, /*AnimateScroll*/ false, EDescendantScrollDestination::IntoView);
			}
		}
	}

	void Pulse(UWidget& Widget)
	{
		const double Now = FPlatformTime::Seconds();
		for (FPulse& Existing : Pulses)
		{
			if (Existing.Widget.Get() == &Widget)
			{
				Existing.StartSeconds = Now;
				return;
			}
		}
		const float Width = Widget.GetCachedGeometry().GetLocalSize().X;
		Pulses.Add({ &Widget, Now, Widget.GetRenderOpacity(), Widget.GetRenderTransform(), Width > 0.f && Width <= MaxSwellWidth });
		if (!PulseTicker.IsValid())
		{
			PulseTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickPulses));
		}
	}
}
