// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/StagePackage.h"
#include "Marketplace/Tests/StageTestMarketplaceBackend.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Scene/StageSceneStore.h"
#include "Serialization/MemoryWriter.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestFixtures.h"
#include "UObject/StrongObjectPtr.h"

namespace StagePackageFixtureTests
{
	constexpr uint32 MakeTag(char A, char B, char C, char D)
	{
		return uint32(uint8(A)) | (uint32(uint8(B)) << 8) | (uint32(uint8(C)) << 16) | (uint32(uint8(D)) << 24);
	}

	const uint32 ManifestTag = MakeTag('M', 'N', 'F', 'T');
	const uint32 SceneTag = MakeTag('S', 'C', 'E', 'N');
	const uint32 FixtureTag = MakeTag('F', 'I', 'X', 'T');

	FGuid SpotId()
	{
		FGuid Id;
		FGuid::Parse(StageGdtfTestFiles::SpotId, Id);
		return Id;
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FStageItemSnapshot MakeLibraryItem(const FGuid& TypeId)
	{
		FStageItemSnapshot Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Item = TSoftObjectPtr<UBaseItemData>(StageFixtureLibrary::MakeItemPath(TypeId, TEXT("Standard")));
		Item.Transform = FTransform(FVector(0.0, 300.0, 400.0));
		Item.DisplayName = FText::FromString(TEXT("Spot"));
		Item.FixtureType.Id = TypeId;
		Item.FixtureType.Manufacturer = TEXT("StageCraft");
		Item.FixtureType.Name = TEXT("Test Spot");
		Item.FixtureType.Mode = TEXT("Standard");
		return Item;
	}

	FStageSceneDocument MakeFixtureScene(const FString& Name, TConstArrayView<FGuid> Types)
	{
		FStageSceneDocument Scene = StageTestFixtures::MakeScene(Name);
		for (const FGuid& TypeId : Types)
		{
			Scene.Items.Add(MakeLibraryItem(TypeId));
		}
		return Scene;
	}

	FStagePackage MakePackage(const FStageSceneDocument& Scene)
	{
		FStagePackage Package;
		Package.Scene = Scene;
		Package.Manifest.PackageId = FGuid::NewGuid();
		Package.Manifest.Title = Scene.Name;
		Package.Manifest.AuthorName = TEXT("Test Publisher");
		Package.Manifest.Category = StageCraftTags::Marketplace_Category_Concert;
		Package.Manifest.CreatedAtUtc = FDateTime::UtcNow();
		Package.Manifest.ItemCount = Scene.Items.Num();
		Package.Manifest.CueCount = Scene.Cues.Num();
		Package.Manifest.RequiredItems = StagePackage::CollectRequiredItems(Scene);
		Package.Manifest.FixtureTypes = StagePackage::CollectFixtureTypes(Scene);
		for (const FGuid& TypeId : Package.Manifest.FixtureTypes)
		{
			FStagePackageFixtureFile& File = Package.FixtureFiles.AddDefaulted_GetRef();
			File.FixtureTypeId = TypeId;
			File.Bytes = StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml());
		}
		return Package;
	}

	struct FEntry
	{
		FGuid Id;
		TArray<uint8> Bytes;
	};

	TArray<uint8> FixturePayload(const TArray<FEntry>& Entries, TOptional<uint32> Count = {})
	{
		TArray<uint8> Raw;
		FMemoryWriter Writer(Raw);
		uint32 Written = Count.Get(Entries.Num());
		Writer << Written;
		for (const FEntry& Entry : Entries)
		{
			uint32 Words[4] = { Entry.Id.A, Entry.Id.B, Entry.Id.C, Entry.Id.D };
			for (uint32& Word : Words)
			{
				Writer << Word;
			}
			uint32 Size = Entry.Bytes.Num();
			Writer << Size;
			Writer.Serialize(const_cast<uint8*>(Entry.Bytes.GetData()), Entry.Bytes.Num());
		}
		return Raw;
	}

