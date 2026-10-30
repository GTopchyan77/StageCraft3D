// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAIArmenian.h"

#include "AI/StageAILanguage.h"
#include "Algo/Find.h"

namespace StageAIArmenian
{
	namespace
	{
		/** One Armenian word or stem and the grammar words it stands for (empty: a filler word that is dropped). */
		struct FEntry
		{
			const TCHAR* Armenian;
			const TCHAR* Grammar;
			/** Matched only as the whole word, for short words that begin many others ("մեկ" in "մեկնարկ"). */
			bool bExact = false;
		};

		constexpr bool Exact = true;

		/** Two-word expressions, read before single words. */
		const FEntry Phrases[] = {
			{ TEXT("մի քանի"), TEXT("few") },
			{ TEXT("ցույց տուր"), TEXT("show") },
			{ TEXT("ցույց տվեք"), TEXT("show") },
			{ TEXT("ամեն ինչ"), TEXT("all") },
			{ TEXT("կեսօրից հետո"), TEXT("afternoon") },
			{ TEXT("նոր տեսարան"), TEXT("new scene") },
			{ TEXT("ինչ կարող"), TEXT("what can") },
		};

		const FEntry Lexicon[] = {
			// Adding and placing.
			{ TEXT("ավելացր"), TEXT("add") }, { TEXT("ավելացն"), TEXT("add") }, { TEXT("ավելացվ"), TEXT("add") },
			{ TEXT("տեղադրված"), TEXT("placed") }, { TEXT("տեղադր"), TEXT("place") },
			{ TEXT("դիր"), TEXT("put"), Exact }, { TEXT("դրեք"), TEXT("put"), Exact }, { TEXT("դնել"), TEXT("put"), Exact },
			{ TEXT("կախ"), TEXT("hang") }, { TEXT("ստեղծ"), TEXT("create") },

			// Cues and playback.
			{ TEXT("կյու"), TEXT("cue") }, { TEXT("քյու"), TEXT("cue") }, { TEXT("կիու"), TEXT("cue") }, { TEXT("կյուներ"), TEXT("cues") },
			{ TEXT("պահպան"), TEXT("save") }, { TEXT("պահիր"), TEXT("store") }, { TEXT("պահեք"), TEXT("store") }, { TEXT("պահել"), TEXT("store") },
			{ TEXT("գրանց"), TEXT("record") },
			{ TEXT("անցում"), TEXT("fade") }, { TEXT("ֆեյդ"), TEXT("fade") }, { TEXT("վայրկյան"), TEXT("seconds") }, { TEXT("վրկ"), TEXT("seconds") },
			{ TEXT("անունով"), TEXT("called") }, { TEXT("անունը"), TEXT("called") }, { TEXT("կոչվող"), TEXT("called") },
			{ TEXT("գնա"), TEXT("go") }, { TEXT("անցիր"), TEXT("go") }, { TEXT("խաղարկ"), TEXT("play") }, { TEXT("գործարկ"), TEXT("run") },
			{ TEXT("հաջորդ"), TEXT("next") }, { TEXT("նախորդ"), TEXT("previous") },
			{ TEXT("հետ"), TEXT("back"), Exact }, { TEXT("ետ"), TEXT("back"), Exact },
			// "List" is matched as whole words: the stem also begins ցանկանալ (to want).
			{ TEXT("ցանկ"), TEXT("list"), Exact }, { TEXT("ցանկը"), TEXT("list"), Exact }, { TEXT("ցանկի"), TEXT("list"), Exact }, { TEXT("ցանկում"), TEXT("list"), Exact },
			{ TEXT("ցանկան"), TEXT("") }, { TEXT("ցուցակ"), TEXT("list") },

			// History.
			// Redo only in its imperative and infinitive: the stem also begins կրկնակի (double).
			{ TEXT("հետարկ"), TEXT("undo") }, { TEXT("չեղարկ"), TEXT("undo") }, { TEXT("ետարկ"), TEXT("undo") },
			{ TEXT("կրկնիր"), TEXT("redo"), Exact }, { TEXT("կրկնեք"), TEXT("redo"), Exact }, { TEXT("կրկնել"), TEXT("redo"), Exact },

			// Reports.
			{ TEXT("արտահան"), TEXT("export") }, { TEXT("էքսպորտ"), TEXT("export") },
			{ TEXT("նյութ"), TEXT("materials") }, { TEXT("գույքագր"), TEXT("inventory") }, { TEXT("պատչ"), TEXT("patch") }, { TEXT("փաթչ"), TEXT("patch") },
			{ TEXT("մալուխ"), TEXT("cable") }, { TEXT("կաբել"), TEXT("cable") }, { TEXT("հաշվետվությ"), TEXT("") },

			// Totals.
			{ TEXT("քաշ"), TEXT("weight") }, { TEXT("կշռ"), TEXT("weigh") }, { TEXT("ծանր"), TEXT("heavy") }, { TEXT("կիլոգրամ"), TEXT("kg") }, { TEXT("կգ"), TEXT("kg"), Exact },
			{ TEXT("հզորությ"), TEXT("power") }, { TEXT("սնուց"), TEXT("power") }, { TEXT("էներգ"), TEXT("power") }, { TEXT("հոսանք"), TEXT("power") },
			{ TEXT("սպառ"), TEXT("consumption") }, { TEXT("կիլովատ"), TEXT("kw") }, { TEXT("վատտ"), TEXT("watts") },
			{ TEXT("ընդհանուր"), TEXT("total") }, { TEXT("բոլոր"), TEXT("all") }, { TEXT("ամբողջ"), TEXT("all") }, { TEXT("ամեն"), TEXT("all") },
			{ TEXT("վիճակագր"), TEXT("statistics") }, { TEXT("վիճակ"), TEXT("status") }, { TEXT("կարգավիճակ"), TEXT("status") }, { TEXT("ամփոփ"), TEXT("summary") },
			{ TEXT("քանակ"), TEXT("count") }, { TEXT("հաշվ"), TEXT("count") }, { TEXT("թիվ"), TEXT("number") },

			// Questions (also mark the prompt as one, see QuestionWords).
			{ TEXT("քանի"), TEXT("how many") }, { TEXT("ինչ"), TEXT("what") }, { TEXT("որքան"), TEXT("what") }, { TEXT("որն"), TEXT("what") },
			{ TEXT("ցույց"), TEXT("show") }, { TEXT("ասա"), TEXT("tell") }, { TEXT("ասեք"), TEXT("tell") },

			// Equipment.
			{ TEXT("սարքավորում"), TEXT("equipment") }, { TEXT("սարք"), TEXT("fixture") },
			{ TEXT("լույս"), TEXT("light") }, { TEXT("լուսատու"), TEXT("light") }, { TEXT("լուսարձակ"), TEXT("light") }, { TEXT("պրոժեկտոր"), TEXT("light") },
			{ TEXT("լուսային"), TEXT("lighting") }, { TEXT("լուսավոր"), TEXT("lighting") }, { TEXT("լամպ"), TEXT("lamp") },
			{ TEXT("շարժական"), TEXT("moving") }, { TEXT("շարժվող"), TEXT("moving") }, { TEXT("մուվինգ"), TEXT("moving") }, { TEXT("մուվեր"), TEXT("mover") },
			{ TEXT("գլուխ"), TEXT("head") }, { TEXT("գլխ"), TEXT("head") }, { TEXT("հեդ"), TEXT("head") },
			{ TEXT("սփոթ"), TEXT("spot") }, { TEXT("սպոտ"), TEXT("spot") }, { TEXT("վոշ"), TEXT("wash") }, { TEXT("վաշ"), TEXT("wash") }, { TEXT("լվաց"), TEXT("wash") },
			{ TEXT("փար"), TEXT("par") }, { TEXT("բիմ"), TEXT("beam") }, { TEXT("լազեր"), TEXT("laser") }, { TEXT("ստրոբ"), TEXT("strobe") }, { TEXT("լեդ"), TEXT("led") },
			{ TEXT("ֆերմա"), TEXT("truss") }, { TEXT("տրաս"), TEXT("truss") }, { TEXT("տրուս"), TEXT("truss") },
			{ TEXT("հարթակ"), TEXT("deck") }, { TEXT("բեմահարթակ"), TEXT("deck") }, { TEXT("պոդիում"), TEXT("deck") }, { TEXT("ռայզեր"), TEXT("riser") },
			{ TEXT("դեկ"), TEXT("deck"), Exact }, { TEXT("դեկեր"), TEXT("deck"), Exact },
			{ TEXT("բարձրախոս"), TEXT("speaker") }, { TEXT("կոլոնկա"), TEXT("speaker") }, { TEXT("դինամիկ"), TEXT("speaker") },
			{ TEXT("իր"), TEXT("item"), Exact }, { TEXT("իրեր"), TEXT("item") }, { TEXT("առարկա"), TEXT("item") }, { TEXT("օբյեկտ"), TEXT("item") },
			{ TEXT("բեմ"), TEXT("stage") },

			// Weather, sky and time of day.
			{ TEXT("անձրև"), TEXT("rain") }, { TEXT("տեղատարափ"), TEXT("rain") }, { TEXT("փոթորիկ"), TEXT("storm") },
			{ TEXT("ձյուն"), TEXT("snow") }, { TEXT("ձյան"), TEXT("snow") }, { TEXT("ձնաբուք"), TEXT("snow") },
			{ TEXT("մառախուղ"), TEXT("fog") }, { TEXT("մշուշ"), TEXT("mist") }, { TEXT("ամպ"), TEXT("cloudy") },
			{ TEXT("արևոտ"), TEXT("sunny") }, { TEXT("արևային"), TEXT("sunny") }, { TEXT("պարզ"), TEXT("clear") },
			{ TEXT("եղանակ"), TEXT("weather") }, { TEXT("երկինք"), TEXT("sky") }, { TEXT("երկնք"), TEXT("sky") }, { TEXT("միջավայր"), TEXT("environment") },
			{ TEXT("բացօթյա"), TEXT("outdoors") }, { TEXT("դրսում"), TEXT("outside") }, { TEXT("դուրս"), TEXT("outside") },
			{ TEXT("փակ"), TEXT("indoor"), Exact }, { TEXT("ներս"), TEXT("indoors") }, { TEXT("սրահ"), TEXT("indoors") },
			{ TEXT("միացր"), TEXT("on") }, { TEXT("միացն"), TEXT("on") }, { TEXT("անջատ"), TEXT("off") },
			{ TEXT("կանգնեցր"), TEXT("stop") }, { TEXT("դադար"), TEXT("stop") }, { TEXT("առանց"), TEXT("without") },
			{ TEXT("ժամացույց"), TEXT("clock") }, { TEXT("ժամ"), TEXT("time at") },
			{ TEXT("արևածագ"), TEXT("sunrise") }, { TEXT("լուսաբաց"), TEXT("dawn") }, { TEXT("առավոտ"), TEXT("morning") },
			{ TEXT("կեսօր"), TEXT("noon") }, { TEXT("կեսօրից"), TEXT("afternoon") }, { TEXT("ցերեկ"), TEXT("afternoon") },
			{ TEXT("երեկո"), TEXT("evening") }, { TEXT("մայրամուտ"), TEXT("sunset") }, { TEXT("արևամուտ"), TEXT("sunset") }, { TEXT("մթնշաղ"), TEXT("dusk") },
			{ TEXT("գիշեր"), TEXT("night") }, { TEXT("կեսգիշեր"), TEXT("midnight") },

			// Targeted edits of placed items (Docs/ADR/0018 §6).
			{ TEXT("ջնջ"), TEXT("delete") }, { TEXT("հեռացր"), TEXT("remove") }, { TEXT("հեռացն"), TEXT("remove") }, { TEXT("մաքր"), TEXT("clear") },
			{ TEXT("տեղափոխ"), TEXT("move") }, { TEXT("տեղաշարժ"), TEXT("move") }, { TEXT("շարժիր"), TEXT("move"), Exact }, { TEXT("շարժեք"), TEXT("move"), Exact },
			{ TEXT("բարձրացր"), TEXT("raise") }, { TEXT("բարձրացն"), TEXT("raise") }, { TEXT("իջեցր"), TEXT("lower") }, { TEXT("իջեցն"), TEXT("lower") },
			{ TEXT("փոխարին"), TEXT("replace") }, { TEXT("փոխիր"), TEXT("change"), Exact }, { TEXT("փոխեք"), TEXT("change"), Exact },
			{ TEXT("ընտրված"), TEXT("selected") }, { TEXT("ընտրած"), TEXT("selected") }, { TEXT("նշված"), TEXT("selected") },
			{ TEXT("ձախ"), TEXT("left") }, { TEXT("աջ"), TEXT("right") }, { TEXT("առաջ"), TEXT("forward"), Exact },
			{ TEXT("վեր"), TEXT("up"), Exact }, { TEXT("վերև"), TEXT("up") }, { TEXT("ներքև"), TEXT("down") }, { TEXT("ցած"), TEXT("down"), Exact },
			{ TEXT("դեպի"), TEXT(""), Exact }, { TEXT("ուղղությ"), TEXT("") }, { TEXT("չափով"), TEXT(""), Exact },

			// Fixture controls (Docs/ADR/0020 §7).
			{ TEXT("կարմիր"), TEXT("red") }, { TEXT("կանաչ"), TEXT("green") }, { TEXT("կապույտ"), TEXT("blue") }, { TEXT("երկնագույն"), TEXT("light blue") },
			{ TEXT("սպիտակ"), TEXT("white") }, { TEXT("դեղին"), TEXT("yellow") }, { TEXT("նարնջագույն"), TEXT("orange") }, { TEXT("նարնջ"), TEXT("orange") },
			{ TEXT("մանուշակագույն"), TEXT("purple") }, { TEXT("մանուշակ"), TEXT("violet") }, { TEXT("վարդագույն"), TEXT("pink") }, { TEXT("վարդ"), TEXT("pink") },
			{ TEXT("ամբեր"), TEXT("amber") }, { TEXT("սաթագույն"), TEXT("amber") }, { TEXT("փիրուզագույն"), TEXT("turquoise") }, { TEXT("ոսկեգույն"), TEXT("gold") },
			{ TEXT("մագենտա"), TEXT("magenta") }, { TEXT("ցիան"), TEXT("cyan") }, { TEXT("տաք"), TEXT("warm"), Exact }, { TEXT("սառը"), TEXT("cool"), Exact },
			{ TEXT("գույն"), TEXT("color") },
			{ TEXT("պայծառությ"), TEXT("brightness") }, { TEXT("ինտենսիվությ"), TEXT("intensity") }, { TEXT("դիմեր"), TEXT("dimmer") },
			{ TEXT("զում"), TEXT("zoom") }, { TEXT("խոշորաց"), TEXT("zoom") }, { TEXT("նեղ"), TEXT("narrow") }, { TEXT("ֆրոստ"), TEXT("frost") },
			{ TEXT("ցրիչ"), TEXT("frost") }, { TEXT("իրիս"), TEXT("iris") }, { TEXT("պրիզմ"), TEXT("prism") }, { TEXT("գոբո"), TEXT("gobo") },
			{ TEXT("աստղ"), TEXT("star") }, { TEXT("ժել"), TEXT("gel") }, { TEXT("գել"), TEXT("gel"), Exact }, { TEXT("գելը"), TEXT("gel"), Exact },
			{ TEXT("լի"), TEXT("lee"), Exact }, { TEXT("ռոսկո"), TEXT("rosco") }, { TEXT("պան"), TEXT("pan"), Exact }, { TEXT("թիլթ"), TEXT("tilt") },
			{ TEXT("տիլտ"), TEXT("tilt") }, { TEXT("պտտ"), TEXT("spin") }, { TEXT("պտույտ"), TEXT("rpm") }, { TEXT("տոկոս"), TEXT("percent") },
			{ TEXT("լրիվ"), TEXT("full") }, { TEXT("կես"), TEXT("half"), Exact }, { TEXT("կեսը"), TEXT("half"), Exact }, { TEXT("քառորդ"), TEXT("quarter") },
			{ TEXT("հերց"), TEXT("hz") }, { TEXT("սահման"), TEXT("set") }, { TEXT("փակիր"), TEXT("close"), Exact }, { TEXT("փակեք"), TEXT("close"), Exact },
			{ TEXT("ստրոբոսկոպ"), TEXT("strobe") }, { TEXT("թրթռ"), TEXT("strobe") }, { TEXT("պիքսել"), TEXT("pixel") }, { TEXT("բար"), TEXT("bar") }, { TEXT("բլայնդեր"), TEXT("blinder") },
			{ TEXT("ֆրենել"), TEXT("fresnel") }, { TEXT("պրոֆիլ"), TEXT("profile") }, { TEXT("հիբրիդ"), TEXT("hybrid") },

			// Stage shapes and their sizes.
			{ TEXT("կառուց"), TEXT("build") }, { TEXT("պատրաստ"), TEXT("make") },
			{ TEXT("կլոր"), TEXT("round") }, { TEXT("շրջանաձև"), TEXT("circular") }, { TEXT("շրջան"), TEXT("circle") }, { TEXT("օղակ"), TEXT("ring") },
			{ TEXT("կիսաշրջան"), TEXT("semicircle") }, { TEXT("աղեղ"), TEXT("arc") }, { TEXT("կոր"), TEXT("curved"), Exact }, { TEXT("կորաձև"), TEXT("curved") },
			{ TEXT("կորացած"), TEXT("curved") },
			// աստիճան is both a step and a degree; after a number in a curved request it is read as degrees (TranslateToGrammar).
			{ TEXT("աստիճան"), TEXT("stairs") }, { TEXT("սանդուղք"), TEXT("stairs") }, { TEXT("սանդուխ"), TEXT("stairs") },
			{ TEXT("բազրիք"), TEXT("handrails") }, { TEXT("ճաղավանդակ"), TEXT("handrails") },
			{ TEXT("շառավիղ"), TEXT("radius") }, { TEXT("շառավղ"), TEXT("radius") }, { TEXT("տրամագ"), TEXT("diameter") },
			{ TEXT("լայնությ"), TEXT("wide") }, { TEXT("լայն"), TEXT("wide") }, { TEXT("բարձրությ"), TEXT("high") }, { TEXT("բարձր"), TEXT("high") },
			{ TEXT("մետր"), TEXT("m") }, { TEXT("մ"), TEXT("m"), Exact }, { TEXT("սանտիմետր"), TEXT("cm") }, { TEXT("սմ"), TEXT("cm"), Exact },
			{ TEXT("միլիմետր"), TEXT("mm") }, { TEXT("մմ"), TEXT("mm"), Exact },

			// Operations the assistant explains instead of running.
			{ TEXT("բացիր"), TEXT("open") }, { TEXT("բացեք"), TEXT("open") }, { TEXT("բացել"), TEXT("open") }, { TEXT("բեռն"), TEXT("load") },
			{ TEXT("նոր"), TEXT("new") }, { TEXT("տեսարան"), TEXT("scene") }, { TEXT("սցենա"), TEXT("scene") },

			// Help.
			{ TEXT("օգն"), TEXT("help") }, { TEXT("բարև"), TEXT("hello") }, { TEXT("ողջույն"), TEXT("hello") }, { TEXT("հրաման"), TEXT("commands") },
			{ TEXT("կարող"), TEXT("can") }, { TEXT("ես"), TEXT("you"), Exact }, { TEXT("անել"), TEXT("do"), Exact },

			// Numbers and amounts.
			{ TEXT("զրո"), TEXT("0") }, { TEXT("մեկ"), TEXT("1"), Exact }, { TEXT("մեկը"), TEXT("1"), Exact }, { TEXT("մի"), TEXT("a"), Exact },
			{ TEXT("երկու"), TEXT("2") }, { TEXT("երեք"), TEXT("3") }, { TEXT("չորս"), TEXT("4") }, { TEXT("հինգ"), TEXT("5") }, { TEXT("վեց"), TEXT("6") },
			{ TEXT("յոթ"), TEXT("7") }, { TEXT("ութ"), TEXT("8") }, { TEXT("ինը"), TEXT("9"), Exact }, { TEXT("տասը"), TEXT("10") }, { TEXT("տաս"), TEXT("10"), Exact },
			{ TEXT("տասնմեկ"), TEXT("11") }, { TEXT("տասներկու"), TEXT("12") }, { TEXT("տասներեք"), TEXT("13") }, { TEXT("տասնչորս"), TEXT("14") },
			{ TEXT("տասնհինգ"), TEXT("15") }, { TEXT("տասնվեց"), TEXT("16") }, { TEXT("տասնյոթ"), TEXT("17") }, { TEXT("տասնութ"), TEXT("18") },
			{ TEXT("տասնինը"), TEXT("19") }, { TEXT("քսան"), TEXT("20") }, { TEXT("քսանմեկ"), TEXT("21") }, { TEXT("քսաներկու"), TEXT("22") },
			{ TEXT("քսաներեք"), TEXT("23") }, { TEXT("քսանչորս"), TEXT("24") },
			{ TEXT("առաջին"), TEXT("1") }, { TEXT("երկրորդ"), TEXT("2") }, { TEXT("երրորդ"), TEXT("3") }, { TEXT("չորրորդ"), TEXT("4") },
			{ TEXT("հինգերորդ"), TEXT("5") }, { TEXT("վեցերորդ"), TEXT("6") }, { TEXT("յոթերորդ"), TEXT("7") }, { TEXT("ութերորդ"), TEXT("8") },
			{ TEXT("իններորդ"), TEXT("9") }, { TEXT("տասներորդ"), TEXT("10") },
			{ TEXT("զույգ"), TEXT("pair") }, { TEXT("դյուժին"), TEXT("dozen") },

			// Fillers, and the endings written after a number ("5-րդ", "6-ին", "3-ով").
			{ TEXT("կա"), TEXT(""), Exact }, { TEXT("կան"), TEXT(""), Exact }, { TEXT("է"), TEXT(""), Exact }, { TEXT("են"), TEXT(""), Exact },
			{ TEXT("էր"), TEXT(""), Exact }, { TEXT("էին"), TEXT(""), Exact }, { TEXT("ու"), TEXT(""), Exact }, { TEXT("և"), TEXT(""), Exact },
			{ TEXT("էլ"), TEXT(""), Exact }, { TEXT("նաև"), TEXT(""), Exact }, { TEXT("մեր"), TEXT(""), Exact }, { TEXT("իմ"), TEXT(""), Exact },
			{ TEXT("ինձ"), TEXT(""), Exact }, { TEXT("մեզ"), TEXT(""), Exact }, { TEXT("համար"), TEXT(""), Exact }, { TEXT("խնդր"), TEXT("") },
			{ TEXT("եմ"), TEXT(""), Exact }, { TEXT("ենք"), TEXT(""), Exact }, { TEXT("եք"), TEXT(""), Exact }, { TEXT("ուզում"), TEXT(""), Exact },
			{ TEXT("կուզեի"), TEXT(""), Exact }, { TEXT("թող"), TEXT(""), Exact }, { TEXT("գա"), TEXT(""), Exact }, { TEXT("գան"), TEXT(""), Exact },
			{ TEXT("լինի"), TEXT(""), Exact }, { TEXT("լինեն"), TEXT(""), Exact }, { TEXT("դառնա"), TEXT(""), Exact }, { TEXT("դարձր"), TEXT("make") },
			{ TEXT("այն"), TEXT(""), Exact }, { TEXT("այս"), TEXT(""), Exact }, { TEXT("այդ"), TEXT(""), Exact }, { TEXT("հիմա"), TEXT(""), Exact },
			{ TEXT("արդեն"), TEXT(""), Exact }, { TEXT("մոտ"), TEXT(""), Exact }, { TEXT("վրա"), TEXT(""), Exact }, { TEXT("մեջ"), TEXT(""), Exact },
			{ TEXT("շարք"), TEXT("") }, { TEXT("հատ"), TEXT("") }, { TEXT("ևս"), TEXT(""), Exact }, { TEXT("էլի"), TEXT(""), Exact },
			{ TEXT("տուր"), TEXT(""), Exact }, { TEXT("տվեք"), TEXT(""), Exact }, { TEXT("ինչպես"), TEXT(""), Exact }, { TEXT("արդյոք"), TEXT(""), Exact },
			{ TEXT("որ"), TEXT(""), Exact }, { TEXT("հետո"), TEXT(""), Exact }, { TEXT("դա"), TEXT(""), Exact },
			{ TEXT("ունե"), TEXT("") }, { TEXT("ունի"), TEXT(""), Exact },
			{ TEXT("րդ"), TEXT(""), Exact }, { TEXT("ին"), TEXT(""), Exact }, { TEXT("ից"), TEXT(""), Exact }, { TEXT("ով"), TEXT(""), Exact },
			{ TEXT("ում"), TEXT(""), Exact }, { TEXT("ը"), TEXT(""), Exact }, { TEXT("ի"), TEXT(""), Exact }, { TEXT("ն"), TEXT(""), Exact },
			{ TEXT("երորդ"), TEXT(""), Exact },
		};

