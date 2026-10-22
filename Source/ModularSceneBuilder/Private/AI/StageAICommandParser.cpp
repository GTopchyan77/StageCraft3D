// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandParser.h"

#include "AI/StageAIArmenian.h"
#include "Algo/AllOf.h"
#include "Algo/Find.h"

#define LOCTEXT_NAMESPACE "StageAICommandParser"

namespace StageAICommandParser
{
	namespace
	{
		/** A prompt split into lowercase words: letters, digits, ':' and a '.' between digits; apostrophes are dropped. */
		struct FPrompt
		{
			FString Raw;
			TArray<FString> Words;
			bool bQuestion = false;

			explicit FPrompt(const FString& Text)
			{
				Raw = Text.Left(MaxPromptLength).TrimStartAndEnd();
				FString Word;
				const auto Flush = [this, &Word]()
				{
					if (!Word.IsEmpty())
					{
						Words.Add(MoveTemp(Word));
						Word.Reset();
					}
				};
				for (int32 Index = 0; Index < Raw.Len(); ++Index)
				{
					// Armenian words the lexicon does not know are kept (StageAIArmenian), so they count as letters here too.
					const TCHAR Char = FChar::ToLower(StageAILanguage::ToLowerArmenian(Raw[Index]));
					const bool bDecimalPoint = Char == TEXT('.') && !Word.IsEmpty() && FChar::IsDigit(Word[Word.Len() - 1])
						&& Index + 1 < Raw.Len() && FChar::IsDigit(Raw[Index + 1]);
					if (FChar::IsAlnum(Char) || StageAILanguage::IsArmenianLetter(Char) || Char == TEXT(':') || bDecimalPoint)
					{
						Word.AppendChar(Char);
					}
					else if (Char != TEXT('\''))
					{
						Flush();
					}
				}
				Flush();

				static const TCHAR* const QuestionStarts[] = { TEXT("what"), TEXT("whats"), TEXT("how"), TEXT("hows"), TEXT("is"), TEXT("are"),
					TEXT("which"), TEXT("show"), TEXT("tell"), TEXT("list"), TEXT("do"), TEXT("does") };
				bQuestion = Raw.Contains(TEXT("?")) || (!Words.IsEmpty() && Algo::FindByPredicate(QuestionStarts, [this](const TCHAR* Start) { return Words[0] == Start; }));
			}

			int32 Find(const TCHAR* Word, int32 From = 0) const
			{
				for (int32 Index = FMath::Max(From, 0); Index < Words.Num(); ++Index)
				{
					if (Words[Index] == Word)
					{
						return Index;
					}
				}
				return INDEX_NONE;
			}

			bool Has(const TCHAR* Word) const { return Find(Word) != INDEX_NONE; }

			bool HasAny(std::initializer_list<const TCHAR*> Candidates) const
			{
				for (const TCHAR* Candidate : Candidates)
				{
					if (Has(Candidate))
					{
						return true;
					}
				}
				return false;
			}

			/** Words appear in this order, next to each other. */
			bool HasPhrase(std::initializer_list<const TCHAR*> Phrase) const
			{
				const int32 Length = int32(Phrase.size());
				for (int32 Start = 0; Start + Length <= Words.Num(); ++Start)
				{
					int32 Offset = 0;
					for (const TCHAR* Part : Phrase)
					{
						if (Words[Start + Offset] != Part)
						{
							break;
						}
						++Offset;
					}
					if (Offset == Length)
					{
						return true;
					}
				}
				return false;
			}

			bool IsExactly(std::initializer_list<const TCHAR*> Phrase) const
			{
				return Words.Num() == int32(Phrase.size()) && HasPhrase(Phrase);
			}
		};

		/** A whole or decimal number written as digits, or "zero" to "twenty". */
		TOptional<double> ParseNumber(const FString& Word)
		{
			if (!Word.IsEmpty() && FChar::IsDigit(Word[0]) && Word.IsNumeric())
			{
				return FCString::Atod(*Word);
			}
			static const TCHAR* const Names[] = { TEXT("zero"), TEXT("one"), TEXT("two"), TEXT("three"), TEXT("four"), TEXT("five"), TEXT("six"),
				TEXT("seven"), TEXT("eight"), TEXT("nine"), TEXT("ten"), TEXT("eleven"), TEXT("twelve"), TEXT("thirteen"), TEXT("fourteen"),
				TEXT("fifteen"), TEXT("sixteen"), TEXT("seventeen"), TEXT("eighteen"), TEXT("nineteen"), TEXT("twenty") };
			for (int32 Value = 0; Value < UE_ARRAY_COUNT(Names); ++Value)
			{
				if (Word == Names[Value])
				{
					return double(Value);
				}
			}
			return {};
		}

		/** The number written right after the word Anchor ("cue 5", "fade 3"), skipping a few filler words. */
		TOptional<double> NumberAfter(const FPrompt& Prompt, const TCHAR* Anchor)
		{
			for (int32 Index = Prompt.Find(Anchor); Index != INDEX_NONE; Index = Prompt.Find(Anchor, Index + 1))
			{
				for (int32 Next = Index + 1; Next < Prompt.Words.Num() && Next <= Index + 3; ++Next)
				{
					if (TOptional<double> Number = ParseNumber(Prompt.Words[Next]))
					{
						return Number;
					}
					static const TCHAR* const Fillers[] = { TEXT("number"), TEXT("no"), TEXT("of"), TEXT("time"), TEXT("in"), TEXT("to"), TEXT("is") };
					if (!Algo::FindByPredicate(Fillers, [&Prompt, Next](const TCHAR* Filler) { return Prompt.Words[Next] == Filler; }))
					{
						break;
					}
				}
			}
			return {};
		}

		/** "heads" -> "head", "washes" -> "wash"; short words and "ss" endings are kept. */
		FString Singular(const FString& Word)
		{
			if (Word.Len() > 4 && (Word.EndsWith(TEXT("shes")) || Word.EndsWith(TEXT("ches")) || Word.EndsWith(TEXT("xes"))))
			{
				return Word.LeftChop(2);
			}
			if (Word.Len() > 3 && Word.EndsWith(TEXT("s")) && !Word.EndsWith(TEXT("ss")))
			{
				return Word.LeftChop(1);
			}
			return Word;
		}

