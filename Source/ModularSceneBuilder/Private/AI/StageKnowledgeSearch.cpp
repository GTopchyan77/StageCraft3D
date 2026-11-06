// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageKnowledgeSearch.h"

#include "AI/StageAIArmenian.h"
#include "AI/StageAILanguage.h"
#include "Algo/StableSort.h"

namespace StageKnowledgeSearch
{
	namespace
	{
		/** Words that carry no topic in either language. Matched after normalisation, before stemming. */
		const TSet<FString>& GetStopWords()
		{
			static const TSet<FString> Words = {
				// English.
				TEXT("a"), TEXT("an"), TEXT("the"), TEXT("to"), TEXT("of"), TEXT("in"), TEXT("on"), TEXT("and"), TEXT("or"), TEXT("is"),
				TEXT("are"), TEXT("be"), TEXT("do"), TEXT("does"), TEXT("did"), TEXT("i"), TEXT("me"), TEXT("my"), TEXT("you"), TEXT("your"),
				TEXT("it"), TEXT("its"), TEXT("this"), TEXT("that"), TEXT("how"), TEXT("what"), TEXT("where"), TEXT("why"), TEXT("when"),
				TEXT("which"), TEXT("can"), TEXT("could"), TEXT("should"), TEXT("would"), TEXT("will"), TEXT("with"), TEXT("for"),
				TEXT("from"), TEXT("at"), TEXT("by"), TEXT("as"), TEXT("into"), TEXT("about"), TEXT("please"), TEXT("tell"), TEXT("show"),
				TEXT("explain"), TEXT("there"), TEXT("them"), TEXT("they"), TEXT("we"), TEXT("us"), TEXT("find"), TEXT("get"), TEXT("use"),
				TEXT("using"), TEXT("work"), TEXT("works"), TEXT("mean"), TEXT("means"), TEXT("want"), TEXT("need"), TEXT("way"), TEXT("s"),
				TEXT("t"), TEXT("doesn"), TEXT("don"), TEXT("isn"), TEXT("happen"), TEXT("happens"), TEXT("some"), TEXT("any"), TEXT("all"),
				TEXT("help"), TEXT("so"), TEXT("if"), TEXT("then"), TEXT("than"), TEXT("not"), TEXT("no"), TEXT("one"), TEXT("make"),
				// Armenian (question marks already removed).
				TEXT("ինչպես"), TEXT("որտեղ"), TEXT("ինչ"), TEXT("ինչու"), TEXT("ինչի"), TEXT("ինչը"), TEXT("ինչն"), TEXT("է"), TEXT("են"),
				TEXT("եմ"), TEXT("ես"), TEXT("ենք"), TEXT("էր"), TEXT("և"), TEXT("ու"), TEXT("որ"), TEXT("մի"), TEXT("այս"), TEXT("այդ"),
				TEXT("այն"), TEXT("կարող"), TEXT("կարելի"), TEXT("պետք"), TEXT("իմ"), TEXT("ինձ"), TEXT("դու"), TEXT("դուք"), TEXT("համար"),
				TEXT("հետ"), TEXT("վրա"), TEXT("մեջ"), TEXT("կա"), TEXT("կան"), TEXT("չի"), TEXT("չեն"), TEXT("ոնց"), TEXT("թե"),
				TEXT("ասա"), TEXT("ասեք"), TEXT("բացատրիր"), TEXT("բացատրեք"), TEXT("ցույց"), TEXT("տուր"), TEXT("տվեք"), TEXT("օգնիր"),
				TEXT("օգնություն"), TEXT("կոճակ"), TEXT("կոճակը"), TEXT("անել"), TEXT("անեմ"), TEXT("գտնել"), TEXT("գտնեմ"),
			};
			return Words;
		}

		/** Armenian endings, longest first; a stem keeps at least three letters. */
		const TCHAR* const ArmenianEndings[] = {
			TEXT("ներից"), TEXT("ներով"), TEXT("ներում"), TEXT("ներին"), TEXT("ների"), TEXT("ները"), TEXT("ներն"),
			TEXT("երից"), TEXT("երով"), TEXT("երում"), TEXT("երին"), TEXT("երի"), TEXT("երը"), TEXT("երն"),
			TEXT("ներ"), TEXT("եր"), TEXT("ում"), TEXT("ից"), TEXT("ով"), TEXT("ին"), TEXT("իր"), TEXT("եք"), TEXT("ել"), TEXT("ալ"),
			TEXT("ի"), TEXT("ը"), TEXT("ն"),
		};

