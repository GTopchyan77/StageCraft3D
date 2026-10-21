// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageMeshImportTypes.generated.h"

UENUM(BlueprintType)
enum class EStageMeshImportResult : uint8
{
	Success,
	/** The extension is not one the app imports, or the importer for it is not part of this build (FBX in packaged builds). */
	UnsupportedFormat,
	FileNotFound,
	FileTooLarge,
	/** The file is already in the Library, or is being imported. */
	AlreadyImported,
	/** The importer finished without producing static geometry. */
	NoGeometry,
	ImporterFailed,
	/** Importing is switched off by the session rules, or the importer is not available. */
	NotAllowed,
};

/** Where an import request came from. Restores re-import last session's models at startup and report only their failures. */
UENUM(BlueprintType)
enum class EStageMeshImportOrigin : uint8
{
	User,
	Restore,
};

/** The outcome of importing one file. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMeshImportOutcome
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Import")
	FString SourceFile;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Import")
	EStageMeshImportResult Result = EStageMeshImportResult::ImporterFailed;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Import")
	EStageMeshImportOrigin Origin = EStageMeshImportOrigin::User;

	/** A sentence for the status bar, e.g. "Imported Chair (GLB) into the Library." */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Import")
	FText Message;

	/** The new catalog item on success, null otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Import")
	TObjectPtr<class UStageImportedItemData> Item = nullptr;

	bool IsSuccess() const { return Result == EStageMeshImportResult::Success; }
};

/**
 * Pure rules of the mesh importer: formats, file checks, identities and the import list file. No engine state, so all of it is
 * unit tested (StageCraft.Import.*). Game thread or worker thread.
 */
namespace StageMeshImport
{
	/** Largest source file accepted. Bigger files would stall the importer for minutes and are almost never stage props. */
	constexpr int64 MaxFileSizeBytes = 512ll * 1024 * 1024;

	/** Most models remembered for the next start. Further imports still work but are not restored. */
	constexpr int32 MaxRememberedFiles = 256;

	/**
	 * Package root of imported models. /Temp is mounted in every build (to Saved, read-only) and is not project content, so the
	 * packages are never saved and never listed with the project's assets. (/Memory cannot be used: in builds with editor code
	 * the material importer asks for a package's file name, and /Memory has none.)
	 */
	MODULARSCENEBUILDER_API extern const TCHAR* const PackageRoot;

	/** Extensions the app offers, lower case, in the order shown in dialogs. Builds without an FBX importer drop "fbx" at runtime. */
	MODULARSCENEBUILDER_API TArray<FString> GetCandidateExtensions();

	/** Lower-case extension of a path without the dot ("C:/A/Chair.GLB" reads "glb"). */
	MODULARSCENEBUILDER_API FString GetExtension(const FString& FilePath);

	/** True when the file's extension is one of Supported (lower case, no dots). */
	MODULARSCENEBUILDER_API bool HasSupportedExtension(const FString& FilePath, const TArray<FString>& Supported);

	/**
	 * True for a Datasmith scene (.udatasmith, or .datasmith as users often call it). There is no runtime static-mesh path for
	 * them in UE 5.8 (ADR 0006 §4.3, ADR 0007 §7), so they get their own refusal with what to export instead.
	 */
	MODULARSCENEBUILDER_API bool IsDatasmithScene(const FString& FilePath);

	/**
	 * Extensions from importer format strings ("fbx;Filmbox", "gltf;GL Transmission Format") that are also candidates.
	 * Keeps the candidate order. Unknown or malformed strings are ignored.
	 */
	MODULARSCENEBUILDER_API TArray<FString> FilterSupportedExtensions(const TArray<FString>& ImporterFormats);

	/** Rotation and uniform scale applied to a model while it is imported (Interchange's import offset). */
	struct FImportOffset
	{
		FRotator Rotation = FRotator::ZeroRotator;
		double UniformScale = 1.0;
	};

	/**
	 * Correction for formats that store no units or up axis. glTF defines both (metres, Y-up) and FBX stores them, so Interchange
	 * converts those itself. OBJ defines neither and Interchange reads it as centimetres, Z-up; StageCraft assumes metres, Y-up,
	 * which is what Blender, Maya and Cinema 4D write.
	 */
	MODULARSCENEBUILDER_API FImportOffset GetImportOffset(const FString& Extension);

	/** Absolute, normalized form used as the file's identity, so "a/../Chair.glb" and "Chair.glb" are one import. */
	MODULARSCENEBUILDER_API FString NormalizeSourcePath(const FString& FilePath);

	/** Stable item name for a normalized path ("IMP_" + 16 hex digits), the same in every session and on every machine. */
	MODULARSCENEBUILDER_API FString MakeItemName(const FString& NormalizedPath);

	/** Package path of the item's catalog entry, and the folder its imported assets go to. */
	MODULARSCENEBUILDER_API FString MakeItemPackagePath(const FString& ItemName);

	/** True for object paths under PackageRoot, i.e. items that only an import can provide. */
	MODULARSCENEBUILDER_API bool IsImportedItemPath(const FString& PackageName);

	/** Library name of a file: its base name with separators as spaces ("big_speaker-stack.glb" reads "big speaker stack"). */
	MODULARSCENEBUILDER_API FText MakeDisplayName(const FString& FilePath);

	/** File dialog filter: "3D models (*.glb;*.gltf)|*.glb;*.gltf" plus one entry per format. Empty for no extensions. */
	MODULARSCENEBUILDER_API FString MakeFileDialogFilter(const TArray<FString>& Extensions);

	/** The remembered imports as a small versioned JSON document. */
	MODULARSCENEBUILDER_API FString WriteImportList(const TArray<FString>& SourceFiles);

	/**
	 * Reads a document written by WriteImportList. Untrusted input: entries that are not strings, empty, relative, too long,
	 * or duplicates are dropped, and at most MaxRememberedFiles are kept. False when the document itself is unreadable.
	 */
	MODULARSCENEBUILDER_API bool ReadImportList(const FString& Json, TArray<FString>& OutSourceFiles);

	/** What LoadImportListFile found (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.3). */
	enum class EImportListRead : uint8
	{
		/** No list yet (first run): start empty, save freely. */
		Missing,
		Read,
		/** Not an import list (damaged, truncated, another version): moved to Path.bak, start empty, save freely. */
		Damaged,
		/** The file exists but could not be read or moved aside (locked, no permission): it must not be overwritten this session. */
		Unreadable,
	};

	/**
	 * Reads the import list file at Path into OutSourceFiles (through ReadImportList). A damaged list is moved aside before
	 * anything can overwrite it. File system only: any thread.
	 */
	MODULARSCENEBUILDER_API EImportListRead LoadImportListFile(const FString& Path, TArray<FString>& OutSourceFiles);
}