		bool IsOneOf(const FString& Word, std::initializer_list<const TCHAR*> Candidates)
		{
			for (const TCHAR* Candidate : Candidates)
			{
				if (Word == Candidate)
				{
					return true;
				}
			}
			return false;
		}

		bool IsGenericFixtureWord(const FString& Word)
		{
			return IsOneOf(Singular(Word), { TEXT("light"), TEXT("lighting"), TEXT("fixture"), TEXT("lamp"), TEXT("unit"), TEXT("luminaire"), TEXT("instrument") });
		}

		/** Words of a name, lowercase and singular ("Moving Head (Spot)" -> moving, head, spot). */
		TArray<FString> NameWords(const FString& Name)
		{
			TArray<FString> Words = FPrompt(Name).Words;
			for (FString& Word : Words)
			{
				Word = Singular(Word);
			}
			return Words;
		}

		// --- Rules, tried in order by Parse ---

		TOptional<FStageAICommand> ParseExport(const FPrompt& Prompt)
		{
			if (!Prompt.HasAny({ TEXT("export"), TEXT("pdf"), TEXT("csv") }))
			{
				return {};
			}
			FStageAICommand Command;
			Command.Intent = EStageAIIntent::Export;
			const bool bAll = Prompt.HasAny({ TEXT("all"), TEXT("every"), TEXT("everything") });
			if (!bAll && Prompt.HasAny({ TEXT("bom"), TEXT("bill"), TEXT("materials"), TEXT("material"), TEXT("inventory") }))
			{
				Command.Report = EStageAIReport::BillOfMaterials;
			}
			else if (!bAll && Prompt.HasAny({ TEXT("patch"), TEXT("dmx") }))
			{
				Command.Report = EStageAIReport::PatchSheet;
			}
			else if (!bAll && Prompt.HasAny({ TEXT("cable"), TEXT("cables"), TEXT("cabling"), TEXT("schedule"), TEXT("power"), TEXT("shopping") }))
			{
				Command.Report = EStageAIReport::CableSchedule;
			}
			return Command;
		}

