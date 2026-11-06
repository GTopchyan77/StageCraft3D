// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAIToolCatalogue.h"

#include "AI/StageAICommandParser.h"
#include "Algo/AnyOf.h"
#include "Algo/Find.h"

#define LOCTEXT_NAMESPACE "StageAIToolCatalogue"

namespace StageAIToolCatalogue
{
	namespace
	{
		using StageAILanguage::Pick;
		using EEffect = EStageAIToolEffect;

		constexpr bool Undoable = true;
		constexpr bool NotUndoable = false;
		constexpr bool Plannable = true;
		constexpr bool Alone = false;

		/** In EStageAIIntent order; GetTool indexes it. The wire names are the online model's contract: never rename one. */
		const FStageAITool Tools[] = {
			{ EStageAIIntent::Help, TEXT("help"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::StageSummary, TEXT("stage_summary"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::ItemCount, TEXT("item_count"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::TotalWeight, TEXT("total_weight"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::PowerDraw, TEXT("power_draw"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::FixtureCount, TEXT("fixture_count"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::CountMatching, TEXT("count_matching"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::ListCues, TEXT("list_cues"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::EnvironmentStatus, TEXT("environment_status"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::ExplainFixture, TEXT("explain_fixture"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::ListProblems, TEXT("list_problems"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::AddFixtures, TEXT("add_fixtures"), EEffect::Stage, Undoable, Plannable },
			{ EStageAIIntent::StoreCue, TEXT("store_cue"), EEffect::Cues, Undoable, Plannable },
			{ EStageAIIntent::UpdateCue, TEXT("update_cue"), EEffect::Cues, Undoable, Plannable },
			{ EStageAIIntent::DeleteCue, TEXT("delete_cue"), EEffect::Cues, Undoable, Plannable },
			{ EStageAIIntent::PatchFixtures, TEXT("patch_fixtures"), EEffect::Patch, Undoable, Plannable },
			{ EStageAIIntent::GoToCue, TEXT("go_to_cue"), EEffect::Playback, NotUndoable, Plannable },
			{ EStageAIIntent::CueGo, TEXT("cue_go"), EEffect::Playback, NotUndoable, Plannable },
			{ EStageAIIntent::CueBack, TEXT("cue_back"), EEffect::Playback, NotUndoable, Plannable },
			{ EStageAIIntent::SetWeather, TEXT("set_weather"), EEffect::Environment, Undoable, Plannable },
			{ EStageAIIntent::SetTimeOfDay, TEXT("set_time_of_day"), EEffect::Environment, Undoable, Plannable },
			{ EStageAIIntent::SetSky, TEXT("set_sky"), EEffect::Environment, Undoable, Plannable },
			{ EStageAIIntent::Export, TEXT("export"), EEffect::Files, NotUndoable, Alone },
			{ EStageAIIntent::Undo, TEXT("undo"), EEffect::History, NotUndoable, Alone },
			{ EStageAIIntent::Redo, TEXT("redo"), EEffect::History, NotUndoable, Alone },
			{ EStageAIIntent::DeleteItems, TEXT("delete_items"), EEffect::Stage, Undoable, Plannable },
			{ EStageAIIntent::MoveItems, TEXT("move_items"), EEffect::Stage, Undoable, Plannable },
			{ EStageAIIntent::ReplaceItems, TEXT("replace_items"), EEffect::Stage, Undoable, Plannable },
			{ EStageAIIntent::BuildShape, TEXT("build_shape"), EEffect::Stage, Undoable, Plannable },
			{ EStageAIIntent::SetFixture, TEXT("set_fixture"), EEffect::Fixtures, Undoable, Plannable },
			{ EStageAIIntent::SearchMarketplace, TEXT("search_marketplace"), EEffect::Marketplace, NotUndoable, Alone },
			{ EStageAIIntent::ListingDetails, TEXT("listing_details"), EEffect::None, NotUndoable, Alone },
			{ EStageAIIntent::PreviewListing, TEXT("preview_listing"), EEffect::Marketplace, NotUndoable, Alone },
			{ EStageAIIntent::AcquireListing, TEXT("acquire_listing"), EEffect::Marketplace, NotUndoable, Alone },
			{ EStageAIIntent::Unsupported, TEXT("unsupported"), EEffect::None, NotUndoable, Alone },
		};
		static_assert(UE_ARRAY_COUNT(Tools) == int32(EStageAIIntent::Count), "Every intent needs a catalogue entry.");

		const TCHAR* const NeverExposed[] = {
			TEXT("purchase credits"), TEXT("wallet"), TEXT("profile"), TEXT("entitlement"), TEXT("credential"), TEXT("api key"),
			TEXT("delete file"), TEXT("open scene"), TEXT("save scene"), TEXT("overwrite scene"), TEXT("clear stage"),
			TEXT("console command"), TEXT("dmx output"),
		};

		FText FormatNumber(double Value, int32 MaxDigits = 3)
		{
			FNumberFormattingOptions Format;
			Format.SetMaximumFractionalDigits(MaxDigits);
			Format.SetUseGrouping(false);
			return FText::AsNumber(Value, &Format);
		}

		/** "the selected items", or "\"spot\" items" (at most N). */
		FText DescribeTarget(const FStageAICommand& Command, EStageAILanguage Language, bool bFixtures)
		{
			if (Command.Target == EStageAITarget::Selection)
			{
				return bFixtures ? Pick(Language, LOCTEXT("SelectedFixtures", "the selected fixtures"), INVTEXT("ընտրված սարքերը"))
					: Pick(Language, LOCTEXT("SelectedItems", "the selected items"), INVTEXT("ընտրված իրերը"));
			}
			const FText Query = FText::FromString(Command.ItemQuery);
			return Command.Count.IsSet()
				? FText::Format(Pick(Language, LOCTEXT("NamedCount", "up to {1} \"{0}\""), INVTEXT("մինչև {1} «{0}»")), Query, FText::AsNumber(*Command.Count))
				: FText::Format(Pick(Language, LOCTEXT("NamedAll", "every \"{0}\""), INVTEXT("բոլոր «{0}»-ները")), Query);
		}

		FText DescribeDirection(EStageAIDirection Direction, EStageAILanguage Language)
		{
			switch (Direction)
			{
			case EStageAIDirection::Left:	return Pick(Language, LOCTEXT("Left", "left"), INVTEXT("ձախ"));
			case EStageAIDirection::Right:	return Pick(Language, LOCTEXT("Right", "right"), INVTEXT("աջ"));
			case EStageAIDirection::Forward:return Pick(Language, LOCTEXT("Forward", "forward"), INVTEXT("առաջ"));
			case EStageAIDirection::Back:	return Pick(Language, LOCTEXT("Back", "back"), INVTEXT("հետ"));
			case EStageAIDirection::Up:		return Pick(Language, LOCTEXT("Up", "up"), INVTEXT("վեր"));
			default:						return Pick(Language, LOCTEXT("Down", "down"), INVTEXT("վար"));
			}
		}

		FText DescribeShape(EStageAIShape Shape, EStageAILanguage Language)
		{
			switch (Shape)
			{
			case EStageAIShape::RoundRiser:	return Pick(Language, LOCTEXT("RoundRiser", "a round riser"), INVTEXT("կլոր հարթակ"));
			case EStageAIShape::ArcRiser:	return Pick(Language, LOCTEXT("ArcRiser", "a curved riser"), INVTEXT("կոր հարթակ"));
			case EStageAIShape::Stairs:		return Pick(Language, LOCTEXT("Stairs", "stairs"), INVTEXT("աստիճաններ"));
			default:						return Pick(Language, LOCTEXT("ArcTruss", "a curved truss"), INVTEXT("կոր ֆերմա"));
			}
		}

		/** "dimmer 50 %", "colour red", "zoom 20°". */
		FText DescribeSetting(const FStageAIFixtureSetting& Setting, EStageAILanguage Language)
		{
			struct FControlWords { EStageAIFixtureControl Control; FText English; FText Armenian; };
			const FControlWords Words[] = {
				{ EStageAIFixtureControl::Dimmer, LOCTEXT("Dimmer", "dimmer"), INVTEXT("դիմեր") },
				{ EStageAIFixtureControl::Color, LOCTEXT("Colour", "colour"), INVTEXT("գույն") },
				{ EStageAIFixtureControl::Gel, LOCTEXT("Gel", "gel"), INVTEXT("ժել") },
				{ EStageAIFixtureControl::Zoom, LOCTEXT("Zoom", "zoom"), INVTEXT("զում") },
				{ EStageAIFixtureControl::Frost, LOCTEXT("Frost", "frost"), INVTEXT("ֆրոստ") },
				{ EStageAIFixtureControl::Iris, LOCTEXT("Iris", "iris"), INVTEXT("իրիս") },
				{ EStageAIFixtureControl::CTO, LOCTEXT("CTO", "CTO"), INVTEXT("CTO") },
				{ EStageAIFixtureControl::Prism, LOCTEXT("Prism", "prism"), INVTEXT("պրիզմա") },
				{ EStageAIFixtureControl::PrismSpin, LOCTEXT("PrismSpin", "prism spin"), INVTEXT("պրիզմայի պտույտ") },
				{ EStageAIFixtureControl::Gobo, LOCTEXT("Gobo", "gobo"), INVTEXT("գոբո") },
				{ EStageAIFixtureControl::GoboSpin, LOCTEXT("GoboSpin", "gobo spin"), INVTEXT("գոբոյի պտույտ") },
				{ EStageAIFixtureControl::Strobe, LOCTEXT("Strobe", "strobe"), INVTEXT("ստրոբ") },
				{ EStageAIFixtureControl::Pan, LOCTEXT("Pan", "pan"), INVTEXT("պան") },
				{ EStageAIFixtureControl::Tilt, LOCTEXT("Tilt", "tilt"), INVTEXT("թիլթ") },
			};
			const FControlWords* Found = Algo::FindByPredicate(Words, [&Setting](const FControlWords& Entry) { return Entry.Control == Setting.Control; });
			const FText Name = Found ? Pick(Language, Found->English, Found->Armenian) : FText::GetEmpty();
			if (!Setting.Name.IsEmpty())
			{
				return FText::Format(INVTEXT("{0} {1}"), Name, FText::FromString(Setting.Name));
			}
			if (!Setting.Value.IsSet())
			{
				return Name;
			}
			const double Value = *Setting.Value;
			switch (Setting.Control)
			{
			case EStageAIFixtureControl::Dimmer:
			case EStageAIFixtureControl::Frost:
			case EStageAIFixtureControl::Iris:
			case EStageAIFixtureControl::CTO:
				return FText::Format(INVTEXT("{0} {1} %"), Name, FormatNumber(Value * 100.0, 0));
			case EStageAIFixtureControl::Zoom:
			case EStageAIFixtureControl::Pan:
			case EStageAIFixtureControl::Tilt:
				return FText::Format(INVTEXT("{0} {1}°"), Name, FormatNumber(Value, 1));
			case EStageAIFixtureControl::GoboSpin:
			case EStageAIFixtureControl::PrismSpin:
				return FText::Format(Pick(Language, LOCTEXT("Rpm", "{0} {1} rpm"), INVTEXT("{0} {1} պտ/ր")), Name, FormatNumber(Value, 1));
			case EStageAIFixtureControl::Strobe:
				return Value <= 0.0 ? FText::Format(Pick(Language, LOCTEXT("StrobeOff", "{0} off"), INVTEXT("{0} անջատ")), Name)
					: FText::Format(Pick(Language, LOCTEXT("Hz", "{0} {1} Hz"), INVTEXT("{0} {1} Հց")), Name, FormatNumber(Value, 1));
			default:
				return FText::Format(INVTEXT("{0} {1}"), Name, FormatNumber(Value, 0));
			}
		}

		FText DescribeReport(EStageAIReport Report, EStageAILanguage Language)
		{
			switch (Report)
			{
			case EStageAIReport::BillOfMaterials:	return Pick(Language, LOCTEXT("Bom", "the bill of materials"), INVTEXT("նյութերի ցուցակը"));
			case EStageAIReport::PatchSheet:		return Pick(Language, LOCTEXT("PatchSheet", "the patch sheet"), INVTEXT("պատչի աղյուսակը"));
			case EStageAIReport::CableSchedule:		return Pick(Language, LOCTEXT("Cables", "the cable schedule"), INVTEXT("մալուխների ցուցակը"));
			default:								return Pick(Language, LOCTEXT("AllReports", "every report"), INVTEXT("բոլոր հաշվետվությունները"));
			}
		}

		/** "1, 2, 3", "1-8" for a run of consecutive IDs, or "the selected fixtures". */
		FText DescribeFixtureIds(TConstArrayView<int32> Ids, EStageAILanguage Language)
		{
			if (Ids.IsEmpty())
			{
				return Pick(Language, LOCTEXT("PatchSelected", "the selected fixtures"), INVTEXT("ընտրված սարքերը"));
			}
			bool bRun = Ids.Num() > 2;
			for (int32 Index = 1; bRun && Index < Ids.Num(); ++Index)
			{
				bRun = Ids[Index] == Ids[Index - 1] + 1;
			}
			FString List;
			if (bRun)
			{
				List = FString::Printf(TEXT("%d-%d"), Ids[0], Ids.Last());
			}
			else
			{
				constexpr int32 MaxListed = 8;
				for (int32 Index = 0; Index < Ids.Num() && Index < MaxListed; ++Index)
				{
					List += (Index > 0 ? TEXT(", ") : TEXT("")) + FString::FromInt(Ids[Index]);
				}
				if (Ids.Num() > MaxListed)
				{
					List += TEXT(", ...");
				}
			}
			return FText::Format(Pick(Language, LOCTEXT("FixtureIds", "{0}|plural(one=fixture,other=fixtures) {1}"), INVTEXT("սարքեր {1}")), FText::AsNumber(Ids.Num()),
				FText::FromString(List));
		}
	}

	TConstArrayView<FStageAITool> GetTools()
	{
		return MakeArrayView(Tools);
	}

	const FStageAITool& GetTool(EStageAIIntent Intent)
	{
		check(uint8(Intent) < uint8(EStageAIIntent::Count));
		return Tools[uint8(Intent)];
	}

	const FStageAITool* FindTool(FStringView Name)
	{
		return Algo::FindByPredicate(Tools, [Name](const FStageAITool& Tool) { return Name.Equals(Tool.Name, ESearchCase::CaseSensitive); });
	}

	bool CanApplyWithoutAsking(const FStageAICommand& Command)
	{
		if (uint8(Command.Intent) >= uint8(EStageAIIntent::Count))
		{
			return false;
		}
		const FStageAITool& Tool = GetTool(Command.Intent);
		return Tool.Effect == EStageAIToolEffect::Playback
			|| (Tool.Effect == EStageAIToolEffect::Fixtures && Command.Target == EStageAITarget::Selection);
	}

	bool CheckPlan(TConstArrayView<FStageAICommand> Steps, EStageAILanguage Language, FText& OutError)
	{
		if (Steps.IsEmpty())
		{
			OutError = Pick(Language, LOCTEXT("EmptyPlan", "There was nothing to change."), INVTEXT("Փոխելու բան չկար։"));
			return false;
		}
		if (Steps.Num() > MaxPlanSteps)
		{
			OutError = FText::Format(Pick(Language, LOCTEXT("PlanTooLong", "That is {0} changes; I make at most {1} at a time. Ask for fewer."),
				INVTEXT("Դա {0} փոփոխություն է, իսկ ես միանգամից կատարում եմ առավելագույնը {1}։ Խնդրեք ավելի քիչ։")), FText::AsNumber(Steps.Num()), FText::AsNumber(MaxPlanSteps));
			return false;
		}
		for (const FStageAICommand& Step : Steps)
		{
			if (uint8(Step.Intent) >= uint8(EStageAIIntent::Count) || !GetTool(Step.Intent).IsChange())
			{
				OutError = Pick(Language, LOCTEXT("PlanQuestion", "A plan can only hold changes."), INVTEXT("Պլանը կարող է պարունակել միայն փոփոխություններ։"));
				return false;
			}
			if (Steps.Num() > 1 && !GetTool(Step.Intent).bPlannable)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("PlanAlone", "\"{0}\" has to be asked on its own, not as part of several changes."),
					INVTEXT("«{0}»-ը պետք է խնդրել առանձին, ոչ թե մի քանի փոփոխությունների մաս։")), FText::FromString(GetTool(Step.Intent).Name));
				return false;
			}
		}
		return true;
	}

	bool HasStepsNotUndone(TConstArrayView<FStageAICommand> Steps)
	{
		return Algo::AnyOf(Steps, [](const FStageAICommand& Step)
		{
			return uint8(Step.Intent) < uint8(EStageAIIntent::Count) && GetTool(Step.Intent).IsChange() && !GetTool(Step.Intent).bUndoable;
		});
	}

	FText DescribeStep(const FStageAICommand& Command, EStageAILanguage Language)
	{
		const auto CueText = [](const TOptional<float>& Number) { return Number.IsSet() ? FormatNumber(*Number) : FText::GetEmpty(); };
		switch (Command.Intent)
		{
		case EStageAIIntent::AddFixtures:
			return FText::Format(Pick(Language, LOCTEXT("AddFixtures", "Add {0} {1}"), INVTEXT("Ավելացնել {0} {1}")), FText::AsNumber(Command.Count.Get(1)),
				Command.ItemQuery.IsEmpty() ? Pick(Language, LOCTEXT("AddFixturesAny", "fixtures"), INVTEXT("սարք")) : FText::FromString(Command.ItemQuery));
		case EStageAIIntent::StoreCue:
		{
			FText Text = Command.CueNumber.IsSet()
				? FText::Format(Pick(Language, LOCTEXT("StoreCue", "Store cue {0}"), INVTEXT("Պահել կյու {0}")), CueText(Command.CueNumber))
				: Pick(Language, LOCTEXT("StoreNextCue", "Store the next cue"), INVTEXT("Պահել հաջորդ կյուն"));
			if (!Command.Label.IsEmpty())
			{
				Text = FText::Format(INVTEXT("{0} \"{1}\""), Text, FText::FromString(Command.Label));
			}
			if (Command.FadeSeconds.IsSet())
			{
				Text = FText::Format(Pick(Language, LOCTEXT("WithFade", "{0}, {1} s fade"), INVTEXT("{0}, {1} վ անցում")), Text, FormatNumber(*Command.FadeSeconds, 2));
			}
			return Text;
		}
		case EStageAIIntent::UpdateCue:
		{
			TArray<FText> Parts;
			if (!Command.Label.IsEmpty())
			{
				Parts.Add(FText::Format(Pick(Language, LOCTEXT("UpdateLabel", "label \"{0}\""), INVTEXT("անուն «{0}»")), FText::FromString(Command.Label)));
			}
			if (Command.FadeSeconds.IsSet())
			{
				Parts.Add(FText::Format(Pick(Language, LOCTEXT("UpdateFade", "{0} s fade"), INVTEXT("{0} վ անցում")), FormatNumber(*Command.FadeSeconds, 2)));
			}
			return FText::Format(Pick(Language, LOCTEXT("UpdateCue", "Change cue {0}: {1}"), INVTEXT("Փոխել կյու {0}-ը՝ {1}")), CueText(Command.CueNumber),
				FText::Join(INVTEXT(", "), Parts));
		}
		case EStageAIIntent::DeleteCue:
			return FText::Format(Pick(Language, LOCTEXT("DeleteCue", "Delete cue {0}"), INVTEXT("Ջնջել կյու {0}-ը")), CueText(Command.CueNumber));
		case EStageAIIntent::PatchFixtures:
			return FText::Format(Pick(Language, LOCTEXT("Patch", "Patch {0} from {1}.{2}"), INVTEXT("Պատչել {0}՝ սկսած {1}.{2}-ից")),
				DescribeFixtureIds(Command.FixtureIds, Language), FText::AsNumber(Command.Universe.Get(1)),
				FText::FromString(FString::Printf(TEXT("%03d"), Command.Address.Get(1))));
		case EStageAIIntent::GoToCue:
			return FText::Format(Pick(Language, LOCTEXT("GoToCue", "Go to cue {0}"), INVTEXT("Անցնել կյու {0}-ին")), CueText(Command.CueNumber));
		case EStageAIIntent::CueGo:
			return Pick(Language, LOCTEXT("CueGo", "GO: the next cue"), INVTEXT("GO՝ հաջորդ կյուն"));
		case EStageAIIntent::CueBack:
			return Pick(Language, LOCTEXT("CueBack", "BACK: the previous cue"), INVTEXT("BACK՝ նախորդ կյուն"));
		case EStageAIIntent::SetWeather:
			return FText::Format(Pick(Language, LOCTEXT("Weather", "Weather: {0}"), INVTEXT("Եղանակ՝ {0}")), StageAIAnswers::DescribeWeather(Command.Weather, Language));
		case EStageAIIntent::SetTimeOfDay:
			return FText::Format(Pick(Language, LOCTEXT("Time", "Time of day: {0}"), INVTEXT("Օրվա ժամ՝ {0}")), FText::FromString(StageAIAnswers::FormatHour(Command.Hour)));
		case EStageAIIntent::SetSky:
			return Command.bSkyOn ? Pick(Language, LOCTEXT("SkyOn", "Outdoor sky on"), INVTEXT("Միացնել բաց երկինքը"))
				: Pick(Language, LOCTEXT("SkyOff", "Outdoor sky off (indoor stage)"), INVTEXT("Անջատել բաց երկինքը (փակ բեմ)"));
		case EStageAIIntent::Export:
			return FText::Format(Pick(Language, LOCTEXT("Export", "Export {0}"), INVTEXT("Արտահանել {0}")), DescribeReport(Command.Report, Language));
		case EStageAIIntent::Undo:
			return Pick(Language, LOCTEXT("Undo", "Undo the last step"), INVTEXT("Հետարկել վերջին քայլը"));
		case EStageAIIntent::Redo:
			return Pick(Language, LOCTEXT("Redo", "Redo the last undone step"), INVTEXT("Կրկնել վերջին հետարկված քայլը"));
		case EStageAIIntent::DeleteItems:
			return FText::Format(Pick(Language, LOCTEXT("DeleteItems", "Delete {0}"), INVTEXT("Ջնջել {0}")), DescribeTarget(Command, Language, false));
		case EStageAIIntent::MoveItems:
			return FText::Format(Pick(Language, LOCTEXT("MoveItems", "Move {0} {1} {2} cm"), INVTEXT("Տեղափոխել {0} {1} {2} սմ")), DescribeTarget(Command, Language, false),
				DescribeDirection(Command.Direction, Language), FormatNumber(Command.DistanceCm.Get(100.0), 0));
		case EStageAIIntent::ReplaceItems:
			return FText::Format(Pick(Language, LOCTEXT("ReplaceItems", "Replace {0} with \"{1}\""), INVTEXT("Փոխարինել {0} «{1}»-ով")), DescribeTarget(Command, Language, false),
				FText::FromString(Command.Replacement));
		case EStageAIIntent::BuildShape:
			return FText::Format(Pick(Language, LOCTEXT("BuildShape", "Build {0}"), INVTEXT("Կառուցել {0}")), DescribeShape(Command.Shape, Language));
		case EStageAIIntent::SetFixture:
		{
			TArray<FText> Settings;
			for (const FStageAIFixtureSetting& Setting : Command.FixtureSettings)
			{
				Settings.Add(DescribeSetting(Setting, Language));
			}
			return FText::Format(Pick(Language, LOCTEXT("SetFixture", "Set {0} on {1}"), INVTEXT("Դնել {0}՝ {1}")), FText::Join(INVTEXT(", "), Settings),
				DescribeTarget(Command, Language, true));
		}
		case EStageAIIntent::SearchMarketplace:
			return Pick(Language, LOCTEXT("Search", "Search the marketplace"), INVTEXT("Որոնել շուկայում"));
		case EStageAIIntent::PreviewListing:
			return Pick(Language, LOCTEXT("Preview", "Preview a marketplace listing"), INVTEXT("Նախադիտել շուկայի հայտարարությունը"));
		case EStageAIIntent::AcquireListing:
			return Pick(Language, LOCTEXT("Acquire", "Get a marketplace listing"), INVTEXT("Ստանալ շուկայի հայտարարությունը"));
		default:
			return FText::FromString(GetTool(Command.Intent).Name);
		}
	}

	TConstArrayView<const TCHAR*> GetNeverExposed()
	{
		return MakeArrayView(NeverExposed);
	}
}

#undef LOCTEXT_NAMESPACE
