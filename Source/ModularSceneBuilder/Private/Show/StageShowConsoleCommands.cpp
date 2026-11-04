// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for fixtures, the patch and the strobe clock (Docs/ADR/0007 §5-6). Parameter edits go through
// AModularPlayerController::RequestParameterChange, the same validated path the inspector, the faders and the patch sheet use.
// Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Algo/Find.h"
#include "Actors/LightingFixtureActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpotLightComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "GameplayTagsManager.h"
#include "HAL/IConsoleManager.h"
#include "Interaction/StageParameterInterface.h"
#include "Materials/Material.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Show/StageFixtureCells.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"
#include "UI/StagePatchPanel.h"

namespace StageShowCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	/** "Gobo" -> StageCraft.Attribute.Gobo, "Patch.Address" -> StageCraft.Param.Patch.Address; a full tag name works too. */
	FGameplayTag ResolveTag(const FString& Name)
	{
		for (const FString& Candidate : { Name, TEXT("StageCraft.Attribute.") + Name, TEXT("StageCraft.Param.") + Name })
		{
			const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*Candidate), /*ErrorIfNotFound*/ false);
			if (Tag.IsValid())
			{
				return Tag;
			}
		}
		return FGameplayTag();
	}

	/** Parses Text into a value of the parameter's current type (so "2" is an Enum slot for Gobo and a float for Dimmer). */
	bool ParseValue(const FStageParameterValue& Current, const TArray<FString>& Args, FStageParameterValue& OutValue)
	{
		OutValue = Current;
		switch (Current.Type)
		{
		case EStageParameterType::Float:	return Args.Num() >= 2 && LexTryParseString(OutValue.Float, *Args[1]);
		case EStageParameterType::Integer:
		case EStageParameterType::Enum:		return Args.Num() >= 2 && LexTryParseString(OutValue.Integer, *Args[1]);
		case EStageParameterType::Bool:		OutValue.bBool = Args.Num() >= 2 && (Args[1] == TEXT("1") || Args[1].Equals(TEXT("true"), ESearchCase::IgnoreCase)); return Args.Num() >= 2;
		case EStageParameterType::Color:	return Args.Num() >= 4 && LexTryParseString(OutValue.Color.R, *Args[1]) && LexTryParseString(OutValue.Color.G, *Args[2]) && LexTryParseString(OutValue.Color.B, *Args[3]);
		case EStageParameterType::Text:		OutValue.Text = FText::FromString(FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "))); return Args.Num() >= 2;
		default:							return false;
		}
	}

	FString DescribeValue(const FStageParameterValue& Value)
	{
		switch (Value.Type)
		{
		case EStageParameterType::Float:	return FString::Printf(TEXT("%.3f"), Value.Float);
		case EStageParameterType::Integer:
		case EStageParameterType::Enum:		return FString::FromInt(Value.Integer);
		case EStageParameterType::Bool:		return Value.bBool ? TEXT("true") : TEXT("false");
		case EStageParameterType::Color:	return Value.Color.ToString();
		case EStageParameterType::Text:		return Value.Text.ToString();
		default:							return TEXT("?");
		}
	}

	void Set(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		AActor* Target = Controller ? Controller->GetSelection()->GetSelectedActor() : nullptr;
		const FGameplayTag Tag = Args.IsEmpty() ? FGameplayTag() : ResolveTag(Args[0]);
		FStageParameterValue Current;
		FStageParameterValue Value;
		if (!Target || !Tag.IsValid() || !Target->Implements<UStageParameterInterface>()
			|| !IStageParameterInterface::Execute_GetParameterValue(Target, Tag, Current) || !ParseValue(Current, Args, Value))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select an item, then StageCraft.Show.Set <Attribute or Param, e.g. Gobo, Iris, BladeA, Patch.Address> <value> (Color: r g b)"));
			return;
		}

		const FStageEconomyResultInfo Result = Controller->RequestParameterChange(Target, Tag, Value);
		FStageParameterValue After;
		IStageParameterInterface::Execute_GetParameterValue(Target, Tag, After);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Set %s %s on %s: %s; now %s."), *Tag.ToString(), *DescribeValue(Value), *Target->GetName(),
			*UEnum::GetValueAsString(Result.Code), *DescribeValue(After));
	}

	void Photometrics(const TArray<FString>& Args, UWorld* World)
	{
		const AModularPlayerController* Controller = GetController(World);
		const ALightingFixtureActor* Fixture = Controller ? Cast<ALightingFixtureActor>(Controller->GetSelection()->GetSelectedActor()) : nullptr;
		if (!Fixture)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select a fixture, then StageCraft.Show.Photometrics"));
			return;
		}
		const StagePhotometry::FResult Photo = Fixture->ComputePhotometrics();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Photometrics %s: beam %.2f deg, %.0f cd, throw %s, %.1f lx, beam %.2f m, strobe %.1f Hz."),
			*Fixture->GetName(), Photo.BeamAngleDegrees, Photo.IntensityCd, Photo.bHasThrow ? *FString::Printf(TEXT("%.2f m"), Photo.ThrowMetres) : TEXT("none"),
			Photo.IlluminanceLux, Photo.BeamDiameterMetres, Fixture->GetStrobeRateHz());

		// The beam-shaping projection as the renderer sees it: which light function and its domain (only LightFunction projects).
		const USpotLightComponent* Beam = Fixture->FindComponentByClass<USpotLightComponent>();
		const UMaterialInterface* LightFunction = Beam ? Beam->LightFunctionMaterial.Get() : nullptr;
		const UMaterial* BaseMaterial = LightFunction ? LightFunction->GetMaterial() : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Photometrics %s: light function %s (parent %s, domain %s), cone %.1f deg, intensity %.0f lm."),
			*Fixture->GetName(), *GetNameSafe(LightFunction), *GetNameSafe(BaseMaterial),
			BaseMaterial ? *UEnum::GetValueAsString(BaseMaterial->MaterialDomain.GetValue()) : TEXT("-"),
			Beam ? Beam->OuterConeAngle * 2.f : 0.f, Beam ? Beam->Intensity : 0.f);
	}

	void Patch(const TArray<FString>& Args, UWorld* World)
	{
		const UShowControlSubsystem* ShowControl = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		const UStageDMXPatchSubsystem* Patching = World ? World->GetSubsystem<UStageDMXPatchSubsystem>() : nullptr;
		if (!ShowControl || !Patching)
		{
			return;
		}
		const TArray<int32> Conflicts = Patching->FindPatchConflicts();
		for (const ALightingFixtureActor* Fixture : ShowControl->GetFixtures())
		{
			const FStageDMXPatch FixturePatch = Fixture->GetPatch();
			const ULightingFixtureData* Data = Fixture->GetFixtureData();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Patch: FID %d %s \"%s\" %s %s, %d ch%s."), Fixture->GetFixtureId(), *Fixture->GetName(),
				*Fixture->GetInstanceLabel().ToString(), Data ? *Data->DMXModeName.ToString() : TEXT("?"),
				*StagePatchFormat::FormatPatch(FixturePatch.Universe, FixturePatch.Address).ToString(), Fixture->GetDMXFootprint(),
				Conflicts.Contains(Fixture->GetFixtureId()) ? TEXT(", CONFLICT") : TEXT(""));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Patch: %d fixtures, %d conflicts, %d strobing."), ShowControl->GetFixtures().Num(), Conflicts.Num(),
			ShowControl->GetStrobingFixtureCount());
	}

	/**
	 * Applies "key=value" pairs to a cue's timing (Docs/ADR/0026 §2): delay, out (or out=none), wait, trigger=Go|Follow|Wait,
	 * curve=Linear|SCurve, and feature times int|pos|col|beam|foc (or =none). False on an unknown key or value.
	 */
	bool ParseTiming(TArrayView<const FString> Pairs, FStageCueTiming& InOutTiming)
	{
		for (const FString& Pair : Pairs)
		{
			FString Key;
			FString Value;
			if (!Pair.Split(TEXT("="), &Key, &Value))
			{
				return false;
			}
			float Seconds = 0.f;
			const bool bNumber = LexTryParseString(Seconds, *Value);
			const TPair<const TCHAR*, EStagePaletteKind> FeatureKeys[] = {
				{ TEXT("int"), EStagePaletteKind::Intensity }, { TEXT("pos"), EStagePaletteKind::Position }, { TEXT("col"), EStagePaletteKind::Color },
				{ TEXT("beam"), EStagePaletteKind::Beam }, { TEXT("foc"), EStagePaletteKind::Focus } };
			const TPair<const TCHAR*, EStagePaletteKind>* Feature = Algo::FindByPredicate(FeatureKeys, [&Key](const TPair<const TCHAR*, EStagePaletteKind>& Each) { return Key == Each.Key; });
			if (Key == TEXT("delay") && bNumber)
			{
				InOutTiming.Delay = Seconds;
			}
			else if (Key == TEXT("out"))
			{
				InOutTiming.bHasOutTime = bNumber;
				InOutTiming.OutTime = bNumber ? Seconds : 0.f;
			}
			else if (Key == TEXT("wait") && bNumber)
			{
				InOutTiming.WaitTime = Seconds;
			}
			else if (Key == TEXT("trigger"))
			{
				const int64 Trigger = StaticEnum<EStageCueTrigger>()->GetValueByNameString(Value);
				if (Trigger == INDEX_NONE)
				{
					return false;
				}
				InOutTiming.Trigger = static_cast<EStageCueTrigger>(Trigger);
			}
			else if (Key == TEXT("curve"))
			{
				const int64 Curve = StaticEnum<EStageCueCurve>()->GetValueByNameString(Value);
				if (Curve == INDEX_NONE)
				{
					return false;
				}
				InOutTiming.Curve = static_cast<EStageCueCurve>(Curve);
			}
			else if (Feature && bNumber)
			{
				InOutTiming.FeatureTimes.Add(Feature->Value, Seconds);
			}
			else if (Feature && Value == TEXT("none"))
			{
				InOutTiming.FeatureTimes.Remove(Feature->Value);
			}
			else
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Cue requests through the controller (the Cue panel's path): Store|Track|CueOnly <n> [fade] [label...] (Store records the whole
	 * look), Update <n> <fade> [label...], Timing <n> key=value..., Block <n> On|Off, Delete <n>, Go, Back, GoTo <n>,
	 * Source Internal|External, List.
	 */
	void Cue(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UShowControlSubsystem* ShowControl = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Controller || !ShowControl || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Cue Store|Track|CueOnly <n> [fade] [label] | Update <n> <fade> [label] | Timing <n> key=value... | Block <n> On|Off | Delete <n> | Go | Back | GoTo <n> | Source Internal|External | List"));
			return;
		}

		const FString& Verb = Args[0];
		float Number = 0.f;
		const bool bHasNumber = Args.Num() >= 2 && LexTryParseString(Number, *Args[1]);
		float Fade = 2.f;
		const bool bHasFade = Args.Num() >= 3 && LexTryParseString(Fade, *Args[2]);
		FString Label;
		for (int32 Index = 3; Index < Args.Num(); ++Index)
		{
			Label += (Label.IsEmpty() ? TEXT("") : TEXT(" ")) + Args[Index];
		}

		bool bOk = false;
		const bool bStore = Verb == TEXT("Store") || Verb == TEXT("Track") || Verb == TEXT("CueOnly");
		if (bStore && bHasNumber)
		{
			const EStageCueStoreMode Mode = Verb == TEXT("Track") ? EStageCueStoreMode::Track
				: (Verb == TEXT("CueOnly") ? EStageCueStoreMode::CueOnly : EStageCueStoreMode::Look);
			bOk = Controller->RequestStoreCue(Number, FText::FromString(Label), bHasFade ? Fade : 2.f, Mode);
		}
		else if (Verb == TEXT("Timing") && bHasNumber && ShowControl->FindCue(Number))
		{
			FStageCueTiming Timing = ShowControl->FindCue(Number)->Timing;
			bOk = ParseTiming(TArrayView<const FString>(Args).RightChop(2), Timing) && Controller->RequestUpdateCueTiming(Number, Timing);
		}
		else if (Verb == TEXT("Block") && bHasNumber && Args.Num() >= 3)
		{
			bOk = Controller->RequestSetCueBlocked(Number, Args[2] == TEXT("On"));
		}
		else if (Verb == TEXT("Update") && bHasNumber && bHasFade)
		{
			bOk = Controller->RequestUpdateCue(Number, FText::FromString(Label), Fade);
		}
		else if (Verb == TEXT("Delete") && bHasNumber)
		{
			bOk = Controller->RequestDeleteCue(Number);
		}
		else if (Verb == TEXT("Go"))
		{
			bOk = Controller->RequestCueGo();
		}
		else if (Verb == TEXT("Back"))
		{
			bOk = Controller->RequestCueBack();
		}
		else if (Verb == TEXT("GoTo") && bHasNumber)
		{
			bOk = Controller->RequestGoToCue(Number);
		}
		else if (Verb == TEXT("Source") && Args.Num() >= 2)
		{
			bOk = Controller->RequestSetControlSource(Args[1] == TEXT("External") ? EStageControlSource::External : EStageControlSource::Internal);
		}
		else if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Cue: unknown or incomplete command \"%s\"."), *FString::Join(Args, TEXT(" ")));
			return;
		}

		if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Cue %s: %s."), *FString::Join(Args, TEXT(" ")), bOk ? TEXT("done") : TEXT("refused"));
		}
		for (const FShowCue& Each : ShowControl->GetCues())
		{
			const FStageCueTiming& Timing = Each.Timing;
			TArray<FString> Features;
			for (const TPair<EStagePaletteKind, float>& Pair : Timing.FeatureTimes)
			{
				Features.Add(FString::Printf(TEXT("%s %.2f"), *UEnum::GetDisplayValueAsText(Pair.Key).ToString(), Pair.Value));
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Cue: %s%.3f \"%s\" in %.2f s, out %s, delay %.2f, %s%s, %s, %s, %d fixtures%s%s."),
				FMath::IsNearlyEqual(Each.Number, ShowControl->GetActiveCueNumber(), 0.0005f) ? TEXT("> ") : TEXT("  "),
				Each.Number, *Each.Label.ToString(), Each.FadeTime, Timing.bHasOutTime ? *FString::SanitizeFloat(Timing.OutTime) : TEXT("=in"), Timing.Delay,
				*UEnum::GetDisplayValueAsText(Timing.Trigger).ToString(), Timing.Trigger == EStageCueTrigger::Wait ? *FString::Printf(TEXT(" %.2f s"), Timing.WaitTime) : TEXT(""),
				*UEnum::GetDisplayValueAsText(Timing.Curve).ToString(), Each.bBlock ? TEXT("blocked") : TEXT("tracking"), Each.Fixtures.Num(),
				Features.IsEmpty() ? TEXT("") : TEXT(", features "), *FString::Join(Features, TEXT(", ")));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Cue: %d cues, active %.3f, %s%s, source %s."), ShowControl->GetCues().Num(),
			ShowControl->GetActiveCueNumber(), ShowControl->IsFading() ? TEXT("fading") : TEXT("idle"), ShowControl->IsAutoGoArmed() ? TEXT(", next cue armed") : TEXT(""),
			*UEnum::GetValueAsString(ShowControl->GetControlSource()));
	}

	/** Selects fixtures by fixture ID, in the order given (the last is the primary), so scripted runs can build a selection. */
	void Select(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UShowControlSubsystem* ShowControl = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Controller || !ShowControl)
		{
			return;
		}
		TArray<AModularBaseActor*> Fixtures;
		for (const FString& Arg : Args)
		{
			int32 FixtureId = 0;
			if (ALightingFixtureActor* Fixture = LexTryParseString(FixtureId, *Arg) ? ShowControl->FindFixture(FixtureId) : nullptr)
			{
				Fixtures.Add(Fixture);
			}
		}
		Controller->RequestSelectItems(Fixtures, EStageSelectionClick::Replace);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Select: %d fixtures selected."), Fixtures.Num());
	}

	void Programmer(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UStageProgrammerSubsystem* ProgrammerState = World ? World->GetSubsystem<UStageProgrammerSubsystem>() : nullptr;
		if (!Controller || !ProgrammerState)
		{
			return;
		}
		const FString Verb = Args.IsEmpty() ? TEXT("List") : Args[0];
		if (Verb == TEXT("Clear"))
		{
			Controller->RequestClearProgrammer();
		}
		else if (Verb == TEXT("Highlight") && Args.Num() >= 2)
		{
			Controller->RequestSetHighlight(Args[1] == TEXT("On") || Args[1] == TEXT("1"));
		}
		else if (Verb == TEXT("Fan") && Args.Num() >= 2)
		{
			const int64 Mode = StaticEnum<EStageFanMode>()->GetValueByNameString(Args[1]);
			if (Mode == INDEX_NONE)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Programmer Fan: Off, Linear, V or InvertedV."));
				return;
			}
			Controller->RequestSetFanMode(static_cast<EStageFanMode>(Mode));
		}
		else if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Programmer Clear | Highlight On|Off | Fan Off|Linear|V|InvertedV | List"));
			return;
		}
		const UStageShowLibrarySubsystem* Library = World->GetSubsystem<UStageShowLibrarySubsystem>();
		for (const FShowCueFixtureState& Entry : ProgrammerState->GetEntries())
		{
			TArray<FString> Values;
			for (const TPair<FGameplayTag, float>& Pair : Entry.Attributes)
			{
				// A referenced attribute shows the palette it follows: "ColorR=1.000@Red".
				const FGuid* Reference = Entry.PaletteRefs.Find(Pair.Key);
				const FStagePalette* Palette = Reference && Library ? Library->FindPalette(*Reference) : nullptr;
				Values.Add(FString::Printf(TEXT("%s=%.3f%s"), *Pair.Key.GetTagName().ToString().RightChop(21), Pair.Value,
					Reference ? *(TEXT("@") + (Palette ? Palette->Name.ToString() : FString(TEXT("(missing palette)")))) : TEXT("")));
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Programmer: fixture %d: %s"), Entry.FixtureId, *FString::Join(Values, TEXT(" ")));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Programmer: %d fixtures held, highlight %s, fan %s."), ProgrammerState->GetFixtureCount(),
			Controller->IsHighlightOn() ? TEXT("on") : TEXT("off"), *UEnum::GetValueAsString(Controller->GetFanMode()));
	}

	void Group(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UStageShowLibrarySubsystem* Library = World ? World->GetSubsystem<UStageShowLibrarySubsystem>() : nullptr;
		if (!Controller || !Library)
		{
			return;
		}
		const FString Verb = Args.IsEmpty() ? TEXT("List") : Args[0];
		int32 Index = 0;
		const bool bHasIndex = Args.Num() >= 2 && LexTryParseString(Index, *Args[1]) && Library->GetGroups().IsValidIndex(Index - 1);
		const FGuid GroupId = bHasIndex ? Library->GetGroups()[Index - 1].Id : FGuid();
		bool bOk = true;
		if (Verb == TEXT("Record"))
		{
			bOk = Controller->RequestRecordGroup(FText::FromString(FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" ")))).IsValid();
		}
		else if (Verb == TEXT("Select") && bHasIndex)
		{
			bOk = Controller->RequestSelectGroup(GroupId);
		}
		else if (Verb == TEXT("Update") && bHasIndex)
		{
			bOk = Controller->RequestUpdateGroup(GroupId);
		}
		else if (Verb == TEXT("Delete") && bHasIndex)
		{
			bOk = Controller->RequestDeleteGroup(GroupId);
		}
		else if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Group Record [name] | Select <n> | Update <n> | Delete <n> | List (n counts from 1)"));
			return;
		}
		if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Group %s: %s."), *FString::Join(Args, TEXT(" ")), bOk ? TEXT("done") : TEXT("refused"));
		}
		int32 Number = 0;
		for (const FStageFixtureGroup& Each : Library->GetGroups())
		{
			TArray<FString> Ids;
			for (const int32 FixtureId : Each.FixtureIds)
			{
				Ids.Add(FString::FromInt(FixtureId));
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Group %d \"%s\": %s"), ++Number, *Each.Name.ToString(), *FString::Join(Ids, TEXT(" ")));
		}
	}

	void PaletteRequest(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UStageShowLibrarySubsystem* Library = World ? World->GetSubsystem<UStageShowLibrarySubsystem>() : nullptr;
		const UShowControlSubsystem* Show = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Controller || !Library || !Show)
		{
			return;
		}
		const FString Verb = Args.IsEmpty() ? TEXT("List") : Args[0];
		int32 Index = 0;
		const bool bHasIndex = Args.Num() >= 2 && LexTryParseString(Index, *Args[1]) && Library->GetPalettes().IsValidIndex(Index - 1);
		const FGuid PaletteId = bHasIndex ? Library->GetPalettes()[Index - 1].Id : FGuid();
		const UEnum* KindEnum = StaticEnum<EStagePaletteKind>();
		const int64 Kind = Args.Num() >= 2 && KindEnum ? KindEnum->GetValueByNameString(Args[1]) : INDEX_NONE;
		bool bOk = true;
		if (Verb == TEXT("Record") && Kind != INDEX_NONE)
		{
			// Record <kind> [PerFixture] [name...]
			const bool bPerFixture = Args.Num() >= 3 && Args[2] == TEXT("PerFixture");
			const int32 NameStart = bPerFixture ? 3 : 2;
			const FString Name = FString::Join(TArrayView<const FString>(Args).RightChop(NameStart), TEXT(" "));
			bOk = Controller->RequestRecordPalette(static_cast<EStagePaletteKind>(Kind), FText::FromString(Name),
				bPerFixture ? EStagePaletteScope::PerFixture : EStagePaletteScope::Global).IsValid();
		}
		else if (Verb == TEXT("Apply") && bHasIndex)
		{
			bOk = Controller->RequestApplyPalette(PaletteId).IsSuccess();
		}
		else if (Verb == TEXT("Update") && bHasIndex)
		{
			const bool bPerFixture = !Library->GetPalettes()[Index - 1].FixtureValues.IsEmpty();
			bOk = Controller->RequestUpdatePalette(PaletteId, bPerFixture ? EStagePaletteScope::PerFixture : EStagePaletteScope::Global);
		}
		else if (Verb == TEXT("Rename") && bHasIndex && Args.Num() >= 3)
		{
			bOk = Controller->RequestRenamePalette(PaletteId, FText::FromString(FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(" "))));
		}
		else if (Verb == TEXT("Delete") && bHasIndex)
		{
			bOk = Controller->RequestDeletePalette(PaletteId);
		}
		else if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Palette Record <All|Intensity|Position|Color|Beam|Focus> [PerFixture] [name] | Apply <n> | Update <n> | Rename <n> <name> | Delete <n> | List (n counts from 1)"));
			return;
		}
		if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Palette %s: %s."), *FString::Join(Args, TEXT(" ")), bOk ? TEXT("done") : TEXT("refused"));
		}
		int32 Number = 0;
		for (const FStagePalette& Each : Library->GetPalettes())
		{
			TArray<FString> Cues;
			for (const float CueNumber : StagePaletteRules::FindCuesUsing(Show->GetCues(), Each.Id))
			{
				Cues.Add(FString::SanitizeFloat(CueNumber));
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Palette %d \"%s\" %s: colour %s, %d global value(s), %d fixture(s); cues [%s]"), ++Number,
				*Each.Name.ToString(), *UEnum::GetDisplayValueAsText(Each.Kind).ToString(), Each.bHasColor ? *Each.Color.ToString() : TEXT("none"),
				Each.GlobalValues.Num(), Each.FixtureValues.Num(), *FString::Join(Cues, TEXT(" ")));
		}
	}

	/** Logs fixtures' output (the mix as it reaches the stage), cell controls left out: how scripted runs read what a fixture shows. */
	void Output(const TArray<FString>& Args, UWorld* World)
	{
		const UShowControlSubsystem* Show = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Show || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Output <fixture id>..."));
			return;
		}
		for (const FString& Arg : Args)
		{
			int32 FixtureId = 0;
			const ALightingFixtureActor* Fixture = LexTryParseString(FixtureId, *Arg) ? Show->FindFixture(FixtureId) : nullptr;
			if (!Fixture)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Output: no fixture %s."), *Arg);
				continue;
			}
			TArray<FString> Values;
			for (const TPair<FGameplayTag, float>& Pair : Fixture->GetAttributes())
			{
				if (!StageFixtureCells::IsCellAttribute(Pair.Key))
				{
					Values.Add(FString::Printf(TEXT("%s=%.3f"), *Pair.Key.GetTagName().ToString().RightChop(21), Pair.Value));
				}
			}
			Values.Sort();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Output: fixture %d: %s"), FixtureId, *FString::Join(Values, TEXT(" ")));
		}
	}

	/** Playback n (1..10): Record | Clear | Rename <name> | Level <0..1> | Flash On|Off | Go | Release | List. */
	void PlaybackRequest(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UShowControlSubsystem* Show = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Controller || !Show)
		{
			return;
		}
		int32 Number = 0;
		const bool bHasNumber = Args.Num() >= 2 && LexTryParseString(Number, *Args[0]);
		const int32 Index = Number - 1;
		const FString Verb = bHasNumber ? Args[1] : (Args.IsEmpty() ? TEXT("List") : Args[0]);
		float Value = 0.f;
		const bool bHasValue = Args.Num() >= 3 && LexTryParseString(Value, *Args[2]);
		bool bOk = true;
		if (bHasNumber && Verb == TEXT("Record"))
		{
			bOk = Controller->RequestRecordPlayback(Index);
		}
		else if (bHasNumber && Verb == TEXT("Clear"))
		{
			bOk = Controller->RequestClearPlayback(Index);
		}
		else if (bHasNumber && Verb == TEXT("Rename"))
		{
			bOk = Controller->RequestRenamePlayback(Index, FText::FromString(FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(" "))));
		}
		else if (bHasNumber && Verb == TEXT("Level") && bHasValue)
		{
			bOk = Controller->RequestSetPlaybackLevel(Index, Value);
		}
		else if (bHasNumber && Verb == TEXT("Flash") && Args.Num() >= 3)
		{
			bOk = Controller->RequestPlaybackFlash(Index, Args[2] == TEXT("On"));
		}
		else if (bHasNumber && Verb == TEXT("Go"))
		{
			bOk = Controller->RequestPlaybackGo(Index);
		}
		else if (bHasNumber && Verb == TEXT("Release"))
		{
			bOk = Controller->RequestReleasePlayback(Index);
		}
		else if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Playback <1..10> Record | Clear | Rename <name> | Level <0..1> | Flash On|Off | Go | Release, or List"));
			return;
		}
		if (Verb != TEXT("List"))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Playback %s: %s."), *FString::Join(Args, TEXT(" ")), bOk ? TEXT("done") : TEXT("refused"));
		}
		for (int32 Each = 0; Each < Show->GetPlaybacks().Num(); ++Each)
		{
			const FStagePlayback& Playback = Show->GetPlaybacks()[Each];
			if (Playback.Cues.IsEmpty() && !Show->IsPlaybackActive(Each))
			{
				continue;
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Playback %d \"%s\": %d cue(s), at %d, level %.2f%s%s."), Each + 1, *Playback.Name.ToString(),
				Playback.Cues.Num(), Show->GetPlaybackCueIndex(Each) + 1, Show->GetPlaybackLevel(Each), Show->IsPlaybackFlashing(Each) ? TEXT(", flash") : TEXT(""),
				Show->IsPlaybackActive(Each) ? TEXT(", active") : TEXT(""));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Playback: Grand Master %.2f, blackout %s."), Show->GetGrandMaster(), Show->IsBlackout() ? TEXT("on") : TEXT("off"));
	}

	/** GM <0..1> | Blackout On|Off. */
	void MasterRequest(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		float Value = 0.f;
		bool bOk = false;
		if (Controller && Args.Num() >= 2 && Args[0] == TEXT("GM") && LexTryParseString(Value, *Args[1]))
		{
			bOk = Controller->RequestSetGrandMaster(Value);
		}
		else if (Controller && Args.Num() >= 2 && Args[0] == TEXT("Blackout"))
		{
			bOk = Controller->RequestSetBlackout(Args[1] == TEXT("On"));
		}
		else
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Show.Master GM <0..1> | Blackout On|Off"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Show.Master %s: %s."), *FString::Join(Args, TEXT(" ")), bOk ? TEXT("done") : TEXT("refused"));
	}

	FAutoConsoleCommandWithWorldAndArgs PlaybackCommand(TEXT("StageCraft.Show.Playback"),
		TEXT("<1..10> Record | Clear | Rename <name> | Level <0..1> | Flash On|Off | Go | Release, or List (as the Playback panel)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PlaybackRequest));
	FAutoConsoleCommandWithWorldAndArgs MasterCommand(TEXT("StageCraft.Show.Master"), TEXT("GM <0..1> | Blackout On|Off (as the Playback panel's masters)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MasterRequest));
	FAutoConsoleCommandWithWorldAndArgs OutputCommand(TEXT("StageCraft.Show.Output"), TEXT("<fixture id>... Logs the fixtures' output values (cells left out)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Output));
	FAutoConsoleCommandWithWorldAndArgs PaletteCommand(TEXT("StageCraft.Show.Palette"),
		TEXT("Record <kind> [PerFixture] [name] | Apply <n> | Update <n> | Rename <n> <name> | Delete <n> | List (validated, undoable like the Palettes panel)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PaletteRequest));
	FAutoConsoleCommandWithWorldAndArgs SelectCommand(TEXT("StageCraft.Show.Select"), TEXT("<fixture id>... Selects fixtures by ID in that order (the last is the primary)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Select));
	FAutoConsoleCommandWithWorldAndArgs ProgrammerCommand(TEXT("StageCraft.Show.Programmer"), TEXT("Clear | Highlight On|Off | Fan Off|Linear|V|InvertedV | List (as the Groups panel's programmer bar)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Programmer));
	FAutoConsoleCommandWithWorldAndArgs GroupCommand(TEXT("StageCraft.Show.Group"), TEXT("Record [name] | Select <n> | Update <n> | Delete <n> | List (validated, undoable like the Groups panel)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Group));
	FAutoConsoleCommandWithWorldAndArgs CueCommand(TEXT("StageCraft.Show.Cue"), TEXT("Store|Track|CueOnly <n> [fade] [label] | Update <n> <fade> [label] | Timing <n> delay= out= wait= trigger= curve= int= pos= col= beam= foc= | Block <n> On|Off | Delete <n> | Go | Back | GoTo <n> | Source Internal|External | List (validated, undoable like the Cue panel)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Cue));
	FAutoConsoleCommandWithWorldAndArgs SetCommand(TEXT("StageCraft.Show.Set"), TEXT("<Attribute|Param> <value> Inspector-equivalent edit of the selected item (validated)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Set));
	FAutoConsoleCommandWithWorldAndArgs PhotometricsCommand(TEXT("StageCraft.Show.Photometrics"), TEXT("Logs the selected fixture's beam angle, intensity, throw, illuminance and strobe rate."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Photometrics));
	FAutoConsoleCommandWithWorldAndArgs PatchCommand(TEXT("StageCraft.Show.Patch"), TEXT("Logs every fixture's ID, mode, patch and conflicts, and the strobe clock."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Patch));
}

#endif // !UE_BUILD_SHIPPING
