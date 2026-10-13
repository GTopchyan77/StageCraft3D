// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "NGGVideoRecorderFrameCapture.h"
#include "NGGVideoRecorderLog.h"
#include "NGGVideoRecorderPanel.h"
#include "NGGVideoRecorderSettings.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "NGGVideoRecorder"

/**
 * Watches for the toggle key at the Slate level, so recording can be started in a packaged build without
 * the game having to bind anything.
 */
class FNGGRecorderHotkeyProcessor : public IInputProcessor
{
public:
	explicit FNGGRecorderHotkeyProcessor(UNGGVideoRecorderSubsystem* InOwner)
		: Owner(InOwner)
	{
	}

	virtual void Tick(const float /*DeltaTime*/, FSlateApplication& /*SlateApp*/, TSharedRef<ICursor> /*Cursor*/) override
	{
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& /*SlateApp*/, const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.IsRepeat())
		{
			return false;
		}

		const UNGGVideoRecorderSettings& Settings = UNGGVideoRecorderSettings::Get();
		UNGGVideoRecorderSubsystem* Subsystem = Owner.Get();
		if (!Subsystem || !Settings.bEnableToggleHotkey)
		{
			return false;
		}

		if (Settings.ToggleRecordingKey.IsValid() && InKeyEvent.GetKey() == Settings.ToggleRecordingKey)
		{
			Subsystem->ToggleRecording();
			return true;
		}

		if (Settings.ToggleUIKey.IsValid() && InKeyEvent.GetKey() == Settings.ToggleUIKey)
		{
			Subsystem->ToggleRecorderUI();
			return true;
		}

		return false;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("NGGVideoRecorderHotkey");
	}

private:
	TWeakObjectPtr<UNGGVideoRecorderSubsystem> Owner;
};

namespace NGGVideoRecorderConsole
{
	static UNGGVideoRecorderSubsystem* Find(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		UGameInstance* GameInstance = World->GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UNGGVideoRecorderSubsystem>() : nullptr;
	}

	static FAutoConsoleCommandWithWorldAndArgs StartCommand(
		TEXT("NGGRecorder.Start"),
		TEXT("Start recording the viewport to an MP4. Optional argument: file name without extension."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			UNGGVideoRecorderSubsystem* Recorder = Find(World);
			if (!Recorder)
			{
				UE_LOG(LogNGGVideoRecorder, Warning, TEXT("No recorder available; is a game world running?"));
				return;
			}

			FNGGVideoRecorderOptions Options = UNGGVideoRecorderSettings::Get().DefaultOptions;
			if (Args.Num() > 0)
			{
				Options.FileName = Args[0];
			}

			FString Path;
			FString Error;
			if (Recorder->StartRecording(Options, Path, Error))
			{
				UE_LOG(LogNGGVideoRecorder, Display, TEXT("Recording to %s"), *Path);
			}
			else
			{
				UE_LOG(LogNGGVideoRecorder, Error, TEXT("Could not start recording: %s"), *Error);
			}
		}));

	/** Wraps a no-argument subsystem method as a console command body. */
	template <typename RetType>
	static FConsoleCommandWithWorldAndArgsDelegate Call(RetType (UNGGVideoRecorderSubsystem::*Method)())
	{
		return FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[Method](const TArray<FString>&, UWorld* World)
			{
				if (UNGGVideoRecorderSubsystem* Recorder = Find(World))
				{
					(Recorder->*Method)();
				}
			});
	}

	static FAutoConsoleCommandWithWorldAndArgs StopCommand(
		TEXT("NGGRecorder.Stop"),
		TEXT("Stop the current recording and finalise the MP4."),
		Call(&UNGGVideoRecorderSubsystem::StopRecording));

	static FAutoConsoleCommandWithWorldAndArgs ToggleCommand(
		TEXT("NGGRecorder.Toggle"),
		TEXT("Start recording if idle, stop it otherwise."),
		Call(&UNGGVideoRecorderSubsystem::ToggleRecording));

	static FAutoConsoleCommandWithWorldAndArgs ToggleUICommand(
		TEXT("NGGRecorder.ToggleUI"),
		TEXT("Show or hide the recorder control panel."),
		Call(&UNGGVideoRecorderSubsystem::ToggleRecorderUI));

	static FAutoConsoleCommandWithWorldAndArgs StatusCommand(
		TEXT("NGGRecorder.Status"),
		TEXT("Print the state of the video recorder."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>&, UWorld* World)
		{
			UNGGVideoRecorderSubsystem* Recorder = Find(World);
			if (!Recorder)
			{
				UE_LOG(LogNGGVideoRecorder, Display, TEXT("No recorder available."));
				return;
			}

			const FNGGVideoRecorderStats Stats = Recorder->GetStats();
			UE_LOG(LogNGGVideoRecorder, Display,
				TEXT("%s | %dx%d @ %d fps | %.1f s | %d frames written, %d dropped | %s"),
				Stats.bIsRecording ? TEXT("RECORDING") : TEXT("idle"),
				Stats.Resolution.X, Stats.Resolution.Y, Stats.FrameRate,
				Stats.ElapsedSeconds, Stats.FramesCaptured, Stats.FramesDropped,
				Stats.FilePath.IsEmpty() ? TEXT("(no file yet)") : *Stats.FilePath);
		}));
}