		/** Armenian words that make a prompt a question even without "՞". */
		const TCHAR* const QuestionWords[] = { TEXT("քանի"), TEXT("ինչ"), TEXT("որքան"), TEXT("որն"), TEXT("արդյոք"), TEXT("ինչքան"), TEXT("ինչպիսի") };

		// Armenian punctuation.
		constexpr TCHAR QuestionMark = 0x055E;		// ՞, written over the stressed vowel
		constexpr TCHAR EmphasisMark = 0x055B;		// ՛
		constexpr TCHAR ExclamationMark = 0x055C;	// ՜
		constexpr TCHAR OpenGuillemet = 0x00AB;		// «
		constexpr TCHAR CloseGuillemet = 0x00BB;	// »

		struct FToken
		{
			FString Text;
			/** Quoted text, kept as typed. */
			bool bQuoted = false;
			bool bArmenian = false;
		};

		bool IsDigitText(const FString& Text)
		{
			return !Text.IsEmpty() && FChar::IsDigit(Text[0]);
		}

		/** Words, numbers ("18:30", "2.5") and quoted spans; Armenian lowercased; Armenian marks noted and removed. */
		TArray<FToken> Tokenize(const FString& Prompt, bool& bOutQuestion)
		{
			TArray<FToken> Tokens;
			FToken Word;
			const auto Flush = [&Tokens, &Word]()
			{
				if (!Word.Text.IsEmpty())
				{
					Tokens.Add(MoveTemp(Word));
				}
				Word = FToken();
			};
			for (int32 Index = 0; Index < Prompt.Len(); ++Index)
			{
				const TCHAR Char = Prompt[Index];
				if (Char == QuestionMark || Char == TEXT('?'))
				{
					bOutQuestion = true;
					continue;
				}
				if (Char == EmphasisMark || Char == ExclamationMark || Char == TEXT('\''))
				{
					continue;
				}
				const TCHAR Close = Char == OpenGuillemet ? CloseGuillemet : Char == TEXT('"') ? TEXT('"') : Char == TEXT('“') ? TEXT('”') : TEXT('\0');
				if (Close != TEXT('\0'))
				{
					const int32 End = Prompt.Find(FString::Chr(Close), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
					if (End != INDEX_NONE)
					{
						Flush();
						FToken Quoted;
						Quoted.Text = Prompt.Mid(Index + 1, End - Index - 1).TrimStartAndEnd();
						Quoted.bQuoted = true;
						if (!Quoted.Text.IsEmpty())
						{
							Tokens.Add(MoveTemp(Quoted));
						}
						Index = End;
						continue;
					}
				}

				const bool bArmenian = StageAILanguage::IsArmenianLetter(Char);
				const bool bDecimal = (Char == TEXT('.') || Char == TEXT(':')) && IsDigitText(Word.Text) && Index + 1 < Prompt.Len() && FChar::IsDigit(Prompt[Index + 1]);
				if (bArmenian || FChar::IsAlnum(Char) || bDecimal)
				{
					// "5րդ": a number and the ending written after it are separate words.
					if (!Word.Text.IsEmpty() && bArmenian != Word.bArmenian && !bDecimal)
					{
						Flush();
					}
					Word.bArmenian = bArmenian;
					Word.Text.AppendChar(bArmenian ? StageAILanguage::ToLowerArmenian(Char) : Char);
				}
				else
				{
					Flush();
				}
			}
			Flush();
			return Tokens;
		}

		const FEntry* FindEntry(const FString& Word)
		{
			const FEntry* Best = nullptr;
			int32 BestLength = 0;
			for (const FEntry& Entry : Lexicon)
			{
				const int32 Length = FCString::Strlen(Entry.Armenian);
				const bool bMatch = Entry.bExact ? Word == Entry.Armenian : Word.StartsWith(Entry.Armenian, ESearchCase::CaseSensitive);
				// An exact match always wins over a stem.
				const int32 Score = Entry.bExact ? Length + 1000 : Length;
				if (bMatch && Score > BestLength)
				{
					Best = &Entry;
					BestLength = Score;
				}
			}
			return Best;
		}

		bool IsPlainInteger(const FString& Word)
		{
			return IsDigitText(Word) && Word.IsNumeric() && !Word.Contains(TEXT(".")) && !Word.Contains(TEXT(":"));
		}

		/** Grammar words of requests that change something (and so are not a time of day on their own). */
		bool IsActionWord(const FString& Word)
		{
			static const TCHAR* const Actions[] = { TEXT("add"), TEXT("place"), TEXT("put"), TEXT("hang"), TEXT("create"), TEXT("store"), TEXT("record"),
				TEXT("save"), TEXT("export"), TEXT("go"), TEXT("play"), TEXT("run"), TEXT("undo"), TEXT("redo"), TEXT("delete"), TEXT("remove"),
				TEXT("clear"), TEXT("open"), TEXT("load"), TEXT("on"), TEXT("off"), TEXT("stop"), TEXT("move"), TEXT("raise"), TEXT("lower"),
				TEXT("replace"), TEXT("change"), TEXT("build"), TEXT("make") };
			return Algo::FindByPredicate(Actions, [&Word](const TCHAR* Action) { return Word == Action; }) != nullptr;
		}

		/**
		 * "8 evening" -> "8 pm": a part of the day next to a bare hour of 1 to 12 sets am or pm, and is dropped. Not in a request to
		 * change something else ("add 4 spots in the evening" keeps its count).
		 */
		void ApplyPartOfDay(TArray<FString>& Words)
		{
			const auto IsPartOfDay = [](const FString& Word)
			{
				return Word == TEXT("morning") || Word == TEXT("afternoon") || Word == TEXT("evening") || Word == TEXT("night");
			};
			if (!Words.Contains(TEXT("time")) && Words.ContainsByPredicate(IsActionWord))
			{
				// The grammar would read the word alone as a time change and run that instead of the request.
				Words.RemoveAll(IsPartOfDay);
				return;
			}
			const int32 PartIndex = Words.IndexOfByPredicate(IsPartOfDay);
			if (PartIndex == INDEX_NONE)
			{
				return;
			}
			for (int32 Index = 0; Index < Words.Num(); ++Index)
			{
				const int32 Hour = IsPlainInteger(Words[Index]) ? FCString::Atoi(*Words[Index]) : 0;
				if (Hour < 1 || Hour > 12 || FMath::Abs(Index - PartIndex) > 2)
				{
					continue;
				}
				const FString& Part = Words[PartIndex];
				const bool bMorning = Part == TEXT("morning") || (Part == TEXT("night") && (Hour < 6 || Hour == 12));
				Words.Insert(bMorning ? TEXT("am") : TEXT("pm"), Index + 1);
				Words.RemoveAt(PartIndex < Index + 1 ? PartIndex : PartIndex + 1);
				return;
			}
		}
	}