		/** The label of "store cue 3 called Intro" or "record cue "Walk in"", in the prompt's own capitals. */
		FString FindLabel(const FPrompt& Prompt)
		{
			for (const TCHAR Quote : { TEXT('"'), TEXT('“') })
			{
				int32 Open = INDEX_NONE;
				if (Prompt.Raw.FindChar(Quote, Open))
				{
					const TCHAR Close = Quote == TEXT('"') ? TEXT('"') : TEXT('”');
					const int32 End = Prompt.Raw.Find(FString::Chr(Close), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
					if (End != INDEX_NONE)
					{
						return Prompt.Raw.Mid(Open + 1, End - Open - 1).TrimStartAndEnd();
					}
				}
			}

			// Unquoted: the original words after "called", up to the fade.
			TArray<FString> RawWords;
			Prompt.Raw.ParseIntoArrayWS(RawWords);
			for (int32 Index = 0; Index + 1 < RawWords.Num(); ++Index)
			{
				const FString Lower = RawWords[Index].ToLower();
				if (!IsOneOf(Lower, { TEXT("called"), TEXT("named"), TEXT("label"), TEXT("labelled"), TEXT("labeled"), TEXT("titled") }))
				{
					continue;
				}
				TArray<FString> LabelWords;
				for (int32 Next = Index + 1; Next < RawWords.Num(); ++Next)
				{
					const FString NextLower = RawWords[Next].ToLower();
					if (IsOneOf(NextLower, { TEXT("fade"), TEXT("with"), TEXT("in"), TEXT("and") }))
					{
						break;
					}
					LabelWords.Add(RawWords[Next]);
				}
				return FString::Join(LabelWords, TEXT(" ")).TrimStartAndEnd();
			}
			return FString();
		}

		TOptional<FStageAICommand> ParseStoreCue(const FPrompt& Prompt)
		{
			if (!Prompt.HasAny({ TEXT("cue"), TEXT("cues") })
				|| !Prompt.HasAny({ TEXT("store"), TEXT("record"), TEXT("save"), TEXT("add"), TEXT("create"), TEXT("make"), TEXT("program"), TEXT("new") }))
			{
				return {};
			}
			FStageAICommand Command;
			Command.Intent = EStageAIIntent::StoreCue;
			if (const TOptional<double> Number = NumberAfter(Prompt, TEXT("cue")))
			{
				Command.CueNumber = float(*Number);
			}
			Command.Label = FindLabel(Prompt);
			if (TOptional<double> Fade = NumberAfter(Prompt, TEXT("fade")))
			{
				Command.FadeSeconds = float(*Fade);
			}
			else
			{
				// "... in 3 seconds".
				for (int32 Index = 0; Index + 1 < Prompt.Words.Num(); ++Index)
				{
					if (const TOptional<double> Seconds = ParseNumber(Prompt.Words[Index]))
					{
						if (IsOneOf(Prompt.Words[Index + 1], { TEXT("s"), TEXT("sec"), TEXT("secs"), TEXT("second"), TEXT("seconds") }))
						{
							Command.FadeSeconds = float(*Seconds);
						}
					}
				}
			}
			return Command;
		}

		TOptional<FStageAICommand> ParseCuePlayback(const FPrompt& Prompt)
		{
			FStageAICommand Command;
			const bool bCue = Prompt.HasAny({ TEXT("cue"), TEXT("q") });
			const TOptional<double> Number = NumberAfter(Prompt, TEXT("cue"));
			if (bCue && Number.IsSet()
				&& (Prompt.HasAny({ TEXT("go"), TEXT("goto"), TEXT("play"), TEXT("run"), TEXT("fire"), TEXT("jump"), TEXT("recall"), TEXT("trigger") })
					|| (Prompt.Words.Num() == 2 && Prompt.Words[0] == TEXT("cue"))))
			{
				Command.Intent = EStageAIIntent::GoToCue;
				Command.CueNumber = float(*Number);
				return Command;
			}
			if (Prompt.IsExactly({ TEXT("go") }) || Prompt.HasPhrase({ TEXT("next"), TEXT("cue") }) || (bCue && Prompt.Has(TEXT("go")) && !Number.IsSet()))
			{
				Command.Intent = EStageAIIntent::CueGo;
				return Command;
			}
			if (Prompt.IsExactly({ TEXT("back") }) || Prompt.IsExactly({ TEXT("go"), TEXT("back") }) || Prompt.HasPhrase({ TEXT("previous"), TEXT("cue") })
				|| (bCue && Prompt.Has(TEXT("back"))))
			{
				Command.Intent = EStageAIIntent::CueBack;
				return Command;
			}
			return {};
		}

		TOptional<FStageAICommand> ParseUnsupported(const FPrompt& Prompt)
		{
			FStageAICommand Command;
			Command.Intent = EStageAIIntent::Unsupported;
			if (Prompt.HasAny({ TEXT("delete"), TEXT("remove"), TEXT("erase"), TEXT("wipe"), TEXT("clear"), TEXT("destroy") }))
			{
				Command.ItemQuery = TEXT("delete");
				return Command;
			}
			if (Prompt.HasAny({ TEXT("open"), TEXT("load") }) || Prompt.HasPhrase({ TEXT("new"), TEXT("scene") }))
			{
				Command.ItemQuery = TEXT("open");
				return Command;
			}
			if (Prompt.HasAny({ TEXT("save") }))
			{
				Command.ItemQuery = TEXT("save");
				return Command;
			}
			return {};
		}

		TOptional<EStageWeatherPreset> FindWeather(const FPrompt& Prompt)
		{
			if (Prompt.HasAny({ TEXT("rain"), TEXT("rainy"), TEXT("raining"), TEXT("drizzle"), TEXT("storm"), TEXT("stormy") }))
			{
				// "stop the rain", "no more rain".
				return Prompt.HasAny({ TEXT("stop"), TEXT("no"), TEXT("without") }) ? EStageWeatherPreset::Clear : EStageWeatherPreset::Rain;
			}
			if (Prompt.HasAny({ TEXT("snow"), TEXT("snowy"), TEXT("snowing"), TEXT("blizzard") }))
			{
				return Prompt.HasAny({ TEXT("stop"), TEXT("no"), TEXT("without") }) ? EStageWeatherPreset::Clear : EStageWeatherPreset::Snow;
			}
			if (Prompt.HasAny({ TEXT("fog"), TEXT("foggy"), TEXT("mist"), TEXT("misty"), TEXT("haze"), TEXT("hazy") }))
			{
				return Prompt.HasAny({ TEXT("stop"), TEXT("no"), TEXT("without") }) ? EStageWeatherPreset::Clear : EStageWeatherPreset::Fog;
			}
			if (Prompt.HasAny({ TEXT("cloudy"), TEXT("clouds"), TEXT("overcast"), TEXT("grey"), TEXT("gray") }))
			{
				return EStageWeatherPreset::Cloudy;
			}
			if (Prompt.HasAny({ TEXT("sunny"), TEXT("sunshine") })
				|| (Prompt.Has(TEXT("clear")) && Prompt.HasAny({ TEXT("sky"), TEXT("skies"), TEXT("weather"), TEXT("day") })))
			{
				return EStageWeatherPreset::Clear;
			}
			return {};
		}

		/** "18:30", "6 pm", "at 9", "sunset", "noon". */
		TOptional<float> FindHour(const FPrompt& Prompt)
		{
			for (int32 Index = 0; Index < Prompt.Words.Num(); ++Index)
			{
				const FString& Word = Prompt.Words[Index];
				FString HourPart, MinutePart;
				if (Word.Split(TEXT(":"), &HourPart, &MinutePart) && HourPart.IsNumeric() && MinutePart.IsNumeric() && !HourPart.IsEmpty() && !MinutePart.IsEmpty())
				{
					return float(FCString::Atoi(*HourPart)) + float(FCString::Atoi(*MinutePart)) / 60.f;
				}
				const bool bPm = Word.EndsWith(TEXT("pm")) || (Index + 1 < Prompt.Words.Num() && Prompt.Words[Index + 1] == TEXT("pm"));
				const bool bAm = Word.EndsWith(TEXT("am")) || (Index + 1 < Prompt.Words.Num() && Prompt.Words[Index + 1] == TEXT("am"));
				const FString Digits = (bPm || bAm) && (Word.EndsWith(TEXT("pm")) || Word.EndsWith(TEXT("am"))) ? Word.LeftChop(2) : Word;
				if (const TOptional<double> Number = ParseNumber(Digits))
				{
					const bool bTimeContext = bPm || bAm || (Index > 0 && IsOneOf(Prompt.Words[Index - 1], { TEXT("at"), TEXT("to") }) && Prompt.HasAny({ TEXT("time"), TEXT("clock"), TEXT("oclock") }))
						|| (Index + 1 < Prompt.Words.Num() && Prompt.Words[Index + 1] == TEXT("oclock"));
					if (bTimeContext)
					{
						float Hour = float(*Number);
						if (bPm && Hour < 12.f)
						{
							Hour += 12.f;
						}
						else if (bAm && Hour >= 12.f)
						{
							Hour -= 12.f;
						}
						return Hour;
					}
				}
			}

			struct FNamedTime { const TCHAR* Word; float Hour; };
			static const FNamedTime NamedTimes[] = {
				{ TEXT("sunrise"), 6.5f }, { TEXT("dawn"), 6.f }, { TEXT("morning"), 9.f }, { TEXT("noon"), 12.f }, { TEXT("midday"), 12.f },
				{ TEXT("afternoon"), 15.f }, { TEXT("evening"), 19.f }, { TEXT("sunset"), 19.5f }, { TEXT("dusk"), 20.f }, { TEXT("night"), 22.f },
				{ TEXT("nighttime"), 22.f }, { TEXT("midnight"), 0.f } };
			for (const FNamedTime& Named : NamedTimes)
			{
				if (Prompt.Has(Named.Word))
				{
					return Named.Hour;
				}
			}
			return {};
		}

		TOptional<bool> FindSky(const FPrompt& Prompt)
		{
			if (Prompt.HasAny({ TEXT("outdoor"), TEXT("outdoors"), TEXT("outside") }) || Prompt.HasPhrase({ TEXT("open"), TEXT("air") })
				|| Prompt.HasPhrase({ TEXT("sky"), TEXT("on") }) || Prompt.HasPhrase({ TEXT("turn"), TEXT("on"), TEXT("the"), TEXT("sky") })
				|| Prompt.HasPhrase({ TEXT("enable"), TEXT("the"), TEXT("sky") }) || Prompt.HasPhrase({ TEXT("show"), TEXT("the"), TEXT("sky") }))
			{
				return true;
			}
			if (Prompt.HasAny({ TEXT("indoor"), TEXT("indoors"), TEXT("inside") }) || Prompt.HasPhrase({ TEXT("sky"), TEXT("off") })
				|| Prompt.HasPhrase({ TEXT("turn"), TEXT("off"), TEXT("the"), TEXT("sky") }) || Prompt.HasPhrase({ TEXT("disable"), TEXT("the"), TEXT("sky") })
				|| Prompt.HasPhrase({ TEXT("hide"), TEXT("the"), TEXT("sky") }))
			{
				return false;
			}
			return {};
		}

		TOptional<FStageAICommand> ParseAddFixtures(const FPrompt& Prompt)
		{
			if (Prompt.bQuestion || !Prompt.HasAny({ TEXT("add"), TEXT("place"), TEXT("spawn"), TEXT("put"), TEXT("create"), TEXT("insert"), TEXT("hang"), TEXT("rig") }))
			{
				return {};
			}
			FStageAICommand Command;
			Command.Intent = EStageAIIntent::AddFixtures;
			bool bExplicitCount = false;
			TArray<FString> QueryWords;
			for (const FString& Word : Prompt.Words)
			{
				if (!Command.Count.IsSet())
				{
					if (const TOptional<double> Number = ParseNumber(Word))
					{
						// Fractions and huge values are left for Validate to refuse rather than silently rounded here.
						Command.Count = int32(FMath::Clamp(*Number, -1.0, 1000.0));
						bExplicitCount = true;
						continue;
					}
					if (IsOneOf(Word, { TEXT("a"), TEXT("an"), TEXT("another"), TEXT("single") }))
					{
						Command.Count = 1;
						continue;
					}
				}
				// "a couple", "a few", "a dozen": the amount word replaces the article's one.
				if (!bExplicitCount)
				{
					if (IsOneOf(Word, { TEXT("couple"), TEXT("pair") }))
					{
						Command.Count = 2;
						bExplicitCount = true;
						continue;
					}
					if (Word == TEXT("few"))
					{
						Command.Count = 3;
						bExplicitCount = true;
						continue;
					}
					if (Word == TEXT("dozen"))
					{
						Command.Count = 12;
						bExplicitCount = true;
						continue;
					}
				}
				if (IsOneOf(Word, { TEXT("add"), TEXT("place"), TEXT("spawn"), TEXT("put"), TEXT("create"), TEXT("insert"), TEXT("hang"), TEXT("rig"), TEXT("please"),
					TEXT("the"), TEXT("some"), TEXT("more"), TEXT("of"), TEXT("on"), TEXT("to"), TEXT("in"), TEXT("stage"), TEXT("me"), TEXT("new"), TEXT("x"),
					TEXT("a"), TEXT("an"), TEXT("another"), TEXT("couple"), TEXT("pair"), TEXT("few"), TEXT("dozen"), TEXT("can"), TEXT("you"), TEXT("i"),
					TEXT("want"), TEXT("would"), TEXT("like"), TEXT("could"), TEXT("floor"), TEXT("row"), TEXT("front"), TEXT("there"), TEXT("here") }))
				{
					continue;
				}
				QueryWords.Add(Word);
			}
			Command.ItemQuery = FString::Join(QueryWords, TEXT(" "));
			return Command;
		}

		TOptional<FStageAICommand> ParseQuestion(const FPrompt& Prompt)
		{
			FStageAICommand Command;
			if (Prompt.HasAny({ TEXT("weight"), TEXT("heavy"), TEXT("weigh"), TEXT("weighs"), TEXT("kg"), TEXT("mass"), TEXT("kilos"), TEXT("kilograms") }))
			{
				Command.Intent = EStageAIIntent::TotalWeight;
				return Command;
			}
			if (Prompt.HasAny({ TEXT("power"), TEXT("watts"), TEXT("watt"), TEXT("kw"), TEXT("kilowatts"), TEXT("draw"), TEXT("load"), TEXT("electricity"),
				TEXT("consumption"), TEXT("amps") }))
			{
				Command.Intent = EStageAIIntent::PowerDraw;
				return Command;
			}
			if (Prompt.HasAny({ TEXT("summary"), TEXT("status"), TEXT("stats"), TEXT("statistics"), TEXT("overview"), TEXT("totals") })
				|| Prompt.IsExactly({ TEXT("total") }))
			{
				Command.Intent = EStageAIIntent::StageSummary;
				return Command;
			}

			const bool bCounting = Prompt.HasPhrase({ TEXT("how"), TEXT("many") }) || Prompt.HasAny({ TEXT("count"), TEXT("number") });
			TArray<FString> Target;
			for (const FString& Word : Prompt.Words)
			{
				if (!IsOneOf(Word, { TEXT("how"), TEXT("many"), TEXT("count"), TEXT("number"), TEXT("of"), TEXT("are"), TEXT("there"), TEXT("is"), TEXT("the"),
					TEXT("on"), TEXT("stage"), TEXT("do"), TEXT("i"), TEXT("we"), TEXT("have"), TEXT("placed"), TEXT("in"), TEXT("scene"), TEXT("total"),
					TEXT("my"), TEXT("what"), TEXT("whats"), TEXT("show"), TEXT("list"), TEXT("tell"), TEXT("me"), TEXT("all"), TEXT("give"), TEXT("which"),
					TEXT("our"), TEXT("currently"), TEXT("now"), TEXT("so"), TEXT("far"), TEXT("used"), TEXT("got"),
					TEXT("section"), TEXT("sections"), TEXT("piece"), TEXT("pieces"), TEXT("unit"), TEXT("units") }))
				{
					Target.Add(Word);
				}
			}
			const bool bOnlyItems = Algo::AllOf(Target, [](const FString& Word)
			{
				return IsOneOf(Singular(Word), { TEXT("item"), TEXT("thing"), TEXT("object"), TEXT("piece"), TEXT("equipment"), TEXT("gear") });
			});
			const bool bOnlyFixtures = !Target.IsEmpty() && Algo::AllOf(Target, [](const FString& Word) { return IsGenericFixtureWord(Word); });
			if (bOnlyFixtures && (bCounting || Prompt.bQuestion || Target.Num() == Prompt.Words.Num()))
			{
				Command.Intent = EStageAIIntent::FixtureCount;
				return Command;
			}
			if (!bCounting)
			{
				return {};
			}
			if (bOnlyItems)
			{
				Command.Intent = EStageAIIntent::ItemCount;
				return Command;
			}
			Command.Intent = EStageAIIntent::CountMatching;
			Command.ItemQuery = FString::Join(Target, TEXT(" "));
			return Command;
		}

		/** The English grammar's rules, in order. */
		TOptional<FStageAICommand> ParseGrammar(const FString& Text)
		{
			const FPrompt Prompt(Text);
			if (Prompt.Words.IsEmpty())
			{
				return {};
			}

			FStageAICommand Command;
			if (Prompt.Words[0] == TEXT("undo") || Prompt.Words[0] == TEXT("redo"))
			{
				Command.Intent = Prompt.Words[0] == TEXT("undo") ? EStageAIIntent::Undo : EStageAIIntent::Redo;
				return Command;
			}
			if (Prompt.HasAny({ TEXT("help"), TEXT("commands") }) || Prompt.HasPhrase({ TEXT("what"), TEXT("can"), TEXT("you"), TEXT("do") })
				|| Prompt.IsExactly({ TEXT("hi") }) || Prompt.IsExactly({ TEXT("hello") }))
			{
				Command.Intent = EStageAIIntent::Help;
				return Command;
			}
			if (TOptional<FStageAICommand> Export = ParseExport(Prompt))
			{
				return Export;
			}
			if (TOptional<FStageAICommand> StoreCue = ParseStoreCue(Prompt))
			{
				return StoreCue;
			}
			if (TOptional<FStageAICommand> Playback = ParseCuePlayback(Prompt))
			{
				return Playback;
			}
			if (Prompt.HasAny({ TEXT("cue"), TEXT("cues") }) && !Prompt.HasAny({ TEXT("delete"), TEXT("remove") }))
			{
				Command.Intent = EStageAIIntent::ListCues;
				return Command;
			}

			// The environment: a question about it, else a change to it.
			if (Prompt.bQuestion && Prompt.HasAny({ TEXT("weather"), TEXT("time"), TEXT("sky"), TEXT("sun"), TEXT("raining"), TEXT("snowing"), TEXT("environment"),
				TEXT("outdoor"), TEXT("outdoors"), TEXT("outside") }))
			{
				Command.Intent = EStageAIIntent::EnvironmentStatus;
				return Command;
			}
			if (!Prompt.bQuestion)
			{
				if (const TOptional<EStageWeatherPreset> Weather = FindWeather(Prompt))
				{
					Command.Intent = EStageAIIntent::SetWeather;
					Command.Weather = *Weather;
					return Command;
				}
				if (const TOptional<float> Hour = FindHour(Prompt))
				{
					Command.Intent = EStageAIIntent::SetTimeOfDay;
					Command.Hour = *Hour;
					return Command;
				}
				if (const TOptional<bool> bSkyOn = FindSky(Prompt))
				{
					Command.Intent = EStageAIIntent::SetSky;
					Command.bSkyOn = *bSkyOn;
					return Command;
				}
			}

			if (TOptional<FStageAICommand> Unsupported = ParseUnsupported(Prompt))
			{
				return Unsupported;
			}
			if (TOptional<FStageAICommand> Add = ParseAddFixtures(Prompt))
			{
				return Add;
			}
			return ParseQuestion(Prompt);
		}
	}

