// Copyright Epic Games, Inc. All Rights Reserved.

#include "Support/StageSupportSubsystem.h"

#include "Async/Async.h"
#include "Cloud/StageCloudJson.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "IO/StageAtomicFile.h"
#include "IO/StageCraftPaths.h"
#include "Misc/App.h"
#include "Misc/OutputDeviceFile.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"
#include "Widgets/SWindow.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSupportSubsystem)

#define LOCTEXT_NAMESPACE "StageSupportSubsystem"

namespace StageSupport
{
	/** Sending a report with a screenshot over a slow line. */
	constexpr float SendTimeoutSeconds = 60.f;

	TArray<uint8> ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}
}

FString UStageSupportSubsystem::GetOfflineDirectory()
{
	return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Support"));
}

TArray<FString> UStageSupportSubsystem::GetSecrets() const
{
	const UStageCloudSubsystem* Cloud = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageCloudSubsystem>() : nullptr;
	return Cloud ? Cloud->GetSecretsToRedact() : TArray<FString>();
}

FString UStageSupportSubsystem::Redact(const FString& Text) const
{
	return StageSupportReport::Redact(Text, FPlatformProcess::UserName(), GetSecrets());
}

FStageSupportReport UStageSupportSubsystem::MakeDraft(const UWorld* World) const
{
	FStageSupportReport Report;
	Report.ReportId = FGuid::NewGuid();
	Report.CreatedUtc = FDateTime::UtcNow();
	Report.AppVersion = FString::Printf(TEXT("%s (%s)"), FApp::GetBuildVersion(), LexToString(FApp::GetBuildConfiguration()));
	Report.Platform = FString::Printf(TEXT("%s %s"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), *FPlatformMisc::GetOSVersion());
	Report.Gpu = GRHIAdapterName;
	if (World)
	{
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World);
		Report.SceneSummary = FString::Printf(TEXT("%d items, %d fixtures"), Session ? Session->GetPlacedItems().Num() : 0, Show ? Show->GetFixtures().Num() : 0);
	}
	return Report;
}

FString UStageSupportSubsystem::ReadLogTail() const
{
	if (GLog)
	{
		GLog->Flush();
	}
	const FString LogFile = FPaths::ConvertRelativePathToFull(FPlatformOutputDevices::GetAbsoluteLogFilename());
	const int64 Size = IFileManager::Get().FileSize(*LogFile);
	if (Size <= 0)
	{
		return FString();
	}
	// Read only the end of the file (the log is open for writing, shared for reading).
	const TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*LogFile, FILEREAD_AllowWrite));
	if (!Reader)
	{
		return FString();
	}
	const int64 Start = FMath::Max<int64>(0, Size - StageSupportReport::MaxLogBytes);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(int32(Size - Start));
	Reader->Seek(Start);
	Reader->Serialize(Bytes.GetData(), Bytes.Num());
	if (Reader->IsError())
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return Redact(StageSupportReport::TailLog(FString::ConstructFromPtrSize(Converted.Get(), Converted.Length())));
}

bool UStageSupportSubsystem::CaptureScreenshot(FOnStageSupportScreenshot OnDone, FText& OutError) const
{
	const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
	const TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
	TArray<FColor> Pixels;
	FIntVector Size(0, 0, 0);
	if (!Window.IsValid() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Pixels.IsEmpty())
	{
		OutError = LOCTEXT("NoCapture", "The window could not be captured.");
		return false;
	}
	// The worker must not load modules; the game thread loads the PNG codec first.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TWeakObjectPtr<const UStageSupportSubsystem> WeakThis = this;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, OnDone = MoveTemp(OnDone), Pixels = MoveTemp(Pixels), Size]() mutable
	{
		const double Start = FPlatformTime::Seconds();
		FStageSupportScreenshot Result;
		if (!EncodeScreenshot(MoveTemp(Pixels), FIntPoint(Size.X, Size.Y), StageSupportReport::MaxScreenshotBytes, Result.Png))
		{
			Result.Error = LOCTEXT("CaptureTooLarge", "The screenshot is too large to attach.");
		}
		UE_LOG(LogStageCraft, Log, TEXT("Support: %dx%d screenshot encoded on a worker in %.0f ms (%d KB)."), Size.X, Size.Y,
			(FPlatformTime::Seconds() - Start) * 1000.0, Result.Png.Num() / 1024);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, OnDone = MoveTemp(OnDone), Result = MoveTemp(Result)]()
		{
			if (WeakThis.IsValid())
			{
				OnDone.ExecuteIfBound(Result);
			}
		});
	});
	return true;
}

