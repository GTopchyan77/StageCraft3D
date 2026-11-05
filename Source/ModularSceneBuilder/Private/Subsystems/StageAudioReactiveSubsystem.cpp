// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageAudioReactiveSubsystem.h"

#include "Async/Async.h"
#include "Audio/StageAudioAnalyzer.h"
#include "Audio/StageAudioCaptureSources.h"
#include "Audio/StageAudioFileDecoder.h"
#include "Audio/StageAudioFileSound.h"
#include "Audio/StageAudioTap.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudioReactiveSubsystem)

#define LOCTEXT_NAMESPACE "StageAudioReactive"

namespace StageAudioReactiveSubsystem
{
	/** The tap holds 2.7 s at 48 kHz: the analyser reads it every few milliseconds. */
	constexpr int32 TapCapacitySamples = 1 << 17;

	const TCHAR* SourceName(EStageAudioSource Source)
	{
		switch (Source)
		{
		case EStageAudioSource::Off: return TEXT("Off");
		case EStageAudioSource::File: return TEXT("File");
		case EStageAudioSource::Input: return TEXT("Input");
		case EStageAudioSource::SystemAudio: return TEXT("SystemAudio");
		case EStageAudioSource::TestSignal: return TEXT("TestSignal");
		}
		return TEXT("?");
	}
}

UStageAudioReactiveSubsystem::UStageAudioReactiveSubsystem() = default;
UStageAudioReactiveSubsystem::~UStageAudioReactiveSubsystem() = default;

bool UStageAudioReactiveSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Like the show it will drive: play sessions only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageAudioReactiveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Analyzer = MakeUnique<FStageAudioAnalyzer>();
	Loopback = MakeUnique<FStageLoopbackCapture>();
	Input = MakeUnique<FStageAudioInputCapture>();
}

void UStageAudioReactiveSubsystem::Deinitialize()
{
	StopAll();
	OnAudioStatusChanged.Clear();
	Analyzer.Reset();
	Loopback.Reset();
	Input.Reset();
	TestSettings = nullptr;
	Super::Deinitialize();
}

void UStageAudioReactiveSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// The source chosen last time opens again with this stage (never a file: a song is picked per session).
	const EStageAudioSource Remembered = GetSettings().LastSource;
	if (Remembered != EStageAudioSource::Off && Remembered != EStageAudioSource::File)
	{
		FText Error;
		if (!SetSource(Remembered, Error))
		{
			UE_LOG(LogStageCraft, Log, TEXT("Audio: the remembered source %s did not reopen: %s"), StageAudioReactiveSubsystem::SourceName(Remembered), *Error.ToString());
		}
	}
}

// --- Settings ---

UStageCraftUserSettings* UStageAudioReactiveSubsystem::GetUserSettings() const
{
	return TestSettings ? TestSettings.Get() : UStageCraftUserSettings::Get();
}

FStageAudioReactiveSettings UStageAudioReactiveSubsystem::GetSettings() const
{
	const UStageCraftUserSettings* UserSettings = GetUserSettings();
	return UserSettings ? UserSettings->GetAudioReactiveSettings() : FallbackSettings;
}

void UStageAudioReactiveSubsystem::SaveSettings(const FStageAudioReactiveSettings& Settings)
{
	UStageCraftUserSettings* UserSettings = GetUserSettings();
	if (!UserSettings)
	{
		FallbackSettings = StageAudioReactive::Sanitize(Settings);
		return;
	}
	if (UserSettings->SetAudioReactiveSettings(Settings) && !TestSettings)
	{
		UserSettings->SaveSettings();
	}
}

void UStageAudioReactiveSubsystem::RememberSource(EStageAudioSource Remembered)
{
	FStageAudioReactiveSettings Settings = GetSettings();
	Settings.LastSource = Remembered == EStageAudioSource::File ? EStageAudioSource::Off : Remembered;
	SaveSettings(Settings);
}

#if WITH_DEV_AUTOMATION_TESTS
void UStageAudioReactiveSubsystem::UseSettingsForTests(UStageCraftUserSettings* Settings)
{
	TestSettings = Settings;
}
#endif

