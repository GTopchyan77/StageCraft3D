// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/StageMeshImportTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMeshImportTypes)

#define LOCTEXT_NAMESPACE "StageMeshImport"

namespace StageMeshImport
{
	const TCHAR* const PackageRoot = TEXT("/Temp/StageCraft/Imported/");

	namespace
	{
		constexpr int32 ImportListVersion = 1;
		constexpr int32 MaxPathLength = 1024;
		const TCHAR* const VersionField = TEXT("version");
		const TCHAR* const FilesField = TEXT("files");

		FText FormatLabel(const FString& Extension)
		{
			if (Extension == TEXT("glb") || Extension == TEXT("gltf"))
			{
				return LOCTEXT("GltfFormat", "glTF");
			}
			if (Extension == TEXT("obj"))
			{
				return LOCTEXT("ObjFormat", "Wavefront OBJ");
			}
			if (Extension == TEXT("fbx"))
			{
				return LOCTEXT("FbxFormat", "FBX");
			}
			return FText::FromString(Extension.ToUpper());
		}
	}

	TArray<FString> GetCandidateExtensions()
	{
		return { TEXT("glb"), TEXT("gltf"), TEXT("obj"), TEXT("fbx") };
	}

	FString GetExtension(const FString& FilePath)
	{
		return FPaths::GetExtension(FilePath).ToLower();
	}

	bool HasSupportedExtension(const FString& FilePath, const TArray<FString>& Supported)
	{
		const FString Extension = GetExtension(FilePath);
		return !Extension.IsEmpty() && Supported.Contains(Extension);
	}

	TArray<FString> FilterSupportedExtensions(const TArray<FString>& ImporterFormats)
	{
		TSet<FString> Offered;
		for (const FString& Format : ImporterFormats)
		{
			FString Extensions;
			if (!Format.Split(TEXT(";"), &Extensions, nullptr))
			{
				Extensions = Format;
			}
			// A translator may list several extensions in one entry ("gltf,glb;GL Transmission Format").
			TArray<FString> Parts;
			Extensions.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				Offered.Add(Part.TrimStartAndEnd().ToLower());
			}
		}

		TArray<FString> Result;
		for (const FString& Candidate : GetCandidateExtensions())
		{
			if (Offered.Contains(Candidate))
			{
				Result.Add(Candidate);
			}
		}
		return Result;
	}

	FImportOffset GetImportOffset(const FString& Extension)
	{
		FImportOffset Offset;
		if (Extension.Equals(TEXT("obj"), ESearchCase::IgnoreCase))
		{
			// Metres to centimetres, and a quarter turn about X: as Interchange applies the import offset, the file's +Y (up)
			// becomes Unreal's +Z (checked with Scripts/TestModels/Stage_Riser.obj, STATE.md #28).
			Offset.Rotation = FRotator(0.0, 0.0, 90.0);
			Offset.UniformScale = 100.0;
		}
		return Offset;
	}

	FString NormalizeSourcePath(const FString& FilePath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(FilePath.TrimStartAndEnd());
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	FString MakeItemName(const FString& NormalizedPath)
	{
		// Windows paths are case-insensitive: "C:/Models/Chair.glb" and "c:/models/chair.GLB" are one file.
		const FString Key = NormalizedPath.ToLower();
		FSHAHash Hash;
		FSHA1::HashBuffer(*Key, Key.Len() * sizeof(TCHAR), Hash.Hash);
		return TEXT("IMP_") + Hash.ToString().Left(16);
	}

	FString MakeItemPackagePath(const FString& ItemName)
	{
		return FString(PackageRoot) + ItemName;
	}

	bool IsImportedItemPath(const FString& PackageName)
	{
		return PackageName.StartsWith(PackageRoot);
	}

	FText MakeDisplayName(const FString& FilePath)
	{
		FString Name = FPaths::GetBaseFilename(FilePath);
		Name.ReplaceCharInline(TEXT('_'), TEXT(' '));
		Name.ReplaceCharInline(TEXT('-'), TEXT(' '));
		Name.TrimStartAndEndInline();
		return Name.IsEmpty() ? LOCTEXT("UnnamedModel", "Imported model") : FText::FromString(Name);
	}

	FString MakeFileDialogFilter(const TArray<FString>& Extensions)
	{
		if (Extensions.IsEmpty())
		{
			return FString();
		}

		auto Patterns = [](const TArray<FString>& InExtensions)
		{
			return FString::JoinBy(InExtensions, TEXT(";"), [](const FString& Extension) { return TEXT("*.") + Extension; });
		};

		const FString All = Patterns(Extensions);
		FString Filter = FText::Format(LOCTEXT("AllModels", "3D models ({0})"), FText::FromString(All)).ToString() + TEXT("|") + All;

		// One entry per format, glTF's two extensions together.
		TArray<FString> Done;
		for (const FString& Extension : Extensions)
		{
			if (Done.Contains(Extension))
			{
				continue;
			}
			const FText Label = FormatLabel(Extension);
			TArray<FString> Same;
			for (const FString& Other : Extensions)
			{
				if (FormatLabel(Other).EqualTo(Label))
				{
					Same.Add(Other);
					Done.Add(Other);
				}
			}
			const FString Group = Patterns(Same);
			Filter += TEXT("|") + FText::Format(LOCTEXT("OneFormat", "{0} ({1})"), Label, FText::FromString(Group)).ToString() + TEXT("|") + Group;
		}
		return Filter;
	}

	FString WriteImportList(const TArray<FString>& SourceFiles)
	{
		TArray<TSharedPtr<FJsonValue>> Files;
		Files.Reserve(SourceFiles.Num());
		for (const FString& File : SourceFiles)
		{
			Files.Add(MakeShared<FJsonValueString>(File));
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(VersionField, ImportListVersion);
		Root->SetArrayField(FilesField, Files);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}

	bool ReadImportList(const FString& Json, TArray<FString>& OutSourceFiles)
	{
		OutSourceFiles.Reset();

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		int32 Version = 0;
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()
			|| !Root->TryGetNumberField(VersionField, Version) || Version != ImportListVersion
			|| !Root->TryGetArrayField(FilesField, Files))
		{
			return false;
		}

		TSet<FString> Seen;
		for (const TSharedPtr<FJsonValue>& Value : *Files)
		{
			FString File;
			if (OutSourceFiles.Num() >= MaxRememberedFiles)
			{
				break;
			}
			if (!Value.IsValid() || !Value->TryGetString(File) || File.IsEmpty() || File.Len() > MaxPathLength || FPaths::IsRelative(File))
			{
				continue;
			}
			bool bAlreadySeen = false;
			Seen.Add(File.ToLower(), &bAlreadySeen);
			if (!bAlreadySeen)
			{
				OutSourceFiles.Add(File);
			}
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
