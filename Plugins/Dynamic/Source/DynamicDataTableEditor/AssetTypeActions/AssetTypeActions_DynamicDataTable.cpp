// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetTypeActions_DynamicDataTable.h"
//#include "ToolMenus/Public/ToolMenus.h"
#include "Misc/FileHelper.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/MessageDialog.h"
#include "Framework/Application/SlateApplication.h"

#include "DynamicDataTableEditor/DynamicDataTableEditorModule.h"
//#include "DesktopPlatform/Public/DesktopPlatformModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
//#include "AssetTools/Public/AssetToolsModule.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions"

static const FName NAME_RowStructure(TEXT("RowStructure"));

void FAssetTypeActions_DynamicDataTable::GetActions(const TArray<UObject*>& InObjects, FToolMenuSection& Section)
{
	auto Tables = GetTypedWeakObjectPtrs<UObject>(InObjects);
	
	TArray<FString> ImportPaths;
	for (auto TableIter = Tables.CreateConstIterator(); TableIter; ++TableIter)
	{
		const UDynamicDataTable* CurTable = Cast<UDynamicDataTable>((*TableIter).Get());
		if (CurTable)
		{
			CurTable->AssetImportData->ExtractFilenames(ImportPaths);
		}
	}

	Section.AddMenuEntry(
		"DynamicDataTable_ExportAsCSV",
		LOCTEXT("DynamicDataTable_ExportAsCSV", "Export as CSV"),
		LOCTEXT("DynamicDataTable_ExportAsCSVTooltip", "Export the data table as a file containing CSV data."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP( this, &FAssetTypeActions_DynamicDataTable::ExecuteExportAsCSV, Tables ),
			FCanExecuteAction()
			)
		);

	Section.AddMenuEntry(
		"DynamicDataTable_ExportAsJSON",
		LOCTEXT("DynamicDataTable_ExportAsJSON", "Export as JSON"),
		LOCTEXT("DynamicDataTable_ExportAsJSONTooltip", "Export the data table as a file containing JSON data."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP( this, &FAssetTypeActions_DynamicDataTable::ExecuteExportAsJSON, Tables ),
			FCanExecuteAction()
			)
		);

	/*(TArray<FString> PotentialFileExtensions;
	PotentialFileExtensions.Add(TEXT(".xls"));
	PotentialFileExtensions.Add(TEXT(".xlsm"));
	PotentialFileExtensions.Add(TEXT(".csv"));
	PotentialFileExtensions.Add(TEXT(".json"));
	Section.AddMenuEntry(
		"DynamicDataTable_OpenSourceData",
		LOCTEXT("DynamicDataTable_OpenSourceData", "Open Source Data"),
		LOCTEXT("DynamicDataTable_OpenSourceDataTooltip", "Opens the data table's source data file in an external editor. It will search using the following extensions: .xls/.xlsm/.csv/.json"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP( this, &FAssetTypeActions_DynamicDataTable::ExecuteFindSourceFileInExplorer, ImportPaths, PotentialFileExtensions ),
			FCanExecuteAction::CreateSP(this, &FAssetTypeActions_DynamicDataTable::CanExecuteFindSourceFileInExplorer, ImportPaths, PotentialFileExtensions)
			)
		);*/
}

void FAssetTypeActions_DynamicDataTable::ExecuteExportAsCSV(TArray< TWeakObjectPtr<UObject> > Objects)
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

	const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

	for (auto ObjIt = Objects.CreateConstIterator(); ObjIt; ++ObjIt)
	{
		auto DynamicDataTable = Cast<UDynamicDataTable>((*ObjIt).Get());
		if (DynamicDataTable)
		{
			const FText Title = FText::Format(LOCTEXT("DynamicDataTable_ExportCSVDialogTitle", "Export '{0}' as CSV..."), FText::FromString(*DynamicDataTable->GetName()));
			const FString CurrentFilename = DynamicDataTable->AssetImportData->GetFirstFilename();
			const FString FileTypes = TEXT("Data Table CSV (*.csv)|*.csv");

			TArray<FString> OutFilenames;
			DesktopPlatform->SaveFileDialog(
				ParentWindowWindowHandle,
				Title.ToString(),
				(CurrentFilename.IsEmpty()) ? TEXT("") : FPaths::GetPath(CurrentFilename),
				(CurrentFilename.IsEmpty()) ? TEXT("") : FPaths::GetBaseFilename(CurrentFilename) + TEXT(".csv"),
				FileTypes,
				EFileDialogFlags::None,
				OutFilenames
				);

			if (OutFilenames.Num() > 0)
			{
				FFileHelper::SaveStringToFile(DynamicDataTable->GetTableAsCSV(), *OutFilenames[0]);
			}
		}
	}
}

