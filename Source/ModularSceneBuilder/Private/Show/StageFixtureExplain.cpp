// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageFixtureExplain.h"

#define LOCTEXT_NAMESPACE "StageFixtureExplain"

using StageAILanguage::Pick;

namespace StageFixtureExplain
{
	namespace
	{
		FText Cue(float Number)
		{
			return FText::FromString(FString::Printf(TEXT("%g"), Number));
		}

		FText Address(const FStageDMXPatch& Patch)
		{
			return FText::FromString(FString::Printf(TEXT("%d.%03d"), Patch.Universe, Patch.Address));
		}

		/** Where the dimmer's static value came from, as a phrase ("from cue 2, tracked into the playing cue 4"). */
		FText DescribeSource(const FStageFixtureFacts& Facts, EStageAILanguage Language)
		{
			switch (Facts.Dimmer.Layer)
			{
			case StageShowMix::ELayer::CueList:
				if (Facts.DimmerCue.IsSet() && Facts.ActiveCue.IsSet() && *Facts.DimmerCue != *Facts.ActiveCue)
				{
					return FText::Format(Pick(Language, LOCTEXT("FromTrackedCue", "from cue {0}, tracked into the playing cue {1}"),
						INVTEXT("{0} կյուից՝ թրեքով մինչև ընթացիկ {1} կյուն")), Cue(*Facts.DimmerCue), Cue(*Facts.ActiveCue));
				}
				return Facts.ActiveCue.IsSet()
					? FText::Format(Pick(Language, LOCTEXT("FromCue", "from the playing cue {0}"), INVTEXT("ընթացիկ {0} կյուից")), Cue(*Facts.ActiveCue))
					: Pick(Language, LOCTEXT("FromCueList", "from the cue list"), INVTEXT("կյուների ցանկից"));
			case StageShowMix::ELayer::Playback:
				return FText::Format(Pick(Language, LOCTEXT("FromPlayback", "from playback {0} \"{1}\" with its fader at {2}"),
					INVTEXT("{0} փլեյբեքից («{1}»)՝ ֆեյդերը {2}")), FText::AsNumber(Facts.Dimmer.Playback + 1), Facts.PlaybackName, FText::AsPercent(Facts.PlaybackLevel));
			case StageShowMix::ELayer::Programmer:
				return Pick(Language, LOCTEXT("FromProgrammer", "held in the programmer"), INVTEXT("պահված է պրոգրամերում"));
			case StageShowMix::ELayer::Highlight:
				return Pick(Language, LOCTEXT("FromHighlight", "from HIGHLIGHT"), INVTEXT("HIGHLIGHT-ից"));
			case StageShowMix::ELayer::Home:
				return Pick(Language, LOCTEXT("FromHome", "from its home look (no cue, playback or programmer sets it)"),
					INVTEXT("իր սկզբնական տեսքից (ոչ կյուն, ոչ փլեյբեքը, ոչ պրոգրամերը այն չեն սահմանում)"));
			default:
				return Facts.ControlSource == EStageControlSource::External
					? Pick(Language, LOCTEXT("FromConsole", "from DMX IN (the console on the network)"), INVTEXT("DMX IN-ից (ցանցի կոնսոլից)"))
					: Pick(Language, LOCTEXT("FromNothing", "not set by any cue, playback or the programmer"), INVTEXT("ոչ մի կյու, փլեյբեք կամ պրոգրամեր այն չի սահմանում"));
			}
		}

		EStageDiagnosticPlace PlaceOfLayer(StageShowMix::ELayer Layer)
		{
			switch (Layer)
			{
			case StageShowMix::ELayer::CueList:
				return EStageDiagnosticPlace::Cues;
			case StageShowMix::ELayer::Playback:
				return EStageDiagnosticPlace::Playback;
			case StageShowMix::ELayer::Programmer:
				return EStageDiagnosticPlace::Programmer;
			default:
				return EStageDiagnosticPlace::None;
			}
		}

