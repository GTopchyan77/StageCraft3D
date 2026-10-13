// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "DynamicDataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/EnumProperty.h"
#include "Misc/StringOutputDevice.h"
#include "DynamicDataTableJSON.h"

DEFINE_LOG_CATEGORY(LogDynamicDataTable);

namespace DynamicDataTableUtilsImpl
{

FString GetSourceString(const FText& Text)
{
	if (const FString* SourceString = FTextInspector::GetSourceString(Text))
	{
		return *SourceString;
	}
	return Text.ToString();
}

void AssignStringToPropertyDirect(const FString& InString, const FProperty* InProp, uint8* InData, const int32 InPortFlags, FStringOutputDevice& OutImportError)
{
	auto DoImportText = [&](const FString& InStringToImport)
	{
		InProp->ImportText_Direct(*InStringToImport, InData, nullptr, InPortFlags, &OutImportError);
	};

	bool bNeedsImport = true;

	UEnum* Enum = nullptr;

	if (const FEnumProperty* EnumProp = CastField<const FEnumProperty>(InProp))
	{
		Enum = EnumProp->GetEnum();
	}
	else if (const FByteProperty* ByteProp = CastField<const FByteProperty>(InProp))
	{
		if (ByteProp->IsEnum())
		{
			Enum = ByteProp->GetIntPropertyEnum();
		}
	}

	if (Enum)
	{
		// Enum properties may use the friendly name in their import data, however the FPropertyByte::ImportText function will only accept the internal enum entry name
		// Detect if we're using a friendly name for an entry, and if so, try and map it to the correct internal name before performing the import
		const int32 EnumIndex = Enum->GetIndexByNameString(InString);
		if(EnumIndex == INDEX_NONE)
		{
			// Couldn't find a match for the name we were given, try and find a match using the friendly names
			for(int32 EnumEntryIndex = 0; EnumEntryIndex < Enum->NumEnums(); ++EnumEntryIndex)
			{
				const FText FriendlyEnumEntryName = Enum->GetDisplayNameTextByIndex(EnumEntryIndex);
				if ((FriendlyEnumEntryName.ToString() == InString) || (GetSourceString(FriendlyEnumEntryName) == InString))
				{
					// Get the corresponding internal name and warn the user that we're using this fallback if not a user-defined enum
					FString StringToImport = Enum->GetNameStringByIndex(EnumEntryIndex);
					if (!Enum->IsA<UUserDefinedEnum>())
					{
						UE_LOG(LogDynamicDataTable, Warning, TEXT("Could not a find matching enum entry for '%s', but did find a matching display name. Will import using the enum entry corresponding to that display name ('%s')"), *InString, *StringToImport);
					}
					DoImportText(StringToImport);
					bNeedsImport = false;
					break;
				}
			}
		}
	}

	if(bNeedsImport)
	{
		DoImportText(InString);
	}
}

void AssignStringToProperty(const FString& InString, const FProperty* InProp, uint8* InData, const int32 InIndex, const int32 InPortFlags, FStringOutputDevice& OutImportError)
{
	uint8* ValuePtr = InProp->ContainerPtrToValuePtr<uint8>(InData, InIndex);
	AssignStringToPropertyDirect(InString, InProp, ValuePtr, InPortFlags, OutImportError);
}

void GetPropertyValueAsStringDirect(const FProperty* InProp, const uint8* InData, const int32 InPortFlags, const EDynamicDataTableExportFlags InDTExportFlags, FString& OutString)
{
#if WITH_EDITOR
	if (InPortFlags & PPF_PropertyWindow)
	{
		auto ExportStructAsJson = [InDTExportFlags](const UScriptStruct* InStruct, const void* InStructData) -> FString
		{
			FString JsonOutputStr;
			{
				TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> JsonWriter = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonOutputStr);

				JsonWriter->WriteObjectStart();
				FDynamicDataTableExporterJSON(InDTExportFlags, JsonWriter).WriteStruct(InStruct, InStructData);
				JsonWriter->WriteObjectEnd();

				JsonWriter->Close();
			}

			JsonOutputStr.ReplaceInline(TEXT("\t"), TEXT(""), ESearchCase::CaseSensitive);
			JsonOutputStr.ReplaceInline(TEXT("\r"), TEXT(""), ESearchCase::CaseSensitive);
			JsonOutputStr.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);

			return JsonOutputStr;
		};

		const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(InProp);
		const FSetProperty* SetProp = CastField<const FSetProperty>(InProp);
		const FMapProperty* MapProp = CastField<const FMapProperty>(InProp);

