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
#pragma warning( disable : 4996) //deprication incoming
#include "DynamicDatatableBPLibrary.h"

#include <fstream>

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
#include "Engine/DataTable.h"
#include <iostream>
//#include "CoreUObject/Public/UObject/UObjectBaseUtility.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/UnrealTypePrivate.h"
#include "Misc/CString.h"
#include "Misc/OutputDeviceNull.h"


UDynamicDatatableBPLibrary::UDynamicDatatableBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

namespace DynDTGlobals {
	TMap<FName,FDynamicTable> CachedTables = {};
}

void UDynamicDatatableBPLibrary::DeleteTableRow(FDynamicTable& Table, FName RowName) {
	Table.Map.Remove(RowName);
	Table.RowNames.Remove(RowName);
	Table.Sources.Remove(RowName);
	for (auto& each : Table.SourceRowNames) {
		each.Value.Remove(RowName);
	}
}

void UDynamicDatatableBPLibrary::DeleteTableRowAtIndex( UPARAM(ref) FDynamicTable& Table, int Index) {
	if (Index>=Table.RowNames.Num()) {return;}
	FName RowName = Table.RowNames[Index];
	Table.Map.Remove(RowName);
    Table.RowNames.Remove(RowName);
    Table.Sources.Remove(RowName);
    for (auto& each : Table.SourceRowNames) {
    	each.Value.Remove(RowName);
    }
}

void UDynamicDatatableBPLibrary::ClearTable(FDynamicTable& Table) {
	Table.Clear();
}

bool UDynamicDatatableBPLibrary::TableRowExists(FDynamicTable& Table, FName RowName) {
	return Table.Map.Contains(RowName);
}

TArray<FName> UDynamicDatatableBPLibrary::GetRowNamesFromTable(FDynamicTable& Table) {
	return Table.RowNames;
}

void UDynamicDatatableBPLibrary::AddRowsFromDataTable(FDynamicTable& Table, UDataTable* Source, bool Overwrite) {
	if (Source==nullptr) {return;}
	TArray<UDataTable*> sources = {};
	UCompositeDataTable* obj = dynamic_cast<UCompositeDataTable*>(Source);
	if (obj != nullptr) {
	//if (Source->IsA(UCompositeDataTable::StaticClass())) {
		UCompositeDataTable* cdt = Cast<UCompositeDataTable>(Source);
		FArrayProperty* ParentTablesProperty = FindFProperty<FArrayProperty>(cdt->StaticClass(), "ParentTables");
		if (ParentTablesProperty!=nullptr) {
			FScriptArrayHelper ParentTablesHelper(ParentTablesProperty, ParentTablesProperty->ContainerPtrToValuePtr<void>(cdt));
			for (int32 i = 0; i < ParentTablesHelper.Num(); i++) {
				if (FObjectProperty* InnerProp = CastField<FObjectProperty>(ParentTablesProperty->Inner)) {
					UObject* Object = InnerProp->GetObjectPropertyValue(ParentTablesHelper.GetRawPtr(i));
					UDataTable* table = Cast<UDataTable>(Object);
					if (table!=nullptr) {sources.Emplace(table);}
				}
			}
		}
	}
	else {
		sources.Emplace(Source);
	}
	for (UDataTable* each : sources) {
		//TMap<FName,uint8*> data = each->GetRowMap();
		if (!Table.SourceRowNames.Contains(each)) {
			TArray<FName> blank = {};
			Table.SourceRowNames.Emplace(each,blank);
		}
		UScriptStruct* rowStruct = each->RowStruct;
		Table.Type = each->RowStruct;
		for (auto rowIt = each->GetRowMap().CreateConstIterator(); rowIt; ++rowIt) {
			FName rowName = rowIt.Key();
			if (!Table.Map.Contains(rowName)||Overwrite) {
				//void* rowStructData = FMemory::Malloc(rowStruct->GetStructureSize());
				//rowStruct->InitializeStruct(rowStructData);
				//rowStruct->CopyScriptStruct(rowStructData, rowIt.Value());
				FString strdata = "";
				//StructToString(rowStruct,rowIt.Value(),strdata);
				//Table.Map.Emplace(rowName, MoveTemp(strdata));
				auto& elem = Table.Map.Emplace(rowName,FInstancedStruct());
				elem.InitializeAs(rowStruct, (uint8*)rowIt.Value());
				Table.RowNames.AddUnique(rowName);
				Table.Sources.Emplace(rowName,each);
				Table.SourceRowNames[each].Emplace(rowName);
			}
		}
	}
}

int UDynamicDatatableBPLibrary::GetSizeOfTable(FDynamicTable& Table) {
	return Table.Map.Num();
}

int UDynamicDatatableBPLibrary::GetSizeFromSourceTable(FDynamicTable& Table, UDataTable* Source) {
	if (Table.SourceRowNames.Contains(Source)) {
		return Table.SourceRowNames[Source].Num();
	}
	return 0;
}

TArray<FName> UDynamicDatatableBPLibrary::GetRowNamesFromSourceTable(FDynamicTable& Table, UDataTable* Source) {
	if (Table.SourceRowNames.Contains(Source)) {
		return Table.SourceRowNames[Source];
	}
	TArray<FName> res = {};
	return res;
}

FString UDynamicDatatableBPLibrary::ConvertTableToCsvString(FDynamicTable& Table) {
	FString res = "", FirstLine = "---", Line = "", str = "";
	UObject* Parent = nullptr;
	FString qsingle = "\"", qdouble = "\"\"";
	int i = 0;
	for (auto& each : Table.Map) {
		Line = each.Key.ToString();
		//FStructOnScope Scope(Table.Type);
		//StringToStruct(each.Value, Table.Type, Scope.GetStructMemory());

		for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
			FProperty* Property = *It;
			if (Property!=nullptr) {
				FObjectProperty* p = CastField<FObjectProperty>(Property);
				if (!p||(p&&p->IsValidLowLevel())) {
					if (i==0) {FirstLine += ","+Property->GetAuthoredName().TrimStartAndEnd();}
					for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
						//str = ""; Property->ExportText_Direct(str, Property->ContainerPtrToValuePtr<void>(each.Value), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
						//str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(each.Value), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
						
						//str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(Scope.GetStructMemory()), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
						str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(each.Value.GetMutableMemory()), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
						Line += ",\""+str.Replace(*qsingle,*qdouble)+"\"";;
					}
				}
			}
			//Line += ",\""+each.Value+"\""; //.Replace(*qsingle,*qdouble);
		}
		if (i==0) {res = FirstLine;}
		res += "\n"+MoveTemp(Line); Line = "";
		++i;
	}
	return res;
}