	TOptional<FStageAICommand> Parse(const FString& Text)
	{
		const FString Capped = Text.Left(MaxPromptLength);
		const EStageAILanguage Language = StageAILanguage::Detect(Capped);
		// Armenian is read through the same rules, rewritten into the words they know.
		TOptional<FStageAICommand> Command = ParseGrammar(Language == EStageAILanguage::Armenian ? StageAIArmenian::TranslateToGrammar(Capped) : Capped);
		if (Command.IsSet())
		{
			Command->Language = Language;
		}
		return Command;
	}

	int32 MatchFixture(TConstArrayView<FStageAIFixtureCandidate> Candidates, const FString& Query)
	{
		if (Candidates.IsEmpty())
		{
			return INDEX_NONE;
		}

		TArray<FString> QueryWords;
		for (const FString& Word : FPrompt(Query).Words)
		{
			if (IsGenericFixtureWord(Word))
			{
				continue;
			}
			const FString One = Singular(Word);
			// Common rigging slang for the catalog's type names.
			if (One == TEXT("mover"))
			{
				QueryWords.Append({ TEXT("moving"), TEXT("head") });
			}
			else if (One == TEXT("parcan") || One == TEXT("can"))
			{
				QueryWords.Add(TEXT("par"));
			}
			else
			{
				QueryWords.AddUnique(One);
			}
		}
		if (QueryWords.IsEmpty())
		{
			return 0;
		}

		int32 Best = INDEX_NONE;
		int32 BestScore = 0;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			TArray<FString> CandidateWords = NameWords(Candidates[Index].Name);
			CandidateWords.Append(NameWords(Candidates[Index].KindName));
			int32 Score = 0;
			for (const FString& Word : QueryWords)
			{
				Score += CandidateWords.Contains(Word) ? 1 : 0;
			}
			if (Score > BestScore)
			{
				Best = Index;
				BestScore = Score;
			}
		}
		return Best;
	}