bool UNGGVideoRecorderSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Nothing to capture without a rendered window.
	return !IsRunningDedicatedServer() && !IsRunningCommandlet() && FSlateApplication::IsInitialized();
}

void UNGGVideoRecorderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (FSlateApplication::IsInitialized())
	{
		HotkeyProcessor = MakeShared<FNGGRecorderHotkeyProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(HotkeyProcessor);
	}

	bWantsRecorderUI = UNGGVideoRecorderSettings::Get().bShowRecorderUIOnStartup;

	// The game viewport does not exist yet this early, so the startup panel waits for it on a
	// self-removing ticker (see TickAutoShowUI for why no delegate covers this in PIE). Map loads
	// wipe the viewport's widgets, so PostLoadMapWithWorld re-adds the panel after travel.
	if (bWantsRecorderUI)
	{
		AutoShowUITickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UNGGVideoRecorderSubsystem::TickAutoShowUI), 0.0f);
	}
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &UNGGVideoRecorderSubsystem::HandlePostLoadMap);
}

void UNGGVideoRecorderSubsystem::Deinitialize()
{
	// A half written MP4 is unplayable, so always close the file even on an abrupt shutdown.
	if (IsRecording() || bStartPending)
	{
		StopRecording();
	}

	if (AutoShowUITickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(AutoShowUITickHandle);
		AutoShowUITickHandle.Reset();
	}

	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	PostLoadMapHandle.Reset();

	HideRecorderUI();

	if (HotkeyProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(HotkeyProcessor);
		}
		HotkeyProcessor.Reset();
	}

	Super::Deinitialize();
}

bool UNGGVideoRecorderSubsystem::StartRecording(const FNGGVideoRecorderOptions& Options, FString& OutFilePath, FString& OutError)
{
	OutFilePath.Reset();
	OutError.Reset();

	if (IsRecording() || bStartPending)
	{
		OutFilePath = bStartPending ? PendingFilePath : CurrentFilePath;
		OutError = TEXT("A recording is already running.");
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient)
	{
		OutError = TEXT("There is no game viewport to record.");
		return false;
	}

	const FString FilePath = BuildOutputPath(Options);
	OutFilePath = FilePath;

	// Announce the start before touching the capture at all, and defer the actual capture hook
	// (FNGGRecorderFrameCapture::Start, which just registers an OnBackBufferReadyToPresent callback for
	// FUTURE frames) by one full engine tick. Anything that hides itself in response to
	// OnRecordingWillStart is guaranteed a full render+present cycle to actually disappear from the
	// screen before the hook goes live, so the very first captured frame is already clean — hiding and
	// installing the hook in the same tick would otherwise race, since Slate visibility only takes
	// effect on the next paint, which the current frame's own pending present might predate.
	OnRecordingWillStart.Broadcast(FilePath);

	bStartPending = true;
	PendingOptions = Options;
	PendingFilePath = FilePath;
	PendingRequestFrame = GFrameCounter;
	DeferredStartTickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UNGGVideoRecorderSubsystem::TickDeferredStart), 0.0f);

	return true;
}

