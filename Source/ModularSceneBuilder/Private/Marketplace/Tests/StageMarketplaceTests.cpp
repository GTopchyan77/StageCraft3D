// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the community marketplace (Docs/ADR/0021-community-marketplace.md): the .stage package round trip, refusal of damaged
// and hostile packages, manifest rules, browsing (filter, sort, page), listing ownership and purchase validation, imported scene
// names, and the local community folder in a temporary directory. No world, no purchases (Economy security 10).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Marketplace; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StageParameterTypes.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StagePackage.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace StageMarketplaceTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TCHAR* CratePath = TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate");
	const TCHAR* SpotPath = TEXT("/Game/StageCraft/Items/DA_Test_Spot.DA_Test_Spot");

	// Container layout (ADR 0021 §2.1), for tampering with packages.
	constexpr int32 FixedHeaderBytes = 16;
	constexpr int32 SectionHeaderBytes = 44;

	FString Str(EStageMarketplaceResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageEconomyResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageOwnershipState State) { return UEnum::GetValueAsString(State); }

	FStageItemSnapshot MakeItem(const TCHAR* Path, const FVector& Location, TArray<uint8> State = {})
	{
		FStageItemSnapshot Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(Path));
		Item.Transform = FTransform(FRotator(0, 30, 0).Quaternion(), Location);
		Item.DisplayName = FText::FromString(TEXT("Item"));
		Item.SavedProperties = MoveTemp(State);
		return Item;
	}

	FStageSceneDocument MakeScene()
	{
		FStageSceneDocument Scene;
		Scene.Name = TEXT("Festival Rig");
		Scene.LevelName = TEXT("L_StageTest");
		Scene.Items.Add(MakeItem(CratePath, FVector(100, 0, 0), { 1, 2, 3 }));
		Scene.Items.Add(MakeItem(SpotPath, FVector(0, 0, 600), { 9, 8, 7, 6 }));
		Scene.Items.Add(MakeItem(SpotPath, FVector(200, 0, 600)));

		FShowCue Cue;
		Cue.Number = 1.f;
		Cue.Label = FText::FromString(TEXT("Open"));
		Cue.FadeTime = 3.f;
		FShowCueFixtureState& Fixture = Cue.Fixtures.AddDefaulted_GetRef();
		Fixture.FixtureId = 1;
		Fixture.Attributes.Add(StageCraftTags::Attribute_Dimmer, 0.5f);
		Scene.Cues.Add(Cue);
		return Scene;
	}

	FStagePackageManifest MakeManifest(const FStageSceneDocument& Scene)
	{
		FStagePackageManifest Manifest;
		Manifest.PackageId = FGuid::NewGuid();
		Manifest.Title = TEXT("Festival Rig");
		Manifest.Description = TEXT("Two spots and a crate.\nReady to tour.");
		Manifest.AuthorName = TEXT("Gevor");
		Manifest.Kind = EStagePackageKind::LightingRig;
		Manifest.Category = StageCraftTags::Marketplace_Category_Festival;
		Manifest.Keywords = { TEXT("festival"), TEXT("spots") };
		Manifest.Price.Credits = 250;
		Manifest.CreatedAtUtc = FDateTime(2026, 11, 1, 12, 0, 0);
		Manifest.ItemCount = Scene.Items.Num();
		Manifest.FixtureCount = 2;
		Manifest.CueCount = Scene.Cues.Num();
		Manifest.RequiredItems = StagePackage::CollectRequiredItems(Scene);
		return Manifest;
	}

	TArray<uint8> MakePng()
	{
		return { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4 };
	}

	FStagePackage MakePackage(bool bWithThumbnail)
	{
		FStagePackage Package;
		Package.Scene = MakeScene();
		Package.Manifest = MakeManifest(Package.Scene);
		if (bWithThumbnail)
		{
			Package.ThumbnailPng = MakePng();
		}
		return Package;
	}

	TArray<uint8> WritePackage(const FStagePackage& Package)
	{
		TArray<uint8> Bytes;
		StagePackage::Write(Package, Bytes);
		return Bytes;
	}

	template <typename T>
	void Poke(TArray<uint8>& Bytes, int32 Offset, T Value)
	{
		FMemory::Memcpy(Bytes.GetData() + Offset, &Value, sizeof(T));
	}

	int32 SectionField(int32 Section, int32 FieldOffset)
	{
		return FixedHeaderBytes + Section * SectionHeaderBytes + FieldOffset;
	}

	FStageMarketplaceListing MakeListing(const TCHAR* Title, int64 Credits, int64 Downloads, int32 Day, EStagePackageKind Kind = EStagePackageKind::StageDesign)
	{
		FStageMarketplaceListing Listing;
		Listing.ListingId = FGuid::NewGuid();
		Listing.Title = Title;
		Listing.AuthorName = TEXT("Author");
		Listing.Kind = Kind;
		Listing.Category = StageCraftTags::Marketplace_Category_Concert;
		Listing.Price.Credits = Credits;
		Listing.Downloads = Downloads;
		Listing.PublishedAtUtc = FDateTime(2026, 10, Day);
		return Listing;
	}

	TArray<FString> Titles(const FStageMarketplaceQueryResult& Result)
	{
		TArray<FString> Out;
		for (const FStageMarketplaceListing& Listing : Result.Listings)
		{
			Out.Add(Listing.Title);
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplacePackageRoundTripTest, "StageCraft.Marketplace.Package.RoundTrip", StageMarketplaceTests::Flags)

bool FStageMarketplacePackageRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	const FStagePackage Package = MakePackage(/*bWithThumbnail*/ true);
	TArray<uint8> Bytes;
	TestEqual(TEXT("Writes"), Str(StagePackage::Write(Package, Bytes)), Str(EStageMarketplaceResult::Success));
	TestTrue(TEXT("Smaller than the scene JSON (compressed)"), Bytes.Num() < FStageSceneStore::Serialize(Package.Scene).Len());

	FStagePackage Read;
	if (!TestEqual(TEXT("Reads back"), Str(StagePackage::Read(Bytes, Read)), Str(EStageMarketplaceResult::Success)))
	{
		return false;
	}
	const FStagePackageManifest& In = Package.Manifest;
	const FStagePackageManifest& Out = Read.Manifest;
	TestEqual(TEXT("Id"), Out.PackageId, In.PackageId);
	TestEqual(TEXT("Title"), Out.Title, In.Title);
	TestEqual(TEXT("Description keeps its line break"), Out.Description, In.Description);
	TestEqual(TEXT("Author"), Out.AuthorName, In.AuthorName);
	TestEqual(TEXT("Kind"), Out.Kind, In.Kind);
	TestEqual(TEXT("Category"), Out.Category, In.Category);
	TestEqual(TEXT("Keywords"), Out.Keywords, In.Keywords);
	TestEqual(TEXT("Credits"), Out.Price.Credits, In.Price.Credits);
	TestEqual(TEXT("Created"), Out.CreatedAtUtc, In.CreatedAtUtc);
	TestEqual(TEXT("Counts"), FIntVector(Out.ItemCount, Out.FixtureCount, Out.CueCount), FIntVector(3, 2, 1));
	TestEqual(TEXT("Required items: two distinct paths"), Out.RequiredItems.Num(), 2);
	TestEqual(TEXT("Thumbnail kept byte for byte"), Read.ThumbnailPng, Package.ThumbnailPng);

	if (TestEqual(TEXT("All items"), Read.Scene.Items.Num(), 3))
	{
		TestEqual(TEXT("Instance id kept"), Read.Scene.Items[1].InstanceId, Package.Scene.Items[1].InstanceId);
		TestEqual(TEXT("Instance state (fixture cells, patch, span geometry) kept byte for byte"), Read.Scene.Items[1].SavedProperties, Package.Scene.Items[1].SavedProperties);
		TestTrue(TEXT("Transform kept"), Read.Scene.Items[1].Transform.Equals(Package.Scene.Items[1].Transform, 1.e-6));
	}
	if (TestEqual(TEXT("Cue list"), Read.Scene.Cues.Num(), 1))
	{
		TestEqual(TEXT("Cue attribute"), Read.Scene.Cues[0].Fixtures[0].Attributes.FindRef(StageCraftTags::Attribute_Dimmer), 0.5f);
	}

	FStagePackageManifest Browsed;
	bool bHasThumbnail = false;
	TestEqual(TEXT("Manifest alone reads"), Str(StagePackage::ReadManifest(Bytes, Browsed, bHasThumbnail)), Str(EStageMarketplaceResult::Success));
	TestTrue(TEXT("Browsing sees the thumbnail"), bHasThumbnail);
	TestEqual(TEXT("Browsed title"), Browsed.Title, In.Title);

	// Writing refuses a manifest that does not describe its scene: browsers would be shown the wrong stage.
	FStagePackage Lying = Package;
	Lying.Manifest.ItemCount = 99;
	TArray<uint8> Unused;
	TestEqual(TEXT("Wrong item count refused"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));
	Lying = Package;
	Lying.Manifest.RequiredItems.Pop();
	TestEqual(TEXT("Wrong required items refused"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));
	Lying = Package;
	Lying.ThumbnailPng = { 'G', 'I', 'F', '8', '9', 'a', 0, 0, 0 };
	TestEqual(TEXT("Non-PNG thumbnail refused"), Str(StagePackage::Write(Lying, Unused)), Str(EStageMarketplaceResult::InvalidRequest));

	// Imported models exist only where they were imported.
	FStagePackage Local = Package;
	Local.Scene.Items.Add(MakeItem(TEXT("/Temp/StageCraft/Imported/Model_1/DA_Model.DA_Model"), FVector::ZeroVector));
	Local.Manifest.ItemCount = Local.Scene.Items.Num();
	Local.Manifest.RequiredItems = StagePackage::CollectRequiredItems(Local.Scene);
	TestFalse(TEXT("Scene with an imported model is not portable"), StagePackage::IsPortable(Local.Scene));
	TestEqual(TEXT("Imported model refused"), Str(StagePackage::Write(Local, Unused)), Str(EStageMarketplaceResult::NotPortable));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplacePackageUntrustedTest, "StageCraft.Marketplace.Package.Untrusted", StageMarketplaceTests::Flags)

bool FStageMarketplacePackageUntrustedTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	const TArray<uint8> Good = WritePackage(MakePackage(/*bWithThumbnail*/ false));
	FStagePackage Read;
	const auto ReadResult = [&Read](const TArray<uint8>& Bytes) { return Str(StagePackage::Read(Bytes, Read)); };

	TestEqual(TEXT("Empty"), ReadResult({}), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Not a package"), ReadResult({ 'P', 'K', 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }), Str(EStageMarketplaceResult::Corrupt));

	TArray<uint8> Bytes = Good;
	Poke<uint32>(Bytes, 8, 2);
	TestEqual(TEXT("Newer container"), ReadResult(Bytes), Str(EStageMarketplaceResult::VersionMismatch));

	Bytes = Good;
	Poke<uint32>(Bytes, 12, 0);
	TestEqual(TEXT("No sections"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));
	Poke<uint32>(Bytes, 12, 9);
	TestEqual(TEXT("Too many sections"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));

	Bytes = Good;
	Bytes.Pop();
	TestEqual(TEXT("Truncated"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));
	Bytes = Good;
	Bytes.Add(0);
	TestEqual(TEXT("Trailing bytes"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));

	Bytes = Good;
	Poke<uint64>(Bytes, SectionField(0, 8), MAX_uint64);
	TestEqual(TEXT("Section running past the end (wrapping size)"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));

	Bytes = Good;
	Poke<uint32>(Bytes, SectionField(1, 0), *reinterpret_cast<const uint32*>(Good.GetData() + SectionField(0, 0)));
	TestEqual(TEXT("Repeated section"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));

	Bytes = Good;
	Poke<uint32>(Bytes, SectionField(0, 4), 7);
	TestEqual(TEXT("Unknown codec"), ReadResult(Bytes), Str(EStageMarketplaceResult::Corrupt));

	// A size claim over the cap is refused before anything is allocated (decompression bombs).
	Bytes = Good;
	Poke<uint64>(Bytes, SectionField(0, 16), static_cast<uint64>(StagePackage::MaxManifestBytes) + 1);
	TestEqual(TEXT("Manifest raw size over its cap"), ReadResult(Bytes), Str(EStageMarketplaceResult::TooLarge));
	Bytes = Good;
	Poke<uint64>(Bytes, SectionField(1, 16), static_cast<uint64>(StagePackage::MaxSceneBytes) + 1);
	TestEqual(TEXT("Scene raw size over its cap"), ReadResult(Bytes), Str(EStageMarketplaceResult::TooLarge));

	Bytes = Good;
	Bytes[SectionField(0, 24)] ^= 0xFF;
	TestEqual(TEXT("Manifest hash differs"), ReadResult(Bytes), Str(EStageMarketplaceResult::HashMismatch));

	// The scene is the last section here; damage to it must fail the full read but not browsing, which never inflates it.
	Bytes = Good;
	Bytes.Last() ^= 0xFF;
	// The engine logs its own zlib failure for the damaged stream (warning + error); that is the refusal, not a test fault.
	AddExpectedMessage(TEXT("appUncompressMemoryZLIB failed"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessage(TEXT("Failed to uncompress memory"), EAutomationExpectedMessageFlags::Contains, 1);
	TestNotEqual(TEXT("Damaged scene refused"), ReadResult(Bytes), Str(EStageMarketplaceResult::Success));
	FStagePackageManifest Manifest;
	bool bHasThumbnail = false;
	TestEqual(TEXT("Browsing a package with a damaged scene still reads its manifest"), Str(StagePackage::ReadManifest(Bytes, Manifest, bHasThumbnail)),
		Str(EStageMarketplaceResult::Success));

	// A whole file over the package cap is refused without being parsed.
	TArray<uint8> Huge;
	Huge.SetNumZeroed(static_cast<int32>(StagePackage::MaxPackageBytes) + 1);
	TestEqual(TEXT("Package over its cap"), ReadResult(Huge), Str(EStageMarketplaceResult::TooLarge));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceManifestTest, "StageCraft.Marketplace.Manifest", StageMarketplaceTests::Flags)

bool FStageMarketplaceManifestTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	FStagePackageManifest Manifest = MakeManifest(MakeScene());
	Manifest.Title = TEXT("  Festival\x01 Rig  ");
	Manifest.AuthorName = TEXT("   ");
	Manifest.Keywords = { TEXT(" Spots "), TEXT("spots"), TEXT(""), TEXT("LED") };
	Manifest.Category = StageCraftTags::Marketplace_Category;
	Manifest.Price.RealMoneyMinorUnits = 499;
	Manifest.Price.RealMoneyCurrency = TEXT(" usd ");
	StagePackage::NormalizeManifest(Manifest);
	TestEqual(TEXT("Title trimmed, control characters removed"), Manifest.Title, FString(TEXT("Festival Rig")));
	TestEqual(TEXT("Empty author is Anonymous"), Manifest.AuthorName, FString(TEXT("Anonymous")));
	TestEqual(TEXT("Keywords trimmed, empty and repeated ones dropped"), Manifest.Keywords, TArray<FString>{ TEXT("Spots"), TEXT("LED") });
	TestEqual(TEXT("The root category becomes Other"), Manifest.Category, StageCraftTags::Marketplace_Category_Other.GetTag());
	TestEqual(TEXT("Currency code upper-cased"), Manifest.Price.RealMoneyCurrency, FString(TEXT("USD")));
	TestEqual(TEXT("Normalized manifest is valid"), Str(StagePackage::ValidateManifest(Manifest)), Str(EStageMarketplaceResult::Success));

	const auto Invalid = [this, &Manifest](const TCHAR* What, TFunctionRef<void(FStagePackageManifest&)> Change)
	{
		FStagePackageManifest Copy = Manifest;
		Change(Copy);
		TestEqual(What, Str(StagePackage::ValidateManifest(Copy)), Str(EStageMarketplaceResult::InvalidRequest));
	};
	Invalid(TEXT("No id"), [](FStagePackageManifest& M) { M.PackageId.Invalidate(); });
	Invalid(TEXT("Empty title"), [](FStagePackageManifest& M) { M.Title.Reset(); });
	Invalid(TEXT("Title over the limit (never cut silently)"), [](FStagePackageManifest& M) { M.Title = FString::ChrN(StagePackage::MaxTitleLength + 1, TEXT('a')); });
	Invalid(TEXT("Description over the limit"), [](FStagePackageManifest& M) { M.Description = FString::ChrN(StagePackage::MaxDescriptionLength + 1, TEXT('a')); });
	Invalid(TEXT("Too many keywords"), [](FStagePackageManifest& M) { M.Keywords.Init(TEXT("k"), StagePackage::MaxKeywords + 1); });
	Invalid(TEXT("Negative credits"), [](FStagePackageManifest& M) { M.Price.Credits = -1; });
	Invalid(TEXT("Credits over the limit"), [](FStagePackageManifest& M) { M.Price.Credits = StagePackage::MaxCreditsPrice + 1; });
	Invalid(TEXT("Real money without a currency"), [](FStagePackageManifest& M) { M.Price.RealMoneyCurrency.Reset(); });
	Invalid(TEXT("Malformed currency"), [](FStagePackageManifest& M) { M.Price.RealMoneyCurrency = TEXT("US1"); });
	Invalid(TEXT("More fixtures than items"), [](FStagePackageManifest& M) { M.FixtureCount = M.ItemCount + 1; });
	Invalid(TEXT("Not a marketplace category"), [](FStagePackageManifest& M) { M.Category = StageCraftTags::Attribute_Dimmer; });

	// Parsing: unknown kinds are refused, newer manifests are a version mismatch, unknown categories read as Other.
	const FString Json = StagePackage::SerializeManifest(MakeManifest(MakeScene()));
	FStagePackageManifest Parsed;
	TestEqual(TEXT("Serialized manifest parses"), Str(StagePackage::ParseManifest(Json, Parsed)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Unknown kind"), Str(StagePackage::ParseManifest(Json.Replace(TEXT("\"LightingRig\""), TEXT("\"Hologram\"")), Parsed)), Str(EStageMarketplaceResult::Corrupt));
	TestEqual(TEXT("Newer manifest"), Str(StagePackage::ParseManifest(Json.Replace(TEXT("\"version\":1"), TEXT("\"version\":2")), Parsed)), Str(EStageMarketplaceResult::VersionMismatch));
	TestEqual(TEXT("Unknown category parses"), Str(StagePackage::ParseManifest(Json.Replace(TEXT("Category.Festival"), TEXT("Category.Circus")), Parsed)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("... as Other"), Parsed.Category, StageCraftTags::Marketplace_Category_Other.GetTag());
	TestEqual(TEXT("Not JSON"), Str(StagePackage::ParseManifest(TEXT("{"), Parsed)), Str(EStageMarketplaceResult::Corrupt));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceQueryTest, "StageCraft.Marketplace.Query", StageMarketplaceTests::Flags)

bool FStageMarketplaceQueryTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	TArray<FStageMarketplaceListing> All;
	All.Add(MakeListing(TEXT("Arena Tour"), 500, 40, 3));
	All.Add(MakeListing(TEXT("Club Night"), 0, 90, 5, EStagePackageKind::LightingRig));
	All.Add(MakeListing(TEXT("Theatre Cues"), 100, 10, 9, EStagePackageKind::CueList));
	All.Add(MakeListing(TEXT("Beach Festival"), 0, 40, 1));
	All[3].Keywords = { TEXT("sunset"), TEXT("open air") };
	All[3].Category = StageCraftTags::Marketplace_Category_Festival;

	FStageMarketplaceQuery Query;
	TestEqual(TEXT("Popular: most downloads, ties by title"), Titles(StageMarketplaceRules::Apply(All, Query)),
		TArray<FString>{ TEXT("Club Night"), TEXT("Arena Tour"), TEXT("Beach Festival"), TEXT("Theatre Cues") });

	Query.Sort = EStageMarketplaceSort::Newest;
	TestEqual(TEXT("Newest first"), Titles(StageMarketplaceRules::Apply(All, Query))[0], FString(TEXT("Theatre Cues")));
	Query.Sort = EStageMarketplaceSort::PriceLowToHigh;
	TestEqual(TEXT("Cheapest first, free ties by title"), Titles(StageMarketplaceRules::Apply(All, Query)),
		TArray<FString>{ TEXT("Beach Festival"), TEXT("Club Night"), TEXT("Theatre Cues"), TEXT("Arena Tour") });
	Query.Sort = EStageMarketplaceSort::PriceHighToLow;
	TestEqual(TEXT("Dearest first"), Titles(StageMarketplaceRules::Apply(All, Query))[0], FString(TEXT("Arena Tour")));

	Query = FStageMarketplaceQuery();
	Query.SearchText = TEXT("  OPEN   sunset ");
	TestEqual(TEXT("Every word must match (keywords, any case)"), Titles(StageMarketplaceRules::Apply(All, Query)), TArray<FString>{ TEXT("Beach Festival") });
	Query.SearchText = TEXT("open arena");
	TestEqual(TEXT("A word that matches nothing excludes"), StageMarketplaceRules::Apply(All, Query).TotalMatches, 0);

	Query = FStageMarketplaceQuery();
	Query.Kinds = { EStagePackageKind::LightingRig, EStagePackageKind::CueList };
	TestEqual(TEXT("Kind filter"), StageMarketplaceRules::Apply(All, Query).TotalMatches, 2);
	Query = FStageMarketplaceQuery();
	Query.Category = StageCraftTags::Marketplace_Category_Festival;
	TestEqual(TEXT("Category filter"), Titles(StageMarketplaceRules::Apply(All, Query)), TArray<FString>{ TEXT("Beach Festival") });
	Query = FStageMarketplaceQuery();
	Query.PriceFilter = EStageMarketplacePriceFilter::Free;
	TestEqual(TEXT("Free only"), StageMarketplaceRules::Apply(All, Query).TotalMatches, 2);
	Query.PriceFilter = EStageMarketplacePriceFilter::Paid;
	TestEqual(TEXT("Paid only"), StageMarketplaceRules::Apply(All, Query).TotalMatches, 2);

	Query = FStageMarketplaceQuery();
	Query.Offset = 1;
	Query.Limit = 2;
	FStageMarketplaceQueryResult Page = StageMarketplaceRules::Apply(All, Query);
	TestEqual(TEXT("Page holds Limit listings"), Titles(Page), TArray<FString>{ TEXT("Arena Tour"), TEXT("Beach Festival") });
	TestEqual(TEXT("Total counts every match"), Page.TotalMatches, 4);
	Query.Offset = 10;
	TestEqual(TEXT("Page past the end is empty"), StageMarketplaceRules::Apply(All, Query).Listings.Num(), 0);
	Query.Offset = -5;
	Query.Limit = 100000;
	TestEqual(TEXT("Bad paging is clamped"), StageMarketplaceRules::Apply(All, Query).Listings.Num(), 4);

	// Real money sorts after credits: the two cannot be compared.
	TArray<FStageMarketplaceListing> Mixed = { MakeListing(TEXT("Money"), 0, 0, 1), MakeListing(TEXT("Credits"), 900, 0, 1) };
	Mixed[0].Price.RealMoneyMinorUnits = 100;
	Mixed[0].Price.RealMoneyCurrency = TEXT("USD");
	Query = FStageMarketplaceQuery();
	Query.Sort = EStageMarketplaceSort::PriceLowToHigh;
	TestEqual(TEXT("Real money after credits"), Titles(StageMarketplaceRules::Apply(Mixed, Query)), TArray<FString>{ TEXT("Credits"), TEXT("Money") });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplacePurchaseRulesTest, "StageCraft.Marketplace.PurchaseRules", StageMarketplaceTests::Flags)

bool FStageMarketplacePurchaseRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	FStagePlayerProfile Profile;
	Profile.Balances.Add(StageCraftTags::Currency_Credits, 300);
	const FStageMarketplaceListing Paid = MakeListing(TEXT("Paid"), 250, 0, 1);
	const FStageMarketplaceListing Dear = MakeListing(TEXT("Dear"), 301, 0, 1);
	const FStageMarketplaceListing Free = MakeListing(TEXT("Free"), 0, 0, 1);
	FStageMarketplaceListing Money = MakeListing(TEXT("Money"), 0, 0, 1);
	Money.Price.RealMoneyMinorUnits = 499;
	Money.Price.RealMoneyCurrency = TEXT("USD");

	FStageListingPurchaseContext Context;
	Context.Profile = &Profile;
	Context.bIntegrityValid = true;
	const auto Check = [&Context](const FStageMarketplaceListing* Listing) { Context.Listing = Listing; return Str(StageMarketplaceRules::ValidateListingPurchase(Context).Code); };

	TestEqual(TEXT("Paid listing with enough credits"), Check(&Paid), Str(EStageEconomyResult::Success));
	TestEqual(TEXT("Unknown listing (deny by default)"), Check(nullptr), Str(EStageEconomyResult::UnknownProduct));
	TestEqual(TEXT("Free listing needs no purchase"), Check(&Free), Str(EStageEconomyResult::InvalidRequest));
	TestEqual(TEXT("Not enough credits"), Check(&Dear), Str(EStageEconomyResult::InsufficientFunds));
	TestEqual(TEXT("Real money offline"), Check(&Money), Str(EStageEconomyResult::ServerRequired));
	Context.bCanSettleRealMoney = true;
	TestEqual(TEXT("Real money with a server backend"), Check(&Money), Str(EStageEconomyResult::Success));
	Context.bCanSettleRealMoney = false;

	Context.bPending = true;
	TestEqual(TEXT("Already in flight"), Check(&Paid), Str(EStageEconomyResult::PurchasePending));
	Context.bPending = false;
	Context.bIntegrityValid = false;
	TestEqual(TEXT("Frozen profile"), Check(&Paid), Str(EStageEconomyResult::IntegrityViolation));
	Context.bIntegrityValid = true;
	Context.Profile = nullptr;
	TestEqual(TEXT("No profile"), Check(&Paid), Str(EStageEconomyResult::InvalidRequest));
	Context.Profile = &Profile;

	Profile.OwnedListings.Add(Paid.ListingId);
	TestEqual(TEXT("Already owned"), Check(&Paid), Str(EStageEconomyResult::AlreadyOwned));

	TestEqual(TEXT("Ownership: free"), Str(StageMarketplaceRules::GetOwnership(Free, Profile, true, false)), Str(EStageOwnershipState::Free));
	TestEqual(TEXT("Ownership: owned"), Str(StageMarketplaceRules::GetOwnership(Paid, Profile, true, false)), Str(EStageOwnershipState::Owned));
	TestEqual(TEXT("Ownership: purchasable"), Str(StageMarketplaceRules::GetOwnership(Dear, Profile, true, false)), Str(EStageOwnershipState::Purchasable));
	TestEqual(TEXT("Ownership: real money offline is locked"), Str(StageMarketplaceRules::GetOwnership(Money, Profile, true, false)), Str(EStageOwnershipState::Locked));
	TestEqual(TEXT("Ownership: frozen profile is locked"), Str(StageMarketplaceRules::GetOwnership(Dear, Profile, false, false)), Str(EStageOwnershipState::Locked));

	const FPrimaryAssetId ProductId = StageMarketplaceRules::MakeListingProductId(Paid.ListingId);
	TestEqual(TEXT("Listing product type"), ProductId.PrimaryAssetType.GetName(), FName(StageMarketplaceRules::ListingProductType));
	TestNotEqual(TEXT("Each listing has its own product id"), ProductId, StageMarketplaceRules::MakeListingProductId(Dear.ListingId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceSceneNameTest, "StageCraft.Marketplace.SceneName", StageMarketplaceTests::Flags)

bool FStageMarketplaceSceneNameTest::RunTest(const FString& Parameters)
{
	TSet<FString> Taken;
	const auto Exists = [&Taken](const FString& Name) { return Taken.Contains(Name); };

	TestEqual(TEXT("A clean title is the name"), StageMarketplaceRules::MakeImportSceneName(TEXT("Festival Rig"), Exists), FString(TEXT("Festival Rig")));
	TestEqual(TEXT("Disallowed characters removed"), StageMarketplaceRules::MakeImportSceneName(TEXT("Rock & Roll: <Live>!"), Exists), FString(TEXT("Rock Roll Live")));
	TestEqual(TEXT("Nothing usable falls back"), StageMarketplaceRules::MakeImportSceneName(TEXT("***"), Exists), FString(TEXT("Marketplace Stage")));
	TestEqual(TEXT("Reserved device names fall back"), StageMarketplaceRules::MakeImportSceneName(TEXT("CON"), Exists), FString(TEXT("Marketplace Stage")));
	TestTrue(TEXT("Long titles fit the name limit"), FStageSceneStore::IsValidSceneName(StageMarketplaceRules::MakeImportSceneName(FString::ChrN(80, TEXT('x')), Exists)));

	Taken = { TEXT("Festival Rig"), TEXT("Festival Rig (2)") };
	TestEqual(TEXT("Never replaces a scene: next free suffix"), StageMarketplaceRules::MakeImportSceneName(TEXT("Festival Rig"), Exists), FString(TEXT("Festival Rig (3)")));
	TestEqual(TEXT("Exhausted names give empty"), StageMarketplaceRules::MakeImportSceneName(TEXT("X"), [](const FString&) { return true; }), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceLocalStoreTest, "StageCraft.Marketplace.LocalStore", StageMarketplaceTests::Flags)

bool FStageMarketplaceLocalStoreTest::RunTest(const FString& Parameters)
{
	using namespace StageMarketplaceTests;

	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MarketplaceTests"), FGuid::NewGuid().ToString());
	const FStageLocalMarketplaceStore Store(Directory);

	FStageMarketplaceQueryResult Page;
	TestEqual(TEXT("Missing folder is an empty marketplace"), Str(Store.Query(FStageMarketplaceQuery(), Page)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("... with no listings"), Page.TotalMatches, 0);

	const FStagePackage Package = MakePackage(/*bWithThumbnail*/ true);
	const TArray<uint8> Bytes = WritePackage(Package);
	FStageMarketplaceListing Listing;
	TestEqual(TEXT("Publishes"), Str(Store.Publish(Bytes, Listing)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Listing id is the package id"), Listing.ListingId, Package.Manifest.PackageId);
	TestEqual(TEXT("Same id again"), Str(Store.Publish(Bytes, Listing)), Str(EStageMarketplaceResult::AlreadyExists));
	TArray<uint8> Damaged = Bytes;
	Damaged.Pop();
	TestEqual(TEXT("Damaged package refused"), Str(Store.Publish(Damaged, Listing)), Str(EStageMarketplaceResult::Corrupt));

	const FStagePackage Second = MakePackage(/*bWithThumbnail*/ false);
	TestEqual(TEXT("Second package"), Str(Store.Publish(WritePackage(Second), Listing)), Str(EStageMarketplaceResult::Success));

	// Files the store did not write: garbage, and a copy renamed to another id (it would be bought under one id and owned under another).
	FFileHelper::SaveStringToFile(TEXT("not a package"), *FPaths::Combine(Directory, FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".stage")));
	IFileManager::Get().Copy(*FPaths::Combine(Directory, FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".stage")), *Store.GetPackagePath(Package.Manifest.PackageId));
	// Two skipped files, seen by each of the two queries below.
	AddExpectedMessage(TEXT("Marketplace: skipped"), EAutomationExpectedMessageFlags::Contains, 4);

	TestEqual(TEXT("Query"), Str(Store.Query(FStageMarketplaceQuery(), Page)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Only the two real listings"), Page.TotalMatches, 2);

	TArray<uint8> Fetched;
	TestEqual(TEXT("Preview fetch"), Str(Store.Fetch(Package.Manifest.PackageId, /*bCountDownload*/ false, Fetched)), Str(EStageMarketplaceResult::Success));
	TestEqual(TEXT("Fetched bytes are the package"), Fetched, Bytes);
	TestEqual(TEXT("A preview is not a download"), Store.GetDownloads(Package.Manifest.PackageId), int64(0));
	Store.Fetch(Package.Manifest.PackageId, /*bCountDownload*/ true, Fetched);
	Store.Fetch(Package.Manifest.PackageId, /*bCountDownload*/ true, Fetched);
	TestEqual(TEXT("Acquisitions count"), Store.GetDownloads(Package.Manifest.PackageId), int64(2));
	TestEqual(TEXT("Unknown listing"), Str(Store.Fetch(FGuid::NewGuid(), true, Fetched)), Str(EStageMarketplaceResult::NotFound));

	Store.Query(FStageMarketplaceQuery(), Page);
	if (TestEqual(TEXT("Both listed"), Page.Listings.Num(), 2))
	{
		TestEqual(TEXT("Most downloaded first"), Page.Listings[0].ListingId, Package.Manifest.PackageId);
		TestEqual(TEXT("Downloads shown"), Page.Listings[0].Downloads, int64(2));
		TestTrue(TEXT("Thumbnail flagged"), Page.Listings[0].bHasThumbnail);
		TestEqual(TEXT("Package size shown"), Page.Listings[0].PackageBytes, int64(Bytes.Num()));
	}

	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