void UDynamicDatatableBPLibrary::AddRowsFromCsvString(FDynamicTable& Table, FString& Csv) {
	if (Csv==""||Table.Type==nullptr) {return;}
	/*TArray<FString> Lines = {}, Entries = {}, Columns = {}; Csv.ParseIntoArray(Lines,TEXT("\n"),true);
	int i = 0, x = 0, cl = 0;
	bool inQuotes = false;
	FString str = "", qsingle = "\"", qdouble = "\"\"";
	FString sc = ",", sl = "(", sr = ")";
	FString qc = "\",\"", ql = "(\"", qr = "\")";
	TMap<FString,FProperty*> Props = {};
	for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
		FProperty* Property = *It;
		if (Property!=nullptr) {
			Props.Emplace(Property->GetAuthoredName().TrimStartAndEnd(), Property);
		}
	}
	for (auto& line : Lines) {
		str = "";
		Entries = {};
		inQuotes = false;
		for (x=0; x<line.Len(); ++x) {
			auto& linex = line[x];
			if (linex=='"') {inQuotes = !inQuotes;}
			else if (linex==','&&!inQuotes) {
				Entries.Add(MoveTemp(str));
				str = "";
			}
			else {str += linex;}
		}
		Entries.Add(MoveTemp(str));
		x=0;
		if (i==0) {
			//header
			for (auto& entry : Entries) {
				if (x>0) {//skip first, it's just ---
					Columns.Emplace(entry);
					++cl;
				}
				++x;
			}
		}
		else if (Entries.Num()>0) {
			Table.Map.Emplace(FName(*Entries[0]),FMemory::Malloc(Table.Type->GetStructureSize()));
			Table.Type->InitializeStruct(Table.Map[FName(*Entries[0])]);
			x=1; //skip rowname
			for (auto& column : Columns) {
				if (Props.Contains(column)) {
					FProperty* Property = Props[column];
					if (x<Entries.Num()) {
						FArrayProperty* ArrayProperty = Cast<FArrayProperty>(Property);
						//Entries[x].ReplaceInline(*qdouble,*qsingle);
						if (ArrayProperty!=nullptr) {
							Entries[x] = Entries[x].Replace(*sl,*ql);
							Entries[x] = Entries[x].Replace(*sc,*qc);
							Entries[x] = Entries[x].Replace(*sr,*qr);
						}
						for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
							Property->ImportText_Direct(*Entries[x], Property->ContainerPtrToValuePtr<void>(Table.Map[FName(*Entries[0])]), EPropertyPortFlags::PPF_ExternalEditor, nullptr);
						}
					}
					++x;
				}
			}
		}
		++i;
	}
	*/
	// easy hack
	UDataTable* dt = NewObject<UDataTable>();
	dt->RowStruct = Table.Type;
	dt->CreateTableFromCSVString(Csv);
	AddRowsFromDataTable(Table,dt);
	dt = nullptr;
}

bool UDynamicDatatableBPLibrary::TableHasAnyType(FDynamicTable& Table) {
	return (Table.Type!=nullptr);
}

TArray<FName> UDynamicDatatableBPLibrary::GetRowNamesFromSourceTableName(FDynamicTable& Table, FString SourceName) {
	for (auto& each : Table.SourceRowNames) {
		if (each.Key->GetName()==SourceName) {
			return each.Value;
		}
	}
	TArray<FName> res = {};
	return res;
}

TArray<UDataTable*> UDynamicDatatableBPLibrary::GetSourceTables(FDynamicTable& Table) {
	TArray<UDataTable*> res = {};
	for (auto& each : Table.SourceRowNames) {
		res.Emplace(each.Key);
	}
	return res;
}

void UDynamicDatatableBPLibrary::AppendTable(FDynamicTable& TableA, FDynamicTable& TableB, OverwriteRowBehavior OverwriteExisting) {
	FName rowname = TEXT("");
	for (auto& each : TableB.RowNames) {
		if (TableA.Map.Contains(each)) {
			if (OverwriteExisting==OverwriteRowBehavior::E_Increment) {
				rowname = IncrementRowNum(rowname);
				while(TableA.Map.Contains(rowname)) {
					rowname = IncrementRowNum(rowname);
				}
			}
			else if (OverwriteExisting==OverwriteRowBehavior::E_Skip) {
				goto skip;
			}
			else if (OverwriteExisting==OverwriteRowBehavior::E_Overwrite) {
				if (TableA.RowNames.Contains(each)) {//update existing
					TableA.Map.Emplace(each, TableB.Map[each]);
					UDataTable* Source = TableB.Sources[each];
					if (Source!=nullptr) {
						if (!TableA.SourceRowNames.Contains(Source)) {
							TArray<FName> blank = {};
							TableA.SourceRowNames.Emplace(Source,blank);
						}
						TableA.SourceRowNames[Source].AddUnique(each);
					}
					TableA.Sources.Emplace(each,Source);
				}
				TableA.Type = TableB.Type;
				goto skip;
			}
		}
		else {rowname = each;}
		if (TableB.Map.Contains(each)) {
			TableA.Map.Emplace(rowname, TableB.Map[each]);
			TableA.RowNames.AddUnique(rowname);
			if (TableB.Sources.Contains(each)) {
				UDataTable* Source = TableB.Sources[each];
				if (Source!=nullptr) {
					if (!TableA.SourceRowNames.Contains(Source)) {
						TArray<FName> blank = {};
						TableA.SourceRowNames.Emplace(Source,blank);
					}
					TableA.SourceRowNames[Source].Emplace(rowname);
					TableA.Sources.Emplace(rowname,Source);
				}
				TableA.Sources.Emplace(rowname,nullptr);
			}
			TableA.Type = TableB.Type;
		}
		skip:;
	}
	return;
}

