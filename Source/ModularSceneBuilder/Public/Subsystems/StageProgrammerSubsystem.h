// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageProgrammerSubsystem.generated.h"

/** Fixtures whose programmer values or highlight changed; UShowControlSubsystem re-mixes exactly these. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStageProgrammerFixturesChanged, TConstArrayView<int32> /*FixtureIds*/);
/** The programmer's values changed (not the highlight): the stage has unsaved changes, panels refresh their counts. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageProgrammerValuesChanged);

/**
 * The programmer of one stage (Docs/ADR/0025-programmer-and-output-mix.md §3): the attribute values the user set by hand, per
 * fixture ID, as on a lighting console. They sit above the playing cue in the output mix, so a fade never overwrites them, until
 * Clear (or a cue store, which takes them into the cue) removes them. The highlight set is a transient overlay on top.
 *
 * State only. Writers: AModularPlayerController's requests (RequestParameterChange on a fixture's look, RequestClearProgrammer,
 * RequestSetHighlight), undo and redo (UStageEditHistoryComponent), and scene load (UStageSceneComponent). Every entry point
 * drops values that are not finite or not StageCraft.Attribute tags; the fixture clamps the rest when the mix is written.
 * UShowControlSubsystem is the only reader that touches fixtures. Saved in scenes (format v6). Never ticks.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageProgrammerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Bounds for values from untrusted sources (files, undo steps), the same as a cue's (FStageSceneStore). */
	static constexpr int32 MaxFixtures = 4096;
	static constexpr int32 MaxAttributesPerFixture = 256;

	// --- Values ---

	/** Merges Values into FixtureId's programmer values (FixtureId must be positive). A plain value replaces a palette reference. */
	void SetValues(int32 FixtureId, const TMap<FGameplayTag, float>& Values);

	/**
	 * Merges Values into FixtureId's programmer values as references to PaletteId (Docs/ADR/0028-palettes-and-references.md §3):
	 * the mix shows the palette's current value, Values are the fallback. An invalid PaletteId sets plain values.
	 */
	void SetReferencedValues(int32 FixtureId, const TMap<FGameplayTag, float>& Values, const FGuid& PaletteId);

	/** Removes these fixtures' values; their attributes fall back to the cue and the home look. */
	void ClearFixtures(TConstArrayView<int32> FixtureIds);

	/** Removes every value (Clear on a console). The highlight stays. */
	void ClearAll();

	/** The values and references of these fixtures, one entry per fixture that has any (for undo: CaptureFixtures before and after an edit). */
	TArray<FShowCueFixtureState> CaptureFixtures(TConstArrayView<int32> FixtureIds) const;

	/** Gives exactly these fixtures exactly the values in Entries (a fixture without an entry is cleared). Undo and redo. */
	void RestoreFixtures(TConstArrayView<int32> FixtureIds, TConstArrayView<FShowCueFixtureState> Entries);

	/** Every fixture's values, sorted by fixture ID (scene save). */
	TArray<FShowCueFixtureState> GetEntries() const;

	/** Replaces the whole programmer (scene load); entries are sanitized and bounded. */
	void ReplaceAll(TConstArrayView<FShowCueFixtureState> Entries);

	/** FixtureId's plain values (references included as their fallback), or null when the programmer holds none. */
	const TMap<FGameplayTag, float>* FindValues(int32 FixtureId) const;

	/** FixtureId's palette references, or null when it has none. */
	const TMap<FGameplayTag, FGuid>* FindReferences(int32 FixtureId) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Programmer")
	int32 GetFixtureCount() const { return Entries.Num(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Programmer")
	bool IsEmpty() const { return Entries.IsEmpty(); }

	// --- Highlight ---

	/** The fixtures shown at full, open white on top of everything (never stored, never saved). Empty turns the highlight off. */
	void SetHighlightedFixtures(TConstArrayView<int32> FixtureIds);

	bool IsHighlighted(int32 FixtureId) const { return Highlighted.Contains(FixtureId); }

	FOnStageProgrammerFixturesChanged OnFixturesChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Programmer")
	FOnStageProgrammerValuesChanged OnValuesChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;
	//~ End UWorldSubsystem Interface

private:
	/** Values that may enter: StageCraft.Attribute tags with finite values, at most MaxAttributesPerFixture. */
	static TMap<FGameplayTag, float> SanitizeValues(const TMap<FGameplayTag, float>& InValues);

	/** An entry from an untrusted source (undo step, file): sanitized values and only the references that have a value. */
	static FShowCueFixtureState SanitizeEntry(const FShowCueFixtureState& InEntry);

	/** Merges Values (and, with a valid PaletteId, references to it) into FixtureId's entry. */
	void MergeValues(int32 FixtureId, const TMap<FGameplayTag, float>& InValues, const FGuid& PaletteId);

	void BroadcastValues(TConstArrayView<int32> FixtureIds);

	/** Per fixture ID: its values and palette references (FixtureId in the entry matches the key). */
	TMap<int32, FShowCueFixtureState> Entries;
	TSet<int32> Highlighted;
};
