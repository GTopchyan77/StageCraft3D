// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "AI/StageAIAssistantBackend.h"
#include "AI/StageAIAssistantSettings.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageAIToolCatalogue.h"
#include "Algo/AllOf.h"
#include "AI/StageKnowledgeSearch.h"
#include "AI/StageKnowledgeSubsystem.h"
#include "Engine/LocalPlayer.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "Data/BaseItemData.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowDiagnosticsSubsystem.h"
#include "Components/SelectionComponent.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageUIAnchors.h"
#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAICommandSubsystem)

#define LOCTEXT_NAMESPACE "StageAICommandSubsystem"

bool UStageAICommandSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageAICommandSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GetDefault<UStageAIAssistantSettings>()->UsesOnlineModel())
	{
		Backend = NewObject<UStageClaudeAssistantBackend>(this);
	}
}

void UStageAICommandSubsystem::Deinitialize()
{
	if (Backend)
	{
		Backend->Cancel();
		Backend = nullptr;
	}
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	PendingAssetsCallback.Unbind();
	StopMarketplaceWait();
	PurchaseOffer.Reset();
	Proposal.Reset();
	// A plan still being applied keeps what it did: the player's history component records its group when play ends.
	RunningPlan.Reset();
	PendingHost = FStageAIActionHost();
	State = EStageAIAssistantState::Idle;
	OnConversationChanged.Clear();
	Super::Deinitialize();
}

void UStageAICommandSubsystem::AddMessage(EStageAISpeaker Speaker, EStageAIOutcome Outcome, const FText& Text, const FStageAIReference& Reference, int32 ProposalId)
{
	FStageAIMessage& Message = Messages.AddDefaulted_GetRef();
	Message.Speaker = Speaker;
	Message.Outcome = Outcome;
	Message.Text = Text;
	Message.Reference = Reference;
	Message.ProposalId = ProposalId;
	if (Messages.Num() > MaxMessages)
	{
		Messages.RemoveAt(0, Messages.Num() - MaxMessages);
	}
	OnConversationChanged.Broadcast();
}

void UStageAICommandSubsystem::SetState(EStageAIAssistantState NewState)
{
	if (State == NewState)
	{
		return;
	}
	State = NewState;
	if (State == EStageAIAssistantState::Idle)
	{
		PendingHost = FStageAIActionHost();
		PromptSelection.Reset();
	}
	OnConversationChanged.Broadcast();
}

FText UStageAICommandSubsystem::WithNote(const FText& Text, const FText& Note)
{
	return Note.IsEmpty() ? Text : FText::Format(LOCTEXT("WithNote", "{0}\n({1})"), Text, Note);
}

FText UStageAICommandSubsystem::DescribeOffline(const FText& Reason, EStageAILanguage Language)
{
	return FText::Format(StageAILanguage::Pick(Language, LOCTEXT("AnsweredOffline", "answered offline: {0}"), INVTEXT("պատասխանվեց օֆլայն՝ {0}")), Reason);
}

bool UStageAICommandSubsystem::SubmitPrompt(const FString& Prompt, FStageAICommandExecutor Executor, TArray<FString> SelectedItemNames)
{
	FStageAIActionHost Host;
	Host.Execute = MoveTemp(Executor);
	return SubmitPrompt(Prompt, MoveTemp(Host), MoveTemp(SelectedItemNames));
}

bool UStageAICommandSubsystem::SubmitPrompt(const FString& Prompt, FStageAIActionHost Host, TArray<FString> SelectedItemNames)
{
	const FString Trimmed = Prompt.TrimStartAndEnd().Left(StageAICommandParser::MaxPromptLength);
	if (Trimmed.IsEmpty() || IsBusy())
	{
		return false;
	}
	if (AnswerProposal(Trimmed, Host, SelectedItemNames) || AnswerPurchaseOffer(Trimmed, Host, SelectedItemNames))
	{
		return true;
	}

	// The history the model sees is the conversation before this prompt.
	FStageAIBackendRequest Request;
	const int32 MaxHistory = GetDefault<UStageAIAssistantSettings>()->GetMaxHistoryMessages();
	for (int32 Index = FMath::Max(0, Messages.Num() - MaxHistory); Index < Messages.Num(); ++Index)
	{
		Request.History.Add(Messages[Index]);
	}

	AddMessage(EStageAISpeaker::User, EStageAIOutcome::Info, FText::FromString(Trimmed));
	PendingHost = MoveTemp(Host);
	PromptSelection = MoveTemp(SelectedItemNames);

	// "How do I ..." and "where is ..." are answered offline when the knowledge knows (Docs/ADR/0030 §3), even with the online
	// model on: the answer is the same, free, and can point at the control. An action asked that way is never sent to be run.
	if (StageKnowledgeSearch::IsHowToQuestion(Trimmed))
	{
		const TOptional<FStageAICommand> Parsed = StageAICommandParser::Parse(Trimmed);
		const StageKnowledgeSearch::ERoute Route = StageKnowledgeSearch::Route(Trimmed, Parsed.GetPtrOrNull());
		if (Route != StageKnowledgeSearch::ERoute::Command)
		{
			FStageAIInterpretation Knowledge;
			if (AnswerFromKnowledge(Trimmed, Knowledge) || Route == StageKnowledgeSearch::ERoute::KnowledgeOnly)
			{
				Respond(Route == StageKnowledgeSearch::ERoute::KnowledgeOnly && Knowledge.Reply.IsEmpty() ? InterpretLocally(Trimmed) : Knowledge);
				return true;
			}
		}
	}

	FText Unavailable;
	if (Backend && Backend->IsAvailable(Unavailable))
	{
		Request.Prompt = Trimmed;
		Request.SceneSummary = StageAICommandSchema::DescribeScene(CaptureScene());
		SetState(EStageAIAssistantState::Interpreting);
		if (Backend->Interpret(Request, FOnStageAIBackendFinished::CreateUObject(this, &ThisClass::HandleBackendFinished, Trimmed)))
		{
			return true;
		}
		State = EStageAIAssistantState::Idle;
		Unavailable = LOCTEXT("BackendNotStarted", "the online request could not start");
	}

	FStageAIInterpretation Interpretation = InterpretLocally(Trimmed);
	// Say once per answer why the online model was skipped, only when it is meant to be used.
	if (Backend)
	{
		Interpretation.Note = DescribeOffline(Unavailable, StageAILanguage::Detect(Trimmed));
	}
	Respond(Interpretation);
	return true;
}