bool UStageAudioReactiveSubsystem::ApplySettings(const FStageAudioReactiveSettings& InSettings, FText& OutError)
{
	const FStageAudioReactiveSettings Before = GetSettings();
	FStageAudioReactiveSettings Settings = StageAudioReactive::Sanitize(InSettings);
	Settings.LastSource = Before.LastSource; // the source is chosen with SetSource, not here
	SaveSettings(Settings);
	if (Settings == Before || State == EStageAudioSourceState::Off || State == EStageAudioSourceState::Failed)
	{
		OnAudioStatusChanged.Broadcast();
		return true;
	}

	// Timing and bands live in the analyser; the input device and gain in the capture. Restart what reads them.
	const bool bInputChanged = Settings.InputDeviceId != Before.InputDeviceId || Settings.InputGainDb != Before.InputGainDb;
	if (Source == EStageAudioSource::Input && bInputChanged)
	{
		return SetSource(EStageAudioSource::Input, OutError);
	}
	if (Source == EStageAudioSource::File)
	{
		if (Settings.bLoopFile != Before.bLoopFile && Decoder)
		{
			Decoder->SetLoop(Settings.bLoopFile);
		}
	}
	if (State == EStageAudioSourceState::Running && Tap)
	{
		Analyzer->Shutdown();
		if (!StartAnalyzer(OutError))
		{
			Fail(OutError);
			return false;
		}
	}
	OnAudioStatusChanged.Broadcast();
	return true;
}

// --- Sources ---

bool UStageAudioReactiveSubsystem::SetSource(EStageAudioSource NewSource, FText& OutError)
{
	if (NewSource == EStageAudioSource::File)
	{
		OutError = LOCTEXT("FileNeedsPath", "Choose a music file to play.");
		return false;
	}
	StopAll();
	Source = NewSource;
	bool bOk = true;
	switch (NewSource)
	{
	case EStageAudioSource::Off: break;
	case EStageAudioSource::TestSignal: bOk = StartTestSignal(OutError); break;
	case EStageAudioSource::Input: bOk = StartInput(OutError); break;
	case EStageAudioSource::SystemAudio: bOk = StartSystemAudio(OutError); break;
	case EStageAudioSource::File: break;
	}
	if (!bOk)
	{
		Fail(OutError);
		return false;
	}
	RememberSource(NewSource);
	if (NewSource == EStageAudioSource::Off)
	{
		SetState(EStageAudioSourceState::Off);
	}
	UE_LOG(LogStageCraft, Log, TEXT("Audio: source %s (%s)."), StageAudioReactiveSubsystem::SourceName(Source), *Description);
	return true;
}

bool UStageAudioReactiveSubsystem::OpenFile(const FString& Path, FText& OutError)
{
	if (!FStageAudioFileDecoder::ValidatePath(Path, OutError))
	{
		return false; // nothing changes: the current source keeps running
	}
	StopAll();
	Source = EStageAudioSource::File;
	FilePath = FPaths::ConvertRelativePathToFull(Path);
	if (!StartFile(0.0, /*bPaused*/ false, OutError))
	{
		Fail(OutError);
		return false;
	}
	RememberSource(EStageAudioSource::Off); // a file is not reopened with the next stage, and neither is the source it replaced
	UE_LOG(LogStageCraft, Log, TEXT("Audio: opening \"%s\"."), *FPaths::GetCleanFilename(FilePath));
	return true;
}

void UStageAudioReactiveSubsystem::StopAll()
{
	++Generation; // results still on their way from a source thread are for a source that no longer exists
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StatusTimer);
	}
	if (FileComponent)
	{
		FileComponent->Stop();
		FileComponent->DestroyComponent();
		FileComponent = nullptr;
	}
	FileSound = nullptr; // the mixer may still hold it for a moment; it keeps the decoder and its old tap alive meanwhile
	if (Analyzer)
	{
		Analyzer->Shutdown();
	}
	if (Decoder)
	{
		Decoder->Shutdown();
		Decoder.Reset();
	}
	if (Loopback)
	{
		Loopback->Shutdown();
	}
	if (Input)
	{
		Input->Shutdown();
	}
	Tap.Reset();
	Source = EStageAudioSource::Off;
	State = EStageAudioSourceState::Off;
	LastError = FText::GetEmpty();
	Description.Reset();
	SampleRate = 0;
	Channels = 0;
	bFileEnded = false;
}