	FString TranslateToGrammar(const FString& Prompt)
	{
		if (StageAILanguage::Detect(Prompt) != EStageAILanguage::Armenian)
		{
			return Prompt;
		}

		bool bQuestion = false;
		const TArray<FToken> Tokens = Tokenize(Prompt, bQuestion);
		TArray<FString> Words;
		// Where the first equipment word in the instrumental case (-ով, "with ...") went: the replacement of a replace request.
		int32 InstrumentalAt = INDEX_NONE;
		const auto NoteInstrumental = [&InstrumentalAt, &Words](const FToken& Token, const FString& Grammar)
		{
			static const TCHAR* const NotEquipment[] = { TEXT("called"), TEXT("seconds"), TEXT("radius"), TEXT("diameter"), TEXT("wide"), TEXT("high") };
			if (InstrumentalAt == INDEX_NONE && Token.Text.Len() > 2 && Token.Text.EndsWith(TEXT("ով"), ESearchCase::CaseSensitive)
				&& !Algo::FindByPredicate(NotEquipment, [&Grammar](const TCHAR* Word) { return Grammar == Word; }))
			{
				InstrumentalAt = Words.Num();
			}
		};
		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			const FToken& Token = Tokens[Index];
			if (Token.bQuoted)
			{
				Words.Add(FString::Printf(TEXT("\"%s\""), *Token.Text));
				continue;
			}
			if (!Token.bArmenian)
			{
				Words.Add(Token.Text);
				continue;
			}
			if (Index + 1 < Tokens.Num() && Tokens[Index + 1].bArmenian)
			{
				const FString Pair = Token.Text + TEXT(" ") + Tokens[Index + 1].Text;
				if (const FEntry* Phrase = Algo::FindByPredicate(Phrases, [&Pair](const FEntry& Entry) { return Pair.StartsWith(Entry.Armenian, ESearchCase::CaseSensitive); }))
				{
					Words.Add(Phrase->Grammar);
					++Index;
					continue;
				}
			}
			// "մի" before a verb is a prohibition ("մի՛ ավելացրու" = don't add), not "a": nothing is run for it.
			if (Token.Text == TEXT("մի") && Index + 1 < Tokens.Num() && Tokens[Index + 1].bArmenian)
			{
				const FEntry* Next = FindEntry(Tokens[Index + 1].Text);
				if (Next && IsActionWord(Next->Grammar))
				{
					return FString();
				}
			}
			if (Algo::FindByPredicate(QuestionWords, [&Token](const TCHAR* Question) { return Token.Text.StartsWith(Question, ESearchCase::CaseSensitive); }))
			{
				bQuestion = true;
			}
			if (const FEntry* Entry = FindEntry(Token.Text))
			{
				if (*Entry->Grammar)
				{
					NoteInstrumental(Token, Entry->Grammar);
					Words.Add(Entry->Grammar);
				}
			}
			else
			{
				// Unknown words are kept: a fixture the grammar cannot name must not match the first one in the catalog.
				NoteInstrumental(Token, Token.Text);
				Words.Add(Token.Text);
			}
		}