	TArray<uint8> Container(const TArray<TPair<uint32, TArray<uint8>>>& Sections)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		uint8 Magic[8] = { 'S', 'T', 'A', 'G', 'E', 'P', 'K', 'G' };
		Writer.Serialize(Magic, sizeof(Magic));
		uint32 Version = StagePackage::ContainerVersion;
		uint32 Count = Sections.Num();
		Writer << Version;
		Writer << Count;
		for (const TPair<uint32, TArray<uint8>>& Section : Sections)
		{
			uint32 Tag = Section.Key;
			uint32 Codec = 0;
			uint64 Size = Section.Value.Num();
			uint64 RawSize = Section.Value.Num();
			FSHAHash Hash;
			FSHA1::HashBuffer(Section.Value.GetData(), Section.Value.Num(), Hash.Hash);
			Writer << Tag;
			Writer << Codec;
			Writer << Size;
			Writer << RawSize;
			Writer.Serialize(Hash.Hash, sizeof(Hash.Hash));
		}
		for (const TPair<uint32, TArray<uint8>>& Section : Sections)
		{
			Writer.Serialize(const_cast<uint8*>(Section.Value.GetData()), Section.Value.Num());
		}
		return Bytes;
	}

	TArray<uint8> Craft(const FStagePackageManifest& Manifest, const FStageSceneDocument& Scene, TOptional<TArray<uint8>> Fixtures)
	{
		TArray<TPair<uint32, TArray<uint8>>> Sections;
		Sections.Emplace(ManifestTag, Utf8(StagePackage::SerializeManifest(Manifest)));
		Sections.Emplace(SceneTag, Utf8(FStageSceneStore::Serialize(Scene)));
		if (Fixtures.IsSet())
		{
			Sections.Emplace(FixtureTag, Fixtures.GetValue());
		}
		return Container(Sections);
	}

	FString Str(EStageMarketplaceResult Result) { return UEnum::GetValueAsString(Result); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePackageFixtureFormatTest, "StageCraft.Marketplace.PackageFixtures", StageTestFixtures::Flags)