	TArray<FVector2D> LayoutFixtureRow(int32 Count, TOptional<double> LowestPlacedX)
	{
		const int32 Clamped = FMath::Clamp(Count, 1, MaxFixturesPerCommand);
		const double X = LowestPlacedX.IsSet() ? FMath::GridSnap(*LowestPlacedX - FixtureRowGap, FixtureRowGrid) : 0.0;
		TArray<FVector2D> Points;
		Points.Reserve(Clamped);
		for (int32 Index = 0; Index < Clamped; ++Index)
		{
			const double Y = (double(Index) - double(Clamped - 1) * 0.5) * FixtureRowSpacing;
			Points.Add(FVector2D(X, FMath::GridSnap(Y, FixtureRowGrid)));
		}
		return Points;
	}
}


namespace StageAIAnswers
{
	namespace
	{
		using StageAILanguage::Pick;

		FText FormatWatts(float Watts, EStageAILanguage Language)
		{
			FNumberFormattingOptions OneDecimal;
			OneDecimal.SetMaximumFractionalDigits(1);
			OneDecimal.SetMinimumFractionalDigits(1);
			return Watts >= 1000.f
				? FText::Format(Pick(Language, LOCTEXT("Kilowatts", "{0} kW"), INVTEXT("{0} կՎտ")), FText::AsNumber(Watts / 1000.f, &OneDecimal))
				: FText::Format(Pick(Language, LOCTEXT("Watts", "{0} W"), INVTEXT("{0} Վտ")), FText::AsNumber(FMath::RoundToInt(Watts)));
		}