		bool IsWordChar(TCHAR Char)
		{
			return FChar::IsAlnum(Char) || StageAILanguage::IsArmenianLetter(Char);
		}

		/** Armenian punctuation written inside or after a word: question, emphasis and exclamation marks. */
		bool IsInWordMark(TCHAR Char)
		{
			return Char == 0x055E || Char == 0x055B || Char == 0x055C;
		}

		FString StemEnglish(FString Word)
		{
			const auto Ends = [&Word](const TCHAR* Suffix) { return Word.EndsWith(Suffix, ESearchCase::CaseSensitive); };
			if (Word.Len() > 4 && Ends(TEXT("ies")))
			{
				Word.LeftChopInline(3);
				Word.AppendChar(TEXT('y'));
			}
			else if (Word.Len() > 4 && Ends(TEXT("sses")))
			{
				Word.LeftChopInline(2);
			}
			else if (Word.Len() > 3 && Ends(TEXT("s")) && !Ends(TEXT("ss")) && !Ends(TEXT("us")))
			{
				Word.LeftChopInline(1);
			}
			if (Word.Len() > 5 && Ends(TEXT("ing")))
			{
				Word.LeftChopInline(3);
			}
			else if (Word.Len() > 4 && Ends(TEXT("ed")))
			{
				Word.LeftChopInline(2);
			}
			if (Word.Len() > 3 && Ends(TEXT("e")))
			{
				Word.LeftChopInline(1);
			}
			return Word;
		}

		FString StemArmenian(FString Word)
		{
			// Abstract nouns change their stem in the cases: -ություն, -ության, -ությամբ, -ությունը all become -ություն first.
			const int32 Abstract = Word.Find(TEXT("ությ"), ESearchCase::CaseSensitive);
			if (Abstract >= 2)
			{
				Word = Word.Left(Abstract) + TEXT("ություն");
			}
			// Endings stack (a case after a plural, the article after both), so they are taken off until none is left; a word and
			// its inflected forms then meet ("միկրոֆոն", "միկրոֆոնը").
			for (bool bStripped = true; bStripped;)
			{
				bStripped = false;
				for (const TCHAR* Ending : ArmenianEndings)
				{
					const int32 Length = FCString::Strlen(Ending);
					if (Word.Len() - Length >= 3 && Word.EndsWith(Ending, ESearchCase::CaseSensitive))
					{
						Word.LeftChopInline(Length);
						bStripped = true;
						break;
					}
				}
			}
			return Word;
		}

		void AddTerm(FString Word, TArray<FString>& OutTerms)
		{
			if (Word.IsEmpty() || GetStopWords().Contains(Word))
			{
				return;
			}
			const bool bArmenian = StageAILanguage::IsArmenianLetter(Word[0]);
			if (bArmenian && Word.Len() < 2)
			{
				// A lone ending split off a Latin word ("DMX-ի" -> "ի") carries no topic.
				return;
			}
			FString Stem = bArmenian ? StemArmenian(MoveTemp(Word)) : StemEnglish(MoveTemp(Word));
			if (!GetStopWords().Contains(Stem))
			{
				OutTerms.Add(MoveTemp(Stem));
			}
		}

		/** Lowered words of a phrase, Armenian marks removed: the input of IsHowToQuestion. */
		TArray<FString> LowerWords(const FString& Text)
		{
			TArray<FString> Words;
			FString Word;
			for (const TCHAR Char : Text)
			{
				if (IsInWordMark(Char))
				{
					continue;
				}
				// "where's" reads as "where s", so the pair "where" + "s" is found.
				if (IsWordChar(Char))
				{
					Word.AppendChar(StageAILanguage::IsArmenianLetter(Char) ? StageAILanguage::ToLowerArmenian(Char) : FChar::ToLower(Char));
				}
				else if (!Word.IsEmpty())
				{
					Words.Add(MoveTemp(Word));
					Word.Reset();
				}
			}
			if (!Word.IsEmpty())
			{
				Words.Add(MoveTemp(Word));
			}
			return Words;
		}
	}