		if (ArrayProp && ArrayProp->Inner->IsA<FStructProperty>() && EnumHasAnyFlags(InDTExportFlags, EDynamicDataTableExportFlags::UseJsonObjectsForStructs))
		{
			const FStructProperty* StructInner = CastFieldChecked<const FStructProperty>(ArrayProp->Inner);

			OutString.AppendChar('(');

			FScriptArrayHelper ArrayHelper(ArrayProp, InData);
			for (int32 ArrayEntryIndex = 0; ArrayEntryIndex < ArrayHelper.Num(); ++ArrayEntryIndex)
			{
				if (ArrayEntryIndex > 0)
				{
					OutString.AppendChar(',');
					OutString.AppendChar(' ');
				}

				const uint8* ArrayEntryData = ArrayHelper.GetRawPtr(ArrayEntryIndex);
				OutString.Append(ExportStructAsJson(StructInner->Struct, ArrayEntryData));
			}

			OutString.AppendChar(')');
			return;
		}
		else if (SetProp && SetProp->ElementProp->IsA<FStructProperty>() && EnumHasAnyFlags(InDTExportFlags, EDynamicDataTableExportFlags::UseJsonObjectsForStructs))
		{
			const FStructProperty* StructInner = CastFieldChecked<const FStructProperty>(SetProp->ElementProp);

			OutString.AppendChar('(');

			int32 NumWrittenSetEntries = 0;
			FScriptSetHelper SetHelper(SetProp, InData);
			for (int32 SetSparseIndex = 0; SetSparseIndex < SetHelper.GetMaxIndex(); ++SetSparseIndex)
			{
				if (SetHelper.IsValidIndex(SetSparseIndex))
				{
					if (NumWrittenSetEntries++ > 0)
					{
						OutString.AppendChar(',');
						OutString.AppendChar(' ');
					}

					const uint8* SetEntryData = SetHelper.GetElementPtr(SetSparseIndex);
					OutString.Append(ExportStructAsJson(StructInner->Struct, SetEntryData));
				}
			}

			OutString.AppendChar(')');
			return;
		}
		else if (MapProp)
		{
			OutString.AppendChar('(');

			int32 NumWrittenMapEntries = 0;
			FScriptMapHelper MapHelper(MapProp, InData);
			for (int32 MapSparseIndex = 0; MapSparseIndex < MapHelper.GetMaxIndex(); ++MapSparseIndex)
			{
				if (MapHelper.IsValidIndex(MapSparseIndex))
				{
					if (NumWrittenMapEntries++ > 0)
					{
						OutString.AppendChar(',');
						OutString.AppendChar(' ');
					}

					const uint8* MapKeyData = MapHelper.GetKeyPtr(MapSparseIndex);
					const uint8* MapValueData = MapHelper.GetValuePtr(MapSparseIndex);

					OutString.AppendChar('"');
					GetPropertyValueAsStringDirect(MapHelper.GetKeyProperty(), MapKeyData, InPortFlags, InDTExportFlags, OutString);
					OutString.AppendChar('"');

					OutString.Append(TEXT(" = "));

					if (MapHelper.GetValueProperty()->IsA<FStructProperty>() && EnumHasAnyFlags(InDTExportFlags, EDynamicDataTableExportFlags::UseJsonObjectsForStructs))
					{
						const FStructProperty* StructMapValue = CastFieldChecked<const FStructProperty>(MapHelper.GetValueProperty());
						OutString.Append(ExportStructAsJson(StructMapValue->Struct, MapValueData));
					}
					else
					{
						GetPropertyValueAsStringDirect(MapHelper.GetValueProperty(), MapValueData, InPortFlags, InDTExportFlags, OutString);
					}
				}
			}

			OutString.AppendChar(')');
			return;
		}
		else if (const FStructProperty* StructProp = CastField<const FStructProperty>(InProp))
		{
			OutString.Append(ExportStructAsJson(StructProp->Struct, InData));
			return;
		}
	}
#endif // WITH_EDITOR

	if (EnumHasAnyFlags(InDTExportFlags, EDynamicDataTableExportFlags::UseSimpleText) && InProp->IsA<FTextProperty>())
	{
		const FTextProperty* TextProp = CastFieldChecked<const FTextProperty>(InProp);
		const FText& TextValue = TextProp->GetPropertyValue(InData);
		OutString.Append(TextValue.ToString());
		return;
	}

	InProp->ExportText_Direct(OutString, InData, InData, nullptr, InPortFlags);
}

void GetPropertyValueAsString(const FProperty* InProp, const uint8* InData, const int32 InIndex, const int32 InPortFlags, const EDynamicDataTableExportFlags InDTExportFlags, FString& OutString)
{
	const uint8* ValuePtr = InProp->ContainerPtrToValuePtr<uint8>(InData, InIndex);
	return GetPropertyValueAsStringDirect(InProp, ValuePtr, InPortFlags, InDTExportFlags, OutString);
}

}