bool UNGGVideoRecorderSubsystem::TickDeferredStart(float /*DeltaTime*/)
{
	// A ticker added with a 0.0f delay only means "as soon as possible" — if it was added before the core
	// ticker's own dispatch point ran this frame, it can fire again THIS SAME frame rather than the next
	// one (confirmed happening in practice when the request comes from the F9 hotkey's Slate input
	// handler, as opposed to a console command, which happens to land with more margin). Keep re-arming
	// until GFrameCounter has actually moved past the request frame, so listeners that hid themselves in
	// response to OnRecordingWillStart are always guaranteed a real render+present cycle first.
	if (GFrameCounter <= PendingRequestFrame)
	{
		return true;
	}

	bStartPending = false;

	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient)
	{
		CurrentFilePath = PendingFilePath;
		FinishRecording(false, TEXT("There is no game viewport to record."));
		return false;
	}

	FString Error;
	TSharedPtr<FNGGRecorderFrameCapture> NewCapture = MakeShared<FNGGRecorderFrameCapture>();
	if (!NewCapture->Start(PendingOptions, ViewportClient, PendingFilePath, Error))
	{
		// Start cleans up after itself; drop the partial file so a failed attempt leaves nothing behind.
		IFileManager::Get().Delete(*PendingFilePath, false, true, true);
		CurrentFilePath = PendingFilePath;
		FinishRecording(false, Error);
		return false;
	}

	Capture = NewCapture;
	CurrentFilePath = PendingFilePath;
	RecordingStartTime = FPlatformTime::Seconds();

	if (PendingOptions.bLockToFixedFrameRate)
	{
		ApplyFixedTimeStep(PendingOptions.FrameRate);
	}

	IndicatorTickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UNGGVideoRecorderSubsystem::TickIndicator), 0.0f);

	if (UNGGVideoRecorderSettings::Get().bLogRecordingEvents)
	{
		UE_LOG(LogNGGVideoRecorder, Display, TEXT("Recording started: %dx%d @ %d fps -> %s"),
			Capture->GetResolution().X, Capture->GetResolution().Y, Capture->GetFrameRate(), *CurrentFilePath);
	}

	OnRecordingStarted.Broadcast(CurrentFilePath);
	return false; // one-shot
}

bool UNGGVideoRecorderSubsystem::StartRecordingWithDefaults(FString& OutFilePath, FString& OutError)
{
	return StartRecording(UNGGVideoRecorderSettings::Get().DefaultOptions, OutFilePath, OutError);
}

void UNGGVideoRecorderSubsystem::StopRecording()
{
	if (bStartPending)
	{
		// The capture hook was never actually installed yet (still waiting out the one-tick deferral from
		// StartRecording) — cancel it outright rather than letting TickDeferredStart start it a moment
		// after Stop was pressed. Still broadcast OnRecordingFinished so anything that hid itself in
		// response to OnRecordingWillStart knows to show itself again.
		bStartPending = false;
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredStartTickHandle);
		DeferredStartTickHandle.Reset();
		IFileManager::Get().Delete(*PendingFilePath, false, true, true);
		CurrentFilePath = PendingFilePath;
		FinishRecording(false, TEXT("Recording cancelled before capture started."));
		return;
	}

	if (!Capture.IsValid())
	{
		return;
	}

	const float Elapsed = GetRecordingDuration();

	FString Error;
	const bool bSuccess = Capture->Stop(Error);

	// Snapshot after the flush so the counts include the frames drained during shutdown, but before the
	// capture is released, so GetStats keeps reporting the finished recording.
	CachedStats = GetStats();
	CachedStats.bIsRecording = false;
	CachedStats.ElapsedSeconds = Elapsed;

	Capture.Reset();
	FinishRecording(bSuccess, Error);
}

bool UNGGVideoRecorderSubsystem::ToggleRecording()
{
	if (IsRecording())
	{
		StopRecording();
		return false;
	}

	FString Path;
	FString Error;
	if (!StartRecordingWithDefaults(Path, Error))
	{
		UE_LOG(LogNGGVideoRecorder, Error, TEXT("Could not start recording: %s"), *Error);
		return false;
	}
	return true;
}

bool UNGGVideoRecorderSubsystem::IsRecording() const
{
	return Capture.IsValid() && Capture->IsActive();
}

float UNGGVideoRecorderSubsystem::GetRecordingDuration() const
{
	if (!IsRecording())
	{
		return 0.0f;
	}
	return (float)(FPlatformTime::Seconds() - RecordingStartTime);
}

FString UNGGVideoRecorderSubsystem::GetRecordingTimecode() const
{
	const float Elapsed = GetRecordingDuration();
	return FString::Printf(TEXT("%02d:%02d"), (int32)(Elapsed / 60.0f), (int32)Elapsed % 60);
}

FString UNGGVideoRecorderSubsystem::GetLastRecordingPath() const
{
	return CurrentFilePath;
}

