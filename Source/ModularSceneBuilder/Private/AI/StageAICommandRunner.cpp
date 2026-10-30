// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandRunner.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "Algo/AllOf.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SelectionComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageArcTrussData.h"
#include "Data/StageDeckData.h"
#include "Data/StageStairsData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "History/StageEditHistorySubsystem.h"
#include "Placement/StageDeckMath.h"
#include "Placement/StageProceduralShapes.h"
#include "Show/StageGelLibrary.h"
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

		// --- Targeted edits and stage shapes (Docs/ADR/0018 §6) ---

		/** Gap between new shapes and the items already placed, and the grid their place snaps to (cm). */
		constexpr double ShapeGap = 200.0;
		constexpr double ShapeGrid = 50.0;

		/** Hang height of the bottom of a curved truss when none is given, as the Sketch window's default truss height. */
		constexpr double DefaultArcTrussHeight = 600.0;

		/** A round or curved riser without sizes: 2 m radius; a curved riser is 1 m deep and 180 degrees wide. */
		constexpr double DefaultRiserRadius = 200.0;
		constexpr double DefaultArcRiserWidth = 100.0;
		constexpr double DefaultArcRiserAngle = 180.0;
		constexpr double MaxRiserRadius = 2500.0;

		/** "2 × Spot 350, 1 × LED Par". */
		FText DescribeItems(TConstArrayView<AModularBaseActor*> Items)
		{
			TArray<TPair<FText, int32>> Counts;
			for (const AModularBaseActor* Item : Items)
			{
				const FText Name = Item && Item->GetItemData() ? Item->GetItemData()->DisplayName : LOCTEXT("UnnamedItem", "item");
				if (TPair<FText, int32>* Entry = Counts.FindByPredicate([&Name](const TPair<FText, int32>& Existing) { return Existing.Key.EqualTo(Name); }))
				{
					++Entry->Value;
				}
				else
				{
					Counts.Emplace(Name, 1);
				}
			}
			TArray<FText> Parts;
			for (const TPair<FText, int32>& Entry : Counts)
			{
				Parts.Add(FText::Format(LOCTEXT("ItemTimes", "{0} × {1}"), FText::AsNumber(Entry.Value), Entry.Key));
			}
			return FText::Join(LOCTEXT("ItemSeparator", ", "), Parts);
		}

		/**
		 * The items a targeted edit applies to: the selection (narrowed by ItemQuery when it has one), or the placed items matching
		 * ItemQuery, the latest placed first, at most Count. Empty, with OutError in the command's language, when none.
		 */
		TArray<AModularBaseActor*> FindTargets(const AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			const EStageAILanguage Language = Command.Language;
			const auto Matches = [&Command](const AModularBaseActor& Item)
			{
				const UBaseItemData* Data = Item.GetItemData();
				return Command.ItemQuery.IsEmpty()
					|| (Data && StageAICommandParser::MatchesItemName(Data->DisplayName.ToString(), Command.ItemQuery, Item.IsA<ALightingFixtureActor>()));
			};

			TArray<AModularBaseActor*> Targets;
			if (Command.Target == EStageAITarget::Selection)
			{
				const USelectionComponent* Selection = Controller.GetSelection();
				for (AActor* Actor : Selection ? Selection->GetSelectedActors() : TArray<AActor*>())
				{
					AModularBaseActor* Item = Cast<AModularBaseActor>(Actor);
					if (IsValid(Item) && Matches(*Item))
					{
						Targets.Add(Item);
					}
				}
				if (Targets.IsEmpty())
				{
					OutError = Command.ItemQuery.IsEmpty()
						? Pick(Language, LOCTEXT("NothingSelected", "Nothing is selected. Select the items first, or name them (\"delete the spots\")."),
							INVTEXT("Ոչինչ ընտրված չէ։ Նախ ընտրեք իրերը կամ նշեք դրանց անունը («ջնջիր սփոթերը»)։"))
						: FText::Format(Pick(Language, LOCTEXT("NoSelectedMatch", "None of the selected items is a \"{0}\"."), INVTEXT("Ընտրված իրերից ոչ մեկը «{0}» չէ։")),
							FText::FromString(Command.ItemQuery));
				}
			}
			else
			{
				const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(Controller.GetWorld());
				const TArray<AModularBaseActor*> Placed = Session ? Session->GetPlacedItems() : TArray<AModularBaseActor*>();
				for (int32 Index = Placed.Num() - 1; Index >= 0; --Index)
				{
					if (IsValid(Placed[Index]) && Matches(*Placed[Index]))
					{
						Targets.Add(Placed[Index]);
						if (Command.Count.IsSet() && Targets.Num() >= *Command.Count)
						{
							break;
						}
					}
				}
				if (Targets.IsEmpty())
				{
					OutError = FText::Format(Pick(Language, LOCTEXT("NoPlacedMatch", "No placed item matches \"{0}\". Say \"status\" to see what is on the stage."),
						INVTEXT("Տեղադրված իրերից ոչ մեկը չի համապատասխանում «{0}»-ին։ Գրեք «վիճակը»՝ տեսնելու, թե ինչ կա բեմում։")), FText::FromString(Command.ItemQuery));
				}
			}
			if (Targets.Num() > StageAICommandParser::MaxEditTargets)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("TooManyTargets", "That is {0} items; I change at most {1} at a time. Select fewer, or give a number (\"delete 10 spots\")."),
						INVTEXT("Դա {0} իր է, իսկ ես միանգամից փոխում եմ առավելագույնը {1}։ Ընտրեք ավելի քիչ կամ նշեք քանակը («ջնջիր 10 սփոթ»)։")),
					FText::AsNumber(Targets.Num()), FText::AsNumber(StageAICommandParser::MaxEditTargets));
				Targets.Reset();
			}
			return Targets;
		}

		FStageAIActionOutcome DeleteItems(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			const TArray<AModularBaseActor*> Targets = FindTargets(Controller, Command, Error);
			if (Targets.IsEmpty())
			{
				return Refused(Error);
			}
			const FText What = DescribeItems(Targets);
			int32 Deleted = 0;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Deleted = Controller.RequestDeleteItems(Targets); });
			if (Deleted == 0)
			{
				return Refused(RefusalOf(Refusals, Pick(Language, LOCTEXT("NothingDeleted", "Nothing was deleted."), INVTEXT("Ոչինչ չջնջվեց։"))));
			}
			return Done(FText::Format(Pick(Language, LOCTEXT("Deleted", "Deleted {0}. Ctrl+Z brings {1}|plural(one=it,other=them) back."),
				INVTEXT("Ջնջվեց {0}։ Ctrl+Z-ը կվերադարձնի դրանք։")), What, FText::AsNumber(Deleted)));
		}

		FText DescribeDirection(EStageAIDirection Direction, EStageAILanguage Language)
		{
			switch (Direction)
			{
			case EStageAIDirection::Left:		return Pick(Language, LOCTEXT("DirLeft", "left"), INVTEXT("ձախ"));
			case EStageAIDirection::Right:		return Pick(Language, LOCTEXT("DirRight", "right"), INVTEXT("աջ"));
			case EStageAIDirection::Forward:	return Pick(Language, LOCTEXT("DirForward", "forward"), INVTEXT("առաջ"));
			case EStageAIDirection::Back:		return Pick(Language, LOCTEXT("DirBack", "back"), INVTEXT("հետ"));
			case EStageAIDirection::Up:			return Pick(Language, LOCTEXT("DirUp", "up"), INVTEXT("վեր"));
			default:							return Pick(Language, LOCTEXT("DirDown", "down"), INVTEXT("ներքև"));
			}
		}

		FText DescribeDistance(double Cm, EStageAILanguage Language)
		{
			FNumberFormattingOptions Format;
			Format.SetMaximumFractionalDigits(2);
			return Cm >= 100.0
				? FText::Format(Pick(Language, LOCTEXT("DistanceM", "{0} m"), INVTEXT("{0} մ")), FText::AsNumber(Cm / 100.0, &Format))
				: FText::Format(Pick(Language, LOCTEXT("DistanceCm", "{0} cm"), INVTEXT("{0} սմ")), FText::AsNumber(Cm, &Format));
		}

		/** Left, right, forward and back as the camera sees the stage, snapped to the nearest stage axis so items stay on their grid. */
		FVector GetMoveOffset(const AModularPlayerController& Controller, EStageAIDirection Direction, double Distance)
		{
			if (Direction == EStageAIDirection::Up || Direction == EStageAIDirection::Down)
			{
				return FVector(0.0, 0.0, Direction == EStageAIDirection::Up ? Distance : -Distance);
			}
			const double Yaw = Controller.PlayerCameraManager ? Controller.PlayerCameraManager->GetCameraRotation().Yaw : Controller.GetControlRotation().Yaw;
			const double Snapped = FMath::GridSnap(FRotator::NormalizeAxis(Yaw), 90.0);
			const auto Axis = [](double Degrees)
			{
				const FVector Raw = FRotator(0.0, Degrees, 0.0).Vector();
				return FVector(FMath::RoundToDouble(Raw.X), FMath::RoundToDouble(Raw.Y), 0.0);
			};
			switch (Direction)
			{
			case EStageAIDirection::Forward:	return Axis(Snapped) * Distance;
			case EStageAIDirection::Back:		return -Axis(Snapped) * Distance;
			case EStageAIDirection::Right:		return Axis(Snapped + 90.0) * Distance;
			default:							return -Axis(Snapped + 90.0) * Distance;
			}
		}

		FStageAIActionOutcome MoveItems(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			const TArray<AModularBaseActor*> Targets = FindTargets(Controller, Command, Error);
			if (Targets.IsEmpty())
			{
				return Refused(Error);
			}
			const double Distance = Command.DistanceCm.Get(StageAICommandParser::DefaultMoveDistanceCm);
			const FVector Offset = GetMoveOffset(Controller, Command.Direction, Distance);
			FStageEconomyResultInfo Result;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Result = Controller.RequestMoveItems(Targets, Offset); });
			if (!Result.IsSuccess())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("MoveRefused", "Nothing was moved: {0}"), INVTEXT("Ոչինչ չտեղափոխվեց՝ {0}")),
					RefusalOf(Refusals, Result.Message)));
			}
			return Done(FText::Format(Pick(Language, LOCTEXT("Moved", "Moved {0} {1} {2}. Ctrl+Z moves {3}|plural(one=it,other=them) back."),
					INVTEXT("{0}-ը տեղափոխվեց {1} {2}։ Ctrl+Z-ը կվերադարձնի դրանք։")),
				DescribeItems(Targets), DescribeDistance(Distance, Language), DescribeDirection(Command.Direction, Language), FText::AsNumber(Targets.Num())));
		}

		/** The catalog item Query names, for a replacement: fixtures first when it only says "light". Null with OutError when none. */
		UBaseItemData* FindCatalogItem(const UStageItemSubsystem& Catalog, const FString& Query, EStageAILanguage Language, FText& OutError)
		{
			const bool bFixturesOnly = StageAICommandParser::IsGenericFixtureQuery(Query);
			TArray<UBaseItemData*> Items;
			TArray<FStageAIFixtureCandidate> Candidates;
			for (UBaseItemData* Item : Catalog.GetCatalog())
			{
				const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item);
				if (!Item || (bFixturesOnly && !Fixture))
				{
					continue;
				}
				Items.Add(Item);
				Candidates.Add({ Item->DisplayName.ToString(), Fixture ? UEnum::GetDisplayValueAsText(Fixture->FixtureKind).ToString()
					: UEnum::GetDisplayValueAsText(Item->ItemType).ToString() });
			}
			const int32 Match = StageAICommandParser::MatchFixture(Candidates, Query);
			if (Match == INDEX_NONE)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("NoCatalogMatch", "Nothing in the catalog matches \"{0}\"."), INVTEXT("Կատալոգում «{0}»-ին համապատասխանող իր չկա։")),
					FText::FromString(Query));
				return nullptr;
			}
			return Items[Match];
		}

		FStageAIActionOutcome ReplaceItems(AModularPlayerController& Controller, const FStageAICommand& Command)
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
			FText Error;
			const TArray<AModularBaseActor*> Targets = FindTargets(Controller, Command, Error);
			if (Targets.IsEmpty())
			{
				return Refused(Error);
			}
			UBaseItemData* Item = FindCatalogItem(*Catalog, Command.Replacement, Language, Error);
			if (!Item)
			{
				return Refused(Error);
			}
			if (Algo::AllOf(Targets, [Item](const AModularBaseActor* Target) { return Target->GetItemData() == Item; }))
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("AlreadyThat", "Those are already {0}."), INVTEXT("Դրանք արդեն {0} են։")), Item->DisplayName));
			}
			const FStageEconomyResultInfo Allowed = Controller.CanPlaceItem(Item);
			if (!Allowed.IsSuccess())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("NotAllowed", "I can't place {0}: {1}"), INVTEXT("Չեմ կարող տեղադրել {0}՝ {1}")), Item->DisplayName, Allowed.Message));
			}

			const FText What = DescribeItems(Targets);
			const FText Name = Item->DisplayName;
			const FText StepName = FText::Format(LOCTEXT("ReplaceStep", "Assistant: replace {0} with {1}"), What, Name);
			TArray<TWeakObjectPtr<AModularBaseActor>> WeakTargets(Targets);
			TWeakObjectPtr<AModularPlayerController> WeakController = &Controller;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = Assistant;
			TWeakObjectPtr<UBaseItemData> WeakItem = Item;
			const bool bStarted = Assistant->RequestItemAssets(*Item, FSimpleDelegate::CreateLambda([WeakController, WeakAssistant, WeakTargets, WeakItem, StepName, What, Name, Language]()
			{
				UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
				AModularPlayerController* PendingController = WeakController.Get();
				if (!PendingAssistant)
				{
					return;
				}
				TArray<AModularBaseActor*> Remaining;
				for (const TWeakObjectPtr<AModularBaseActor>& Target : WeakTargets)
				{
					if (AModularBaseActor* Valid = Target.Get())
					{
						Remaining.Add(Valid);
					}
				}
				if (!PendingController || !WeakItem.IsValid() || Remaining.IsEmpty())
				{
					PendingAssistant->FinishPendingAction(Refused(Pick(Language, LOCTEXT("ReplaceGone", "Nothing was replaced: the items or the player are gone."),
						INVTEXT("Ոչինչ չփոխարինվեց. իրերը կամ խաղացողն այլևս չկան։"))));
					return;
				}
				FStageEconomyResultInfo Result;
				const TArray<FStageEconomyResultInfo> Refusals = PendingController->RunCapturingRejections([&]()
				{
					Result = PendingController->RequestReplaceItems(Remaining, WeakItem.Get(), StepName);
				});
				PendingAssistant->FinishPendingAction(Result.IsSuccess()
					? Done(FText::Format(Pick(Language, LOCTEXT("Replaced", "Replaced {0} with {1} × {2} in the same places. Ctrl+Z brings the old ones back."),
						INVTEXT("{0}-ը փոխարինվեց {1} × {2}-ով նույն տեղերում։ Ctrl+Z-ը կվերադարձնի նախկինները։")), What, FText::AsNumber(Remaining.Num()), Name))
					: Refused(RefusalOf(Refusals, Result.Message)));
			}));
			return bStarted
				? FStageAIActionOutcome::Make(EStageAIOutcome::Pending, FText::Format(Pick(Language, LOCTEXT("Replacing", "Replacing {0} with {1}…"), INVTEXT("Փոխարինում եմ {0}-ը {1}-ով…")),
					What, Name))
				: Refused(Pick(Language, LOCTEXT("ReplaceLoadNotStarted", "The new item could not be loaded."), INVTEXT("Նոր իրը հնարավոր չեղավ բեռնել։")));
		}

		template <typename TData>
		TData* FindCatalogItemOfClass(const UStageItemSubsystem& Catalog)
		{
			for (UBaseItemData* Item : Catalog.GetCatalog())
			{
				if (TData* Typed = Cast<TData>(Item))
				{
					return Typed;
				}
			}
			return nullptr;
		}

		/** Where a new shape of HalfDepth goes: in front of (towards -X) everything placed, or the stage centre when it is empty. */
		FVector2D FindShapeSpot(const UWorld* World, double HalfDepth)
		{
			const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
			TOptional<double> LowestX;
			for (const AModularBaseActor* Placed : Session ? Session->GetPlacedItems() : TArray<AModularBaseActor*>())
			{
				if (IsValid(Placed))
				{
					FVector Origin, Extent;
					Placed->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
					LowestX = FMath::Min(LowestX.Get(TNumericLimits<double>::Max()), Origin.X - Extent.X);
				}
			}
			return FVector2D(LowestX.IsSet() ? FMath::GridSnap(*LowestX - ShapeGap - HalfDepth, ShapeGrid) : 0.0, 0.0);
		}

		/** The plan for one shape and the item it uses; false with OutError when the catalog has no item for it. */
		bool PlanShape(const UWorld* World, const UStageItemSubsystem& Catalog, const FStageAICommand& Command, FStageSketchBuildPlan& OutPlan,
			UBaseItemData*& OutItem, FText& OutName, FText& OutError)
		{
			const EStageAILanguage Language = Command.Language;
			switch (Command.Shape)
			{
			case EStageAIShape::RoundRiser:
			case EStageAIShape::ArcRiser:
			{
				UStageDeckData* Deck = FindCatalogItemOfClass<UStageDeckData>(Catalog);
				if (!Deck)
				{
					OutError = Pick(Language, LOCTEXT("NoDeckItem", "The catalog has no stage deck to build a riser from."), INVTEXT("Կատալոգում հարթակ չկա, որից ռայզեր կառուցեմ։"));
					return false;
				}
				const double Radius = FMath::Clamp(Command.RadiusCm.Get(DefaultRiserRadius), 25.0, MaxRiserRadius);
				const bool bRound = Command.Shape == EStageAIShape::RoundRiser;
				TArray<FVector2D> Outline = bRound ? StageProceduralShapes::MakeCircle(Radius)
					: StageProceduralShapes::MakeArcRing(FMath::Max(0.0, Radius - Command.WidthCm.Get(DefaultArcRiserWidth)), Radius,
						FMath::Min(Command.AngleDegrees.Get(DefaultArcRiserAngle), 359.0));
				if (Outline.Num() < 3)
				{
					OutError = Pick(Language, LOCTEXT("BadRiserSize", "Those riser sizes do not make a shape."), INVTEXT("Ռայզերի այդ չափերը ձև չեն կազմում։"));
					return false;
				}
				const FVector2D Spot = FindShapeSpot(World, Radius);
				FStageSketchDeckPlan& Planned = OutPlan.Decks.AddDefaulted_GetRef();
				Planned.Transform = FTransform(FVector(Spot, 0.0));
				Planned.Area = FMath::Abs(StageDeckMath::GetSignedArea(Outline));
				Planned.LocalOutline = MoveTemp(Outline);
				Planned.Height = FMath::Clamp(Command.HeightCm.Get(Deck->GetDefaultHeight()), double(Deck->GetMinHeight()), double(Deck->GetMaxHeight()));
				Planned.Panels = StageDeckMath::GetPanelCount(Planned.Area, Deck->GetPanelArea());
				OutPlan.DeckItem = Deck;
				OutItem = Deck;
				OutName = bRound ? Pick(Language, LOCTEXT("RoundRiserName", "a round riser"), INVTEXT("կլոր ռայզեր"))
					: Pick(Language, LOCTEXT("ArcRiserName", "a curved riser"), INVTEXT("կոր ռայզեր"));
				return true;
			}
			case EStageAIShape::Stairs:
			{
				UStageStairsData* Stairs = FindCatalogItemOfClass<UStageStairsData>(Catalog);
				if (!Stairs)
				{
					OutError = Pick(Language, LOCTEXT("NoStairsItem", "The catalog has no stairs item."), INVTEXT("Կատալոգում աստիճաններ չկան։"));
					return false;
				}
				FStageSketchStairsPlan& Planned = OutPlan.Stairs.AddDefaulted_GetRef();
				Planned.Rise = FMath::Clamp(Command.HeightCm.Get(Stairs->GetDefaultRise()), double(Stairs->GetMinRise()), double(Stairs->GetMaxRise()));
				Planned.Width = FMath::Clamp(Command.WidthCm.Get(Stairs->GetDefaultWidth()), double(Stairs->GetMinWidth()), double(Stairs->GetMaxWidth()));
				Planned.bHandrails = Command.bHandrails;
				const FStageStairsLayout Layout = StageProceduralShapes::LayoutStairs(Planned.Rise, Planned.Width, Stairs->TreadDepth, Stairs->MaxStepRise);
				Planned.Transform = FTransform(FVector(FindShapeSpot(World, Layout.Run * 0.5), 0.0));
				OutPlan.StairsItem = Stairs;
				OutItem = Stairs;
				OutName = Pick(Language, LOCTEXT("StairsName", "stairs"), INVTEXT("աստիճաններ"));
				return true;
			}
			default:
			{
				UStageArcTrussData* Arc = FindCatalogItemOfClass<UStageArcTrussData>(Catalog);
				if (!Arc)
				{
					OutError = Pick(Language, LOCTEXT("NoArcItem", "The catalog has no curved truss item."), INVTEXT("Կատալոգում կոր ֆերմա չկա։"));
					return false;
				}
				FStageSketchArcTrussPlan& Planned = OutPlan.ArcTrusses.AddDefaulted_GetRef();
				Planned.Radius = FMath::Clamp(Command.RadiusCm.Get(Arc->GetDefaultRadius()), double(Arc->GetMinRadius()), double(Arc->GetMaxRadius()));
				Planned.AngleDegrees = FMath::Clamp(Command.AngleDegrees.Get(Arc->GetDefaultAngle()), double(Arc->GetMinAngle()), double(Arc->GetMaxAngle()));
				Planned.Sections = StageProceduralShapes::LayoutArcTruss(Planned.Radius, Planned.AngleDegrees, Arc->ProfileWidth, Arc->SectionLength).Sections;
				// The arc's centre goes on the spot; the actor's origin is its apex, Radius along +Y from the centre, on the profile's centre line.
				const FVector2D Spot = FindShapeSpot(World, Planned.Radius + Arc->ProfileWidth);
				const double Height = FMath::Max(0.0, Command.HeightCm.Get(DefaultArcTrussHeight));
				Planned.Transform = FTransform(FVector(Spot.X, Spot.Y + Planned.Radius, Height + Arc->ProfileWidth * 0.5));
				OutPlan.ArcTrussItem = Arc;
				OutItem = Arc;
				OutName = Planned.AngleDegrees >= 360.0
					? Pick(Language, LOCTEXT("CircleTrussName", "a circular truss"), INVTEXT("շրջանաձև ֆերմա"))
					: Pick(Language, LOCTEXT("ArcTrussName", "a curved truss"), INVTEXT("կոր ֆերմա"));
				return true;
			}
			}
		}

		FStageAIActionOutcome BuildShape(AModularPlayerController& Controller, const FStageAICommand& Command)
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
			FStageSketchBuildPlan Plan;
			UBaseItemData* Item = nullptr;
			FText Name, Error;
			if (!PlanShape(World, *Catalog, Command, Plan, Item, Name, Error))
			{
				return Refused(Error);
			}
			const FStageEconomyResultInfo Allowed = Controller.CanPlaceItem(Item);
			if (!Allowed.IsSuccess())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("NotAllowed", "I can't place {0}: {1}"), INVTEXT("Չեմ կարող տեղադրել {0}՝ {1}")), Item->DisplayName, Allowed.Message));
			}

			const FText StepName = FText::Format(LOCTEXT("ShapeStep", "Assistant: build {0}"), Name);
			TWeakObjectPtr<AModularPlayerController> WeakController = &Controller;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = Assistant;
			const bool bStarted = Assistant->RequestItemAssets(*Item, FSimpleDelegate::CreateLambda([WeakController, WeakAssistant, Plan, StepName, Name, Language]()
			{
				UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
				AModularPlayerController* PendingController = WeakController.Get();
				if (!PendingAssistant)
				{
					return;
				}
				if (!PendingController)
				{
					PendingAssistant->FinishPendingAction(Refused(Pick(Language, LOCTEXT("ShapePlayerGone", "Nothing was built: the player who asked has left."),
						INVTEXT("Ոչինչ չկառուցվեց. խնդրողը դուրս է եկել։"))));
					return;
				}
				FStageSketchGenerateResult Result;
				PendingController->RunCapturingRejections([&]() { Result = PendingController->RequestBuildPlan(Plan, StepName); });
				PendingAssistant->FinishPendingAction(Result.bSucceeded
					? Done(FText::Format(Pick(Language, LOCTEXT("ShapeBuilt", "Built {0} in front of the other items. Select it to change its size in the inspector; Ctrl+Z removes it."),
						INVTEXT("Կառուցվեց {0}՝ մյուս իրերի առջև։ Ընտրեք այն՝ չափերը տեսուչում փոխելու համար. Ctrl+Z-ը կհեռացնի այն։")), Name))
					: Refused(FText::Format(Pick(Language, LOCTEXT("ShapeRefused", "Nothing was built: {0}"), INVTEXT("Ոչինչ չկառուցվեց՝ {0}")), Result.Message)));
			}));
			return bStarted
				? FStageAIActionOutcome::Make(EStageAIOutcome::Pending, FText::Format(Pick(Language, LOCTEXT("Building", "Building {0}…"), INVTEXT("Կառուցում եմ {0}…")), Name))
				: Refused(Pick(Language, LOCTEXT("ShapeLoadNotStarted", "The item could not be loaded."), INVTEXT("Իրը հնարավոր չեղավ բեռնել։")));
		}

		FText DescribeControl(const FStageAIFixtureSetting& Setting, EStageAILanguage Language)
		{
			FNumberFormattingOptions Format;
			Format.SetMaximumFractionalDigits(1);
			const FText Value = Setting.Value.IsSet() ? FText::AsNumber(*Setting.Value, &Format) : FText::FromString(Setting.Name);
			const FText Percent = Setting.Value.IsSet() ? FText::AsNumber(*Setting.Value * 100.0, &Format) : Value;
			switch (Setting.Control)
			{
			case EStageAIFixtureControl::Dimmer:	return FText::Format(Pick(Language, LOCTEXT("CtlDimmer", "dimmer {0} %"), INVTEXT("դիմեր {0} %")), Percent);
			case EStageAIFixtureControl::Color:		return FText::Format(Pick(Language, LOCTEXT("CtlColor", "colour {0}"), INVTEXT("գույն՝ {0}")), FText::FromString(Setting.Name));
			case EStageAIFixtureControl::Gel:		return FText::Format(Pick(Language, LOCTEXT("CtlGel", "gel {0}"), INVTEXT("ժել՝ {0}")), FText::FromString(Setting.Name));
			case EStageAIFixtureControl::Zoom:		return FText::Format(Pick(Language, LOCTEXT("CtlZoom", "zoom {0}°"), INVTEXT("զում {0}°")), Value);
			case EStageAIFixtureControl::Frost:		return FText::Format(Pick(Language, LOCTEXT("CtlFrost", "frost {0} %"), INVTEXT("ֆրոստ {0} %")), Percent);
			case EStageAIFixtureControl::Iris:		return FText::Format(Pick(Language, LOCTEXT("CtlIris", "iris {0} % closed"), INVTEXT("իրիս {0} % փակ")), Percent);
			case EStageAIFixtureControl::CTO:		return FText::Format(Pick(Language, LOCTEXT("CtlCto", "CTO {0} %"), INVTEXT("CTO {0} %")), Percent);
			case EStageAIFixtureControl::Prism:		return FText::Format(Pick(Language, LOCTEXT("CtlPrism", "prism {0}"), INVTEXT("պրիզմա {0}")), Value);
			case EStageAIFixtureControl::PrismSpin:	return FText::Format(Pick(Language, LOCTEXT("CtlPrismSpin", "prism spin {0} rpm"), INVTEXT("պրիզմայի պտույտ {0} պտ/ր")), Value);
			case EStageAIFixtureControl::Gobo:		return FText::Format(Pick(Language, LOCTEXT("CtlGobo", "gobo {0}"), INVTEXT("գոբո {0}")), Value);
			case EStageAIFixtureControl::GoboSpin:	return FText::Format(Pick(Language, LOCTEXT("CtlGoboSpin", "gobo spin {0} rpm"), INVTEXT("գոբոյի պտույտ {0} պտ/ր")), Value);
			case EStageAIFixtureControl::Strobe:	return FText::Format(Pick(Language, LOCTEXT("CtlStrobe", "strobe {0} Hz"), INVTEXT("ստրոբ {0} Հց")), Value);
			case EStageAIFixtureControl::Pan:		return FText::Format(Pick(Language, LOCTEXT("CtlPan", "pan {0}°"), INVTEXT("պան {0}°")), Value);
			default:								return FText::Format(Pick(Language, LOCTEXT("CtlTilt", "tilt {0}°"), INVTEXT("թիլթ {0}°")), Value);
			}
		}

		/** Slot of a gobo named Name on one of the fixture's wheels (rotating first), as the attribute that selects it. */
		bool FindGoboSlot(const ULightingFixtureData& Fixture, const FString& Name, FGameplayTag& OutAttribute, int32& OutSlot)
		{
			for (const EStageGoboWheel Wheel : { EStageGoboWheel::Rotating, EStageGoboWheel::Static })
			{
				for (int32 Slot = 1; Slot < Fixture.GetGoboSlotCount(Wheel); ++Slot)
				{
					if (Fixture.GetGoboSlotName(Slot, Wheel).ToString().Contains(Name, ESearchCase::IgnoreCase))
					{
						OutAttribute = Wheel == EStageGoboWheel::Rotating ? StageCraftTags::Attribute_Gobo : StageCraftTags::Attribute_Gobo2;
						OutSlot = Slot;
						return true;
					}
				}
			}
			return false;
		}

		/**
		 * The parameter writes one setting asks of one fixture; empty when the fixture lacks the control. Every write is an
		 * ordinary RequestParameterChange, so the rules (locks, entitlements) judge each one as they judge an inspector edit.
		 */
		TArray<TPair<FGameplayTag, FStageParameterValue>> GetSettingWrites(const ALightingFixtureActor& Fixture, const ULightingFixtureData& Data,
			const FStageAIFixtureSetting& Setting, int32 GelIndex)
		{
			TArray<TPair<FGameplayTag, FStageParameterValue>> Writes;
			const auto Float = [&Writes, &Data](const FGameplayTag& Attribute, double Value)
			{
				if (Data.SupportsAttribute(Attribute))
				{
					Writes.Emplace(Attribute, FStageParameterValue::MakeFloat(Value));
				}
			};
			const auto Slot = [&Writes, &Data](const FGameplayTag& Attribute, int32 Value)
			{
				if (Data.SupportsAttribute(Attribute))
				{
					Writes.Emplace(Attribute, FStageParameterValue::MakeEnum(Value));
				}
			};
			const double Value = Setting.Value.Get(0.0);
			switch (Setting.Control)
			{
			case EStageAIFixtureControl::Dimmer:	Float(StageCraftTags::Attribute_Dimmer, Value); break;
			case EStageAIFixtureControl::Frost:		Float(StageCraftTags::Attribute_Frost, Value); break;
			case EStageAIFixtureControl::Iris:		Float(StageCraftTags::Attribute_Iris, Value); break;
			case EStageAIFixtureControl::CTO:		Float(StageCraftTags::Attribute_CTO, Value); break;
			case EStageAIFixtureControl::GoboSpin:	Float(StageCraftTags::Attribute_GoboSpin, Value); break;
			case EStageAIFixtureControl::PrismSpin:	Float(StageCraftTags::Attribute_PrismRotation, Value); break;
			case EStageAIFixtureControl::Pan:		Float(StageCraftTags::Attribute_Pan, Value); break;
			case EStageAIFixtureControl::Tilt:		Float(StageCraftTags::Attribute_Tilt, Value); break;
			case EStageAIFixtureControl::Prism:		Slot(StageCraftTags::Attribute_Prism, FMath::Min(FMath::RoundToInt32(Value), Data.Prisms.Num())); break;
			case EStageAIFixtureControl::Zoom:
				Float(StageCraftTags::Attribute_Zoom, Setting.Value.IsSet() ? Value : Setting.Name == TEXT("narrow") ? Data.BeamAngleMin : Data.BeamAngleMax);
				break;
			case EStageAIFixtureControl::Strobe:
				// The shutter attribute is the position in the fixture's strobe range (ALightingFixtureActor::GetStrobeRateHz); 0 is open.
				if (Data.bHasShutter)
				{
					const double Range = Data.StrobeRateHz.Y - Data.StrobeRateHz.X;
					const double Shutter = Value <= 0.0 ? 0.0 : Range > 0.0 ? FMath::Clamp((Value - Data.StrobeRateHz.X) / Range, 0.001, 1.0) : 1.0;
					Writes.Emplace(StageCraftTags::Attribute_Shutter, FStageParameterValue::MakeFloat(Shutter));
				}
				break;
			case EStageAIFixtureControl::Gobo:
				if (Setting.Value.IsSet())
				{
					const FGameplayTag Wheel = Data.GoboWheel.IsEmpty() ? StageCraftTags::Attribute_Gobo2 : StageCraftTags::Attribute_Gobo;
					const int32 Slots = Data.GetGoboSlotCount(Wheel == StageCraftTags::Attribute_Gobo ? EStageGoboWheel::Rotating : EStageGoboWheel::Static) - 1;
					Slot(Wheel, FMath::Min(FMath::RoundToInt32(Value), Slots));
				}
				else
				{
					FGameplayTag Wheel;
					int32 Found = 0;
					if (FindGoboSlot(Data, Setting.Name, Wheel, Found))
					{
						Slot(Wheel, Found);
					}
				}
				break;
			case EStageAIFixtureControl::Color:
				for (const TPair<FGameplayTag, float>& Pair : Fixture.MatchColor(Setting.Color))
				{
					Writes.Emplace(Pair.Key, FStageParameterValue::MakeFloat(Pair.Value));
				}
				break;
			case EStageAIFixtureControl::Gel:
				if (GelIndex != INDEX_NONE && (Data.AcceptsGelPreset() || Data.HasCellColor()))
				{
					Writes.Emplace(StageCraftTags::Param_Fixture_Gel, FStageParameterValue::MakeEnum(GelIndex + 1));
				}
				break;
			default:
				break;
			}
			return Writes;
		}

		FStageAIActionOutcome SetFixtures(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			TArray<ALightingFixtureActor*> Fixtures;
			for (AModularBaseActor* Target : FindTargets(Controller, Command, Error))
			{
				if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Target); Fixture && Fixture->GetFixtureData())
				{
					Fixtures.Add(Fixture);
				}
			}
			if (Fixtures.IsEmpty())
			{
				return Refused(!Error.IsEmpty() ? Error : Pick(Language, LOCTEXT("NoFixturesTargeted", "None of those is a lighting fixture."),
					INVTEXT("Դրանցից ոչ մեկը լուսային սարք չէ։")));
			}

			// A gel is looked up once; one the library does not have is an answer, not a silent skip.
			int32 GelIndex = INDEX_NONE;
			if (const FStageAIFixtureSetting* Gel = Command.FixtureSettings.FindByPredicate([](const FStageAIFixtureSetting& Setting) { return Setting.Control == EStageAIFixtureControl::Gel; }))
			{
				GelIndex = FStageGelLibrary::Get().Find(Gel->Name);
				if (GelIndex == INDEX_NONE)
				{
					return Refused(FText::Format(Pick(Language, LOCTEXT("UnknownGel", "I don't know the gel \"{0}\". Try a Lee or Rosco number (\"lee 106\", \"rosco 27\") or the gel's name."),
						INVTEXT("«{0}» ժելը չգիտեմ։ Նշեք Lee կամ Rosco համարը («լի 106», «ռոսկո 27») կամ ժելի անունը։")), FText::FromString(Gel->Name)));
				}
			}

			// One continuous edit, as one inspector drag: the requests are judged one by one but belong together.
			const bool bOwnsTransaction = Controller.BeginEditTransaction();
			TArray<FText> Applied;
			TArray<FText> Missing;
			TArray<FStageEconomyResultInfo> Refusals;
			for (const FStageAIFixtureSetting& Setting : Command.FixtureSettings)
			{
				int32 Changed = 0;
				for (ALightingFixtureActor* Fixture : Fixtures)
				{
					for (const TPair<FGameplayTag, FStageParameterValue>& Write : GetSettingWrites(*Fixture, *Fixture->GetFixtureData(), Setting, GelIndex))
					{
						FStageEconomyResultInfo Result;
						Refusals.Append(Controller.RunCapturingRejections([&]() { Result = Controller.RequestParameterChange(Fixture, Write.Key, Write.Value); }));
						Changed += Result.IsSuccess() ? 1 : 0;
					}
				}
				(Changed > 0 ? Applied : Missing).Add(DescribeControl(Setting, Language));
			}
			if (bOwnsTransaction)
			{
				Controller.EndEditTransaction();
			}

			const FText Separator = LOCTEXT("ControlSeparator", ", ");
			if (Applied.IsEmpty())
			{
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Language, LOCTEXT("NoControl", "{0} cannot do that: {1}."), INVTEXT("{0}-ը դա չի կարող՝ {1}։")),
					DescribeItems(TArray<AModularBaseActor*>(Fixtures)), FText::Join(Separator, Missing))));
			}
			const FText Summary = FText::Format(Pick(Language, LOCTEXT("FixturesSet", "{0}: {1}."), INVTEXT("{0}՝ {1}։")),
				DescribeItems(TArray<AModularBaseActor*>(Fixtures)), FText::Join(Separator, Applied));
			return Done(Missing.IsEmpty() ? Summary : FText::Format(Pick(Language, LOCTEXT("FixturesSetPartly", "{0} Not available on them: {1}."),
				INVTEXT("{0} Դրանք չունեն՝ {1}։")), Summary, FText::Join(Separator, Missing)));
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
		case EStageAIIntent::DeleteItems:	return DeleteItems(Controller, Command);
		case EStageAIIntent::MoveItems:		return MoveItems(Controller, Command);
		case EStageAIIntent::ReplaceItems:	return ReplaceItems(Controller, Command);
		case EStageAIIntent::BuildShape:	return BuildShape(Controller, Command);
		case EStageAIIntent::SetFixture:	return SetFixtures(Controller, Command);
		default:
			return Refused(Pick(Command.Language, LOCTEXT("NotAnAction", "That is not something I can change."), INVTEXT("Դա այն չէ, ինչ կարող եմ փոխել։")));
		}
	}
}

#undef LOCTEXT_NAMESPACE
