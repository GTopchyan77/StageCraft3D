// Copyright Epic Games, Inc. All Rights Reserved.

// AModularPlayerController: the MVR scene import requests (Docs/ADR/0034 §6, MV2 and MV3), kept apart from the main file.

#include "Player/ModularPlayerController.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageSpanData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "History/StageEditHistoryComponent.h"
#include "Import/StageFileDialog.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Mvr/StageMvrImportSubsystem.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/ShowControlSubsystem.h"
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
	if (const TOptional<FText> Refusal = Importer->RequestRead(File))
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

FStageEconomyResultInfo AModularPlayerController::RequestConfirmMvrImport(const FGuid& ReviewId)
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
	// The Sketch window's truss and fixture are the ones a drawing builds with: MVR trusses become runs of that truss, and fixtures
	// without a usable GDTF type that fixture.
	if (const UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(GetWorld()))
	{
		Request.TrussItem = Cast<UStageSpanData>(Sketch->GetItem(EStageSketchElement::Truss));
		Request.GenericFixture = Sketch->GetItem(EStageSketchElement::Fixture);
	}
	if (const UShowControlSubsystem* ShowControl = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld()))
	{
		for (const ALightingFixtureActor* Fixture : ShowControl->GetFixtures())
		{
			Request.UsedFixtureIds.Add(Fixture->GetFixtureId());
		}
	}
	if (const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld()))
	{
		for (const AModularBaseActor* Item : Session->GetPlacedItems())
		{
			if (Item && Item->GetMvrId().IsValid())
			{
				Request.ExistingMvrIds.Add(Item->GetMvrId());
			}
		}
	}
	Request.OnPlaced = [WeakThis = TWeakObjectPtr<AModularPlayerController>(this)](TConstArrayView<AModularBaseActor*> Items, const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)
	{
		AModularPlayerController* Self = WeakThis.Get();
		return Self ? Self->RecordMvrImport(Items, LayerGroups, StepName) : 0;
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

int32 AModularPlayerController::RecordMvrImport(TConstArrayView<AModularBaseActor*> Items, const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)
{
	if (!EditHistory)
	{
		return 0;
	}
	// One step for the items and the groups. When another group is open (an assistant plan), the import joins it.
	const bool bOwnGroup = EditHistory->BeginGroup(StepName);
	EditHistory->RecordPlacements(Items, StepName);
	int32 Made = 0;
	if (!LayerGroups.IsEmpty())
	{
		EditGroups(StepName, [&LayerGroups, &Made](UStageShowLibrarySubsystem& Library)
		{
			for (const TPair<FString, TArray<int32>>& Layer : LayerGroups)
			{
				if (Library.AddGroup(FText::FromString(Layer.Key), Layer.Value).IsValid())
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