bool FStagePackageFixtureFormatTest::RunTest(const FString& Parameters)
{
	using namespace StagePackageFixtureTests;

	const FGuid Spot = SpotId();
	const FGuid Low(0x00000001, 0, 0, 1);
	const FGuid High(0x80000000, 0, 0, 0);
	const TArray<uint8> Gdtf = StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml());

	TestTrue(TEXT("Unsigned order: 0x80000000 sorts after 0x00000001"), StagePackage::IsFixtureOrderLess(Low, High) && !StagePackage::IsFixtureOrderLess(High, Low));
	TestTrue(TEXT("Equal A: B decides"), StagePackage::IsFixtureOrderLess(FGuid(1, 1, 9, 9), FGuid(1, 2, 0, 0)));
	TestFalse(TEXT("Equal ids are not less"), StagePackage::IsFixtureOrderLess(Spot, Spot));

	const FStageSceneDocument Scene = MakeFixtureScene(TEXT("GDTF Rig"), { Spot });
	TestTrue(TEXT("A library fixture scene is portable"), StagePackage::IsPortable(Scene));
	TestTrue(TEXT("Its type is collected once"), StagePackage::CollectFixtureTypes(MakeFixtureScene(TEXT("Twice"), { Spot, Spot })) == TArray<FGuid>({ Spot }));

	AddInfo(TEXT("Round trip with the FIXT section."));
	FStagePackage Package = MakePackage(Scene);
	TArray<uint8> Bytes;
	TestEqual(TEXT("Writes"), Str(StagePackage::Write(Package, Bytes)), Str(EStageMarketplaceResult::Success));
	FStagePackage Read;
	if (TestEqual(TEXT("Reads back"), Str(StagePackage::Read(Bytes, Read)), Str(EStageMarketplaceResult::Success)))
	{
		TestTrue(TEXT("The manifest names the type"), Read.Manifest.FixtureTypes == TArray<FGuid>({ Spot }));
		TestTrue(TEXT("The GDTF file comes back byte for byte"), Read.FixtureFiles.Num() == 1 && Read.FixtureFiles[0].FixtureTypeId == Spot && Read.FixtureFiles[0].Bytes == Gdtf);
	}
	FStagePackageManifest Browsed;
	bool bHasThumbnail = true;
	TestTrue(TEXT("Browsing reads the manifest only"), StagePackage::ReadManifest(Bytes, Browsed, bHasThumbnail) == EStageMarketplaceResult::Success && Browsed.FixtureTypes.Num() == 1);
	const FString Json = StagePackage::SerializeManifest(Package.Manifest);
	TestTrue(TEXT("Manifest version 3 with the ids in hyphenated form"), Json.Contains(TEXT("\"version\":3")) && Json.Contains(Spot.ToString(EGuidFormats::DigitsWithHyphens)));

	AddInfo(TEXT("Without fixtures nothing changes but the version."));
	FStagePackage Plain = MakePackage(StageTestFixtures::MakeScene(TEXT("Plain")));
	TestTrue(TEXT("A plain package has no FIXT"), StagePackage::Write(Plain, Bytes) == EStageMarketplaceResult::Success && StagePackage::Read(Bytes, Read) == EStageMarketplaceResult::Success
		&& Read.FixtureFiles.IsEmpty() && Read.Manifest.FixtureTypes.IsEmpty());
	FString Version2 = StagePackage::SerializeManifest(Plain.Manifest).Replace(TEXT("\"version\":3"), TEXT("\"version\":2")).Replace(TEXT(",\"fixtureTypes\":[]"), TEXT(""));
	FStagePackageManifest Parsed;
	TestEqual(TEXT("A version 2 manifest still reads"), Str(StagePackage::ParseManifest(Version2, Parsed)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Version 3 without fixtureTypes"), Str(StagePackage::ParseManifest(Version2.Replace(TEXT("\"version\":2"), TEXT("\"version\":3")), Parsed)), Str(EStageMarketplaceResult::Corrupt));
	const FString Unsorted = StagePackage::SerializeManifest(Plain.Manifest).Replace(TEXT("\"fixtureTypes\":[]"),
		*FString::Printf(TEXT("\"fixtureTypes\":[\"%s\",\"%s\"]"), *High.ToString(EGuidFormats::DigitsWithHyphens), *Low.ToString(EGuidFormats::DigitsWithHyphens)));
	TestEqual(TEXT("Unsorted fixture types"), Str(StagePackage::ParseManifest(Unsorted, Parsed)), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("A braced id"), Str(StagePackage::ParseManifest(StagePackage::SerializeManifest(Package.Manifest).Replace(*Spot.ToString(EGuidFormats::DigitsWithHyphens),
		*Spot.ToString(EGuidFormats::DigitsWithHyphensInBraces)), Parsed)), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("A fixtureTypes that is not an array"), Str(StagePackage::ParseManifest(StagePackage::SerializeManifest(Plain.Manifest).Replace(TEXT("\"fixtureTypes\":[]"), TEXT("\"fixtureTypes\":\"x\"")), Parsed)),
		Str(EStageMarketplaceResult::Corrupt));

	AddInfo(TEXT("Write refuses packages that do not describe their scene."));
	TArray<uint8> Unused;
	FStagePackage Lying = Package;
	Lying.Manifest.FixtureTypes.Reset();
	Lying.FixtureFiles.Reset();
	TestEqual(TEXT("Fixture types left out"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));
	Lying = Package;
	Lying.FixtureFiles.Reset();
	TestEqual(TEXT("File left out"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));
	Lying = Package;
	Lying.FixtureFiles[0].Bytes = Utf8(TEXT("not a zip file at all, really"));
	TestEqual(TEXT("Not a ZIP"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));
	Lying = Package;
	Lying.FixtureFiles[0].Bytes.SetNumZeroed(StagePackage::MaxFixtureFileBytes + 1);
	FMemory::Memcpy(Lying.FixtureFiles[0].Bytes.GetData(), Gdtf.GetData(), 4);
	TestEqual(TEXT("Over 16 MB"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::TooLarge));
	Lying = Package;
	Lying.Manifest.FixtureTypes = { Spot, Spot };
	TestEqual(TEXT("Repeated type"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));

	AddInfo(TEXT("Read refuses tampered packages (hashes recomputed, so only the checks stand between)."));
	const FStageSceneDocument& RigScene = Package.Scene;
	const FStagePackageManifest& Rig = Package.Manifest;
	const auto ReadResult = [&Read](const TArray<uint8>& Crafted) { return Str(StagePackage::Read(Crafted, Read)); };
	TestEqual(TEXT("The crafted original reads"), ReadResult(Craft(Rig, RigScene, FixturePayload({ { Spot, Gdtf } }))), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("No FIXT although the manifest names a type"), ReadResult(Craft(Rig, RigScene, {})), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Count 0"), ReadResult(Craft(Rig, RigScene, FixturePayload({}, 0u))), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Count larger than the entries"), ReadResult(Craft(Rig, RigScene, FixturePayload({ { Spot, Gdtf } }, 2u))), Str(EStageMarketplaceResult::Corrupt));
	TArray<uint8> Trailing = FixturePayload({ { Spot, Gdtf } });
	Trailing.Add(0);
	TestEqual(TEXT("Trailing byte"), ReadResult(Craft(Rig, RigScene, Trailing)), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Not a ZIP"), ReadResult(Craft(Rig, RigScene, FixturePayload({ { Spot, Utf8(TEXT("0123456789012345678901234")) } }))), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Too small"), ReadResult(Craft(Rig, RigScene, FixturePayload({ { Spot, TArray<uint8>({ 0x50, 0x4B, 0x03, 0x04 }) } }))), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("FIXT id differs from the manifest"), ReadResult(Craft(Rig, RigScene, FixturePayload({ { High, Gdtf } }))), Str(EStageMarketplaceResult::Corrupt));
	TArray<uint8> Huge = FixturePayload({ { Spot, Gdtf } });
	const uint32 HugeSize = uint32(StagePackage::MaxFixtureFileBytes + 1);
	FMemory::Memcpy(Huge.GetData() + 4 + 16, &HugeSize, sizeof(HugeSize));
	TestEqual(TEXT("A declared size over 16 MB"), ReadResult(Craft(Rig, RigScene, Huge)), Str(EStageMarketplaceResult::TooLarge));

	FStageSceneDocument TwoTypes = MakeFixtureScene(TEXT("Two"), { Low, High });
	FStagePackageManifest TwoManifest = MakePackage(TwoTypes).Manifest;
	TestEqual(TEXT("Two types in order read"), ReadResult(Craft(TwoManifest, TwoTypes, FixturePayload({ { Low, Gdtf }, { High, Gdtf } }))), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Unsorted entries"), ReadResult(Craft(TwoManifest, TwoTypes, FixturePayload({ { High, Gdtf }, { Low, Gdtf } }))), Str(EStageMarketplaceResult::Corrupt));

	FStagePackageManifest Missing = Rig;
	Missing.FixtureTypes.Reset();
	TestEqual(TEXT("The scene uses a type the package does not carry"), ReadResult(Craft(Missing, RigScene, {})), Str(EStageMarketplaceResult::NotPortable));
	FStageSceneDocument PlainScene = StageTestFixtures::MakeScene(TEXT("Plain"));
	TestEqual(TEXT("A plain package crafted reads"), ReadResult(Craft(MakePackage(PlainScene).Manifest, PlainScene, {})), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("FIXT although the manifest names none"), ReadResult(Craft(MakePackage(PlainScene).Manifest, PlainScene, FixturePayload({ { Spot, Gdtf } }))), Str(EStageMarketplaceResult::Corrupt));
	FStagePackageManifest Extra = MakePackage(PlainScene).Manifest;
	Extra.FixtureTypes = { Spot };
	TestEqual(TEXT("A carried type no item uses"), ReadResult(Craft(Extra, PlainScene, FixturePayload({ { Spot, Gdtf } }))), Str(EStageMarketplaceResult::Corrupt));

	FStageSceneDocument ZeroScene = StageTestFixtures::MakeScene(TEXT("Zero"));
	FStageItemSnapshot& Zero = ZeroScene.Items.Add_GetRef(MakeLibraryItem(Spot));
	Zero.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Temp/StageCraftFixtures/00000000000000000000000000000000/Standard.Standard")));
	Zero.FixtureType = FStageFixtureTypeRef();
	TestFalse(TEXT("A library path with a zero type can never travel"), StagePackage::IsPortable(ZeroScene));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePackageFixtureFlowTest, "StageCraft.Marketplace.FixtureFlow", StageTestFixtures::Flags)