bool UStageAudioReactiveSubsystem::StartAnalyzer(FText& OutError)
{
	const FStageAudioReactiveSettings Settings = GetSettings();
	FStageAudioAnalyzerConfig Config;
	Config.Settings = Settings;
	Config.HeardDelaySeconds = StageAudioReactive::GetHeardDelaySeconds(Settings, Source);
	Config.bGenerateTestSignal = Source == EStageAudioSource::TestSignal;
	Config.TestSignal.BPM = Settings.TestSignalBPM;
	return Analyzer->Start(Tap.ToSharedRef(), Config, OutError);
}

bool UStageAudioReactiveSubsystem::StartTestSignal(FText& OutError)
{
	Tap = MakeShared<FStageAudioTap>();
	Tap->Init(StageAudioReactiveSubsystem::TapCapacitySamples, StageAudioAnalysis::SampleRate);
	if (!StartAnalyzer(OutError))
	{
		return false;
	}
	SampleRate = StageAudioAnalysis::SampleRate;
	Channels = 1;
	Description = FString::Printf(TEXT("Test signal, %.0f BPM"), GetSettings().TestSignalBPM);
	SetState(EStageAudioSourceState::Running);
	return true;
}

bool UStageAudioReactiveSubsystem::StartInput(FText& OutError)
{
	const FStageAudioReactiveSettings Settings = GetSettings();
	Tap = MakeShared<FStageAudioTap>();
	if (!Input->Start(Tap.ToSharedRef(), Settings.InputDeviceId, Settings.InputGainDb, Description, OutError))
	{
		return false;
	}
	SampleRate = Tap->GetSampleRate();
	Channels = 0;
	if (!StartAnalyzer(OutError))
	{
		Input->Shutdown();
		return false;
	}
	SetState(EStageAudioSourceState::Running);
	return true;
}

bool UStageAudioReactiveSubsystem::StartSystemAudio(FText& OutError)
{
	Tap = MakeShared<FStageAudioTap>();
	const uint32 ForGeneration = Generation;
	TWeakObjectPtr<UStageAudioReactiveSubsystem> WeakThis(this);
	const bool bStarted = Loopback->Start(Tap.ToSharedRef(),
		[WeakThis, ForGeneration](bool bOk, const FText& Error, const FString& OpenedDescription, int32 Rate, int32 OpenedChannels)
		{
			// On the capture thread: hand the result to the game thread.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, ForGeneration, bOk, Error, OpenedDescription, Rate, OpenedChannels]()
				{
					if (UStageAudioReactiveSubsystem* This = WeakThis.Get())
					{
						This->SampleRate = Rate;
						This->Channels = OpenedChannels;
						This->HandleSystemAudioOpened(ForGeneration, bOk, Error, OpenedDescription);
					}
				});
		}, OutError);
	if (!bStarted)
	{
		return false;
	}
	Description = TEXT("System audio");
	SetState(EStageAudioSourceState::Starting);
	return true;
}

void UStageAudioReactiveSubsystem::HandleSystemAudioOpened(uint32 ForGeneration, bool bOk, const FText& Error, const FString& OpenedDescription)
{
	if (ForGeneration != Generation || Source != EStageAudioSource::SystemAudio)
	{
		return;
	}
	if (!bOk)
	{
		Loopback->Shutdown();
		Fail(Error);
		return;
	}
	Description = OpenedDescription;
	FText AnalyzerError;
	if (!StartAnalyzer(AnalyzerError))
	{
		Loopback->Shutdown();
		Fail(AnalyzerError);
		return;
	}
	SetState(EStageAudioSourceState::Running);
	UE_LOG(LogStageCraft, Log, TEXT("Audio: listening to system audio (%s)."), *Description);
}

// --- File ---

bool UStageAudioReactiveSubsystem::StartFile(double StartSeconds, bool bPaused, FText& OutError)
{
	Decoder = MakeShared<FStageAudioFileDecoder>();
	FileStartSeconds = StartSeconds;
	bStartPaused = bPaused;
	bFilePaused = bPaused;
	bFileEnded = false;
	const uint32 ForGeneration = Generation;
	TWeakObjectPtr<UStageAudioReactiveSubsystem> WeakThis(this);
	const bool bStarted = Decoder->Start(FilePath, StartSeconds, GetSettings().bLoopFile,
		[WeakThis, ForGeneration](bool bOk, const FText& Error, const FStageAudioFileInfo& Info)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, ForGeneration, bOk, Error, Info]()
				{
					if (UStageAudioReactiveSubsystem* This = WeakThis.Get())
					{
						This->HandleFileOpened(ForGeneration, bOk, Error, Info);
					}
				});
		}, OutError);
	if (!bStarted)
	{
		Decoder.Reset();
		return false;
	}
	Description = FPaths::GetCleanFilename(FilePath);
	SetState(EStageAudioSourceState::Starting);
	return true;
}

