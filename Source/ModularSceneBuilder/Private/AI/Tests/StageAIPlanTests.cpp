// Copyright Epic Games, Inc. All Rights Reserved.

// Phase H3 (Docs/ADR/0030 §6, §16): the tool catalogue, the grammar of the new tools, history groups, plans as one undo step with
// rollback, proposal cards, and several tool calls of the online model becoming one plan. No network: the online model is a
// scripted backend (UStageAITestBackend).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Assistant; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAICommandSubsystem.h"
#include "AI/StageAIToolCatalogue.h"
#include "AI/Tests/StageAITestBackend.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageEditHistorySubsystem.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace StageAIPlanTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** History steps here change a plain counter; the editor itself is never asked for anything. */
	class FNullItemEditor final : public IStageItemEditor
	{
	public:
		virtual bool CaptureItem(const FGuid&, FStageItemSnapshot&) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid&, const FTransform&) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid&, const FGameplayTag&, const FStageParameterValue&) override { return EStageCommandResult::Invalid; }
	};

	/** Adds Delta to a counter that stands for the stage; undo takes it away again. Can refuse its undo. */
	class FCounterCommand final : public IStageEditCommand
	{
	public:
		FCounterCommand(int32& InStage, int32 InDelta, FString InName, bool bInRefuseUndo = false)
			: Stage(InStage), Delta(InDelta), Name(MoveTemp(InName)), bRefuseUndo(bInRefuseUndo) {}

		virtual FText GetDescription() const override { return FText::FromString(Name); }
		virtual EStageCommandResult Undo(IStageItemEditor&) override
		{
			if (bRefuseUndo)
			{
				return EStageCommandResult::Refused;
			}
			Stage -= Delta;
			return EStageCommandResult::Succeeded;
		}
		virtual EStageCommandResult Redo(IStageItemEditor&) override
		{
			Stage += Delta;
			return EStageCommandResult::Succeeded;
		}

	private:
		int32& Stage;
		int32 Delta;
		FString Name;
		bool bRefuseUndo;
	};

	/** A game world (subsystems initialized) torn down with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAIPlanTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};

	/**
	 * The asking player as the tests play it: each command it runs records one counter step in the real history subsystem (a GO
	 * records nothing, like cue playback), and plans open, keep and roll back a real history group.
	 */
	struct FFakePlayer
	{
		UStageEditHistorySubsystem* History = nullptr;
		FNullItemEditor Editor;
		int32 Stage = 0;
		TArray<FStageAICommand> Executed;
		/** Intents refused when run. */
		TArray<EStageAIIntent> Refuse;
		/** Intents that answer Pending (finished later by the test, or at once inside Execute when bFinishInside). */
		TArray<EStageAIIntent> Pending;
		bool bFinishInside = false;
		UStageAICommandSubsystem* Assistant = nullptr;
		bool bAsk = false;

		FStageAIActionHost MakeHost()
		{
			FStageAIActionHost Host;
			Host.Execute = FStageAICommandExecutor::CreateLambda([this](const FStageAICommand& Command)
			{
				Executed.Add(Command);
				if (Refuse.Contains(Command.Intent))
				{
					return FStageAIActionOutcome::Make(EStageAIOutcome::Refused, FText::FromString(TEXT("refused by the rules")));
				}
				if (StageAIToolCatalogue::GetTool(Command.Intent).bUndoable)
				{
					History->Record(MakeShared<FCounterCommand>(Stage, 1, StageAICommandSchema::GetIntentName(Command.Intent)));
					Stage += 1;
				}
				if (Pending.Contains(Command.Intent))
				{
					if (bFinishInside)
					{
						Assistant->FinishPendingAction(FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("finished inside"))));
					}
					return FStageAIActionOutcome::Make(EStageAIOutcome::Pending, FText::FromString(TEXT("working")));
				}
				return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(StageAICommandSchema::GetIntentName(Command.Intent) + TEXT(" done")));
			});
			Host.BeginPlan = FStageAIPlanBegin::CreateLambda([this](const FText& Description) { return History->BeginGroup(Description); });
			Host.EndPlan = FStageAIPlanEnd::CreateLambda([this](bool bKeep)
			{
				if (bKeep)
				{
					History->EndGroup();
					return EStageCommandResult::Succeeded;
				}
				return History->AbortGroup(Editor);
			});
			Host.bAskBeforeChanges = bAsk;
			return Host;
		}
	};

	FStageAICommand Parsed(const TCHAR* Prompt)
	{
		return StageAICommandParser::Parse(Prompt).Get(FStageAICommand());
	}

	FStageAIPlan MakePlan(std::initializer_list<const TCHAR*> Prompts)
	{
		FStageAIPlan Plan;
		for (const TCHAR* Prompt : Prompts)
		{
			Plan.Steps.Add(Parsed(Prompt));
		}
		return Plan;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantToolCatalogueTest, "StageCraft.Assistant.ToolCatalogue", StageAIPlanTests::Flags)

