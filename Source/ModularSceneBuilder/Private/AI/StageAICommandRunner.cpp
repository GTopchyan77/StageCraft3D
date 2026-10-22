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
		using StageAILanguage::Pick;

		/** The cue panel's default crossfade. */
		constexpr float DefaultFadeSeconds = 2.f;

		FStageAIActionOutcome Done(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, Message); }
		FStageAIActionOutcome Refused(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Refused, Message); }

		/**
		 * Work's first reported refusal, else Fallback. Refusals are worded by the rules that made them (GameMode, economy) in the UI
		 * language; an Armenian reply quotes them as they are.
		 */
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
			const EStageAILanguage Language = Command.Language;
			UWorld* World = Controller.GetWorld();
			UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			const UStageItemSubsystem* Catalog = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
			if (!Assistant || !Catalog)
			{
				return Refused(Pick(Language, LOCTEXT("NoCatalog", "The catalog is not available in this level."), INVTEXT("Կատալոգը հասանելի չէ այս մակարդակում։")));
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
					? Pick(Language, LOCTEXT("NoFixtures", "The catalog has no lighting fixtures to add."), INVTEXT("Կատալոգում ավելացնելու լուսային սարքեր չկան։"))
					: Pick(Language, LOCTEXT("CatalogLoading", "The catalog is still loading; try again in a moment."), INVTEXT("Կատալոգը դեռ բեռնվում է, փորձեք մի պահից։")));
			}
			const int32 Match = StageAICommandParser::MatchFixture(Candidates, Command.ItemQuery);
			if (Match == INDEX_NONE)
			{
				TArray<FText> Names;
				for (const FStageAIFixtureCandidate& Candidate : Candidates)
				{
					Names.Add(FText::FromString(Candidate.Name));
				}
				return Refused(FText::Format(Pick(Language, LOCTEXT("NoMatch", "No lighting fixture in the catalog matches \"{0}\". I can add: {1}."),
						INVTEXT("Կատալոգում «{0}»-ին համապատասխանող լուսային սարք չկա։ Կարող եմ ավելացնել՝ {1}։")),
					FText::FromString(Command.ItemQuery), FText::Join(LOCTEXT("Separator", ", "), Names)));
			}
			ULightingFixtureData* Item = Fixtures[Match];

			// The rules may refuse this item outright (locked, not owned): say so before streaming anything.
			const FStageEconomyResultInfo Allowed = Controller.CanPlaceItem(Item);
			if (!Allowed.IsSuccess())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("NotAllowed", "I can't place {0}: {1}"), INVTEXT("Չեմ կարող տեղադրել {0}՝ {1}")), Item->DisplayName, Allowed.Message));
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
				return Refused(Plan.Warnings.IsEmpty()
					? Pick(Language, LOCTEXT("PlanFailed", "The fixtures could not be laid out."), INVTEXT("Սարքերը հնարավոր չեղավ դասավորել։"))
					: Plan.Warnings[0]);
			}

			// The undo step is named in the UI language, like every other entry of the Edit menu.
			const FText Name = Item->DisplayName;
			const FText StepName = FText::Format(LOCTEXT("AddStep", "Assistant: add {0} × {1}"), FText::AsNumber(Count), Name);
			TWeakObjectPtr<AModularPlayerController> WeakController = &Controller;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = Assistant;
			const bool bStarted = Assistant->RequestItemAssets(*Item, FSimpleDelegate::CreateLambda([WeakController, WeakAssistant, Plan, StepName, Name, Count, Language]()
			{
				UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
				AModularPlayerController* PendingController = WeakController.Get();
				if (!PendingAssistant)
				{
					return;
				}
				if (!PendingController)
				{
					PendingAssistant->FinishPendingAction(Refused(Pick(Language, LOCTEXT("PlayerGone", "Nothing was placed: the player who asked has left."),
						INVTEXT("Ոչինչ չտեղադրվեց. խնդրողը դուրս է եկել։"))));
					return;
				}
				FStageSketchGenerateResult Result;
				PendingController->RunCapturingRejections([&]() { Result = PendingController->RequestBuildPlan(Plan, StepName); });
				PendingAssistant->FinishPendingAction(Result.bSucceeded
					? Done(FText::Format(Pick(Language, LOCTEXT("Added", "Added {0} × {1} in a row on the floor, ready to move into place. Ctrl+Z removes all of them."),
						INVTEXT("Ավելացվեց {0} × {1}՝ մեկ շարքով հատակին, պատրաստ տեղափոխման։ Ctrl+Z-ը կհեռացնի բոլորը։")), FText::AsNumber(Count), Name))
					: Refused(FText::Format(Pick(Language, LOCTEXT("AddRefused", "Nothing was placed: {0}"), INVTEXT("Ոչինչ չտեղադրվեց՝ {0}")), Result.Message)));
			}));
			return bStarted
				? FStageAIActionOutcome::Make(EStageAIOutcome::Pending, FText::Format(Pick(Language, LOCTEXT("Placing", "Placing {0} × {1}…"), INVTEXT("Տեղադրում եմ {0} × {1}…")),
					FText::AsNumber(Count), Name))
				: Refused(Pick(Language, LOCTEXT("LoadNotStarted", "The fixture could not be loaded."), INVTEXT("Սարքը հնարավոր չեղավ բեռնել։")));
		}

		FStageAIActionOutcome StoreCue(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Controller.GetWorld());
			if (!Show)
			{
				return Refused(Pick(Language, LOCTEXT("NoShow", "Cues are not available in this level."), INVTEXT("Կյուները հասանելի չեն այս մակարդակում։")));
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
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Language, LOCTEXT("StoreFailed", "Cue {0} could not be stored."), INVTEXT("Կյու {0}-ը հնարավոր չեղավ պահել։")),
					FormatCue(Number))));
			}
			const FText Label = Command.Label.IsEmpty() ? FText::GetEmpty()
				: FText::Format(Pick(Language, LOCTEXT("CueLabel", " \"{0}\""), INVTEXT(" «{0}»")), FText::FromString(Command.Label));
			return Done(FText::Format(bReplacing
					? Pick(Language, LOCTEXT("CueReplaced", "Re-recorded cue {0}{1} with a {2} s fade from every fixture's current look. Ctrl+Z restores the old one."),
						INVTEXT("Կյու {0}{1}-ը վերագրանցվեց {2} վ անցումով՝ բոլոր սարքերի ընթացիկ վիճակից։ Ctrl+Z-ը կվերադարձնի նախորդը։"))
					: Pick(Language, LOCTEXT("CueStored", "Stored cue {0}{1} with a {2} s fade from every fixture's current look. Ctrl+Z removes it."),
						INVTEXT("Պահվեց կյու {0}{1}՝ {2} վ անցումով, բոլոր սարքերի ընթացիկ վիճակից։ Ctrl+Z-ը կհեռացնի այն։")),
				FormatCue(Number), Label, FText::AsNumber(Fade)));
		}

		FStageAIActionOutcome PlayCue(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
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
					? FText::Format(Pick(Language, LOCTEXT("NoSuchCue", "There is no cue {0}. Say \"list cues\" to see them."), INVTEXT("Կյու {0} չկա։ Գրեք «կյուների ցանկը»՝ դրանք տեսնելու համար։")),
						FormatCue(*Command.CueNumber))
					: Command.Intent == EStageAIIntent::CueGo
						? Pick(Language, LOCTEXT("NoNextCue", "There is no next cue to play."), INVTEXT("Հաջորդ կյու չկա։"))
						: Pick(Language, LOCTEXT("NoPreviousCue", "There is no previous cue to go back to."), INVTEXT("Նախորդ կյու չկա։"));
				return Refused(RefusalOf(Refusals, Fallback));
			}
			const FShowCue* Active = Show->FindCue(Show->GetActiveCueNumber());
			const FText Label = Active && !Active->Label.IsEmpty()
				? FText::Format(Pick(Language, LOCTEXT("ActiveLabel", " \"{0}\""), INVTEXT(" «{0}»")), Active->Label) : FText::GetEmpty();
			return Done(FText::Format(Pick(Language, LOCTEXT("Playing", "Playing cue {0}{1}."), INVTEXT("Միացված է կյու {0}{1}։")), FormatCue(Show->GetActiveCueNumber()), Label));
		}

		FStageAIActionOutcome ChangeEnvironment(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(Controller.GetWorld());
			if (!Environment)
			{
				return Refused(Pick(Language, LOCTEXT("NoEnvironment", "This level has no outdoor environment."), INVTEXT("Այս մակարդակում բացօթյա միջավայր չկա։")));
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
					return Refused(Pick(Language, LOCTEXT("NoSkySystem", "No sky system is installed in this build, so the stage stays indoors."),
						INVTEXT("Այս տարբերակում երկնքի համակարգ տեղադրված չէ, ուստի բեմը մնում է փակ։")));
				}
				Settings.bSkyEnabled = Command.bSkyOn;
				StepName = Command.bSkyOn ? LOCTEXT("SkyOnStep", "Assistant: outdoor sky on") : LOCTEXT("SkyOffStep", "Assistant: outdoor sky off");
				break;
			}

			bool bApplied = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bApplied = Controller.RequestSetEnvironment(Settings, StepName); });
			if (!bApplied)
			{
				return Refused(RefusalOf(Refusals, Pick(Language, LOCTEXT("EnvironmentFailed", "The environment could not be changed."), INVTEXT("Միջավայրը հնարավոր չեղավ փոխել։"))));
			}

			// Describe what was stored, not what was asked for.
			const FStageEnvironmentSettings Stored = Environment->GetSettings();
			const FText SkyNote = !Stored.bSkyEnabled
				? (bSkyAvailable
					? Pick(Language, LOCTEXT("SkyOffNote", " The outdoor sky is off, so say \"go outdoors\" to see it."), INVTEXT(" Բացօթյա երկինքն անջատված է. գրեք «բացօթյա»՝ այն տեսնելու համար։"))
					: Pick(Language, LOCTEXT("NoSkyNote", " No sky system is installed in this build, so it is stored but not shown."),
						INVTEXT(" Այս տարբերակում երկնքի համակարգ չկա, ուստի այն պահվել է, բայց չի ցուցադրվում։")))
				: (!bSkyWasOn ? Pick(Language, LOCTEXT("SkyTurnedOn", " The outdoor sky is now on."), INVTEXT(" Բացօթյա երկինքն այժմ միացված է։")) : FText::GetEmpty());
			switch (Command.Intent)
			{
			case EStageAIIntent::SetWeather:
				return Done(FText::Format(Pick(Language, LOCTEXT("WeatherSet", "Weather set to {0}.{1} Ctrl+Z restores the previous environment."),
						INVTEXT("Եղանակը՝ {0}։{1} Ctrl+Z-ը կվերադարձնի նախորդ միջավայրը։")),
					StageAIAnswers::DescribeWeather(StageEnvironment::FindWeatherPreset(Stored), Language), SkyNote));
			case EStageAIIntent::SetTimeOfDay:
				return Done(FText::Format(Pick(Language, LOCTEXT("TimeSet", "Time of day set to {0}.{1} Ctrl+Z restores the previous environment."),
						INVTEXT("Օրվա ժամը՝ {0}։{1} Ctrl+Z-ը կվերադարձնի նախորդ միջավայրը։")),
					FText::FromString(StageAIAnswers::FormatHour(Stored.TimeOfDay)), SkyNote));
			default:
				return Done(Stored.bSkyEnabled
					? Pick(Language, LOCTEXT("SkyOn", "The outdoor sky is on: the sun, sky and weather replace the indoor lighting. Ctrl+Z turns it off again."),
						INVTEXT("Բացօթյա երկինքը միացված է. արևը, երկինքը և եղանակը փոխարինում են ներքին լուսավորությանը։ Ctrl+Z-ը կանջատի այն։"))
					: Pick(Language, LOCTEXT("SkyOff", "Indoor stage: the outdoor sky is off. Ctrl+Z turns it back on."),
						INVTEXT("Փակ բեմ. բացօթյա երկինքն անջատված է։ Ctrl+Z-ը նորից կմիացնի այն։")));
			}
		}

		FStageAIActionOutcome Export(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			struct FReport
			{
				EStageAIReport Kind;
				FText Name;
				bool (AModularPlayerController::*Request)();
			};
			const FReport Reports[] = {
				{ EStageAIReport::BillOfMaterials, Pick(Language, LOCTEXT("Bom", "bill of materials"), INVTEXT("նյութերի ցուցակը")), &AModularPlayerController::RequestExportBillOfMaterials },
				{ EStageAIReport::PatchSheet, Pick(Language, LOCTEXT("Patch", "DMX patch sheet"), INVTEXT("DMX պատչ-թերթը")), &AModularPlayerController::RequestExportPatchSheet },
				{ EStageAIReport::CableSchedule, Pick(Language, LOCTEXT("Cables", "cable schedule and power plan"), INVTEXT("մալուխների ցուցակը և սնուցման պլանը")),
					&AModularPlayerController::RequestExportCableSchedule },
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
					Failed.Add(FText::Format(Pick(Language, LOCTEXT("ExportRefused", "the {0} was not exported: {1}"), INVTEXT("{0} չարտահանվեց՝ {1}")), Report.Name,
						RefusalOf(Refusals, Pick(Language, LOCTEXT("ExportFailed", "it could not start"), INVTEXT("չհաջողվեց սկսել")))));
				}
			}
			const FText Separator = LOCTEXT("ExportSeparator", ", ");
			if (Started.IsEmpty())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("NothingExported", "Nothing was exported: {0}."), INVTEXT("Ոչինչ չարտահանվեց՝ {0}։")),
					FText::Join(LOCTEXT("FailSeparator", "; "), Failed)));
			}
			const FText StartedText = FText::Format(Pick(Language,
					LOCTEXT("Exporting", "Exporting the {0} as PDF and CSV to Saved/StageCraft/Reports; the status bar shows when each file is written."),
					INVTEXT("Արտահանում եմ {0}՝ PDF և CSV ձևաչափով, Saved/StageCraft/Reports թղթապանակում. վիճակի տողը ցույց կտա, երբ յուրաքանչյուր ֆայլ գրվի։")),
				FText::Join(Separator, Started));
			return Failed.IsEmpty()
				? Done(StartedText)
				: Done(FText::Format(Pick(Language, LOCTEXT("ExportPartial", "{0} Also: {1}."), INVTEXT("{0} Նաև՝ {1}։")), StartedText, FText::Join(LOCTEXT("FailSeparator", "; "), Failed)));
		}

		FStageAIActionOutcome StepHistory(AModularPlayerController& Controller, bool bUndo, EStageAILanguage Language)
		{
			const UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(Controller.GetWorld());
			const FText Description = History ? (bUndo ? History->GetUndoDescription() : History->GetRedoDescription()) : FText::GetEmpty();
			EStageCommandResult Result = EStageCommandResult::NothingToDo;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Result = bUndo ? Controller.RequestUndo() : Controller.RequestRedo(); });
			switch (Result)
			{
			case EStageCommandResult::Succeeded:
				return Done(FText::Format(bUndo
					? Pick(Language, LOCTEXT("Undone", "Undid \"{0}\"."), INVTEXT("Հետարկվեց «{0}»։"))
					: Pick(Language, LOCTEXT("Redone", "Redid \"{0}\"."), INVTEXT("Կրկնվեց «{0}»։")), Description));
			case EStageCommandResult::NothingToDo:
				return Refused(bUndo
					? Pick(Language, LOCTEXT("NothingToUndo", "There is nothing to undo."), INVTEXT("Հետարկելու բան չկա։"))
					: Pick(Language, LOCTEXT("NothingToRedo", "There is nothing to redo."), INVTEXT("Կրկնելու բան չկա։")));
			case EStageCommandResult::Invalid:
				return Refused(FText::Format(Pick(Language, LOCTEXT("StepInvalid", "\"{0}\" can no longer be applied and was dropped from the history."),
					INVTEXT("«{0}»-ն այլևս հնարավոր չէ կիրառել և հեռացվեց պատմությունից։")), Description));
			default:
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Language, LOCTEXT("StepRefused", "\"{0}\" is not allowed right now."),
					INVTEXT("«{0}»-ն այս պահին թույլատրված չէ։")), Description)));
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
		case EStageAIIntent::Undo:			return StepHistory(Controller, /*bUndo*/ true, Command.Language);
		case EStageAIIntent::Redo:			return StepHistory(Controller, /*bUndo*/ false, Command.Language);
		default:
			return Refused(Pick(Command.Language, LOCTEXT("NotAnAction", "That is not something I can change."), INVTEXT("Դա այն չէ, ինչ կարող եմ փոխել։")));
		}
	}
}

#undef LOCTEXT_NAMESPACE
