// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the toolbar dropdowns (STATE.md #100): the menu description (StageMenu), the Sketch window's Edit, Snap, Items and
// Photo menus (StageSketchMenus) and the Audio panel's Source menu (StageAudioMenus). No world and no widgets: what each menu
// offers, what is checked or greyed out, and what a pick asks for. The live panels are checked by Scripts/Tests/RunToolbarMenuCheck.ps1.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UI/StageAudioMenus.h"
#include "UI/StageAudioPanel.h"
#include "UI/StageMenuModel.h"
#include "UI/StageSketchMenus.h"
#include "UI/StageUIAnchors.h"

namespace StageMenuTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FText Text(const TCHAR* Value)
	{
		return FText::FromString(Value);
	}

	bool IsChecked(const FStageMenu& Menu, const TCHAR* Path)
	{
		const FStageMenuEntry* Entry = StageMenu::Find(Menu, Path);
		return Entry && Entry->bChecked;
	}

	bool IsEnabled(const FStageMenu& Menu, const TCHAR* Path)
	{
		const FStageMenuEntry* Entry = StageMenu::Find(Menu, Path);
		return Entry && Entry->bEnabled;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMenuModelTest, "StageCraft.UI.MenuModel", StageMenuTests::Flags)

bool FStageMenuModelTest::RunTest(const FString& Parameters)
{
	using namespace StageMenuTests;
	int32 Runs = 0;
	const auto Count = [&Runs]() { ++Runs; };

	FStageMenu Menu;
	TestTrue(TEXT("A new menu is empty"), Menu.IsEmpty());
	Menu.AddSection(TEXT("Empty"), Text(TEXT("Nothing")));
	TestTrue(TEXT("A menu of empty sections is empty"), Menu.IsEmpty());

	FStageMenuSection& First = Menu.AddSection(TEXT("First"), Text(TEXT("First")));
	First.Entries.Add(StageMenu::MakeAction(Text(TEXT("Run")), FText::GetEmpty(), true, Count));
	First.Entries.Add(StageMenu::MakeAction(Text(TEXT("Greyed")), FText::GetEmpty(), false, Count));
	First.Entries.Add(StageMenu::MakeRadio(Text(TEXT("Chosen")), FText::GetEmpty(), true, true, Count));
	First.Entries.Add(StageMenu::MakeToggle(Text(TEXT("Option")), FText::GetEmpty(), false, true, Count));
	Menu.AddSection(TEXT("Second"), Text(TEXT("Second"))).Entries.Add(StageMenu::MakeSubMenu(Text(TEXT("More Things")), FText::GetEmpty(),
		{ StageMenu::MakeRadio(Text(TEXT("Inner One")), FText::GetEmpty(), false, true, Count) }));
	Menu.Sections.Last().Entries.Add(StageMenu::MakeSubMenu(Text(TEXT("Nothing Inside")), FText::GetEmpty(), {}));
	TestFalse(TEXT("A menu with entries is not empty"), Menu.IsEmpty());

	TestTrue(TEXT("Picks an enabled entry"), StageMenu::Pick(Menu, TEXT("Run")));
	TestEqual(TEXT("Its action ran once"), Runs, 1);
	TestFalse(TEXT("A greyed-out entry refuses"), StageMenu::Pick(Menu, TEXT("Greyed")));
	TestFalse(TEXT("A missing entry refuses"), StageMenu::Pick(Menu, TEXT("Absent")));
	TestFalse(TEXT("A submenu itself is not picked"), StageMenu::Pick(Menu, TEXT("More Things")));
	TestEqual(TEXT("Refused picks ran nothing"), Runs, 1);
	TestTrue(TEXT("A path reaches into a submenu, ignoring case and spaces around /"), StageMenu::Pick(Menu, TEXT("more things / inner one")));
	TestEqual(TEXT("The submenu entry ran"), Runs, 2);
	TestFalse(TEXT("A submenu without entries is greyed out"), IsEnabled(Menu, TEXT("Nothing Inside")));
	TestNull(TEXT("A section heading is not part of a path"), StageMenu::Find(Menu, TEXT("First/Run")));

	const FString Described = StageMenu::Describe(Menu);
	TestTrue(TEXT("Describe lists the heading"), Described.Contains(TEXT("[First]")));
	TestTrue(TEXT("Describe marks a checked radio"), Described.Contains(TEXT("(x) Chosen")));
	TestTrue(TEXT("Describe marks an unchecked toggle"), Described.Contains(TEXT("[ ] Option")));
	TestTrue(TEXT("Describe marks a greyed-out entry"), Described.Contains(TEXT("Greyed -")));
	TestTrue(TEXT("Describe nests a submenu"), Described.Contains(TEXT("> More Things")) && Described.Contains(TEXT("    ( ) Inner One")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchMenusTest, "StageCraft.UI.SketchMenus", StageMenuTests::Flags)

bool FStageSketchMenusTest::RunTest(const FString& Parameters)
{
	using namespace StageMenuTests;
	using namespace StageSketchMenus;

	// Snap: the label, the checked presets, and a pick changing exactly one field.
	FStageSketchSettings Settings;
	Settings.GridStep = 50.f;
	Settings.bSnapToGrid = true;
	Settings.Smoothing = 0.5f;
	Settings.DeckHeight = 120.f;
	TestEqual(TEXT("Snap label"), DescribeSnap(Settings).ToString(), FString(TEXT("Snap 50 cm · Smooth Low")));
	FStageSketchSettings Unsnapped = Settings;
	Unsnapped.bSnapToGrid = false;
	Unsnapped.Smoothing = 0.f;
	TestEqual(TEXT("Snap label when off"), DescribeSnap(Unsnapped).ToString(), FString(TEXT("Snap Off · Smooth Off")));
	FStageSketchSettings Metre = Settings;
	Metre.GridStep = 100.f;
	Metre.Smoothing = 0.75f;
	TestEqual(TEXT("A whole metre reads in metres; smoothing reads as the nearest preset"), DescribeSnap(Metre).ToString(), FString(TEXT("Snap 1 m · Smooth High")));

	// The window applies a pick's edit to the settings as they are at pick time; Current stands in for the sketch.
	FStageSketchSettings Current = Settings;
	TOptional<FStageSketchSettings> Applied;
	const auto Apply = [&Applied, &Current](const FSettingsEdit& Edit)
	{
		FStageSketchSettings Next = Current;
		Edit(Next);
		Applied = Next;
	};
	const FStageMenu Snap = MakeSnapMenu(Settings, true, Apply);
	TestTrue(TEXT("The grid step in use is checked"), IsChecked(Snap, TEXT("50 cm")));
	TestFalse(TEXT("Another grid step is not"), IsChecked(Snap, TEXT("25 cm")));
	TestTrue(TEXT("Snap to Grid shows on"), IsChecked(Snap, TEXT("Snap to Grid")));
	TestTrue(TEXT("The smoothing in use is checked"), IsChecked(Snap, TEXT("Low")));
	TestTrue(TEXT("Pick 1 m"), StageMenu::Pick(Snap, TEXT("1 m")));
	TestTrue(TEXT("1 m applies a 100 cm grid"), Applied.IsSet() && Applied->GridStep == 100.f);
	TestTrue(TEXT("and changes nothing else"), Applied.IsSet() && Applied->DeckHeight == 120.f && Applied->bSnapToGrid && Applied->Smoothing == 0.5f);
	Applied.Reset();
	StageMenu::Pick(Snap, TEXT("Snap to Grid"));
	TestTrue(TEXT("Snap to Grid turns snapping off"), Applied.IsSet() && !Applied->bSnapToGrid && Applied->GridStep == 50.f);
	Applied.Reset();
	StageMenu::Pick(Snap, TEXT("High"));
	TestTrue(TEXT("High smoothing"), Applied.IsSet() && FMath::IsNearlyEqual(Applied->Smoothing, 0.8f));
	Applied.Reset();
	// A setting changed elsewhere while the menu was open (a console or assistant edit) survives the pick.
	Current.DeckHeight = 200.f;
	StageMenu::Pick(Snap, TEXT("25 cm"));
	TestTrue(TEXT("A pick changes only its own field of the current settings"), Applied.IsSet() && Applied->GridStep == 25.f && Applied->DeckHeight == 200.f);
	Applied.Reset();
	const FStageMenu NoSketch = MakeSnapMenu(Settings, false, Apply);
	TestFalse(TEXT("Without a sketch nothing can be picked"), StageMenu::Pick(NoSketch, TEXT("25 cm")) || StageMenu::Pick(NoSketch, TEXT("Snap to Grid")));
	TestFalse(TEXT("and nothing was applied"), Applied.IsSet());

	// Edit: undo and clear only with strokes; each pick names its command.
	TArray<EEditCommand> Commands;
	const auto Record = [&Commands](EEditCommand Command) { Commands.Add(Command); };
	const FStageMenu EmptyEdit = MakeEditMenu(true, false, Record);
	TestFalse(TEXT("Undo Stroke needs a stroke"), IsEnabled(EmptyEdit, TEXT("Undo Stroke")));
	TestFalse(TEXT("Clear Sketch needs a stroke"), IsEnabled(EmptyEdit, TEXT("Clear Sketch")));
	TestTrue(TEXT("Frame Sketch works on an empty sketch"), StageMenu::Pick(EmptyEdit, TEXT("Frame Sketch")));
	const FStageMenu Edit = MakeEditMenu(true, true, Record);
	StageMenu::Pick(Edit, TEXT("Undo Stroke"));
	StageMenu::Pick(Edit, TEXT("Clear Sketch"));
	TestTrue(TEXT("Edit commands in pick order"), Commands == TArray<EEditCommand>({ EEditCommand::Frame, EEditCommand::UndoStroke, EEditCommand::Clear }));

	// Items: one submenu per element, labelled with the item in use; a pick names the element and the candidate.
	TArray<FItemChoice> Choices;
	Choices.Add({ EStageSketchElement::Deck, { Text(TEXT("Deck 2x1")), Text(TEXT("Deck 2x2")) }, 1 });
	Choices.Add({ EStageSketchElement::Truss, { Text(TEXT("Box Truss")) }, 0 });
	Choices.Add({ EStageSketchElement::Fixture, {}, INDEX_NONE });
	TOptional<TPair<EStageSketchElement, int32>> Picked;
	const FStageMenu Items = MakeItemsMenu(Choices, [&Picked](EStageSketchElement Element, int32 Index) { Picked.Emplace(Element, Index); });
	TestTrue(TEXT("The deck submenu names the deck in use"), StageMenu::Find(Items, TEXT("Deck: Deck 2x2")) != nullptr);
	TestTrue(TEXT("The deck in use is checked"), IsChecked(Items, TEXT("Deck: Deck 2x2/Deck 2x2")));
	TestFalse(TEXT("The other deck is not"), IsChecked(Items, TEXT("Deck: Deck 2x2/Deck 2x1")));
	TestFalse(TEXT("An element no catalog item builds is greyed out"), IsEnabled(Items, TEXT("Fixture: none in catalog")));
	TestTrue(TEXT("Pick the other deck"), StageMenu::Pick(Items, TEXT("Deck: Deck 2x2/Deck 2x1")));
	TestTrue(TEXT("The pick names the deck element and candidate 0"), Picked.IsSet() && Picked->Key == EStageSketchElement::Deck && Picked->Value == 0);

	// A large library (STATE.md #102): past MaxUngroupedItems the candidates are split by kind, kinds in name order, each candidate
	// once, and a pick still names the candidate's index in the full list.
	const auto MakeLibrary = [](int32 Count, bool bOneKind)
	{
		const TCHAR* Kinds[] = { TEXT("Wash"), TEXT("Spot"), TEXT("LED Par"), TEXT("") };
		FItemChoice Library{ EStageSketchElement::Fixture, {}, 7, {} };
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Library.Names.Add(FText::FromString(FString::Printf(TEXT("Fixture %02d"), Index)));
			Library.Groups.Add(Text(bOneKind ? TEXT("Wash") : Kinds[Index % UE_ARRAY_COUNT(Kinds)]));
		}
		return Library;
	};
	const auto CountRadios = [](TConstArrayView<FStageMenuEntry> Entries)
	{
		int32 Count = 0;
		for (const FStageMenuEntry& Entry : Entries)
		{
			Count += Entry.Kind == EStageMenuEntryKind::Radio;
			for (const FStageMenuEntry& Sub : Entry.SubEntries)
			{
				Count += Sub.Kind == EStageMenuEntryKind::Radio;
			}
		}
		return Count;
	};
	const int32 Large = MaxUngroupedItems + 5;
	Picked.Reset();
	const FStageMenu Grouped = MakeItemsMenu({ MakeLibrary(Large, false) }, [&Picked](EStageSketchElement Element, int32 Index) { Picked.Emplace(Element, Index); });
	const FStageMenuEntry* Fixtures = StageMenu::Find(Grouped, TEXT("Fixture: Fixture 07"));
	TArray<FString> KindLabels;
	for (const FStageMenuEntry& Kind : Fixtures ? TConstArrayView<FStageMenuEntry>(Fixtures->SubEntries) : TConstArrayView<FStageMenuEntry>())
	{
		KindLabels.Add(Kind.Kind == EStageMenuEntryKind::SubMenu ? Kind.Label.ToString() : TEXT("(not a submenu)"));
	}
	TestEqual(TEXT("A long list is split by kind, in name order, no kind under Other"), FString::Join(KindLabels, TEXT(", ")), FString(TEXT("LED Par, Other, Spot, Wash")));
	TestEqual(TEXT("Every candidate is listed once"), Fixtures ? CountRadios(Fixtures->SubEntries) : 0, Large);
	TestTrue(TEXT("The fixture in use is checked in its kind"), IsChecked(Grouped, TEXT("Fixture: Fixture 07/Other/Fixture 07")));
	TestTrue(TEXT("Pick a fixture inside a kind"), StageMenu::Pick(Grouped, TEXT("Fixture: Fixture 07/Spot/Fixture 21")));
	TestTrue(TEXT("The pick names its index in the whole list"), Picked.IsSet() && Picked->Key == EStageSketchElement::Fixture && Picked->Value == 21);

	const auto IsFlat = [&CountRadios](const FStageMenu& Menu, int32 Count)
	{
		const FStageMenuEntry* Entry = Menu.Sections.IsEmpty() || Menu.Sections[0].Entries.IsEmpty() ? nullptr : &Menu.Sections[0].Entries[0];
		return Entry && Entry->SubEntries.Num() == Count && CountRadios(Entry->SubEntries) == Count;
	};
	TestTrue(TEXT("Up to MaxUngroupedItems stays one list"), IsFlat(MakeItemsMenu({ MakeLibrary(MaxUngroupedItems, false) }, nullptr), MaxUngroupedItems));
	TestTrue(TEXT("A long list of one kind stays one list"), IsFlat(MakeItemsMenu({ MakeLibrary(Large, true) }, nullptr), Large));
	FItemChoice Unknown = MakeLibrary(Large, false);
	Unknown.Groups.Pop();
	TestTrue(TEXT("Kinds that do not match the names are ignored"), IsFlat(MakeItemsMenu({ Unknown }, nullptr), Large));

	// Photo: the label, what an import in progress greys out, and Claude only when it can be reached.
	FPhotoState Photo;
	Photo.bAvailable = true;
	Photo.bCanBrowse = true;
	Photo.bOnline = false;
	Photo.OnlineUnavailable = Text(TEXT("No connection."));
	TArray<EPhotoCommand> PhotoCommands;
	const auto RecordPhoto = [&PhotoCommands](EPhotoCommand Command) { PhotoCommands.Add(Command); };
	const FStageMenu NoPhoto = MakePhotoMenu(Photo, RecordPhoto);
	TestEqual(TEXT("Photo label"), DescribePhoto(Photo).ToString(), FString(TEXT("Photo")));
	TestTrue(TEXT("Import is offered"), IsEnabled(NoPhoto, TEXT("Import Photo...")));
	TestFalse(TEXT("Remove needs a photo"), IsEnabled(NoPhoto, TEXT("Remove Photo")));
	TestFalse(TEXT("Show needs a photo"), IsEnabled(NoPhoto, TEXT("Show Under the Sketch")));
	TestTrue(TEXT("This PC reads by default"), IsChecked(NoPhoto, TEXT("This PC")));
	const FStageMenuEntry* Claude = StageMenu::Find(NoPhoto, TEXT("Claude"));
	TestTrue(TEXT("Claude is greyed out offline and says why"), Claude && !Claude->bEnabled && Claude->Tip.ToString() == TEXT("No connection."));
	StageMenu::Pick(NoPhoto, TEXT("Import Photo..."));
	StageMenu::Pick(NoPhoto, TEXT("Claude"));
	TestTrue(TEXT("Only the import ran"), PhotoCommands == TArray<EPhotoCommand>({ EPhotoCommand::Import }));

	Photo.bOnline = true;
	Photo.bHasPhoto = true;
	Photo.bShown = true;
	Photo.Reader = EStageSketchReader::Claude;
	const FStageMenu WithPhoto = MakePhotoMenu(Photo, RecordPhoto);
	TestTrue(TEXT("Claude is checked when chosen and reachable"), IsChecked(WithPhoto, TEXT("Claude")));
	TestTrue(TEXT("The shown photo is checked"), IsChecked(WithPhoto, TEXT("Show Under the Sketch")));
	StageMenu::Pick(WithPhoto, TEXT("Show Under the Sketch"));
	StageMenu::Pick(WithPhoto, TEXT("Remove Photo"));
	StageMenu::Pick(WithPhoto, TEXT("This PC"));
	TestTrue(TEXT("Photo commands in pick order"), PhotoCommands ==
		TArray<EPhotoCommand>({ EPhotoCommand::Import, EPhotoCommand::ToggleShown, EPhotoCommand::Remove, EPhotoCommand::ReadHere }));

	Photo.bImporting = true;
	const FStageMenu Importing = MakePhotoMenu(Photo, RecordPhoto);
	TestEqual(TEXT("Label while reading"), DescribePhoto(Photo).ToString(), FString(TEXT("Reading...")));
	TestFalse(TEXT("No second import while one runs"), IsEnabled(Importing, TEXT("Import Photo...")));
	TestFalse(TEXT("The reader is not changed mid-import"), IsEnabled(Importing, TEXT("This PC")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioMenuTest, "StageCraft.UI.AudioMenu", StageMenuTests::Flags)

bool FStageAudioMenuTest::RunTest(const FString& Parameters)
{
	using namespace StageMenuTests;
	TestEqual(TEXT("Off label"), StageAudioMenus::DescribeSource(EStageAudioSource::Off).ToString(), FString(TEXT("Off")));
	TestEqual(TEXT("System label"), StageAudioMenus::DescribeSource(EStageAudioSource::SystemAudio).ToString(), FString(TEXT("System Audio")));

	TOptional<EStageAudioSource> Source;
	TOptional<bool> Background;
	const auto PickSource = [&Source](EStageAudioSource Picked) { Source = Picked; };
	const auto SetBackground = [&Background](bool bOn) { Background = bOn; };

	const FStageMenu Menu = StageAudioMenus::MakeSourceMenu(EStageAudioSource::SystemAudio, true, true, PickSource, SetBackground);
	for (const TCHAR* Label : { TEXT("Off"), TEXT("File..."), TEXT("Input"), TEXT("System Audio"), TEXT("Test Beat"), TEXT("Keep Playing in the Background") })
	{
		TestTrue(FString::Printf(TEXT("%s is offered"), Label), IsEnabled(Menu, Label));
	}
	TestTrue(TEXT("The source playing is checked"), IsChecked(Menu, TEXT("System Audio")));
	TestFalse(TEXT("Only that one"), IsChecked(Menu, TEXT("Off")));
	TestTrue(TEXT("Background playback shows on"), IsChecked(Menu, TEXT("Keep Playing in the Background")));
	TestTrue(TEXT("Pick the test beat"), StageMenu::Pick(Menu, TEXT("Test Beat")));
	TestTrue(TEXT("It asks for the test signal"), Source.IsSet() && *Source == EStageAudioSource::TestSignal);
	StageMenu::Pick(Menu, TEXT("Keep Playing in the Background"));
	TestTrue(TEXT("The toggle asks to turn background playback off"), Background.IsSet() && !*Background);
	Source.Reset();
	TestTrue(TEXT("Picking the checked source still runs (File... picks another song)"), StageMenu::Pick(Menu, TEXT("System Audio")) && Source.IsSet());

	Source.Reset();
	Background.Reset();
	const FStageMenu NoController = StageAudioMenus::MakeSourceMenu(EStageAudioSource::Off, false, false, PickSource, SetBackground);
	TestFalse(TEXT("Without a controller nothing is sent"), StageMenu::Pick(NoController, TEXT("Input")) || StageMenu::Pick(NoController, TEXT("Keep Playing in the Background")));
	TestFalse(TEXT("No request was made"), Source.IsSet() || Background.IsSet());

	// The assistant names an anchor inside the dropdown by its path (STATE.md #101): every anchor that says it is a SOURCE entry is
	// one, every entry is such an anchor, and the path reads as the panel shows it.
	TSet<FString> Entries;
	for (const FStageMenuSection& Section : Menu.Sections)
	{
		for (const FStageMenuEntry& Entry : Section.Entries)
		{
			Entries.Add(Entry.Label.ToString());
		}
	}
	int32 InMenu = 0;
	for (const FStageUIAnchor& Anchor : UStageAudioPanel::GetAnchors())
	{
		const FString Label = Anchor.Label.ToString();
		if (Anchor.Menu.IsEmpty())
		{
			TestFalse(FString::Printf(TEXT("%s is on the panel, not in the dropdown"), *Label), Entries.Contains(Label));
			continue;
		}
		++InMenu;
		TestEqual(FString::Printf(TEXT("%s is under SOURCE"), *Label), Anchor.Menu.ToString(), FString(TEXT("SOURCE")));
		TestTrue(FString::Printf(TEXT("%s is an entry of the dropdown"), *Label), StageMenu::Find(Menu, Label) != nullptr);
	}
	TestEqual(TEXT("Every dropdown entry is an anchor under SOURCE"), InMenu, Entries.Num());
	TestEqual(TEXT("Path of a dropdown entry"), StageUIAnchors::DescribePath(StageUIAnchors::Get(UStageAudioPanel::GetAnchors(), TEXT("SourceOff"))).ToString(),
		FString(TEXT("SOURCE › Off")));
	TestEqual(TEXT("Path of a control on the panel"), StageUIAnchors::DescribePath(StageUIAnchors::Get(UStageAudioPanel::GetAnchors(), TEXT("Play"))).ToString(),
		FString(TEXT("PLAY")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
