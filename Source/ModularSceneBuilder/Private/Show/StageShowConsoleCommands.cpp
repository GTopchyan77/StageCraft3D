// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for fixtures, the patch and the strobe clock (Docs/ADR/0007 §5-6). Parameter edits go through
// AModularPlayerController::RequestParameterChange, the same validated path the inspector, the faders and the patch sheet use.
// Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

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
#include "Subsystems/ShowControlSubsystem.h"
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
		if (!ShowControl)
		{
			return;
		}
		const TArray<int32> Conflicts = ShowControl->FindPatchConflicts();
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

	FAutoConsoleCommandWithWorldAndArgs SetCommand(TEXT("StageCraft.Show.Set"), TEXT("<Attribute|Param> <value> Inspector-equivalent edit of the selected item (validated)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Set));
	FAutoConsoleCommandWithWorldAndArgs PhotometricsCommand(TEXT("StageCraft.Show.Photometrics"), TEXT("Logs the selected fixture's beam angle, intensity, throw, illuminance and strobe rate."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Photometrics));
	FAutoConsoleCommandWithWorldAndArgs PatchCommand(TEXT("StageCraft.Show.Patch"), TEXT("Logs every fixture's ID, mode, patch and conflicts, and the strobe clock."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Patch));
}

#endif // !UE_BUILD_SHIPPING
