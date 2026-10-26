// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "AI/StageAIAssistantBackend.h"
#include "AI/StageAIAssistantSettings.h"
#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSchema.h"
#include "Data/BaseItemData.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

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
	PendingExecutor.Unbind();
	State = EStageAIAssistantState::Idle;
	OnConversationChanged.Clear();
	Super::Deinitialize();
}

void UStageAICommandSubsystem::AddMessage(EStageAISpeaker Speaker, EStageAIOutcome Outcome, const FText& Text)
{
	FStageAIMessage& Message = Messages.AddDefaulted_GetRef();
	Message.Speaker = Speaker;
	Message.Outcome = Outcome;
	Message.Text = Text;
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
		PendingExecutor.Unbind();
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
	const FString Trimmed = Prompt.TrimStartAndEnd().Left(StageAICommandParser::MaxPromptLength);
	if (Trimmed.IsEmpty() || IsBusy())
	{
		return false;
	}

	// The history the model sees is the conversation before this prompt.
	FStageAIBackendRequest Request;
	const int32 MaxHistory = GetDefault<UStageAIAssistantSettings>()->GetMaxHistoryMessages();
	for (int32 Index = FMath::Max(0, Messages.Num() - MaxHistory); Index < Messages.Num(); ++Index)
	{
		Request.History.Add(Messages[Index]);
	}

	AddMessage(EStageAISpeaker::User, EStageAIOutcome::Info, FText::FromString(Trimmed));
	PendingExecutor = MoveTemp(Executor);
	PromptSelection = MoveTemp(SelectedItemNames);

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
	return Command.IsSet()
		? FStageAIInterpretation::MakeCommand(*Command)
		: FStageAIInterpretation::MakeReply(StageAIAnswers::DescribeNotUnderstood(Prompt));
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
	SetState(EStageAIAssistantState::Idle);
	Respond(Interpretation);
}

void UStageAICommandSubsystem::Respond(const FStageAIInterpretation& Interpretation)
{
	if (Interpretation.Kind == FStageAIInterpretation::EKind::Reply)
	{
		AddMessage(EStageAISpeaker::Assistant, Interpretation.ReplyOutcome, WithNote(Interpretation.Reply, Interpretation.Note));
		PendingExecutor.Unbind();
		return;
	}

	FStageAICommand Command = Interpretation.Command;
	FText Error;
	if (!StageAICommandSchema::Validate(Command, Error))
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, WithNote(Error, Interpretation.Note));
		PendingExecutor.Unbind();
		return;
	}
	UE_LOG(LogStageCraft, Log, TEXT("AI assistant: running %s."), *StageAICommandSchema::GetIntentName(Command.Intent));

	if (Command.Intent == EStageAIIntent::Help)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::DescribeHelp(Command.Language), Interpretation.Note));
	}
	else if (Command.Intent == EStageAIIntent::Unsupported)
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::DescribeUnsupported(Command.ItemQuery, Command.Language), Interpretation.Note));
	}
	else if (Command.IsQuestion())
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Info, WithNote(StageAIAnswers::AnswerQuestion(Command, CaptureScene()), Interpretation.Note));
	}
	else if (!PendingExecutor.IsBound())
	{
		AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Refused, StageAILanguage::Pick(Command.Language,
			LOCTEXT("NoPlayer", "There is no player to make that change for."), INVTEXT("Չկա խաղացող, որի համար կատարեմ այդ փոփոխությունը։")));
	}
	else
	{
		// Acting while the executor runs, so a pending action may stream assets (RequestItemAssets).
		State = EStageAIAssistantState::Acting;
		const FStageAIActionOutcome Outcome = PendingExecutor.Execute(Command);
		if (Outcome.Outcome == EStageAIOutcome::Pending && State == EStageAIAssistantState::Acting)
		{
			AddMessage(EStageAISpeaker::Assistant, EStageAIOutcome::Pending, WithNote(Outcome.Message, Interpretation.Note));
			return;
		}
		if (State == EStageAIAssistantState::Acting)
		{
			State = EStageAIAssistantState::Idle;
			AddMessage(EStageAISpeaker::Assistant, Outcome.Outcome, WithNote(Outcome.Message, Interpretation.Note));
		}
	}
	SetState(EStageAIAssistantState::Idle);
}

void UStageAICommandSubsystem::FinishPendingAction(const FStageAIActionOutcome& Outcome)
{
	if (State != EStageAIAssistantState::Acting)
	{
		return;
	}
	State = EStageAIAssistantState::Idle;
	PendingExecutor.Unbind();
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
	return Scene;
}

#undef LOCTEXT_NAMESPACE
