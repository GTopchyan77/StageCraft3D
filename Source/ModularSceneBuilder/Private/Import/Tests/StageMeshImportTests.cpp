// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for runtime model import (Docs/ADR/0006-inspector-polish-and-runtime-model-import.md): formats, file identities,
// display names, the file dialog filter and the remembered-imports list (untrusted input). No world, no importer, no files.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Import; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Import/StageMeshImportTypes.h"
#include "Misc/AutomationTest.h"

namespace StageMeshImportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImportFormatsTest, "StageCraft.Import.Formats", StageMeshImportTests::Flags)

bool FStageImportFormatsTest::RunTest(const FString& Parameters)
{
	using namespace StageMeshImport;

	// Importer format strings: only candidates survive, in candidate order, whatever the importer's order or case.
	const TArray<FString> Importer = { TEXT("obj;OBJ File"), TEXT("png;Image"), TEXT("GLTF;GL Transmission Format"), TEXT("glb;Binary glTF"), TEXT("fbx;Filmbox") };
	TestTrue(TEXT("All four formats, candidate order"), FilterSupportedExtensions(Importer) == TArray<FString>({ TEXT("glb"), TEXT("gltf"), TEXT("obj"), TEXT("fbx") }));

	// A packaged build has no FBX translator: FBX drops out.
	const TArray<FString> Packaged = { TEXT("gltf;GL Transmission Format"), TEXT("glb;Binary glTF"), TEXT("obj;OBJ File") };
	TestTrue(TEXT("No FBX without its translator"), FilterSupportedExtensions(Packaged) == TArray<FString>({ TEXT("glb"), TEXT("gltf"), TEXT("obj") }));

	// Several extensions in one entry, stray spaces, entries without a description.
	TestTrue(TEXT("Comma lists and bare extensions"), FilterSupportedExtensions({ TEXT(" gltf , glb ;glTF"), TEXT("obj") }) == TArray<FString>({ TEXT("glb"), TEXT("gltf"), TEXT("obj") }));
	TestTrue(TEXT("Nothing supported"), FilterSupportedExtensions({ TEXT("wav;Audio"), TEXT(""), TEXT(";") }).IsEmpty());
	TestTrue(TEXT("No importer"), FilterSupportedExtensions({}).IsEmpty());

	const TArray<FString> Supported = { TEXT("glb"), TEXT("obj") };
	TestTrue(TEXT("Extension match ignores case"), HasSupportedExtension(TEXT("C:/Models/Chair.GLB"), Supported));
	TestFalse(TEXT("Unsupported extension"), HasSupportedExtension(TEXT("C:/Models/Chair.dae"), Supported));
	TestFalse(TEXT("No extension"), HasSupportedExtension(TEXT("C:/Models/Chair"), Supported));
	TestFalse(TEXT("A dotted folder is not an extension"), HasSupportedExtension(TEXT("C:/Models.glb/Chair"), Supported));
	TestFalse(TEXT("Empty path"), HasSupportedExtension(TEXT(""), Supported));
	TestFalse(TEXT("Nothing supported"), HasSupportedExtension(TEXT("C:/Models/Chair.glb"), {}));

	// Unit and axis correction: only OBJ, which stores neither, gets one (Y-up metres to Z-up centimetres).
	const FImportOffset Obj = GetImportOffset(TEXT("OBJ"));
	TestEqual(TEXT("OBJ metres become centimetres"), Obj.UniformScale, 100.0);
	// A quarter turn about X (roll). How Interchange applies it (+Y up ends as +Z up) was checked on Stage_Riser.obj at runtime:
	// bounds Z 0 to 40 cm (STATE.md #28); FRotator::RotateVector alone would send +Y to -Z.
	TestTrue(TEXT("OBJ gets a quarter turn about X"), Obj.Rotation.Equals(FRotator(0.0, 0.0, 90.0)));
	for (const TCHAR* Extension : { TEXT("glb"), TEXT("gltf"), TEXT("fbx"), TEXT("") })
	{
		const FImportOffset None = GetImportOffset(Extension);
		TestTrue(FString::Printf(TEXT("No correction for '%s'"), Extension), None.UniformScale == 1.0 && None.Rotation.IsZero());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImportIdentityTest, "StageCraft.Import.Identity", StageMeshImportTests::Flags)

bool FStageImportIdentityTest::RunTest(const FString& Parameters)
{
	using namespace StageMeshImport;

	// One file, one identity: relative segments, separators and case do not create a second import.
	const FString Path = NormalizeSourcePath(TEXT("C:/Models/Stage/../Props/Chair.glb"));
	TestEqual(TEXT("Relative segments collapse"), Path, FString(TEXT("C:/Models/Props/Chair.glb")));
	TestEqual(TEXT("Backslashes normalize"), NormalizeSourcePath(TEXT("C:\\Models\\Props\\Chair.glb")), Path);
	TestEqual(TEXT("Surrounding blanks are ignored"), NormalizeSourcePath(TEXT("  C:/Models/Props/Chair.glb ")), Path);

	const FString Name = MakeItemName(Path);
	TestTrue(TEXT("Item names are IMP_ + 16 hex digits"), Name.StartsWith(TEXT("IMP_")) && Name.Len() == 20);
	TestEqual(TEXT("The name is stable"), MakeItemName(Path), Name);
	TestEqual(TEXT("Case does not matter (Windows paths)"), MakeItemName(TEXT("c:/models/props/CHAIR.GLB")), Name);
	TestNotEqual(TEXT("Another file has another name"), MakeItemName(TEXT("C:/Models/Props/Table.glb")), Name);
	TestTrue(TEXT("Names are valid object names"), FName(*Name).IsValidXName(INVALID_OBJECTNAME_CHARACTERS));

	const FString Package = MakeItemPackagePath(Name);
	TestTrue(TEXT("Packages live under the import root"), IsImportedItemPath(Package));
	TestTrue(TEXT("The root is the temporary mount, never saved"), Package.StartsWith(TEXT("/Temp/")));
	TestFalse(TEXT("Content items are not imports"), IsImportedItemPath(TEXT("/Game/StageCraft/Items/DA_Test_Light")));
	TestFalse(TEXT("Other temporary packages are not imports"), IsImportedItemPath(TEXT("/Temp/Something")));

	TestEqual(TEXT("Display name from the file name"), MakeDisplayName(TEXT("C:/Models/big_speaker-stack.glb")).ToString(), FString(TEXT("big speaker stack")));
	TestEqual(TEXT("Plain names stay"), MakeDisplayName(TEXT("C:/Models/Chair.obj")).ToString(), FString(TEXT("Chair")));
	TestFalse(TEXT("A name made of separators still reads as something"), MakeDisplayName(TEXT("C:/Models/__.glb")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImportDialogFilterTest, "StageCraft.Import.DialogFilter", StageMeshImportTests::Flags)

bool FStageImportDialogFilterTest::RunTest(const FString& Parameters)
{
	using namespace StageMeshImport;

	const FString Filter = MakeFileDialogFilter({ TEXT("glb"), TEXT("gltf"), TEXT("obj"), TEXT("fbx") });
	TArray<FString> Parts;
	Filter.ParseIntoArray(Parts, TEXT("|"), /*CullEmpty*/ false);

	// Label|Pattern pairs: all models first, then glTF (both extensions), OBJ, FBX.
	TestEqual(TEXT("Four label / pattern pairs"), Parts.Num(), 8);
	if (Parts.Num() == 8)
	{
		TestEqual(TEXT("All models pattern"), Parts[1], FString(TEXT("*.glb;*.gltf;*.obj;*.fbx")));
		TestEqual(TEXT("glTF groups both extensions"), Parts[3], FString(TEXT("*.glb;*.gltf")));
		TestEqual(TEXT("OBJ"), Parts[5], FString(TEXT("*.obj")));
		TestEqual(TEXT("FBX"), Parts[7], FString(TEXT("*.fbx")));
		TestTrue(TEXT("The all-models label shows the patterns"), Parts[0].Contains(TEXT("*.glb;*.gltf;*.obj;*.fbx")));
	}

	TArray<FString> ObjOnly;
	MakeFileDialogFilter({ TEXT("obj") }).ParseIntoArray(ObjOnly, TEXT("|"), /*CullEmpty*/ false);
	TestTrue(TEXT("Only what the build supports"), ObjOnly.Num() == 4 && ObjOnly[1] == TEXT("*.obj") && ObjOnly[3] == TEXT("*.obj"));
	TestTrue(TEXT("No formats, no filter"), MakeFileDialogFilter({}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImportListTest, "StageCraft.Import.List", StageMeshImportTests::Flags)

bool FStageImportListTest::RunTest(const FString& Parameters)
{
	using namespace StageMeshImport;

	const TArray<FString> Files = { TEXT("C:/Models/Chair.glb"), TEXT("D:/Show/Riser.obj") };
	TArray<FString> Read;
	TestTrue(TEXT("Round trip reads"), ReadImportList(WriteImportList(Files), Read));
	TestTrue(TEXT("Round trip keeps files and order"), Read == Files);

	TestTrue(TEXT("An empty list is valid"), ReadImportList(WriteImportList({}), Read) && Read.IsEmpty());

	// Untrusted file contents: the document itself must be right, entries are filtered one by one.
	TestFalse(TEXT("Not JSON"), ReadImportList(TEXT("chair.glb"), Read));
	TestFalse(TEXT("Empty file"), ReadImportList(TEXT(""), Read));
	TestFalse(TEXT("Missing version"), ReadImportList(TEXT("{\"files\":[]}"), Read));
	TestFalse(TEXT("Future version"), ReadImportList(TEXT("{\"version\":2,\"files\":[]}"), Read));
	TestFalse(TEXT("Files is not a list"), ReadImportList(TEXT("{\"version\":1,\"files\":\"C:/a.glb\"}"), Read));

	const FString Mixed = TEXT("{\"version\":1,\"files\":[\"C:/A.glb\", 7, \"\", \"relative/B.glb\", \"c:/a.GLB\", null, \"D:/C.obj\"]}");
	TestTrue(TEXT("Mixed entries read"), ReadImportList(Mixed, Read));
	TestTrue(TEXT("Only absolute, non-empty, unique strings survive"), Read == TArray<FString>({ TEXT("C:/A.glb"), TEXT("D:/C.obj") }));

	FString TooLong = TEXT("C:/") + FString::ChrN(2000, TEXT('a')) + TEXT(".glb");
	TestTrue(TEXT("Over-long paths are dropped"), ReadImportList(FString::Printf(TEXT("{\"version\":1,\"files\":[\"%s\"]}"), *TooLong), Read) && Read.IsEmpty());

	TArray<FString> Many;
	for (int32 Index = 0; Index < MaxRememberedFiles + 10; ++Index)
	{
		Many.Add(FString::Printf(TEXT("C:/Models/Item%d.glb"), Index));
	}
	TestTrue(TEXT("Long lists read"), ReadImportList(WriteImportList(Many), Read));
	TestEqual(TEXT("At most MaxRememberedFiles are kept"), Read.Num(), MaxRememberedFiles);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
