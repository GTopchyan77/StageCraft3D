// Copyright Epic Games, Inc. All Rights Reserved.

// AModularPlayerController: the MVR scene import, re-import and export requests (Docs/ADR/0034 §6–§7, §18; MV2–MV5), kept apart from
// the main file.

#include "Player/ModularPlayerController.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageItemSnapshot.h"
#include "Import/StageFileDialog.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Mvr/StageMvrExportSubsystem.h"
#include "Mvr/StageMvrImportSubsystem.h"
#include "Mvr/StageMvrStageReader.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

#define LOCTEXT_NAMESPACE "StageCraftMvr"

namespace StageControllerMvr
{
	UStageMvrImportSubsystem* GetImporter(const AModularPlayerController& Controller)
	{
		const UGameInstance* GameInstance = Controller.GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UStageMvrImportSubsystem>() : nullptr;
	}

	FStageMvrStage ReadStage(const AModularPlayerController& Controller)
	{
		const UWorld* World = Controller.GetWorld();
		return World ? StageMvrStageReader::Read(*World) : FStageMvrStage();
	}
}

bool AModularPlayerController::RequestImportMvr(const FString& File)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageMvrImportSubsystem* Importer = StageControllerMvr::GetImporter(*this);
	const FStageEconomyResultInfo Verdict = GameMode && Importer
		? GameMode->EvaluateMvrImport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrImport", "MVR import needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	if (const TOptional<FText> Refusal = Importer->RequestRead(File, StageControllerMvr::ReadStage(*this)))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, *Refusal));
		return false;
	}
	return true;
}

bool AModularPlayerController::RequestBrowseMvrFile()
{
	if (!CanBrowseModelFiles())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("NoMvrDialog", "This build has no file dialog. Drag the .mvr file onto the viewport instead.")));
		return false;
	}
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateMvrImport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrImportMode", "MVR import needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	TArray<FString> Files;
	const FString Filter = LOCTEXT("MvrFilter", "MVR scenes (*.mvr)").ToString() + TEXT("|*.mvr");
	if (!StageFileDialog::OpenFiles(LOCTEXT("ImportMvrTitle", "Import an MVR Scene"), LastImportDirectory, Filter, Files) || Files.IsEmpty())
	{
		return false;
	}
	LastImportDirectory = FPaths::GetPath(Files[0]);
	return RequestImportMvr(Files[0]);
}

FStageEconomyResultInfo AModularPlayerController::RequestConfirmMvrImport(const FGuid& ReviewId, EStageMvrMissingItems Missing)
{
	const auto Refuse = [this](const FStageEconomyResultInfo& Refusal)
	{
		ReportRejection(Refusal);
		return Refusal;
	};
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageMvrImportSubsystem* Importer = StageControllerMvr::GetImporter(*this);
	if (!GameMode || !Importer)
	{
		return Refuse(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrConfirm", "MVR import needs the StageCraft game mode.")));
	}
	const FStageEconomyResultInfo Verdict = GameMode->EvaluateMvrImport(this);
	if (!Verdict.IsSuccess())
	{
		return Refuse(Verdict);
	}
	if (!SpawnSystem || !EditHistory)
	{
		return Refuse(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("MvrNoSpawner", "Placing is not available in this level.")));
	}
	if (IsBusyWithPointer())
	{
		return Refuse(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("MvrBusy", "Finish the drag or camera move first.")));
	}

	UStageMvrImportSubsystem::FPlacementRequest Request;
	Request.Spawner = SpawnSystem;
	Request.Check = [WeakThis = TWeakObjectPtr<AModularPlayerController>(this)](const UBaseItemData& Item, int32 Quantity)
	{
		AModularPlayerController* Self = WeakThis.Get();
		return Self ? Self->EvaluatePlacementPreview(Item, Quantity)
			: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("MvrNoPlayer", "The player left while the rig was placed."));
	};
	// Read again now: the stage may have changed since the review, and the plan must compare with what it will change.
	Request.Stage = StageControllerMvr::ReadStage(*this);
	Request.Missing = Missing;
	Request.OnPlaced = [WeakThis = TWeakObjectPtr<AModularPlayerController>(this)](TConstArrayView<FStageItemSnapshot> Removed, TConstArrayView<AModularBaseActor*> Items,
		const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)
	{
		AModularPlayerController* Self = WeakThis.Get();
		return Self ? Self->RecordMvrImport(Removed, Items, LayerGroups, StepName) : 0;
	};

	const FStageEconomyResultInfo Started = Importer->StartImport(ReviewId, MoveTemp(Request));
	return Started.IsSuccess() ? Started : Refuse(Started);
}

