// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageShowDiagnostics.h"

#include "Algo/Sort.h"
#include "Show/StageEffectTypes.h"

#define LOCTEXT_NAMESPACE "StageShowDiagnostics"

using StageAILanguage::Pick;

bool FStageDiagnostic::operator==(const FStageDiagnostic& Other) const
{
	return Severity == Other.Severity && Code == Other.Code && Place == Other.Place && FixtureIds == Other.FixtureIds && Cues == Other.Cues
		&& Playback == Other.Playback && Universes == Other.Universes && Error.EqualTo(Other.Error) && Level == Other.Level;
}

namespace StageShowDiagnostics
{
	namespace
	{
		FStageDiagnostic Make(EStageDiagnosticSeverity Severity, EStageDiagnosticCode Code, EStageDiagnosticPlace Place)
		{
			FStageDiagnostic Diagnostic;
			Diagnostic.Severity = Severity;
			Diagnostic.Code = Code;
			Diagnostic.Place = Place;
			return Diagnostic;
		}

		void SortUnique(TArray<int32>& Numbers)
		{
			Numbers.Sort();
			for (int32 Index = Numbers.Num() - 1; Index > 0; --Index)
			{
				if (Numbers[Index] == Numbers[Index - 1])
				{
					Numbers.RemoveAt(Index, EAllowShrinking::No);
				}
			}
		}

