// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/StageFileDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"

#if WITH_STAGECRAFT_FILE_DIALOG
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#elif WITH_STAGECRAFT_NATIVE_FILE_DIALOG
// UE's windows.h wrapper first (NOMINMAX, WIN32_LEAN_AND_MEAN): COMPointer.h (<Unknwn.h>) and <shobjidl.h> would otherwise
// pull in the raw windows.h, whose min/max macros break engine headers after this file in the same unity blob (HittestGrid.h).
#include "Windows/WindowsHWrapper.h"
#include "Microsoft/COMPointer.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shobjidl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace StageFileDialog
{
#if WITH_STAGECRAFT_NATIVE_FILE_DIALOG
	namespace
	{
		// The Windows common item dialogs (IFileOpenDialog, IFileSaveDialog), for builds without the engine's DesktopPlatform
		// (Shipping). Same filter form and results as DesktopPlatform's Windows implementation, which wraps the same COM dialogs.

		/** The folder, title and filter setup both dialogs share. Parts holds the filter strings, which must outlive the dialog's use. */
		void SetUpDialog(IFileDialog& Dialog, const FText& Title, const FString& DefaultDirectory, const FString& Filter, TArray<FString>& Parts)
		{
			Dialog.SetTitle(*Title.ToString());

			if (!DefaultDirectory.IsEmpty())
			{
				// The shell parses only absolute paths with backslashes.
				FString WindowsPath = FPaths::ConvertRelativePathToFull(DefaultDirectory);
				WindowsPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
				TComPtr<IShellItem> Folder;
				if (SUCCEEDED(::SHCreateItemFromParsingName(*WindowsPath, nullptr, IID_PPV_ARGS(&Folder))))
				{
					Dialog.SetFolder(Folder);
				}
			}

			// "Label|*.a;*.b|Label|*.c" -> label/spec pairs.
			Filter.ParseIntoArray(Parts, TEXT("|"), /*bCullEmpty*/ true);
			TArray<COMDLG_FILTERSPEC> Specs;
			if (Parts.Num() % 2 == 0)
			{
				for (int32 Index = 0; Index + 1 < Parts.Num(); Index += 2)
				{
					COMDLG_FILTERSPEC& Spec = Specs.AddDefaulted_GetRef();
					Spec.pszName = *Parts[Index];
					Spec.pszSpec = *Parts[Index + 1];
				}
			}
			Dialog.SetFileTypes(Specs.Num(), Specs.GetData());
		}

		/** FindBestParentWindowHandleForDialogs returns the native window handle as const void*. */
		HWND GetParentWindow()
		{
			return FSlateApplication::IsInitialized() ? static_cast<HWND>(const_cast<void*>(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr))) : nullptr;
		}

		bool SaveFileNative(const FText& Title, const FString& DefaultDirectory, const FString& DefaultFileName, const FString& Filter, FString& OutFile)
		{
			TComPtr<IFileSaveDialog> Dialog;
			if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
			{
				return false;
			}

			DWORD Options = 0;
			Dialog->GetOptions(&Options);
			Dialog->SetOptions(Options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
			TArray<FString> Parts;
			SetUpDialog(*Dialog, Title, DefaultDirectory, Filter, Parts);
			Dialog->SetFileName(*DefaultFileName);
			if (FAILED(Dialog->Show(GetParentWindow())))
			{
				// Includes the user cancelling.
				return false;
			}

			TComPtr<IShellItem> Item;
			PWSTR Path = nullptr;
			if (FAILED(Dialog->GetResult(&Item)) || FAILED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)))
			{
				return false;
			}
			OutFile = Path;
			::CoTaskMemFree(Path);
			FPaths::NormalizeFilename(OutFile);
			OutFile = FPaths::ConvertRelativePathToFull(OutFile);
			return true;
		}

		bool OpenFilesNative(const FText& Title, const FString& DefaultDirectory, const FString& Filter, TArray<FString>& OutFiles)
		{
			TComPtr<IFileOpenDialog> Dialog;
			if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
			{
				return false;
			}

			DWORD Options = 0;
			Dialog->GetOptions(&Options);
			Dialog->SetOptions(Options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
			TArray<FString> Parts;
			SetUpDialog(*Dialog, Title, DefaultDirectory, Filter, Parts);

			if (FAILED(Dialog->Show(GetParentWindow())))
			{
				// Includes the user cancelling.
				return false;
			}

			TComPtr<IShellItemArray> Results;
			if (FAILED(Dialog->GetResults(&Results)))
			{
				return false;
			}
			DWORD Count = 0;
			Results->GetCount(&Count);
			for (DWORD Index = 0; Index < Count; ++Index)
			{
				TComPtr<IShellItem> Item;
				PWSTR Path = nullptr;
				if (SUCCEEDED(Results->GetItemAt(Index, &Item)) && SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)))
				{
					FString File(Path);
					::CoTaskMemFree(Path);
					FPaths::NormalizeFilename(File);
					OutFiles.Add(FPaths::ConvertRelativePathToFull(File));
				}
			}
			return !OutFiles.IsEmpty();
		}
	}
#endif

	bool IsAvailable()
	{
#if WITH_STAGECRAFT_FILE_DIALOG
		return FDesktopPlatformModule::Get() != nullptr;
#elif WITH_STAGECRAFT_NATIVE_FILE_DIALOG
		return true;
#else
		return false;
#endif
	}

	bool OpenFiles(const FText& Title, const FString& DefaultDirectory, const FString& Filter, TArray<FString>& OutFiles)
	{
		OutFiles.Reset();
#if WITH_STAGECRAFT_FILE_DIALOG
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		// Parenting to the active app window keeps the dialog on top of it and modal to it.
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		TArray<FString> Picked;
		if (!DesktopPlatform->OpenFileDialog(ParentWindow, Title.ToString(), DefaultDirectory, FString(), Filter, EFileDialogFlags::Multiple, Picked))
		{
			return false;
		}
		for (const FString& File : Picked)
		{
			OutFiles.Add(FPaths::ConvertRelativePathToFull(File));
		}
		return !OutFiles.IsEmpty();
#elif WITH_STAGECRAFT_NATIVE_FILE_DIALOG
		return OpenFilesNative(Title, DefaultDirectory, Filter, OutFiles);
#else
		return false;
#endif
	}

	bool SaveFile(const FText& Title, const FString& DefaultDirectory, const FString& DefaultFileName, const FString& Filter, FString& OutFile)
	{
		OutFile.Reset();
#if WITH_STAGECRAFT_FILE_DIALOG
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		TArray<FString> Picked;
		if (!DesktopPlatform->SaveFileDialog(ParentWindow, Title.ToString(), DefaultDirectory, DefaultFileName, Filter, EFileDialogFlags::None, Picked) || Picked.IsEmpty())
		{
			return false;
		}
		OutFile = FPaths::ConvertRelativePathToFull(Picked[0]);
		return true;
#elif WITH_STAGECRAFT_NATIVE_FILE_DIALOG
		return SaveFileNative(Title, DefaultDirectory, DefaultFileName, Filter, OutFile);
#else
		return false;
#endif
	}
}
