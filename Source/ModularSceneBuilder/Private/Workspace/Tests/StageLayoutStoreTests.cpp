// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for FStageLayoutStore (Docs/ADR/0001-dockable-workspace.md §7). No world, no Slate.
// Run: Session Frontend > Automation > StageCraft.Workspace.LayoutStore, or
//      UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Workspace.LayoutStore; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Workspace/StageLayoutStore.h"

namespace StageLayoutStoreTests
{
	const FName TestVersion(TEXT("StageCraft_Workspace_v1"));

	FString Str(EStageLayoutResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(const FVector2D& Value) { return Value.ToString(); }
	FString Str(const FBox2D& Value) { return Value.ToString(); }
	FString Str(const TArray<FString>& Value) { return FString::Join(Value, TEXT("|")); }

	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A fresh, empty folder per test, deleted when the test ends. */
	struct FScopedTestDirectory
	{
		FString Path;

		FScopedTestDirectory()
			: Path(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageLayoutStore"), FGuid::NewGuid().ToString()))
		{
		}

		~FScopedTestDirectory()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	/** A minimal Slate layout: one floating window (Placement_Specified) at Position/Size. */
	FString MakeSlateLayout(FName Version, const FVector2D& Position = FVector2D(100, 100), const FVector2D& Size = FVector2D(800, 600))
	{
		return FString::Printf(TEXT("{\"Type\":\"Layout\",\"Name\":\"%s\",\"PrimaryAreaIndex\":0,\"Areas\":[")
			TEXT("{\"SizeCoefficient\":1,\"Type\":\"Area\",\"Orientation\":\"Orient_Horizontal\",\"WindowPlacement\":\"Placement_NoWindow\",\"Nodes\":[]},")
			TEXT("{\"SizeCoefficient\":1,\"Type\":\"Area\",\"Orientation\":\"Orient_Horizontal\",\"WindowPlacement\":\"Placement_Specified\",")
			TEXT("\"WindowPosition_X\":%f,\"WindowPosition_Y\":%f,\"WindowSize_X\":%f,\"WindowSize_Y\":%f,\"bIsMaximized\":false,\"Nodes\":[]}]}"),
			*Version.ToString(), Position.X, Position.Y, Size.X, Size.Y);
	}

	FStageSavedLayout MakeLayout(const FString& Name)
	{
		FStageSavedLayout Layout;
		Layout.Name = Name;
		Layout.SlateLayout = MakeSlateLayout(TestVersion);
		Layout.MainWindow = FStageWindowPlacement{ FVector2D(10, 20), FVector2D(1600, 900), true };
		return Layout;
	}

	/** Two side-by-side 1920x1080 monitors with a 40 px taskbar. */
	TArray<FBox2D> TwoMonitors()
	{
		return { FBox2D(FVector2D(0, 0), FVector2D(1920, 1040)), FBox2D(FVector2D(1920, 0), FVector2D(3840, 1040)) };
	}

	/** Reads the floating area's rectangle back out of a layout's Slate JSON. */
	FBox2D ReadFloatingArea(const FString& SlateLayout)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SlateLayout), Root);
		const TSharedPtr<FJsonObject> Area = Root->GetArrayField(TEXT("Areas"))[1]->AsObject();
		const FVector2D Position(Area->GetNumberField(TEXT("WindowPosition_X")), Area->GetNumberField(TEXT("WindowPosition_Y")));
		return FBox2D(Position, Position + FVector2D(Area->GetNumberField(TEXT("WindowSize_X")), Area->GetNumberField(TEXT("WindowSize_Y"))));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLayoutStoreRoundTripTest, "StageCraft.Workspace.LayoutStore.RoundTrip", StageLayoutStoreTests::Flags)
bool FStageLayoutStoreRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace StageLayoutStoreTests;
	FScopedTestDirectory Dir;
	const FStageLayoutStore Store(Dir.Path, TestVersion);

	const FStageSavedLayout Saved = MakeLayout(TEXT("Show Night (2)"));
	TestEqual(TEXT("Save succeeds"), Str(Store.SaveUserLayout(Saved)), Str(EStageLayoutResult::Success));