FStageAIInterpretation UStageAICommandSubsystem::InterpretLocally(const FString& Prompt) const
{
	const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Prompt);
	const StageKnowledgeSearch::ERoute Route = StageKnowledgeSearch::Route(Prompt, Command.GetPtrOrNull());
	if (Route != StageKnowledgeSearch::ERoute::Command)
	{
		FStageAIInterpretation Knowledge;
		if (AnswerFromKnowledge(Prompt, Knowledge))
		{
			return Knowledge;
		}
		if (Route == StageKnowledgeSearch::ERoute::KnowledgeOnly)
		{
			// Asked how, not asked to: nothing runs.
			return FStageAIInterpretation::MakeReply(StageAILanguage::Pick(StageAILanguage::Detect(Prompt),
				LOCTEXT("NoHelpTopic", "I have no help on that yet, so I changed nothing. To have it done, say it as a request, for example \"store cue 5\"."),
				INVTEXT("Այդ մասին դեռ օգնություն չունեմ, ուստի ոչինչ չփոխեցի։ Որպեսզի դա արվի, ասեք որպես խնդրանք, օրինակ՝ «պահիր կյու 5»։")));
		}
	}
	return Command.IsSet()
		? FStageAIInterpretation::MakeCommand(*Command)
		: FStageAIInterpretation::MakeReply(StageAIAnswers::DescribeNotUnderstood(Prompt));
}

FText UStageAICommandSubsystem::AnswerDiagnostics(const FStageAICommand& Command, FStageAIReference& OutReference) const
{
	using StageAILanguage::Pick;
	const EStageAILanguage Language = Command.Language;
	UWorld* World = GetWorld();
	UStageShowDiagnosticsSubsystem* Diagnostics = UWorld::GetSubsystem<UStageShowDiagnosticsSubsystem>(World);
	if (!Diagnostics)
	{
		return Pick(Language, LOCTEXT("NoDiagnostics", "There is no stage to check yet."), INVTEXT("Դեռ ստուգելու բեմ չկա։"));
	}
	EStageDiagnosticPlace Place = EStageDiagnosticPlace::None;
	FText Answer;
	if (Command.Intent == EStageAIIntent::ExplainFixture)
	{
		int32 FixtureId = Command.FixtureId.Get(INDEX_NONE);
		if (!Command.FixtureId.IsSet())
		{
			// "Why is it dark?": the asking player's selected fixture (the primary one when several are).
			const AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
			const USelectionComponent* Selection = Controller ? Controller->GetSelection() : nullptr;
			const ALightingFixtureActor* Fixture = Selection ? Cast<ALightingFixtureActor>(Selection->GetSelectedActor()) : nullptr;
			if (!Fixture && Selection)
			{
				for (AActor* Selected : Selection->GetSelectedActors())
				{
					if ((Fixture = Cast<ALightingFixtureActor>(Selected)) != nullptr)
					{
						break;
					}
				}
			}
			if (!Fixture)
			{
				return Pick(Language, LOCTEXT("WhichFixture", "Which fixture? Select one, or name it: \"why is fixture 12 dark\"."),
					INVTEXT("Ո՞ր սարքը։ Ընտրեք մեկը կամ նշեք այն՝ «ինչու է 12 սարքը մութ»։"));
			}
			FixtureId = Fixture->GetFixtureId();
		}
		const FStageFixtureVerdict Verdict = Diagnostics->ExplainFixture(FixtureId, Language);
		Place = Verdict.Place;
		Answer = FText::FromString(Verdict.Text);
	}
	else
	{
		const TArray<FStageDiagnostic>& Problems = Diagnostics->RefreshProblems();
		if (Problems.IsEmpty())
		{
			return Pick(Language, LOCTEXT("NoProblems", "I found no problems: the patch, DMX, masters, cues and music source look right."),
				INVTEXT("Խնդիրներ չգտա. պատչը, DMX-ը, մաստերները, կյուները և երաժշտության աղբյուրը կարգին են։"));
		}
		TArray<FString> Lines;
		Lines.Add(FText::Format(Pick(Language, LOCTEXT("ProblemsFound", "{0} {0}|plural(one=thing,other=things) to look at, worst first (all in the Problems panel):"),
			INVTEXT("Ուշադրության արժանի է {0} բան՝ ըստ կարևորության (բոլորը Problems վահանակում)։")), FText::AsNumber(Problems.Num())).ToString());
		constexpr int32 MaxListed = 6;
		for (int32 Index = 0; Index < Problems.Num() && Index < MaxListed; ++Index)
		{
			Lines.Add(FString::Printf(TEXT("%d. %s"), Index + 1, *StageShowDiagnostics::Describe(Problems[Index], Language).ToString()));
		}
		if (Problems.Num() > MaxListed)
		{
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("ProblemsMore", "...and {0} more."), INVTEXT("...և ևս {0}։")), FText::AsNumber(Problems.Num() - MaxListed)).ToString());
		}
		// The Problems panel lists them all, each with its own GO TO.
		Answer = FText::FromString(FString::Join(Lines, TEXT("\n")));
	}
	FGameplayTag Panel;
	FName Control;
	if (StageUIAnchors::GetPlaceTarget(Place, Panel, Control))
	{
		OutReference.Panel = Panel;
		OutReference.Control = Control;
	}
	else if (Command.Intent == EStageAIIntent::ListProblems)
	{
		OutReference.Panel = StageCraftTags::Panel_Problems;
		OutReference.Control = TEXT("ProblemList");
	}
	return Answer;
}

