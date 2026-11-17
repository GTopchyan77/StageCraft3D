// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMenuModel.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Textures/SlateIcon.h"

FStageMenuSection& FStageMenu::AddSection(FName Name, const FText& Heading)
{
	FStageMenuSection& Section = Sections.AddDefaulted_GetRef();
	Section.Name = Name;
	Section.Heading = Heading;
	return Section;
}

bool FStageMenu::IsEmpty() const
{
	return !Sections.ContainsByPredicate([](const FStageMenuSection& Section) { return !Section.Entries.IsEmpty(); });
}

namespace StageMenu
{
	namespace
	{
		FStageMenuEntry MakeEntry(const FText& Label, const FText& Tip, EStageMenuEntryKind Kind, bool bChecked, bool bEnabled, TFunction<void()> Execute)
		{
			FStageMenuEntry Entry;
			Entry.Label = Label;
			Entry.Tip = Tip;
			Entry.Kind = Kind;
			Entry.bChecked = bChecked;
			Entry.bEnabled = bEnabled;
			Entry.Execute = MoveTemp(Execute);
			return Entry;
		}

		const FStageMenuEntry* FindIn(TConstArrayView<FStageMenuEntry> Entries, TConstArrayView<FString> Labels)
		{
			if (Labels.IsEmpty())
			{
				return nullptr;
			}
			for (const FStageMenuEntry& Entry : Entries)
			{
				if (Entry.Label.ToString().Equals(Labels[0], ESearchCase::IgnoreCase))
				{
					return Labels.Num() == 1 ? &Entry : FindIn(Entry.SubEntries, Labels.RightChop(1));
				}
			}
			return nullptr;
		}

		void DescribeEntries(TConstArrayView<FStageMenuEntry> Entries, int32 Depth, FString& Out)
		{
			for (const FStageMenuEntry& Entry : Entries)
			{
				const TCHAR* Mark = TEXT("");
				switch (Entry.Kind)
				{
				case EStageMenuEntryKind::Radio:	Mark = Entry.bChecked ? TEXT("(x) ") : TEXT("( ) "); break;
				case EStageMenuEntryKind::Toggle:	Mark = Entry.bChecked ? TEXT("[x] ") : TEXT("[ ] "); break;
				case EStageMenuEntryKind::SubMenu:	Mark = TEXT("> "); break;
				default: break;
				}
				Out += FString::Printf(TEXT("%s%s%s%s\n"), *FString::ChrN(2 + Depth * 2, TEXT(' ')), Mark, *Entry.Label.ToString(), Entry.bEnabled ? TEXT("") : TEXT(" -"));
				DescribeEntries(Entry.SubEntries, Depth + 1, Out);
			}
		}

		void AddEntries(FMenuBuilder& Builder, TConstArrayView<FStageMenuEntry> Entries)
		{
			for (const FStageMenuEntry& Entry : Entries)
			{
				const FCanExecuteAction CanExecute = FCanExecuteAction::CreateLambda([bEnabled = Entry.bEnabled]() { return bEnabled; });
				if (Entry.Kind == EStageMenuEntryKind::SubMenu)
				{
					// The submenu's entries are copied into the delegate: it is built when the submenu opens, after this description is gone.
					Builder.AddSubMenu(Entry.Label, Entry.Tip,
						FNewMenuDelegate::CreateLambda([SubEntries = Entry.SubEntries](FMenuBuilder& SubBuilder) { AddEntries(SubBuilder, SubEntries); }),
						FUIAction(FExecuteAction(), CanExecute), NAME_None, EUserInterfaceActionType::Button);
					continue;
				}
				const EUserInterfaceActionType Type = Entry.Kind == EStageMenuEntryKind::Radio ? EUserInterfaceActionType::RadioButton
					: Entry.Kind == EStageMenuEntryKind::Toggle ? EUserInterfaceActionType::ToggleButton
					: EUserInterfaceActionType::Button;
				Builder.AddMenuEntry(Entry.Label, Entry.Tip, FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([Execute = Entry.Execute, bEnabled = Entry.bEnabled]()
						{
							if (Execute && bEnabled)
							{
								Execute();
							}
						}),
						CanExecute,
						FIsActionChecked::CreateLambda([bChecked = Entry.bChecked]() { return bChecked; })),
					NAME_None, Type);
			}
		}
	}

	FStageMenuEntry MakeAction(const FText& Label, const FText& Tip, bool bEnabled, TFunction<void()> Execute)
	{
		return MakeEntry(Label, Tip, EStageMenuEntryKind::Action, false, bEnabled, MoveTemp(Execute));
	}

	FStageMenuEntry MakeRadio(const FText& Label, const FText& Tip, bool bChecked, bool bEnabled, TFunction<void()> Execute)
	{
		return MakeEntry(Label, Tip, EStageMenuEntryKind::Radio, bChecked, bEnabled, MoveTemp(Execute));
	}

	FStageMenuEntry MakeToggle(const FText& Label, const FText& Tip, bool bChecked, bool bEnabled, TFunction<void()> Execute)
	{
		return MakeEntry(Label, Tip, EStageMenuEntryKind::Toggle, bChecked, bEnabled, MoveTemp(Execute));
	}

	FStageMenuEntry MakeSubMenu(const FText& Label, const FText& Tip, TArray<FStageMenuEntry> Entries)
	{
		FStageMenuEntry Entry = MakeEntry(Label, Tip, EStageMenuEntryKind::SubMenu, false, !Entries.IsEmpty(), nullptr);
		Entry.SubEntries = MoveTemp(Entries);
		return Entry;
	}

	const FStageMenuEntry* Find(const FStageMenu& Menu, const FString& Path)
	{
		TArray<FString> Labels;
		Path.ParseIntoArray(Labels, TEXT("/"), /*bCullEmpty*/ true);
		for (FString& Label : Labels)
		{
			Label.TrimStartAndEndInline();
		}
		for (const FStageMenuSection& Section : Menu.Sections)
		{
			if (const FStageMenuEntry* Entry = FindIn(Section.Entries, Labels))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	bool Pick(const FStageMenu& Menu, const FString& Path)
	{
		const FStageMenuEntry* Entry = Find(Menu, Path);
		if (!Entry || !Entry->bEnabled || Entry->Kind == EStageMenuEntryKind::SubMenu || !Entry->Execute)
		{
			return false;
		}
		Entry->Execute();
		return true;
	}

	FString Describe(const FStageMenu& Menu)
	{
		FString Out;
		for (const FStageMenuSection& Section : Menu.Sections)
		{
			Out += FString::Printf(TEXT("[%s]\n"), *Section.Heading.ToString());
			DescribeEntries(Section.Entries, 0, Out);
		}
		return Out;
	}

	TSharedRef<SWidget> MakeWidget(const FStageMenu& Menu)
	{
		FMenuBuilder Builder(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);
		for (const FStageMenuSection& Section : Menu.Sections)
		{
			if (Section.Entries.IsEmpty())
			{
				continue;
			}
			Builder.BeginSection(Section.Name, Section.Heading);
			AddEntries(Builder, Section.Entries);
			Builder.EndSection();
		}
		return Builder.MakeWidget();
	}
}