FNGGVideoRecorderStats UNGGVideoRecorderSubsystem::GetStats() const
{
	if (!Capture.IsValid())
	{
		return CachedStats;
	}

	FNGGVideoRecorderStats Stats;
	Stats.bIsRecording = Capture->IsActive();
	Stats.ElapsedSeconds = GetRecordingDuration();
	Stats.FramesCaptured = Capture->GetFramesCaptured();
	Stats.FramesDropped = Capture->GetFramesDropped();
	Stats.Resolution = Capture->GetResolution();
	Stats.FrameRate = Capture->GetFrameRate();
	Stats.FilePath = CurrentFilePath;
	return Stats;
}

void UNGGVideoRecorderSubsystem::RevealLastRecordingInExplorer() const
{
	if (CurrentFilePath.IsEmpty())
	{
		return;
	}
	FPlatformProcess::ExploreFolder(*FPaths::GetPath(CurrentFilePath));
}

void UNGGVideoRecorderSubsystem::ShowRecorderUI()
{
	bWantsRecorderUI = true;

	if (PanelWidget.IsValid())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient)
	{
		// Normal before the first map load: HandlePostLoadMap shows the panel once the viewport exists.
		return;
	}

	PanelWidget = SNew(SNGGRecorderPanel, this);
	ViewportClient->AddViewportWidgetContent(PanelWidget.ToSharedRef(), 100);

	// The panel is useless without a cursor to click it with. Remember which controller we turned it
	// on for, so closing the panel restores that controller and no other.
	if (APlayerController* PlayerController = GameInstance->GetFirstLocalPlayerController())
	{
		if (!PlayerController->ShouldShowMouseCursor())
		{
			PlayerController->SetShowMouseCursor(true);
			CursorForcedPC = PlayerController;
		}
	}

	UE_LOG(LogNGGVideoRecorder, Log, TEXT("Recorder panel shown."));
}

void UNGGVideoRecorderSubsystem::HideRecorderUI()
{
	bWantsRecorderUI = false;

	if (!PanelWidget.IsValid())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	if (UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr)
	{
		ViewportClient->RemoveViewportWidgetContent(PanelWidget.ToSharedRef());
	}
	PanelWidget.Reset();

	// Only put the cursor back if it is still the state this panel set; if the game changed cursor
	// visibility (or the controller) in the meantime, that choice wins.
	if (APlayerController* PlayerController = CursorForcedPC.Get())
	{
		if (PlayerController->ShouldShowMouseCursor())
		{
			PlayerController->SetShowMouseCursor(false);
		}
	}
	CursorForcedPC.Reset();
}

bool UNGGVideoRecorderSubsystem::ToggleRecorderUI()
{
	if (IsRecorderUIVisible())
	{
		HideRecorderUI();
		return false;
	}
	ShowRecorderUI();
	return IsRecorderUIVisible();
}

bool UNGGVideoRecorderSubsystem::IsRecorderUIVisible() const
{
	return PanelWidget.IsValid();
}

bool UNGGVideoRecorderSubsystem::TickAutoShowUI(float /*DeltaTime*/)
{
	if (!bWantsRecorderUI)
	{
		// Something (F10, Blueprint) already decided before startup finished; nothing to do.
		AutoShowUITickHandle.Reset();
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || !GameInstance->GetGameViewportClient() || !GameInstance->GetFirstLocalPlayerController())
	{
		// Keep waiting; the viewport and player appear a few frames into startup.
		return true;
	}

	AutoShowUITickHandle.Reset();
	ShowRecorderUI();
	return false;
}

void UNGGVideoRecorderSubsystem::HandlePostLoadMap(UWorld* /*LoadedWorld*/)
{
	// LoadMap cleared the viewport's widgets, so any panel we still hold is a stale reference.
	PanelWidget.Reset();
	CursorForcedPC.Reset();

	if (bWantsRecorderUI)
	{
		ShowRecorderUI();
	}
}