bool UStageAICommandSubsystem::AnswerFromKnowledge(const FString& Prompt, FStageAIInterpretation& OutReply) const
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UStageKnowledgeSubsystem* Knowledge = GameInstance ? GameInstance->GetSubsystem<UStageKnowledgeSubsystem>() : nullptr;
	if (!Knowledge)
	{
		return false;
	}
	const UStageInputPreferencesSubsystem* Preferences = nullptr;
	if (const ULocalPlayer* Player = World->GetFirstLocalPlayerFromController())
	{
		Preferences = Player->GetSubsystem<UStageInputPreferencesSubsystem>();
	}
	const FStageKnowledgeAnswer Answer = StageKnowledge::Answer(Knowledge->GetIndex(), Prompt, StageAILanguage::Detect(Prompt),
		Preferences ? Preferences->GetKeyBindings() : FStageKeyBindings::MakeDefaults());
	if (!Answer.bFound)
	{
		return false;
	}
	OutReply = FStageAIInterpretation::MakeReply(Answer.Text);
	OutReply.Reference.Panel = Answer.Panel;
	OutReply.Reference.Control = Answer.Control;
	OutReply.Reference.SourceTitle = Answer.SourceTitle;
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: answered from the knowledge (%s)."), *Answer.SourceTitle);
	return true;
}

void UStageAICommandSubsystem::HandleBackendFinished(const FStageAIModelResponse& Response, FString Prompt)
{
	if (State != EStageAIAssistantState::Interpreting)
	{
		return;
	}
	FStageAIInterpretation Interpretation;
	switch (Response.Status)
	{
	case FStageAIModelResponse::EStatus::Command:
		Interpretation = FStageAIInterpretation::MakeCommand(Response.Command);
		for (int32 Index = 1; Index < Response.Commands.Num(); ++Index)
		{
			Interpretation.MoreCommands.Add(Response.Commands[Index]);
		}
		Interpretation.bFromOnlineModel = true;
		break;
	case FStageAIModelResponse::EStatus::Text:
		Interpretation = FStageAIInterpretation::MakeReply(FText::FromString(Response.Text));
		break;
	case FStageAIModelResponse::EStatus::Refused:
		Interpretation = FStageAIInterpretation::MakeReply(StageAILanguage::Pick(StageAILanguage::Detect(Prompt),
			LOCTEXT("Declined", "The online model declined that request. Try rephrasing it as a stage command, or type \"help\"."),
			INVTEXT("Առցանց մոդելը մերժեց այդ հարցումը։ Փորձեք այն ձևակերպել որպես բեմի հրաման կամ գրեք «օգնություն»։")), EStageAIOutcome::Refused);
		break;
	default:
		// A validation failure of the model's own command is reported, never retried as something else.
		if (Response.bInvalidCommand)
		{
			Interpretation = FStageAIInterpretation::MakeReply(Response.Error, EStageAIOutcome::Refused);
			break;
		}
		Interpretation = InterpretLocally(Prompt);
		Interpretation.Note = DescribeOffline(Response.Error, StageAILanguage::Detect(Prompt));
		break;
	}
	// Not SetState: going idle forgets the asking player's host, and Respond still has to run the answer for them.
	State = EStageAIAssistantState::Idle;
	Respond(Interpretation);
}

void UStageAICommandSubsystem::Respond(const FStageAIInterpretation& Interpretation)
{
	const auto EndPrompt = [this]()
	{
		PendingHost = FStageAIActionHost();
		PromptSelection.Reset();
		SetState(EStageAIAssistantState::Idle);
	};
	if (Interpretation.Kind == FStageAIInterpretation::EKind::Reply)
	{
		AddMessage(EStageAISpeaker::Assistant, Interpretation.ReplyOutcome, WithNote(Interpretation.Reply, Interpretation.Note), Interpretation.Reference);
		EndPrompt();
		return;
	}

	// Several commands in one answer (the online model, Docs/ADR/0030 §6): every one is checked first; questions among them are
	// answered, and the changes become one plan.
	if (!Interpretation.MoreCommands.IsEmpty())
	{
		FStageAIPlan Plan;
		Plan.Language = Interpretation.Command.Language;
		Plan.bFromOnlineModel = Interpretation.bFromOnlineModel;
		TArray<FStageAICommand> All;
		All.Add(Interpretation.Command);
		All.Append(Interpretation.MoreCommands);
		for (FStageAICommand& Command : All)
		{
			FText Error;
			if (!StageAICommandSchema::Validate(Command, Error))
			{
				AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(Error, Interpretation.Note));
				EndPrompt();
				return;
			}
		}
		for (const FStageAICommand& Command : All)
		{
			if (Command.IsQuestion() && Command.Intent != EStageAIIntent::ListingDetails)
			{
				FStageAIReference Reference;
				const FText Answer = Command.Intent == EStageAIIntent::ExplainFixture || Command.Intent == EStageAIIntent::ListProblems
					? AnswerDiagnostics(Command, Reference) : StageAIAnswers::AnswerQuestion(Command, CaptureScene());
				AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, Answer, Reference);
			}
			else if (StageAIToolCatalogue::GetTool(Command.Intent).IsChange())
			{
				Plan.Steps.Add(Command);
			}
		}
		if (Plan.Steps.IsEmpty())
		{
			EndPrompt();
			return;
		}
		HandlePlan(MoveTemp(Plan), Interpretation.Note);
		return;
	}

	FStageAICommand Command = Interpretation.Command;
	FText Error;
	if (!StageAICommandSchema::Validate(Command, Error))
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(Error, Interpretation.Note));
		EndPrompt();
		return;
	}
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: interpreted %s."), *StageAICommandSchema::GetIntentName(Command.Intent));

	// Listings are named against the marketplace's current results, here and only here (Docs/ADR/0021 §13.2).
	if (Command.Intent == EStageAIIntent::ListingDetails || Command.Intent == EStageAIIntent::PreviewListing || Command.Intent == EStageAIIntent::AcquireListing)
	{
		FText NotFound;
		if (!ResolveListing(Command, NotFound))
		{
			AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(NotFound, Interpretation.Note));
			EndPrompt();
			return;
		}
	}
	else if (Command.Intent == EStageAIIntent::SearchMarketplace)
	{
		// New results: "it" means nothing in them until one is named (or there is only one).
		LastListing.Invalidate();
	}

	if (Command.Intent == EStageAIIntent::Help)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::DescribeHelp(Command.Language), Interpretation.Note));
	}
	else if (Command.Intent == EStageAIIntent::Unsupported)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::DescribeUnsupported(Command.ItemQuery, Command.Language), Interpretation.Note));
	}
	else if (Command.Intent == EStageAIIntent::ExplainFixture || Command.Intent == EStageAIIntent::ListProblems)
	{
		FStageAIReference Reference;
		const FText Answer = AnswerDiagnostics(Command, Reference);
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(Answer, Interpretation.Note), Reference);
	}
	else if (Command.IsQuestion())
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::AnswerQuestion(Command, CaptureScene()), Interpretation.Note));
	}
	else
	{
		FStageAIPlan Plan;
		Plan.Language = Command.Language;
		Plan.bFromOnlineModel = Interpretation.bFromOnlineModel;
		Plan.Steps.Add(MoveTemp(Command));
		HandlePlan(MoveTemp(Plan), Interpretation.Note);
		return;
	}
	EndPrompt();
}