void FAssetTypeActions_DynamicDataTable::ExecuteExportAsJSON(TArray< TWeakObjectPtr<UObject> > Objects)
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

	const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

	for (auto ObjIt = Objects.CreateConstIterator(); ObjIt; ++ObjIt)
	{
		auto DynamicDataTable = Cast<UDynamicDataTable>((*ObjIt).Get());
		if (DynamicDataTable)
		{
			const FText Title = FText::Format(LOCTEXT("DynamicDataTable_ExportJSONDialogTitle", "Export '{0}' as JSON..."), FText::FromString(*DynamicDataTable->GetName()));
			const FString CurrentFilename = DynamicDataTable->AssetImportData->GetFirstFilename();
			const FString FileTypes = TEXT("Data Table JSON (*.json)|*.json");

			TArray<FString> OutFilenames;
			DesktopPlatform->SaveFileDialog(
				ParentWindowWindowHandle,
				Title.ToString(),
				(CurrentFilename.IsEmpty()) ? TEXT("") : FPaths::GetPath(CurrentFilename),
				(CurrentFilename.IsEmpty()) ? TEXT("") : FPaths::GetBaseFilename(CurrentFilename) + TEXT(".json"),
				FileTypes,
				EFileDialogFlags::None,
				OutFilenames
				);

			if (OutFilenames.Num() > 0)
			{
				FFileHelper::SaveStringToFile(DynamicDataTable->GetTableAsJSON(EDynamicDataTableExportFlags::UseJsonObjectsForStructs), *OutFilenames[0]);
			}
		}
	}
}

void FAssetTypeActions_DynamicDataTable::OpenAssetEditor( const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor )
{
	TArray<UDynamicDataTable*> DynamicDataTablesToOpen;
	TArray<UDynamicDataTable*> InvalidDynamicDataTables;

	for (UObject* Obj : InObjects)
	{
		UDynamicDataTable* Table = Cast<UDynamicDataTable>(Obj);
		if (Table)
		{
			if (Table->GetRowStruct())
			{
				DynamicDataTablesToOpen.Add(Table);
			}
			else
			{
				InvalidDynamicDataTables.Add(Table);
			}
		}
	}

	if (InvalidDynamicDataTables.Num() > 0)
	{
		FTextBuilder DynamicDataTablesListText;
		DynamicDataTablesListText.Indent();
		for (UDynamicDataTable* Table : InvalidDynamicDataTables)
		{
			const FTopLevelAssetPath ResolvedRowStructName = Table->GetRowStructPathName();
			DynamicDataTablesListText.AppendLineFormat(LOCTEXT("DynamicDataTable_MissingRowStructListEntry", "* {0} (Row Structure: {1})"), FText::FromString(Table->GetName()), FText::FromString(ResolvedRowStructName.ToString()));
		}

		FText Title = LOCTEXT("DynamicDataTable_MissingRowStructTitle", "Continue?");
		const EAppReturnType::Type DlgResult = FMessageDialog::Open(
			EAppMsgType::YesNoCancel, 
			FText::Format(LOCTEXT("DynamicDataTable_MissingRowStructMsg", "The following Data Tables are missing their row structure and will not be editable.\n\n{0}\n\nDo you want to open these data tables?"), DynamicDataTablesListText.ToText()), 
			&Title
			);

		switch(DlgResult)
		{
		case EAppReturnType::Yes:
			DynamicDataTablesToOpen.Append(InvalidDynamicDataTables);
			break;
		case EAppReturnType::Cancel:
			return;
		default:
			break;
		}
	}

	FDynamicDataTableEditorModule& DynamicDataTableEditorModule = FModuleManager::LoadModuleChecked<FDynamicDataTableEditorModule>("DynamicDataTableEditor");
	for (UDynamicDataTable* Table : DynamicDataTablesToOpen)
	{
		DynamicDataTableEditorModule.CreateDynamicDataTableEditor(EToolkitMode::Standalone, EditWithinLevelEditor, Table);
	}
}

