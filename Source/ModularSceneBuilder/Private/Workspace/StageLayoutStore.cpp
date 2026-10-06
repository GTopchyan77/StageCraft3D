// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageLayoutStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace StageLayoutStore
{
	const TCHAR* LayoutsFolder = TEXT("Layouts");
	const TCHAR* SessionFile = TEXT("Session.json");
	const TCHAR* Extension = TEXT(".json");

	// Wrapper field names (file format v1).
	const TCHAR* FormatVersionField = TEXT("FormatVersion");
	const TCHAR* NameField = TEXT("Name");
	const TCHAR* SlateLayoutField = TEXT("SlateLayout");
	const TCHAR* MainWindowField = TEXT("MainWindow");

	// Slate's FLayout JSON (TabManager.cpp, FLayout::ToJson / PersistToString_Helper).
	const TCHAR* SlateTypeField = TEXT("Type");
	const TCHAR* SlateNameField = TEXT("Name");
	const TCHAR* SlateAreasField = TEXT("Areas");
	const TCHAR* SlatePlacementField = TEXT("WindowPlacement");

	/** Areas with these placements are separate OS windows with a saved position (FArea::EWindowPlacementInternal). */
	bool IsPositionedWindow(const FString& Placement)
	{
		return Placement == TEXT("Placement_Specified") || Placement == TEXT("Placement_ParentSpecified");
	}

	TSharedRef<FJsonObject> PlacementToJson(const FStageWindowPlacement& Placement)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("X"), Placement.Position.X);
		Json->SetNumberField(TEXT("Y"), Placement.Position.Y);
		Json->SetNumberField(TEXT("W"), Placement.Size.X);
		Json->SetNumberField(TEXT("H"), Placement.Size.Y);
		Json->SetBoolField(TEXT("Maximized"), Placement.bMaximized);
		return Json;
	}

	bool PlacementFromJson(const FJsonObject& Json, FStageWindowPlacement& OutPlacement)
	{
		double X = 0.0, Y = 0.0, W = 0.0, H = 0.0;
		if (!Json.TryGetNumberField(TEXT("X"), X) || !Json.TryGetNumberField(TEXT("Y"), Y)
			|| !Json.TryGetNumberField(TEXT("W"), W) || !Json.TryGetNumberField(TEXT("H"), H) || W <= 0.0 || H <= 0.0)
		{
			return false;
		}
		OutPlacement.Position = FVector2D(X, Y);
		OutPlacement.Size = FVector2D(W, H);
		Json.TryGetBoolField(TEXT("Maximized"), OutPlacement.bMaximized);
		return true;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	FString WriteCondensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	double OverlapArea(const FBox2D& A, const FBox2D& B)
	{
		const FVector2D Min(FMath::Max(A.Min.X, B.Min.X), FMath::Max(A.Min.Y, B.Min.Y));
		const FVector2D Max(FMath::Min(A.Max.X, B.Max.X), FMath::Min(A.Max.Y, B.Max.Y));
		return (Max.X > Min.X && Max.Y > Min.Y) ? (Max.X - Min.X) * (Max.Y - Min.Y) : 0.0;
	}
}

FStageLayoutStore::FStageLayoutStore(FString InDirectory, FName InSlateLayoutVersion)
	: Directory(MoveTemp(InDirectory))
	, SlateLayoutVersion(InSlateLayoutVersion)
{
	check(!Directory.IsEmpty() && !SlateLayoutVersion.IsNone());
}

bool FStageLayoutStore::IsValidLayoutName(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > MaxNameLength || Name.TrimStartAndEnd() != Name)
	{
		return false;
	}
	for (const TCHAR Char : Name)
	{
		const bool bAllowed = FChar::IsAlnum(Char) || Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('(') || Char == TEXT(')');
		if (!bAllowed)
		{
			return false;
		}
	}

	// Windows refuses these as file names whatever the extension.
	static const TCHAR* ReservedDeviceNames[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"),
		TEXT("COM1"), TEXT("COM2"), TEXT("COM3"), TEXT("COM4"), TEXT("COM5"), TEXT("COM6"), TEXT("COM7"), TEXT("COM8"), TEXT("COM9"),
		TEXT("LPT1"), TEXT("LPT2"), TEXT("LPT3"), TEXT("LPT4"), TEXT("LPT5"), TEXT("LPT6"), TEXT("LPT7"), TEXT("LPT8"), TEXT("LPT9") };
	for (const TCHAR* Reserved : ReservedDeviceNames)
	{
		if (Name.Equals(Reserved, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return true;
}

FString FStageLayoutStore::GetUserLayoutPath(const FString& Name) const
{
	return FPaths::Combine(Directory, StageLayoutStore::LayoutsFolder, Name + StageLayoutStore::Extension);
}

FString FStageLayoutStore::GetSessionPath() const
{
	return FPaths::Combine(Directory, StageLayoutStore::SessionFile);
}

TArray<FString> FStageLayoutStore::ListUserLayouts() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, StageLayoutStore::LayoutsFolder, FString(TEXT("*")) + StageLayoutStore::Extension), true, false);

	TArray<FString> Names;
	Names.Reserve(Files.Num());
	for (const FString& File : Files)
	{
		// Files a user dropped in by hand with a name we would never write are ignored, not listed.
		const FString Name = FPaths::GetBaseFilename(File);
		if (IsValidLayoutName(Name))
		{
			Names.Add(Name);
		}
	}
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	return Names;
}