void UStageAICommandSubsystem::HandlePlan(FStageAIPlan Plan, const FText& Note)
{
	// Every step was validated by Respond (StageAICommandSchema::Validate); this checks the plan as a whole.
	FText Error;
	if (!StageAIToolCatalogue::CheckPlan(Plan.Steps, Plan.Language, Error))
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(Error, Note));
		PendingHost = FStageAIActionHost();
		PromptSelection.Reset();
		SetState(EStageAIAssistantState::Idle);
		return;
	}
	// Undo, export and the marketplace run alone and as they always have: not grouped (an undo inside a group is refused), and
	// a paid listing has its own confirmation turn.
	if (Plan.Steps.Num() == 1 && !StageAIToolCatalogue::GetTool(Plan.Steps[0].Intent).bPlannable)
	{
		RunAction(Plan.Steps[0], Note);
		return;
	}
	if (NeedsApproval(Plan))
	{
		ProposePlan(MoveTemp(Plan), Note);
		return;
	}
	RunPlan(MoveTemp(Plan), Note);
}

bool UStageAICommandSubsystem::NeedsApproval(const FStageAIPlan& Plan) const
{
	if (PendingHost.bAskBeforeChanges)
	{
		return true;
	}
	if (!Plan.bFromOnlineModel && Plan.Steps.Num() == 1)
	{
		// One change the user typed, read by the local grammar: like pressing the button.
		return false;
	}
	return !Algo::AllOf(Plan.Steps, [](const FStageAICommand& Step) { return StageAIToolCatalogue::CanApplyWithoutAsking(Step); });
}

void UStageAICommandSubsystem::ProposePlan(FStageAIPlan Plan, const FText& Note)
{
	using StageAILanguage::Pick;
	const EStageAILanguage Language = Plan.Language;
	TArray<FString> Lines;
	Lines.Add((Plan.Steps.Num() == 1
		? Pick(Language, LOCTEXT("ProposeOne", "I would make this change:"), INVTEXT("Կկատարեմ այս փոփոխությունը՝"))
		: FText::Format(Pick(Language, LOCTEXT("ProposeMany", "I would make these {0} changes, as one undo step:"), INVTEXT("Կկատարեմ այս {0} փոփոխությունները՝ որպես մեկ հետարկման քայլ՝")),
			FText::AsNumber(Plan.Steps.Num()))).ToString());
	for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
	{
		Lines.Add(FString::Printf(TEXT("%d. %s"), Index + 1, *StageAIToolCatalogue::DescribeStep(Plan.Steps[Index], Language).ToString()));
	}
	if (StageAIToolCatalogue::HasStepsNotUndone(Plan.Steps))
	{
		Lines.Add(Pick(Language, LOCTEXT("ProposeNotUndone", "(Cue playback is not undone by Ctrl+Z.)"), INVTEXT("(Կյուների նվագարկումը Ctrl+Z-ով չի հետարկվում։)")).ToString());
	}
	Lines.Add(Pick(Language, LOCTEXT("ProposeAsk", "APPLY to make it, CANCEL to leave the stage as it is (or answer yes / no)."),
		INVTEXT("APPLY՝ կատարելու համար, CANCEL՝ բեմը անփոփոխ թողնելու համար (կամ պատասխանեք այո / ոչ)։")).ToString());

	Proposal.Emplace();
	Proposal->Id = ++LastProposalId;
	Proposal->Plan = MoveTemp(Plan);
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: proposed plan %d (%d step(s))."), Proposal->Id, Proposal->Plan.Steps.Num());
	PendingHost = FStageAIActionHost();
	PromptSelection.Reset();
	State = EStageAIAssistantState::Idle;
	AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(FText::FromString(FString::Join(Lines, TEXT("\n"))), Note), FStageAIReference(), Proposal->Id);
}

#if !UE_BUILD_SHIPPING
bool UStageAICommandSubsystem::SubmitPlanForTesting(FStageAIPlan Plan, FStageAIActionHost Host)
{
	if (IsBusy())
	{
		return false;
	}
	Proposal.Reset();
	PendingHost = MoveTemp(Host);
	Plan.bFromOnlineModel = true;
	for (FStageAICommand& Step : Plan.Steps)
	{
		FText Error;
		if (!StageAICommandSchema::Validate(Step, Error))
		{
			AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, Error);
			PendingHost = FStageAIActionHost();
			return true;
		}
	}
	HandlePlan(MoveTemp(Plan), FText::GetEmpty());
	return true;
}