void FAssetTypeActions_DynamicDataTable::GetResolvedSourceFilePaths(const TArray<UObject*>& TypeAssets, TArray<FString>& OutSourceFilePaths) const
{
	for (auto& Asset : TypeAssets)
	{
		const auto DynamicDataTable = CastChecked<UDynamicDataTable>(Asset);
		DynamicDataTable->AssetImportData->ExtractFilenames(OutSourceFilePaths);
	}
}

// Attempts to export temporary CSV files and diff those. If that fails we fall back to diffing the data table assets directly.
void FAssetTypeActions_DynamicDataTable::PerformAssetDiff(UObject* OldAsset, UObject* NewAsset, const FRevisionInfo& OldRevision, const FRevisionInfo& NewRevision) const
{
	UDynamicDataTable* OldDynamicDataTable = CastChecked<UDynamicDataTable>(OldAsset);
	UDynamicDataTable* NewDynamicDataTable = CastChecked<UDynamicDataTable>(NewAsset);

	// Build names for temp csv files
	FString RelOldTempFileName = FString::Printf(TEXT("%sTemp%s-%s.csv"), *FPaths::DiffDir(), *OldAsset->GetName(), *OldRevision.Revision);
	FString AbsoluteOldTempFileName = FPaths::ConvertRelativePathToFull(RelOldTempFileName);
	FString RelNewTempFileName = FString::Printf(TEXT("%sTemp%s-%s.csv"), *FPaths::DiffDir(), *NewAsset->GetName(), *NewRevision.Revision);
	FString AbsoluteNewTempFileName = FPaths::ConvertRelativePathToFull(RelNewTempFileName);

	// save temp files
	bool OldResult = FFileHelper::SaveStringToFile(OldDynamicDataTable->GetTableAsCSV(EDynamicDataTableExportFlags::UseSimpleText), *AbsoluteOldTempFileName);
	bool NewResult = FFileHelper::SaveStringToFile(NewDynamicDataTable->GetTableAsCSV(EDynamicDataTableExportFlags::UseSimpleText), *AbsoluteNewTempFileName);

	if (OldResult && NewResult)
	{
		FString DiffCommand = GetDefault<UEditorLoadingSavingSettings>()->TextDiffToolPath.FilePath;

		FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
		AssetToolsModule.Get().CreateDiffProcess(DiffCommand, AbsoluteOldTempFileName, AbsoluteNewTempFileName);
	}
	else
	{
		FAssetTypeActions_CSVAssetBase::PerformAssetDiff(OldAsset, NewAsset, OldRevision, NewRevision);
	}
}

FText FAssetTypeActions_DynamicDataTable::GetDisplayNameFromAssetData(const FAssetData& AssetData) const
{
	if (AssetData.IsValid())
	{
		const FAssetDataTagMapSharedView::FFindTagResult RowStructureTag = AssetData.TagsAndValues.FindTag(NAME_RowStructure);
		if (RowStructureTag.IsSet())
		{
			if (UScriptStruct* FoundStruct = UClass::TryFindTypeSlow<UScriptStruct>(RowStructureTag.GetValue(), EFindFirstObjectOptions::ExactClass))
			{
				return FText::Format(LOCTEXT("DynamicDataTableWithRowType", "Dynamic Table ({0})"), FoundStruct->GetDisplayNameText());
			}
		}
	}

	return FText::GetEmpty();
}

FColor FAssetTypeActions_DynamicDataTable::GetTypeColor() const {
	return FColor(128,255,128,255);
}

uint32 FAssetTypeActions_DynamicDataTable::GetCategories() {
	return GDepricatedCategory; //EAssetTypeCategories::Misc;
}

#undef LOCTEXT_NAMESPACE
