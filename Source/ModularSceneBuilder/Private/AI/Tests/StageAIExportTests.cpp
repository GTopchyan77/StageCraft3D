// Copyright Epic Games, Inc. All Rights Reserved.

// STATE.md #115 (Docs/ADR/0016 §7): an export the assistant started is reported in the conversation when its files are written.
// The report subsystems' OnExportFinished is broadcast by the test, so no file is written and nothing waits for a worker.
// Run: powershell -File Scripts\Tests\RunAutomation.ps1 -Filter StageCraft.Assistant.ExportCompletion -NullRHI

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAICommandSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "Report/StageCableScheduleSubsystem.h"
#include "Report/StagePatchSheetSubsystem.h"

namespace StageAIExportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world (subsystems initialized) torn down with the scope. */
	struct FExportTestWorld
	{
		UWorld* World = nullptr;

		FExportTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAIExportTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FExportTestWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}
	};

	/** The runner's side of an export: the reports in Started begin (as RequestExport* would), the others are refused. */
	FStageAICommandExecutor MakeExporter(const TArray<EStageAIReport>& Started)
	{
		return FStageAICommandExecutor::CreateLambda([Started](const FStageAICommand& Command)
		{
			if (Command.Intent != EStageAIIntent::Export)
			{
				return FStageAIActionOutcome::Make(EStageAIOutcome::Refused, FText::FromString(TEXT("not an export")));
			}
			FStageAIActionOutcome Outcome = FStageAIActionOutcome::Make(Started.IsEmpty() ? EStageAIOutcome::Refused : EStageAIOutcome::Succeeded,
				FText::FromString(Started.IsEmpty() ? TEXT("Nothing was exported.") : TEXT("Exporting.")));
			for (const EStageAIReport Report : Started)
			{
				if (Command.Report == EStageAIReport::All || Command.Report == Report)
				{
					Outcome.StartedExports.Add(Report);
				}
			}
			return Outcome;
		});
	}

	FStageReportExportResult Written(const TCHAR* Stem)
	{
		FStageReportExportResult Result;
		Result.bSucceeded = true;
		Result.FilePath = FString::Printf(TEXT("C:/Project/Saved/StageCraft/Reports/%s.pdf"), Stem);
		Result.Message = FText::FromString(TEXT("Exported."));
		return Result;
	}

	FStageReportExportResult NotWritten(const TCHAR* Stem)
	{
		FStageReportExportResult Result = Written(Stem);
		Result.bSucceeded = false;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAssistantExportCompletionTest, "StageCraft.Assistant.ExportCompletion", StageAIExportTests::Flags)