	FStageSavedLayout Loaded;
	TestEqual(TEXT("Load succeeds"), Str(Store.LoadUserLayout(TEXT("Show Night (2)"), Loaded)), Str(EStageLayoutResult::Success));
	TestEqual(TEXT("Name kept"), Loaded.Name, Saved.Name);
	TestTrue(TEXT("Main window kept"), Loaded.MainWindow.IsSet());
	if (Loaded.MainWindow.IsSet())
	{
		TestEqual(TEXT("Main window position"), Str(Loaded.MainWindow->Position), Str(FVector2D(10, 20)));
		TestEqual(TEXT("Main window size"), Str(Loaded.MainWindow->Size), Str(FVector2D(1600, 900)));
		TestTrue(TEXT("Main window maximized"), Loaded.MainWindow->bMaximized);
	}
	TestEqual(TEXT("Floating area kept"), Str(ReadFloatingArea(Loaded.SlateLayout)), Str(FBox2D(FVector2D(100, 100), FVector2D(900, 700))));

	// Saving again under the same name replaces the file; no temp file is left behind.
	TestEqual(TEXT("Overwrite succeeds"), Str(Store.SaveUserLayout(Saved)), Str(EStageLayoutResult::Success));
	TestFalse(TEXT("No .tmp left"), IFileManager::Get().FileExists(*(Store.GetUserLayoutPath(Saved.Name) + TEXT(".tmp"))));