void UStageAICommandSubsystem::SetBackendForTesting(UStageAIAssistantBackend* InBackend)
{
	if (Backend)
	{
		Backend->Cancel();
	}
	Backend = InBackend;
}
#endif

bool UStageAICommandSubsystem::ApplyProposal(FStageAIActionHost Host)
{
	if (!Proposal.IsSet() || IsBusy())
	{
		return false;
	}
	FStageAIPlan Plan = MoveTemp(Proposal->Plan);
	Proposal.Reset();
	PendingHost = MoveTemp(Host);
	RunPlan(MoveTemp(Plan), FText::GetEmpty());
	return true;
}

bool UStageAICommandSubsystem::CancelProposal()
{
	if (!Proposal.IsSet() || IsBusy())
	{
		return false;
	}
	const EStageAILanguage Language = Proposal->Plan.Language;
	Proposal.Reset();
	AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info,
		StageAILanguage::Pick(Language, LOCTEXT("ProposalCancelled", "OK, I changed nothing."), INVTEXT("Լավ, ոչինչ չփոխեցի։")));
	return true;
}

bool UStageAICommandSubsystem::AnswerProposal(const FString& Prompt, FStageAIActionHost& Host, TArray<FString>& SelectedItemNames)
{
	if (!Proposal.IsSet())
	{
		return false;
	}
	// The proposal stands for exactly one prompt: a yes, a no, or anything else, which drops it unapplied.
	const StageAICommandParser::EStageAIConfirmation Answer = StageAICommandParser::ParseConfirmation(Prompt);
	if (Answer == StageAICommandParser::EStageAIConfirmation::None)
	{
		Proposal.Reset();
		return false;
	}
	AddMessage(EStageAISpeaker::User, EStageAIOutcome::Info, FText::FromString(Prompt));
	if (Answer == StageAICommandParser::EStageAIConfirmation::No)
	{
		return CancelProposal();
	}
	PromptSelection = MoveTemp(SelectedItemNames);
	return ApplyProposal(MoveTemp(Host));
}

void UStageAICommandSubsystem::RunPlan(FStageAIPlan Plan, const FText& Note)
{
	check(!RunningPlan.IsSet());
	// The history step's name is in the UI language (the Edit menu shows it); the conversation speaks the plan's.
	const FText First = StageAIToolCatalogue::DescribeStep(Plan.Steps[0], EStageAILanguage::English);
	const FText Description = Plan.Steps.Num() == 1
		? FText::Format(LOCTEXT("PlanStepOne", "Assistant: {0}"), First)
		: FText::Format(LOCTEXT("PlanStepMany", "Assistant: {0} (+{1} more)"), First, FText::AsNumber(Plan.Steps.Num() - 1));
	State = EStageAIAssistantState::Acting;
	RunningPlan.Emplace();
	RunningPlan->Plan = MoveTemp(Plan);
	RunningPlan->Note = Note;
	RunningPlan->bGrouped = PendingHost.BeginPlan.IsBound() && PendingHost.BeginPlan.Execute(Description);
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: applying %d step(s)%s."), RunningPlan->Plan.Steps.Num(), RunningPlan->bGrouped ? TEXT(" as one undo step") : TEXT(""));
	OnConversationChanged.Broadcast();
	AdvancePlan();
}

void UStageAICommandSubsystem::AdvancePlan()
{
	while (RunningPlan.IsSet() && RunningPlan->NextStep < RunningPlan->Plan.Steps.Num())
	{
		if (!PendingHost.Execute.IsBound())
		{
			FinishPlan(FStageAIActionOutcome::Make(EStageAIOutcome::Refused, StageAILanguage::Pick(RunningPlan->Plan.Language,
				LOCTEXT("PlanNoPlayer", "There is no player to make that change for."), INVTEXT("Չկա խաղացող, որի համար կատարեմ այդ փոփոխությունը։"))));
			return;
		}
		const FStageAICommand Step = RunningPlan->Plan.Steps[RunningPlan->NextStep];
		UE_LOG(LogStageCraft, Log, TEXT("AI assistant: step %d of %d, %s."), RunningPlan->NextStep + 1, RunningPlan->Plan.Steps.Num(),
			*StageAICommandSchema::GetIntentName(Step.Intent));
		RunningPlan->bExecuting = true;
		FStageAIActionOutcome Outcome = PendingHost.Execute.Execute(Step);
		if (!RunningPlan.IsSet())
		{
			return;
		}
		RunningPlan->bExecuting = false;
		if (Outcome.Outcome == EStageAIOutcome::Pending)
		{
			if (!RunningPlan->EarlyOutcome.IsSet())
			{
				// Streaming assets or waiting for an answer: FinishPendingAction brings the step's result.
				AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Pending, Outcome.Message);
				return;
			}
			Outcome = *RunningPlan->EarlyOutcome;
			RunningPlan->EarlyOutcome.Reset();
		}
		if (Outcome.Outcome == EStageAIOutcome::Refused)
		{
			FinishPlan(Outcome);
			return;
		}
		RunningPlan->StepLines.Add(Outcome.Message);
		++RunningPlan->NextStep;
	}
	if (RunningPlan.IsSet())
	{
		FinishPlan({});
	}
}

void UStageAICommandSubsystem::HandleStepOutcome(const FStageAIActionOutcome& Outcome)
{
	if (Outcome.Outcome == EStageAIOutcome::Refused)
	{
		FinishPlan(Outcome);
		return;
	}
	RunningPlan->StepLines.Add(Outcome.Message);
	++RunningPlan->NextStep;
	AdvancePlan();
}