FString DynamicDataTableUtils::AssignStringToPropertyDirect(const FString& InString, const FProperty* InProp, uint8* InData)
{
	FStringOutputDevice ImportError;
	if(InProp && IsSupportedTableProperty(InProp))
	{
		DynamicDataTableUtilsImpl::AssignStringToPropertyDirect(InString, InProp, InData, PPF_ExternalEditor, ImportError);
	}

	FString Error = ImportError;
	return Error;
}

FString DynamicDataTableUtils::AssignStringToProperty(const FString& InString, const FProperty* InProp, uint8* InData)
{
	FStringOutputDevice ImportError;
	if(InProp && IsSupportedTableProperty(InProp))
	{
		if(InProp->ArrayDim == 1)
		{
			DynamicDataTableUtilsImpl::AssignStringToProperty(InString, InProp, InData, 0, PPF_ExternalEditor, ImportError);
		}
		else
		{
			if(InString.Len() >= 2 && InString[0] == '(' && InString[InString.Len() - 1] == ')')
			{
				// Trim the ( and )
				FString StringToSplit = InString.Mid(1, InString.Len() - 2);

				TArray<FString> Values;
				StringToSplit.ParseIntoArray(Values, TEXT(","), 1);

				if (InProp->ArrayDim != Values.Num())
				{
					UE_LOG(LogDynamicDataTable, Warning, TEXT("%s - Array is %d elements large, but we have %d values to import"), *InProp->GetName(), InProp->ArrayDim, Values.Num());
				}

				for (int32 Index = 0; Index < InProp->ArrayDim; ++Index)
				{
					if (Values.IsValidIndex(Index))
					{
						DynamicDataTableUtilsImpl::AssignStringToProperty(Values[Index], InProp, InData, Index, PPF_Delimited, ImportError);
					}
				}
			}
			else
			{
				UE_LOG(LogDynamicDataTable, Warning, TEXT("%s - Malformed array string. It must start with '(' and end with ')'"), *InProp->GetName());
			}
		}
	}

	FString Error = ImportError;
	return Error;
}

FString DynamicDataTableUtils::GetPropertyValueAsStringDirect(const FProperty* InProp, const uint8* InData, const EDynamicDataTableExportFlags InDTExportFlags)
{
	FString Result;

	if(InProp && IsSupportedTableProperty(InProp))
	{
		DynamicDataTableUtilsImpl::GetPropertyValueAsStringDirect(InProp, InData, PPF_ExternalEditor, InDTExportFlags, Result);
	}

	return Result;
}

FString DynamicDataTableUtils::GetPropertyValueAsString(const FProperty* InProp, const uint8* InData, const EDynamicDataTableExportFlags InDTExportFlags)
{
	FString Result;

	if(InProp && IsSupportedTableProperty(InProp))
	{
		if(InProp->ArrayDim == 1)
		{
			DynamicDataTableUtilsImpl::GetPropertyValueAsString(InProp, InData, 0, PPF_ExternalEditor, InDTExportFlags, Result);
		}
		else
		{
			Result.AppendChar('(');

			for(int32 Index = 0; Index < InProp->ArrayDim; ++Index)
			{
				DynamicDataTableUtilsImpl::GetPropertyValueAsString(InProp, InData, Index, PPF_Delimited | PPF_ExternalEditor, InDTExportFlags, Result);

				if((Index + 1) < InProp->ArrayDim)
				{
					Result.AppendChar(',');
				}
			}

			Result.AppendChar(')');
		}
	}

	return Result;
}

FText DynamicDataTableUtils::GetPropertyValueAsTextDirect(const FProperty* InProp, const uint8* InData)
{
	FText Result;

	if(InProp && IsSupportedTableProperty(InProp))
	{
		FString ExportedString;
		DynamicDataTableUtilsImpl::GetPropertyValueAsStringDirect(InProp, InData, PPF_PropertyWindow, EDynamicDataTableExportFlags::UseJsonObjectsForStructs, ExportedString);

		Result = FText::FromString(MoveTemp(ExportedString));
	}

	return Result;
}

FText DynamicDataTableUtils::GetPropertyValueAsText(const FProperty* InProp, const uint8* InData)
{
	FText Result;

	if(InProp && IsSupportedTableProperty(InProp))
	{
		FString ExportedString;

		if(InProp->ArrayDim == 1)
		{
			DynamicDataTableUtilsImpl::GetPropertyValueAsString(InProp, InData, 0, PPF_PropertyWindow, EDynamicDataTableExportFlags::UseJsonObjectsForStructs, ExportedString);
		}
		else
		{
			ExportedString.AppendChar('(');

			for(int32 Index = 0; Index < InProp->ArrayDim; ++Index)
			{
				DynamicDataTableUtilsImpl::GetPropertyValueAsString(InProp, InData, Index, PPF_PropertyWindow | PPF_Delimited, EDynamicDataTableExportFlags::UseJsonObjectsForStructs, ExportedString);

				if((Index + 1) < InProp->ArrayDim)
				{
					ExportedString.AppendChar(',');
					ExportedString.AppendChar(' ');
				}
			}

			ExportedString.AppendChar(')');
		}
		
		Result = FText::FromString(MoveTemp(ExportedString));
	}

	return Result;
}

