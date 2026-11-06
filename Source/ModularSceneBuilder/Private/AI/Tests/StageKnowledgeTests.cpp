// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for assistant v2 phases H0 and H1 (Docs/ADR/0030-assistant-knowledge-live-context-and-tools.md §3, §8; STATE.md #72): the SSE
// stream parser, the knowledge index built from the live tables and help articles, the offline EN/HY search, how-to routing, and that
// a how-to question never runs the action it mentions. No network.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Assistant; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "AI/StageKnowledge.h"
#include "AI/StageKnowledgeSearch.h"
#include "AI/StageKnowledgeSubsystem.h"
#include "AI/StageSseParser.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "UI/StageUIAnchors.h"

namespace StageKnowledgeTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	TArray<FStageSseEvent> FeedInPieces(const FString& Text, int32 Split)
	{
		const FTCHARToUTF8 Utf8(*Text);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		FStageSseParser Parser;
		TArray<FStageSseEvent> Events;
		Parser.Feed(Bytes, Split, Events);
		Parser.Feed(Bytes + Split, Utf8.Length() - Split, Events);
		Parser.Finish(Events);
		return Events;
	}

	/** The index tests build from: everything that needs no game instance, articles read from Content/StageCraft/Help. */
	const FStageKnowledgeIndex& GetTestIndex()
	{
		static const FStageKnowledgeIndex Index = StageKnowledge::Build(UStageKnowledgeSubsystem::GatherStaticSources());
		return Index;
	}

	/** Whether one of Expected is among the top three Ids for Question. */
	bool FindsInTopThree(const FString& Question, const TArray<const TCHAR*>& Expected, FString& OutFound)
	{
		const FStageKnowledgeIndex& Index = GetTestIndex();
		const TArray<StageKnowledgeSearch::FHit> Hits = StageKnowledgeSearch::Search(Index, Question, StageAILanguage::Detect(Question), 3);
		TArray<FString> Ids;
		for (const StageKnowledgeSearch::FHit& Hit : Hits)
		{
			Ids.Add(Index.Entries[Hit.Entry].Id.ToString());
		}
		OutFound = FString::Join(Ids, TEXT(", "));
		for (const TCHAR* Id : Expected)
		{
			if (Ids.Contains(Id))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantSseTest, "StageCraft.Assistant.Sse", StageKnowledgeTests::Flags)

// The streamed body splits into the same events wherever the HTTP module cuts it, with every SSE line rule the API uses.
bool FStageAssistantSseTest::RunTest(const FString& Parameters)
{
	using namespace StageKnowledgeTests;
	const FString Stream = TEXT("event: message_start\ndata: {\"type\":\"message_start\"}\n\n")
		TEXT(": keep-alive\n\n")
		TEXT("event: content_block_delta\r\ndata: {\"text\":\"Բլեքաութ — ok\"}\r\n\r\n")
		TEXT("data: line one\rdata:line two\r\r")
		TEXT("event: ping\n\n")
		TEXT("id: 7\nretry: 100\nevent: message_stop\ndata: {}\n\n")
		TEXT("event: tail\ndata: no blank line at the end");
	const int32 Length = FTCHARToUTF8(*Stream).Length();
	bool bAllSplitsAgree = true;
	TArray<FStageSseEvent> Reference;
	for (int32 Split = 0; Split <= Length; ++Split)
	{
		const TArray<FStageSseEvent> Events = FeedInPieces(Stream, Split);
		if (Split == 0)
		{
			Reference = Events;
			continue;
		}
		bool bSame = Events.Num() == Reference.Num();
		for (int32 Index = 0; bSame && Index < Events.Num(); ++Index)
		{
			bSame = Events[Index].Event == Reference[Index].Event && Events[Index].Data == Reference[Index].Data;
		}
		bAllSplitsAgree &= bSame;
	}
	TestTrue(TEXT("Every split point (UTF-8 sequences included) gives the same events"), bAllSplitsAgree);
	if (!TestEqual(TEXT("Five events: the comment and the data-less ping are not events"), Reference.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("Event name"), Reference[0].Event, FString(TEXT("message_start")));
	TestEqual(TEXT("Data"), Reference[0].Data, FString(TEXT("{\"type\":\"message_start\"}")));
	TestEqual(TEXT("CRLF lines, Armenian text intact"), Reference[1].Data, FString(TEXT("{\"text\":\"Բլեքաութ — ok\"}")));
	TestEqual(TEXT("CR lines; data lines joined; no space after the colon kept as is"), Reference[2].Data, FString(TEXT("line one\nline two")));
	TestTrue(TEXT("... unnamed event"), Reference[2].Event.IsEmpty());
	TestEqual(TEXT("id and retry ignored"), Reference[3].Event, FString(TEXT("message_stop")));
	TestEqual(TEXT("An event the server closed without a blank line is reported by Finish"), Reference[4].Event, FString(TEXT("tail")));

	// A line longer than the cap is cut and the stream marked malformed, never grown without bound.
	FStageSseParser Parser;
	TArray<uint8> Long;
	Long.Init('x', FStageSseParser::MaxLineBytes + 10);
	TArray<FStageSseEvent> Events;
	Parser.Feed(Long.GetData(), Long.Num(), Events);
	TestTrue(TEXT("Over-long line marks the stream malformed"), Parser.IsMalformed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantKnowledgeTest, "StageCraft.Assistant.Knowledge", StageKnowledgeTests::Flags)

// The index comes from the code's own tables and the help articles: nothing written twice, nothing missing, nothing dev-only.
bool FStageAssistantKnowledgeTest::RunTest(const FString& Parameters)
{
	using namespace StageKnowledgeTests;

	TArray<FString> ArticleErrors;
	const FStageKnowledgeSources Sources = UStageKnowledgeSubsystem::GatherStaticSources(&ArticleErrors);
	TestEqual(TEXT("Every help article reads"), FString::Join(ArticleErrors, TEXT("; ")), FString());
	TestTrue(TEXT("At least 18 articles in each language"), Sources.Articles.Num() >= 36);

	// Every article exists in English and Armenian, and points only at real panels and anchors.
	TMap<FName, int32> Languages;
	for (const FStageHelpArticle& Article : Sources.Articles)
	{
		Languages.FindOrAdd(Article.Id) |= 1 << static_cast<int32>(Article.Language);
		if (!Article.Control.IsNone())
		{
			TestNotNull(*FString::Printf(TEXT("Article %s points at a real anchor"), *Article.Id.ToString()), StageUIAnchors::Find(Article.Panel, Article.Control));
		}
		TestTrue(*FString::Printf(TEXT("Article %s has keywords"), *Article.Id.ToString()), !Article.Keywords.IsEmpty());
	}
	for (const TPair<FName, int32>& Language : Languages)
	{
		TestEqual(*FString::Printf(TEXT("Article %s in both languages"), *Language.Key.ToString()), Language.Value, 3);
	}

	// Anchor tables: every anchor has a label and a tooltip, and ids are unique in their panel.
	int32 Anchors = 0;
	const TArray<StageUIAnchors::FTable> Tables = StageUIAnchors::GetTables();
	TestEqual(TEXT("Eight anchored panels (the Lighting Desk seven and Problems)"), Tables.Num(), 8);
	for (const StageUIAnchors::FTable& Table : Tables)
	{
		TSet<FName> Ids;
		for (const FStageUIAnchor& Anchor : Table.Anchors)
		{
			const FString Name = FString::Printf(TEXT("%s/%s"), *Table.Panel.ToString(), *Anchor.ControlId.ToString());
			TestFalse(*(Name + TEXT(" label")), Anchor.Label.IsEmpty());
			TestFalse(*(Name + TEXT(" tooltip")), Anchor.Tooltip.IsEmpty());
			TestFalse(*(Name + TEXT(" unique")), Ids.Contains(Anchor.ControlId));
			Ids.Add(Anchor.ControlId);
			TestTrue(*(Name + TEXT(" found by Find")), StageUIAnchors::Find(Table.Panel, Anchor.ControlId) != nullptr);
			++Anchors;
		}
	}
	TestTrue(TEXT("Anchors cover the Lighting Desk panels"), Anchors >= 60);
	TestNull(TEXT("Unknown anchor"), StageUIAnchors::Find(Tables[0].Panel, TEXT("NoSuchControl")));

	// The index: one entry per command, anchor and attribute, plus the articles.
	const FStageKnowledgeIndex& Index = GetTestIndex();
	int32 Commands = 0;
	int32 Controls = 0;
	int32 Attributes = 0;
	for (const FStageKnowledgeEntry& Entry : Index.Entries)
	{
		Commands += Entry.Kind == EStageKnowledgeKind::Command;
		Controls += Entry.Kind == EStageKnowledgeKind::Control;
		Attributes += Entry.Kind == EStageKnowledgeKind::Parameter;
	}
	TestEqual(TEXT("A command entry for every bindable command"), Commands, static_cast<int32>(EStageInputCommand::Count));
	TestEqual(TEXT("A control entry for every anchor"), Controls, Anchors);
	TestEqual(TEXT("Sixteen attribute entries"), Attributes, 16);
	TestEqual(TEXT("Statistics for every entry"), Index.Documents.Num(), Index.Num());
	TestTrue(TEXT("Entry ids find their entry"), Index.Find(TEXT("control.Playback.Blackout")) != nullptr && Index.Find(TEXT("article.tracking"), EStageAILanguage::Armenian)->Language == EStageAILanguage::Armenian);

	// The cached system prompt's digest: names only, the same every time, and no development console command.
	const FString Digest = StageKnowledge::BuildDigest(Index);
	TestTrue(TEXT("Digest names commands and controls"), Digest.Contains(TEXT("Blackout")) && Digest.Contains(TEXT("Playback › BLACKOUT")));
	TestFalse(TEXT("Digest has no console command"), Digest.Contains(TEXT("StageCraft.")));
	TestFalse(TEXT("Digest has no article text"), Digest.Contains(TEXT("A cue list tracks")));
	TestEqual(TEXT("Built twice, the same digest (byte-stable cache prefix)"), StageKnowledge::BuildDigest(StageKnowledge::Build(UStageKnowledgeSubsystem::GatherStaticSources())), Digest);
	for (const FStageKnowledgeEntry& Entry : Index.Entries)
	{
		if (Entry.Body.Contains(TEXT("StageCraft.Music")) || Entry.Body.Contains(TEXT("StageCraft.Bench")) || Entry.Body.Contains(TEXT("StageCraft.Assistant")))
		{
			AddError(FString::Printf(TEXT("%s mentions a development console command"), *Entry.Id.ToString()));
		}
	}

	// Article parsing: hostile and malformed files are refused with a reason, never half-read.
	FStageHelpArticle Article;
	FString Error;
	TestTrue(TEXT("Parse a good article"), StageKnowledge::ParseArticle(TEXT("﻿# Title\nkeywords: a, b\npanel: StageCraft.Panel.Patch\ncontrol: AutoPatch\nunknown: x\n\nBody line.\n"),
		TEXT("good"), EStageAILanguage::English, Article, Error));
	TestTrue(TEXT("... fields"), Article.Title == TEXT("Title") && Article.Keywords == TEXT("a, b") && Article.Panel.IsValid() && Article.Control == TEXT("AutoPatch") && Article.Body == TEXT("Body line."));
	TestFalse(TEXT("No title refused"), StageKnowledge::ParseArticle(TEXT("Body only"), TEXT("bad"), EStageAILanguage::English, Article, Error));
	TestFalse(TEXT("No body refused"), StageKnowledge::ParseArticle(TEXT("# Title\nkeywords: x\n"), TEXT("bad"), EStageAILanguage::English, Article, Error));
	TestTrue(TEXT("Unknown panel left unset"), StageKnowledge::ParseArticle(TEXT("# T\npanel: StageCraft.Panel.Nope\n\nB"), TEXT("p"), EStageAILanguage::English, Article, Error) && !Article.Panel.IsValid());
	TestFalse(TEXT("Too long refused"), StageKnowledge::ParseArticle(TEXT("# T\n\n") + FString::ChrN(StageKnowledge::MaxArticleLength, TEXT('x')), TEXT("long"), EStageAILanguage::English, Article, Error));

	// A command's answer shows the user's own chords, not the defaults.
	FStageKeyBindings Bindings = FStageKeyBindings::MakeDefaults();
	TestTrue(TEXT("Rebind Blackout"), Bindings.Assign(EStageInputCommand::Blackout, EStageKeySlot::Primary, FStageKeyChord(EKeys::F9, true, false, false)) == EStageKeyBindingResult::Success);
	const FStageKnowledgeAnswer Answer = StageKnowledge::Answer(Index, TEXT("blackout keyboard shortcut"), EStageAILanguage::English, Bindings);
	TestTrue(TEXT("Command answer found"), Answer.bFound);
	TestEqual(TEXT("... from the Blackout command"), Answer.SourceTitle, FString(TEXT("Blackout")));
	TestTrue(TEXT("... with the current chord"), Answer.Text.ToString().Contains(TEXT("Ctrl+F9")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantSearchTest, "StageCraft.Assistant.Search", StageKnowledgeTests::Flags)

// English and Armenian questions find the entry that answers them in the top three, nonsense finds nothing, and ranking is stable.
bool FStageAssistantSearchTest::RunTest(const FString& Parameters)
{
	using namespace StageKnowledgeTests;
	struct FCase
	{
		const TCHAR* Question;
		TArray<const TCHAR*> Expected;
	};
	const FCase Cases[] = {
		// English.
		{ TEXT("how do I store a cue"), { TEXT("article.tracking"), TEXT("control.Cues.Store") } },
		{ TEXT("what is tracking"), { TEXT("article.tracking") } },
		{ TEXT("what does cue only mean"), { TEXT("article.tracking"), TEXT("control.Cues.ModeCueOnly") } },
		{ TEXT("how do I block a cue"), { TEXT("article.tracking") } },
		{ TEXT("how do I change the fade time of a cue"), { TEXT("article.cue-timing") } },
		{ TEXT("what is a follow cue"), { TEXT("article.cue-timing") } },
		{ TEXT("what is the s-curve"), { TEXT("article.cue-timing") } },
		{ TEXT("cue delay"), { TEXT("article.cue-timing") } },
		{ TEXT("where is the blackout button"), { TEXT("control.Playback.Blackout"), TEXT("article.masters"), TEXT("command.Blackout") } },
		{ TEXT("how do I black out all lights"), { TEXT("control.Playback.Blackout"), TEXT("article.masters"), TEXT("command.Blackout") } },
		{ TEXT("what does the grand master do"), { TEXT("article.masters"), TEXT("control.Playback.GrandMaster") } },
		{ TEXT("what is htp"), { TEXT("article.playbacks") } },
		{ TEXT("difference between htp and ltp"), { TEXT("article.playbacks") } },
		{ TEXT("how do playback faders work"), { TEXT("article.playbacks"), TEXT("control.Playback.Fader") } },
		{ TEXT("what does flash do on a playback"), { TEXT("control.Playback.Flash"), TEXT("article.playbacks") } },
		{ TEXT("how do I release a playback"), { TEXT("control.Playback.Release"), TEXT("article.playbacks") } },
		{ TEXT("how do I clear the programmer"), { TEXT("control.Groups.ClearProgrammer"), TEXT("article.programmer"), TEXT("command.ClearProgrammer") } },
		{ TEXT("what is highlight"), { TEXT("control.Groups.Highlight"), TEXT("command.Highlight"), TEXT("article.programmer") } },
		{ TEXT("how does fan work"), { TEXT("control.Groups.Fan"), TEXT("article.programmer") } },
		{ TEXT("how do I record a group"), { TEXT("control.Groups.RecordGroup"), TEXT("article.groups") } },
		{ TEXT("how do I make a palette"), { TEXT("article.palettes"), TEXT("control.Palettes.RecordPalette") } },
		{ TEXT("what is record per fixture"), { TEXT("control.Palettes.RecordPerFixture"), TEXT("article.palettes") } },
		{ TEXT("how do I update a colour palette"), { TEXT("article.palettes") } },
		{ TEXT("how do I patch fixtures"), { TEXT("article.patch"), TEXT("control.Patch.AutoPatch") } },
		{ TEXT("what is a dmx address"), { TEXT("article.patch") } },
		{ TEXT("how do I fix address conflicts"), { TEXT("control.Patch.FixConflicts"), TEXT("article.patch") } },
		{ TEXT("import a csv fixture schedule"), { TEXT("control.Patch.ImportCsv"), TEXT("article.patch") } },
		{ TEXT("how do I export the patch sheet"), { TEXT("control.Patch.Export"), TEXT("article.patch") } },
		{ TEXT("how do I receive art-net from a console"), { TEXT("article.dmx-in"), TEXT("control.Patch.DMXIn") } },
		{ TEXT("what is external dmx"), { TEXT("article.dmx-in"), TEXT("control.Cues.External") } },
		{ TEXT("how do I send sacn to real fixtures"), { TEXT("article.dmx-out"), TEXT("control.Patch.DMXOut"), TEXT("control.Patch.SACN") } },
		{ TEXT("what is the sacn priority"), { TEXT("control.Patch.Priority"), TEXT("article.dmx-out") } },
		{ TEXT("unicast art-net to a node"), { TEXT("control.Patch.ArtNet"), TEXT("article.dmx-out"), TEXT("control.Patch.Unicast") } },
		{ TEXT("how do I make a chase"), { TEXT("article.effects"), TEXT("control.Effects.Spread") } },
		{ TEXT("how do I change the effect speed"), { TEXT("control.Effects.Rate"), TEXT("article.effects") } },
		{ TEXT("what does spread do"), { TEXT("control.Effects.Spread") } },
		{ TEXT("how do I stop all effects"), { TEXT("control.Effects.StopAll"), TEXT("article.effects") } },
		{ TEXT("tap tempo"), { TEXT("control.Effects.Tap") } },
		{ TEXT("how do I make the lights follow the music"), { TEXT("article.audio"), TEXT("control.Effects.Audio") } },
		{ TEXT("how do I play an mp3"), { TEXT("article.audio"), TEXT("control.Audio.SourceFile") } },
		{ TEXT("use youtube audio from my browser"), { TEXT("article.audio"), TEXT("control.Audio.SourceSystem") } },
		{ TEXT("how do I use a microphone"), { TEXT("article.audio"), TEXT("control.Audio.SourceInput") } },
		{ TEXT("what is bpm"), { TEXT("article.audio"), TEXT("control.Audio.Tempo") } },
		{ TEXT("what is the flash limit"), { TEXT("article.flash-safety"), TEXT("control.Audio.FlashLimit") } },
		{ TEXT("is strobing dangerous for epilepsy"), { TEXT("article.flash-safety") } },
		{ TEXT("what does kill do"), { TEXT("control.Playback.AudioKill"), TEXT("control.Effects.AudioKill"), TEXT("article.flash-safety") } },
		{ TEXT("how do I fix audio latency"), { TEXT("control.Audio.Latency"), TEXT("article.flash-safety") } },
		{ TEXT("how do I undo"), { TEXT("article.undo"), TEXT("command.Undo") } },
		{ TEXT("how do I redo"), { TEXT("command.Redo"), TEXT("article.undo") } },
		{ TEXT("what is the shortcut for blackout"), { TEXT("command.Blackout") } },
		{ TEXT("keyboard shortcut to save"), { TEXT("command.Save") } },
		{ TEXT("how do I switch to the top view"), { TEXT("command.ViewTop") } },
		{ TEXT("how do I save my show"), { TEXT("article.scenes"), TEXT("command.Save") } },
		{ TEXT("how do I recover an autosave"), { TEXT("article.scenes") } },
		{ TEXT("how do I change the layout"), { TEXT("article.layouts") } },
		{ TEXT("what is the lighting desk layout"), { TEXT("article.layouts") } },
		{ TEXT("why is my fixture dark"), { TEXT("article.why-dark") } },
		{ TEXT("my light is not working"), { TEXT("article.why-dark") } },
		{ TEXT("where do I see the problems in my show"), { TEXT("article.problems"), TEXT("panel.Problems") } },
		{ TEXT("show me the diagnostics list"), { TEXT("article.problems"), TEXT("panel.Problems") } },
		{ TEXT("what is zoom"), { TEXT("parameter.Zoom") } },
		{ TEXT("what is a gobo"), { TEXT("parameter.Gobo") } },
		{ TEXT("what is cmy"), { TEXT("parameter.ColorC") } },
		{ TEXT("what does frost do"), { TEXT("parameter.Frost") } },
		{ TEXT("what is a prism"), { TEXT("parameter.Prism") } },
		{ TEXT("what is colour temperature"), { TEXT("parameter.ColorTemp") } },
		{ TEXT("can I talk to the assistant in armenian"), { TEXT("article.assistant") } },

		// Armenian.
		{ TEXT("ինչպե՞ս պահել կյու"), { TEXT("article.tracking"), TEXT("control.Cues.Store") } },
		{ TEXT("ինչ է թրեքինգը"), { TEXT("article.tracking") } },
		{ TEXT("որտե՞ղ է բլեքաութը"), { TEXT("article.masters"), TEXT("control.Playback.Blackout"), TEXT("command.Blackout") } },
		{ TEXT("ինչ է գրանդ մաստերը"), { TEXT("article.masters"), TEXT("control.Playback.GrandMaster") } },
		{ TEXT("ինչպե՞ս պատչել սարքերը"), { TEXT("article.patch"), TEXT("control.Patch.AutoPatch") } },
		{ TEXT("ինչպե՞ս ուղարկել DMX իրական սարքերին"), { TEXT("article.dmx-out"), TEXT("control.Patch.DMXOut") } },
		{ TEXT("ինչպե՞ս ընդունել art-net կոնսոլից"), { TEXT("article.dmx-in"), TEXT("control.Patch.DMXIn") } },
		{ TEXT("ինչպե՞ս ստեղծել պալիտրա"), { TEXT("article.palettes") } },
		{ TEXT("ինչպե՞ս գրանցել խումբ"), { TEXT("article.groups"), TEXT("control.Groups.RecordGroup") } },
		{ TEXT("ինչպե՞ս մաքրել պրոգրամերը"), { TEXT("article.programmer"), TEXT("control.Groups.ClearProgrammer") } },
		{ TEXT("ինչ է HTP-ն"), { TEXT("article.playbacks") } },
		{ TEXT("ինչպե՞ս փոխել ֆեյդի ժամանակը"), { TEXT("article.cue-timing") } },
		{ TEXT("ինչպե՞ս անել չեյզ"), { TEXT("article.effects") } },
		{ TEXT("ինչպե՞ս լույսերը հետևեն երաժշտությանը"), { TEXT("article.audio") } },
		{ TEXT("ինչ է թարթման սահմանը"), { TEXT("article.flash-safety") } },
		{ TEXT("ինչպե՞ս հետարկել"), { TEXT("article.undo"), TEXT("command.Undo") } },
		{ TEXT("ինչպե՞ս պահել տեսարանը"), { TEXT("article.scenes") } },
		{ TEXT("ինչո՞ւ է սարքը մութ"), { TEXT("article.why-dark") } },
		{ TEXT("որտեղ տեսնել շոուի խնդիրները"), { TEXT("article.problems"), TEXT("panel.Problems") } },
		{ TEXT("ինչպե՞ս փոխել դասավորությունը"), { TEXT("article.layouts") } },
		{ TEXT("ինչ է օգնականը"), { TEXT("article.assistant") } },
		{ TEXT("ինչպե՞ս միացնել միկրոֆոնը"), { TEXT("article.audio"), TEXT("control.Audio.SourceInput") } },
		{ TEXT("ինչ է KILL-ը"), { TEXT("control.Playback.AudioKill"), TEXT("control.Effects.AudioKill"), TEXT("article.flash-safety") } },
		{ TEXT("ինչպե՞ս ուղղել հասցեների կոնֆլիկտը"), { TEXT("article.patch"), TEXT("control.Patch.FixConflicts") } },
		{ TEXT("ինչ է DMX IN-ը"), { TEXT("article.dmx-in"), TEXT("control.Patch.DMXIn") } },
		{ TEXT("ինչպե՞ս կանգնեցնել բոլոր էֆեկտները"), { TEXT("article.effects"), TEXT("control.Effects.StopAll") } },
		{ TEXT("ինչ է պալիտրան"), { TEXT("article.palettes") } },
		{ TEXT("ինչպե՞ս փոխել յունիվերսը և հասցեն"), { TEXT("article.patch") } },
		{ TEXT("որտե՞ղ է ստրոբի սահմանը"), { TEXT("article.flash-safety") } },
	};

	int32 Passed = 0;
	for (const FCase& Case : Cases)
	{
		FString Found;
		if (FindsInTopThree(Case.Question, Case.Expected, Found))
		{
			++Passed;
		}
		else
		{
			AddError(FString::Printf(TEXT("\"%s\" found [%s], expected %s"), Case.Question, *Found, Case.Expected[0]));
		}
	}
	AddInfo(FString::Printf(TEXT("%d of %d questions found their answer in the top three."), Passed, static_cast<int32>(UE_ARRAY_COUNT(Cases))));

	// The armenian answer comes in Armenian when the article has a translation.
	const FStageKnowledgeIndex& Index = GetTestIndex();
	const TArray<StageKnowledgeSearch::FHit> Armenian = StageKnowledgeSearch::Search(Index, TEXT("ինչ է թրեքինգը"), EStageAILanguage::Armenian, 1);
	TestTrue(TEXT("Armenian question, Armenian article"), Armenian.Num() == 1 && Index.Entries[Armenian[0].Entry].Language == EStageAILanguage::Armenian);
	const TArray<StageKnowledgeSearch::FHit> English = StageKnowledgeSearch::Search(Index, TEXT("what is tracking"), EStageAILanguage::English, 1);
	TestTrue(TEXT("English question, English article"), English.Num() == 1 && Index.Entries[English[0].Entry].Language == EStageAILanguage::English);

	// Nonsense is not answered.
	const FStageKeyBindings Bindings = FStageKeyBindings::MakeDefaults();
	for (const TCHAR* Nonsense : { TEXT("purple elephant"), TEXT("asdf qwer"), TEXT("բանան"), TEXT(""), TEXT("???") })
	{
		TestFalse(*FString::Printf(TEXT("No answer for \"%s\""), Nonsense), StageKnowledge::Answer(Index, Nonsense, EStageAILanguage::English, Bindings).bFound);
	}

	// Stable ranking.
	const TArray<StageKnowledgeSearch::FHit> First = StageKnowledgeSearch::Search(Index, TEXT("how do I patch fixtures"), EStageAILanguage::English, 5);
	const TArray<StageKnowledgeSearch::FHit> Second = StageKnowledgeSearch::Search(Index, TEXT("how do I patch fixtures"), EStageAILanguage::English, 5);
	bool bSame = First.Num() == Second.Num();
	for (int32 Position = 0; bSame && Position < First.Num(); ++Position)
	{
		bSame = First[Position].Entry == Second[Position].Entry && First[Position].Score == Second[Position].Score;
	}
	TestTrue(TEXT("The same question ranks the same way"), bSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantRoutingTest, "StageCraft.Assistant.Routing", StageKnowledgeTests::Flags)

// Words become the same stems on both sides, how-to questions are recognised in both languages, and only actions asked "how" are
// kept from running.
bool FStageAssistantRoutingTest::RunTest(const FString& Parameters)
{
	TArray<FString> Terms;
	StageKnowledgeSearch::Tokenize(TEXT("Storing stored STORES store"), Terms);
	TestEqual(TEXT("English stems meet"), FString::Join(Terms, TEXT(" ")), FString(TEXT("stor stor stor stor")));
	Terms.Reset();
	StageKnowledgeSearch::Tokenize(TEXT("How do I patch the fixtures?"), Terms);
	TestEqual(TEXT("Question and filler words dropped"), FString::Join(Terms, TEXT(" ")), FString(TEXT("patch fixtur")));
	Terms.Reset();
	StageKnowledgeSearch::Tokenize(TEXT("Կյուները կյուի DMX-ի"), Terms);
	TestEqual(TEXT("Armenian endings and capitals; a lone ending dropped"), FString::Join(Terms, TEXT(" ")), FString(TEXT("կյու կյու dmx")));
	Terms.Reset();
	StageKnowledgeSearch::Tokenize(TEXT("ինչպե՞ս"), Terms);
	TestEqual(TEXT("Armenian question word with its mark is a stop word"), Terms.Num(), 0);
	TestTrue(TEXT("Armenian query also read through the lexicon"), StageKnowledgeSearch::MakeQueryTerms(TEXT("ինչպե՞ս պահել կյու")).Contains(TEXT("cue")));

	struct FHowTo
	{
		const TCHAR* Prompt;
		bool bHowTo;
	};
	const FHowTo HowTos[] = {
		{ TEXT("how do I store a cue"), true }, { TEXT("How can I patch"), true }, { TEXT("how to make a chase"), true },
		{ TEXT("where is blackout?"), true }, { TEXT("where's the grand master"), true }, { TEXT("what is tracking"), true },
		{ TEXT("what does KILL do"), true }, { TEXT("why is fixture 3 dark"), true }, { TEXT("explain palettes"), true },
		{ TEXT("tell me about effects"), true }, { TEXT("show me where blackout is"), true },
		{ TEXT("store cue 1"), false }, { TEXT("add 4 moving heads"), false }, { TEXT("how many fixtures"), false },
		{ TEXT("show the cue list"), false }, { TEXT("make it rain"), false },
		{ TEXT("ինչպե՞ս պահել կյու"), true }, { TEXT("որտե՞ղ է բլեքաութը"), true }, { TEXT("ինչո՞ւ է սարքը մութ"), true },
		{ TEXT("ինչ է թրեքինգը"), true }, { TEXT("պահիր կյու 1"), false }, { TEXT("ավելացրու 4 լույս"), false },
	};
	for (const FHowTo& HowTo : HowTos)
	{
		TestEqual(*FString::Printf(TEXT("How-to: \"%s\""), HowTo.Prompt), StageKnowledgeSearch::IsHowToQuestion(HowTo.Prompt), HowTo.bHowTo);
	}

	using StageKnowledgeSearch::ERoute;
	const auto RouteOf = [](const TCHAR* Prompt)
	{
		const TOptional<FStageAICommand> Parsed = StageAICommandParser::Parse(Prompt);
		return StageKnowledgeSearch::Route(Prompt, Parsed.GetPtrOrNull());
	};
	TestTrue(TEXT("An action asked how never runs"), RouteOf(TEXT("how do I store cue 5")) != ERoute::Command);
	TestTrue(TEXT("... in Armenian too"), RouteOf(TEXT("ինչպե՞ս պահել կյու 5")) != ERoute::Command);
	TestTrue(TEXT("An action asked for runs"), RouteOf(TEXT("store cue 5")) == ERoute::Command);
	TestTrue(TEXT("A stage question keeps its live answer"), RouteOf(TEXT("how many fixtures")) == ERoute::Command);
	TestTrue(TEXT("Bare help keeps the command list"), RouteOf(TEXT("help")) == ERoute::Command && RouteOf(TEXT("what can you do")) == ERoute::Command);
	TestTrue(TEXT("Help with a topic asks the knowledge"), RouteOf(TEXT("help with patching")) == ERoute::Knowledge);
	TestTrue(TEXT("Not understood asks the knowledge"), RouteOf(TEXT("purple elephant")) == ERoute::Knowledge);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantHowToTest, "StageCraft.Assistant.HowTo", StageKnowledgeTests::Flags)

// Against a real game world: a how-to question never reaches the executor, even with no knowledge to answer it, while the same
// action asked for still runs.
bool FStageAssistantHowToTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageKnowledgeTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->GetWorldSettings()->NotifyBeginPlay();
	ON_SCOPE_EXIT
	{
		World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		GEngine->DestroyWorldContext(World);
	};
	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
	if (!TestNotNull(TEXT("Assistant subsystem"), Assistant))
	{
		return false;
	}
	TArray<FStageAICommand> Executed;
	const auto MakeExecutor = [&Executed]()
	{
		return FStageAICommandExecutor::CreateLambda([&Executed](const FStageAICommand& Command)
		{
			Executed.Add(Command);
			return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, FText::FromString(TEXT("ran")));
		});
	};
	const auto LastLine = [Assistant]() { return Assistant->GetMessages().IsEmpty() ? FStageAIMessage() : Assistant->GetMessages().Last(); };

	// This world has no game instance, so there is no knowledge: the reply says nothing was changed.
	TestTrue(TEXT("How-to accepted"), Assistant->SubmitPrompt(TEXT("how do I store cue 5"), MakeExecutor()));
	TestEqual(TEXT("... the executor is not called"), Executed.Num(), 0);
	TestTrue(TEXT("... the reply says nothing changed"), LastLine().Text.ToString().Contains(TEXT("changed nothing")));
	TestTrue(TEXT("... an information line"), LastLine().Outcome == EStageAIOutcome::Info);
	Assistant->SubmitPrompt(TEXT("ինչպե՞ս պահել կյու 5"), MakeExecutor());
	TestEqual(TEXT("Armenian how-to: executor not called"), Executed.Num(), 0);

	TestTrue(TEXT("The action asked for"), Assistant->SubmitPrompt(TEXT("store cue 5"), MakeExecutor()));
	TestTrue(TEXT("... runs once"), Executed.Num() == 1 && Executed[0].Intent == EStageAIIntent::StoreCue);

	// Stage questions are still answered live.
	Assistant->SubmitPrompt(TEXT("how many fixtures?"), MakeExecutor());
	TestTrue(TEXT("Stage question answered by the grammar"), LastLine().Text.ToString().Contains(TEXT("No lighting fixtures")));
	TestFalse(TEXT("... with no reference"), LastLine().Reference.HasPanel());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