	void Tokenize(const FString& Text, TArray<FString>& OutTerms)
	{
		FString Word;
		for (const TCHAR Char : Text)
		{
			if (IsInWordMark(Char))
			{
				continue;
			}
			if (IsWordChar(Char))
			{
				// A word that switches script ("dmx-ի") is split at the switch, so the Latin part stays searchable.
				const bool bArmenian = StageAILanguage::IsArmenianLetter(Char);
				if (!Word.IsEmpty() && bArmenian != StageAILanguage::IsArmenianLetter(Word[Word.Len() - 1]))
				{
					AddTerm(MoveTemp(Word), OutTerms);
					Word.Reset();
				}
				Word.AppendChar(bArmenian ? StageAILanguage::ToLowerArmenian(Char) : FChar::ToLower(Char));
			}
			else if (Char == TEXT('\'') && !Word.IsEmpty())
			{
				// "don't", "what's": the part after the apostrophe is dropped with the stop words.
				AddTerm(MoveTemp(Word), OutTerms);
				Word.Reset();
			}
			else if (!Word.IsEmpty())
			{
				AddTerm(MoveTemp(Word), OutTerms);
				Word.Reset();
			}
		}
		if (!Word.IsEmpty())
		{
			AddTerm(MoveTemp(Word), OutTerms);
		}
	}

	TArray<FString> MakeQueryTerms(const FString& Prompt)
	{
		TArray<FString> Terms;
		Tokenize(Prompt, Terms);
		if (StageAILanguage::Detect(Prompt) == EStageAILanguage::Armenian)
		{
			Tokenize(StageAIArmenian::TranslateToGrammar(Prompt), Terms);
		}
		TArray<FString> Unique;
		for (FString& Term : Terms)
		{
			Unique.AddUnique(MoveTemp(Term));
		}
		return Unique;
	}

	TArray<FHit> Search(const FStageKnowledgeIndex& Index, const FString& Prompt, EStageAILanguage Language, int32 MaxHits)
	{
		TArray<FHit> Hits;
		const int32 Count = Index.Num();
		if (Count == 0 || MaxHits <= 0 || Index.Documents.Num() != Count)
		{
			return Hits;
		}
		const TArray<FString> Terms = MakeQueryTerms(Prompt);
		struct FTermWeight
		{
			const FString* Term;
			float Idf;
		};
		TArray<FTermWeight> Weights;
		for (const FString& Term : Terms)
		{
			if (const int32* Frequency = Index.DocumentFrequency.Find(Term))
			{
				const float Idf = FMath::Loge(1.f + (Count - *Frequency + 0.5f) / (*Frequency + 0.5f));
				Weights.Add({ &Term, Idf });
			}
		}
		if (Weights.IsEmpty())
		{
			return Hits;
		}

		const float AverageLength = FMath::Max(Index.AverageLength, 1.f);
		TArray<FHit> Scored;
		for (int32 Entry = 0; Entry < Count; ++Entry)
		{
			const FStageKnowledgeIndex::FDocument& Document = Index.Documents[Entry];
			float Score = 0.f;
			for (const FTermWeight& Weight : Weights)
			{
				if (const float* Frequency = Document.Terms.Find(*Weight.Term))
				{
					Score += Weight.Idf * (*Frequency * (K1 + 1.f)) / (*Frequency + K1 * (1.f - B + B * Document.Length / AverageLength));
				}
			}
			if (Score > 0.f)
			{
				Scored.Add({ Entry, Score });
			}
		}
		// Stable: equal scores keep entry order, so the same index and prompt always rank the same way.
		Algo::StableSort(Scored, [](const FHit& A, const FHit& Bh) { return A.Score > Bh.Score; });

		TSet<FName> SeenIds;
		for (const FHit& Hit : Scored)
		{
			const FStageKnowledgeEntry& Found = Index.Entries[Hit.Entry];
			if (SeenIds.Contains(Found.Id))
			{
				continue;
			}
			SeenIds.Add(Found.Id);
			FHit Best = Hit;
			if (Found.Language != Language)
			{
				// Same knowledge, the asker's language: keep the score that found it.
				for (int32 Entry = 0; Entry < Count; ++Entry)
				{
					if (Index.Entries[Entry].Id == Found.Id && Index.Entries[Entry].Language == Language)
					{
						Best.Entry = Entry;
						break;
					}
				}
			}
			Hits.Add(Best);
			if (Hits.Num() >= MaxHits)
			{
				break;
			}
		}
		return Hits;
	}