FName UDynamicDatatableBPLibrary::IncrementRowNum(FName Row) {
	FString str = Row.ToString();
	int32 i = str.FindLastCharByPredicate([](TCHAR ch) {return FChar::IsDigit(ch);});
	if (i<=0) {return FName(str+"_0");}
	FString digits = str.RightChop(i);
	int64 number = FCString::Strtoui64(*digits, NULL, 10); ++number;
	char tmp[21];
	sprintf_s(tmp, "%llu", number);
	FString res = tmp;
	return FName(str.Left(i) + res);
}

bool UDynamicDatatableBPLibrary::WriteStringToFile(FString Path, FString FileName, FString& SaveText, bool Overwrite) {
	if(!FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Path)) {return false;}
	Path += FileName;
	if (!Overwrite) {if (FPlatformFileManager::Get().GetPlatformFile().FileExists( * Path)) {return false;}}
	return FFileHelper::SaveStringToFile(SaveText, *Path);
}

bool UDynamicDatatableBPLibrary::ReadStringFromFile(FString Path, FString FileName, FString& OutStr) {
	FString FullPath = Path+FileName;
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {
		return false;
	}
	FFileHelper::LoadFileToString(OutStr, *FullPath);
	return true;
}

bool UDynamicDatatableBPLibrary::SaveTableAsCsvFile(FDynamicTable& Table, FString Path, FString FileName) {
	if (!FileName.EndsWith(".csv")) {FileName+=".csv";}
	FString res = ConvertTableToCsvString(Table);
	return WriteStringToFile(Path, FileName, res, true);
}

void UDynamicDatatableBPLibrary::LoadTableFromCsvFile(FDynamicTable& Table, FString Path, FString FileName) {
	if (!FileName.EndsWith(".csv")) {FileName+=".csv";}
    FString FullPath = Path+FileName;
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {return;}
	FString str;
	ReadStringFromFile(Path, FileName, str);
	AddRowsFromCsvString(Table,str);
}

FString UDynamicDatatableBPLibrary::LoadCsvStringFromFile(FString Path, FString FileName) {
	if (!FileName.EndsWith(".csv")) {FileName+=".csv";}
	FString FullPath = Path+FileName;
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {return "";}
	FString res;
	ReadStringFromFile(Path, FileName, res);
	return res;
}

bool UDynamicDatatableBPLibrary::SaveTable(UObject* WorldContextObject, FDynamicTable& Table, FString Path, FString FileName) {
	int idx = FileName.Find(".");
	if (idx>=0) {FileName = FileName.Left(idx);}
	if (!FileName.EndsWith(".dyntable")) {FileName+=".dyntable";}
	
	FString rstr = ConvertTableToCsvString(Table);
	rstr += "\n[--Table Info--]\n";

	FString res = "", FirstLine = "", Line = "", str = "";
	UObject* Parent = nullptr;
	FString qsingle = "\"", qdouble = "\"\"";
	int i = 0;
	
	for (TFieldIterator<FProperty> It(Table.StaticStruct()); It; ++It) {
		FProperty* Property = *It;
		if (Property!=nullptr) {
			FString propname = Property->GetAuthoredName().TrimStartAndEnd();
			if (propname!="Map") {
				if (i==0) {FirstLine += ","+propname;}
				for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
					str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(&Table), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
					Line += ",\""+str.Replace(*qsingle,*qdouble)+"\"";
				}
			}
		}
	}
	if (i==0) {res = FirstLine.RightChop(1);}
	res += "\n"+MoveTemp(Line).RightChop(1); Line = "";
	
	rstr += res;
	return WriteStringToFile(Path, FileName, rstr, true);
}