		/** One cue list's missing fixtures and palettes, as at most one finding of each code (Playback INDEX_NONE: the main list). */
		void CheckCueList(TConstArrayView<FShowCue> Cues, int32 Playback, const TSet<int32>& OnStage, const TSet<FGuid>& Palettes, TArray<FStageDiagnostic>& Out)
		{
			const EStageDiagnosticPlace Place = Playback == INDEX_NONE ? EStageDiagnosticPlace::Cues : EStageDiagnosticPlace::Playback;
			FStageDiagnostic Missing = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::CueNamesMissingFixture, Place);
			FStageDiagnostic References = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::PaletteReferenceMissing, Place);
			Missing.Playback = References.Playback = Playback;
			for (const FShowCue& Cue : Cues)
			{
				bool bMissing = false;
				bool bReference = false;
				for (const FShowCueFixtureState& Entry : Cue.Fixtures)
				{
					if (!OnStage.Contains(Entry.FixtureId))
					{
						Missing.FixtureIds.Add(Entry.FixtureId);
						bMissing = true;
					}
					for (const TPair<FGameplayTag, FGuid>& Reference : Entry.PaletteRefs)
					{
						if (!Palettes.Contains(Reference.Value))
						{
							References.FixtureIds.Add(Entry.FixtureId);
							bReference = true;
						}
					}
				}
				if (bMissing)
				{
					Missing.Cues.Add(Cue.Number);
				}
				if (bReference)
				{
					References.Cues.Add(Cue.Number);
				}
			}
			for (FStageDiagnostic* Finding : { &References, &Missing })
			{
				if (!Finding->Cues.IsEmpty())
				{
					SortUnique(Finding->FixtureIds);
					Finding->Cues.Sort();
					Out.Add(MoveTemp(*Finding));
				}
			}
		}

		void CheckPatch(const FStageShowSnapshot& Snapshot, TArray<FStageDiagnostic>& Out)
		{
			if (!Snapshot.ConflictingFixtureIds.IsEmpty())
			{
				FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Error, EStageDiagnosticCode::PatchConflict, EStageDiagnosticPlace::Patch);
				Diagnostic.FixtureIds = Snapshot.ConflictingFixtureIds;
				SortUnique(Diagnostic.FixtureIds);
				Out.Add(MoveTemp(Diagnostic));
			}
			// Without DMX in or out an address does nothing, so a previs-only stage is not nagged about it.
			const bool bUsesDMX = Snapshot.OutputState != EStageDMXOutputState::Off || Snapshot.ControlSource == EStageControlSource::External;
			FStageDiagnostic Unpatched = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::Unpatched, EStageDiagnosticPlace::Patch);
			FStageDiagnostic Muted = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::UniverseMuted, EStageDiagnosticPlace::DMXOut);
			for (const FStageShowSnapshot::FFixture& Fixture : Snapshot.Fixtures)
			{
				if (!Fixture.Patch.IsPatched())
				{
					Unpatched.FixtureIds.Add(Fixture.Id);
				}
				else if (Snapshot.OutputState == EStageDMXOutputState::Sending && Snapshot.MutedUniverses.Contains(Fixture.Patch.Universe))
				{
					Muted.FixtureIds.Add(Fixture.Id);
					Muted.Universes.AddUnique(Fixture.Patch.Universe);
				}
			}
			if (bUsesDMX && !Unpatched.FixtureIds.IsEmpty())
			{
				SortUnique(Unpatched.FixtureIds);
				Out.Add(MoveTemp(Unpatched));
			}
			if (!Muted.FixtureIds.IsEmpty())
			{
				SortUnique(Muted.FixtureIds);
				Muted.Universes.Sort();
				Out.Add(MoveTemp(Muted));
			}
		}

		void CheckDMX(const FStageShowSnapshot& Snapshot, TArray<FStageDiagnostic>& Out)
		{
			const bool bExternal = Snapshot.ControlSource == EStageControlSource::External;
			if (Snapshot.InputState == EStageDMXInputState::Failed)
			{
				FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Error, EStageDiagnosticCode::DMXInFailed, EStageDiagnosticPlace::DMXIn);
				Diagnostic.Error = Snapshot.InputError;
				Out.Add(MoveTemp(Diagnostic));
			}
			else if (bExternal && Snapshot.InputState == EStageDMXInputState::Off)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Error, EStageDiagnosticCode::DMXInOff, EStageDiagnosticPlace::DMXIn));
			}
			else if (bExternal && Snapshot.InputState == EStageDMXInputState::Listening)
			{
				FStageDiagnostic Silent = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::DMXInNoSignal, EStageDiagnosticPlace::DMXIn);
				for (const FStageShowSnapshot::FFixture& Fixture : Snapshot.Fixtures)
				{
					if (Fixture.Patch.IsPatched() && !Snapshot.LiveUniverses.Contains(Fixture.Patch.Universe))
					{
						Silent.FixtureIds.Add(Fixture.Id);
						Silent.Universes.AddUnique(Fixture.Patch.Universe);
					}
				}
				if (!Silent.Universes.IsEmpty())
				{
					SortUnique(Silent.FixtureIds);
					Silent.Universes.Sort();
					Out.Add(MoveTemp(Silent));
				}
			}

			if (Snapshot.OutputState == EStageDMXOutputState::Failed)
			{
				FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Error, EStageDiagnosticCode::DMXOutFailed, EStageDiagnosticPlace::DMXOut);
				Diagnostic.Error = Snapshot.OutputError;
				Out.Add(MoveTemp(Diagnostic));
			}
			else if (bExternal && Snapshot.OutputState == EStageDMXOutputState::Sending)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Info, EStageDiagnosticCode::OutputPausedUnderExternal, EStageDiagnosticPlace::ControlSource));
			}
			if (bExternal && Snapshot.bEffectsRunning)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Info, EStageDiagnosticCode::EffectsHeldUnderExternal, EStageDiagnosticPlace::ControlSource));
			}
		}

		void CheckMasters(const FStageShowSnapshot& Snapshot, TArray<FStageDiagnostic>& Out)
		{
			if (Snapshot.bBlackout)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::BlackoutOn, EStageDiagnosticPlace::Blackout));
			}
			if (Snapshot.GrandMaster <= GrandMasterLowLevel)
			{
				FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::GrandMasterLow, EStageDiagnosticPlace::GrandMaster);
				Diagnostic.Level = FMath::Clamp(Snapshot.GrandMaster, 0.f, 1.f);
				Out.Add(MoveTemp(Diagnostic));
			}
		}

		void CheckAudio(const FStageShowSnapshot& Snapshot, TArray<FStageDiagnostic>& Out)
		{
			if (Snapshot.AudioState == EStageAudioSourceState::Failed)
			{
				FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Error, EStageDiagnosticCode::AudioSourceFailed, EStageDiagnosticPlace::Audio);
				Diagnostic.Error = Snapshot.AudioError;
				Out.Add(MoveTemp(Diagnostic));
			}
			// The rest only matters while effects follow the music (and run: under External they hold anyway).
			if (!Snapshot.bAudioEffectsRunning || Snapshot.ControlSource == EStageControlSource::External)
			{
				return;
			}
			if (Snapshot.AudioSource == EStageAudioSource::Off)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::AudioNoSource, EStageDiagnosticPlace::Audio));
			}
			else if (Snapshot.AudioState == EStageAudioSourceState::Running && !Snapshot.bAudioSignal)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Warning, EStageDiagnosticCode::AudioNoSignal, EStageDiagnosticPlace::Audio));
			}
			if (Snapshot.bAudioKill)
			{
				Out.Add(Make(EStageDiagnosticSeverity::Info, EStageDiagnosticCode::AudioKilled, EStageDiagnosticPlace::AudioKill));
			}
		}
	}

	TArray<FStageDiagnostic> Run(const FStageShowSnapshot& Snapshot)
	{
		TArray<FStageDiagnostic> Out;
		CheckPatch(Snapshot, Out);
		CheckDMX(Snapshot, Out);
		CheckMasters(Snapshot, Out);

		TSet<int32> OnStage;
		OnStage.Reserve(Snapshot.Fixtures.Num());
		for (const FStageShowSnapshot::FFixture& Fixture : Snapshot.Fixtures)
		{
			OnStage.Add(Fixture.Id);
		}
		CheckCueList(Snapshot.Cues, INDEX_NONE, OnStage, Snapshot.Palettes, Out);
		for (int32 Index = 0; Index < Snapshot.Playbacks.Num(); ++Index)
		{
			CheckCueList(Snapshot.Playbacks[Index].Cues, Index, OnStage, Snapshot.Palettes, Out);
		}

		CheckAudio(Snapshot, Out);
		if (!Snapshot.ProgrammerFixtureIds.IsEmpty() && Snapshot.ControlSource == EStageControlSource::Internal)
		{
			FStageDiagnostic Diagnostic = Make(EStageDiagnosticSeverity::Info, EStageDiagnosticCode::ProgrammerHolds, EStageDiagnosticPlace::Programmer);
			Diagnostic.FixtureIds = Snapshot.ProgrammerFixtureIds;
			SortUnique(Diagnostic.FixtureIds);
			Out.Add(MoveTemp(Diagnostic));
		}

		// Worst first; within a severity, in code order, then the main list before the playbacks: a stable, reproducible list.
		Algo::StableSort(Out, [](const FStageDiagnostic& A, const FStageDiagnostic& B)
		{
			if (A.Severity != B.Severity)
			{
				return A.Severity > B.Severity;
			}
			if (A.Code != B.Code)
			{
				return A.Code < B.Code;
			}
			return A.Playback < B.Playback;
		});
		return Out;
	}

	FString JoinNumbers(TConstArrayView<int32> Numbers, EStageAILanguage Language, int32 MaxShown)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Numbers.Num() && Index < MaxShown; ++Index)
		{
			Parts.Add(FString::FromInt(Numbers[Index]));
		}
		FString Joined = FString::Join(Parts, TEXT(", "));
		if (Numbers.Num() > MaxShown)
		{
			Joined += Language == EStageAILanguage::Armenian ? FString::Printf(TEXT(" և ևս %d"), Numbers.Num() - MaxShown) : FString::Printf(TEXT(" and %d more"), Numbers.Num() - MaxShown);
		}
		return Joined;
	}

	FString JoinCues(TConstArrayView<float> Numbers, EStageAILanguage Language, int32 MaxShown)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Numbers.Num() && Index < MaxShown; ++Index)
		{
			Parts.Add(FString::Printf(TEXT("%g"), Numbers[Index]));
		}
		FString Joined = FString::Join(Parts, TEXT(", "));
		if (Numbers.Num() > MaxShown)
		{
			Joined += Language == EStageAILanguage::Armenian ? FString::Printf(TEXT(" և ևս %d"), Numbers.Num() - MaxShown) : FString::Printf(TEXT(" and %d more"), Numbers.Num() - MaxShown);
		}
		return Joined;
	}

	FText Describe(const FStageDiagnostic& Diagnostic, EStageAILanguage Language)
	{
		const FText Fixtures = FText::FromString(JoinNumbers(Diagnostic.FixtureIds, Language));
		const FText Count = FText::AsNumber(Diagnostic.FixtureIds.Num());
		const FText Cues = FText::FromString(JoinCues(Diagnostic.Cues, Language));
		const FText Universes = FText::FromString(JoinNumbers(Diagnostic.Universes, Language));
		const FText Playback = FText::AsNumber(Diagnostic.Playback + 1);
		const FText CueCount = FText::AsNumber(Diagnostic.Cues.Num());
		const FText Error = Diagnostic.Error.IsEmpty() ? FText::GetEmpty() : FText::Format(INVTEXT(" ({0})"), Diagnostic.Error);
		const bool bInPlayback = Diagnostic.Playback != INDEX_NONE;
		switch (Diagnostic.Code)
		{
		case EStageDiagnosticCode::PatchConflict:
			return FText::Format(Pick(Language,
				LOCTEXT("PatchConflict", "DMX addresses overlap, or run past channel 512, for fixtures {0}: a real fixture would follow the wrong channels. FIX CONFLICTS in the Patch panel moves them apart."),
				INVTEXT("{0} սարքերի DMX հասցեները համընկնում են կամ անցնում են 512-րդ ալիքից. իրական սարքը կհետևի սխալ ալիքներին։ Patch վահանակի FIX CONFLICTS-ը կբաժանի դրանք։")), Fixtures);
		case EStageDiagnosticCode::Unpatched:
			return FText::Format(Pick(Language,
				LOCTEXT("Unpatched", "{1} {1}|plural(one=fixture has,other=fixtures have) no DMX address ({0}): DMX does not reach {1}|plural(one=it,other=them). AUTO PATCH in the Patch panel gives each the first free address."),
				INVTEXT("{1} սարք DMX հասցե չունի ({0}). DMX-ը դրանց չի հասնում։ Patch վահանակի AUTO PATCH-ը յուրաքանչյուրին կտա առաջին ազատ հասցեն։")), Fixtures, Count);
		case EStageDiagnosticCode::DMXInOff:
			return Pick(Language,
				LOCTEXT("DMXInOff", "The show follows DMX IN (EXTERNAL DMX), but DMX IN is off, so nothing drives the fixtures. Turn DMX IN on in the Patch panel, or switch the Cues panel to INTERNAL."),
				INVTEXT("Շոուն հետևում է DMX IN-ին (EXTERNAL DMX), բայց DMX IN-ն անջատված է, և սարքերը ոչինչ չի կառավարում։ Միացրեք DMX IN-ը Patch վահանակում կամ Cues վահանակում ընտրեք INTERNAL։"));
		case EStageDiagnosticCode::DMXInFailed:
			return FText::Format(Pick(Language,
				LOCTEXT("DMXInFailed", "DMX IN could not open its ports{0}. Another program may hold ports 6454 and 5568, or the network card is wrong."),
				INVTEXT("DMX IN-ը չկարողացավ բացել իր պորտերը{0}։ Հնարավոր է՝ 6454 և 5568 պորտերը զբաղեցրել է այլ ծրագիր, կամ ցանցային քարտը սխալ է։")), Error);
		case EStageDiagnosticCode::DMXInNoSignal:
			return FText::Format(Pick(Language,
				LOCTEXT("DMXInNoSignal", "No console is sending universe {0}: its {2} {2}|plural(one=fixture holds,other=fixtures hold) the last values received ({1}). Check the console's output and the network."),
				INVTEXT("Ոչ մի կոնսոլ չի ուղարկում {0} յունիվերսը. դրա {2} սարքերը պահում են վերջին ստացված արժեքները ({1})։ Ստուգեք կոնսոլի ելքը և ցանցը։")), Universes, Fixtures, Count);
		case EStageDiagnosticCode::DMXOutFailed:
			return FText::Format(Pick(Language,
				LOCTEXT("DMXOutFailed", "DMX OUT failed{0}, so real fixtures get nothing. Check the protocol, the unicast address and the network card in the Patch panel."),
				INVTEXT("DMX OUT-ը ձախողվեց{0}, և իրական սարքերը ոչինչ չեն ստանում։ Ստուգեք պրոտոկոլը, unicast հասցեն և ցանցային քարտը Patch վահանակում։")), Error);
		case EStageDiagnosticCode::UniverseMuted:
			return FText::Format(Pick(Language,
				LOCTEXT("UniverseMuted", "Universe {0} is muted on DMX OUT, so {2} patched {2}|plural(one=fixture gets,other=fixtures get) nothing ({1})."),
				INVTEXT("{0} յունիվերսը DMX OUT-ում լռեցված է, և {2} պատչված սարքեր ոչինչ չեն ստանում ({1})։")), Universes, Fixtures, Count);
		case EStageDiagnosticCode::OutputPausedUnderExternal:
			return Pick(Language,
				LOCTEXT("OutputPaused", "DMX OUT is on but paused: it sends only while the cue list drives the show (INTERNAL), not while DMX IN follows a console."),
				INVTEXT("DMX OUT-ը միացված է, բայց դադարեցված. այն ուղարկում է միայն երբ շոուն կառավարում է կյուների ցանկը (INTERNAL), ոչ երբ DMX IN-ը հետևում է կոնսոլին։"));
		case EStageDiagnosticCode::EffectsHeldUnderExternal:
			return Pick(Language,
				LOCTEXT("EffectsHeld", "Effects hold still while DMX IN follows a console (EXTERNAL DMX); they run again under INTERNAL."),
				INVTEXT("Էֆեկտները կանգնած են, քանի դեռ DMX IN-ը հետևում է կոնսոլին (EXTERNAL DMX). INTERNAL-ի դեպքում դրանք նորից կաշխատեն։"));
		case EStageDiagnosticCode::PaletteReferenceMissing:
			return bInPlayback
				? FText::Format(Pick(Language,
					LOCTEXT("PaletteMissingPlayback", "Playback {1}'s {2}|plural(one=cue,other=cues) {0} {2}|plural(one=follows,other=follow) palettes that were deleted; {2}|plural(one=it uses the values stored with the cue,other=they use the values stored with the cues) instead."),
					INVTEXT("{1} փլեյբեքի {0} կյուները հետևում են ջնջված պալիտրաների. դրանց փոխարեն օգտագործվում են կյուի հետ պահված արժեքները։")), Cues, Playback, CueCount)
				: FText::Format(Pick(Language,
					LOCTEXT("PaletteMissing", "{2}|plural(one=Cue,other=Cues) {0} {2}|plural(one=follows,other=follow) palettes that were deleted; {2}|plural(one=it uses the values stored with the cue,other=they use the values stored with the cues) instead."),
					INVTEXT("{0} կյուները հետևում են ջնջված պալիտրաների. դրանց փոխարեն օգտագործվում են կյուի հետ պահված արժեքները։")), Cues, Playback, CueCount);
		case EStageDiagnosticCode::CueNamesMissingFixture:
			return bInPlayback
				? FText::Format(Pick(Language,
					LOCTEXT("MissingFixturePlayback", "Playback {2}'s {3}|plural(one=cue,other=cues) {0} {3}|plural(one=names,other=name) fixtures that are not on stage ({1}); those values do nothing."),
					INVTEXT("{2} փլեյբեքի {0} կյուները նշում են բեմում չեղած սարքեր ({1}). այդ արժեքները ոչինչ չեն անում։")), Cues, Fixtures, Playback, CueCount)
				: FText::Format(Pick(Language,
					LOCTEXT("MissingFixture", "{3}|plural(one=Cue,other=Cues) {0} {3}|plural(one=names,other=name) fixtures that are not on stage ({1}); those values do nothing."),
					INVTEXT("{0} կյուները նշում են բեմում չեղած սարքեր ({1}). այդ արժեքները ոչինչ չեն անում։")), Cues, Fixtures, Playback, CueCount);
		case EStageDiagnosticCode::BlackoutOn:
			return Pick(Language,
				LOCTEXT("Blackout", "BLACKOUT is on: every light is at zero. Click BLACKOUT in the Playback panel (Ctrl+Shift+B) to bring them back."),
				INVTEXT("BLACKOUT-ը միացված է. բոլոր լույսերը զրոյի վրա են։ Սեղմեք BLACKOUT-ը Playback վահանակում (Ctrl+Shift+B)՝ դրանք վերադարձնելու համար։"));
		case EStageDiagnosticCode::GrandMasterLow:
			return FText::Format(Pick(Language,
				LOCTEXT("GrandMasterLow", "The Grand Master is at {0}, so every light is at most that bright. Raise GM in the Playback panel."),
				INVTEXT("Grand Master-ը {0} է, և ոչ մի լույս դրանից պայծառ չէ։ Բարձրացրեք GM-ը Playback վահանակում։")), FText::AsPercent(Diagnostic.Level));
		case EStageDiagnosticCode::AudioSourceFailed:
			return FText::Format(Pick(Language,
				LOCTEXT("AudioFailed", "The music source failed{0}. Choose it again in the Audio panel."),
				INVTEXT("Երաժշտության աղբյուրը ձախողվեց{0}։ Ընտրեք այն նորից Audio վահանակում։")), Error);
		case EStageDiagnosticCode::AudioNoSource:
			return Pick(Language,
				LOCTEXT("AudioNoSource", "Effects follow the music, but no music source is chosen: they hold their pattern or stay fully lit. Pick FILE, INPUT, SYSTEM or TEST in the Audio panel."),
				INVTEXT("Էֆեկտները հետևում են երաժշտությանը, բայց աղբյուր ընտրված չէ. դրանք պահում են իրենց նախշը կամ մնում են լրիվ վառ։ Audio վահանակում ընտրեք FILE, INPUT, SYSTEM կամ TEST։"));
		case EStageDiagnosticCode::AudioNoSignal:
			return Pick(Language,
				LOCTEXT("AudioNoSignal", "Effects follow the music, but the source hears nothing. Check that the music plays, the input device and its GAIN in the Audio panel."),
				INVTEXT("Էֆեկտները հետևում են երաժշտությանը, բայց աղբյուրը ոչինչ չի լսում։ Ստուգեք, որ երաժշտությունը հնչում է, մուտքային սարքը և դրա GAIN-ը Audio վահանակում։"));
		case EStageDiagnosticCode::AudioKilled:
			return Pick(Language,
				LOCTEXT("AudioKilled", "AUDIO KILL is on: effects run as without music. Click KILL in the Playback panel to follow the music again."),
				INVTEXT("AUDIO KILL-ը միացված է. էֆեկտներն աշխատում են առանց երաժշտության։ Սեղմեք KILL-ը Playback վահանակում՝ նորից երաժշտությանը հետևելու համար։"));
		case EStageDiagnosticCode::ProgrammerHolds:
			return FText::Format(Pick(Language,
				LOCTEXT("ProgrammerHolds", "The programmer holds values for {1} {1}|plural(one=fixture,other=fixtures) ({0}): they ignore cues and playbacks for what it holds until it is cleared."),
				INVTEXT("Պրոգրամերը պահում է {1} սարքի արժեքներ ({0}). դրանք անտեսում են կյուներն ու փլեյբեքները, մինչև պրոգրամերը մաքրվի։")), Fixtures, Count);
		default:
			return FText::GetEmpty();
		}
	}
}

#undef LOCTEXT_NAMESPACE
