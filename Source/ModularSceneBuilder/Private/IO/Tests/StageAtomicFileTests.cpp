// Copyright Epic Games, Inc. All Rights Reserved.

// File tests for crash-safe writes and the import list (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.3, audit
// risk R8): a write leaves the old or the new content and no temporary file; a damaged import list is moved aside instead of
// being overwritten; a list that exists but cannot be read is reported so the session never writes over it.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.IO; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IO/StageAtomicFile.h"
#include "Import/StageMeshImportTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace StageAtomicFileTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A fresh folder under Saved/Automation, deleted with the scope. */
	struct FScopedFolder
	{
		FString Path;

		explicit FScopedFolder(const TCHAR* Name)
			: Path(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Name))
		{
			IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
			IFileManager::Get().MakeDirectory(*Path, /*Tree*/ true);
		}

		~FScopedFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	FString Read(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAtomicFileTest, "StageCraft.IO.AtomicFile", StageAtomicFileTests::Flags)

bool FStageAtomicFileTest::RunTest(const FString& Parameters)
{
	using namespace StageAtomicFileTests;
	FScopedFolder Folder(TEXT("AtomicFile"));
	const FString Path = Folder.Path / TEXT("Nested") / TEXT("Scene.json");

	TestTrue(TEXT("Write: creates the folder and the file"), StageAtomicFile::WriteText(Path, TEXT("first")));
	TestEqual(TEXT("Write: content"), Read(Path), FString(TEXT("first")));
	TestFalse(TEXT("Write: no temporary file left"), IFileManager::Get().FileExists(*(Path + TEXT(".tmp"))));

	TestTrue(TEXT("Replace: an existing file is replaced"), StageAtomicFile::WriteText(Path, TEXT("second")));
	TestEqual(TEXT("Replace: new content"), Read(Path), FString(TEXT("second")));

	TestFalse(TEXT("Keep: refuses an existing target"), StageAtomicFile::WriteText(Path, TEXT("third"), EStageFileWrite::KeepExisting));
	TestEqual(TEXT("Keep: the target is untouched"), Read(Path), FString(TEXT("second")));
	TestFalse(TEXT("Keep: no temporary file left after a refusal"), IFileManager::Get().FileExists(*(Path + TEXT(".tmp"))));

	// UTF-8 without a BOM, except a BOM the text itself starts with (the CSV exports put one there for Excel).
	const FString Csv = FString(TEXT("\uFEFF")) + TEXT("Gr\x00F6\x00DF" "e,Lux");
	TestTrue(TEXT("Text: written"), StageAtomicFile::WriteText(Path, Csv));
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Path);
	TestTrue(TEXT("Text: the text's own BOM is written once"), Bytes.Num() > 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF && Bytes[3] == 'G');
	TestEqual(TEXT("Text: UTF-8 bytes"), Bytes.Num(), 3 + 11);

	const TArray<uint8> Png = { 0x89, 'P', 'N', 'G', 0, 1, 2 };
	const FString ImagePath = Folder.Path / TEXT("Render.png");
	TestTrue(TEXT("Bytes: written"), StageAtomicFile::WriteBytes(ImagePath, TConstArrayView64<uint8>(Png.GetData(), Png.Num()), EStageFileWrite::KeepExisting));
	TArray<uint8> ReadBack;
	TestTrue(TEXT("Bytes: exact"), FFileHelper::LoadFileToArray(ReadBack, *ImagePath) && ReadBack == Png);

	TestTrue(TEXT("Move aside: moved"), StageAtomicFile::MoveAside(Path));
	TestFalse(TEXT("Move aside: the original name is free"), IFileManager::Get().FileExists(*Path));
	TestTrue(TEXT("Move aside: kept as .bak"), IFileManager::Get().FileExists(*(Path + TEXT(".bak"))));
	TestFalse(TEXT("Move aside: nothing to move"), StageAtomicFile::MoveAside(Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImportListFileTest, "StageCraft.IO.ImportListFile", StageAtomicFileTests::Flags)

bool FStageImportListFileTest::RunTest(const FString& Parameters)
{
	using namespace StageAtomicFileTests;
	using StageMeshImport::EImportListRead;
	FScopedFolder Folder(TEXT("ImportListFile"));
	const FString Path = Folder.Path / TEXT("Imports.json");
	TArray<FString> Files = { TEXT("stale") };

	TestTrue(TEXT("Missing: first run"), StageMeshImport::LoadImportListFile(Path, Files) == EImportListRead::Missing && Files.IsEmpty());

	const TArray<FString> Saved = { FPaths::ConvertRelativePathToFull(Folder.Path / TEXT("Chair.glb")), FPaths::ConvertRelativePathToFull(Folder.Path / TEXT("Desk.obj")) };
	StageAtomicFile::WriteText(Path, StageMeshImport::WriteImportList(Saved));
	TestTrue(TEXT("Read: a valid list"), StageMeshImport::LoadImportListFile(Path, Files) == EImportListRead::Read && Files == Saved);

	// What a crash or a hand edit can leave: the list must survive, not be overwritten by the next import.
	const FString Damaged = TEXT("{\"version\":1,\"files\":[\"C:/Models/Ch");
	StageAtomicFile::WriteText(Path, Damaged);
	TestTrue(TEXT("Damaged: reported"), StageMeshImport::LoadImportListFile(Path, Files) == EImportListRead::Damaged && Files.IsEmpty());
	TestFalse(TEXT("Damaged: moved away from the name the next save writes"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("Damaged: kept byte for byte as .bak"), Read(Path + TEXT(".bak")), Damaged);

	// A list that exists but cannot be read (here: held open exclusively) must never be written over.
	StageAtomicFile::WriteText(Path, StageMeshImport::WriteImportList(Saved));
	{
		TUniquePtr<IFileHandle> Lock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false));
		if (TestNotNull(TEXT("Unreadable: the file can be locked"), Lock.Get()))
		{
			TestTrue(TEXT("Unreadable: reported"), StageMeshImport::LoadImportListFile(Path, Files) == EImportListRead::Unreadable);
		}
	}
	TestTrue(TEXT("Unreadable: the list itself is unchanged"), StageMeshImport::LoadImportListFile(Path, Files) == EImportListRead::Read && Files == Saved);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
