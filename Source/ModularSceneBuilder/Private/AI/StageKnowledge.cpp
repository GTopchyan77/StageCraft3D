// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageKnowledge.h"

#include "AI/StageKnowledgeSearch.h"
#include "Algo/StableSort.h"

#define LOCTEXT_NAMESPACE "StageKnowledge"

const FStageKnowledgeEntry* FStageKnowledgeIndex::Find(FName Id, EStageAILanguage Language) const
{
	const FStageKnowledgeEntry* Fallback = nullptr;
	for (const FStageKnowledgeEntry& Entry : Entries)
	{
		if (Entry.Id != Id)
		{
			continue;
		}
		if (Entry.Language == Language)
		{
			return &Entry;
		}
		if (!Fallback || Entry.Language == EStageAILanguage::English)
		{
			Fallback = &Entry;
		}
	}
	return Fallback;
}

namespace StageKnowledge
{
	namespace
	{
		/** The tag's last segment ("StageCraft.Panel.Playback" -> "Playback"), for ids. */
		FString LastSegment(const FGameplayTag& Tag)
		{
			FString Name = Tag.ToString();
			int32 Dot = INDEX_NONE;
			if (Name.FindLastChar(TEXT('.'), Dot))
			{
				Name.RightChopInline(Dot + 1);
			}
			return Name;
		}

		void AddTerms(const FString& Text, float Weight, FStageKnowledgeIndex::FDocument& Document)
		{
			TArray<FString> Terms;
			StageKnowledgeSearch::Tokenize(Text, Terms);
			for (const FString& Term : Terms)
			{
				Document.Terms.FindOrAdd(Term) += Weight;
				Document.Length += Weight;
			}
		}

		/** Title and keywords count three times the body: a hit on a name beats a passing mention. */
		constexpr float NameWeight = 3.f;

		FText Frame(EStageAILanguage Language, const FText& English, const TCHAR* Armenian)
		{
			return Language == EStageAILanguage::Armenian ? FText::FromString(Armenian) : English;
		}

		FText DescribeKeys(const FStageKnowledgeEntry& Entry, const FStageKeyBindings& Bindings, EStageAILanguage Language)
		{
			const FText Keys = Entry.Command.IsSet() ? StageKeyBindings::DescribeCommand(Bindings, *Entry.Command) : FText::GetEmpty();
			return Keys.IsEmpty() ? Frame(Language, LOCTEXT("NotBound", "not bound (Preferences › Keys)"), TEXT("կապված չէ (Preferences › Keys)")) : Keys;
		}

		FText ComposeEntry(const FStageKnowledgeEntry& Entry, const FStageKeyBindings& Bindings, EStageAILanguage Language)
		{
			switch (Entry.Kind)
			{
			case EStageKnowledgeKind::Command:
				return FText::FromString(Entry.Body.Replace(TEXT("{keys}"), *DescribeKeys(Entry, Bindings, Language).ToString()));
			case EStageKnowledgeKind::Control:
			case EStageKnowledgeKind::Panel:
				// The body already says where it is; the title is cited under the reply.
				return FText::FromString(Entry.Body);
			default:
				return FText::Format(INVTEXT("{0}\n{1}"), FText::FromString(Entry.Title), FText::FromString(Entry.Body));
			}
		}
	}

	bool ParseArticle(const FString& Text, FName Id, EStageAILanguage Language, FStageHelpArticle& OutArticle, FString& OutError)
	{
		OutArticle = FStageHelpArticle();
		OutArticle.Id = Id;
		OutArticle.Language = Language;
		if (Text.Len() > MaxArticleLength)
		{
			OutError = FString::Printf(TEXT("%s: longer than %d characters"), *Id.ToString(), MaxArticleLength);
			return false;
		}

		TArray<FString> Lines;
		Text.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		int32 Line = 0;
		while (Line < Lines.Num() && Lines[Line].TrimStartAndEnd().IsEmpty())
		{
			++Line;
		}
		// A byte-order mark survives FFileHelper on some paths; it is not part of the title.
		const FString TitleLine = Line < Lines.Num() ? Lines[Line].TrimStartAndEnd().Replace(TEXT("﻿"), TEXT("")) : FString();
		if (!TitleLine.StartsWith(TEXT("# ")))
		{
			OutError = FString::Printf(TEXT("%s: the first line must be \"# Title\""), *Id.ToString());
			return false;
		}
		OutArticle.Title = TitleLine.Mid(2).TrimStartAndEnd();
		++Line;

		for (; Line < Lines.Num(); ++Line)
		{
			const FString Header = Lines[Line].TrimStartAndEnd();
			if (Header.IsEmpty())
			{
				break;
			}
			FString Key;
			FString Value;
			if (!Header.Split(TEXT(":"), &Key, &Value))
			{
				break;
			}
			Key = Key.TrimStartAndEnd().ToLower();
			Value = Value.TrimStartAndEnd();
			if (Key == TEXT("keywords"))
			{
				OutArticle.Keywords = Value;
			}
			else if (Key == TEXT("panel"))
			{
				OutArticle.Panel = FGameplayTag::RequestGameplayTag(FName(*Value), /*ErrorIfNotFound*/ false);
			}
			else if (Key == TEXT("control"))
			{
				OutArticle.Control = FName(*Value);
			}
		}

		TArray<FString> Body;
		for (; Line < Lines.Num(); ++Line)
		{
			Body.Add(Lines[Line].TrimEnd());
		}
		OutArticle.Body = FString::Join(Body, TEXT("\n")).TrimStartAndEnd();
		if (OutArticle.Title.IsEmpty() || OutArticle.Body.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s: no title or no body"), *Id.ToString());
			return false;
		}
		return true;
	}

