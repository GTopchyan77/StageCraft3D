// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for runtime model import (Docs/ADR/0006-inspector-polish-and-runtime-model-import.md):
// import through the controller's request, list what was imported, and drive a real OS-style file drop through Slate
// (the same entry points Windows' drag and drop calls) for scripted verification. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Data/StageImportedItemData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Import/StageMeshImportSubsystem.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Widgets/SWindow.h"

namespace StageImportCommands
{
	/** Arguments joined with spaces, then split on '|': paths may contain spaces, several files are separated by '|'. */
	TArray<FString> ParseFiles(const TArray<FString>& Args, int32 FirstArg)
	{
		TArray<FString> Remaining;
		for (int32 Index = FirstArg; Index < Args.Num(); ++Index)
		{
			Remaining.Add(Args[Index]);
		}
		TArray<FString> Files;
		FString::Join(Remaining, TEXT(" ")).ParseIntoArray(Files, TEXT("|"));
		for (FString& File : Files)
		{
			File.TrimStartAndEndInline();
			File.TrimQuotesInline();
		}
		return Files;
	}

	void File(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		const TArray<FString> Files = ParseFiles(Args, 0);
		const int32 Queued = Controller ? Controller->RequestImportModelFiles(Files) : 0;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.File: %d of %d file(s) queued."), Queued, Files.Num());
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UStageMeshImportSubsystem* Importer = GameInstance ? GameInstance->GetSubsystem<UStageMeshImportSubsystem>() : nullptr;
		if (!Importer)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.Status: no importer."));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.Status: formats [%s], %d pending, %d imported, list %s."),
			*FString::Join(Importer->GetSupportedExtensions(), TEXT(", ")), Importer->GetPendingCount(), Importer->GetImportedItems().Num(),
			*UStageMeshImportSubsystem::GetImportListPath());
		for (const UStageImportedItemData* Item : Importer->GetImportedItems())
		{
			if (Item)
			{
				const UStaticMesh* Mesh = Item->Mesh.Get();
				const FBox Bounds = Mesh ? Mesh->GetBoundingBox() : FBox(ForceInit);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.Status:     bounds %s, %d triangles, %d material slot(s)"), *Bounds.ToString(),
					Mesh ? Mesh->GetNumTriangles(0) : 0, Mesh ? Mesh->GetStaticMaterials().Num() : 0);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.Status:     %s \"%s\" (%s) mesh %s, from %s"), *Item->GetPathName(),
					*Item->DisplayName.ToString(), *Item->GetCatalogSubtitle().ToString(), *Item->Mesh.ToString(), *Item->GetSourceFile());
			}
		}
	}

	/** The OS window of the main app window, and the screen point at a fraction of its client area. */
	TSharedPtr<SWindow> FindMainWindow()
	{
		return GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr;
	}

	/**
	 * Moves the OS cursor to (X, Y) as fractions of the main window and starts dragging files over it, exactly as Windows'
	 * drop target does (FSlateApplication::OnDragEnterFiles + OnDragOver). The drop hint should appear over the panel there.
	 */
	void DragEnter(const TArray<FString>& Args, UWorld* World)
	{
		const TSharedPtr<SWindow> Window = FindMainWindow();
		if (Args.Num() < 3 || !Window.IsValid() || !Window->GetNativeWindow().IsValid())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.DragEnter <x 0-1> <y 0-1> <file>[|<file>...]: no window or too few arguments."));
			return;
		}
		const FVector2D Fraction(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]));
		const FVector2D Point = Window->GetPositionInScreen() + Window->GetSizeInScreen() * Fraction;
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.SetCursorPos(Point);
		const TArray<FString> Files = ParseFiles(Args, 2);
		const EDropEffect::Type Enter = Slate.OnDragEnterFiles(Window->GetNativeWindow().ToSharedRef(), Files);
		// Slate re-evaluates the hover when the cursor moves, as it does during a real drag.
		Slate.SetCursorPos(Point + FVector2D(1.0, 1.0));
		const EDropEffect::Type Over = Slate.OnDragOver(Window->GetNativeWindow());
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.DragEnter: %d file(s) at (%.0f, %.0f), enter effect %d, over effect %d."),
			Files.Num(), Point.X, Point.Y, static_cast<int32>(Enter), static_cast<int32>(Over));
	}

	/** Drops what DragEnter started, as releasing the mouse button over the window does. */
	void DragDrop(const TArray<FString>& Args, UWorld* World)
	{
		const TSharedPtr<SWindow> Window = FindMainWindow();
		if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
		{
			return;
		}
		const EDropEffect::Type Effect = FSlateApplication::Get().OnDragDrop(Window->GetNativeWindow());
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.DragDrop: drop effect %d."), static_cast<int32>(Effect));
	}

	/** Writes every thumbnail to Saved/StageCraft/Thumbnails as PNG, to check them outside the app. */
	void SaveThumbnails(const TArray<FString>& Args, UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UStageMeshImportSubsystem* Importer = GameInstance ? GameInstance->GetSubsystem<UStageMeshImportSubsystem>() : nullptr;
		if (!Importer)
		{
			return;
		}
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("StageCraft") / TEXT("Thumbnails");
		for (const UStageImportedItemData* Item : Importer->GetImportedItems())
		{
			UTextureRenderTarget2D* Thumbnail = Item ? Cast<UTextureRenderTarget2D>(Item->GetCatalogIcon()) : nullptr;
			if (!Thumbnail)
			{
				continue;
			}
			const FString Path = Directory / (Item->GetName() + TEXT(".png"));
			TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*Path));
			const bool bSaved = File.IsValid() && FImageUtils::ExportRenderTarget2DAsPNG(Thumbnail, *File);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Import.SaveThumbnails: %s %s"), *Path, bSaved ? TEXT("saved") : TEXT("FAILED"));
		}
	}

	FAutoConsoleCommandWithWorldAndArgs FileCommand(TEXT("StageCraft.Import.File"),
		TEXT("Imports model files through the controller, like a drop: StageCraft.Import.File <path>[|<path>...]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&File));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Import.Status"), TEXT("Logs import formats, the queue and every imported model."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs DragEnterCommand(TEXT("StageCraft.Import.DragEnter"),
		TEXT("Moves the OS cursor and drags files over the main window through Slate's OS drag and drop path: <x 0-1> <y 0-1> <file>[|<file>...]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DragEnter));
	FAutoConsoleCommandWithWorldAndArgs SaveThumbnailsCommand(TEXT("StageCraft.Import.SaveThumbnails"), TEXT("Writes the Library thumbnails of imported models to Saved/StageCraft/Thumbnails."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SaveThumbnails));
	FAutoConsoleCommandWithWorldAndArgs DragDropCommand(TEXT("StageCraft.Import.DragDrop"), TEXT("Drops the files of StageCraft.Import.DragEnter."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DragDrop));
}

#endif // !UE_BUILD_SHIPPING