FDynamicTable UDynamicDatatableBPLibrary::LoadTable(UObject* WorldContextObject, FString Path, FString FileName, bool& Success) {
	int idx = FileName.Find(".");
	if (idx>=0) {FileName = FileName.Left(idx);}
	if (!FileName.EndsWith(".dyntable")) {FileName+=".dyntable";}
	FString FullPath = Path+FileName;
	Success = false;
	FDynamicTable Table;
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {return Table;}
	FString str;
	ReadStringFromFile(Path, FileName, str);
	FString left = "", right = "", right2 = "";
	str.Split("\n[--Table Info--]\n", &left, &right2, ESearchCase::IgnoreCase);
	TMap<FString,FProperty*> Props = {};
	bool hasSources = false;
	int x = 0, sourceIndex = 0;
	for (TFieldIterator<FProperty> It(Table.StaticStruct()); It; ++It) {
		FProperty* Property = *It;
		if (Property!=nullptr) {
			FString propname = Property->GetAuthoredName().TrimStartAndEnd();
			if (propname!="Map") {
				Props.Emplace(propname, Property);
				if (propname=="Sources") {hasSources = true; sourceIndex = x;}
				++x;
			}
		}
	}
	FString tstr = "", rhead = "";
	TArray<FString> Entries = {};
	bool inQuotes = false;
	right2.Split("\n",&rhead, &right);
	for (x=0; x<right.Len(); ++x) {
		auto& rightx = right[x];
		if (rightx=='"') {inQuotes = !inQuotes;}
		else if (rightx==','&&!inQuotes) {
			Entries.Emplace(MoveTemp(tstr));
			tstr = "";
		}
		else {tstr += rightx;}
	}
	Entries.Add(MoveTemp(tstr));
	Table.Type = FindObject<UScriptStruct>(nullptr, *Entries.Last());
	AddRowsFromCsvString(Table,left);
	Success = (Table.RowNames.Num()>0);
	if (hasSources) {
		Props["Sources"]->ImportText_Direct(*Entries[sourceIndex], Props["Sources"]->ContainerPtrToValuePtr<void>(&Table), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		RecreateMissingData(Table);
	}
	return Table;
}

void UDynamicDatatableBPLibrary::RecreateMissingData(FDynamicTable& Table) {
	Table.RowNames = {};
	for (auto& each : Table.Map) {
		if (!Table.Sources.Contains(each.Key)) {
			Table.Sources.Emplace(each.Key,nullptr);
		}
		Table.RowNames.Emplace(FName(each.Key));
	}
	TArray<FName> blank = {};
	Table.SourceRowNames = {};
	for (auto& each : Table.Sources) {
		if (each.Value!=nullptr) {
			if (!Table.SourceRowNames.Contains(each.Value)) {Table.SourceRowNames.Emplace(each.Value,blank);}
			Table.SourceRowNames[each.Value].Emplace(FName(each.Key));
		}
	}
}

double UDynamicDatatableBPLibrary::EvaluateMath(FString Input, TMap<FName,double>& Variables) {
	if (Input=="") {return 0.0f;}
	FString left = "", right = "", tstr = "";
	bool inside = false;
	int x = 0, start = 0, end = 0;
	redo2:;
	start = 0;
	tstr = "";
	for (x=0; x<Input.Len(); ++x) {
		auto& inputx = Input[x];
		if (IsOp(inputx)) {
			if (tstr.Len()>0&&Variables.Contains(FName(tstr))) {
				left = Input.Left(start);
				right = Input.RightChop(x);
				Input = left + FString::SanitizeFloat(Variables[FName(tstr)]) + right;
				//{FString str = Input; UE_LOG(LogTemp, Log, TEXT("%s"), *str);}
				goto redo2;
			}
			else {tstr = ""; start = x+1;}
		}
		else {
			tstr += inputx;
		}
	}
	if (Input.Contains("(")) {
		redo:;
		if (Input.Contains("(")) {
			tstr = "";
			inside = false;
			x = 0, start = 0, end = 0;
			for (x=0; x<Input.Len(); ++x) {
				auto& inputx = Input[x];
				if (inputx=='(') {inside = true; start = x; tstr = "";}
				else if (inputx==')') {
					inside = false;
					left = Input.Left(start);
					right = Input.RightChop(x+1);
					Input = left + FString::Printf(TEXT("%f"), EvaluateMathSimple(tstr)) + right;
					//{FString str = tstr; UE_LOG(LogTemp, Log, TEXT("%s"), *str);}
					tstr = "";
					goto redo;
				}
				else {tstr += inputx;}
			}
		}
	}
	return EvaluateMathSimple(Input);
}

double UDynamicDatatableBPLibrary::EvaluateMathNoVariables(FString Input) {
	if (Input=="") {return 0.0f;}
	FString left = "", right = "", tstr = "";
	bool inside = false;
	int x = 0, start = 0, end = 0;
	if (Input.Contains("(")) {
		redo:;
		if (Input.Contains("(")) {
			tstr = "";
			inside = false;
			x = 0, start = 0, end = 0;
			for (x=0; x<Input.Len(); ++x) {
				auto& inputx = Input[x];
				if (inputx=='(') {inside = true; start = x; tstr = "";}
				else if (inputx==')') {
					inside = false;
					left = Input.Left(start);
					right = Input.RightChop(x+1);
					Input = left + FString::Printf(TEXT("%f"), EvaluateMathSimple(tstr)) + right;
					//{FString str = tstr; UE_LOG(LogTemp, Log, TEXT("%s"), *str);}
					tstr = "";
					goto redo;
				}
				else {tstr += inputx;}
			}
		}
	}
	return EvaluateMathSimple(Input);
}

double UDynamicDatatableBPLibrary::EvaluateMathSimple(const FString& input) {
	TArray<double> operandStack;
	TArray<char> operatorStack;
	TMap<char, int> precedence{{'+', 1},{'-', 1},{'*', 2},{'/', 2},{'^', 3}};
	TMap<char, int> associativity{{'+', 1},{'-', 1},{'*', 1},{'/', 1},{'^', 2}};
	for (int i = 0; i < input.Len(); i++) {
		char c = input[i];
		if (isdigit(c) || c == '.') {
			FString operand;
			while (i < input.Len() && (isdigit(input[i]) || input[i] == '.')) {
				operand += input[i++];
			}
			--i;
			operandStack.Emplace(FCString::Atof(*operand));
		} else if (precedence.Contains(c)) {
			while (operatorStack.Num() > 0 && (precedence[c] < precedence[operatorStack.Top()] ||
			(precedence[c] == precedence[operatorStack.Top()] && associativity[c] == 1))) {
				double rightOperand = 0.0f;
				double leftOperand = 0.0f;
				if (operandStack.Num() > 1) {
					rightOperand = operandStack.Pop();
					leftOperand = operandStack.Pop();
				} else {
					goto skip;
				}
				char op = operatorStack.Pop();
				operandStack.Emplace(ApplyOperation(leftOperand, rightOperand, op));
			}
			skip:;
			operatorStack.Emplace(c);
		}
	}
	while (operatorStack.Num() > 0) {
		if (operandStack.Num() > 1) {
			double rightOperand = operandStack.Pop();
			double leftOperand = operandStack.Pop();
			char op = operatorStack.Pop();
			operandStack.Emplace(ApplyOperation(leftOperand, rightOperand, op));
		}
	}
	return operandStack.Top();
}

double UDynamicDatatableBPLibrary::ApplyOperation(double leftOperand, double rightOperand, char op) {
	switch (op) {
		case '+':
			return leftOperand + rightOperand;
		case '-':
			return leftOperand - rightOperand;
		case '*':
			return leftOperand * rightOperand;
		case '/':
			if (rightOperand!=0.0f) {return leftOperand / rightOperand;}
			else {return 9999999999.0f;}
		case '^':
			return pow(leftOperand, rightOperand);
		default:
			return 0;
	}
}

bool UDynamicDatatableBPLibrary::IsOp(char op) {
	switch (op) {
	case '+':
		return true;
	case '-':
		return true;
	case '*':
		return true;
	case '/':
		return true;
	case '^':
		return true;
	case '(':
		return true;
	case ')':
		return true;
	default:
		return false;
	}
}

FString UDynamicDatatableBPLibrary::GetPropertyOfTableRowAsString(FDynamicTable& Table, FName RowName, FName PropertyName) {
	if (Table.Type==nullptr) {return "";}
	TMap<FString,FProperty*> Props = {};
	FProperty* Property = nullptr;
	for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
		Property = *It;
		if (Property!=nullptr&&Property->GetAuthoredName().TrimStartAndEnd()==PropertyName.ToString()) {goto found;}
	}
	return ""; //didn't have property
	found:;
	FString res = "";
	if (Table.Map.Contains(RowName)) {
		//FStructOnScope Scope(Table.Type);
		//StringToStruct(Table.Map[RowName], Table.Type, Scope.GetStructMemory());
		//Property->ExportTextItem_Direct(res, Property->ContainerPtrToValuePtr<void>(Scope.GetStructMemory()), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
		Property->ExportTextItem_Direct(res, Property->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
		//GetPropFromString(Table.Map[RowName],PropertyName.ToString(),res);
	}

	
	return res;
}

bool UDynamicDatatableBPLibrary::SetPropertyOfTableRowAsString(FDynamicTable& Table, FName RowName, FName PropertyName, FString Value) {
	if (Table.Type==nullptr) {return false;}
	TMap<FString,FProperty*> Props = {};
	FProperty* Property = nullptr;
	for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
		Property = *It;
		if (Property!=nullptr&&Property->GetAuthoredName().TrimStartAndEnd()==PropertyName.ToString()) {goto found;}
	}
	return false; //didn't have property
	found:;
	if (Table.Map.Contains(RowName)) {
		
		//FStructOnScope Scope(Table.Type);
		//StringToStruct(Table.Map[RowName], Table.Type, Scope.GetStructMemory());
		//SetPropInString(Table.Map[RowName],PropertyName.ToString(),Value);
		//Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Scope.GetStructMemory()), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		//StructToString(Table.Type,Scope.GetStructMemory(),Table.Map[RowName]);
		return true;
	}
	return false;
}