		FText FormatKg(float Kg, EStageAILanguage Language)
		{
			return FText::Format(Pick(Language, LOCTEXT("Kilograms", "{0} kg"), INVTEXT("{0} կգ")), FText::AsNumber(FMath::RoundToInt(Kg)));
		}

		FText FormatPercent(float Value, float Limit)
		{
			return FText::AsPercent(Limit > 0.f ? Value / Limit : 0.f);
		}

		/** "4 × Spot 350, 2 × LED Par". */
		FText JoinCounts(TConstArrayView<TPair<FString, int32>> Counts)
		{
			TArray<FText> Parts;
			for (const TPair<FString, int32>& Entry : Counts)
			{
				Parts.Add(FText::Format(LOCTEXT("CountedName", "{0} × {1}"), FText::AsNumber(Entry.Value), FText::FromString(Entry.Key)));
			}
			return FText::Join(LOCTEXT("ListSeparator", ", "), Parts);
		}

		FText DescribeEnvironment(const FStageAISceneView& Scene, EStageAILanguage Language)
		{
			if (!Scene.bHasEnvironment)
			{
				return Pick(Language, LOCTEXT("NoEnvironment", "This level has no outdoor environment."), INVTEXT("Այս մակարդակում բացօթյա միջավայր չկա։"));
			}
			const FStageEnvironmentSettings& Settings = Scene.Environment;
			const FText Time = FText::FromString(FormatHour(Settings.TimeOfDay));
			if (!Settings.bSkyEnabled)
			{
				return Scene.bSkyAvailable
					? FText::Format(Pick(Language, LOCTEXT("Indoor", "Indoor stage: the outdoor sky is off (its clock stands at {0}). Say \"go outdoors\" to turn it on."),
						INVTEXT("Փակ բեմ. բացօթյա երկինքն անջատված է (նրա ժամացույցը ցույց է տալիս {0})։ Գրեք «բացօթյա»՝ այն միացնելու համար։")), Time)
					: Pick(Language, LOCTEXT("IndoorNoSky", "Indoor stage. No sky system is installed in this build, so weather cannot be shown."),
						INVTEXT("Փակ բեմ։ Այս տարբերակում երկնքի համակարգ տեղադրված չէ, ուստի եղանակը չի կարող ցուցադրվել։"));
			}
			return FText::Format(Pick(Language, LOCTEXT("Outdoor", "Outdoor sky at {0}, {1}: clouds {2}, rain {3}, snow {4}, fog {5}."),
					INVTEXT("Բացօթյա երկինք, ժամը {0}, {1}. ամպեր {2}, անձրև {3}, ձյուն {4}, մառախուղ {5}։")), Time,
				DescribeWeather(StageEnvironment::FindWeatherPreset(Settings), Language), FText::AsPercent(Settings.CloudCoverage), FText::AsPercent(Settings.Rain),
				FText::AsPercent(Settings.Snow), FText::AsPercent(Settings.Fog));
		}

		/** Placed item names matching every word of Query (singular, case-insensitive), counted per name. */
		TArray<TPair<FString, int32>> CountMatching(TConstArrayView<FString> Names, const FString& Query)
		{
			using namespace StageAICommandParser;
			TArray<FString> QueryWords;
			for (const FString& Word : FPrompt(Query).Words)
			{
				QueryWords.Add(Singular(Word));
			}
			TArray<TPair<FString, int32>> Counts;
			if (QueryWords.IsEmpty())
			{
				return Counts;
			}
			for (const FString& Name : Names)
			{
				const TArray<FString> Words = NameWords(Name);
				if (!Algo::AllOf(QueryWords, [&Words](const FString& Word) { return Words.Contains(Word); }))
				{
					continue;
				}
				if (TPair<FString, int32>* Existing = Counts.FindByPredicate([&Name](const TPair<FString, int32>& Entry) { return Entry.Key == Name; }))
				{
					++Existing->Value;
				}
				else
				{
					Counts.Emplace(Name, 1);
				}
			}
			return Counts;
		}
	}