	// A layout without a main window (built-in style) round-trips without one.
	FStageSavedLayout NoWindow = MakeLayout(TEXT("Panels"));
	NoWindow.MainWindow.Reset();
	Store.SaveUserLayout(NoWindow);
	TestEqual(TEXT("Load without main window"), Str(Store.LoadUserLayout(TEXT("Panels"), Loaded)), Str(EStageLayoutResult::Success));
	TestFalse(TEXT("Main window stays unset"), Loaded.MainWindow.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLayoutStoreBadFilesTest, "StageCraft.Workspace.LayoutStore.BadFiles", StageLayoutStoreTests::Flags)
bool FStageLayoutStoreBadFilesTest::RunTest(const FString& Parameters)
{
	using namespace StageLayoutStoreTests;
	FScopedTestDirectory Dir;
	const FStageLayoutStore Store(Dir.Path, TestVersion);
	FStageSavedLayout Loaded;

	TestEqual(TEXT("Missing layout"), Str(Store.LoadUserLayout(TEXT("Nope"), Loaded)), Str(EStageLayoutResult::NotFound));
	TestEqual(TEXT("Missing session"), Str(Store.LoadSession(Loaded)), Str(EStageLayoutResult::NotFound));

	const auto ExpectMovedAside = [this, &Store, &Loaded](const TCHAR* What, const FString& Name, const FString& Contents, EStageLayoutResult Expected)
	{
		const FString Path = Store.GetUserLayoutPath(Name);
		FFileHelper::SaveStringToFile(Contents, *Path);
		TestEqual(What, Str(Store.LoadUserLayout(Name, Loaded)), Str(Expected));
		TestFalse(FString::Printf(TEXT("%s: original moved away"), What), IFileManager::Get().FileExists(*Path));
		TestTrue(FString::Printf(TEXT("%s: kept as .bak"), What), IFileManager::Get().FileExists(*(Path + TEXT(".bak"))));
		TestFalse(FString::Printf(TEXT("%s: no longer listed"), What), Store.ListUserLayouts().Contains(Name));
	};

	ExpectMovedAside(TEXT("Not JSON"), TEXT("Garbage"), TEXT("{ this is not json"), EStageLayoutResult::Corrupt);
	ExpectMovedAside(TEXT("Empty file"), TEXT("Empty"), TEXT(""), EStageLayoutResult::Corrupt);
	ExpectMovedAside(TEXT("No Slate layout"), TEXT("NoSlate"), TEXT("{\"FormatVersion\":1,\"Name\":\"NoSlate\"}"), EStageLayoutResult::Corrupt);
	ExpectMovedAside(TEXT("Bad main window"), TEXT("BadWindow"),
		FString::Printf(TEXT("{\"FormatVersion\":1,\"MainWindow\":{\"X\":0,\"Y\":0,\"W\":-5,\"H\":10},\"SlateLayout\":%s}"), *MakeSlateLayout(TestVersion)), EStageLayoutResult::Corrupt);
	ExpectMovedAside(TEXT("Future format"), TEXT("Future"),
		FString::Printf(TEXT("{\"FormatVersion\":99,\"SlateLayout\":%s}"), *MakeSlateLayout(TestVersion)), EStageLayoutResult::VersionMismatch);
	ExpectMovedAside(TEXT("Old panel structure"), TEXT("OldPanels"),
		FString::Printf(TEXT("{\"FormatVersion\":1,\"SlateLayout\":%s}"), *MakeSlateLayout(TEXT("StageCraft_Workspace_v0"))), EStageLayoutResult::VersionMismatch);

	// A corrupt session is moved aside the same way, so the next launch starts clean.
	FFileHelper::SaveStringToFile(TEXT("nonsense"), *Store.GetSessionPath());
	TestEqual(TEXT("Corrupt session"), Str(Store.LoadSession(Loaded)), Str(EStageLayoutResult::Corrupt));
	TestTrue(TEXT("Session kept as .bak"), IFileManager::Get().FileExists(*(Store.GetSessionPath() + TEXT(".bak"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLayoutStoreNamesTest, "StageCraft.Workspace.LayoutStore.Names", StageLayoutStoreTests::Flags)
bool FStageLayoutStoreNamesTest::RunTest(const FString& Parameters)
{
	using namespace StageLayoutStoreTests;

	TestTrue(TEXT("Plain"), FStageLayoutStore::IsValidLayoutName(TEXT("Show Night")));
	TestTrue(TEXT("Punctuation allowed"), FStageLayoutStore::IsValidLayoutName(TEXT("Dual_Monitor-2 (FOH)")));
	TestTrue(TEXT("Max length"), FStageLayoutStore::IsValidLayoutName(FString::ChrN(FStageLayoutStore::MaxNameLength, TEXT('a'))));
	TestFalse(TEXT("Empty"), FStageLayoutStore::IsValidLayoutName(TEXT("")));
	TestFalse(TEXT("Too long"), FStageLayoutStore::IsValidLayoutName(FString::ChrN(FStageLayoutStore::MaxNameLength + 1, TEXT('a'))));
	TestFalse(TEXT("Leading space"), FStageLayoutStore::IsValidLayoutName(TEXT(" Show")));
	TestFalse(TEXT("Trailing space"), FStageLayoutStore::IsValidLayoutName(TEXT("Show ")));
	TestFalse(TEXT("Path traversal"), FStageLayoutStore::IsValidLayoutName(TEXT("../Session")));
	TestFalse(TEXT("Separator"), FStageLayoutStore::IsValidLayoutName(TEXT("a/b")));
	TestFalse(TEXT("Extension dot"), FStageLayoutStore::IsValidLayoutName(TEXT("x.json")));
	TestFalse(TEXT("Device name"), FStageLayoutStore::IsValidLayoutName(TEXT("con")));

	FScopedTestDirectory Dir;
	const FStageLayoutStore Store(Dir.Path, TestVersion);
	FStageSavedLayout Bad = MakeLayout(TEXT("../Escape"));
	TestEqual(TEXT("Save refuses invalid names"), Str(Store.SaveUserLayout(Bad)), Str(EStageLayoutResult::InvalidName));
	FStageSavedLayout Loaded;
	TestEqual(TEXT("Load refuses invalid names"), Str(Store.LoadUserLayout(TEXT("../Session"), Loaded)), Str(EStageLayoutResult::InvalidName));
	TestEqual(TEXT("Delete refuses invalid names"), Str(Store.DeleteUserLayout(TEXT("a/b"))), Str(EStageLayoutResult::InvalidName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLayoutStoreListDeleteTest, "StageCraft.Workspace.LayoutStore.ListAndDelete", StageLayoutStoreTests::Flags)
bool FStageLayoutStoreListDeleteTest::RunTest(const FString& Parameters)
{
	using namespace StageLayoutStoreTests;
	FScopedTestDirectory Dir;
	const FStageLayoutStore Store(Dir.Path, TestVersion);

	TestEqual(TEXT("Empty folder lists nothing"), Store.ListUserLayouts().Num(), 0);
	Store.SaveUserLayout(MakeLayout(TEXT("zeta")));
	Store.SaveUserLayout(MakeLayout(TEXT("Alpha")));
	Store.SaveUserLayout(MakeLayout(TEXT("beta")));

	// The session file lives outside the layouts folder and must never be listed as a user layout.
	FStageSavedLayout Session = MakeLayout(TEXT("Alpha"));
	FStageLayoutStore::WriteFileAtomic(Store.GetSessionPath(), Store.Serialize(Session));

	TestEqual(TEXT("Sorted case-insensitively, session excluded"), Str(Store.ListUserLayouts()), Str(TArray<FString>{ TEXT("Alpha"), TEXT("beta"), TEXT("zeta") }));

	TestEqual(TEXT("Delete"), Str(Store.DeleteUserLayout(TEXT("beta"))), Str(EStageLayoutResult::Success));
	TestEqual(TEXT("Delete again"), Str(Store.DeleteUserLayout(TEXT("beta"))), Str(EStageLayoutResult::NotFound));
	TestEqual(TEXT("Listed after delete"), Str(Store.ListUserLayouts()), Str(TArray<FString>{ TEXT("Alpha"), TEXT("zeta") }));

	FStageSavedLayout Loaded;
	TestEqual(TEXT("Session loads"), Str(Store.LoadSession(Loaded)), Str(EStageLayoutResult::Success));
	TestEqual(TEXT("Session remembers its layout name"), Loaded.Name, FString(TEXT("Alpha")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLayoutStoreClampTest, "StageCraft.Workspace.LayoutStore.ClampToMonitors", StageLayoutStoreTests::Flags)
bool FStageLayoutStoreClampTest::RunTest(const FString& Parameters)
{
	using namespace StageLayoutStoreTests;
	const TArray<FBox2D> Monitors = TwoMonitors();
	const TArray<FBox2D> PrimaryOnly = { Monitors[0] };

	const auto Clamp = [&](const FVector2D& Position, const FVector2D& Size, TConstArrayView<FBox2D> Areas)
	{
		return FStageLayoutStore::ClampPlacement(FStageWindowPlacement{ Position, Size, false }, Areas);
	};

	FStageWindowPlacement Result = Clamp(FVector2D(2000, 100), FVector2D(1280, 720), Monitors);
	TestEqual(TEXT("On the second monitor: unchanged"), Str(Result.Position), Str(FVector2D(2000, 100)));

	Result = Clamp(FVector2D(2000, 100), FVector2D(1280, 720), PrimaryOnly);
	TestEqual(TEXT("Second monitor unplugged: moved onto the primary"), Str(Result.Position), Str(FVector2D(640, 100)));
	TestEqual(TEXT("Second monitor unplugged: size kept when it fits"), Str(Result.Size), Str(FVector2D(1280, 720)));

	Result = Clamp(FVector2D(-5000, -5000), FVector2D(4000, 3000), Monitors);
	TestEqual(TEXT("Far off-screen and oversized: fills the primary work area"), Str(Result.Position), Str(FVector2D(0, 0)));
	TestEqual(TEXT("Far off-screen and oversized: shrunk"), Str(Result.Size), Str(FVector2D(1920, 1040)));

	Result = Clamp(FVector2D(1800, 500), FVector2D(800, 800), Monitors);
	TestEqual(TEXT("Straddling: goes to the monitor it overlaps most"), Str(Result.Position), Str(FVector2D(1920, 240)));

	Result = Clamp(FVector2D(5, 5), FVector2D(100, 100), {});
	TestEqual(TEXT("No monitor information: unchanged"), Str(Result.Position), Str(FVector2D(5, 5)));

	// The whole layout: main window in screen pixels, floating windows in Slate units (here a 150% primary monitor).
	FStageSavedLayout Layout;
	Layout.Name = TEXT("Clamp");
	Layout.SlateLayout = MakeSlateLayout(TestVersion, FVector2D(3000, 200), FVector2D(800, 600));
	Layout.MainWindow = FStageWindowPlacement{ FVector2D(2500, 100), FVector2D(1600, 900), true };
	const TArray<FBox2D> SlateAreas = { FBox2D(FVector2D(0, 0), FVector2D(1280, 693)) };
	FStageLayoutStore::ClampToWorkAreas(Layout, PrimaryOnly, SlateAreas);

	TestEqual(TEXT("Main window clamped in screen pixels"), Str(Layout.MainWindow->Position), Str(FVector2D(320, 100)));
	TestTrue(TEXT("Maximized flag kept"), Layout.MainWindow->bMaximized);
	TestEqual(TEXT("Floating window clamped in Slate units"), Str(ReadFloatingArea(Layout.SlateLayout)), Str(FBox2D(FVector2D(480, 93), FVector2D(1280, 693))));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