	FStageKnowledgeIndex Build(const FStageKnowledgeSources& Sources)
	{
		FStageKnowledgeIndex Index;
		TArray<FStageKnowledgeEntry>& Entries = Index.Entries;

		TArray<const FStageHelpArticle*> Articles;
		for (const FStageHelpArticle& Article : Sources.Articles)
		{
			Articles.Add(&Article);
		}
		Algo::StableSort(Articles, [](const FStageHelpArticle* A, const FStageHelpArticle* B)
		{
			const int32 Order = A->Id.Compare(B->Id);
			return Order != 0 ? Order < 0 : A->Language < B->Language;
		});
		for (const FStageHelpArticle* Article : Articles)
		{
			FStageKnowledgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Id = FName(*(TEXT("article.") + Article->Id.ToString()));
			Entry.Kind = EStageKnowledgeKind::Article;
			Entry.Language = Article->Language;
			Entry.Title = Article->Title;
			Entry.Body = Article->Body;
			Entry.Keywords = Article->Keywords;
			Entry.Panel = Article->Panel;
			Entry.Control = Article->Control;
		}

		for (const FStageKnowledgeSources::FCommand& Command : Sources.Commands)
		{
			FStageKnowledgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Id = FName(*(TEXT("command.") + StageKeyBindings::GetCommandName(Command.Command).ToString()));
			Entry.Kind = EStageKnowledgeKind::Command;
			Entry.Title = Command.Label;
			Entry.Body = FString::Printf(TEXT("%s: {keys}. %s"), *Command.Label, *Command.Description);
			Entry.Keywords = FString::Printf(TEXT("%s shortcut key keyboard"), *Command.DefaultKeys);
			Entry.Command = Command.Command;
		}

		for (const FStageKnowledgeSources::FPanel& Panel : Sources.Panels)
		{
			TArray<FString> Controls;
			for (const FStageKnowledgeSources::FControl& Control : Sources.Controls)
			{
				if (Control.Panel == Panel.Panel)
				{
					Controls.Add(Control.Label);
				}
			}
			FStageKnowledgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Id = FName(*(TEXT("panel.") + LastSegment(Panel.Panel)));
			Entry.Kind = EStageKnowledgeKind::Panel;
			Entry.Title = FString::Printf(TEXT("%s panel"), *Panel.Name);
			Entry.Body = Controls.IsEmpty()
				? FString::Printf(TEXT("The %s panel opens from Window › %s."), *Panel.Name, *Panel.Name)
				: FString::Printf(TEXT("The %s panel opens from Window › %s. It has: %s."), *Panel.Name, *Panel.Name, *FString::Join(Controls, TEXT(", ")));
			Entry.Keywords = FString::Printf(TEXT("%s panel window"), *Panel.Name);
			Entry.Panel = Panel.Panel;
		}

		for (const FStageKnowledgeSources::FControl& Control : Sources.Controls)
		{
			FStageKnowledgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Id = FName(*FString::Printf(TEXT("control.%s.%s"), *LastSegment(Control.Panel), *Control.Control.ToString()));
			Entry.Kind = EStageKnowledgeKind::Control;
			Entry.Title = FString::Printf(TEXT("%s › %s"), *Control.PanelName, *Control.Label);
			Entry.Body = FString::Printf(TEXT("%s is in the %s panel. %s"), *Control.Label, *Control.PanelName, *Control.Tooltip);
			Entry.Keywords = FString::Printf(TEXT("%s button control"), *Control.Label);
			Entry.Panel = Control.Panel;
			Entry.Control = Control.Control;
		}

		for (const FStageKnowledgeSources::FParameter& Parameter : Sources.Parameters)
		{
			FStageKnowledgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Id = FName(*(TEXT("parameter.") + LastSegment(Parameter.Attribute)));
			Entry.Kind = EStageKnowledgeKind::Parameter;
			Entry.Title = Parameter.Name;
			Entry.Body = Parameter.Models.IsEmpty()
				? Parameter.Meaning
				: FString::Printf(TEXT("%s\nIn the catalog: %s."), *Parameter.Meaning, *FString::Join(Parameter.Models, TEXT("; ")));
			Entry.Keywords = FString::Printf(TEXT("%s parameter attribute channel"), *LastSegment(Parameter.Attribute));
		}

		// The statistics BM25 needs: weighted term counts per document, document lengths, and document frequency.
		Index.Documents.SetNum(Entries.Num());
		float TotalLength = 0.f;
		for (int32 Position = 0; Position < Entries.Num(); ++Position)
		{
			const FStageKnowledgeEntry& Entry = Entries[Position];
			FStageKnowledgeIndex::FDocument& Document = Index.Documents[Position];
			AddTerms(Entry.Title, NameWeight, Document);
			AddTerms(Entry.Keywords, NameWeight, Document);
			AddTerms(Entry.Body, 1.f, Document);
			for (const TPair<FString, float>& Term : Document.Terms)
			{
				++Index.DocumentFrequency.FindOrAdd(Term.Key);
			}
			TotalLength += Document.Length;
		}
		Index.AverageLength = Entries.IsEmpty() ? 0.f : TotalLength / Entries.Num();
		return Index;
	}