void UStageAudioReactiveSubsystem::HandleFileOpened(uint32 ForGeneration, bool bOk, const FText& Error, const FStageAudioFileInfo& Info)
{
	if (ForGeneration != Generation || Source != EStageAudioSource::File || !Decoder)
	{
		return;
	}
	if (!bOk)
	{
		Decoder->Shutdown();
		Decoder.Reset();
		Fail(Error);
		return;
	}
	SampleRate = Info.SampleRate;
	Channels = Info.Channels;
	FileDuration = Info.DurationSeconds;
	Description = FString::Printf(TEXT("%s, %d Hz, %d ch"), *FPaths::GetCleanFilename(Info.Path), Info.SampleRate, Info.Channels);

	Tap = MakeShared<FStageAudioTap>();
	Tap->Init(StageAudioReactiveSubsystem::TapCapacitySamples, Info.SampleRate);
	FileSound = NewObject<UStageAudioFileSound>(this);
	FileSound->Setup(Decoder.ToSharedRef(), Tap.ToSharedRef(), Info.SampleRate, Info.Channels);
	FileComponent = UGameplayStatics::CreateSound2D(GetWorld(), FileSound, 1.f, 1.f, 0.f, nullptr, /*bPersistAcrossLevelTransition*/ false, /*bAutoDestroy*/ false);
	if (!FileComponent)
	{
		Decoder->Shutdown();
		Decoder.Reset();
		FileSound = nullptr;
		Fail(LOCTEXT("NoAudioDevice", "Music cannot play: this session has no audio device (started with -nosound?)."));
		return;
	}
	FileComponent->bAllowSpatialization = false;
	FText AnalyzerError;
	if (!StartAnalyzer(AnalyzerError))
	{
		StopAll();
		Source = EStageAudioSource::File;
		Fail(AnalyzerError);
		return;
	}
	FileComponent->Play();
	if (bStartPaused)
	{
		FileComponent->SetPaused(true);
	}
	SetState(EStageAudioSourceState::Running);
	UE_LOG(LogStageCraft, Log, TEXT("Audio: playing %s from %.1f s%s."), *Description, FileStartSeconds, bStartPaused ? TEXT(" (paused)") : TEXT(""));
}

double UStageAudioReactiveSubsystem::GetFilePosition() const
{
	if (Source != EStageAudioSource::File || SampleRate <= 0)
	{
		return 0.0;
	}
	const double Played = FileSound ? double(FileSound->GetFramesPlayed()) / double(SampleRate) : 0.0;
	const double Position = FileStartSeconds + Played;
	return FileDuration > 0.0 ? FMath::Fmod(Position, FileDuration) : Position;
}

bool UStageAudioReactiveSubsystem::Transport(const FStageAudioTransportRequest& Request, FText& OutError)
{
	if (Source != EStageAudioSource::File || FilePath.IsEmpty())
	{
		OutError = LOCTEXT("NoFile", "No music file is open.");
		return false;
	}
	if (State == EStageAudioSourceState::Failed)
	{
		OutError = LastError;
		return false;
	}
	auto Restart = [this, &OutError](double At, bool bPaused)
	{
		const FString Path = FilePath;
		StopAll();
		Source = EStageAudioSource::File;
		FilePath = Path;
		if (!StartFile(At, bPaused, OutError))
		{
			Fail(OutError);
			return false;
		}
		return true;
	};

	switch (Request.Command)
	{
	case EStageAudioTransportCommand::Play:
		if (bFileEnded || (FileComponent && !FileComponent->IsPlaying()))
		{
			return Restart(0.0, false);
		}
		if (FileComponent)
		{
			FileComponent->SetPaused(false);
		}
		bStartPaused = false;
		bFilePaused = false;
		break;
	case EStageAudioTransportCommand::Pause:
		if (FileComponent)
		{
			FileComponent->SetPaused(true);
		}
		bStartPaused = true;
		bFilePaused = true;
		break;
	case EStageAudioTransportCommand::Stop:
		return Restart(0.0, true);
	case EStageAudioTransportCommand::Seek:
	{
		if (!FMath::IsFinite(Request.Seconds) || Request.Seconds < 0.0)
		{
			OutError = LOCTEXT("BadSeek", "That is not a position in the song.");
			return false;
		}
		const double At = FileDuration > 0.0 ? FMath::Min(Request.Seconds, FMath::Max(0.0, FileDuration - 0.01)) : Request.Seconds;
		// Media Foundation seeks the reader; the decoder starts over there, and so does the analysis (its tempo window refills).
		return Restart(At, bFilePaused);
	}
	case EStageAudioTransportCommand::SetLoop:
	{
		FStageAudioReactiveSettings Settings = GetSettings();
		Settings.bLoopFile = Request.bLoop;
		SaveSettings(Settings);
		if (Decoder)
		{
			Decoder->SetLoop(Request.bLoop);
		}
		break;
	}
	}
	OnAudioStatusChanged.Broadcast();
	return true;
}