bool UDynamicDatatableBPLibrary::IncrementPropertyOfTableRow(FDynamicTable& Table, FName RowName, FName PropertyName, int Value) {
	if (Table.Type==nullptr) {return false;}
	TMap<FString,FProperty*> Props = {};
	FProperty* Property = nullptr;
	for (TFieldIterator<FProperty> It(Table.Type); It; ++It) {
		Property = *It;
		if (Property!=nullptr&&Property->GetAuthoredName().TrimStartAndEnd()==PropertyName.ToString()) {goto found;}
	}
	return false; //didn't have property
	found:;
	FString res = "";
	if (Table.Map.Contains(RowName)) {
		if (Property->IsA(FIntProperty::StaticClass())) {
			auto& Struct = Table.Map[RowName];
			FProperty* Prop = Table.Type->FindPropertyByName(PropertyName);
			if (FIntProperty* IntProp = CastField<FIntProperty>(Property)) {
				if (int32* ValuePtr = IntProp->GetPropertyValuePtr_InContainer(Struct.GetMutableMemory())) {
					*ValuePtr += Value;
				}
			}
			return true;
		}
		//FStructOnScope Scope(Table.Type);
		//StringToStruct(Table.Map[RowName], Table.Type, Scope.GetStructMemory());
		//GetPropFromString(Table.Map[RowName],PropertyName.ToString(),res);
		//Property->ExportTextItem_Direct(res, Property->ContainerPtrToValuePtr<void>(Scope.GetStructMemory()), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
		Property->ExportTextItem_Direct(res, Property->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
		if (Property->IsA(FIntProperty::StaticClass())||Property->IsA(FInt16Property::StaticClass())||Property->IsA(FInt64Property::StaticClass())||Property->IsA(FInt8Property::StaticClass())||
			Property->IsA(FUInt32Property::StaticClass())||Property->IsA(FUInt64Property::StaticClass())||Property->IsA(FUInt16Property::StaticClass())) {
			int64 tmp = FCString::Atoi64(*res);
			tmp+=Value;
			char tmpc[21];
			sprintf_s(tmpc, "%llu", tmp);
			res = tmpc;
		}
		if (Property->IsA(FDoubleProperty::StaticClass())||Property->IsA(FFloatProperty::StaticClass())) {
			double tmp = FCString::Atod(*res);
			tmp+=Value;
			char tmpc[21];
			sprintf_s(tmpc, "%f", tmp);
			res = tmpc;
		}
		if (Property->IsA(FStrProperty::StaticClass())||Property->IsA(FNameProperty::StaticClass())) {
			int32 i = res.FindLastCharByPredicate([](TCHAR ch) {return FChar::IsDigit(ch);});
			if (i<=0) {res+="_0";}
			else {
				FString digits = res.RightChop(i);
				int64 tmp = FCString::Strtoui64(*digits, NULL, 10);
				tmp += Value;
				char tmpc[21];
				sprintf_s(tmpc, "%llu", tmp);
				FString res2 = tmpc;
				res = (res.Left(i) + res2);
			}
		}
		//SetPropInString(Table.Map[RowName],PropertyName.ToString(),res);
		
		//FStructOnScope Scope2(Table.Type);
		//StringToStruct(Table.Map[RowName], Table.Type, Scope2.GetStructMemory());
		//Property->ImportText_Direct(*res, Property->ContainerPtrToValuePtr<void>(Scope2.GetStructMemory()), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		//StructToString(Table.Type,Scope2.GetStructMemory(),Table.Map[RowName]);
		Property->ImportText_Direct(*res, Property->ContainerPtrToValuePtr<void>(Table.Map[RowName].GetMutableMemory()), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		return true;
	}
	return false;
}

void UDynamicDatatableBPLibrary::Async_SaveTableAsCsvFile(FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncSaveCsvDelegate Completed) {
	AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, [&Table, AsyncID, Path, FileName, Completed]() {
		FString tFileName = FileName;
		if (!tFileName.EndsWith(".csv")) {tFileName+=".csv";}
		FString res = ConvertTableToCsvString(Table);
		bool success = WriteStringToFile(Path, tFileName, res, true);
		AsyncTask(ENamedThreads::GameThread, [Completed, AsyncID, success]() {
			Completed.ExecuteIfBound(AsyncID,success);
		});
	});
}

void UDynamicDatatableBPLibrary::Async_LoadTableFromCsvFile(FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncLoadCsvDelegate Completed) {
	AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, [&Table, AsyncID, Path, FileName, Completed]() {
		bool success = false;
		FString tFileName = FileName;
		if (!tFileName.EndsWith(".csv")) {tFileName+=".csv";}
		FString FullPath = Path+tFileName;
		FString str = "";
		if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {goto done;}
		ReadStringFromFile(Path, tFileName, str);
		AddRowsFromCsvString(Table,str);
		success = true;
		done:;
		AsyncTask(ENamedThreads::GameThread, [Completed, AsyncID, success]() {
			Completed.ExecuteIfBound(AsyncID,success);
		});
	});
}

