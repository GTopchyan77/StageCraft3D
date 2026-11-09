// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"
#include "Fixtures/StageFixtureLibraryTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Pipe.h"
#include "StageFixtureLibrarySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageFixtureImportFinished, const FStageFixtureImportOutcome&, Outcome);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageFixtureLibraryChanged);

/** One fixture type of the library and the engine objects built for it. Owned by UStageFixtureLibrarySubsystem. */
USTRUCT()
struct FStageFixtureLibraryType
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid FixtureTypeId;

	/** The file name the user imported; empty for session-only types. */
	UPROPERTY()
	FString SourceFileName;

	/** One catalog item per usable mode, in mode order. */
	UPROPERTY()
	TArray<TObjectPtr<class ULightingFixtureData>> Modes;

	/** The base mesh every mode shares (the root of the placed fixture). Catalog items reference it softly, so this keeps it alive. */
	UPROPERTY()
	TObjectPtr<class UStaticMesh> BaseMesh = nullptr;

	/** When the file was added to the library (Fixtures.json). */
	UPROPERTY()
	FDateTime ImportedUtc;

	/** The user's correction of the inferred kind (ADR 0033 §5.6), kept in Fixtures.json. */
	UPROPERTY()
	bool bKindCorrected = false;

	UPROPERTY()
	EStageFixtureKind CorrectedKind = EStageFixtureKind::MovingHeadSpot;

	/** Built for this session only (development benchmarks) and never written to the library. */
	UPROPERTY()
	bool bSessionOnly = false;
};

/**
 * The fixture device library (Docs/ADR/0033 §7): GDTF files the user imported, each turned into one catalog fixture per DMX mode.
 *
 * - Files are copied to Saved/StageCraft/Fixtures/<FixtureTypeID>.gdtf (StageFixtureLibrary::GetLibraryDirectory) and listed in
 *   Fixtures.json. At start every library file is read and parsed again on worker threads, then built and registered on the game
 *   thread, so scenes that place these fixtures resolve: each mode's catalog path comes from the FixtureTypeID and the mode name.
 * - A .gdtf is untrusted input. It is validated by the GD1 reader (ZIP, XML, model limits) on a worker; only the parsed description
 *   reaches the game thread, where UObjects are created. Nothing from the archive is written to disk except the whole original file.
 * - Importing a FixtureTypeID the library already holds is refused (AlreadyInLibrary) until the Library UI can ask to replace it.
 *
 * Who calls: AModularPlayerController::RequestImportFixtureFiles after the GameMode allows it; dev console commands.
 * Lifetime: the whole run (GameInstance). Game-thread API; worker results are matched to this instance by weak pointer and dropped
 * after Deinitialize. Deinitialize moves the built objects out of their catalog paths so a later game instance in the same process
 * can build them again.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageFixtureLibrarySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Imports GDTF files. Files that cannot be imported now (format, missing, too large, already importing) are refused and
	 * returned; nothing is broadcast for them. Accepted files are read on workers and report through OnImportFinished.
	 */
	TArray<FStageFixtureImportOutcome> RequestImport(const TArray<FString>& Files);

	/**
	 * The library is still being read at start, or files are importing. Until this is false the catalog may lack fixture types a
	 * scene places, so a scene load waits (Docs/ADR/0013 §3.1).
	 */
	bool HasLoadsInProgress() const { return bRestoring || !Pending.IsEmpty(); }

	/** Files importing now. */
	int32 GetPendingCount() const { return Pending.Num(); }

	/** Every fixture type in the library, in the order added. */
	const TArray<FStageFixtureLibraryType>& GetTypes() const { return Types; }

	/** The type with this FixtureTypeID, or null. */
	const FStageFixtureLibraryType* FindType(const FGuid& FixtureTypeId) const;

	/** True for a file name the library imports (.gdtf). */
	static bool IsFixtureFile(const FString& File);

#if !UE_BUILD_SHIPPING
	/**
	 * Development only: builds a fixture type from GDTF bytes for this session, without writing anything to the library (the
	 * fixture benchmark's 64-cell bar, scripted runs). Synchronous on the game thread.
	 */
	FStageFixtureImportOutcome AddSessionType(TConstArrayView<uint8> Bytes, const FString& Label);
#endif

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Fixtures")
	FOnStageFixtureImportFinished OnImportFinished;

	/** A type was added (import or start-up restore). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Fixtures")
	FOnStageFixtureLibraryChanged OnLibraryChanged;

	/** A parsed file waiting for the game thread. Opaque here: defined in the .cpp so the GDTF model stays out of this header. */
	struct FParsedFile;

private:
	void RestoreLibrary();
	void HandleLibraryRestored(TArray<TSharedPtr<FParsedFile>> Files, double WorkerMs);
	void FinishImport(uint64 Serial, TSharedPtr<FParsedFile> File);

	/** Builds and registers every usable mode. Null when nothing could be built. */
	FStageFixtureLibraryType* RegisterType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome);

	/** Writes Fixtures.json from the library types (session-only types are left out). Not called by the start-up restore. */
	void SaveIndex();

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;

	UPROPERTY(Transient)
	TArray<FStageFixtureLibraryType> Types;

	/** Files reading on workers, by serial. */
	TMap<uint64, FString> Pending;
	uint64 NextSerial = 0;
	bool bRestoring = false;

	/** Library writes (file copies, the index) run in order on this pipe, so an older index never overwrites a newer one. */
	UE::Tasks::FPipe WritePipe{ UE_SOURCE_LOCATION };
};