// --- State ---

void UStageAudioReactiveSubsystem::SetState(EStageAudioSourceState NewState, const FText& Error)
{
	State = NewState;
	LastError = Error;
	UWorld* World = GetWorld();
	const bool bLive = NewState == EStageAudioSourceState::Starting || NewState == EStageAudioSourceState::Running;
	if (World)
	{
		if (bLive && !World->GetTimerManager().IsTimerActive(StatusTimer))
		{
			World->GetTimerManager().SetTimer(StatusTimer, FTimerDelegate::CreateUObject(this, &ThisClass::HandleStatusTimer), StatusIntervalSeconds, /*bLoop*/ true);
		}
		else if (!bLive)
		{
			World->GetTimerManager().ClearTimer(StatusTimer);
		}
	}
	OnAudioStatusChanged.Broadcast();
}

void UStageAudioReactiveSubsystem::Fail(const FText& Error)
{
	const EStageAudioSource Failed = Source;
	StopAll();
	Source = Failed;
	UE_LOG(LogStageCraft, Warning, TEXT("Audio: %s failed: %s"), StageAudioReactiveSubsystem::SourceName(Failed), *Error.ToString());
	SetState(EStageAudioSourceState::Failed, Error);
}

void UStageAudioReactiveSubsystem::HandleStatusTimer()
{
	// A device that went away, a file that ended or broke: the only things that change without a request.
	if (Source == EStageAudioSource::SystemAudio && Loopback && Loopback->HasFailed())
	{
		Fail(Loopback->GetError());
		return;
	}
	if (Source == EStageAudioSource::File && Decoder)
	{
		if (Decoder->HasFailed())
		{
			Fail(LOCTEXT("DecodeFailed", "The music file stopped decoding (damaged, or its drive was removed)."));
			return;
		}
		if (!bFileEnded && Decoder->HasEnded())
		{
			bFileEnded = true;
			UE_LOG(LogStageCraft, Log, TEXT("Audio: %s ended."), *FPaths::GetCleanFilename(FilePath));
		}
	}
	OnAudioStatusChanged.Broadcast();
}

FStageAudioFeatures UStageAudioReactiveSubsystem::GetFeatures()
{
	if (State != EStageAudioSourceState::Running || !Analyzer)
	{
		return FStageAudioFeatures();
	}
	return Analyzer->GetFeatures(FPlatformTime::Seconds());
}

FStageAudioStatus UStageAudioReactiveSubsystem::GetStatus()
{
	FStageAudioStatus Status;
	Status.Source = Source;
	Status.State = State;
	Status.Error = LastError;
	Status.Description = Description;
	Status.SampleRate = SampleRate;
	Status.Channels = Channels;
	if (Source == EStageAudioSource::File)
	{
		Status.PositionSeconds = GetFilePosition();
		Status.DurationSeconds = FileDuration;
		Status.bPaused = bFilePaused;
		Status.bPlaying = State == EStageAudioSourceState::Running && !bFilePaused && !bFileEnded;
		Status.bLoop = GetSettings().bLoopFile;
		Status.Underruns = FileSound ? FileSound->GetUnderruns() : 0;
	}
	if (Tap)
	{
		Status.DroppedSamples = Tap->GetDroppedSamples();
	}
	if (Analyzer && Analyzer->IsRunning())
	{
		Status.AnalyzerCpuPercent = Analyzer->GetCpuPercent();
		Status.AnalysedSeconds = Analyzer->GetAnalysedSeconds();
	}
	Status.Features = GetFeatures();
	return Status;
}

#undef LOCTEXT_NAMESPACE