void UDynamicDatatableBPLibrary::Async_SaveTable(UObject* WorldContextObject, FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncSaveCsvDelegate Completed) {
	AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, [WorldContextObject, &Table, AsyncID, Path, FileName, Completed]() {
		bool success = SaveTable(WorldContextObject, Table,Path,FileName);
		AsyncTask(ENamedThreads::GameThread, [Completed, AsyncID, success]() {
			Completed.ExecuteIfBound(AsyncID,success);
		});
	});
}

void UDynamicDatatableBPLibrary::Async_LoadTable(UObject* WorldContextObject, FDynamicTable& Table, FName AsyncID, FString Path, FString FileName, FAsyncLoadCsvDelegate Completed) {
	AsyncTask(ENamedThreads::AnyHiPriThreadNormalTask, [WorldContextObject, &Table, AsyncID, Path, FileName, Completed]() {
		int tl = Table.RowNames.Num();
		bool s = false;
		FDynamicTable tTable = LoadTable(WorldContextObject, Path, FileName, s);
		bool success = (tTable.RowNames.Num()>=tl); //don't delete table if something went wrong
		if (success) {Table = MoveTemp(tTable);}
		AsyncTask(ENamedThreads::GameThread, [Completed, AsyncID, success]() {
			Completed.ExecuteIfBound(AsyncID,success);
		});
	});
}

FTableDiffResult UDynamicDatatableBPLibrary::DiffTables(FDynamicTable& Table1, FDynamicTable& Table2, bool StopEarly, bool& DiffRowNames, bool& SharedRowsIdentical) {
	FTableDiffResult res;
	res.SameType = (Table1.Type==Table2.Type);
	for (auto& each : Table1.Map) {
		if (Table2.Map.Contains(each.Key)) {
			res.SharedRows.Emplace(each.Key);
		}
		else {
			res.RowsNotInTable2.Emplace(each.Key); DiffRowNames = true; if (StopEarly) {return res;}
		}
	}
	for (auto& each : Table2.Map) {
		if (!Table1.Map.Contains(each.Key)) {
			res.RowsNotInTable1.Emplace(each.Key); DiffRowNames = true; if (StopEarly) {return res;}
		}
	}
	if (res.SameType) {
		for (auto& each : res.SharedRows) {
			if (Table1.Map[each]==Table2.Map[each]) {
				res.IdenticalRows.Emplace(each);
			}
			else {
				res.DifferentRows.Emplace(each); if (StopEarly) {SharedRowsIdentical = false; return res;}
			}
		}
	}
	SharedRowsIdentical = (res.SameType&&res.DifferentRows.Num()==0);
	return res;
}

FString UDynamicDatatableBPLibrary::Conv_TableDiffResultToString(FTableDiffResult Diff) {
	FString res = "";
	if (Diff.SameType) {res += "Same Base Type.\n";}
	else {res += "Different Base Types.\n";}
	res += "Rows not in Table1 ("+FString::FromInt(Diff.RowsNotInTable1.Num())+"): (";
	for (auto& each : Diff.RowsNotInTable1) {
		res += each.ToString()+",";
	}
	if (Diff.RowsNotInTable1.Num()>0) {res = res.LeftChop(1);}
	res += ")\n";
	res += "Rows not in Table2 ("+FString::FromInt(Diff.RowsNotInTable2.Num())+"): (";
	for (auto& each : Diff.RowsNotInTable2) {
		res += each.ToString()+",";
	}
	if (Diff.RowsNotInTable2.Num()>0) {res = res.LeftChop(1);}
	res += ")\n";
	res += "Shared Rows ("+FString::FromInt(Diff.SharedRows.Num())+"): (";
	for (auto& each : Diff.SharedRows) {
		res += each.ToString()+",";
	}
	if (Diff.SharedRows.Num()>0) {res = res.LeftChop(1);}
	res += ")\n";
	res += "Identical Rows ("+FString::FromInt(Diff.IdenticalRows.Num())+"): (";
	for (auto& each : Diff.IdenticalRows) {
		res += each.ToString()+",";
	}
	if (Diff.IdenticalRows.Num()>0) {res = res.LeftChop(1);}
	res += ")\n";
	res += "Different Rows ("+FString::FromInt(Diff.DifferentRows.Num())+"): (";
	for (auto& each : Diff.DifferentRows) {
		res += each.ToString()+",";
	}
	if (Diff.DifferentRows.Num()>0) {res = res.LeftChop(1);}
	res += ")\n";
	if (!Diff.SameType) {
		res = "Verdict: Tables are different.\n\n" + res;
	}
	else if (Diff.RowsNotInTable1.Num()==0&&Diff.RowsNotInTable2.Num()==0&&Diff.DifferentRows.Num()==0) {
		res = "Verdict: Tables are identical.\n\n" + res;
	}
	else if (Diff.DifferentRows.Num()==0) {
		res = "Verdict: Tables are identical but have different RowNames.\n\n" + res;
	}
	else {
		res = "Verdict: Tables are different.\n\n" + res;
	}
	return res;
}

uint8 UDynamicDatatableBPLibrary::GetSourceIndex(FDynamicTable& Table, UDataTable* Source, int& Index) {
	if (Source==nullptr) {Index = -1; return 0;}
	int x = 0;
	for (auto& each : Table.SourceRowNames) {
		if (each.Key==Source) {
			Index = x;
			return x;
		}
		++x;
	}
	Index = -1;
	return 0;
}

FName UDynamicDatatableBPLibrary::MakeIndexedRowNameFromXY(FName Base, int X, int Y) {
	FString str = Base.ToString() + "_" + FString::FromInt(X) + "_"+ FString::FromInt(Y);
	return FName(str);
}

void UDynamicDatatableBPLibrary::MakeXYFromIndexedRowName(FName RowName, int& X, int& Y) {
	FString str = RowName.ToString();
	int idx = -1, idx2 = -1, x = 0, l = str.Len()-1, i = 0;
	for (x=l; x>=0; --x) {
		char c = str[x];
		if (c == '_') {
			if (idx2<0) {idx2 = x;}
			else {idx = x; break;}
		}
	}
	if (idx<0||idx2<0) {X = 0; Y = 0; return;}
	FString first = str.Mid(idx+1,(idx2-idx)-1), second = str.RightChop(idx2+1);
	X = FCString::Atoi(*first);
	Y = FCString::Atoi(*second);
}