EStageLayoutResult FStageLayoutStore::LoadUserLayout(const FString& Name, FStageSavedLayout& OutLayout) const
{
	if (!IsValidLayoutName(Name))
	{
		return EStageLayoutResult::InvalidName;
	}
	const EStageLayoutResult Result = LoadFile(GetUserLayoutPath(Name), OutLayout);
	if (Result == EStageLayoutResult::Success)
	{
		// The file name is the identity; a stale Name field inside (e.g. a renamed file) must not win.
		OutLayout.Name = Name;
	}
	return Result;
}

EStageLayoutResult FStageLayoutStore::SaveUserLayout(const FStageSavedLayout& Layout) const
{
	if (!IsValidLayoutName(Layout.Name))
	{
		return EStageLayoutResult::InvalidName;
	}
	return WriteFileAtomic(GetUserLayoutPath(Layout.Name), Serialize(Layout)) ? EStageLayoutResult::Success : EStageLayoutResult::WriteFailed;
}

EStageLayoutResult FStageLayoutStore::DeleteUserLayout(const FString& Name) const
{
	if (!IsValidLayoutName(Name))
	{
		return EStageLayoutResult::InvalidName;
	}
	const FString Path = GetUserLayoutPath(Name);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return EStageLayoutResult::NotFound;
	}
	return IFileManager::Get().Delete(*Path) ? EStageLayoutResult::Success : EStageLayoutResult::WriteFailed;
}

EStageLayoutResult FStageLayoutStore::LoadSession(FStageSavedLayout& OutLayout) const
{
	return LoadFile(GetSessionPath(), OutLayout);
}

EStageLayoutResult FStageLayoutStore::LoadFile(const FString& Path, FStageSavedLayout& OutLayout) const
{
	if (!IFileManager::Get().FileExists(*Path))
	{
		return EStageLayoutResult::NotFound;
	}

	FString Contents;
	EStageLayoutResult Result = FFileHelper::LoadFileToString(Contents, *Path) ? Parse(Contents, OutLayout) : EStageLayoutResult::Corrupt;
	if (Result == EStageLayoutResult::Corrupt || Result == EStageLayoutResult::VersionMismatch)
	{
		MoveAside(Path);
	}
	return Result;
}

EStageLayoutResult FStageLayoutStore::Parse(const FString& Contents, FStageSavedLayout& OutLayout) const
{
	using namespace StageLayoutStore;

	const TSharedPtr<FJsonObject> Root = ParseObject(Contents);
	int32 Version = 0;
	if (!Root.IsValid() || !Root->TryGetNumberField(FormatVersionField, Version))
	{
		return EStageLayoutResult::Corrupt;
	}
	if (Version != FormatVersion)
	{
		return EStageLayoutResult::VersionMismatch;
	}

	const TSharedPtr<FJsonObject>* SlateLayout = nullptr;
	FString SlateType;
	FString SlateName;
	if (!Root->TryGetObjectField(SlateLayoutField, SlateLayout) || !(*SlateLayout)->TryGetStringField(SlateTypeField, SlateType)
		|| SlateType != TEXT("Layout") || !(*SlateLayout)->TryGetStringField(SlateNameField, SlateName) || !(*SlateLayout)->HasTypedField<EJson::Array>(SlateAreasField))
	{
		return EStageLayoutResult::Corrupt;
	}
	// The FLayout name versions the panel structure (FStageWorkspaceShell::PanelLayoutVersion).
	if (FName(*SlateName) != SlateLayoutVersion)
	{
		return EStageLayoutResult::VersionMismatch;
	}

	FStageSavedLayout Parsed;
	Root->TryGetStringField(NameField, Parsed.Name);
	Parsed.SlateLayout = WriteCondensed(SlateLayout->ToSharedRef());

	const TSharedPtr<FJsonObject>* MainWindow = nullptr;
	if (Root->TryGetObjectField(MainWindowField, MainWindow))
	{
		FStageWindowPlacement Placement;
		if (!PlacementFromJson(**MainWindow, Placement))
		{
			return EStageLayoutResult::Corrupt;
		}
		Parsed.MainWindow = Placement;
	}

	OutLayout = MoveTemp(Parsed);
	return EStageLayoutResult::Success;
}

