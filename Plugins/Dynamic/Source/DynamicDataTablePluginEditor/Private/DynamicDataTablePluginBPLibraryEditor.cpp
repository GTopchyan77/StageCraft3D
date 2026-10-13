// Copyright Vyrulent 2023 All Rights Reserved.

/*
	THIS FILE IS NOT INTENDED TO BE DOCUMENTED.
	This is due to both time considerations, and rudimentary protection of methods used.
	You can hover over a node in a blueprint for a brief description, check out the Content\READ_ME blueprint, or check out the header file in your IDE.
	Email me at vyrulentstudios@gmail.com if you wish to understand some of the code (and it's important)
*/

#pragma warning( disable : 4701)
#pragma warning( disable : 4703)
#pragma warning( disable : 4102) //unused label
#include "DynamicDataTablePluginBPLibraryEditor.h"
#include "DynamicDataTablePluginEditor.h"
#include "DynamicDatatableBPLibrary.h"
#include "DynamicDatatable2.h"
#include "Chaos/ChaosPerfTest.h"
#include "Serialization/BufferArchive.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include <thread>
#include "Async/TaskGraphInterfaces.h"
#include "Async/Async.h"
#include "Engine/CompositeDataTable.h"
#include "BlueprintUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "FramePro/FramePro.h"
#include "PackageUtils/PackageUtils.h"
#include "UObject/SavePackage.h"