void UStageAICommandSubsystem::FinishPlan(TOptional<FStageAIActionOutcome> Refusal)
{
	using StageAILanguage::Pick;
	FRunningPlan Finished = MoveTemp(*RunningPlan);
	RunningPlan.Reset();
	const FStageAIPlan& Plan = Finished.Plan;
	const EStageAILanguage Language = Plan.Language;
	const int32 StepCount = Plan.Steps.Num();
	FStageAIActionHost Host = MoveTemp(PendingHost);
	PendingHost = FStageAIActionHost();
	PromptSelection.Reset();
	State = EStageAIAssistantState::Idle;

	if (!Refusal.IsSet())
	{
		if (Finished.bGrouped && Host.EndPlan.IsBound())
		{
			Host.EndPlan.Execute(/*bKeep*/ true);
		}
		FText Text = Finished.StepLines.IsEmpty() ? FText::GetEmpty() : Finished.StepLines[0];
		if (StepCount > 1)
		{
			TArray<FString> Lines;
			Lines.Add(FText::Format(Pick(Language, LOCTEXT("PlanDone", "Done: {0} changes, one undo step (Ctrl+Z reverts them all)."),
				INVTEXT("Կատարված է՝ {0} փոփոխություն, մեկ հետարկման քայլ (Ctrl+Z-ը կհետարկի բոլորը)։")), FText::AsNumber(StepCount)).ToString());
			for (int32 Index = 0; Index < Finished.StepLines.Num(); ++Index)
			{
				Lines.Add(FString::Printf(TEXT("%d. %s"), Index + 1, *Finished.StepLines[Index].ToString()));
			}
			Text = FText::FromString(FString::Join(Lines, TEXT("\n")));
		}
		UE_LOG(LogStageCraft, Log, TEXT("AI assistant: plan of %d step(s) applied."), StepCount);
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Succeeded, WithNote(Text, Finished.Note));
		return;
	}

	EStageCommandResult Rollback = EStageCommandResult::Succeeded;
	if (Finished.bGrouped && Host.EndPlan.IsBound())
	{
		Rollback = Host.EndPlan.Execute(/*bKeep*/ false);
	}
	const int32 Failed = Finished.NextStep;
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: step %d of %d refused; rollback %s."), Failed + 1, StepCount, *UEnum::GetValueAsString(Rollback));
	if (StepCount == 1)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(Refusal->Message, Finished.Note));
		return;
	}
	TArray<FString> Lines;
	Lines.Add(FText::Format(Pick(Language, LOCTEXT("PlanStepRefused", "Step {0} of {1} was not done ({2}): {3}"), INVTEXT("{1} քայլից {0}-րդը չկատարվեց ({2})՝ {3}")),
		FText::AsNumber(Failed + 1), FText::AsNumber(StepCount), StageAIToolCatalogue::DescribeStep(Plan.Steps[Failed], Language), Refusal->Message).ToString());
	if (Failed > 0)
	{
		const bool bRolledBack = Rollback == EStageCommandResult::Succeeded || Rollback == EStageCommandResult::NothingToDo;
		Lines.Add((bRolledBack
			? FText::Format(Pick(Language, LOCTEXT("PlanRolledBack", "The {0} {0}|plural(one=step,other=steps) before it {0}|plural(one=was,other=were) undone, so nothing changed."),
				INVTEXT("Դրանից առաջ կատարված {0} քայլը հետարկվեց, ուստի ոչինչ չփոխվեց։")), FText::AsNumber(Failed))
			: Pick(Language, LOCTEXT("PlanNotRolledBack", "The steps before it could not be undone; Ctrl+Z reverts them as one step."),
				INVTEXT("Դրանից առաջ կատարված քայլերը հնարավոր չեղավ հետարկել. Ctrl+Z-ը դրանք կհետարկի որպես մեկ քայլ։"))).ToString());
		if (StageAIToolCatalogue::HasStepsNotUndone(MakeArrayView(Plan.Steps.GetData(), Failed)))
		{
			Lines.Add(Pick(Language, LOCTEXT("PlanPlaybackStays", "Cue playback it started stays as it is."), INVTEXT("Սկսված կյուների նվագարկումը մնում է ինչպես կա։")).ToString());
		}
	}
	AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(FText::FromString(FString::Join(Lines, TEXT("\n"))), Finished.Note));
}

void UStageAICommandSubsystem::RunAction(const FStageAICommand& Command, const FText& Note)
{
	if (!PendingHost.Execute.IsBound())
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, StageAILanguage::Pick(Command.Language,
			LOCTEXT("NoPlayer", "There is no player to make that change for."), INVTEXT("Չկա խաղացող, որի համար կատարեմ այդ փոփոխությունը։")));
		SetState(EStageAIAssistantState::Idle);
		return;
	}

	// Acting while the executor runs, so a pending action may stream assets (RequestItemAssets) or wait for the marketplace.
	State = EStageAIAssistantState::Acting;
	const FStageAIActionOutcome Outcome = PendingHost.Execute.Execute(Command);
	if (Outcome.Outcome == EStageAIOutcome::Pending && State == EStageAIAssistantState::Acting)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Pending, WithNote(Outcome.Message, Note));
		return;
	}
	if (State == EStageAIAssistantState::Acting)
	{
		State = EStageAIAssistantState::Idle;
		// Kept before the line is added, so a view refreshing on it already sees the offer.
		PurchaseOffer = Outcome.PurchaseOffer;
		AddMessage(EStageAISpeaker::Assistant, Outcome.Outcome, WithNote(Outcome.Message, Note));
	}
	PendingHost = FStageAIActionHost();
	PromptSelection.Reset();
	SetState(EStageAIAssistantState::Idle);
}