FString FStageLayoutStore::Serialize(const FStageSavedLayout& Layout) const
{
	using namespace StageLayoutStore;

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(FormatVersionField, FormatVersion);
	Root->SetStringField(NameField, Layout.Name);
	if (Layout.MainWindow.IsSet())
	{
		Root->SetObjectField(MainWindowField, PlacementToJson(Layout.MainWindow.GetValue()));
	}

	// Embedded as an object, not a string, so the file stays readable and diffable.
	const TSharedPtr<FJsonObject> SlateLayout = ParseObject(Layout.SlateLayout);
	ensureMsgf(SlateLayout.IsValid(), TEXT("Serializing layout '%s' whose Slate layout is not JSON; it will fail validation on load."), *Layout.Name);
	Root->SetObjectField(SlateLayoutField, SlateLayout.IsValid() ? SlateLayout : MakeShared<FJsonObject>());

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	return Text;
}

bool FStageLayoutStore::WriteFileAtomic(const FString& Path, const FString& Contents)
{
	const FString TempPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Contents, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	const bool bMoved = IFileManager::Get().Move(*Path, *TempPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true);
	if (!bMoved)
	{
		IFileManager::Get().Delete(*TempPath, false, false, true);
	}
	return bMoved;
}

void FStageLayoutStore::MoveAside(const FString& Path)
{
	// Never delete what the user may want back; the newest bad copy replaces an older one.
	IFileManager::Get().Move(*(Path + TEXT(".bak")), *Path, /*bReplace*/ true, false, false, /*bDoNotRetryOrError*/ true);
}

FStageWindowPlacement FStageLayoutStore::ClampPlacement(const FStageWindowPlacement& Placement, TConstArrayView<FBox2D> WorkAreas)
{
	if (WorkAreas.IsEmpty())
	{
		return Placement;
	}

	const FBox2D Rect(Placement.Position, Placement.Position + Placement.Size);
	const FBox2D* Target = &WorkAreas[0];
	double BestOverlap = 0.0;
	for (const FBox2D& WorkArea : WorkAreas)
	{
		const double Overlap = StageLayoutStore::OverlapArea(Rect, WorkArea);
		if (Overlap > BestOverlap)
		{
			BestOverlap = Overlap;
			Target = &WorkArea;
		}
	}

	FStageWindowPlacement Result = Placement;
	const FVector2D TargetSize = Target->GetSize();
	Result.Size = FVector2D(FMath::Min(Placement.Size.X, TargetSize.X), FMath::Min(Placement.Size.Y, TargetSize.Y));
	Result.Position = FVector2D(
		FMath::Clamp(Placement.Position.X, Target->Min.X, Target->Max.X - Result.Size.X),
		FMath::Clamp(Placement.Position.Y, Target->Min.Y, Target->Max.Y - Result.Size.Y));
	return Result;
}

void FStageLayoutStore::ClampToWorkAreas(FStageSavedLayout& Layout, TConstArrayView<FBox2D> ScreenWorkAreas, TConstArrayView<FBox2D> SlateWorkAreas)
{
	using namespace StageLayoutStore;

	if (Layout.MainWindow.IsSet() && !ScreenWorkAreas.IsEmpty())
	{
		Layout.MainWindow = ClampPlacement(Layout.MainWindow.GetValue(), ScreenWorkAreas);
	}

	if (SlateWorkAreas.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FJsonObject> SlateLayout = ParseObject(Layout.SlateLayout);
	const TArray<TSharedPtr<FJsonValue>>* Areas = nullptr;
	if (!SlateLayout.IsValid() || !SlateLayout->TryGetArrayField(SlateAreasField, Areas))
	{
		return; // Validation on apply rejects it; nothing to clamp.
	}

	bool bChanged = false;
	for (const TSharedPtr<FJsonValue>& AreaValue : *Areas)
	{
		const TSharedPtr<FJsonObject> Area = AreaValue.IsValid() ? AreaValue->AsObject() : nullptr;
		FString Placement;
		if (!Area.IsValid() || !Area->TryGetStringField(SlatePlacementField, Placement) || !IsPositionedWindow(Placement))
		{
			continue;
		}

		FStageWindowPlacement Window;
		Window.Position = FVector2D(Area->GetNumberField(TEXT("WindowPosition_X")), Area->GetNumberField(TEXT("WindowPosition_Y")));
		Window.Size = FVector2D(Area->GetNumberField(TEXT("WindowSize_X")), Area->GetNumberField(TEXT("WindowSize_Y")));
		const FStageWindowPlacement Clamped = ClampPlacement(Window, SlateWorkAreas);
		if (!Clamped.Position.Equals(Window.Position) || !Clamped.Size.Equals(Window.Size))
		{
			Area->SetNumberField(TEXT("WindowPosition_X"), Clamped.Position.X);
			Area->SetNumberField(TEXT("WindowPosition_Y"), Clamped.Position.Y);
			Area->SetNumberField(TEXT("WindowSize_X"), Clamped.Size.X);
			Area->SetNumberField(TEXT("WindowSize_Y"), Clamped.Size.Y);
			bChanged = true;
		}
	}

	if (bChanged)
	{
		Layout.SlateLayout = WriteCondensed(SlateLayout.ToSharedRef());
	}
}