bool FStagePackageFixtureFlowTest::RunTest(const FString& Parameters)
{
	using namespace StagePackageFixtureTests;
	using namespace StageTestFixtures;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
	UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
	UStageTestMarketplaceBackend* Store = UStageTestMarketplaceBackend::Find(Game.Get());
	if (!TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Library"), Library) || !TestNotNull(TEXT("Store"), Store) || !TestNotNull(TEXT("Catalog"), Items))
	{
		return false;
	}
	TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
	Recorder->Bind(Game.Get());
	ON_SCOPE_EXIT { Recorder->Unbind(); };
	TestTrue(TEXT("The library and catalog are ready"), Game.Pump([Library, Items]() { return !Library->HasLoadsInProgress() && Items->IsCatalogLoaded(); }, 20.0));

	const FGuid Spot = SpotId();
	TestTrue(TEXT("The spot is read for review"), Library->RequestImportBytes(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), TEXT("My Spot.gdtf")).IsSuccess());
	TestEqual(TEXT("The same name again is refused while it waits"), Library->RequestImportBytes({}, TEXT("My Spot.gdtf")).Result, EStageFixtureImportResult::AlreadyInLibrary);
	TestEqual(TEXT("A name that is not a .gdtf file"), Library->RequestImportBytes({}, TEXT("notes.txt")).Result, EStageFixtureImportResult::UnsupportedFormat);
	TestEqual(TEXT("A path, not a name"), Library->RequestImportBytes({}, TEXT("../x.gdtf")).Result, EStageFixtureImportResult::UnsupportedFormat);
	TestTrue(TEXT("The review appears"), Game.Pump([Library]() { return Library->GetReviews().Num() == 1 && !Library->HasLoadsInProgress(); }, 10.0));
	if (Library->GetReviews().Num() != 1 || !TestTrue(TEXT("Added"), Library->ConfirmImport(Library->GetReviews()[0].ReviewId, {}).IsSuccess()))
	{
		return false;
	}

	TestTrue(TEXT("The library copy is written"), Game.Pump([Spot]() { return IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(Spot)); }, 10.0));
	const FString SceneName = TEXT("GDTF Rig");
	TestTrue(TEXT("A scene with the library spot is saved"), FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Save(MakeFixtureScene(SceneName, { Spot })) == EStageSceneResult::Success);
	FStageMarketplacePublishRequest Request;
	Request.SceneName = SceneName;
	Request.Title = SceneName;
	Request.Category = StageCraftTags::Marketplace_Category_Concert;

	AddInfo(TEXT("Publishing without the sharing confirmation is refused."));
	TestTrue(TEXT("Publish starts"), Marketplace->RequestPublish(Request).IsSuccess());
	TestTrue(TEXT("and ends"), Game.Pump([&Recorder]() { return Recorder->Publishes.Num() == 1; }, 10.0));
	TestTrue(TEXT("SharingNotConfirmed"), Recorder->Publishes.Num() == 1 && Recorder->Publishes[0].Result.Code == EStageMarketplaceResult::SharingNotConfirmed
		&& !Recorder->Publishes[0].Result.Message.IsEmpty());

	AddInfo(TEXT("Confirmed, the GDTF file travels in the package."));
	Request.bConfirmFixtureSharing = true;
	TestTrue(TEXT("Publish starts again"), Marketplace->RequestPublish(Request).IsSuccess());
	TestTrue(TEXT("and succeeds"), Game.Pump([&Recorder]() { return Recorder->Publishes.Num() == 2; }, 10.0) && Recorder->Publishes.Last().Result.IsSuccess());
	const FGuid ListingId = Recorder->Publishes.Num() == 2 ? Recorder->Publishes.Last().ListingId : FGuid();
	TArray<uint8> Published;
	FStagePackage Package;
	const FStageLocalMarketplaceStore Folder(Store->GetDirectory());
	TestTrue(TEXT("The package carries the spot's file from the library"), FFileHelper::LoadFileToArray(Published, *Folder.GetPackagePath(ListingId))
		&& StagePackage::Read(Published, Package) == EStageMarketplaceResult::Success && Package.FixtureFiles.Num() == 1 && Package.FixtureFiles[0].FixtureTypeId == Spot);

	AddInfo(TEXT("Another computer without the type: acquiring adds it to the library."));
	TestTrue(TEXT("The type is removed here"), Library->RemoveType(Spot).IsSuccess() && !Library->FindType(Spot));
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	TestTrue(TEXT("Acquire starts"), Marketplace->RequestAcquire(ListingId).IsSuccess());
	TestTrue(TEXT("and ends"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 1; }, 20.0));
	if (TestTrue(TEXT("in success"), Recorder->Acquires.Num() == 1 && Recorder->Acquires[0].Result.IsSuccess()))
	{
		TestTrue(*FString::Printf(TEXT("The message says the type was added (%s)"), *Recorder->Acquires[0].Result.Message.ToString()),
			Recorder->Acquires[0].Result.Message.ToString().Contains(TEXT("Added 1 GDTF fixture type")));
		TestTrue(TEXT("The spot is in the library again"), Library->FindType(Spot) != nullptr);
		TestTrue(TEXT("with its library copy"), Game.Pump([Spot]() { return IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(Spot)); }, 10.0));
		const FSoftObjectPath ModePath = StageFixtureLibrary::MakeItemPath(Spot, TEXT("Standard"));
		TestTrue(TEXT("The scene's fixture resolves in the catalog"), ModePath.ResolveObject() != nullptr);
	}

	AddInfo(TEXT("Acquiring again with the type present keeps the library's copy."));
	TestTrue(TEXT("Acquire again"), Marketplace->RequestAcquire(ListingId).IsSuccess());
	TestTrue(TEXT("ends"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 2; }, 20.0));
	TestTrue(TEXT("in success, with nothing added"), Recorder->Acquires.Num() == 2 && Recorder->Acquires[1].Result.IsSuccess()
		&& !Recorder->Acquires[1].Result.Message.ToString().Contains(TEXT("fixture type")) && Library->GetTypes().Num() == 1);
	return true;
}

#endif
