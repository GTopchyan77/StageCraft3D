// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandRunner.h"

#include "Actors/ModularBaseActor.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "Data/LightingFixtureData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "History/StageEditHistorySubsystem.h"
#include "Sketch/StageSketchGenerator.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

#define LOCTEXT_NAMESPACE "StageAICommandRunner"

namespace StageAICommandRunner
{
	namespace
	{
		/** The cue panel's default crossfade. */
		constexpr float DefaultFadeSeconds = 2.f;

		FStageAIActionOutcome Done(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, Message); }
		FStageAIActionOutcome Refused(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Refused, Message); }

		/** Work's first reported refusal, else Fallback. */
		FText RefusalOf(const TArray<FStageEconomyResultInfo>& Refusals, const FText& Fallback)
		{
			return Refusals.IsEmpty() || Refusals[0].Message.IsEmpty() ? Fallback : Refusals[0].Message;
		}

		FText FormatCue(float Number)
		{
			FNumberFormattingOptions Format;
			Format.SetMaximumFractionalDigits(3);
			return FText::AsNumber(Number, &Format);
		}

		FStageAIActionOutcome AddFixtures(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			UWorld* World = Controller.GetWorld();
			UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			const UStageItemSubsystem* Catalog = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
			if (!Assistant || !Catalog)
			{
				return Refused(LOCTEXT("NoCatalog", "The catalog is not available in this level."));
			}

			TArray<ULightingFixtureData*> Fixtures;
			TArray<FStageAIFixtureCandidate> Candidates;
			for (UBaseItemData* Item : Catalog->GetCatalog())
			{
				if (ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item))
				{
					Fixtures.Add(Fixture);
					Candidates.Add({ Fixture->DisplayName.ToString(), UEnum::GetDisplayValueAsText(Fixture->FixtureKind).ToString() });
				}
			}
			if (Fixtures.IsEmpty())
			{
				return Refused(Catalog->IsCatalogLoaded()
					? LOCTEXT("NoFixtures", "The catalog has no lighting fixtures to add.")
					: LOCTEXT("CatalogLoading", "The catalog is still loading; try again in a moment."));
			}
			const int32 Match = StageAICommandParser::MatchFixture(Candidates, Command.ItemQuery);
			if (Match == INDEX_NONE)
			{
				TArray<FText> Names;
				for (const FStageAIFixtureCandidate& Candidate : Candidates)
				{
					Names.Add(FText::FromString(Candidate.Name));
				}
				return Refused(FText::Format(LOCTEXT("NoMatch", "No lighting fixture in the catalog matches \"{0}\". I can add: {1}."),
					FText::FromString(Command.ItemQuery), FText::Join(LOCTEXT("Separator", ", "), Names)));
			}
			ULightingFixtureData* Item = Fixtures[Match];

			// The rules may refuse this item outright (locked, not owned): say so before streaming anything.
			const FStageEconomyResultInfo Allowed = Controller.CanPlaceItem(Item);
			if (!Allowed.IsSuccess())
			{
				return Refused(FText::Format(LOCTEXT("NotAllowed", "I can't place {0}: {1}"), Item->DisplayName, Allowed.Message));
			}

			// A row beyond everything already placed, so nothing lands inside other gear.
			TOptional<double> LowestX;
			if (const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World))
			{
				for (const AModularBaseActor* Placed : Session->GetPlacedItems())
				{
					if (Placed)
					{
						LowestX = FMath::Min(LowestX.Get(TNumericLimits<double>::Max()), Placed->GetActorLocation().X);
					}
				}
			}
			const int32 Count = Command.Count.Get(1);
			TArray<FStageSketchStroke> Strokes;
			for (const FVector2D& Point : StageAICommandParser::LayoutFixtureRow(Count, LowestX))
			{
				FStageSketchStroke& Stroke = Strokes.AddDefaulted_GetRef();
				Stroke.Kind = EStageSketchStrokeKind::Fixture;
				Stroke.Points.Add(Point);
			}
			FStageSketchBuildRules Rules;
			Rules.GridStep = StageAICommandParser::FixtureRowGrid;
			Rules.FixtureItem = Item;
			FStageSketchBuildPlan Plan = StageSketchGenerator::BuildPlan(Strokes, Rules);
			if (Plan.Fixtures.Num() != Count)
			{
				return Refused(Plan.Warnings.IsEmpty() ? LOCTEXT("PlanFailed", "The fixtures could not be laid out.") : Plan.Warnings[0]);
			}

			const FText Name = Item->DisplayName;
			const FText StepName = FText::Format(LOCTEXT("AddStep", "Assistant: add {0} × {1}"), FText::AsNumber(Count), Name);
			TWeakObjectPtr<AModularPlayerController> WeakController = &Controller;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = Assistant;
			const bool bStarted = Assistant->RequestItemAssets(*Item, FSimpleDelegate::CreateLambda([WeakController, WeakAssistant, Plan, StepName, Name, Count]()
			{
				UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
				AModularPlayerController* PendingController = WeakController.Get();
				if (!PendingAssistant)
				{
					return;
				}
				if (!PendingController)
				{
					PendingAssistant->FinishPendingAction(Refused(LOCTEXT("PlayerGone", "Nothing was placed: the player who asked has left.")));
					return;
				}
				FStageSketchGenerateResult Result;
				PendingController->RunCapturingRejections([&]() { Result = PendingController->RequestBuildPlan(Plan, StepName); });
				PendingAssistant->FinishPendingAction(Result.bSucceeded
					? Done(FText::Format(LOCTEXT("Added", "Added {0} × {1} in a row on the floor, ready to move into place. Ctrl+Z removes all of them."), FText::AsNumber(Count), Name))
					: Refused(FText::Format(LOCTEXT("AddRefused", "Nothing was placed: {0}"), Result.Message)));
			}));
			return bStarted
				? FStageAIActionOutcome::Make(EStageAIOutcome::Pending, FText::Format(LOCTEXT("Placing", "Placing {0} × {1}…"), FText::AsNumber(Count), Name))
				: Refused(LOCTEXT("LoadNotStarted", "The fixture could not be loaded."));
		}

		FStageAIActionOutcome StoreCue(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Controller.GetWorld());
			if (!Show)
			{
				return Refused(LOCTEXT("NoShow", "Cues are not available in this level."));
			}
			float Number = 1.f;
			if (Command.CueNumber.IsSet())
			{
				Number = *Command.CueNumber;
			}
			else if (!Show->GetCues().IsEmpty())
			{
				// The next whole number after the last cue.
				Number = FMath::FloorToFloat(Show->GetCues().Last().Number) + 1.f;
			}
			const float Fade = Command.FadeSeconds.Get(DefaultFadeSeconds);
			const bool bReplacing = Show->FindCue(Number) != nullptr;
			bool bStored = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]()
			{
				bStored = Controller.RequestStoreCue(Number, FText::FromString(Command.Label), Fade);
			});
			if (!bStored)
			{
				return Refused(RefusalOf(Refusals, FText::Format(LOCTEXT("StoreFailed", "Cue {0} could not be stored."), FormatCue(Number))));
			}
			const FText Label = Command.Label.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("CueLabel", " \"{0}\""), FText::FromString(Command.Label));
			return Done(FText::Format(bReplacing
					? LOCTEXT("CueReplaced", "Re-recorded cue {0}{1} with a {2} s fade from every fixture's current look. Ctrl+Z restores the old one.")
					: LOCTEXT("CueStored", "Stored cue {0}{1} with a {2} s fade from every fixture's current look. Ctrl+Z removes it."),
				FormatCue(Number), Label, FText::AsNumber(Fade)));
		}

		FStageAIActionOutcome PlayCue(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			bool bPlayed = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]()
			{
				switch (Command.Intent)
				{
				case EStageAIIntent::GoToCue:	bPlayed = Controller.RequestGoToCue(*Command.CueNumber); break;
				case EStageAIIntent::CueGo:		bPlayed = Controller.RequestCueGo(); break;
				default:						bPlayed = Controller.RequestCueBack(); break;
				}
			});
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Controller.GetWorld());
			if (!bPlayed || !Show)
			{
				const FText Fallback = Command.Intent == EStageAIIntent::GoToCue
					? FText::Format(LOCTEXT("NoSuchCue", "There is no cue {0}. Say \"list cues\" to see them."), FormatCue(*Command.CueNumber))
					: Command.Intent == EStageAIIntent::CueGo
						? LOCTEXT("NoNextCue", "There is no next cue to play.")
						: LOCTEXT("NoPreviousCue", "There is no previous cue to go back to.");
				return Refused(RefusalOf(Refusals, Fallback));
			}
			const FShowCue* Active = Show->FindCue(Show->GetActiveCueNumber());
			const FText Label = Active && !Active->Label.IsEmpty() ? FText::Format(LOCTEXT("ActiveLabel", " \"{0}\""), Active->Label) : FText::GetEmpty();
			return Done(FText::Format(LOCTEXT("Playing", "Playing cue {0}{1}."), FormatCue(Show->GetActiveCueNumber()), Label));
		}

		FStageAIActionOutcome ChangeEnvironment(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(Controller.GetWorld());
			if (!Environment)
			{
				return Refused(LOCTEXT("NoEnvironment", "This level has no outdoor environment."));
			}
			const bool bSkyAvailable = Environment->IsSkyAvailable();
			const bool bSkyWasOn = Environment->GetSettings().bSkyEnabled;
			FStageEnvironmentSettings Settings = Environment->GetSettings();
			FText StepName;
			switch (Command.Intent)
			{
			case EStageAIIntent::SetWeather:
				Settings = StageEnvironment::ApplyWeatherPreset(Settings, Command.Weather);
				// Weather is only visible under the outdoor sky.
				Settings.bSkyEnabled = Settings.bSkyEnabled || bSkyAvailable;
				StepName = FText::Format(LOCTEXT("WeatherStep", "Assistant: weather {0}"), StageAIAnswers::DescribeWeather(Command.Weather));
				break;
			case EStageAIIntent::SetTimeOfDay:
				Settings.TimeOfDay = Command.Hour;
				StepName = FText::Format(LOCTEXT("TimeStep", "Assistant: time {0}"), FText::FromString(StageAIAnswers::FormatHour(Command.Hour)));
				break;
			default:
				if (Command.bSkyOn && !bSkyAvailable)
				{
					return Refused(LOCTEXT("NoSkySystem", "No sky system is installed in this build, so the stage stays indoors."));
				}
				Settings.bSkyEnabled = Command.bSkyOn;
				StepName = Command.bSkyOn ? LOCTEXT("SkyOnStep", "Assistant: outdoor sky on") : LOCTEXT("SkyOffStep", "Assistant: outdoor sky off");
				break;
			}

			bool bApplied = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bApplied = Controller.RequestSetEnvironment(Settings, StepName); });
			if (!bApplied)
			{
				return Refused(RefusalOf(Refusals, LOCTEXT("EnvironmentFailed", "The environment could not be changed.")));
			}

			// Describe what was stored, not what was asked for.
			const FStageEnvironmentSettings Stored = Environment->GetSettings();
			const FText SkyNote = !Stored.bSkyEnabled
				? (bSkyAvailable ? LOCTEXT("SkyOffNote", " The outdoor sky is off, so say \"go outdoors\" to see it.") : LOCTEXT("NoSkyNote", " No sky system is installed in this build, so it is stored but not shown."))
				: (!bSkyWasOn ? LOCTEXT("SkyTurnedOn", " The outdoor sky is now on.") : FText::GetEmpty());
			switch (Command.Intent)
			{
			case EStageAIIntent::SetWeather:
				return Done(FText::Format(LOCTEXT("WeatherSet", "Weather set to {0}.{1} Ctrl+Z restores the previous environment."),
					StageAIAnswers::DescribeWeather(StageEnvironment::FindWeatherPreset(Stored)), SkyNote));
			case EStageAIIntent::SetTimeOfDay:
				return Done(FText::Format(LOCTEXT("TimeSet", "Time of day set to {0}.{1} Ctrl+Z restores the previous environment."),
					FText::FromString(StageAIAnswers::FormatHour(Stored.TimeOfDay)), SkyNote));
			default:
				return Done(Stored.bSkyEnabled
					? LOCTEXT("SkyOn", "The outdoor sky is on: the sun, sky and weather replace the indoor lighting. Ctrl+Z turns it off again.")
					: LOCTEXT("SkyOff", "Indoor stage: the outdoor sky is off. Ctrl+Z turns it back on."));
			}
		}

		FStageAIActionOutcome Export(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			struct FReport
			{
				EStageAIReport Kind;
				FText Name;
				bool (AModularPlayerController::*Request)();
			};
			const FReport Reports[] = {
				{ EStageAIReport::BillOfMaterials, LOCTEXT("Bom", "bill of materials"), &AModularPlayerController::RequestExportBillOfMaterials },
				{ EStageAIReport::PatchSheet, LOCTEXT("Patch", "DMX patch sheet"), &AModularPlayerController::RequestExportPatchSheet },
				{ EStageAIReport::CableSchedule, LOCTEXT("Cables", "cable schedule and power plan"), &AModularPlayerController::RequestExportCableSchedule },
			};
			TArray<FText> Started;
			TArray<FText> Failed;
			for (const FReport& Report : Reports)
			{
				if (Command.Report != EStageAIReport::All && Command.Report != Report.Kind)
				{
					continue;
				}
				bool bStarted = false;
				const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bStarted = (Controller.*Report.Request)(); });
				if (bStarted)
				{
					Started.Add(Report.Name);
				}
				else
				{
					Failed.Add(FText::Format(LOCTEXT("ExportRefused", "the {0} was not exported: {1}"), Report.Name, RefusalOf(Refusals, LOCTEXT("ExportFailed", "it could not start"))));
				}
			}
			const FText Separator = LOCTEXT("ExportSeparator", ", ");
			if (Started.IsEmpty())
			{
				return Refused(FText::Format(LOCTEXT("NothingExported", "Nothing was exported: {0}."), FText::Join(LOCTEXT("FailSeparator", "; "), Failed)));
			}
			const FText StartedText = FText::Format(LOCTEXT("Exporting", "Exporting the {0} as PDF and CSV to Saved/StageCraft/Reports; the status bar shows when each file is written."),
				FText::Join(Separator, Started));
			return Failed.IsEmpty()
				? Done(StartedText)
				: Done(FText::Format(LOCTEXT("ExportPartial", "{0} Also: {1}."), StartedText, FText::Join(LOCTEXT("FailSeparator", "; "), Failed)));
		}

		FStageAIActionOutcome StepHistory(AModularPlayerController& Controller, bool bUndo)
		{
			const UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(Controller.GetWorld());
			const FText Description = History ? (bUndo ? History->GetUndoDescription() : History->GetRedoDescription()) : FText::GetEmpty();
			EStageCommandResult Result = EStageCommandResult::NothingToDo;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Result = bUndo ? Controller.RequestUndo() : Controller.RequestRedo(); });
			switch (Result)
			{
			case EStageCommandResult::Succeeded:
				return Done(FText::Format(bUndo ? LOCTEXT("Undone", "Undid \"{0}\".") : LOCTEXT("Redone", "Redid \"{0}\"."), Description));
			case EStageCommandResult::NothingToDo:
				return Refused(bUndo ? LOCTEXT("NothingToUndo", "There is nothing to undo.") : LOCTEXT("NothingToRedo", "There is nothing to redo."));
			case EStageCommandResult::Invalid:
				return Refused(FText::Format(LOCTEXT("StepInvalid", "\"{0}\" can no longer be applied and was dropped from the history."), Description));
			default:
				return Refused(RefusalOf(Refusals, FText::Format(LOCTEXT("StepRefused", "\"{0}\" is not allowed right now."), Description)));
			}
		}
	}

	FStageAIActionOutcome Run(AModularPlayerController& Controller, const FStageAICommand& Command)
	{
		switch (Command.Intent)
		{
		case EStageAIIntent::AddFixtures:	return AddFixtures(Controller, Command);
		case EStageAIIntent::StoreCue:		return StoreCue(Controller, Command);
		case EStageAIIntent::GoToCue:
		case EStageAIIntent::CueGo:
		case EStageAIIntent::CueBack:		return PlayCue(Controller, Command);
		case EStageAIIntent::SetWeather:
		case EStageAIIntent::SetTimeOfDay:
		case EStageAIIntent::SetSky:		return ChangeEnvironment(Controller, Command);
		case EStageAIIntent::Export:		return Export(Controller, Command);
		case EStageAIIntent::Undo:			return StepHistory(Controller, /*bUndo*/ true);
		case EStageAIIntent::Redo:			return StepHistory(Controller, /*bUndo*/ false);
		default:							return Refused(LOCTEXT("NotAnAction", "That is not something I can change."));
		}
	}
}

#undef LOCTEXT_NAMESPACE
