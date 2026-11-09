// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageFixtureLibraryView.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "History/StageEditCommand.h"
#include "History/StageItemCommands.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tests/StageTestGameInstance.h"
#include "UObject/Package.h"

namespace StageFixtureLibraryUITests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FGuid SpotTypeId()
	{
		FGuid Id;
		FGuid::Parse(StageGdtfTestFiles::SpotId, Id);
		return Id;
	}

	FString MakeSource(const FString& Root, const TCHAR* Name)
	{
		const FString File = FPaths::Combine(Root, TEXT("Downloads"), Name);
		FFileHelper::SaveArrayToFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()), *File);
		return File;
	}

	const FStageFixtureImportReview* ReadOne(FStageTestGameInstance& Game, UStageFixtureLibrarySubsystem& Library, const FString& File)
	{
		const int32 Before = Library.GetReviews().Num();
		if (!Library.RequestImport({ File }).IsEmpty() || !Game.Pump([&Library, Before]() { return Library.GetReviews().Num() > Before && !Library.HasLoadsInProgress(); }, 20.0))
		{
			return nullptr;
		}
		return &Library.GetReviews().Last();
	}

	ALightingFixtureActor* Place(UWorld& World, ULightingFixtureData* Data)
	{
		ALightingFixtureActor* Fixture = World.SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Fixture)
		{
			return nullptr;
		}
		Fixture->InitializeFromItemData(Data);
		Fixture->FinishSpawning(FTransform::Identity);
		FStageDMXPatch Patch;
		Patch.Universe = 2;
		Patch.Address = 101;
		Fixture->SetPatch(Patch);
		return Fixture;
	}

	TArray<FText> TypeOptions(const AModularBaseActor& Actor)
	{
		for (const FStageParameterSection& Section : IStageParameterInterface::Execute_GetParameterSections(&Actor))
		{
			for (const FStageParameterDescriptor& Descriptor : Section.Parameters)
			{
				if (Descriptor.Id == StageCraftTags::Param_Info_Type)
				{
					return Descriptor.Options;
				}
			}
		}
		return {};
	}

	bool HasRow(const AModularBaseActor& Actor, const FGameplayTag& Id, FString& OutText)
	{
		for (const FStageParameterSection& Section : IStageParameterInterface::Execute_GetParameterSections(&Actor))
		{
			for (const FStageParameterDescriptor& Descriptor : Section.Parameters)
			{
				if (Descriptor.Id == Id)
				{
					OutText = Descriptor.Value.Text.ToString();
					return true;
				}
			}
		}
		return false;
	}

	class FTestItemEditor final : public IStageItemEditor
	{
	public:
		explicit FTestItemEditor(AModularBaseActor& InItem) : Item(&InItem) {}

		virtual bool CaptureItem(const FGuid& InstanceId, FStageItemSnapshot& OutSnapshot) const override { return false; }
		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot& Snapshot) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult RemoveItem(const FGuid& InstanceId) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemTransform(const FGuid& InstanceId, const FTransform& Transform) override { return EStageCommandResult::Invalid; }
		virtual EStageCommandResult SetItemParameter(const FGuid& InstanceId, const FGameplayTag& ParameterId, const FStageParameterValue& Value) override
		{
			AModularBaseActor* Live = Item.Get();
			if (!Live || Live->GetInstanceId() != InstanceId)
			{
				return EStageCommandResult::Invalid;
			}
			return IStageParameterInterface::Execute_SetParameterValue(Live, ParameterId, Value) ? EStageCommandResult::Succeeded : EStageCommandResult::Invalid;
		}

	private:
		TWeakObjectPtr<AModularBaseActor> Item;
	};

	ULightingFixtureData* MakeMode(const FGuid& TypeId, const TCHAR* Name, const TCHAR* Mode)
	{
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage(), NAME_None, RF_Transient);
		Data->DisplayName = FText::FromString(Name);
		Data->FixtureTypeId = TypeId;
		Data->SourceTypeName = TEXT("Mega Spot");
		Data->Specs.Manufacturer = FText::FromString(TEXT("Acme"));
		Data->DMXModeName = Mode;
		return Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureImportReportTextTest, "StageCraft.UI.FixtureImportReport", StageFixtureLibraryUITests::Flags)

bool FStageFixtureImportReportTextTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryView;

	FStageFixtureImportReview Review;
	Review.ReviewId = FGuid::NewGuid();
	Review.SourceFile = TEXT("C:/Downloads/Acme Mega Spot.gdtf");
	Review.FixtureTypeId = FGuid::NewGuid();
	Review.Manufacturer = TEXT("Acme");
	Review.Name = TEXT("Mega Spot");
	Review.bCanImport = true;
	FStageFixtureModeSummary& Standard = Review.Modes.AddDefaulted_GetRef();
	Standard.Name = TEXT("Standard");
	Standard.Footprint = 15;
	Standard.Mapped = 11;
	Standard.Approximated = 1;
	Standard.Defaulted = 3;
	Standard.ApproximatedNames = { TEXT("Blade1A") };
	Standard.DefaultedNames = { TEXT("Focus1"), TEXT("Control1") };
	Standard.bUsable = true;
	FStageFixtureModeSummary& Broken = Review.Modes.AddDefaulted_GetRef();
	Broken.Name = TEXT("Extended");
	Broken.Footprint = 40;
	Broken.Reason = TEXT("a DMX break other than 1");
	Review.Warnings.Add(TEXT("CTB is kept at its default"));

	FReviewText Text = DescribeReview(Review);
	TestEqual(TEXT("Title: manufacturer and name"), Text.Title.ToString(), FString(TEXT("Acme Mega Spot")));
	TestEqual(TEXT("Source: manufacturer and file"), Text.Source.ToString(), FString(TEXT("Manufacturer: Acme · Acme Mega Spot.gdtf")));
	TestTrue(TEXT("Adds the usable modes only"), Text.Summary.ToString().StartsWith(TEXT("Adds 1 mode to the Library")));
	TestEqual(TEXT("ADD for a new type"), Text.ConfirmLabel.ToString(), FString(TEXT("ADD")));
	TestTrue(TEXT("Can be added"), Text.bCanConfirm);
	if (TestEqual(TEXT("A row per mode"), Text.Modes.Num(), 2))
	{
		TestEqual(TEXT("Mode name with footprint"), Text.Modes[0].Name.ToString(), FString(TEXT("Standard (15 ch)")));
		TestEqual(TEXT("Mapping breakdown"), Text.Modes[0].Detail.ToString(), FString(TEXT("11 mapped · 1 approximated · 3 at default")));
		TestTrue(TEXT("Approximated and default channels are named"), Text.Modes[0].Notes.Num() == 2
			&& Text.Modes[0].Notes[0].ToString() == TEXT("Approximated: Blade1A") && Text.Modes[0].Notes[1].ToString() == TEXT("At default: Focus1, Control1 +1"));
		TestTrue(TEXT("An unusable mode says why"), !Text.Modes[1].bUsable && Text.Modes[1].Detail.ToString() == TEXT("Not added: a DMX break other than 1"));
	}
	TestTrue(TEXT("Warnings are shown"), Text.Warnings.Num() == 1);

	Review.bReplaces = true;
	Text = DescribeReview(Review);
	TestEqual(TEXT("REPLACE for a known type"), Text.ConfirmLabel.ToString(), FString(TEXT("REPLACE")));
	TestTrue(TEXT("The summary names what is replaced"), Text.Summary.ToString().StartsWith(TEXT("Replaces Acme Mega Spot in the Library: 1 mode.")));

	Review.bCanImport = false;
	Review.Errors.Add(TEXT("line 3: FixtureTypeID is missing"));
	Text = DescribeReview(Review);
	TestFalse(TEXT("A refused file cannot be confirmed"), Text.bCanConfirm);
	TestTrue(TEXT("and shows its error"), Text.Errors.Num() == 1);

	FStageFixtureImportReview Unread;
	Unread.SourceFile = TEXT("C:/Downloads/Broken.gdtf");
	Unread.Errors.Add(TEXT("not a ZIP archive"));
	Text = DescribeReview(Unread);
	TestEqual(TEXT("A file without a type is titled by its name"), Text.Title.ToString(), FString(TEXT("Broken.gdtf")));
	TestFalse(TEXT("and cannot be confirmed"), Text.bCanConfirm);

	AddInfo(TEXT("Library cards: one per fixture type."));
	using namespace StageFixtureLibraryUITests;
	const FGuid TypeId = FGuid::NewGuid();
	ULightingFixtureData* ModeA = MakeMode(TypeId, TEXT("Acme Mega Spot (Standard)"), TEXT("Standard"));
	ULightingFixtureData* ModeB = MakeMode(TypeId, TEXT("Acme Mega Spot (Basic)"), TEXT("Basic"));
	UBaseItemData* Prop = NewObject<UBaseItemData>(GetTransientPackage(), NAME_None, RF_Transient);
	UBaseItemData* Other = NewObject<UBaseItemData>(GetTransientPackage(), NAME_None, RF_Transient);
	FStageFixtureLibraryType Type;
	Type.FixtureTypeId = TypeId;
	Type.Modes = { ModeA, ModeB };
	const TArray<UBaseItemData*> Catalog = { Prop, ModeB, Other, nullptr, ModeA };
	const TArray<FCard> Cards = MakeCards(Catalog, { Type });
	if (TestEqual(TEXT("Three cards: the two modes are one"), Cards.Num(), 3))
	{
		TestTrue(TEXT("Catalog order kept"), Cards[0].Item == Prop && Cards[2].Item == Other && !Cards[0].IsFixtureType());
		TestTrue(TEXT("The type card stands where its first catalog mode was and places the first mode"), Cards[1].IsFixtureType() && Cards[1].Item == ModeA
			&& Cards[1].FixtureTypeId == TypeId && Cards[1].ModeCount == 2);
		TestEqual(TEXT("Card name"), Cards[1].Name.ToString(), FString(TEXT("Acme Mega Spot")));
		TestEqual(TEXT("Card detail"), Cards[1].Detail.ToString(), FString(TEXT("GDTF · 2 modes")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureImportFlowTest, "StageCraft.Fixtures.ImportFlow", StageFixtureLibraryUITests::Flags)

bool FStageFixtureImportFlowTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureLibraryUITests;

	FStageTestGameInstance Game;
	UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
	UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid() && Library && Items))
	{
		return false;
	}
	TestTrue(TEXT("The empty library is read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }));
	const FGuid SpotId = SpotTypeId();
	const FString SpotFile = MakeSource(Game.GetRoot(), TEXT("Test Spot.gdtf"));

	const FStageFixtureImportReview* Review = ReadOne(Game, *Library, SpotFile);
	if (!TestNotNull(TEXT("The file becomes a review"), Review))
	{
		return false;
	}
	TestTrue(TEXT("Review: type, manufacturer, can be added, not a replacement"), Review->FixtureTypeId == SpotId && Review->Manufacturer == TEXT("StageCraft")
		&& Review->bCanImport && !Review->bReplaces);
	TestEqual(TEXT("Three modes listed"), Review->Modes.Num(), 3);
	TestEqual(TEXT("Two of them usable"), Review->CountUsableModes(), 2);
	if (Review->Modes.Num() == 3)
	{
		const FStageFixtureModeSummary& Standard = Review->Modes[0];
		TestTrue(TEXT("Standard: 15 channels"), Standard.bUsable && Standard.Footprint == 15);
		TestTrue(TEXT("Focus and Control are at their default"), Standard.DefaultedNames.Contains(TEXT("Focus1")) && Standard.DefaultedNames.Contains(TEXT("Control1")));
		TestTrue(TEXT("The blade is approximated"), Standard.ApproximatedNames.Contains(TEXT("Blade1A")));
		TestTrue(TEXT("The broken mode says why"), !Review->Modes[2].bUsable && !Review->Modes[2].Reason.IsEmpty());
	}
	TestEqual(TEXT("Nothing in the library before ADD"), Library->GetTypes().Num(), 0);
	TestFalse(TEXT("Nothing in the catalog before ADD"), Items->GetCatalog().ContainsByPredicate([&SpotId](const UBaseItemData* Item)
		{ const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item); return Fixture && Fixture->FixtureTypeId == SpotId; }));

	TestEqual(TEXT("An unknown review is not confirmed"), Library->ConfirmImport(FGuid::NewGuid(), {}).Result, EStageFixtureImportResult::Unknown);
	TestEqual(TEXT("nor cancelled"), Library->CancelImport(FGuid::NewGuid()).Result, EStageFixtureImportResult::Unknown);
	TestTrue(TEXT("A file under review is not read twice"), Library->RequestImport({ SpotFile }).Num() == 1);

	const FStageFixtureImportOutcome Added = Library->ConfirmImport(Review->ReviewId, EStageFixtureKind::MovingHeadBeam);
	TestTrue(TEXT("ADD adds both modes"), Added.IsSuccess() && Added.Modes.Num() == 2 && Library->GetReviews().IsEmpty());
	const FStageFixtureLibraryType* Spot = Library->FindType(SpotId);
	if (!TestNotNull(TEXT("The type is in the library"), Spot))
	{
		return false;
	}
	TestTrue(TEXT("The kind correction is kept"), Spot->bKindCorrected && Spot->CorrectedKind == EStageFixtureKind::MovingHeadBeam
		&& Spot->Modes[0]->FixtureKind == EStageFixtureKind::MovingHeadBeam);
	TestTrue(TEXT("Every mode is in the catalog"), Items->GetCatalog().Contains(Spot->Modes[0].Get()) && Items->GetCatalog().Contains(Spot->Modes[1].Get()));
	TestTrue(TEXT("FindTypeOfItem finds the type of a mode"), Library->FindTypeOfItem(Spot->Modes[1]) == Spot);
	ULightingFixtureData* Standard = Spot->Modes[0];
	ULightingFixtureData* Basic = Spot->Modes[1];
	const FString StandardPath = Standard->GetPathName();

	TArray<FString> Many;
	for (int32 Index = 0; Index <= UStageFixtureLibrarySubsystem::MaxReviews; ++Index)
	{
		Many.Add(MakeSource(Game.GetRoot(), *FString::Printf(TEXT("Copy %d.gdtf"), Index)));
	}
	const TArray<FStageFixtureImportOutcome> TooMany = Library->RequestImport(Many);
	TestTrue(TEXT("At most MaxReviews files wait at once"), TooMany.Num() == 1 && TooMany[0].Result == EStageFixtureImportResult::Refused);
	TestTrue(TEXT("The others are read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }, 30.0));
	TestTrue(TEXT("and wait as replacements"), Library->GetReviews().Num() == UStageFixtureLibrarySubsystem::MaxReviews
		&& !Library->GetReviews().ContainsByPredicate([](const FStageFixtureImportReview& Waiting) { return !Waiting.bReplaces; }));
	while (!Library->GetReviews().IsEmpty())
	{
		Library->CancelImport(Library->GetReviews()[0].ReviewId);
	}
	TestEqual(TEXT("CANCEL changes nothing"), Library->GetTypes().Num(), 1);

	AddInfo(TEXT("Placed fixtures: modes swap, Replace moves them, Remove waits for them."));
	UWorld* World = Game.Get().GetWorld();
	if (!TestNotNull(TEXT("World"), World))
	{
		return false;
	}
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->NotifyBeginPlay();
	}
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
	ALightingFixtureActor* Fixture = Place(*World, Standard);
	if (!TestNotNull(TEXT("A spot is placed"), Fixture) || !TestNotNull(TEXT("The session tracks it"), Session) || !TestTrue(TEXT("registered"), Session->GetPlacedItems().Contains(Fixture)))
	{
		return false;
	}

	TArray<UBaseItemData*> Swappable;
	Fixture->GetSwappableItems(Swappable);
	TestTrue(TEXT("Type lists exactly the two modes"), Swappable.Num() == 2 && Swappable.Contains(Standard) && Swappable.Contains(Basic));
	const TArray<FText> Options = TypeOptions(*Fixture);
	TestTrue(TEXT("Options read mode and footprint"), Options.ContainsByPredicate([](const FText& Option) { return Option.ToString() == TEXT("Standard (15 ch)"); })
		&& Options.ContainsByPredicate([](const FText& Option) { return Option.ToString() == TEXT("Basic (3 ch)"); }));
	FString RowText;
	TestTrue(TEXT("Inspector shows the manufacturer"), HasRow(*Fixture, StageCraftTags::Param_Patch_Manufacturer, RowText) && RowText == TEXT("StageCraft"));
	TestTrue(TEXT("and the source"), HasRow(*Fixture, StageCraftTags::Param_Patch_Source, RowText) && RowText.EndsWith(TEXT("(GDTF)")));

	if (const ULightingFixtureData* BuiltIn = LoadObject<ULightingFixtureData>(nullptr, TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Spot470.DA_Fixture_Spot470")))
	{
		TestFalse(TEXT("A built-in spot never lists GDTF modes"), BuiltIn->CanSwapWith(*Standard));
		TestFalse(TEXT("and a GDTF mode never lists built-ins"), Standard->CanSwapWith(*BuiltIn));
		TestTrue(TEXT("A Type path outside the list selects nothing"), Fixture->ResolveSwapTarget(FStageParameterValue::MakeText(FText::FromString(BuiltIn->GetPathName()))) == nullptr);
	}

	const int32 BasicIndex = Swappable.IndexOfByKey(Basic);
	TestTrue(TEXT("Switching to Basic by index"), IStageParameterInterface::Execute_SetParameterValue(Fixture, StageCraftTags::Param_Info_Type, FStageParameterValue::MakeEnum(BasicIndex)));
	TestTrue(TEXT("swaps the mode in place and keeps the patch"), Fixture->GetItemData() == Basic && Fixture->GetDMXFootprint() == 3
		&& Fixture->GetPatch().Universe == 2 && Fixture->GetPatch().Address == 101);

	FStageParameterItemCommand Step(Fixture->GetInstanceId(), Fixture->GetInstanceLabel(), FText::FromString(TEXT("Type")), StageCraftTags::Param_Info_Type,
		FStageParameterValue::MakeText(FText::FromString(StandardPath)), FStageParameterValue::MakeText(FText::FromString(Basic->GetPathName())));
	FTestItemEditor Editor(*Fixture);
	TestEqual(TEXT("Undo by path"), Step.Undo(Editor), EStageCommandResult::Succeeded);
	TestTrue(TEXT("brings back Standard"), Fixture->GetItemData() == Standard && Fixture->GetDMXFootprint() == 15);
	TestEqual(TEXT("Redo by path"), Step.Redo(Editor), EStageCommandResult::Succeeded);
	TestTrue(TEXT("goes to Basic again"), Fixture->GetItemData() == Basic);

	const FStageFixtureImportOutcome InUse = Library->RemoveType(SpotId);
	TestEqual(TEXT("REMOVE is refused while a fixture of the type is placed"), InUse.Result, EStageFixtureImportResult::InUse);
	TestTrue(TEXT("and changes nothing"), Library->FindType(SpotId) && Items->GetCatalog().Contains(Basic));

	const FStageFixtureImportReview* Again = ReadOne(Game, *Library, SpotFile);
	if (!TestTrue(TEXT("The same file again is a replacement"), Again && Again->bReplaces))
	{
		return false;
	}
	const FStageFixtureImportOutcome Replaced = Library->ConfirmImport(Again->ReviewId, {});
	TestTrue(TEXT("REPLACE succeeds"), Replaced.IsSuccess() && Library->GetTypes().Num() == 1);
	const FStageFixtureLibraryType* NewSpot = Library->FindType(SpotId);
	if (!TestNotNull(TEXT("The type is still there"), NewSpot) || !TestEqual(TEXT("with two modes"), NewSpot->Modes.Num(), 2))
	{
		return false;
	}
	ULightingFixtureData* NewBasic = NewSpot->Modes[1];
	TestTrue(TEXT("New objects at the same paths"), NewBasic != Basic && NewSpot->Modes[0]->GetPathName() == StandardPath);
	TestTrue(TEXT("The placed fixture moved to the new Basic"), Fixture->GetItemData() == NewBasic && Fixture->GetDMXFootprint() == 3);
	TestTrue(TEXT("and kept its patch"), Fixture->GetPatch().Universe == 2 && Fixture->GetPatch().Address == 101);
	TestFalse(TEXT("The old modes left the catalog"), Items->GetCatalog().Contains(Basic) || Items->GetCatalog().Contains(Standard));
	TestTrue(TEXT("The new ones are in it"), Items->GetCatalog().Contains(NewBasic));

	Fixture->Destroy();
	TestFalse(TEXT("The fixture is gone from the stage"), Session->GetPlacedItems().Contains(Fixture));
	const FStageFixtureImportOutcome Removed = Library->RemoveType(SpotId);
	TestTrue(TEXT("REMOVE succeeds once nothing uses the type"), Removed.IsSuccess() && Library->GetTypes().IsEmpty());
	TestFalse(TEXT("Its modes left the catalog"), Items->GetCatalog().Contains(NewBasic));
	TestEqual(TEXT("A second REMOVE knows no such type"), Library->RemoveType(SpotId).Result, EStageFixtureImportResult::Unknown);

	Game.Shutdown();
	TestFalse(TEXT("Its library file is deleted"), IFileManager::Get().FileExists(*StageFixtureLibrary::GetLibraryFilePath(SpotId)));
	FString IndexJson;
	TArray<StageFixtureLibrary::FIndexEntry> Index;
	int32 Skipped = 0;
	TestTrue(TEXT("The index is empty"), FFileHelper::LoadFileToString(IndexJson, *StageFixtureLibrary::GetIndexPath())
		&& StageFixtureLibrary::ReadIndex(IndexJson, Index, Skipped) && Index.IsEmpty());
	return true;
}

#endif