bool FStageAssistantExportCompletionTest::RunTest(const FString& Parameters)
{
	using namespace StageAIExportTests;
	FExportTestWorld Scope;
	UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(Scope.World);
	UStageBillOfMaterialsSubsystem* Bom = UWorld::GetSubsystem<UStageBillOfMaterialsSubsystem>(Scope.World);
	UStagePatchSheetSubsystem* Patch = UWorld::GetSubsystem<UStagePatchSheetSubsystem>(Scope.World);
	UStageCableScheduleSubsystem* Cables = UWorld::GetSubsystem<UStageCableScheduleSubsystem>(Scope.World);
	if (!TestNotNull(TEXT("Assistant"), Assistant) || !TestNotNull(TEXT("Bill of materials"), Bom) || !TestNotNull(TEXT("Patch sheet"), Patch)
		|| !TestNotNull(TEXT("Cable schedule"), Cables))
	{
		return false;
	}
	const auto Count = [Assistant]() { return Assistant->GetMessages().Num(); };
	const auto Last = [Assistant]() { return Assistant->GetMessages().Last(); };

	// An export started from a panel or the menu is not the conversation's: the status bar reports it.
	Bom->OnExportFinished.Broadcast(Written(TEXT("BOM_Panel")));
	TestEqual(TEXT("An export nobody asked the assistant for adds no line"), Count(), 0);

	// Asked for: one line when the files are written, naming both of them.
	TestTrue(TEXT("Export the bill of materials"), Assistant->SubmitPrompt(TEXT("export the bill of materials"), MakeExporter({ EStageAIReport::BillOfMaterials })));
	TestEqual(TEXT("... prompt and the exporting line"), Count(), 2);
	Bom->OnExportFinished.Broadcast(Written(TEXT("BOM_Concert_20261126-120000")));
	if (!TestEqual(TEXT("The finished export adds one line"), Count(), 3))
	{
		return false;
	}
	TestTrue(TEXT("... from the assistant, as done"), Last().Speaker == EStageAISpeaker::Assistant && Last().Outcome == EStageAIOutcome::Succeeded);
	TestEqual(TEXT("... naming the report and its files"), Last().Text.ToString(),
		FString(TEXT("The bill of materials is written: BOM_Concert_20261126-120000.pdf and BOM_Concert_20261126-120000.csv in Saved/StageCraft/Reports.")));
	Bom->OnExportFinished.Broadcast(Written(TEXT("BOM_Again")));
	TestEqual(TEXT("A later export from the panel adds nothing"), Count(), 3);

	// All reports, one of which did not start: each started report is reported once, in the order its files arrive; a failure says
	// where it could not write.
	TestTrue(TEXT("Export all reports"), Assistant->SubmitPrompt(TEXT("export all reports"), MakeExporter({ EStageAIReport::BillOfMaterials, EStageAIReport::CableSchedule })));
	TestEqual(TEXT("... prompt and the exporting line"), Count(), 5);
	Patch->OnExportFinished.Broadcast(Written(TEXT("PatchSheet_Panel")));
	TestEqual(TEXT("The patch sheet did not start for the assistant: nothing"), Count(), 5);
	Cables->OnExportFinished.Broadcast(NotWritten(TEXT("CableSchedule_Locked")));
	if (TestEqual(TEXT("The cable schedule's failure adds a line"), Count(), 6))
	{
		TestTrue(TEXT("... as refused"), Last().Outcome == EStageAIOutcome::Refused);
		TestTrue(FString::Printf(TEXT("... naming the report and the path (%s)"), *Last().Text.ToString()),
			Last().Text.ToString().StartsWith(TEXT("The cable schedule and power plan was not written to C:/Project/Saved/StageCraft/Reports/CableSchedule_Locked.pdf.")));
	}
	Bom->OnExportFinished.Broadcast(Written(TEXT("BOM_All")));
	if (TestEqual(TEXT("The bill of materials adds a line"), Count(), 7))
	{
		TestTrue(TEXT("... with its files"), Last().Text.ToString().Contains(TEXT("BOM_All.pdf and BOM_All.csv")));
	}
	Cables->OnExportFinished.Broadcast(Written(TEXT("CableSchedule_Later")));
	TestEqual(TEXT("Each awaited report is reported once"), Count(), 7);

	// A refused export waits for nothing.
	TestTrue(TEXT("A refused export"), Assistant->SubmitPrompt(TEXT("export the patch sheet"), MakeExporter({})));
	TestTrue(TEXT("... is refused"), Last().Outcome == EStageAIOutcome::Refused);
	const int32 AfterRefusal = Count();
	Patch->OnExportFinished.Broadcast(Written(TEXT("PatchSheet_Panel2")));
	TestEqual(TEXT("... and a later patch sheet export adds nothing"), Count(), AfterRefusal);

	// Asked in Armenian: answered in Armenian.
	TestTrue(TEXT("Armenian export"), Assistant->SubmitPrompt(TEXT("արտահանիր պատչը"), MakeExporter({ EStageAIReport::PatchSheet })));
	Patch->OnExportFinished.Broadcast(Written(TEXT("PatchSheet_HY")));
	TestEqual(TEXT("... the written line is Armenian"), Last().Text.ToString(),
		FString(TEXT("Պատրաստ է DMX պատչ-թերթը՝ PatchSheet_HY.pdf և PatchSheet_HY.csv, Saved/StageCraft/Reports թղթապանակում։")));

	// The conversation can go on while the files are written: a question in between does not lose the export.
	TestTrue(TEXT("Export again"), Assistant->SubmitPrompt(TEXT("export the cable schedule"), MakeExporter({ EStageAIReport::CableSchedule })));
	TestTrue(TEXT("A question while it writes"), Assistant->SubmitPrompt(TEXT("how many fixtures"), MakeExporter({})));
	TestFalse(TEXT("... the assistant is idle"), Assistant->IsBusy());
	Cables->OnExportFinished.Broadcast(Written(TEXT("CableSchedule_After")));
	TestTrue(TEXT("... and the export is still reported"), Last().Text.ToString().Contains(TEXT("CableSchedule_After.pdf")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