	FString FormatHour(float Hour)
	{
		const int32 Minutes = FMath::RoundToInt(FMath::Fmod(FMath::Fmod(Hour, 24.f) + 24.f, 24.f) * 60.f) % (24 * 60);
		return FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60);
	}

	FText DescribeWeather(EStageWeatherPreset Preset, EStageAILanguage Language)
	{
		switch (Preset)
		{
		case EStageWeatherPreset::Clear:	return Pick(Language, LOCTEXT("WeatherClear", "clear"), INVTEXT("պարզ"));
		case EStageWeatherPreset::Cloudy:	return Pick(Language, LOCTEXT("WeatherCloudy", "cloudy"), INVTEXT("ամպամած"));
		case EStageWeatherPreset::Rain:		return Pick(Language, LOCTEXT("WeatherRain", "rain"), INVTEXT("անձրև"));
		case EStageWeatherPreset::Snow:		return Pick(Language, LOCTEXT("WeatherSnow", "snow"), INVTEXT("ձյուն"));
		case EStageWeatherPreset::Fog:		return Pick(Language, LOCTEXT("WeatherFog", "fog"), INVTEXT("մառախուղ"));
		default:							return Pick(Language, LOCTEXT("WeatherCustom", "custom weather"), INVTEXT("անհատական եղանակ"));
		}
	}

	FText DescribeHelp(EStageAILanguage Language)
	{
		return Pick(Language, LOCTEXT("Help",
			"I can answer questions about the stage and make safe, undoable changes. Try:\n"
			"• \"status\", \"how many fixtures\", \"total weight\", \"power draw\", \"list cues\", \"how many truss\"\n"
			"• \"add 6 moving heads\", \"place a led par\"\n"
			"• \"store cue 5 called Intro fade 3\", \"go\", \"back\", \"go to cue 2\"\n"
			"• \"make it rain\", \"fog\", \"set time to 18:30\", \"sunset\", \"go outdoors\"\n"
			"• \"export bill of materials\", \"export patch sheet\", \"export cable schedule\", \"export all reports\"\n"
			"• \"undo\", \"redo\"\n"
			"I also understand Armenian. Everything I change goes through the stage's rules and can be undone with Ctrl+Z."),
			INVTEXT(
			"Կարող եմ պատասխանել բեմի մասին հարցերին և կատարել անվտանգ փոփոխություններ, որոնք կարելի է հետարկել։ Փորձեք՝\n"
			"• վիճակը · քանի՞ սարք կա · ընդհանուր քաշը · հզորությունը · կյուների ցանկը · քանի՞ ֆերմա կա\n"
			"• ավելացրու 6 շարժական գլուխ · տեղադրիր մեկ LED փար\n"
			"• պահիր կյու 5 «Ներածություն» 3 վայրկյան · գնա · հետ · գնա կյու 2\n"
			"• թող անձրև գա · մառախուղ · ժամը 18:30 · մայրամուտ · բացօթյա\n"
			"• արտահանիր նյութերի ցուցակը · արտահանիր պատչը · արտահանիր մալուխների ցուցակը · արտահանիր բոլոր հաշվետվությունները\n"
			"• հետարկիր · կրկնիր\n"
			"Հասկանում եմ նաև անգլերեն։ Իմ բոլոր փոփոխություններն անցնում են բեմի կանոններով և հետարկվում են Ctrl+Z-ով։"));
	}

	FText DescribeNotUnderstood(const FString& Prompt)
	{
		return FText::Format(Pick(StageAILanguage::Detect(Prompt), LOCTEXT("NotUnderstood", "Sorry, I didn't understand \"{0}\". Type \"help\" to see what I can do."),
			INVTEXT("Ներեցեք, չհասկացա «{0}»։ Գրեք «օգնություն»՝ տեսնելու, թե ինչ կարող եմ անել։")), FText::FromString(Prompt.Left(80)));
	}

	FText DescribeUnsupported(const FString& Operation, EStageAILanguage Language)
	{
		if (Operation == TEXT("open") || Operation == TEXT("save"))
		{
			return Pick(Language, LOCTEXT("UnsupportedFile", "I don't open, save or replace scenes from the chat, so nothing is lost by accident. Use the File menu (Save, Save As, Open); it asks before discarding changes."),
				INVTEXT("Զրույցից չեմ բացում, չեմ պահպանում և չեմ փոխարինում տեսարաններ, որպեսզի ոչինչ պատահաբար չկորչի։ Օգտագործեք File ընտրացանկը (Save, Save As, Open). այն հարցնում է նախքան փոփոխությունները կորցնելը։"));
		}
		return Pick(Language, LOCTEXT("UnsupportedDelete", "I don't delete or clear things from the chat. Select items and press Delete, or use Edit › Clear Stage, which asks first. Ctrl+Z brings them back. Cues are deleted in the Cues panel."),
			INVTEXT("Զրույցից չեմ ջնջում և չեմ մաքրում իրեր։ Ընտրեք դրանք և սեղմեք Delete, կամ օգտագործեք Edit › Clear Stage, որը նախ հարցնում է։ Ctrl+Z-ը դրանք հետ է բերում։ Կյուները ջնջվում են Cues վահանակում։"));
	}

	FText AnswerQuestion(const FStageAICommand& Command, const FStageAISceneView& Scene)
	{
		const EStageAILanguage Language = Command.Language;
		switch (Command.Intent)
		{
		case EStageAIIntent::ItemCount:
			return Scene.MaxPlacedItems > 0
				? FText::Format(Pick(Language, LOCTEXT("ItemCountLimit", "{0} {0}|plural(one=item is,other=items are) placed, of {1} allowed."), INVTEXT("Տեղադրված է {0} իր՝ թույլատրված {1}-ից։")),
					FText::AsNumber(Scene.PlacedItems), FText::AsNumber(Scene.MaxPlacedItems))
				: FText::Format(Pick(Language, LOCTEXT("ItemCount", "{0} {0}|plural(one=item is,other=items are) placed."), INVTEXT("Տեղադրված է {0} իր։")), FText::AsNumber(Scene.PlacedItems));

		case EStageAIIntent::TotalWeight:
			return Scene.MaxWeightKg > 0.f
				? FText::Format(Pick(Language, LOCTEXT("WeightLimit", "Total weight: {0} of {1} allowed ({2})."), INVTEXT("Ընդհանուր քաշը՝ {0}՝ թույլատրված {1}-ից ({2})։")),
					FormatKg(Scene.TotalWeightKg, Language), FormatKg(Scene.MaxWeightKg, Language), FormatPercent(Scene.TotalWeightKg, Scene.MaxWeightKg))
				: FText::Format(Pick(Language, LOCTEXT("Weight", "Total weight: {0}, across {1} {1}|plural(one=item,other=items)."), INVTEXT("Ընդհանուր քաշը՝ {0} ({1} իր)։")),
					FormatKg(Scene.TotalWeightKg, Language), FText::AsNumber(Scene.PlacedItems));

		case EStageAIIntent::PowerDraw:
			return Scene.MaxPowerWatts > 0.f
				? FText::Format(Pick(Language, LOCTEXT("PowerLimit", "Total power draw: {0} of the venue's {1} ({2})."), INVTEXT("Ընդհանուր հզորությունը՝ {0}՝ վայրի {1}-ից ({2})։")),
					FormatWatts(Scene.TotalPowerWatts, Language), FormatWatts(Scene.MaxPowerWatts, Language), FormatPercent(Scene.TotalPowerWatts, Scene.MaxPowerWatts))
				: FText::Format(Pick(Language, LOCTEXT("Power", "Total power draw: {0}, across {1} {1}|plural(one=item,other=items). The cable schedule export plans the circuits."),
					INVTEXT("Ընդհանուր հզորությունը՝ {0} ({1} իր)։ Մալուխների ցուցակի արտահանումը պլանավորում է շղթաները։")),
					FormatWatts(Scene.TotalPowerWatts, Language), FText::AsNumber(Scene.PlacedItems));

		case EStageAIIntent::FixtureCount:
		{
			const int32 Fixtures = Scene.GetFixtureCount();
			return Fixtures == 0
				? Pick(Language, LOCTEXT("NoFixtures", "No lighting fixtures are placed. Say \"add 4 fixtures\" to place some."),
					INVTEXT("Լուսային սարքեր տեղադրված չեն։ Գրեք «ավելացրու 4 լույս»՝ մի քանիսը տեղադրելու համար։"))
				: FText::Format(Pick(Language, LOCTEXT("Fixtures", "{0} {0}|plural(one=fixture,other=fixtures): {1}."), INVTEXT("{0} լուսային սարք՝ {1}։")),
					FText::AsNumber(Fixtures), JoinCounts(Scene.FixturesByName));
		}

		case EStageAIIntent::CountMatching:
		{
			const TArray<TPair<FString, int32>> Counts = CountMatching(Scene.ItemNames, Command.ItemQuery);
			int32 Total = 0;
			for (const TPair<FString, int32>& Entry : Counts)
			{
				Total += Entry.Value;
			}
			return Total == 0
				? FText::Format(Pick(Language, LOCTEXT("NoneMatch", "None of the {0} placed {0}|plural(one=item,other=items) match \"{1}\"."), INVTEXT("Տեղադրված {0} իրերից ոչ մեկը չի համապատասխանում «{1}»-ին։")),
					FText::AsNumber(Scene.PlacedItems), FText::FromString(Command.ItemQuery))
				: FText::Format(Pick(Language, LOCTEXT("Matches", "{0} placed {0}|plural(one=item matches,other=items match) \"{1}\": {2}."), INVTEXT("«{1}»-ին համապատասխանում է {0} տեղադրված իր՝ {2}։")),
					FText::AsNumber(Total), FText::FromString(Command.ItemQuery), JoinCounts(Counts));
		}

		case EStageAIIntent::ListCues:
		{
			if (Scene.Cues.IsEmpty())
			{
				return Pick(Language, LOCTEXT("NoCues", "There are no cues yet. Set up a look, then say \"store cue 1\"."),
					INVTEXT("Կյուներ դեռ չկան։ Ստեղծեք լուսային պատկեր, ապա գրեք «պահիր կյու 1»։"));
			}
			constexpr int32 MaxListed = 12;
			FNumberFormattingOptions CueFormat;
			CueFormat.SetMaximumFractionalDigits(3);
			TArray<FText> Parts;
			for (int32 Index = 0; Index < Scene.Cues.Num() && Index < MaxListed; ++Index)
			{
				const FStageAISceneView::FCue& Cue = Scene.Cues[Index];
				const FText Number = FText::AsNumber(Cue.Number, &CueFormat);
				Parts.Add(Cue.Label.IsEmpty()
					? FText::Format(Pick(Language, LOCTEXT("CueEntry", "Q{0} ({1} s)"), INVTEXT("Q{0} ({1} վ)")), Number, FText::AsNumber(Cue.FadeSeconds, &CueFormat))
					: FText::Format(Pick(Language, LOCTEXT("CueEntryLabel", "Q{0} {1} ({2} s)"), INVTEXT("Q{0} {1} ({2} վ)")), Number, FText::FromString(Cue.Label),
						FText::AsNumber(Cue.FadeSeconds, &CueFormat)));
			}
			FText List = FText::Join(LOCTEXT("ListSeparator", ", "), Parts);
			if (Scene.Cues.Num() > MaxListed)
			{
				List = FText::Format(Pick(Language, LOCTEXT("CueMore", "{0} and {1} more"), INVTEXT("{0} և ևս {1}")), List, FText::AsNumber(Scene.Cues.Num() - MaxListed));
			}
			const FText Active = Scene.ActiveCue > 0.f
				? FText::Format(Pick(Language, LOCTEXT("ActiveCue", " Cue {0} is on stage."), INVTEXT(" Այժմ բեմում է կյու {0}-ը։")), FText::AsNumber(Scene.ActiveCue, &CueFormat))
				: FText::GetEmpty();
			return FText::Format(Pick(Language, LOCTEXT("Cues", "{0} {0}|plural(one=cue,other=cues): {1}.{2}"), INVTEXT("{0} կյու՝ {1}։{2}")), FText::AsNumber(Scene.Cues.Num()), List, Active);
		}

		case EStageAIIntent::EnvironmentStatus:
			return DescribeEnvironment(Scene, Language);

		case EStageAIIntent::StageSummary:
		default:
		{
			const FText Items = FText::Format(Pick(Language, LOCTEXT("SummaryItems", "{0} {0}|plural(one=item,other=items) ({1} {1}|plural(one=fixture,other=fixtures))"),
				INVTEXT("{0} իր ({1} լուսային սարք)")), FText::AsNumber(Scene.PlacedItems), FText::AsNumber(Scene.GetFixtureCount()));
			const FText Totals = FText::Format(Pick(Language, LOCTEXT("SummaryTotals", "drawing {0} and weighing {1}"), INVTEXT("{0} հզորությամբ և {1} քաշով")),
				FormatWatts(Scene.TotalPowerWatts, Language), FormatKg(Scene.TotalWeightKg, Language));
			const FText Cues = FText::Format(Pick(Language, LOCTEXT("SummaryCues", "{0} {0}|plural(one=cue,other=cues)"), INVTEXT("{0} կյու")), FText::AsNumber(Scene.Cues.Num()));
			return FText::Format(Pick(Language, LOCTEXT("Summary", "The stage has {0}, {1}. {2}. {3}"), INVTEXT("Բեմում կա {0}, {1}։ {2}։ {3}")), Items, Totals, Cues,
				DescribeEnvironment(Scene, Language));
		}
		}
	}
}

#undef LOCTEXT_NAMESPACE