UDynamicDataTablePluginBPLibraryEditor::UDynamicDataTablePluginBPLibraryEditor(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

void UDynamicDataTablePluginBPLibraryEditor::CreateEnumFromTableRows_EDITOR(FDynamicTable& Table, FString Path, FString EnumName) {
	FString FullPath = Path+EnumName;
	int x = 0, idx = 0, p = -1, s = -1, id = 0, i2 = 0;
	TArray<TPair<FName, int64>> array = {};
	TPair<FName, int64> blank = {};
	FString str = "", fullpath = Path+EnumName;
	fullpath = FPaths::ConvertRelativePathToFull(fullpath);
	FString PackageName = Path+EnumName;
	FPackageName::TryConvertFilenameToLongPackageName(fullpath, PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	check(Package);
	UEnum* existing = FindObject<UUserDefinedEnum>(Package, *EnumName);
	UEnum* EnumObject = nullptr;
	if (existing) {
		if (existing->IsA(UEnum::StaticClass())) {
			EnumObject = existing;
		}
		else {return;}
	}
	else {
		EnumObject = NewObject<UUserDefinedEnum>(Package, FName(EnumName), RF_Public | RF_Standalone);
	}
	if (EnumObject!=nullptr) {
		x = 0;
		for (auto& each : Table.RowNames) {
			if (!each.IsNone()) {
				idx = array.Emplace(blank);
				str = EnumObject->GetName()+"::"+each.ToString();
				array[idx].Key = FName(str);
				array[idx].Value = x;
				if (x>=254) {goto go;}
				++x;
			}
		}
		go:;
		EnumObject->SetEnums(array, UEnum::ECppForm::Namespaced);
		EnumObject->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
		Package->FullyLoad();
		EnumObject->MarkPackageDirty();
		FString fullpath2 = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags       = RF_Standalone;
		SaveArgs.bForceByteSwapping  = true;
		SaveArgs.bWarnOfLongFilename = true;
		SaveArgs.SaveFlags           = SAVE_NoError;

		const bool bSuccess = UPackage::SavePackage(Package, nullptr, *fullpath2, SaveArgs);

		//UPackage::SavePackage(Package, NULL, RF_Standalone, *fullpath2, GError, nullptr, true, true, SAVE_NoError);
		FAssetRegistryModule::AssetCreated(EnumObject);
		FAssetRegistryModule::AssetsSaved({EnumObject});
	}
}

void UDynamicDataTablePluginBPLibraryEditor::CreateEnumFromTableSources_EDITOR(FDynamicTable& Table, FString Path, FString EnumName, bool RemovePrefix, bool RemoveSuffix) {
	FString FullPath = Path+EnumName;
	int x = 0, idx = 0, p = -1, s = -1, id = 0, i2 = 0;
	TArray<TPair<FName, int64>> array = {};
	TPair<FName, int64> blank = {};
	FString str = "", fullpath = Path+EnumName;
	fullpath = FPaths::ConvertRelativePathToFull(fullpath);
	FString PackageName = Path+EnumName;
	FPackageName::TryConvertFilenameToLongPackageName(fullpath, PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	check(Package);
	UEnum* existing = FindObject<UUserDefinedEnum>(Package, *EnumName);
	UEnum* EnumObject = nullptr;
	if (existing) {
		if (existing->IsA(UEnum::StaticClass())) {
			EnumObject = existing;
		}
		else {return;}
	}
	else {
		EnumObject = NewObject<UUserDefinedEnum>(Package, FName(EnumName), RF_Public | RF_Standalone);
	}
	if (RemovePrefix||RemoveSuffix) {//discover prefix and suffix
		FString prefix = "", suffix = "";
		x = 0;
		for (auto& each : Table.SourceRowNames) {
			if (each.Key!=nullptr) {
				if (x==0) {prefix = each.Key->GetName(); suffix = each.Key->GetName();}
				else {
					if (RemovePrefix) {
						p = -1;
						for (int32 i = 0; i < prefix.Len() && i < each.Key->GetName().Len(); i++) {
							if (prefix[i] == each.Key->GetName()[i]) {
								p = i+1;
							}
							else {break;}
						}
						if (p>=0) {
							prefix = prefix.Left(p);
						}
						else {
							prefix = "";
						}
					}
					if (RemoveSuffix) {
						s = -1;
						id = 0, i2 = each.Key->GetName().Len()-1;
						for (int32 i = std::min(suffix.Len(),each.Key->GetName().Len())-1; i>=0; i--) {
							if (suffix[i] == each.Key->GetName()[i2]) {
								s = id+1;
							}
							else {break;}
							++id;
							--i2;
						}
						if (s>=0) {
							suffix = suffix.Right(s);
						}
						else {
							suffix = "";
						}
					}
				}
				++x;
			}
		}
		//{FString str4 = prefix+" + "+suffix; UE_LOG(LogTemp, Log, TEXT("%s"), *str4);}
	}
	if (EnumObject!=nullptr) {
		//EnumObject->CppType = "ERegularEnum";
		x = 0;
		for (auto& each : Table.SourceRowNames) {
			if (each.Key!=nullptr) {
				idx = array.Emplace(blank);
				if (!RemovePrefix&&!RemoveSuffix) {str = EnumObject->GetName()+"::"+each.Key->GetName();}
				else {
					FString tname = each.Key->GetName();
					if (RemovePrefix&&p>0) {tname = tname.RightChop(p);}
					if (RemoveSuffix&&s>0) {tname = tname.LeftChop(s);}
					str = EnumObject->GetName()+"::"+tname;
				}
				array[idx].Key = FName(str);
				array[idx].Value = x;
				if (x>=254) {goto go;}
				++x;
			}
		}
		go:;
		EnumObject->SetEnums(array, UEnum::ECppForm::Namespaced);
		EnumObject->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
		Package->FullyLoad();
		EnumObject->MarkPackageDirty();
		FString fullpath2 = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags       = RF_Standalone;
		SaveArgs.bForceByteSwapping  = true;
		SaveArgs.bWarnOfLongFilename = true;
		SaveArgs.SaveFlags           = SAVE_NoError;

		UPackage::SavePackage(Package,nullptr,*fullpath2,SaveArgs);

		//UPackage::SavePackage(Package, NULL, RF_Standalone, *fullpath2, GError, nullptr, true, true, SAVE_NoError);
		FAssetRegistryModule::AssetCreated(EnumObject);
		FAssetRegistryModule::AssetsSaved({EnumObject});
	}
}

/*void UDynamicDataTablePluginBPLibraryEditor::ConvertDynamicTableAssetToDataTableAsset_EDITOR(UDynamicDataTable* Table, FString Path, FString AssetName) {
	if (!Table||Path==""||AssetName=="") {return;}
	FString fullpath = Path+AssetName;
	fullpath = FPaths::ConvertRelativePathToFull(fullpath);
	FString PackageName = Path+AssetName;
	FPackageName::TryConvertFilenameToLongPackageName(fullpath, PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	check(Package);
	UDataTable* DTObject = nullptr;
	UDataTable* existing = FindObject<UDataTable>(Package, *AssetName);
    if (existing) {
		DTObject = existing;
    }
	else {
		DTObject = NewObject<UDataTable>(Package, FName(AssetName), RF_Public | RF_Standalone);
	}
	///*DTObject->RowStruct = Table->RowStruct;
	//DTObject->AssetImportData = Table->AssetImportData;
	//DTObject->bIgnoreExtraFields = Table->RowStruct;
	//DTObject->bIgnoreMissingFields = Table->RowStruct;
	//DTObject->ImportKeyField = Table->RowStruct;
	//DTObject->bStripFromClientBuilds = Table->RowStruct;
	//DTObject->RowStructPathName = Table->RowStruct;
	//Table->FindRowUnchecked()
	//for (auto& each : Table->DynamicTable.Map) {
	//	DTObject->AddRow()
	//}//
	////copy all uproperties
	TMap<FName,FProperty*> props = {};
	for (TFieldIterator<FProperty> It(Table->GetClass()); It; ++It) {
		FProperty* Property = *It;
		if (Property) {
			props.Emplace(FName(Property->GetAuthoredName().TrimStartAndEnd()),Property);
		}
	}
	for (TFieldIterator<FProperty> It(DTObject->GetClass()); It; ++It) {
		FProperty* PropB = *It;
		if (PropB) {
			FName name = FName(PropB->GetAuthoredName().TrimStartAndEnd());
			if (props.Contains(name)) {
				FProperty* PropA = props[name];
				PropB->CopyCompleteValue(PropB->ContainerPtrToValuePtr<void>(DTObject), PropA->ContainerPtrToValuePtr<void>(Table));
			}
		}
	}
	auto& data = &DTObject->GetRowMap();
	for (auto It = Table->GetRowMap().CreateConstIterator(); It; ++It) {
		data.Emplace(It.Key(),*It.Value());
	}
	Package->FullyLoad();
	DTObject->MarkPackageDirty();
	FString fullpath2 = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UPackage::SavePackage(Package, NULL, RF_Standalone, *fullpath2, GError, nullptr, true, true, SAVE_NoError);
	FAssetRegistryModule::AssetCreated(DTObject);
	FAssetRegistryModule::AssetSaved(*DTObject);
}
*/