	bool IsHowToQuestion(const FString& Prompt)
	{
		const TArray<FString> Words = LowerWords(Prompt);
		if (Words.IsEmpty())
		{
			return false;
		}
		const auto At = [&Words](int32 Index) -> const FString& { static const FString None; return Words.IsValidIndex(Index) ? Words[Index] : None; };
		const auto Has = [&Words](const TCHAR* Word) { return Words.Contains(Word); };
		const auto HasPair = [&Words](const TCHAR* First, const TCHAR* Second)
		{
			for (int32 Index = 0; Index + 1 < Words.Num(); ++Index)
			{
				if (Words[Index] == First && Words[Index + 1] == Second)
				{
					return true;
				}
			}
			return false;
		};

		// English: "how do/can/to/does ...", "where is/are/do/can ...", "what is/does/are ...", "why ...", "explain ...".
		const FString& First = At(0);
		if (First == TEXT("why") || First == TEXT("explain") || First == TEXT("describe"))
		{
			return true;
		}
		for (const TCHAR* Verb : { TEXT("do"), TEXT("does"), TEXT("can"), TEXT("to"), TEXT("should"), TEXT("would") })
		{
			if (HasPair(TEXT("how"), Verb))
			{
				return true;
			}
		}
		for (const TCHAR* Verb : { TEXT("is"), TEXT("are"), TEXT("do"), TEXT("can"), TEXT("does"), TEXT("s") })
		{
			if (HasPair(TEXT("where"), Verb))
			{
				return true;
			}
		}
		if (First == TEXT("what") && (At(1) == TEXT("is") || At(1) == TEXT("does") || At(1) == TEXT("are") || At(1) == TEXT("s") || At(1) == TEXT("do")))
		{
			// "what is the total weight" and "what is the power draw" are stage questions the parser answers; they never run
			// anything, so treating them as how-to only matters for actions.
			return true;
		}
		if (HasPair(TEXT("tell"), TEXT("me")) && Has(TEXT("about")))
		{
			return true;
		}
		if (HasPair(TEXT("show"), TEXT("me")) && (Has(TEXT("where")) || Has(TEXT("how"))))
		{
			return true;
		}

		// Armenian: ինչպես (how), որտեղ (where), ինչու (why), "ինչ է" (what is), բացատրիր (explain), ոնց (how, colloquial).
		for (const FString& Word : Words)
		{
			if (Word.StartsWith(TEXT("ինչպես")) || Word.StartsWith(TEXT("որտեղ")) || Word == TEXT("ինչու") || Word == TEXT("ոնց")
				|| Word.StartsWith(TEXT("բացատր")))
			{
				return true;
			}
		}
		return HasPair(TEXT("ինչ"), TEXT("է")) || HasPair(TEXT("ինչ"), TEXT("են")) || HasPair(TEXT("ինչի"), TEXT("համար"));
	}

	ERoute Route(const FString& Prompt, const FStageAICommand* Parsed)
	{
		if (!Parsed)
		{
			return ERoute::Knowledge;
		}
		if (Parsed->Intent == EStageAIIntent::Help)
		{
			// "help", "hi", "what can you do" have no topic; "help with patching" does.
			TArray<FString> Terms = MakeQueryTerms(Prompt);
			Terms.RemoveAll([](const FString& Term) { return Term == TEXT("command") || Term == TEXT("hi") || Term == TEXT("hello") || Term == TEXT("hey")
				|| Term == TEXT("օգն") || Term == TEXT("բարև") || Term == TEXT("բարեւ"); });
			return Terms.IsEmpty() ? ERoute::Command : ERoute::Knowledge;
		}
		return Parsed->IsAction() && IsHowToQuestion(Prompt) ? ERoute::KnowledgeOnly : ERoute::Command;
	}
}