bool UStageSupportSubsystem::EncodeScreenshot(TArray<FColor> Pixels, FIntPoint Size, int64 MaxBytes, TArray<uint8>& OutPng)
{
	OutPng.Reset();
	int32 Width = Size.X;
	int32 Height = Size.Y;
	if (Width <= 0 || Height <= 0 || int64(Width) * Height != Pixels.Num())
	{
		return false;
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, Pixels, Png);
		if (Png.Num() > 0 && Png.Num() <= MaxBytes)
		{
			OutPng = TArray<uint8>(Png.GetData(), int32(Png.Num()));
			return true;
		}
		// Too large: half the size (box filter) and try again.
		const int32 HalfWidth = FMath::Max(1, Width / 2);
		const int32 HalfHeight = FMath::Max(1, Height / 2);
		TArray<FColor> Half;
		Half.SetNumUninitialized(HalfWidth * HalfHeight);
		for (int32 Y = 0; Y < HalfHeight; ++Y)
		{
			for (int32 X = 0; X < HalfWidth; ++X)
			{
				const FColor& A = Pixels[(Y * 2) * Width + X * 2];
				const FColor& B = Pixels[(Y * 2) * Width + FMath::Min(X * 2 + 1, Width - 1)];
				const FColor& C = Pixels[FMath::Min(Y * 2 + 1, Height - 1) * Width + X * 2];
				const FColor& D = Pixels[FMath::Min(Y * 2 + 1, Height - 1) * Width + FMath::Min(X * 2 + 1, Width - 1)];
				Half[Y * HalfWidth + X] = FColor(uint8((A.R + B.R + C.R + D.R) / 4), uint8((A.G + B.G + C.G + D.G) / 4), uint8((A.B + B.B + C.B + D.B) / 4), 255);
			}
		}
		Pixels = MoveTemp(Half);
		Width = HalfWidth;
		Height = HalfHeight;
	}
	return false;
}

bool UStageSupportSubsystem::Submit(FStageSupportReport Report, FOnStageSupportSubmitted OnDone, FText& OutError)
{
	if (bSubmitting)
	{
		OutError = LOCTEXT("Busy", "A report is being sent.");
		return false;
	}
	OutError = StageSupportReport::Validate(Report);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	if (!Report.ReportId.IsValid())
	{
		Report.ReportId = FGuid::NewGuid();
	}
	StageSupportReport::RedactReport(Report, FPlatformProcess::UserName(), GetSecrets());
	if (!Report.bIncludeLog)
	{
		Report.LogTail.Reset();
	}
	if (!Report.bIncludeScreenshot)
	{
		Report.ScreenshotPng.Reset();
	}

	UStageCloudSubsystem* Cloud = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageCloudSubsystem>() : nullptr;
	if (!Cloud || !Cloud->IsOnline())
	{
		FStageSupportSubmitResult Result;
		FText SaveError;
		Result.bSaved = SaveOffline(Report, Result.Folder, SaveError);
		Result.Message = Result.bSaved
			? FText::Format(LOCTEXT("SavedOffline", "Saved to {0}. Attach that folder when you write to StageCraft support."), FText::FromString(Result.Folder))
			: SaveError;
		OnDone.ExecuteIfBound(Result);
		return true;
	}

	bSubmitting = true;
	TWeakObjectPtr<UStageSupportSubsystem> WeakThis = this;
	Cloud->Send(TEXT("POST"), TEXT("/v1/support/tickets"), StageSupport::ToUtf8(StageSupportReport::ToJson(Report)), TEXT("application/json"),
		Cloud->IsSignedIn(), StageSupport::SendTimeoutSeconds,
		FOnStageCloudResponse::CreateLambda([WeakThis, Report = MoveTemp(Report), OnDone](const FStageCloudResponse& Response)
		{
			if (UStageSupportSubsystem* Self = WeakThis.Get())
			{
				Self->HandleSent(Response, Report, OnDone);
			}
		}));
	return true;
}