		EStageDarkCause FindDarkCause(const FStageFixtureFacts& Facts, EStageDiagnosticPlace& OutPlace)
		{
			const bool bDimmerDark = Facts.bHasDimmer && Facts.OutputDimmer < DarkLevel;
			const bool bColorDark = Facts.ColorFraction < DarkLevel;
			const bool bIrisClosed = Facts.bHasIris && Facts.Iris >= 0.99f;
			if (!bDimmerDark && !bColorDark && !bIrisClosed)
			{
				return EStageDarkCause::None;
			}
			// Along the chain: the input first (under External nothing else feeds the fixture), then the masters over everything,
			// then the layer that set the dimmer, then what the beam does with the light.
			if (Facts.ControlSource == EStageControlSource::External)
			{
				if (Facts.InputState != EStageDMXInputState::Listening)
				{
					OutPlace = EStageDiagnosticPlace::DMXIn;
					return EStageDarkCause::NoInput;
				}
				if (!Facts.Patch.IsPatched() || !Facts.bUniverseLive)
				{
					OutPlace = Facts.Patch.IsPatched() ? EStageDiagnosticPlace::DMXIn : EStageDiagnosticPlace::Patch;
					return EStageDarkCause::NoInputSignal;
				}
			}
			if (bDimmerDark)
			{
				if (Facts.bBlackout)
				{
					OutPlace = EStageDiagnosticPlace::Blackout;
					return EStageDarkCause::Blackout;
				}
				const float BeforeMasters = Facts.Dimmer.IntensityScale > UE_SMALL_NUMBER ? Facts.Dimmer.Value / Facts.Dimmer.IntensityScale : Facts.Dimmer.StaticValue;
				if (Facts.Dimmer.bScaledByMasters && BeforeMasters >= DarkLevel)
				{
					OutPlace = EStageDiagnosticPlace::GrandMaster;
					return EStageDarkCause::GrandMaster;
				}
				if (Facts.Dimmer.bEffect && Facts.Dimmer.StaticValue >= DarkLevel)
				{
					OutPlace = EStageDiagnosticPlace::Effects;
					return EStageDarkCause::Effect;
				}
				OutPlace = PlaceOfLayer(Facts.Dimmer.Layer);
				return EStageDarkCause::Dimmer;
			}
			OutPlace = EStageDiagnosticPlace::None;
			return bColorDark ? EStageDarkCause::Color : EStageDarkCause::IrisClosed;
		}

		EStageOutputCause FindOutputCause(const FStageFixtureFacts& Facts)
		{
			if (Facts.OutputState == EStageDMXOutputState::Failed)
			{
				return EStageOutputCause::OutputFailed;
			}
			if (Facts.OutputState == EStageDMXOutputState::Off)
			{
				return EStageOutputCause::OutputOff;
			}
			if (!Facts.Patch.IsPatched())
			{
				return EStageOutputCause::Unpatched;
			}
			if (Facts.bPatchConflict)
			{
				return EStageOutputCause::PatchConflict;
			}
			if (Facts.ControlSource == EStageControlSource::External)
			{
				return EStageOutputCause::OutputPaused;
			}
			return Facts.bUniverseMuted ? EStageOutputCause::UniverseMuted : EStageOutputCause::None;
		}

		EStageDiagnosticPlace PlaceOfOutput(EStageOutputCause Cause)
		{
			switch (Cause)
			{
			case EStageOutputCause::OutputOff:
			case EStageOutputCause::OutputFailed:
			case EStageOutputCause::UniverseMuted:
				return EStageDiagnosticPlace::DMXOut;
			case EStageOutputCause::Unpatched:
			case EStageOutputCause::PatchConflict:
				return EStageDiagnosticPlace::Patch;
			case EStageOutputCause::OutputPaused:
				return EStageDiagnosticPlace::ControlSource;
			default:
				return EStageDiagnosticPlace::None;
			}
		}

		FText JoinTexts(TConstArrayView<FText> Texts)
		{
			TArray<FString> Parts;
			for (const FText& Text : Texts)
			{
				Parts.Add(FString::Printf(TEXT("\"%s\""), *Text.ToString()));
			}
			return FText::FromString(FString::Join(Parts, TEXT(", ")));
		}

