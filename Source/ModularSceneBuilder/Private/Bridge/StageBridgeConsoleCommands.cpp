// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the DCC bridge (Docs/ADR/0022-dcc-bridge.md). They call UStageBridgeSubsystem exactly as the
// Send to 3D App panel does, so scripted runs can switch the bridge, export and send from logs. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Bridge/StageSceneExporter.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "UI/StageSendToWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWindow.h"

namespace StageBridgeCommands
{
	UStageBridgeSubsystem* GetBridge(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageBridgeSubsystem* Bridge = GameInstance ? GameInstance->GetSubsystem<UStageBridgeSubsystem>() : nullptr;
		if (!Bridge)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge: no game instance (start PIE or -game first)."));
		}
		return Bridge;
	}

	void Start(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageBridgeSubsystem* Bridge = GetBridge(World))
		{
			FText Error;
			const bool bStarted = Bridge->StartBridge(Error);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Start: %s"), bStarted ? *FString::Printf(TEXT("listening on port %d"), Bridge->GetPort()) : *Error.ToString());
		}
	}

	void Stop(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageBridgeSubsystem* Bridge = GetBridge(World))
		{
			Bridge->StopBridge();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Stop: stopped."));
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (const UStageBridgeSubsystem* Bridge = GetBridge(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Status: %s, port %d, %d apps, %s."), Bridge->IsRunning() ? TEXT("on") : TEXT("off"), Bridge->GetPort(),
				Bridge->GetClients().Num(), Bridge->IsSending() ? TEXT("sending") : TEXT("idle"));
			for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
			{
				UE_LOG(LogStageCraft, Display, TEXT("  client %d: %s %s%s"), Client.ClientId, *StageBridgeProtocol::GetAppDisplayName(Client.App).ToString(), *Client.AppVersion,
					Client.bImportsGlb ? TEXT("") : TEXT(" (cannot import glTF)"));
			}
		}
	}

	/** Exports to Saved/StageCraft/Bridge/Exports/<scene>.glb. */
	void Export(const TArray<FString>& Args, UWorld* World)
	{
		UStageBridgeSubsystem* Bridge = GetBridge(World);
		if (!Bridge || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Bridge.Export <saved scene name>"));
			return;
		}
		const FString SceneName = FString::Join(Args, TEXT(" "));
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Bridge"), TEXT("Exports"),
			SceneName + FStageSceneExporter::FileExtension));
		const FStageBridgeResultInfo Result = Bridge->ExportSceneToFile(SceneName, Path);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Export \"%s\": %s %s"), *SceneName, *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	/** Sends to a client id, or "first" for the first connected app. */
	void Send(const TArray<FString>& Args, UWorld* World)
	{
		UStageBridgeSubsystem* Bridge = GetBridge(World);
		if (!Bridge || Args.Num() < 2)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Bridge.Send <client id | first> <saved scene name>"));
			return;
		}
		int32 ClientId = FCString::Atoi(*Args[0]);
		if (Args[0].Equals(TEXT("first"), ESearchCase::IgnoreCase))
		{
			ClientId = Bridge->GetClients().IsEmpty() ? 0 : Bridge->GetClients()[0].ClientId;
		}
		const FString SceneName = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));
		const FStageBridgeResultInfo Result = Bridge->SendSceneToApp(SceneName, ClientId);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Send \"%s\" to client %d: %s %s"), *SceneName, ClientId, *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	/** Saves the Send to 3D App window (a floating window of its own) to Saved/Screenshots. */
	void Screenshot(const TArray<FString>& Args, UWorld* World)
	{
		for (TObjectIterator<UStageSendToWidget> It; It; ++It)
		{
			const TSharedPtr<SWidget> Widget = It->GetCachedWidget();
			const TSharedPtr<SWindow> Window = Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
			TArray<FColor> Pixels;
			FIntVector Size(0);
			if (!Window.IsValid() || !FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Pixels.IsEmpty())
			{
				continue;
			}
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
			const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("SendTo_%s.png"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
			const bool bSaved = FFileHelper::SaveArrayToFile(Png, *File);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Screenshot: %dx%d, %s %s."), Size.X, Size.Y, bSaved ? TEXT("saved") : TEXT("NOT saved"), *File);
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bridge.Screenshot: the Send to 3D App window is not open."));
	}

	FAutoConsoleCommandWithWorldAndArgs ScreenshotCommand(TEXT("StageCraft.Bridge.Screenshot"), TEXT("Saves the Send to 3D App window to Saved/Screenshots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Screenshot));
	FAutoConsoleCommandWithWorldAndArgs StartCommand(TEXT("StageCraft.Bridge.Start"), TEXT("Lets 3D app add-ons on this computer connect (the panel's switch)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
	FAutoConsoleCommandWithWorldAndArgs StopCommand(TEXT("StageCraft.Bridge.Stop"), TEXT("Closes the bridge and every connection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Stop));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Bridge.Status"), TEXT("Logs whether the bridge is on, its port and the connected apps."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ExportCommand(TEXT("StageCraft.Bridge.Export"), TEXT("<saved scene name> Exports a saved scene to Saved/StageCraft/Bridge/Exports as .glb."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Export));
	FAutoConsoleCommandWithWorldAndArgs SendCommand(TEXT("StageCraft.Bridge.Send"), TEXT("<client id | first> <saved scene name> Sends a saved scene to a connected 3D app."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Send));
}

#endif // !UE_BUILD_SHIPPING