		// "փոխարինիր փարերը վոշերով" (replace the pars with washes): the grammar finds the replacement after "with".
		if (InstrumentalAt != INDEX_NONE && (Words.Contains(TEXT("replace")) || Words.Contains(TEXT("change"))) && !Words.Contains(TEXT("with")))
		{
			Words.Insert(TEXT("with"), InstrumentalAt);
		}

		// աստիճան after a number is an angle ("180 աստիճան աղեղ") in a curved request; stairs are counted, not measured in degrees.
		const bool bCurved = Words.ContainsByPredicate([](const FString& Word)
		{
			return Word == TEXT("arc") || Word == TEXT("curved") || Word == TEXT("semicircle") || Word == TEXT("circle") || Word == TEXT("circular")
				|| Word == TEXT("ring") || Word == TEXT("truss");
		});
		for (int32 Index = 1; bCurved && Index < Words.Num(); ++Index)
		{
			if (Words[Index] == TEXT("stairs") && IsDigitText(Words[Index - 1]))
			{
				Words[Index] = TEXT("deg");
			}
		}

		// Multi-word replacements ("how many") become separate words for the rules below.
		TArray<FString> Split;
		for (const FString& Word : Words)
		{
			if (Word.StartsWith(TEXT("\"")))
			{
				Split.Add(Word);
			}
			else
			{
				TArray<FString> Parts;
				Word.ParseIntoArrayWS(Parts);
				Split.Append(Parts);
			}
		}
		Words = MoveTemp(Split);