		FText DescribeVerdict(const FStageFixtureFacts& Facts, EStageDarkCause Cause, const FText& Fixture, EStageAILanguage Language)
		{
			switch (Cause)
			{
			case EStageDarkCause::None:
				return Facts.bHasDimmer
					? FText::Format(Pick(Language, LOCTEXT("Lit", "{0} is lit: its dimmer is at {1}."), INVTEXT("{0} վառ է. դրա դիմմերը {1} է։")), Fixture, FText::AsPercent(Facts.OutputDimmer))
					: FText::Format(Pick(Language, LOCTEXT("LitNoDimmer", "{0} is lit."), INVTEXT("{0} վառ է։")), Fixture);
			case EStageDarkCause::NoInput:
				return FText::Format(Pick(Language, LOCTEXT("DarkNoInput", "{0} is dark: the show follows DMX IN (EXTERNAL DMX), but DMX IN is off. Turn it on in the Patch panel, or switch the Cues panel to INTERNAL."),
					INVTEXT("{0} մութ է. շոուն հետևում է DMX IN-ին (EXTERNAL DMX), բայց DMX IN-ն անջատված է։ Միացրեք այն Patch վահանակում կամ Cues վահանակում ընտրեք INTERNAL։")), Fixture);
			case EStageDarkCause::NoInputSignal:
				return Facts.Patch.IsPatched()
					? FText::Format(Pick(Language, LOCTEXT("DarkNoSignal", "{0} is dark: the show follows DMX IN, but no console sends universe {1}."),
						INVTEXT("{0} մութ է. շոուն հետևում է DMX IN-ին, բայց ոչ մի կոնսոլ չի ուղարկում {1} յունիվերսը։")), Fixture, FText::AsNumber(Facts.Patch.Universe))
					: FText::Format(Pick(Language, LOCTEXT("DarkNoAddress", "{0} is dark: the show follows DMX IN, but the fixture has no DMX address, so the console cannot reach it."),
						INVTEXT("{0} մութ է. շոուն հետևում է DMX IN-ին, բայց սարքը DMX հասցե չունի, և կոնսոլը չի կարող հասնել դրան։")), Fixture);
			case EStageDarkCause::Blackout:
				return FText::Format(Pick(Language, LOCTEXT("DarkBlackout", "{0} is dark: BLACKOUT is on (Playback panel, Ctrl+Shift+B)."),
					INVTEXT("{0} մութ է. BLACKOUT-ը միացված է (Playback վահանակ, Ctrl+Shift+B)։")), Fixture);
			case EStageDarkCause::GrandMaster:
				return FText::Format(Pick(Language, LOCTEXT("DarkGrandMaster", "{0} is dark: the Grand Master is at {1}."),
					INVTEXT("{0} մութ է. Grand Master-ը {1} է։")), Fixture, FText::AsPercent(Facts.GrandMaster));
			case EStageDarkCause::Dimmer:
				return FText::Format(Pick(Language, LOCTEXT("DarkDimmer", "{0} is dark: its dimmer is at 0, {1}."),
					INVTEXT("{0} մութ է. դրա դիմմերը 0 է՝ {1}։")), Fixture, DescribeSource(Facts, Language));
			case EStageDarkCause::Effect:
				return FText::Format(Pick(Language, LOCTEXT("DarkEffect", "{0} is dark: the effect {1} takes its dimmer down."),
					INVTEXT("{0} մութ է. {1} էֆեկտը իջեցնում է դրա դիմմերը։")), Fixture, JoinTexts(Facts.IntensityEffects));
			case EStageDarkCause::Color:
				return FText::Format(Pick(Language, LOCTEXT("DarkColor", "{0} is dark: its colour lets no light out (every colour at 0, or full CMY)."),
					INVTEXT("{0} մութ է. դրա գույնը լույս դուրս չի թողնում (բոլոր գույները 0, կամ լրիվ CMY)։")), Fixture);
			case EStageDarkCause::IrisClosed:
				return FText::Format(Pick(Language, LOCTEXT("DarkIris", "{0} is dark: its iris is closed."), INVTEXT("{0} մութ է. դրա իրիսը փակ է։")), Fixture);
			default:
				return FText::GetEmpty();
			}
		}

