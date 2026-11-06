// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandRunner.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "AI/StageAIMarketplaceRunner.h"
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
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

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

		/** Outcome, naming the fixtures it acted on by ID: what "them" means next (Docs/ADR/0030 §6). */
		FStageAIActionOutcome WithFixtures(FStageAIActionOutcome Outcome, TConstArrayView<ALightingFixtureActor*> Fixtures)
		{
			Outcome.AffectedFixtureIds.Reserve(Fixtures.Num());
			for (const ALightingFixtureActor* Fixture : Fixtures)
			{
				if (IsValid(Fixture))
				{
					Outcome.AffectedFixtureIds.AddUnique(Fixture->GetFixtureId());
				}
			}
			return Outcome;
		}

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
				// The fixtures placed are the ones that were not there before: "them" in the next request (Docs/ADR/0030 §6).
				const UStageSessionSubsystem* PendingSession = UWorld::GetSubsystem<UStageSessionSubsystem>(PendingController->GetWorld());
				TSet<const AModularBaseActor*> Before;
				for (const AModularBaseActor* Placed : PendingSession ? PendingSession->GetPlacedItems() : TArray<AModularBaseActor*>())
				{
					Before.Add(Placed);
				}
				FStageSketchGenerateResult Result;
				PendingController->RunCapturingRejections([&]() { Result = PendingController->RequestBuildPlan(Plan, StepName); });
				TArray<ALightingFixtureActor*> Added;
				for (AModularBaseActor* Placed : PendingSession && Result.bSucceeded ? PendingSession->GetPlacedItems() : TArray<AModularBaseActor*>())
				{
					if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Placed); Fixture && !Before.Contains(Placed))
					{
						Added.Add(Fixture);
					}
				}
				PendingAssistant->FinishPendingAction(Result.bSucceeded
					? WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("Added", "Added {0} × {1} in a row on the floor, ready to move into place. Ctrl+Z removes all of them."),
						INVTEXT("Ավելացվեց {0} × {1}՝ մեկ շարքով հատակին, պատրաստ տեղափոխման։ Ctrl+Z-ը կհեռացնի բոլորը։")), FText::AsNumber(Count), Name)), Added)
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

		FStageAIActionOutcome EditCue(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Controller.GetWorld());
			const float Number = *Command.CueNumber;
			const FShowCue* Cue = Show ? Show->FindCue(Number) : nullptr;
			if (!Cue)
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("NoSuchCue", "There is no cue {0}."), INVTEXT("Կյու {0} չկա։")), FormatCue(Number)));
			}
			bool bDone = false;
			const bool bDelete = Command.Intent == EStageAIIntent::DeleteCue;
			// What the update leaves out keeps its value.
			const FText Label = Command.Label.IsEmpty() ? Cue->Label : FText::FromString(Command.Label);
			const float Fade = Command.FadeSeconds.Get(Cue->FadeTime);
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]()
			{
				bDone = bDelete ? Controller.RequestDeleteCue(Number) : Controller.RequestUpdateCue(Number, Label, Fade);
			});
			if (!bDone)
			{
				return Refused(RefusalOf(Refusals, FText::Format(bDelete
					? Pick(Language, LOCTEXT("DeleteCueFailed", "Cue {0} could not be deleted."), INVTEXT("Կյու {0}-ը հնարավոր չեղավ ջնջել։"))
					: Pick(Language, LOCTEXT("UpdateCueFailed", "Cue {0} could not be changed."), INVTEXT("Կյու {0}-ը հնարավոր չեղավ փոխել։")), FormatCue(Number))));
			}
			return Done(bDelete
				? FText::Format(Pick(Language, LOCTEXT("CueDeleted", "Deleted cue {0}. Ctrl+Z brings it back."), INVTEXT("Կյու {0}-ը ջնջվեց։ Ctrl+Z-ը այն հետ կբերի։")), FormatCue(Number))
				: FText::Format(Pick(Language, LOCTEXT("CueUpdated", "Cue {0} is now \"{1}\" with a {2} s fade."), INVTEXT("Կյու {0}-ն այժմ «{1}» է՝ {2} վ անցումով։")),
					FormatCue(Number), Label, FText::AsNumber(Fade)));
		}

		FStageAIActionOutcome PatchFixtures(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			UWorld* World = Controller.GetWorld();
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World);
			const UStageDMXPatchSubsystem* Patch = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(World);
			if (!Show || !Patch)
			{
				return Refused(Pick(Language, LOCTEXT("NoPatch", "The DMX patch is not available in this level."), INVTEXT("DMX պատչը հասանելի չէ այս մակարդակում։")));
			}
			TArray<ALightingFixtureActor*> Fixtures;
			if (Command.FixtureIds.IsEmpty())
			{
				const USelectionComponent* Selection = Controller.GetSelection();
				for (AActor* Selected : Selection ? Selection->GetSelectedActors() : TArray<AActor*>())
				{
					if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Selected))
					{
						Fixtures.AddUnique(Fixture);
					}
				}
				if (Fixtures.IsEmpty())
				{
					return Refused(Pick(Language, LOCTEXT("PatchWhich", "Which fixtures? Select them, or name them: \"patch fixtures 1 to 4 to universe 2\"."),
						INVTEXT("Ո՞ր սարքերը։ Ընտրեք դրանք կամ նշեք՝ «պատչիր 1-ից 4 սարքերը 2 յունիվերսում»։")));
				}
			}
			else
			{
				TArray<FString> Missing;
				for (const int32 Id : Command.FixtureIds)
				{
					if (ALightingFixtureActor* Fixture = Show->FindFixture(Id))
					{
						Fixtures.AddUnique(Fixture);
					}
					else
					{
						Missing.Add(FString::FromInt(Id));
					}
				}
				if (!Missing.IsEmpty())
				{
					return Refused(FText::Format(Pick(Language, LOCTEXT("PatchMissing", "There is no fixture {0} on the stage."), INVTEXT("Բեմում {0} սարք չկա։")),
						FText::FromString(FString::Join(Missing, TEXT(", ")))));
				}
			}

			FStageDMXPatch Start;
			Start.Universe = Command.Universe.Get(1);
			Start.Address = Command.Address.Get(1);
			const TArray<FStageDMXPatchAssignment> Plan = Patch->PlanConsecutive(Fixtures, Start);
			const FText StartText = FText::FromString(FString::Printf(TEXT("%d.%03d"), Start.Universe, Start.Address));
			if (Plan.IsEmpty())
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("PatchNoRoom", "Those fixtures do not fit from {0}: there is no free room for all of them."),
					INVTEXT("Այդ սարքերը {0}-ից չեն տեղավորվում. բոլորի համար ազատ տեղ չկա։")), StartText));
			}
			// Already there is what was asked for, not a refusal (a plan carries on).
			if (Algo::AllOf(Plan, [](const FStageDMXPatchAssignment& Entry) { return Entry.Fixture.IsValid() && Entry.Fixture->GetPatch() == Entry.Patch; }))
			{
				return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("PatchAlready", "Those fixtures are already patched from {0}; nothing changed."),
					INVTEXT("Այդ սարքերն արդեն պատչված են {0}-ից. ոչինչ չփոխվեց։")), StartText)), Fixtures);
			}
			FStageEconomyResultInfo Result;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]()
			{
				Result = Controller.RequestPatchFixtures(Plan, FText::GetEmpty());
			});
			if (!Result.IsSuccess())
			{
				return Refused(RefusalOf(Refusals, Result.Message));
			}
			const FStageDMXPatch& Last = Plan.Last().Patch;
			return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("Patched", "Patched {0} {0}|plural(one=fixture,other=fixtures) from {1} to {2}. Ctrl+Z restores the old patch."),
				INVTEXT("Պատչվեց {0} սարք՝ {1}-ից մինչև {2}։ Ctrl+Z-ը կվերադարձնի նախորդ պատչը։")), FText::AsNumber(Plan.Num()),
				FText::FromString(FString::Printf(TEXT("%d.%03d"), Plan[0].Patch.Universe, Plan[0].Patch.Address)),
				FText::FromString(FString::Printf(TEXT("%d.%03d"), Last.Universe, Last.Address)))), Fixtures);
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
			const bool bSkyWasOn = Environment->GetSettings().IsOutdoorSky();
			FStageEnvironmentSettings Settings = Environment->GetSettings();
			FText StepName;
			switch (Command.Intent)
			{
			case EStageAIIntent::SetWeather:
				Settings = StageEnvironment::ApplyWeatherPreset(Settings, Command.Weather);
				// Weather is only visible under the outdoor sky.
				if (bSkyAvailable)
				{
					Settings.Lighting = EStageLightingSource::OutdoorSky;
				}
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
				Settings.Lighting = Command.bSkyOn ? EStageLightingSource::OutdoorSky : EStageLightingSource::Level;
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
			const FText SkyNote = !Stored.IsOutdoorSky()
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
				return Done(Stored.IsOutdoorSky()
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

		/** "A, B, C" for the first few names, "..." after them (what a pool holds, for a name that matched nothing). */
		FText DescribeNames(TConstArrayView<FString> Names)
		{
			constexpr int32 MaxListed = 10;
			TArray<FString> Listed(Names.GetData(), FMath::Min(Names.Num(), MaxListed));
			if (Names.Num() > MaxListed)
			{
				Listed.Add(TEXT("..."));
			}
			return FText::FromString(FString::Join(Listed, TEXT(", ")));
		}

		/**
		 * The entry of Names that Query means (StageAICommandParser::ResolveName), or INDEX_NONE with OutError saying the pool has none
		 * ("There is no palette \"warm\". The palettes are: ...") or several.
		 */
		int32 ResolvePoolName(TConstArrayView<FString> Names, const FString& Query, const FText& PoolName, EStageAILanguage Language, FText& OutError)
		{
			const StageAICommandParser::FStageAINameMatch Match = StageAICommandParser::ResolveName(Names, Query);
			if (Match.Index != INDEX_NONE)
			{
				return Match.Index;
			}
			if (Names.IsEmpty())
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("PoolEmpty", "There are no {0} yet."), INVTEXT("Դեռ {0} չկան։")), PoolName);
			}
			else if (!Match.Ambiguous.IsEmpty())
			{
				TArray<FString> Several;
				for (const int32 Index : Match.Ambiguous)
				{
					Several.Add(Names[Index]);
				}
				OutError = FText::Format(Pick(Language, LOCTEXT("PoolSeveral", "\"{0}\" matches several {1}: {2}. Say which."), INVTEXT("«{0}»-ին համապատասխանում են մի քանի {1}՝ {2}։ Նշեք որը։")),
					FText::FromString(Query), PoolName, DescribeNames(Several));
			}
			else
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("PoolNone", "There is no \"{0}\" among the {1}: {2}."), INVTEXT("{1}ի մեջ «{0}» չկա՝ {2}։")),
					FText::FromString(Query), PoolName, DescribeNames(Names));
			}
			return INDEX_NONE;
		}

		/**
		 * The fixtures a Fixtures or Group target names, in its order (IDs not on the stage are skipped). Empty, with OutError, when the
		 * group is unknown or none of its fixtures is on the stage.
		 */
		TArray<AModularBaseActor*> FindFixturesById(const AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			const EStageAILanguage Language = Command.Language;
			UWorld* World = Controller.GetWorld();
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World);
			TArray<int32> Ids = Command.FixtureIds;
			if (Command.Target == EStageAITarget::Group)
			{
				const UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(World);
				const TArray<FStageFixtureGroup> NoGroups;
				const TArray<FStageFixtureGroup>& Groups = Library ? Library->GetGroups() : NoGroups;
				TArray<FString> Names;
				for (const FStageFixtureGroup& Group : Groups)
				{
					Names.Add(Group.Name.ToString());
				}
				const int32 Found = ResolvePoolName(Names, Command.Group, Pick(Language, LOCTEXT("PoolGroups", "groups"), INVTEXT("խմբեր")), Language, OutError);
				if (Found == INDEX_NONE)
				{
					return {};
				}
				Ids = Groups[Found].FixtureIds;
			}
			TArray<AModularBaseActor*> Fixtures;
			for (const int32 Id : Ids)
			{
				if (ALightingFixtureActor* Fixture = Show ? Show->FindFixture(Id) : nullptr; IsValid(Fixture))
				{
					Fixtures.AddUnique(Fixture);
				}
			}
			if (Fixtures.IsEmpty())
			{
				OutError = Command.Target == EStageAITarget::Group
					? FText::Format(Pick(Language, LOCTEXT("GroupEmpty", "None of the fixtures of group \"{0}\" is on the stage."), INVTEXT("«{0}» խմբի սարքերից ոչ մեկը բեմում չէ։")),
						FText::FromString(Command.Group))
					: Pick(Language, LOCTEXT("NoSuchFixtures", "None of those fixture IDs is on the stage. Say \"status\" to see what is."),
						INVTEXT("Այդ համարներով սարքեր բեմում չկան։ Գրեք «վիճակը»՝ տեսնելու, թե ինչ կա։"));
			}
			else if (Fixtures.Num() > StageAICommandParser::MaxEditTargets)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("TooManyFixtures", "That is {0} fixtures; I change at most {1} at a time."),
					INVTEXT("Դա {0} սարք է, իսկ ես միանգամից փոխում եմ առավելագույնը {1}։")), FText::AsNumber(Fixtures.Num()), FText::AsNumber(StageAICommandParser::MaxEditTargets));
				Fixtures.Reset();
			}
			return Fixtures;
		}

		/**
		 * The items a targeted edit applies to: the selection (narrowed by ItemQuery when it has one), or the placed items matching
		 * ItemQuery, the latest placed first, at most Count; for the fixture tools also fixtures by ID or a group. Empty, with OutError in
		 * the command's language, when none.
		 */
		TArray<AModularBaseActor*> FindTargets(const AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			const EStageAILanguage Language = Command.Language;
			if (Command.Target == EStageAITarget::Fixtures || Command.Target == EStageAITarget::Group)
			{
				return FindFixturesById(Controller, Command, OutError);
			}
			const auto Matches = [&Command](const AModularBaseActor& Item)
			{
				const UBaseItemData* Data = Item.GetItemData();
				return Command.ItemQuery.IsEmpty()
					|| (Data && StageAICommandParser::MatchesItemName(Data->DisplayName.ToString(), Command.ItemQuery, Item.IsA<ALightingFixtureActor>()));
			};

			TArray<AModularBaseActor*> Targets;
			// "Them" is resolved into fixture IDs before a command runs; one that was not falls back to the selection.
			if (Command.Target == EStageAITarget::Selection || Command.Target == EStageAITarget::Recent)
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
			return WithFixtures(Done(Missing.IsEmpty() ? Summary : FText::Format(Pick(Language, LOCTEXT("FixturesSetPartly", "{0} Not available on them: {1}."),
				INVTEXT("{0} Դրանք չունեն՝ {1}։")), Summary, FText::Join(Separator, Missing))), Fixtures);
		}

		// --- Selection, palettes, groups and effects (Docs/ADR/0030 §6): the requests of the Groups, Palettes and Effects panels ---

		/**
		 * The lighting fixtures of a fixture tool's target. Fixtures named by words come in fixture-ID order (an effect runs its pattern
		 * in that order); the selection and IDs keep theirs.
		 */
		TArray<ALightingFixtureActor*> FindFixtureTargets(const AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			TArray<ALightingFixtureActor*> Fixtures;
			for (AModularBaseActor* Target : FindTargets(Controller, Command, OutError))
			{
				if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Target); Fixture && Fixture->GetFixtureData())
				{
					Fixtures.Add(Fixture);
				}
			}
			if (Command.Target == EStageAITarget::Named)
			{
				Fixtures.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B) { return A.GetFixtureId() < B.GetFixtureId(); });
			}
			if (Fixtures.IsEmpty() && OutError.IsEmpty())
			{
				OutError = Pick(Command.Language, LOCTEXT("ToolNoFixtures", "None of those is a lighting fixture."), INVTEXT("Դրանցից ոչ մեկը լուսային սարք չէ։"));
			}
			return Fixtures;
		}

		/**
		 * Makes Fixtures the player's selection, for a request that acts on the selection (palettes, groups, effects), unless they already
		 * are. A selection is tool state, not an undo step. False, with the reason, when it was refused (a drag, Place mode guards).
		 */
		bool SelectForTool(AModularPlayerController& Controller, const TArray<ALightingFixtureActor*>& Fixtures, EStageAILanguage Language, FText& OutError)
		{
			const auto IsSelected = [&Controller, &Fixtures]()
			{
				const USelectionComponent* Selection = Controller.GetSelection();
				const TArray<AActor*> Selected = Selection ? Selection->GetSelectedActors() : TArray<AActor*>();
				return Selected.Num() == Fixtures.Num() && Algo::AllOf(Fixtures, [&Selected](ALightingFixtureActor* Fixture) { return Selected.Contains(Fixture); });
			};
			if (IsSelected())
			{
				return true;
			}
			const TArray<AModularBaseActor*> Items(Fixtures);
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Controller.RequestSelectItems(Items, EStageSelectionClick::Replace); });
			if (IsSelected())
			{
				return true;
			}
			OutError = RefusalOf(Refusals, Pick(Language, LOCTEXT("SelectRefused", "I could not select them right now."), INVTEXT("Այս պահին չկարողացա ընտրել դրանք։")));
			return false;
		}

		/** The fixtures of Command's target, selected; empty with OutError when there are none or the selection was refused. */
		TArray<ALightingFixtureActor*> SelectToolTargets(AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			TArray<ALightingFixtureActor*> Fixtures = FindFixtureTargets(Controller, Command, OutError);
			if (!Fixtures.IsEmpty() && !SelectForTool(Controller, Fixtures, Command.Language, OutError))
			{
				Fixtures.Reset();
			}
			return Fixtures;
		}

		FStageAIActionOutcome SelectFixtures(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			FText Error;
			const TArray<ALightingFixtureActor*> Fixtures = SelectToolTargets(Controller, Command, Error);
			if (Fixtures.IsEmpty())
			{
				return Refused(Error);
			}
			return WithFixtures(Done(FText::Format(Pick(Command.Language, LOCTEXT("Selected", "Selected {0}."), INVTEXT("Ընտրվեց {0}։")),
				DescribeItems(TArray<AModularBaseActor*>(Fixtures)))), Fixtures);
		}

		const UStageShowLibrarySubsystem* GetLibrary(const AModularPlayerController& Controller)
		{
			return UWorld::GetSubsystem<UStageShowLibrarySubsystem>(Controller.GetWorld());
		}

		/** The palette Command names (by name or pool number), or null with OutError. */
		const FStagePalette* FindPalette(const AModularPlayerController& Controller, const FStageAICommand& Command, FText& OutError)
		{
			const UStageShowLibrarySubsystem* Library = GetLibrary(Controller);
			if (!Library)
			{
				OutError = Pick(Command.Language, LOCTEXT("NoLibrary", "Palettes and groups are not available in this level."), INVTEXT("Պալիտրաներն ու խմբերը հասանելի չեն այս մակարդակում։"));
				return nullptr;
			}
			TArray<FString> Names;
			for (const FStagePalette& Palette : Library->GetPalettes())
			{
				Names.Add(Palette.Name.ToString());
			}
			const int32 Found = ResolvePoolName(Names, Command.Palette, Pick(Command.Language, LOCTEXT("PoolPalettes", "palettes"), INVTEXT("պալիտրաներ")), Command.Language, OutError);
			return Found == INDEX_NONE ? nullptr : &Library->GetPalettes()[Found];
		}

		FStageAIActionOutcome ApplyPalette(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			const FStagePalette* Palette = FindPalette(Controller, Command, Error);
			if (!Palette)
			{
				return Refused(Error);
			}
			const FGuid PaletteId = Palette->Id;
			const FText PaletteName = Palette->Name;
			const TArray<ALightingFixtureActor*> Fixtures = SelectToolTargets(Controller, Command, Error);
			if (Fixtures.IsEmpty())
			{
				return Refused(Error);
			}
			FStageEconomyResultInfo Result;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { Result = Controller.RequestApplyPalette(PaletteId); });
			if (!Result.IsSuccess())
			{
				return Refused(RefusalOf(Refusals, !Result.Message.IsEmpty() ? Result.Message : FText::Format(Pick(Language,
					LOCTEXT("PaletteNotApplied", "Palette \"{0}\" has nothing for those fixtures."), INVTEXT("«{0}» պալիտրան այդ սարքերի համար ոչինչ չունի։")), PaletteName)));
			}
			return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("PaletteApplied", "{0}: palette \"{1}\" (they follow it until a plain edit or Clear)."),
				INVTEXT("{0}՝ «{1}» պալիտրա (կհետևեն դրան մինչև պարզ փոփոխություն կամ Clear)։")), DescribeItems(TArray<AModularBaseActor*>(Fixtures)), PaletteName)), Fixtures);
		}

		FStageAIActionOutcome RecordPalette(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			const TArray<ALightingFixtureActor*> Fixtures = SelectToolTargets(Controller, Command, Error);
			if (Fixtures.IsEmpty())
			{
				return Refused(Error);
			}
			FGuid PaletteId;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]()
			{
				PaletteId = Controller.RequestRecordPalette(Command.PaletteKind, FText::FromString(Command.Label), EStagePaletteScope::Global);
			});
			const UStageShowLibrarySubsystem* Library = GetLibrary(Controller);
			const FStagePalette* Palette = Library && PaletteId.IsValid() ? Library->FindPalette(PaletteId) : nullptr;
			if (!Palette)
			{
				return Refused(RefusalOf(Refusals, Pick(Language, LOCTEXT("PaletteNotRecorded", "Nothing was recorded: those fixtures show nothing of that kind."),
					INVTEXT("Ոչինչ չգրանցվեց. այդ սարքերը այդ տեսակից ոչինչ ցույց չեն տալիս։"))));
			}
			return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("PaletteRecorded", "Recorded palette \"{0}\" from {1}."), INVTEXT("Գրանցվեց «{0}» պալիտրան {1}-ից։")),
				Palette->Name, DescribeItems(TArray<AModularBaseActor*>(Fixtures)))), Fixtures);
		}

		FStageAIActionOutcome DeletePalette(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			FText Error;
			const FStagePalette* Palette = FindPalette(Controller, Command, Error);
			if (!Palette)
			{
				return Refused(Error);
			}
			const FGuid PaletteId = Palette->Id;
			const FText PaletteName = Palette->Name;
			bool bDeleted = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bDeleted = Controller.RequestDeletePalette(PaletteId); });
			if (!bDeleted)
			{
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Command.Language, LOCTEXT("PaletteNotDeleted", "Palette \"{0}\" could not be deleted."),
					INVTEXT("«{0}» պալիտրան հնարավոր չեղավ ջնջել։")), PaletteName)));
			}
			return Done(FText::Format(Pick(Command.Language, LOCTEXT("PaletteDeleted", "Deleted palette \"{0}\"; cues that used it keep its values."),
				INVTEXT("Ջնջվեց «{0}» պալիտրան. այն օգտագործող կյուները պահում են դրա արժեքները։")), PaletteName));
		}

		FStageAIActionOutcome RecordGroup(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			FText Error;
			const TArray<ALightingFixtureActor*> Fixtures = SelectToolTargets(Controller, Command, Error);
			if (Fixtures.IsEmpty())
			{
				return Refused(Error);
			}
			FGuid GroupId;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { GroupId = Controller.RequestRecordGroup(FText::FromString(Command.Label)); });
			const UStageShowLibrarySubsystem* Library = GetLibrary(Controller);
			const FStageFixtureGroup* Group = Library && GroupId.IsValid() ? Library->FindGroup(GroupId) : nullptr;
			if (!Group)
			{
				return Refused(RefusalOf(Refusals, Pick(Language, LOCTEXT("GroupNotRecorded", "The group could not be recorded."), INVTEXT("Խումբը հնարավոր չեղավ գրանցել։"))));
			}
			return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("GroupRecorded", "Recorded group \"{0}\" with {1}."), INVTEXT("Գրանցվեց «{0}» խումբը՝ {1}։")),
				Group->Name, DescribeItems(TArray<AModularBaseActor*>(Fixtures)))), Fixtures);
		}

		FStageAIActionOutcome DeleteGroup(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UStageShowLibrarySubsystem* Library = GetLibrary(Controller);
			if (!Library)
			{
				return Refused(Pick(Language, LOCTEXT("NoLibraryGroups", "Groups are not available in this level."), INVTEXT("Խմբերը հասանելի չեն այս մակարդակում։")));
			}
			TArray<FString> Names;
			for (const FStageFixtureGroup& Group : Library->GetGroups())
			{
				Names.Add(Group.Name.ToString());
			}
			FText Error;
			const int32 Found = ResolvePoolName(Names, Command.Group, Pick(Language, LOCTEXT("PoolGroupsDelete", "groups"), INVTEXT("խմբեր")), Language, Error);
			if (Found == INDEX_NONE)
			{
				return Refused(Error);
			}
			const FGuid GroupId = Library->GetGroups()[Found].Id;
			const FText GroupName = Library->GetGroups()[Found].Name;
			bool bDeleted = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bDeleted = Controller.RequestDeleteGroup(GroupId); });
			if (!bDeleted)
			{
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Language, LOCTEXT("GroupNotDeleted", "Group \"{0}\" could not be deleted."), INVTEXT("«{0}» խումբը հնարավոր չեղավ ջնջել։")),
					GroupName)));
			}
			return Done(FText::Format(Pick(Language, LOCTEXT("GroupDeleted", "Deleted group \"{0}\"; its fixtures stay on the stage."), INVTEXT("Ջնջվեց «{0}» խումբը. դրա սարքերը մնում են բեմում։")),
				GroupName));
		}

		/** Command's rate and size on Effect: a position effect's size is degrees, any other's is the wave's height above its bottom. */
		void ApplyEffectSettings(const FStageAICommand& Command, FStageEffect& Effect)
		{
			if (Command.RateHz.IsSet())
			{
				Effect.RateHz = *Command.RateHz;
			}
			if (Command.EffectSize.IsSet())
			{
				if (Effect.Target == EStageEffectTarget::Position)
				{
					Effect.Size = *Command.EffectSize;
				}
				else
				{
					Effect.High = FMath::Clamp(Effect.Low + FMath::Min(*Command.EffectSize, 1.f), 0.f, 1.f);
				}
			}
		}

		/** The names of the effects TConstArrayView holds. */
		TArray<FString> EffectNames(TConstArrayView<FStageEffect> Effects)
		{
			TArray<FString> Names;
			for (const FStageEffect& Effect : Effects)
			{
				Names.Add(Effect.Name.ToString());
			}
			return Names;
		}

		/** Whether a running effect named Name is one Query names (empty: every effect). */
		bool EffectMatches(const FText& Name, const FString& Query)
		{
			const FString NameText = Name.ToString();
			return Query.IsEmpty() || StageAICommandParser::ResolveName(MakeArrayView(&NameText, 1), Query).Index == 0;
		}

		FStageAIActionOutcome StartEffect(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			TArray<FStageEffect> Effects = StageEffectRules::MakeQuickEffects();
			Effects.Append(StageEffectRules::MakeAudioQuickEffects());
			const TArray<FString> Names = EffectNames(Effects);
			// "Chase" is the plain Dimmer Chase before the Beat Chase: the first of several wins, as on the panel.
			const StageAICommandParser::FStageAINameMatch Match = StageAICommandParser::ResolveName(Names, Command.Effect, /*bFirstOfSeveral*/ true);
			if (Match.Index == INDEX_NONE)
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("UnknownEffect", "I don't know the effect \"{0}\". I can start: {1}."),
					INVTEXT("«{0}» էֆեկտը չգիտեմ։ Կարող եմ սկսել՝ {1}։")), FText::FromString(Command.Effect), FText::FromString(FString::Join(Names, TEXT(", ")))));
			}
			FStageEffect Effect = Effects[Match.Index];
			ApplyEffectSettings(Command, Effect);
			FText Error;
			const TArray<ALightingFixtureActor*> Fixtures = SelectToolTargets(Controller, Command, Error);
			if (Fixtures.IsEmpty())
			{
				return Refused(Error);
			}
			FGuid EffectId;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { EffectId = Controller.RequestStartEffect(Effect); });
			if (!EffectId.IsValid())
			{
				return Refused(RefusalOf(Refusals, FText::Format(Pick(Language, LOCTEXT("EffectNotStarted", "\"{0}\" could not start on them."),
					INVTEXT("«{0}»-ը չկարողացավ սկսել դրանց վրա։")), Effect.Name)));
			}
			return WithFixtures(Done(FText::Format(Pick(Language, LOCTEXT("EffectStarted", "{0}: effect \"{1}\" running. Say \"stop effects\" to stop it."),
				INVTEXT("{0}՝ «{1}» էֆեկտը աշխատում է։ Կանգնեցնելու համար գրեք «կանգնեցրու էֆեկտները»։")), DescribeItems(TArray<AModularBaseActor*>(Fixtures)), Effect.Name)), Fixtures);
		}

		FStageAIActionOutcome UpdateEffect(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(Controller.GetWorld());
			// A copy: each update replaces the programmer's list.
			const TArray<FStageEffectEntry> Running = Programmer ? Programmer->GetEffects() : TArray<FStageEffectEntry>();
			TArray<int32> FixtureIds;
			int32 Changed = 0;
			bool bAnyNamed = false;
			for (const FStageEffectEntry& Entry : Running)
			{
				if (!EffectMatches(Entry.Effect.Name, Command.Effect))
				{
					continue;
				}
				bAnyNamed = true;
				FStageEffect Effect = Entry.Effect;
				ApplyEffectSettings(Command, Effect);
				bool bUpdated = false;
				Controller.RunCapturingRejections([&]() { bUpdated = Controller.RequestUpdateEffect(Entry.Id, Effect); });
				if (bUpdated)
				{
					++Changed;
					for (const int32 Id : Entry.FixtureIds)
					{
						FixtureIds.AddUnique(Id);
					}
				}
			}
			if (!bAnyNamed)
			{
				return Refused(Command.Effect.IsEmpty()
					? Pick(Language, LOCTEXT("NoProgrammerEffect", "No effect of the programmer is running. One a cue runs is changed after EDIT in the Effects panel."),
						INVTEXT("Ծրագրավորողի ոչ մի էֆեկտ չի աշխատում։ Կյուի էֆեկտը փոխվում է Effects վահանակի EDIT-ից հետո։"))
					: FText::Format(Pick(Language, LOCTEXT("NoNamedEffect", "No running effect of the programmer is called \"{0}\"."),
						INVTEXT("Ծրագրավորողի աշխատող էֆեկտներից ոչ մեկը «{0}» չի կոչվում։")), FText::FromString(Command.Effect)));
			}
			if (Changed == 0)
			{
				return Refused(Pick(Language, LOCTEXT("EffectsNotChanged", "The effects could not be changed right now."), INVTEXT("Էֆեկտներն այս պահին հնարավոր չեղավ փոխել։")));
			}
			FStageAIActionOutcome Outcome = Done(FText::Format(Pick(Language, LOCTEXT("EffectsChanged", "Changed {0} {0}|plural(one=effect,other=effects)."),
				INVTEXT("Փոխվեց {0} էֆեկտ։")), FText::AsNumber(Changed)));
			Outcome.AffectedFixtureIds = MoveTemp(FixtureIds);
			return Outcome;
		}

		FStageAIActionOutcome StopEffect(AModularPlayerController& Controller, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Controller.GetWorld());
			TArray<FStageRunningEffect> Running;
			if (Show)
			{
				Show->GetRunningEffects(Running);
			}
			Running.RemoveAll([&Command](const FStageRunningEffect& Effect) { return Effect.bStopping || !EffectMatches(Effect.Entry.Effect.Name, Command.Effect); });
			if (Running.IsEmpty())
			{
				return Refused(Command.Effect.IsEmpty()
					? Pick(Language, LOCTEXT("NoEffects", "No effect is running."), INVTEXT("Ոչ մի էֆեկտ չի աշխատում։"))
					: FText::Format(Pick(Language, LOCTEXT("NoRunningNamed", "No running effect is called \"{0}\"."), INVTEXT("Աշխատող էֆեկտներից ոչ մեկը «{0}» չի կոչվում։")),
						FText::FromString(Command.Effect)));
			}
			int32 Stopped = 0;
			TArray<FStageEconomyResultInfo> Refusals;
			if (Command.Effect.IsEmpty())
			{
				// STOP ALL: one undo step, as the panel's button.
				bool bStopped = false;
				Refusals = Controller.RunCapturingRejections([&]() { bStopped = Controller.RequestStopEffect(FGuid()); });
				Stopped = bStopped ? Running.Num() : 0;
			}
			else
			{
				for (const FStageRunningEffect& Effect : Running)
				{
					bool bStopped = false;
					Refusals.Append(Controller.RunCapturingRejections([&]() { bStopped = Controller.RequestStopEffect(Effect.Entry.Id); }));
					Stopped += bStopped ? 1 : 0;
				}
			}
			if (Stopped == 0)
			{
				return Refused(RefusalOf(Refusals, Pick(Language, LOCTEXT("EffectsNotStopped", "The effects could not be stopped right now."), INVTEXT("Էֆեկտներն այս պահին հնարավոր չեղավ կանգնեցնել։"))));
			}
			return Done(FText::Format(Pick(Language, LOCTEXT("EffectsStopped", "Stopped {0} {0}|plural(one=effect,other=effects). Ctrl+Z starts them again."),
				INVTEXT("Կանգնեցվեց {0} էֆեկտ։ Ctrl+Z-ը դրանք նորից կմիացնի։")), FText::AsNumber(Stopped)));
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
		case EStageAIIntent::UpdateCue:
		case EStageAIIntent::DeleteCue:		return EditCue(Controller, Command);
		case EStageAIIntent::PatchFixtures:	return PatchFixtures(Controller, Command);
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
		case EStageAIIntent::SelectFixtures:	return SelectFixtures(Controller, Command);
		case EStageAIIntent::ApplyPalette:	return ApplyPalette(Controller, Command);
		case EStageAIIntent::RecordPalette:	return RecordPalette(Controller, Command);
		case EStageAIIntent::DeletePalette:	return DeletePalette(Controller, Command);
		case EStageAIIntent::RecordGroup:	return RecordGroup(Controller, Command);
		case EStageAIIntent::DeleteGroup:	return DeleteGroup(Controller, Command);
		case EStageAIIntent::StartEffect:	return StartEffect(Controller, Command);
		case EStageAIIntent::UpdateEffect:	return UpdateEffect(Controller, Command);
		case EStageAIIntent::StopEffect:	return StopEffect(Controller, Command);
		case EStageAIIntent::SearchMarketplace:
		case EStageAIIntent::PreviewListing:
		case EStageAIIntent::AcquireListing:	return StageAIMarketplaceRunner::Run(Controller, Command);
		default:
			return Refused(Pick(Command.Language, LOCTEXT("NotAnAction", "That is not something I can change."), INVTEXT("Դա այն չէ, ինչ կարող եմ փոխել։")));
		}
	}
}

#undef LOCTEXT_NAMESPACE