bool UStageAICommandSubsystem::AnswerPurchaseOffer(const FString& Prompt, FStageAIActionHost& Host, TArray<FString>& SelectedItemNames)
{
	if (!PurchaseOffer.IsSet())
	{
		return false;
	}
	// The offer stands for exactly one prompt: a yes, a no, or anything else, which drops it unbought.
	const FStageAIPurchaseOffer Offer = *PurchaseOffer;
	PurchaseOffer.Reset();
	const StageAICommandParser::EStageAIConfirmation Answer = StageAICommandParser::ParseConfirmation(Prompt);
	if (Answer == StageAICommandParser::EStageAIConfirmation::None)
	{
		return false;
	}

	AddMessage(EStageAISpeaker::User, EStageAIOutcome::Info, FText::FromString(Prompt));
	if (Answer == StageAICommandParser::EStageAIConfirmation::No)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, FText::Format(StageAILanguage::Pick(Offer.Language,
			LOCTEXT("PurchaseDeclined", "OK, \"{0}\" was not bought."), INVTEXT("Լավ, «{0}»-ը չգնվեց։")), FText::FromString(Offer.Title)));
		return true;
	}

	// Validated like any command (which also clears the confirmation fields), then confirmed here: the only place that can.
	FStageAICommand Command;
	Command.Intent = EStageAIIntent::AcquireListing;
	Command.Language = Offer.Language;
	FText Error;
	if (!StageAICommandSchema::Validate(Command, Error))
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, Error);
		return true;
	}
	Command.Listing.Id = Offer.ListingId;
	Command.Listing.bLast = false;
	Command.ConfirmedCredits = Offer.Credits;
	LastListing = Offer.ListingId;
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: purchase of listing %s for %lld credits confirmed by the user."), *Offer.ListingId.ToString(), Offer.Credits);

	PendingHost = MoveTemp(Host);
	PromptSelection = MoveTemp(SelectedItemNames);
	RunAction(Command, FText::GetEmpty());
	return true;
}

bool UStageAICommandSubsystem::ResolveListing(FStageAICommand& Command, FText& OutError)
{
	const FStageAISceneView Scene = CaptureScene();
	const StageAICommandParser::FStageAIListingMatch Match = StageAICommandParser::ResolveListing(Scene.Listings, Command.Listing, LastListing);
	if (!Scene.Listings.IsValidIndex(Match.Index))
	{
		OutError = StageAIAnswers::DescribeListingNotFound(Scene, Command.Listing, Match, Command.Language);
		return false;
	}
	Command.Listing.Id = Scene.Listings[Match.Index].Listing.ListingId;
	LastListing = Command.Listing.Id;
	return true;
}

UStageMarketplaceSubsystem* UStageAICommandSubsystem::GetMarketplace() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UStageMarketplaceSubsystem>() : nullptr;
}

bool UStageAICommandSubsystem::AwaitMarketplace(EStageAIMarketplaceEvent Event, const FGuid& ListingId, FOnStageAIMarketplaceAnswer OnAnswer)
{
	UStageMarketplaceSubsystem* Marketplace = GetMarketplace();
	if (State != EStageAIAssistantState::Acting || !Marketplace || MarketplaceWait.IsSet() || !OnAnswer.IsBound())
	{
		return false;
	}
	MarketplaceWait.Emplace();
	MarketplaceWait->Event = Event;
	MarketplaceWait->ListingId = ListingId;
	MarketplaceWait->OnAnswer = MoveTemp(OnAnswer);
	WaitingMarketplace = Marketplace;
	switch (Event)
	{
	case EStageAIMarketplaceEvent::Query:
		Marketplace->OnListingsUpdated.AddUniqueDynamic(this, &ThisClass::HandleMarketplaceListings);
		break;
	case EStageAIMarketplaceEvent::Acquire:
		Marketplace->OnAcquireCompleted.AddUniqueDynamic(this, &ThisClass::HandleMarketplaceAcquired);
		break;
	case EStageAIMarketplaceEvent::Preview:
		PreviewReadyHandle = Marketplace->OnPreviewReady.AddWeakLambda(this,
			[this](const FGuid& PreviewedListing, const FStageMarketplaceResultInfo& Result, const FStageMarketplacePreviewScene&)
			{
				AnswerMarketplace(EStageAIMarketplaceEvent::Preview, PreviewedListing, Result, FString());
			});
		break;
	}
	return true;
}

void UStageAICommandSubsystem::CancelMarketplaceWait()
{
	StopMarketplaceWait();
}

void UStageAICommandSubsystem::StopMarketplaceWait()
{
	// Removing inside a broadcast is safe: both delegate kinds iterate a copy or defer compaction.
	if (UStageMarketplaceSubsystem* Marketplace = WaitingMarketplace.Get())
	{
		Marketplace->OnListingsUpdated.RemoveDynamic(this, &ThisClass::HandleMarketplaceListings);
		Marketplace->OnAcquireCompleted.RemoveDynamic(this, &ThisClass::HandleMarketplaceAcquired);
		Marketplace->OnPreviewReady.Remove(PreviewReadyHandle);
	}
	PreviewReadyHandle.Reset();
	WaitingMarketplace.Reset();
	MarketplaceWait.Reset();
}

void UStageAICommandSubsystem::HandleMarketplaceListings(FStageMarketplaceResultInfo Result)
{
	AnswerMarketplace(EStageAIMarketplaceEvent::Query, FGuid(), Result, FString());
}

void UStageAICommandSubsystem::HandleMarketplaceAcquired(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName)
{
	AnswerMarketplace(EStageAIMarketplaceEvent::Acquire, ListingId, Result, SceneName);
}

void UStageAICommandSubsystem::AnswerMarketplace(EStageAIMarketplaceEvent Event, const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName)
{
	if (!MarketplaceWait.IsSet() || MarketplaceWait->Event != Event || (Event == EStageAIMarketplaceEvent::Acquire && ListingId != MarketplaceWait->ListingId))
	{
		return;
	}
	FOnStageAIMarketplaceAnswer OnAnswer = MoveTemp(MarketplaceWait->OnAnswer);
	StopMarketplaceWait();
	OnAnswer.ExecuteIfBound(ListingId, Result, SceneName);
}

void UStageAICommandSubsystem::FinishPendingAction(const FStageAIActionOutcome& Outcome)
{
	if (State != EStageAIAssistantState::Acting)
	{
		return;
	}
	StopMarketplaceWait();
	if (RunningPlan.IsSet())
	{
		// A step of a plan: its result decides whether the plan goes on.
		if (RunningPlan->bExecuting)
		{
			RunningPlan->EarlyOutcome = Outcome;
			return;
		}
		HandleStepOutcome(Outcome);
		return;
	}
	State = EStageAIAssistantState::Idle;
	PendingHost = FStageAIActionHost();
	PromptSelection.Reset();
	AddMessage(EStageAISpeaker::Assistant, Outcome.Outcome, Outcome.Message);
}