void UDynamicDatatableBPLibrary::SaveCachedTables(FString Path, FString FileName) {
	FDynSaver saver;
	saver.CachedStrings = {};
	for (auto& each : DynDTGlobals::CachedTables) {
		saver.CachedStrings.Emplace(each.Key,SaveTableString(each.Value));
	}
	TArray<uint8> bytes = {};
	FString FullPath = Path+FileName;
	FMemoryWriter MemWriter(bytes);
	FObjectAndNameAsStringProxyArchive Ar(MemWriter, true);
	saver.StaticStruct()->SerializeItem(Ar, &saver, nullptr);
	FFileHelper::SaveArrayToFile(bytes,*FullPath);
}

void UDynamicDatatableBPLibrary::LoadCachedTables(FString Path, FString FileName) {
	FString FullPath = Path+FileName;
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {return;}
	TArray<uint8> bytes = {};
	FFileHelper::LoadFileToArray(bytes, *FullPath);
	FMemoryReader reader(bytes);
	FObjectAndNameAsStringProxyArchive Ar(reader, true);
	FDynSaver saver;
	saver.StaticStruct()->SerializeItem(Ar, &saver, nullptr);
	DynDTGlobals::CachedTables = {};
	FDynamicTable blank;
	for (auto& each : saver.CachedStrings) {
		DynDTGlobals::CachedTables.Emplace(each.Key,blank);
		LoadTableString(DynDTGlobals::CachedTables[each.Key],each.Value);
	}
}

FDynamicTable UDynamicDatatableBPLibrary::RetrieveCachedTable(FName TableName, bool& Exists) {
	if (TableName.IsNone()) {Exists = false; FDynamicTable res;	res.CacheName = TableName; return res;}
	Exists = DynDTGlobals::CachedTables.Contains(TableName);
	if (Exists) {
		DynDTGlobals::CachedTables[TableName].CacheName = TableName;
		return DynDTGlobals::CachedTables[TableName];
	}
	FDynamicTable res;
	res.CacheName = TableName;
	return res;
}

void UDynamicDatatableBPLibrary::StoreCachedTable(FName TableName, FDynamicTable& Table) {
	if (TableName.IsNone()) {//use cache name if none
		if (!Table.CacheName.IsNone()) {TableName = Table.CacheName;}
		else {return;}
	}
	DynDTGlobals::CachedTables.Emplace(TableName,Table);
	Table.CacheName = TableName;
}

void UDynamicDatatableBPLibrary::RemoveCachedTable(FName TableName) {
	DynDTGlobals::CachedTables.Remove(TableName);
}

void UDynamicDatatableBPLibrary::RemoveTableFromCache(FDynamicTable& Table) {
	if (Table.CacheName.IsNone()) {return;}
	DynDTGlobals::CachedTables.Remove(Table.CacheName);
}

bool UDynamicDatatableBPLibrary::DoesCachedTableExist(FName TableName) {
	return DynDTGlobals::CachedTables.Contains(TableName);
}

bool UDynamicDatatableBPLibrary::IsTableCached(FDynamicTable& Table) {
	if (Table.CacheName.IsNone()) {return false;}
	return DynDTGlobals::CachedTables.Contains(Table.CacheName);
}

bool UDynamicDatatableBPLibrary::IsCachedTableUpToDate(FDynamicTable& Table) {
	if (!DynDTGlobals::CachedTables.Contains(Table.CacheName)) {return false;}
	bool DiffRowNames = false, SharedRowsIdentical = false;
	FTableDiffResult result = DiffTables(Table, DynDTGlobals::CachedTables[Table.CacheName], true, DiffRowNames, SharedRowsIdentical);
	return (!DiffRowNames&&SharedRowsIdentical);
}

TMap<FName,FDynamicTable> UDynamicDatatableBPLibrary::GetEntireCachedTablesMap() {
	return DynDTGlobals::CachedTables;
}

TArray<FName> UDynamicDatatableBPLibrary::GetCachedTableNames() {
	TArray<FName> res = {};
	for (auto& each : DynDTGlobals::CachedTables) {
		res.Emplace(each.Key);
	}
	return MoveTemp(res);
}

FString UDynamicDatatableBPLibrary::SaveTableString(FDynamicTable& Table) {
	FString rstr = ConvertTableToCsvString(Table);
	rstr += "\n[--Table Info--]\n";
	FString res = "", FirstLine = "", Line = "", str = "";
	UObject* Parent = nullptr;
	FString qsingle = "\"", qdouble = "\"\"";
	int i = 0;
	for (TFieldIterator<FProperty> It(Table.StaticStruct()); It; ++It) {
		FProperty* Property = *It;
		if (Property!=nullptr) {
			FString propname = Property->GetAuthoredName().TrimStartAndEnd();
			if (propname!="Map") {
				if (i==0) {FirstLine += ","+propname;}
				for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ArrayIndex++) {
					str = ""; Property->ExportTextItem_Direct(str, Property->ContainerPtrToValuePtr<void>(&Table), nullptr, Parent, EPropertyPortFlags::PPF_ExternalEditor);
					Line += ",\""+str.Replace(*qsingle,*qdouble)+"\"";
				}
			}
		}
	}
	if (i==0) {res = FirstLine.RightChop(1);}
	res += "\n"+MoveTemp(Line).RightChop(1); Line = "";
	rstr += res;
	return MoveTemp(rstr);
}

void UDynamicDatatableBPLibrary::LoadTableString(FDynamicTable& Table, FString& String) {
	FString left = "", right = "", right2 = "";
	String.Split("\n[--Table Info--]\n", &left, &right2, ESearchCase::IgnoreCase);
	TMap<FString,FProperty*> Props = {};
	bool hasSources = false;
	int x = 0, sourceIndex = 0;
	for (TFieldIterator<FProperty> It(Table.StaticStruct()); It; ++It) {
		FProperty* Property = *It;
		if (Property!=nullptr) {
			FString propname = Property->GetAuthoredName().TrimStartAndEnd();
			if (propname!="Map") {
				Props.Emplace(propname, Property);
				if (propname=="Sources") {hasSources = true; sourceIndex = x;}
				++x;
			}
		}
	}
	FString tstr = "", rhead = "";
	TArray<FString> Entries = {};
	bool inQuotes = false;
	right2.Split("\n",&rhead, &right);
	for (x=0; x<right.Len(); ++x) {
		auto& rightx = right[x];
		if (rightx=='"') {inQuotes = !inQuotes;}
		else if (rightx==','&&!inQuotes) {
			Entries.Emplace(MoveTemp(tstr));
			tstr = "";
		}
		else {tstr += rightx;}
	}
	Entries.Add(MoveTemp(tstr));
	Table.Type = FindObject<UScriptStruct>(nullptr, *Entries.Last());
	AddRowsFromCsvString(Table,left);
	if (hasSources) {
		Props["Sources"]->ImportText_Direct(*Entries[sourceIndex], Props["Sources"]->ContainerPtrToValuePtr<void>(&Table), nullptr, EPropertyPortFlags::PPF_ExternalEditor, nullptr);
		RecreateMissingData(Table);
	}
}