		FText DescribeOutput(const FStageFixtureFacts& Facts, EStageOutputCause Cause, EStageAILanguage Language)
		{
			switch (Cause)
			{
			case EStageOutputCause::None:
				return FText::Format(Pick(Language, LOCTEXT("OutputOk", "patched at {0}; DMX OUT sends it."), INVTEXT("պատչված է {0} հասցեով. DMX OUT-ը ուղարկում է այն։")), Address(Facts.Patch));
			case EStageOutputCause::OutputOff:
				return Pick(Language, LOCTEXT("OutputOff", "DMX OUT is off, so a real fixture gets nothing (fine when you only visualise)."),
					INVTEXT("DMX OUT-ն անջատված է, և իրական սարքը ոչինչ չի ստանում (նորմալ է, եթե միայն վիզուալիզացիա եք անում)։"));
			case EStageOutputCause::OutputFailed:
				return Pick(Language, LOCTEXT("OutputFailed", "DMX OUT failed; the Patch panel shows why."), INVTEXT("DMX OUT-ը ձախողվեց. պատճառը ցույց է տալիս Patch վահանակը։"));
			case EStageOutputCause::OutputPaused:
				return Pick(Language, LOCTEXT("OutputPausedLine", "DMX OUT is paused while DMX IN follows a console."), INVTEXT("DMX OUT-ը դադարեցված է, քանի դեռ DMX IN-ը հետևում է կոնսոլին։"));
			case EStageOutputCause::Unpatched:
				return Pick(Language, LOCTEXT("OutputUnpatched", "it has no DMX address, so a real fixture gets nothing; AUTO PATCH in the Patch panel gives it one."),
					INVTEXT("այն DMX հասցե չունի, և իրական սարքը ոչինչ չի ստանում. Patch վահանակի AUTO PATCH-ը կտա հասցե։"));
			case EStageOutputCause::UniverseMuted:
				return FText::Format(Pick(Language, LOCTEXT("OutputMuted", "its universe {0} is muted on DMX OUT."), INVTEXT("դրա {0} յունիվերսը լռեցված է DMX OUT-ում։")), FText::AsNumber(Facts.Patch.Universe));
			case EStageOutputCause::PatchConflict:
				return FText::Format(Pick(Language, LOCTEXT("OutputConflict", "its address {0} overlaps another fixture's; FIX CONFLICTS in the Patch panel moves them apart."),
					INVTEXT("դրա {0} հասցեն համընկնում է այլ սարքի հասցեի հետ. Patch վահանակի FIX CONFLICTS-ը կբաժանի դրանք։")), Address(Facts.Patch));
			default:
				return FText::GetEmpty();
			}
		}
	}