bool UStageAICommandSubsystem::RequestItemAssets(const UBaseItemData& Item, FSimpleDelegate OnReady)
{
	const UBaseItemData* const One = &Item;
	return RequestItemAssets(MakeArrayView(&One, 1), MoveTemp(OnReady));
}

bool UStageAICommandSubsystem::RequestItemAssets(TConstArrayView<const UBaseItemData*> Items, FSimpleDelegate OnReady)
{
	if (State != EStageAIAssistantState::Acting || LoadHandle.IsValid() || PendingAssetsCallback.IsBound())
	{
		return false;
	}
	PendingAssetsCallback = MoveTemp(OnReady);
	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> AssetIds;
	for (const UBaseItemData* Item : Items)
	{
		// Items outside the Asset Manager scan (or built at runtime) are already in memory or load on spawn.
		if (Item && Item->RequiresAssetStreaming() && AssetManager.GetPrimaryAssetPath(Item->GetPrimaryAssetId()).IsValid())
		{
			AssetIds.AddUnique(Item->GetPrimaryAssetId());
		}
	}
	if (AssetIds.IsEmpty())
	{
		HandleItemAssetsLoaded();
		return true;
	}
	// The delegate may run inside this call when everything is loaded already, so the callback is stored first.
	TSharedPtr<FStreamableHandle> Handle = AssetManager.LoadPrimaryAssets(AssetIds, { UBaseItemData::GameBundle },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandleItemAssetsLoaded));
	if (PendingAssetsCallback.IsBound())
	{
		LoadHandle = MoveTemp(Handle);
	}
	return true;
}

void UStageAICommandSubsystem::HandleItemAssetsLoaded()
{
	LoadHandle.Reset();
	FSimpleDelegate Callback = MoveTemp(PendingAssetsCallback);
	PendingAssetsCallback.Unbind();
	Callback.ExecuteIfBound();
}

void UStageAICommandSubsystem::ClearConversation()
{
	if (IsBusy() || Messages.IsEmpty())
	{
		return;
	}
	Messages.Reset();
	PurchaseOffer.Reset();
	Proposal.Reset();
	LastListing.Invalidate();
	OnConversationChanged.Broadcast();
}

FText UStageAICommandSubsystem::GetInterpreterDescription() const
{
	if (!Backend)
	{
		return LOCTEXT("LocalOnly", "Local commands (offline). The online model can be turned on in Project Settings › StageCraft AI Assistant.");
	}
	FText Unavailable;
	return Backend->IsAvailable(Unavailable)
		? FText::Format(LOCTEXT("Online", "{0}: prompts and a stage summary are sent to Anthropic. Every change still goes through the stage's rules."), Backend->GetDisplayName())
		: FText::Format(LOCTEXT("OnlineUnavailable", "Local commands: {0}."), Unavailable);
}

FStageAISceneView UStageAICommandSubsystem::CaptureScene() const
{
	FStageAISceneView Scene;
	UWorld* World = GetWorld();
	Scene.SelectedItemNames = PromptSelection;
	if (const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World))
	{
		const FStageSessionStats& Stats = Session->GetStats();
		Scene.PlacedItems = Stats.PlacedItems;
		Scene.TotalPowerWatts = Stats.TotalPowerWatts;
		Scene.TotalWeightKg = Stats.TotalWeightKg;
		const FStageSessionRules& Rules = Session->GetRules();
		Scene.MaxPlacedItems = Rules.MaxPlacedItems;
		Scene.MaxPowerWatts = Rules.MaxTotalPowerWatts;
		Scene.MaxWeightKg = Rules.MaxTotalWeightKg;
		for (const AModularBaseActor* Item : Session->GetPlacedItems())
		{
			const UBaseItemData* Data = Item ? Item->GetItemData() : nullptr;
			Scene.ItemNames.Add(Data ? Data->DisplayName.ToString() : FString());
		}
	}
	if (const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World))
	{
		for (const ALightingFixtureActor* Fixture : Show->GetFixtures())
		{
			const UBaseItemData* Data = Fixture ? Fixture->GetItemData() : nullptr;
			const FString Name = Data ? Data->DisplayName.ToString() : LOCTEXT("UnnamedFixture", "Fixture").ToString();
			if (TPair<FString, int32>* Entry = Scene.FixturesByName.FindByPredicate([&Name](const TPair<FString, int32>& Existing) { return Existing.Key == Name; }))
			{
				++Entry->Value;
			}
			else
			{
				Scene.FixturesByName.Emplace(Name, 1);
			}
		}
		for (const FShowCue& Cue : Show->GetCues())
		{
			Scene.Cues.Add({ Cue.Number, Cue.Label.ToString(), Cue.FadeTime });
		}
		Scene.ActiveCue = Show->GetActiveCueNumber();
	}
	if (const UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(World))
	{
		Scene.bHasEnvironment = true;
		Scene.bSkyAvailable = Environment->IsSkyAvailable();
		Scene.Environment = Environment->GetSettings();
	}
	// The marketplace's current page (the panel's cards) and the player's credits (Docs/ADR/0021 §13.2).
	if (const UStageMarketplaceSubsystem* Marketplace = GetMarketplace())
	{
		for (const FStageMarketplaceListing& Listing : Marketplace->GetListings())
		{
			Scene.Listings.Add({ Listing, Marketplace->GetListingOwnership(Listing.ListingId) });
		}
		Scene.ListingMatches = Marketplace->GetTotalMatches();
	}
	Scene.LastListing = LastListing;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (const UStageProfileSubsystem* Profile = GameInstance ? GameInstance->GetSubsystem<UStageProfileSubsystem>() : nullptr)
	{
		Scene.Credits = Profile->GetBalance(StageCraftTags::Currency_Credits);
	}
	return Scene;
}

#undef LOCTEXT_NAMESPACE