void AModularPlayerController::RequestCancelMvrImport(const FGuid& ReviewId)
{
	if (UStageMvrImportSubsystem* Importer = StageControllerMvr::GetImporter(*this))
	{
		Importer->CancelReview(ReviewId);
	}
}

bool AModularPlayerController::RequestExportMvr(const FString& FilePath)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageMvrExportSubsystem* Exporter = UWorld::GetSubsystem<UStageMvrExportSubsystem>(GetWorld());
	FStageEconomyResultInfo Verdict = GameMode && Exporter
		? GameMode->EvaluateMvrExport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrExport", "MVR export needs the StageCraft game mode."));
	FText Reason;
	if (Verdict.IsSuccess() && !Exporter->StartExport(FilePath, Reason))
	{
		Verdict = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Reason);
	}
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	return true;
}

bool AModularPlayerController::RequestBrowseMvrExport()
{
	if (!CanBrowseModelFiles())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrSaveDialog", "This build has no file dialog to choose where the .mvr goes.")));
		return false;
	}
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateMvrExport(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoMvrExportMode", "MVR export needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	const FString SceneName = Session ? Session->GetSceneName() : FString();
	const FString DefaultName = (SceneName.IsEmpty() ? FString(TEXT("Stage")) : SceneName) + TEXT(".mvr");
	FString File;
	const FString Filter = LOCTEXT("MvrExportFilter", "MVR scenes (*.mvr)").ToString() + TEXT("|*.mvr");
	if (!StageFileDialog::SaveFile(LOCTEXT("ExportMvrTitle", "Export an MVR Scene"), LastImportDirectory, DefaultName, Filter, File) || File.IsEmpty())
	{
		return false;
	}
	if (!File.EndsWith(TEXT(".mvr"), ESearchCase::IgnoreCase))
	{
		File += TEXT(".mvr");
	}
	LastImportDirectory = FPaths::GetPath(File);
	return RequestExportMvr(File);
}

int32 AModularPlayerController::RecordMvrImport(TConstArrayView<FStageItemSnapshot> Removed, TConstArrayView<AModularBaseActor*> Items, const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)
{
	if (!EditHistory)
	{
		return 0;
	}
	// One step for the items and the groups. When another group is open (an assistant plan), the import joins it.
	const bool bOwnGroup = EditHistory->BeginGroup(StepName);
	// Removals and the old state of updated items first, as they were applied, so undo restores them after the new state is gone.
	EditHistory->RecordReplacement(Removed, Items, StepName);
	int32 Made = 0;
	if (!LayerGroups.IsEmpty())
	{
		EditGroups(StepName, [&LayerGroups, &Made](UStageShowLibrarySubsystem& Library)
		{
			for (const TPair<FString, TArray<int32>>& Layer : LayerGroups)
			{
				// A re-import adds its new fixtures to the layer's group from the first import instead of making a second one.
				const FStageFixtureGroup* Existing = Library.GetGroups().FindByPredicate([&Layer](const FStageFixtureGroup& Group)
				{
					return Group.Name.ToString().Equals(Layer.Key, ESearchCase::IgnoreCase);
				});
				if (Existing)
				{
					TArray<int32> Fixtures = Existing->FixtureIds;
					for (const int32 FixtureId : Layer.Value)
					{
						Fixtures.AddUnique(FixtureId);
					}
					Made += Library.SetGroupFixtures(Existing->Id, Fixtures) ? 1 : 0;
				}
				else if (Library.AddGroup(FText::FromString(Layer.Key), Layer.Value).IsValid())
				{
					++Made;
				}
			}
			return Made > 0;
		});
	}
	if (bOwnGroup)
	{
		EditHistory->EndGroup();
	}
	return Made;
}

#undef LOCTEXT_NAMESPACE