FString UNGGVideoRecorderSubsystem::BuildOutputPath(const FNGGVideoRecorderOptions& Options) const
{
	FString Directory = Options.OutputDirectory;
	if (Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("VideoCaptures");
	}
	// Only a user-supplied relative OutputDirectory needs combining with the project root here — the
	// ProjectSavedDir() fallback above is already correctly anchored on its own. In the editor/PIE,
	// ProjectDir()/ProjectSavedDir() are absolute, so combining a second time was harmless (IsRelative
	// was already false); in a packaged build both are relative ("../../../<Project>/..."), so combining
	// ProjectDir() into an already-ProjectSavedDir()-derived path doubled that relative prefix and
	// resolved to a bogus sibling directory instead of the real Saved folder. Must be "else if", not "if".
	else if (FPaths::IsRelative(Directory))
	{
		Directory = FPaths::Combine(FPaths::ProjectDir(), Directory);
	}
	Directory = FPaths::ConvertRelativePathToFull(Directory);

	FString Name = Options.FileName;
	if (Name.IsEmpty())
	{
		Name = TEXT("{project}_{map}_{date}_{time}");
	}

	FString MapName = TEXT("Level");
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (const UWorld* World = GameInstance->GetWorld())
		{
			MapName = World->GetMapName();
			// In PIE the map name carries a "UEDPIE_0_" style prefix that nobody wants in a file name.
			MapName.RemoveFromStart(World->StreamingLevelsPrefix);
		}
	}

	const FDateTime Now = FDateTime::Now();
	Name = Name.Replace(TEXT("{project}"), FApp::GetProjectName());
	Name = Name.Replace(TEXT("{map}"), *MapName);
	Name = Name.Replace(TEXT("{date}"), *Now.ToString(TEXT("%Y-%m-%d")));
	Name = Name.Replace(TEXT("{time}"), *Now.ToString(TEXT("%H-%M-%S")));
	Name = FPaths::MakeValidFileName(Name);

	IFileManager::Get().MakeDirectory(*Directory, true);

	FString Path = FPaths::Combine(Directory, Name + TEXT(".mp4"));
	for (int32 Suffix = 1; IFileManager::Get().FileExists(*Path) && Suffix < 1000; ++Suffix)
	{
		Path = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%03d.mp4"), *Name, Suffix));
	}
	return Path;
}

void UNGGVideoRecorderSubsystem::ApplyFixedTimeStep(int32 InFrameRate)
{
	if (bFixedTimeStepApplied)
	{
		return;
	}

	bPreviousUseFixedTimeStep = FApp::UseFixedTimeStep();
	PreviousFixedDeltaTime = FApp::GetFixedDeltaTime();

	FApp::SetFixedDeltaTime(1.0 / (double)FMath::Max(1, InFrameRate));
	FApp::SetUseFixedTimeStep(true);

	bFixedTimeStepApplied = true;

	UE_LOG(LogNGGVideoRecorder, Display,
		TEXT("Locked to a fixed time step of 1/%d s. The game no longer runs in real time until recording stops."),
		InFrameRate);
}

void UNGGVideoRecorderSubsystem::RestoreFixedTimeStep()
{
	if (!bFixedTimeStepApplied)
	{
		return;
	}

	FApp::SetUseFixedTimeStep(bPreviousUseFixedTimeStep);
	FApp::SetFixedDeltaTime(PreviousFixedDeltaTime);
	bFixedTimeStepApplied = false;
}

bool UNGGVideoRecorderSubsystem::TickIndicator(float /*DeltaTime*/)
{
	if (!Capture.IsValid())
	{
		return false;
	}

	// An encoder failure only surfaces on its own thread, so close the recording down from here.
	if (Capture->HasFailed())
	{
		UE_LOG(LogNGGVideoRecorder, Error, TEXT("Stopping the recording because the encoder failed."));
		StopRecording();
		return false;
	}

	if (GEngine && UNGGVideoRecorderSettings::Get().bShowOnScreenIndicator)
	{
		// Note this overlay is part of the presented image, so it ends up in the video too.
		GEngine->AddOnScreenDebugMessage(
			(uint64)0x4E474752, 1.1f, FColor::Red,
			FString::Printf(TEXT("REC  %s"), *GetRecordingTimecode()));
	}

	return true;
}

void UNGGVideoRecorderSubsystem::FinishRecording(bool bSuccess, const FString& Error)
{
	RestoreFixedTimeStep();

	if (IndicatorTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(IndicatorTickHandle);
		IndicatorTickHandle.Reset();
	}

	if (UNGGVideoRecorderSettings::Get().bLogRecordingEvents)
	{
		if (bSuccess)
		{
			UE_LOG(LogNGGVideoRecorder, Display, TEXT("Recording finished: %s (%d frames, %d dropped)"),
				*CurrentFilePath, CachedStats.FramesCaptured, CachedStats.FramesDropped);
		}
		else
		{
			UE_LOG(LogNGGVideoRecorder, Error, TEXT("Recording failed: %s"), *Error);
		}
	}

	RecordingStartTime = 0.0;

	OnRecordingFinished.Broadcast(bSuccess, CurrentFilePath, Error);
}

#undef LOCTEXT_NAMESPACE