TArray<FString> UDynamicDatatableBPLibrary::SplitCsvLine(FString& Line) {
	TArray<FString> res = {};
	int x = 0, i = 0, l = Line.Len();
	bool inside = false, lastwascomma = false;
	res.Emplace("");
	FString* strptr = &res[i];
	for (x=0; x<l; ++x) {
		auto& c = Line[x];
		if (c == ','&&!inside) {
			res.Emplace(""); ++i; strptr = &res[i];
			lastwascomma = true;
		}
		else if (c == ' ') {//don't add whitespace
			if (!lastwascomma) {*strptr += c; lastwascomma = false;}
		}
		else if (c == '"') {
			inside = !inside;
			*strptr += c;
			if (lastwascomma) {lastwascomma = false;}
		}
		else {
			*strptr += c;
			if (!lastwascomma) {lastwascomma = false;}
		}
	}
	return res;
}

void UDynamicDatatableBPLibrary::AddRowsFromDynamicTableAsset(FDynamicTable& Table, UDynamicDataTable* Source) {
	if (Source==nullptr) {return;}
	TArray<UDynamicDataTable*> sources = {};
	/*if (Source->IsA(UCompositeDataTable::StaticClass())) {
		UCompositeDataTable* cdt = Cast<UCompositeDataTable>(Source);
		FArrayProperty* ParentTablesProperty = FindFProperty<FArrayProperty>(cdt->StaticClass(), "ParentTables");
		if (ParentTablesProperty!=nullptr) {
			FScriptArrayHelper ParentTablesHelper(ParentTablesProperty, ParentTablesProperty->ContainerPtrToValuePtr<void>(cdt));
			for (int32 i = 0; i < ParentTablesHelper.Num(); i++) {
				if (FObjectProperty* InnerProp = CastField<FObjectProperty>(ParentTablesProperty->Inner)) {
					UObject* Object = InnerProp->GetObjectPropertyValue(ParentTablesHelper.GetRawPtr(i));
					UDataTable* table = Cast<UDataTable>(Object);
					if (table!=nullptr) {sources.Emplace(table);}
				}
			}
		}
	}
	else {*/
		sources.Emplace(Source);
	//}
	for (UDynamicDataTable* each : sources) {
		TMap<FName,uint8*> data = each->GetRowMap();
		/*if (!Table.SourceRowNames.Contains(each)) {
			TArray<FName> blank = {};
			Table.SourceRowNames.Emplace(each,blank);
		}*/
		UScriptStruct* rowStruct = each->RowStruct;
		Table.Type = each->RowStruct;
		for (auto rowIt = each->GetRowMap().CreateConstIterator(); rowIt; ++rowIt) {
			FName rowName = rowIt.Key();
			if (!Table.Map.Contains(rowName)) {
				//void* rowStructData = FMemory::Malloc(rowStruct->GetStructureSize());
				//rowStruct->InitializeStruct(rowStructData);
				//rowStruct->CopyScriptStruct(rowStructData, rowIt.Value());
				//Table.Map.Emplace(rowName, rowStructData);
				//FString strdata = "";
				//StructToString(rowStruct,rowIt.Value(),strdata);
				//Table.Map.Emplace(rowName, MoveTemp(strdata));
				auto& elem = Table.Map.Emplace(rowName,FInstancedStruct());
				elem.InitializeAs(rowStruct, (uint8*)rowIt.Value());
				Table.RowNames.AddUnique(rowName);
				Table.Sources.Emplace(rowName,nullptr);
				//Table.SourceRowNames[each].Emplace(rowName);
			}
		}
	}
}

UDataTable* UDynamicDatatableBPLibrary::GetSourceFromRow(FDynamicTable& Table, FName RowName) {
	if (Table.Sources.Contains(RowName)) {
		return Table.Sources[RowName];
	}
	return nullptr;
}

bool UDynamicDatatableBPLibrary::DeleteTableFile(UObject* WorldContextObject, FString Path, FString FileName) {
	int idx = FileName.Find(".");
	if (idx>=0) {
		if (!FileName.EndsWith(".dyntable")) {return false;}
		FileName = FileName.Left(idx);
	}
	if (!FileName.EndsWith(".dyntable")) {FileName+=".dyntable";}
	Path += FileName;
	if (FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path)) {
		return true;
	}
	return false;
}

TArray<FName> UDynamicDatatableBPLibrary::SearchTableRows(FDynamicTable& Table, FString Search, FString Prefix, FString Suffix, bool CaseSensitive) {
	TArray<FName> Rows = {};
	TArray<FName> res = {};
	Table.Map.GenerateKeyArray(Rows);
	ESearchCase::Type cs = ESearchCase::IgnoreCase;
	if (CaseSensitive) {cs = ESearchCase::CaseSensitive;}
	bool hasSearch = (Search.Len()>0), hasPrefix = (Prefix.Len()>0), hasSuffix = (Suffix.Len()>0); 
	bool fastpath = (hasSearch&&!hasPrefix&&!hasSuffix);
	if (fastpath) {
		for (auto& each : Rows) {
			if (each.ToString().Contains(Search, cs)) {
				res.Emplace(each);
			}
		}
	}
	else {
		for (auto& each : Rows) {
			if (hasSearch&&each.ToString().Contains(Search, cs)) {
				res.Emplace(each); continue;
			}
			if (hasPrefix&&each.ToString().StartsWith(Prefix, cs)) {
				res.Emplace(each); continue;
			}
			if (hasSuffix&&each.ToString().EndsWith(Suffix, cs)) {
				res.Emplace(each);
			}
		}
	}
	return res;
}