		ApplyPartOfDay(Words);

		// "2 cue" (an ordinal before the noun) -> "cue 2", unless a number already follows.
		for (int32 Index = 0; Index + 1 < Words.Num(); ++Index)
		{
			const bool bCueNext = Words[Index + 1] == TEXT("cue") || Words[Index + 1] == TEXT("cues");
			const bool bNumberAfter = Index + 2 < Words.Num() && IsDigitText(Words[Index + 2]);
			if (IsDigitText(Words[Index]) && bCueNext && !bNumberAfter)
			{
				Words.Swap(Index, Index + 1);
				++Index;
			}
		}

		// "turn on cue 2" plays it.
		if (Words.Contains(TEXT("on")) && Words.Contains(TEXT("cue")) && Words.ContainsByPredicate(IsPlainInteger))
		{
			Words.Add(TEXT("go"));
		}

		// Armenian puts "on" / "off" before the sky ("միացրու երկինքը"); the grammar reads "sky on".
		if (Words.Contains(TEXT("sky")))
		{
			if (Words.Contains(TEXT("on")))
			{
				Words.Append({ TEXT("sky"), TEXT("on") });
			}
			else if (Words.Contains(TEXT("off")))
			{
				Words.Append({ TEXT("sky"), TEXT("off") });
			}
		}

		// The grammar reads undo and redo only as the first word.
		for (const TCHAR* History : { TEXT("undo"), TEXT("redo") })
		{
			const int32 At = Words.IndexOfByKey(History);
			if (At > 0)
			{
				Words.RemoveAt(At);
				Words.Insert(History, 0);
				break;
			}
		}

		FString Result = FString::Join(Words, TEXT(" "));
		if (bQuestion)
		{
			Result += TEXT(" ?");
		}
		return Result;
	}
}