	FStageFixtureVerdict Explain(const FStageFixtureFacts& Facts, EStageAILanguage Language)
	{
		FStageFixtureVerdict Verdict;
		const FText Fixture = Facts.Name.IsEmpty()
			? FText::Format(Pick(Language, LOCTEXT("FixtureNoName", "Fixture {0}"), INVTEXT("{0} սարքը")), FText::AsNumber(Facts.FixtureId))
			: FText::Format(Pick(Language, LOCTEXT("FixtureNamed", "Fixture {0} ({1})"), INVTEXT("{0} սարքը ({1})")), FText::AsNumber(Facts.FixtureId), FText::FromString(Facts.Name));
		if (!Facts.bExists)
		{
			Verdict.DarkCause = EStageDarkCause::NotFound;
			Verdict.Text = FText::Format(Pick(Language, LOCTEXT("NotFound", "There is no fixture {0} on stage. The Patch panel lists every fixture ID."),
				INVTEXT("Բեմում {0} սարք չկա։ Patch վահանակը ցույց է տալիս բոլոր սարքերի ID-ները։")), FText::AsNumber(Facts.FixtureId)).ToString();
			Verdict.Place = EStageDiagnosticPlace::Patch;
			return Verdict;
		}

		EStageDiagnosticPlace DarkPlace = EStageDiagnosticPlace::None;
		Verdict.DarkCause = FindDarkCause(Facts, DarkPlace);
		Verdict.OutputCause = FindOutputCause(Facts);
		Verdict.Place = Verdict.DarkCause != EStageDarkCause::None ? DarkPlace : PlaceOfOutput(Verdict.OutputCause);
		if (Verdict.DarkCause == EStageDarkCause::None && Verdict.OutputCause == EStageOutputCause::OutputOff)
		{
			Verdict.Place = EStageDiagnosticPlace::None; // Lit on screen with DMX OUT off is a normal previs stage.
		}

		TArray<FText> Lines;
		Lines.Add(DescribeVerdict(Facts, Verdict.DarkCause, Fixture, Language));

		// The chain, in signal order.
		if (Facts.ControlSource == EStageControlSource::External)
		{
			const FText Live = Facts.Patch.IsPatched()
				? (Facts.bUniverseLive
					? FText::Format(Pick(Language, LOCTEXT("UniverseLive", "universe {0} is live"), INVTEXT("{0} յունիվերսը ակտիվ է")), FText::AsNumber(Facts.Patch.Universe))
					: FText::Format(Pick(Language, LOCTEXT("UniverseSilent", "universe {0} has no source"), INVTEXT("{0} յունիվերսը աղբյուր չունի")), FText::AsNumber(Facts.Patch.Universe)))
				: Pick(Language, LOCTEXT("NoAddressIn", "no DMX address"), INVTEXT("DMX հասցե չկա"));
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("ControlExternal", "• Control: EXTERNAL DMX, DMX IN {0}; {1}."), INVTEXT("• Կառավարում՝ EXTERNAL DMX, DMX IN {0}. {1}։")),
				Facts.InputState == EStageDMXInputState::Listening ? Pick(Language, LOCTEXT("InputOn", "listening"), INVTEXT("լսում է"))
					: Pick(Language, LOCTEXT("InputOffWord", "off"), INVTEXT("անջատված")), Live));
		}
		else
		{
			Lines.Add(Pick(Language, LOCTEXT("ControlInternal", "• Control: INTERNAL, the cue list drives it."), INVTEXT("• Կառավարում՝ INTERNAL, այն կառավարում է կյուների ցանկը։")));
		}
		if (Facts.bHasDimmer)
		{
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("IntensityLine", "• Intensity: {0} {1}; output now {2}."), INVTEXT("• Ինտենսիվություն՝ {0} {1}. այժմ ելքը {2} է։")),
				FText::AsPercent(Facts.Dimmer.StaticValue), DescribeSource(Facts, Language), FText::AsPercent(Facts.OutputDimmer)));
		}
		if (!Facts.IntensityEffects.IsEmpty())
		{
			FText Effect = FText::Format(Pick(Language, LOCTEXT("EffectLine", "• Effect: {0} moves its intensity."), INVTEXT("• Էֆեկտ՝ {0}-ը շարժում է դրա ինտենսիվությունը։")), JoinTexts(Facts.IntensityEffects));
			if (Facts.bAudioEffect)
			{
				Effect = FText::Format(Pick(Language, LOCTEXT("EffectAudio", "{0} It follows the music: rises are limited to {1} flashes a second{2}."),
					INVTEXT("{0} Այն հետևում է երաժշտությանը. բարձրացումները սահմանափակված են վայրկյանում {1} բռնկումով{2}։")),
					Effect, FText::AsNumber(Facts.FlashLimitHz),
					Facts.bAudioKill ? Pick(Language, LOCTEXT("KillOn", ", and AUDIO KILL is on"), INVTEXT(", և AUDIO KILL-ը միացված է")) : FText::GetEmpty());
			}
			Lines.Add(Effect);
		}
		Lines.Add(FText::Format(Pick(Language, LOCTEXT("MastersLine", "• Masters: Grand Master {0}, BLACKOUT {1}."), INVTEXT("• Մաստերներ՝ Grand Master {0}, BLACKOUT {1}։")),
			FText::AsPercent(Facts.GrandMaster),
			Facts.bBlackout ? Pick(Language, LOCTEXT("On", "on"), INVTEXT("միացված")) : Pick(Language, LOCTEXT("Off", "off"), INVTEXT("անջատված"))));
		if (Facts.ColorFraction < 0.999f)
		{
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("ColorLine", "• Colour: lets out {0} of the fixture's light."), INVTEXT("• Գույն՝ դուրս է թողնում սարքի լույսի {0}-ը։")),
				FText::AsPercent(FMath::Clamp(Facts.ColorFraction, 0.f, 1.f))));
		}
		if (Facts.bHasIris && Facts.Iris > 0.f)
		{
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("IrisLine", "• Iris: {0} closed."), INVTEXT("• Իրիս՝ {0} փակ։")), FText::AsPercent(FMath::Clamp(Facts.Iris, 0.f, 1.f))));
		}
		Lines.Add(FText::Format(Pick(Language, LOCTEXT("RealLine", "• Real fixture: {0}"), INVTEXT("• Իրական սարք՝ {0}")), DescribeOutput(Facts, Verdict.OutputCause, Language)));

		TArray<FString> Strings;
		for (const FText& Line : Lines)
		{
			Strings.Add(Line.ToString());
		}
		Verdict.Text = FString::Join(Strings, TEXT("\n"));
		return Verdict;
	}
}

#undef LOCTEXT_NAMESPACE