TArray<FName> DynamicDataTableUtils::GetStructPropertyNames(UStruct* InStruct)
{
	TArray<FName> PropNames;
	for (TFieldIterator<FProperty> It(InStruct); It; ++It)
	{
		PropNames.Add(It->GetFName());
	}
	return PropNames;
}

FName DynamicDataTableUtils::MakeValidName(const FString& InString)
{
	FString InvalidChars(INVALID_NAME_CHARACTERS);

	FString FixedString;
	TArray<TCHAR, FString::AllocatorType>& FixedCharArray = FixedString.GetCharArray();

	// Iterate over input string characters
	for(int32 CharIdx=0; CharIdx<InString.Len(); CharIdx++)
	{
		// See if this char occurs in the InvalidChars string
		FString Char = InString.Mid( CharIdx, 1 );
		if( !InvalidChars.Contains(Char) )
		{
			// Its ok, add to result
			FixedCharArray.Add(Char[0]);
		}
	}
	FixedCharArray.Add(0);

	return FName(*FixedString);
}

bool DynamicDataTableUtils::IsSupportedTableProperty(const FProperty* InProp)
{
	return( InProp &&
			(InProp->IsA(FIntProperty::StaticClass()) || 
			InProp->IsA(FNumericProperty::StaticClass()) ||
			InProp->IsA(FDoubleProperty::StaticClass()) ||
			InProp->IsA(FFloatProperty::StaticClass()) ||
			InProp->IsA(FNameProperty::StaticClass()) ||
			InProp->IsA(FStrProperty::StaticClass()) ||
			InProp->IsA(FBoolProperty::StaticClass()) ||
			InProp->IsA(FObjectPropertyBase::StaticClass()) ||
			InProp->IsA(FStructProperty::StaticClass()) ||
			InProp->IsA(FByteProperty::StaticClass()) ||
			InProp->IsA(FTextProperty::StaticClass()) ||
			InProp->IsA(FArrayProperty::StaticClass()) ||
			InProp->IsA(FSetProperty::StaticClass()) ||
			InProp->IsA(FMapProperty::StaticClass()) ||
			InProp->IsA(FEnumProperty::StaticClass()) ||
			InProp->IsA(FFieldPathProperty::StaticClass())
			)
		
		);
}

FString DynamicDataTableUtils::GetPropertyExportName(const FProperty* Prop, const EDynamicDataTableExportFlags InDTExportFlags)
{
	if (!ensure(Prop))
	{
		return FString();
	}
	return Prop->GetAuthoredName();
}

TArray<FString> DynamicDataTableUtils::GetPropertyImportNames(const FProperty* Prop)
{
	TArray<FString> Result;
	GetPropertyImportNames(Prop, Result);
	return Result;
}

void DynamicDataTableUtils::GetPropertyImportNames(const FProperty* Prop, TArray<FString>& OutResult)
{
	OutResult.Reset();
	if (ensure(Prop))
	{
		OutResult.AddUnique(Prop->GetName());
	}
	OutResult.AddUnique(GetPropertyExportName(Prop));
}

FText DynamicDataTableUtils::GetPropertyDisplayName(const FProperty* Prop, const FString& DefaultName)
{
#if WITH_EDITOR
	if (Prop)
	{
		return Prop->GetDisplayNameText();
	}
#endif // WITH_EDITOR
	return FText::FromString(DefaultName);
}

TArray<FString> DynamicDataTableUtils::GetColumnDataAsString(const UDynamicDataTable* InTable, const FName& PropertyName, const EDynamicDataTableExportFlags InDTExportFlags)
{
	TArray<FString> Result;
	if (!ensure(InTable))
	{
		return Result;
	}
	if (!ensure(PropertyName != NAME_None))
	{
		return Result;
	}

	FProperty* ColumnProperty = InTable->FindTableProperty(PropertyName);
	if (ColumnProperty)
	{
		for (auto RowIt = InTable->GetRowMap().CreateConstIterator(); RowIt; ++RowIt)
		{
			uint8* RowData = RowIt.Value();
			Result.Add(GetPropertyValueAsString(ColumnProperty, RowData, InDTExportFlags));
		}
	}

	return Result;
}