void UStageSupportSubsystem::HandleSent(const FStageCloudResponse& Response, FStageSupportReport Report, FOnStageSupportSubmitted OnDone)
{
	bSubmitting = false;
	FStageSupportSubmitResult Result;
	if (Response.IsSuccess() && StageCloudJson::ParseTicketId(Response.GetBodyAsString(), Result.TicketId))
	{
		Result.bSent = true;
		Result.Message = FText::Format(LOCTEXT("Sent", "Sent. Your reference is {0}."), FText::FromString(Result.TicketId));
		UE_LOG(LogStageCraft, Log, TEXT("Support: report %s sent as ticket %s."), *Report.ReportId.ToString(), *Result.TicketId);
		OnDone.ExecuteIfBound(Result);
		return;
	}
	// Not lost: kept on disk with the reason, for the user to send another way.
	const FStageCloudError Error = StageCloudJson::ParseError(Response);
	FText SaveError;
	Result.bSaved = SaveOffline(Report, Result.Folder, SaveError);
	const FText Why = Error.Message.IsEmpty() ? LOCTEXT("NotReached", "StageCraft Cloud could not be reached") : FText::FromString(Error.Message);
	Result.Message = Result.bSaved
		? FText::Format(LOCTEXT("SendFailedSaved", "Not sent ({0}). Saved to {1} instead."), Why, FText::FromString(Result.Folder))
		: FText::Format(LOCTEXT("SendFailed", "Not sent ({0}), and it could not be saved: {1}"), Why, SaveError);
	UE_LOG(LogStageCraft, Warning, TEXT("Support: report %s not sent (HTTP %d %s)."), *Report.ReportId.ToString(), Error.Status, *Error.Code);
	OnDone.ExecuteIfBound(Result);
}

bool UStageSupportSubsystem::SaveOffline(const FStageSupportReport& Report, FString& OutFolder, FText& OutError) const
{
	const FString Folder = FPaths::Combine(GetOfflineDirectory(), FString::Printf(TEXT("%s_%s"), *Report.CreatedUtc.ToString(TEXT("%Y%m%d-%H%M%S")),
		*Report.ReportId.ToString(EGuidFormats::Digits).Left(8)));
	bool bOk = IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	bOk = bOk && StageAtomicFile::WriteText(FPaths::Combine(Folder, TEXT("report.txt")), StageSupportReport::ToText(Report));
	bOk = bOk && StageAtomicFile::WriteText(FPaths::Combine(Folder, TEXT("report.json")), StageSupportReport::ToJson(Report));
	if (bOk && Report.bIncludeLog && !Report.LogTail.IsEmpty())
	{
		bOk = StageAtomicFile::WriteText(FPaths::Combine(Folder, TEXT("log.txt")), Report.LogTail);
	}
	if (bOk && Report.bIncludeScreenshot && StageSupportReport::IsPng(Report.ScreenshotPng))
	{
		bOk = StageAtomicFile::WriteBytes(FPaths::Combine(Folder, TEXT("screenshot.png")), TConstArrayView64<uint8>(Report.ScreenshotPng.GetData(), Report.ScreenshotPng.Num()));
	}
	if (!bOk)
	{
		OutError = LOCTEXT("SaveFailed", "The report could not be saved on this computer.");
		return false;
	}
	OutFolder = FPaths::ConvertRelativePathToFull(Folder);
	return true;
}

#undef LOCTEXT_NAMESPACE