	FString BuildDigest(const FStageKnowledgeIndex& Index)
	{
		TArray<FString> Panels;
		TArray<FString> Commands;
		TArray<FString> Controls;
		TArray<FString> Articles;
		for (const FStageKnowledgeEntry& Entry : Index.Entries)
		{
			if (Entry.Language != EStageAILanguage::English)
			{
				continue;
			}
			switch (Entry.Kind)
			{
			case EStageKnowledgeKind::Panel:	Panels.Add(Entry.Title); break;
			case EStageKnowledgeKind::Command:	Commands.Add(Entry.Title); break;
			case EStageKnowledgeKind::Control:	Controls.Add(Entry.Title); break;
			case EStageKnowledgeKind::Article:	Articles.Add(Entry.Title); break;
			default: break;
			}
		}
		return FString::Printf(TEXT("Panels: %s.\nCommands: %s.\nControls: %s.\nHelp articles: %s."), *FString::Join(Panels, TEXT(", ")),
			*FString::Join(Commands, TEXT(", ")), *FString::Join(Controls, TEXT("; ")), *FString::Join(Articles, TEXT("; ")));
	}

	FStageKnowledgeAnswer Answer(const FStageKnowledgeIndex& Index, const FString& Prompt, EStageAILanguage Language, const FStageKeyBindings& Bindings)
	{
		FStageKnowledgeAnswer Answer;
		const TArray<StageKnowledgeSearch::FHit> Hits = StageKnowledgeSearch::Search(Index, Prompt, Language, 3);
		if (Hits.IsEmpty() || Hits[0].Score < StageKnowledgeSearch::MinAnswerScore)
		{
			return Answer;
		}
		const FStageKnowledgeEntry& Best = Index.Entries[Hits[0].Entry];
		Answer.bFound = true;
		Answer.SourceTitle = Best.Title;
		Answer.Panel = Best.Panel;
		Answer.Control = Best.Control;

		TArray<FString> SeeAlso;
		for (int32 Position = 1; Position < Hits.Num(); ++Position)
		{
			if (Hits[Position].Score >= StageKnowledgeSearch::MinAnswerScore * 0.5f)
			{
				SeeAlso.Add(Index.Entries[Hits[Position].Entry].Title);
			}
		}
		const FText Text = ComposeEntry(Best, Bindings, Language);
		Answer.Text = SeeAlso.IsEmpty() ? Text
			: FText::Format(Frame(Language, LOCTEXT("SeeAlso", "{0}\nSee also: {1}."), TEXT("{0}\nՏես նաև՝ {1}։")), Text,
				FText::FromString(FString::Join(SeeAlso, TEXT("; "))));
		return Answer;
	}
}

#undef LOCTEXT_NAMESPACE