bool FStageAssistantToolCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace StageAIToolCatalogue;
	TSet<FString> Names;
	for (int32 Index = 0; Index < int32(EStageAIIntent::Count); ++Index)
	{
		const EStageAIIntent Intent = EStageAIIntent(Index);
		const FStageAITool& Tool = GetTool(Intent);
		const FString Name = Tool.Name;
		TestTrue(FString::Printf(TEXT("Entry %d is its intent"), Index), Tool.Intent == Intent);
		TestFalse(FString::Printf(TEXT("%s is unique"), *Name), Names.Contains(Name));
		Names.Add(Name);
		TestTrue(FString::Printf(TEXT("%s found by name"), *Name), FindTool(Name) == &Tool);
		TestEqual(FString::Printf(TEXT("%s is the schema's wire name"), *Name), StageAICommandSchema::GetIntentName(Intent), Name);

		FStageAICommand Command;
		Command.Intent = Intent;
		TestFalse(FString::Printf(TEXT("%s: a question changes nothing"), *Name), Command.IsQuestion() && Tool.IsChange());
		TestEqual(FString::Printf(TEXT("%s: an action is a change"), *Name), Command.IsAction(), Tool.IsChange());
		TestTrue(FString::Printf(TEXT("%s: undoable and plannable only for changes"), *Name), Tool.IsChange() || (!Tool.bUndoable && !Tool.bPlannable));
		for (const TCHAR* Forbidden : GetNeverExposed())
		{
			const FString Token = FString(Forbidden).Replace(TEXT(" "), TEXT("_"));
			TestFalse(FString::Printf(TEXT("%s is not \"%s\""), *Name, Forbidden), Name.Contains(Token));
		}
		if (Tool.IsChange())
		{
			TestFalse(FString::Printf(TEXT("%s described in English"), *Name), DescribeStep(Command, EStageAILanguage::English).IsEmpty());
			TestFalse(FString::Printf(TEXT("%s described in Armenian"), *Name), DescribeStep(Command, EStageAILanguage::Armenian).IsEmpty());
		}
	}
	TestNull(TEXT("Unknown names are no tool"), FindTool(TEXT("clear_stage")));
	const FString Definition = StageAICommandSchema::MakeToolDefinitionJson();
	TestTrue(TEXT("The online tool offers patch_fixtures with its fields"), Definition.Contains(TEXT("\"patch_fixtures\"")) && Definition.Contains(TEXT("\"fixture_ids\""))
		&& Definition.Contains(TEXT("\"universe\"")) && Definition.Contains(TEXT("\"address\"")) && Definition.Contains(TEXT("\"delete_cue\"")));

	// What may apply without asking (ADR §6): cue playback and controls of the selected fixtures.
	TestTrue(TEXT("GO applies at once"), CanApplyWithoutAsking(StageAIPlanTests::Parsed(TEXT("go"))));
	TestTrue(TEXT("Selected fixtures' dimmer applies at once"), CanApplyWithoutAsking(StageAIPlanTests::Parsed(TEXT("set the selected lights to 50%"))));
	TestFalse(TEXT("Fixtures picked by name ask"), CanApplyWithoutAsking(StageAIPlanTests::Parsed(TEXT("make the spots red"))));
	TestFalse(TEXT("A cue store asks"), CanApplyWithoutAsking(StageAIPlanTests::Parsed(TEXT("store cue 5"))));
	TestFalse(TEXT("A patch asks"), CanApplyWithoutAsking(StageAIPlanTests::Parsed(TEXT("patch fixture 3 to universe 2"))));

	// Plans.
	FText Error;
	TestFalse(TEXT("An empty plan is refused"), CheckPlan({}, EStageAILanguage::English, Error));
	TArray<FStageAICommand> Long;
	Long.Init(StageAIPlanTests::Parsed(TEXT("go")), MaxPlanSteps + 1);
	TestFalse(TEXT("More than 32 steps are refused"), CheckPlan(Long, EStageAILanguage::English, Error) || !Error.ToString().Contains(TEXT("32")));
	Long.SetNum(MaxPlanSteps);
	TestTrue(TEXT("32 steps are a plan"), CheckPlan(Long, EStageAILanguage::English, Error));
	TestFalse(TEXT("Undo is not part of a plan"), CheckPlan(StageAIPlanTests::MakePlan({ TEXT("undo"), TEXT("store cue 2") }).Steps, EStageAILanguage::English, Error));
	TestTrue(TEXT("... and says so"), Error.ToString().Contains(TEXT("undo")));
	TestTrue(TEXT("Undo alone is fine"), CheckPlan(StageAIPlanTests::MakePlan({ TEXT("undo") }).Steps, EStageAILanguage::English, Error));
	TestFalse(TEXT("A question is no plan step"), CheckPlan(StageAIPlanTests::MakePlan({ TEXT("how many fixtures?") }).Steps, EStageAILanguage::English, Error));
	TestTrue(TEXT("Store and GO"), CheckPlan(StageAIPlanTests::MakePlan({ TEXT("store cue 2"), TEXT("go") }).Steps, EStageAILanguage::English, Error));
	TestTrue(TEXT("GO is not undone"), HasStepsNotUndone(StageAIPlanTests::MakePlan({ TEXT("store cue 2"), TEXT("go") }).Steps));
	TestFalse(TEXT("A store is"), HasStepsNotUndone(StageAIPlanTests::MakePlan({ TEXT("store cue 2") }).Steps));

	FStageAICommand Patch = StageAIPlanTests::Parsed(TEXT("patch fixtures 1 to 4 at 2.001"));
	TestEqual(TEXT("Patch line"), DescribeStep(Patch, EStageAILanguage::English).ToString(), FString(TEXT("Patch fixtures 1-4 from 2.001")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantPlanGrammarTest, "StageCraft.Assistant.PlanGrammar", StageAIPlanTests::Flags)

bool FStageAssistantPlanGrammarTest::RunTest(const FString& Parameters)
{
	using StageAIPlanTests::Parsed;
	const auto Ids = [](const FStageAICommand& Command) { return FString::JoinBy(Command.FixtureIds, TEXT(","), [](int32 Id) { return FString::FromInt(Id); }); };

	FStageAICommand Command = Parsed(TEXT("patch fixture 3 to universe 2 address 101"));
	TestTrue(TEXT("Patch one fixture"), Command.Intent == EStageAIIntent::PatchFixtures && Ids(Command) == TEXT("3") && Command.Universe.Get(0) == 2 && Command.Address.Get(0) == 101);
	Command = Parsed(TEXT("patch fixtures 1 to 4 at 2.001"));
	TestTrue(TEXT("A range at U.AAA"), Ids(Command) == TEXT("1,2,3,4") && Command.Universe.Get(0) == 2 && Command.Address.Get(0) == 1);
	Command = Parsed(TEXT("patch fixtures 5-7 to universe 3"));
	TestTrue(TEXT("A dashed range"), Ids(Command) == TEXT("5,6,7") && Command.Universe.Get(0) == 3 && !Command.Address.IsSet());
	Command = Parsed(TEXT("patch fixtures 1, 4 and 9 at address 20"));
	TestTrue(TEXT("A list"), Ids(Command) == TEXT("1,4,9") && !Command.Universe.IsSet() && Command.Address.Get(0) == 20);
	Command = Parsed(TEXT("patch the selected fixtures to universe 2"));
	TestTrue(TEXT("The selection"), Command.Intent == EStageAIIntent::PatchFixtures && Command.FixtureIds.IsEmpty() && Command.Universe.Get(0) == 2);
	Command = Parsed(TEXT("patch fixtures 1 to 1000"));
	FText Error;
	TestFalse(TEXT("A huge range is refused by validation"), StageAICommandSchema::Validate(Command, Error));
	Command = Parsed(TEXT("patch fixture 2 at 1.600"));
	TestFalse(TEXT("Address past 512 refused"), StageAICommandSchema::Validate(Command, Error));
	TestTrue(TEXT("The patch sheet is still an export"), Parsed(TEXT("export the patch sheet")).Intent == EStageAIIntent::Export);

	Command = Parsed(TEXT("delete cue 3"));
	TestTrue(TEXT("Delete a cue"), Command.Intent == EStageAIIntent::DeleteCue && FMath::IsNearlyEqual(Command.CueNumber.Get(0.f), 3.f));
	Command = Parsed(TEXT("rename cue 2 to Big Finale"));
	TestTrue(TEXT("Rename a cue"), Command.Intent == EStageAIIntent::UpdateCue && Command.Label == TEXT("Big Finale") && !Command.FadeSeconds.IsSet());
	Command = Parsed(TEXT("set cue 4 fade to 2.5 seconds"));
	TestTrue(TEXT("A cue's fade"), Command.Intent == EStageAIIntent::UpdateCue && FMath::IsNearlyEqual(Command.FadeSeconds.Get(0.f), 2.5f) && Command.Label.IsEmpty());
	Command = Parsed(TEXT("change the fade of cue 4 to 3"));
	TestTrue(TEXT("... said the other way"), Command.Intent == EStageAIIntent::UpdateCue && FMath::IsNearlyEqual(Command.FadeSeconds.Get(0.f), 3.f));
	TestTrue(TEXT("Storing is still storing"), Parsed(TEXT("store cue 5 with a 3 second fade")).Intent == EStageAIIntent::StoreCue);
	TestTrue(TEXT("... and make cue 5"), Parsed(TEXT("make cue 5")).Intent == EStageAIIntent::StoreCue);
	TestTrue(TEXT("Cue 3 alone plays it"), Parsed(TEXT("cue 3")).Intent == EStageAIIntent::GoToCue);
	Command = Parsed(TEXT("set cue 4"));
	TestTrue(TEXT("An update that changes nothing is not one"), Command.Intent != EStageAIIntent::UpdateCue);

	// Armenian.
	Command = Parsed(TEXT("ջնջիր կյու 3"));
	TestTrue(TEXT("HY delete a cue"), Command.Intent == EStageAIIntent::DeleteCue && Command.Language == EStageAILanguage::Armenian);
	Command = Parsed(TEXT("պատչիր 1-4 սարքերը 2 յունիվերսում"));
	TestTrue(TEXT("HY patch a range into a universe"), Command.Intent == EStageAIIntent::PatchFixtures && Ids(Command) == TEXT("1,2,3,4") && Command.Universe.Get(0) == 2);

	// Confirmations of a proposal.
	using StageAICommandParser::EStageAIConfirmation;
	TestTrue(TEXT("apply is yes"), StageAICommandParser::ParseConfirmation(TEXT("apply")) == EStageAIConfirmation::Yes);
	TestTrue(TEXT("կիրառիր is yes"), StageAICommandParser::ParseConfirmation(TEXT("կիրառիր")) == EStageAIConfirmation::Yes);
	TestTrue(TEXT("cancel is no"), StageAICommandParser::ParseConfirmation(TEXT("cancel")) == EStageAIConfirmation::No);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryGroupTest, "StageCraft.History.Group", StageAIPlanTests::Flags)

bool FStageHistoryGroupTest::RunTest(const FString& Parameters)
{
	using namespace StageAIPlanTests;
	FScopedWorld Scope;
	UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(Scope.World);
	if (!TestNotNull(TEXT("History"), History))
	{
		return false;
	}
	FNullItemEditor Editor;
	int32 Stage = 0;
	const auto Do = [&](int32 Delta, const TCHAR* Name, bool bRefuseUndo = false)
	{
		History->Record(MakeShared<FCounterCommand>(Stage, Delta, Name, bRefuseUndo));
		Stage += Delta;
	};

	// Kept: one step for everything.
	TestTrue(TEXT("Group opens"), History->BeginGroup(INVTEXT("Assistant: three")));
	TestFalse(TEXT("Only one at a time"), History->BeginGroup(INVTEXT("Other")));
	Do(1, TEXT("a"));
	Do(10, TEXT("b"));
	Do(100, TEXT("c"));
	TestEqual(TEXT("Collected, not on the stack"), History->GetUndoCount(), 0);
	TestEqual(TEXT("... three in the group"), History->GetGroupCount(), 3);
	TestTrue(TEXT("Undo refused while open"), History->Undo(Editor) == EStageCommandResult::Refused);
	TestEqual(TEXT("... nothing reverted"), Stage, 111);
	History->EndGroup();
	TestFalse(TEXT("Closed"), History->IsGroupOpen());
	TestEqual(TEXT("One step"), History->GetUndoCount(), 1);
	TestEqual(TEXT("... named by the group"), History->GetUndoDescription().ToString(), FString(TEXT("Assistant: three")));
	TestTrue(TEXT("Undo"), History->Undo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("... all of it"), Stage, 0);
	TestTrue(TEXT("Redo"), History->Redo(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("... all of it"), Stage, 111);

	// A group of one keeps its own name; an empty group records nothing.
	History->BeginGroup(INVTEXT("Assistant: one"));
	Do(1000, TEXT("Store Cue 9"));
	History->EndGroup();
	TestEqual(TEXT("One command keeps its name"), History->GetUndoDescription().ToString(), FString(TEXT("Store Cue 9")));
	const int32 Steps = History->GetUndoCount();
	History->BeginGroup(INVTEXT("Empty"));
	History->EndGroup();
	TestEqual(TEXT("Empty group records nothing"), History->GetUndoCount(), Steps);

	// Aborted: reverted and not recorded.
	const int32 Before = Stage;
	History->BeginGroup(INVTEXT("Assistant: aborted"));
	Do(5, TEXT("x"));
	Do(7, TEXT("y"));
	TestTrue(TEXT("Abort rolls back"), History->AbortGroup(Editor) == EStageCommandResult::Succeeded);
	TestEqual(TEXT("... stage as before"), Stage, Before);
	TestEqual(TEXT("... nothing recorded"), History->GetUndoCount(), Steps);
	TestTrue(TEXT("Abort with nothing open"), History->AbortGroup(Editor) == EStageCommandResult::NothingToDo);
	History->BeginGroup(INVTEXT("Empty"));
	TestTrue(TEXT("Abort of an empty group"), History->AbortGroup(Editor) == EStageCommandResult::Succeeded);

	// A rollback the rules refuse: what was done stays, as one undoable step.
	History->BeginGroup(INVTEXT("Assistant: stuck"));
	Do(5, TEXT("x"));
	Do(7, TEXT("y"), /*bRefuseUndo*/ true);
	TestTrue(TEXT("Refused rollback reported"), History->AbortGroup(Editor) == EStageCommandResult::Refused);
	TestEqual(TEXT("... stage keeps the changes"), Stage, Before + 12);
	TestEqual(TEXT("... kept as one step"), History->GetUndoCount(), Steps + 1);
	TestEqual(TEXT("... under the group's name"), History->GetUndoDescription().ToString(), FString(TEXT("Assistant: stuck")));

	// A scene load drops an open group.
	History->BeginGroup(INVTEXT("Dropped"));
	Do(1, TEXT("z"));
	History->ClearHistory();
	TestFalse(TEXT("Cleared with the history"), History->IsGroupOpen());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantPlanTest, "StageCraft.Assistant.Plan", StageAIPlanTests::Flags)

bool FStageAssistantPlanTest::RunTest(const FString& Parameters)
{
	using namespace StageAIPlanTests;
	FScopedWorld Scope;
	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(Scope.World);
	UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(Scope.World);
	if (!TestNotNull(TEXT("Assistant"), Assistant) || !TestNotNull(TEXT("History"), History))
	{
		return false;
	}
	FFakePlayer Player;
	Player.History = History;
	Player.Assistant = Assistant;
	const auto Last = [Assistant]() { return Assistant->GetMessages().Last(); };

	// A typed one-step change runs at once, as one step.
	TestTrue(TEXT("Typed store"), Assistant->SubmitPrompt(TEXT("store cue 5"), Player.MakeHost()));
	TestEqual(TEXT("... ran"), Player.Executed.Num(), 1);
	TestEqual(TEXT("... no card"), Assistant->GetOpenProposalId(), 0);
	TestEqual(TEXT("... one undo step"), History->GetUndoCount(), 1);
	TestTrue(TEXT("... done"), Last().Outcome == EStageAIOutcome::Succeeded);

	// Ask first: a card, nothing runs; no cancels it.
	Player.bAsk = true;
	Assistant->SubmitPrompt(TEXT("store cue 6"), Player.MakeHost());
	TestEqual(TEXT("Ask first: nothing ran"), Player.Executed.Num(), 1);
	TestTrue(TEXT("... a card"), Assistant->GetOpenProposalId() != 0 && Last().ProposalId == Assistant->GetOpenProposalId());
	TestTrue(TEXT("... listing the change"), Last().Text.ToString().Contains(TEXT("1. Store cue 6")));
	TestFalse(TEXT("... idle while it waits"), Assistant->IsBusy());
	Assistant->SubmitPrompt(TEXT("no"), Player.MakeHost());
	TestEqual(TEXT("No: closed"), Assistant->GetOpenProposalId(), 0);
	TestEqual(TEXT("... nothing ran"), Player.Executed.Num(), 1);
	TestTrue(TEXT("... said so"), Last().Text.ToString().Contains(TEXT("changed nothing")));
	TestFalse(TEXT("CANCEL with nothing open"), Assistant->CancelProposal());

	// APPLY runs it.
	Assistant->SubmitPrompt(TEXT("store cue 6"), Player.MakeHost());
	TestTrue(TEXT("APPLY"), Assistant->ApplyProposal(Player.MakeHost()));
	TestEqual(TEXT("... ran"), Player.Executed.Num(), 2);
	TestEqual(TEXT("... card closed"), Assistant->GetOpenProposalId(), 0);
	TestFalse(TEXT("APPLY twice"), Assistant->ApplyProposal(Player.MakeHost()));

	// Any other prompt drops the card.
	Assistant->SubmitPrompt(TEXT("store cue 7"), Player.MakeHost());
	const int32 Card = Assistant->GetOpenProposalId();
	Assistant->SubmitPrompt(TEXT("how many fixtures?"), Player.MakeHost());
	TestTrue(TEXT("A question drops the card"), Card != 0 && Assistant->GetOpenProposalId() == 0);
	TestEqual(TEXT("... unapplied"), Player.Executed.Num(), 2);
	Player.bAsk = false;

	// A plan of three: proposed (it comes as the online model's), "yes" applies it as ONE undo step.
	const int32 Steps = History->GetUndoCount();
	const int32 StageBefore = Player.Stage;
	TestTrue(TEXT("Plan submitted"), Assistant->SubmitPlanForTesting(MakePlan({ TEXT("store cue 8"), TEXT("patch fixtures 1 to 4 at 2.001"), TEXT("go to cue 8") }), Player.MakeHost()));
	FString CardText = Last().Text.ToString();
	TestTrue(TEXT("... a card of three"), Assistant->GetOpenProposalId() != 0 && CardText.Contains(TEXT("1. Store cue 8")) && CardText.Contains(TEXT("2. Patch fixtures 1-4 from 2.001"))
		&& CardText.Contains(TEXT("3. Go to cue 8")) && CardText.Contains(TEXT("not undone")));
	Assistant->SubmitPrompt(TEXT("yes"), Player.MakeHost());
	TestEqual(TEXT("... all three ran"), Player.Executed.Num(), 5);
	TestEqual(TEXT("... one undo step"), History->GetUndoCount(), Steps + 1);
	TestEqual(TEXT("... named for the plan"), History->GetUndoDescription().ToString(), FString(TEXT("Assistant: Store cue 8 (+2 more)")));
	TestTrue(TEXT("... reported"), Last().Outcome == EStageAIOutcome::Succeeded && Last().Text.ToString().Contains(TEXT("3 changes, one undo step")));
	History->Undo(Player.Editor);
	TestEqual(TEXT("Ctrl+Z reverts the whole plan"), Player.Stage, StageBefore);

	// A refused step: the plan stops, the steps before it are rolled back, nothing is recorded.
	const int32 StepsBefore = History->GetUndoCount();
	const int32 StageBeforeRefusal = Player.Stage;
	Player.Refuse = { EStageAIIntent::DeleteCue };
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("store cue 9"), TEXT("make it rain"), TEXT("delete cue 3"), TEXT("store cue 10") }), Player.MakeHost());
	Assistant->ApplyProposal(Player.MakeHost());
	TestEqual(TEXT("Stopped at the refusal"), Player.Executed.Num(), 8);
	TestEqual(TEXT("... stage as before"), Player.Stage, StageBeforeRefusal);
	TestEqual(TEXT("... no step recorded"), History->GetUndoCount(), StepsBefore);
	TestFalse(TEXT("... group closed"), History->IsGroupOpen());
	TestTrue(TEXT("... reported"), Last().Outcome == EStageAIOutcome::Refused && Last().Text.ToString().Contains(TEXT("Step 3 of 4"))
		&& Last().Text.ToString().Contains(TEXT("refused by the rules")) && Last().Text.ToString().Contains(TEXT("2 steps before it were undone")));
	Player.Refuse.Reset();

	// What may apply without asking does, even from the online model.
	const int32 Executed = Player.Executed.Num();
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("go"), TEXT("set the selected lights to 50%") }), Player.MakeHost());
	TestEqual(TEXT("Playback and selected controls: no card"), Assistant->GetOpenProposalId(), 0);
	TestEqual(TEXT("... ran"), Player.Executed.Num(), Executed + 2);

	// Steps that run alone.
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("undo"), TEXT("store cue 11") }), Player.MakeHost());
	TestTrue(TEXT("Undo inside a plan refused"), Last().Outcome == EStageAIOutcome::Refused && Assistant->GetOpenProposalId() == 0);
	TestEqual(TEXT("... nothing ran"), Player.Executed.Num(), Executed + 2);

	// A pending step (fixtures waiting for their assets): the plan waits, then goes on, still one step.
	const int32 PendingSteps = History->GetUndoCount();
	Player.Pending = { EStageAIIntent::AddFixtures };
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("add 2 lights"), TEXT("store cue 12") }), Player.MakeHost());
	Assistant->ApplyProposal(Player.MakeHost());
	TestTrue(TEXT("Waiting for the step"), Assistant->GetState() == EStageAIAssistantState::Acting && Last().Outcome == EStageAIOutcome::Pending);
	TestTrue(TEXT("... the group is open"), History->IsGroupOpen());
	TestFalse(TEXT("... no other prompt"), Assistant->SubmitPrompt(TEXT("go"), Player.MakeHost()));
	Assistant->FinishPendingAction(FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("placed 2"))));
	TestFalse(TEXT("... then finished"), Assistant->IsBusy());
	TestEqual(TEXT("... both steps"), Player.Executed.Num(), Executed + 4);
	TestEqual(TEXT("... one undo step"), History->GetUndoCount(), PendingSteps + 1);
	TestTrue(TEXT("... the waited line is in the report"), Last().Text.ToString().Contains(TEXT("placed 2")));

	// A step that finishes inside its own call.
	Player.bFinishInside = true;
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("add 2 lights"), TEXT("store cue 13") }), Player.MakeHost());
	Assistant->ApplyProposal(Player.MakeHost());
	TestFalse(TEXT("Finished inside: not left waiting"), Assistant->IsBusy());
	TestTrue(TEXT("... both ran"), Player.Executed.Num() == Executed + 6 && Last().Outcome == EStageAIOutcome::Succeeded);

	// A pending step that is refused rolls back too.
	Player.bFinishInside = false;
	const int32 StepsBeforePendingRefusal = History->GetUndoCount();
	const int32 StageBeforePendingRefusal = Player.Stage;
	Assistant->SubmitPlanForTesting(MakePlan({ TEXT("store cue 14"), TEXT("add 2 lights") }), Player.MakeHost());
	Assistant->ApplyProposal(Player.MakeHost());
	Assistant->FinishPendingAction(FStageAIActionOutcome::Make(EStageAIOutcome::Refused, FText::FromString(TEXT("no room"))));
	TestEqual(TEXT("Pending refusal rolls back"), Player.Stage, StageBeforePendingRefusal);
	TestEqual(TEXT("... nothing recorded"), History->GetUndoCount(), StepsBeforePendingRefusal);

	// Armenian card.
	FStageAIPlan Armenian = MakePlan({ TEXT("պահիր կյու 15"), TEXT("ջնջիր կյու 3") });
	Armenian.Language = EStageAILanguage::Armenian;
	Assistant->SubmitPlanForTesting(Armenian, Player.MakeHost());
	TestTrue(TEXT("Armenian card"), Last().Text.ToString().Contains(TEXT("Պահել կյու 15")) && Last().Text.ToString().Contains(TEXT("APPLY")));
	Assistant->CancelProposal();
	TestTrue(TEXT("... cancelled in Armenian"), Last().Text.ToString().Contains(TEXT("ոչինչ չփոխեցի")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantOnlinePlanTest, "StageCraft.Assistant.OnlinePlan", StageAIPlanTests::Flags)

bool FStageAssistantOnlinePlanTest::RunTest(const FString& Parameters)
{
	using namespace StageAIPlanTests;

	// The wire: several tool calls in one answer are one plan; one bad call or a cut-off answer runs nothing.
	const FString Calls = TEXT("{\"stop_reason\":\"tool_use\",\"content\":[")
		TEXT("{\"type\":\"text\",\"text\":\"Storing and patching.\"},")
		TEXT("{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"stage_command\",\"input\":{\"intent\":\"store_cue\",\"cue_number\":3,\"label\":\"Intro\"}},")
		TEXT("{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"stage_command\",\"input\":{\"intent\":\"patch_fixtures\",\"fixture_ids\":[1,2],\"universe\":2,\"address\":1}}]}");
	FStageAIModelResponse Response = StageAICommandSchema::ParseResponseBody(200, Calls);
	TestTrue(TEXT("Two calls parsed"), Response.Status == FStageAIModelResponse::EStatus::Command && Response.Commands.Num() == 2);
	TestTrue(TEXT("... in order"), Response.Commands.Num() == 2 && Response.Command.Intent == EStageAIIntent::StoreCue
		&& Response.Commands[1].Intent == EStageAIIntent::PatchFixtures && Response.Commands[1].FixtureIds.Num() == 2 && Response.Commands[1].Universe.Get(0) == 2);
	Response = StageAICommandSchema::ParseResponseBody(200, Calls.Replace(TEXT("\"universe\":2"), TEXT("\"universe\":2,\"wallet\":1")));
	TestTrue(TEXT("One bad call fails all"), Response.bInvalidCommand && Response.Commands.IsEmpty());
	Response = StageAICommandSchema::ParseResponseBody(200, Calls.Replace(TEXT("\"stop_reason\":\"tool_use\""), TEXT("\"stop_reason\":\"max_tokens\"")));
	TestTrue(TEXT("A cut-off answer runs nothing"), Response.Status == FStageAIModelResponse::EStatus::Failed && Response.Commands.IsEmpty());
	Response = StageAICommandSchema::ParseResponseBody(200, Calls.Replace(TEXT("[1,2]"), TEXT("[1,\"two\"]")));
	TestTrue(TEXT("A fixture id that is not a number"), Response.bInvalidCommand);
	TestFalse(TEXT("No parallel limit on the wire"), StageAICommandSchema::BuildRequestBody(FStageAIModelOptions(), {}, TEXT("x"), TEXT("y")).Contains(TEXT("disable_parallel_tool_use")));

	// Through the subsystem, with a scripted backend.
	FScopedWorld Scope;
	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(Scope.World);
	UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(Scope.World);
	if (!TestNotNull(TEXT("Assistant"), Assistant) || !TestNotNull(TEXT("History"), History))
	{
		return false;
	}
	UStageAITestBackend* Backend = NewObject<UStageAITestBackend>(Assistant);
	Assistant->SetBackendForTesting(Backend);
	ON_SCOPE_EXIT { Assistant->SetBackendForTesting(nullptr); };
	FFakePlayer Player;
	Player.History = History;
	Player.Assistant = Assistant;

	// One online change that may apply at once runs for the player who asked (it used to lose the player's executor).
	Backend->NextResponse = StageAICommandSchema::ParseResponseBody(200,
		TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"stage_command\",\"input\":{\"intent\":\"cue_go\"}}]}"));
	Assistant->SubmitPrompt(TEXT("next please"), Player.MakeHost());
	TestEqual(TEXT("Online GO ran"), Player.Executed.Num(), 1);
	TestTrue(TEXT("... done"), Assistant->GetMessages().Last().Outcome == EStageAIOutcome::Succeeded);

	// One online change that is not: a card.
	Backend->NextResponse = StageAICommandSchema::ParseResponseBody(200,
		TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"stage_command\",\"input\":{\"intent\":\"delete_cue\",\"cue_number\":2}}]}"));
	Assistant->SubmitPrompt(TEXT("get rid of the second cue"), Player.MakeHost());
	TestTrue(TEXT("Online delete waits for APPLY"), Assistant->GetOpenProposalId() != 0 && Player.Executed.Num() == 1);
	Assistant->CancelProposal();

	// Two calls: one card, applied as one step.
	Backend->NextResponse = StageAICommandSchema::ParseResponseBody(200, Calls);
	const int32 Steps = History->GetUndoCount();
	Assistant->SubmitPrompt(TEXT("store an intro cue and patch the first two lights to universe 2"), Player.MakeHost());
	TestTrue(TEXT("Card of two"), Assistant->GetOpenProposalId() != 0 && Assistant->GetMessages().Last().Text.ToString().Contains(TEXT("2. Patch fixtures 1, 2 from 2.001")));
	Assistant->ApplyProposal(Player.MakeHost());
	TestEqual(TEXT("... both ran"), Player.Executed.Num(), 3);
	TestEqual(TEXT("... one step"), History->GetUndoCount(), Steps + 1);

	// A question among changes is answered; the changes still need APPLY.
	Backend->NextResponse = StageAICommandSchema::ParseResponseBody(200,
		TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"stage_command\",\"input\":{\"intent\":\"fixture_count\"}},")
		TEXT("{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"stage_command\",\"input\":{\"intent\":\"store_cue\",\"cue_number\":4}},")
		TEXT("{\"type\":\"tool_use\",\"id\":\"c\",\"name\":\"stage_command\",\"input\":{\"intent\":\"set_weather\",\"weather\":\"rain\"}}]}"));
	const int32 Lines = Assistant->GetMessages().Num();
	Assistant->SubmitPrompt(TEXT("how many lights, then store cue 4 and make it rain"), Player.MakeHost());
	TestEqual(TEXT("Question answered, card shown"), Assistant->GetMessages().Num(), Lines + 3);
	TestTrue(TEXT("... the card"), Assistant->GetOpenProposalId() != 0 && Player.Executed.Num() == 3);
	Assistant->CancelProposal();

	// Undo cannot ride along.
	Backend->NextResponse = StageAICommandSchema::ParseResponseBody(200,
		TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"stage_command\",\"input\":{\"intent\":\"undo\"}},")
		TEXT("{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"stage_command\",\"input\":{\"intent\":\"store_cue\"}}]}"));
	Assistant->SubmitPrompt(TEXT("undo that and store again"), Player.MakeHost());
	TestTrue(TEXT("Undo in a plan refused"), Assistant->GetMessages().Last().Outcome == EStageAIOutcome::Refused && Player.Executed.Num() == 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
