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
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageFixtureReviewsChanged);

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
 * - Import is reviewed: a file read on a worker becomes an FStageFixtureImportReview (the import report), and nothing reaches the
 *   catalog until ConfirmImport. Confirming a FixtureTypeID the library already holds replaces it: placed fixtures move to the new
 *   mode with the same name; a mode that is gone leaves them on the old data for this session (missing at the next load, §7.1).
 * - RemoveType deletes a type and its library file, refused while a placed fixture uses one of its modes.
 *
 * Who calls: AModularPlayerController (RequestImportFixtureFiles, RequestConfirmFixtureImport, RequestCancelFixtureImport,
 * RequestRemoveFixtureType) after the GameMode allows it; dev console commands. Views read GetTypes and GetReviews and follow
 * OnLibraryChanged and OnReviewsChanged.
 * Lifetime: the whole run (GameInstance). Game-thread API; worker results are matched to this instance by weak pointer and dropped
 * after Deinitialize. Deinitialize moves the built objects out of their catalog paths so a later game instance in the same process
 * can build them again.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageFixtureLibrarySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** A parsed GDTF file. Opaque here: defined in the .cpp so the GDTF model stays out of this header. */
	struct FParsedFile;

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Most files waiting at once, reading or under review. */
	static constexpr int32 MaxReviews = 8;

	/**
	 * Reads GDTF files for review. Files that cannot be read now (format, missing, too large, already importing or under review,
	 * too many waiting) are refused and returned; nothing is broadcast for them. Accepted files are read on workers; a file that
	 * could not be read reports through OnImportFinished (IOError), every other one becomes a review (OnReviewsChanged), also
	 * when its report refuses it, so the user sees why.
	 */
	TArray<FStageFixtureImportOutcome> RequestImport(const TArray<FString>& Files);

	/** Files read and waiting for the user, oldest first. */
	const TArray<FStageFixtureImportReview>& GetReviews() const { return Reviews; }

	const FStageFixtureImportReview* FindReview(const FGuid& ReviewId) const;

	/**
	 * Adds the reviewed file to the library, or replaces the type it updates. Kind, when set, corrects the inferred kind. Refused
	 * (and the review kept) when the report refuses the file or the library is full; Unknown for a review that is not waiting.
	 * Broadcasts OnImportFinished with the outcome, OnReviewsChanged and, on success, OnLibraryChanged.
	 */
	FStageFixtureImportOutcome ConfirmImport(const FGuid& ReviewId, TOptional<EStageFixtureKind> Kind);

	/** Drops a review without touching the library (Cancelled, or Unknown). Broadcasts OnReviewsChanged. */
	FStageFixtureImportOutcome CancelImport(const FGuid& ReviewId);

	/**
	 * Removes a fixture type: its catalog items, its library file and its index entry. Refused with InUse while a placed fixture
	 * of the current world uses one of its modes. Broadcasts OnImportFinished with the outcome and, on success, OnLibraryChanged.
	 */
	FStageFixtureImportOutcome RemoveType(const FGuid& FixtureTypeId);

	/** True while a placed fixture of the current world uses one of the type's modes. */
	bool IsTypeInUse(const FGuid& FixtureTypeId) const;

	/** The library type an item is a mode of, or null (built-in items, other items). */
	const FStageFixtureLibraryType* FindTypeOfItem(const class UBaseItemData* Item) const;

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

	// --- GDTF files carried by an MVR scene (Docs/ADR/0034 §6) ---

	/** Reads and checks GDTF bytes taken out of another file (SourceName names it in reports). Any thread; nothing is registered. */
	static TSharedRef<FParsedFile> ParseEmbeddedFile(TArray<uint8> Bytes, const FString& SourceName);

	/** What the import report shows for a parsed file: type, modes, warnings, errors, and whether the library has that type. */
	FStageFixtureImportReview DescribeParsedFile(const FParsedFile& File) const { return MakeReview(File); }

	/**
	 * Adds a GDTF file an MVR scene carries, as ConfirmImport adds a reviewed one (copied into the library, kept for the next start),
	 * without a review of its own: the MVR import report covered it. A type the library already holds (same FixtureTypeID) is used
	 * as it is and not replaced: Result AlreadyInLibrary with its Modes. Refused like ConfirmImport (the report refuses the file, the
	 * library is full). Game thread; broadcasts OnImportFinished and, when a type was added, OnLibraryChanged. File's bytes move into
	 * the library.
	 */
	FStageFixtureImportOutcome AddEmbeddedFile(FParsedFile& File);

#if !UE_BUILD_SHIPPING
	/**
	 * Development only: builds a fixture type from GDTF bytes for this session, without writing anything to the library (the
	 * fixture benchmark's 64-cell bar, scripted runs). Synchronous on the game thread.
	 */
	FStageFixtureImportOutcome AddSessionType(TConstArrayView<uint8> Bytes, const FString& Label);
#endif

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Fixtures")
	FOnStageFixtureImportFinished OnImportFinished;

	/** A type was added, replaced or removed (import, start-up restore, RemoveType). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Fixtures")
	FOnStageFixtureLibraryChanged OnLibraryChanged;

	/** A review was added, confirmed or cancelled. Views show GetReviews()[0] and come back here when it changes. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Fixtures")
	FOnStageFixtureReviewsChanged OnReviewsChanged;

private:
	void RestoreLibrary();
	void HandleLibraryRestored(TArray<TSharedPtr<FParsedFile>> Files, double WorkerMs);
	void FinishImport(uint64 Serial, TSharedPtr<FParsedFile> File);

	/** Builds and registers every usable mode as a new type. Null when nothing could be built. */
	FStageFixtureLibraryType* RegisterType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome);

	/** Builds every usable mode of File as engine objects in the type's catalog paths. Unset when nothing could be built. */
	TOptional<FStageFixtureLibraryType> BuildType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome);

	/** Replaces the type at Index with File, moving placed fixtures to the new modes. False (and the old type kept) when nothing could be built. */
	bool ReplaceType(int32 Index, const FParsedFile& File, FStageFixtureImportOutcome& Outcome);

	/** Takes a type's modes out of the catalog and moves its objects out of their paths. Placed fixtures keep the objects alive. */
	void UnregisterType(const FStageFixtureLibraryType& Type);

	/** Placed fixtures of the current world whose item is one of the type's modes. */
	TArray<class AModularBaseActor*> FindPlacedUsers(const FStageFixtureLibraryType& Type) const;

	FStageFixtureImportReview MakeReview(const FParsedFile& File) const;

	/** Keeps every review's bReplaces in step with the library. */
	void RefreshReviews();

	void WriteLibraryFile(const FGuid& FixtureTypeId, TArray<uint8> Bytes);

	/** Writes Fixtures.json from the library types (session-only types are left out). Not called by the start-up restore. */
	void SaveIndex();

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;

	UPROPERTY(Transient)
	TArray<FStageFixtureLibraryType> Types;

	/** Files reading on workers, by serial. */
	TMap<uint64, FString> Pending;

	/** Files read and waiting for the user, oldest first, with their parsed files (same order). */
	TArray<FStageFixtureImportReview> Reviews;
	TArray<TSharedPtr<FParsedFile>> ReviewFiles;
	uint64 NextSerial = 0;
	bool bRestoring = false;

	/** Library writes (file copies, the index) run in order on this pipe, so an older index never overwrites a newer one. */
	UE::Tasks::FPipe WritePipe{ UE_SOURCE_LOCATION };
};
