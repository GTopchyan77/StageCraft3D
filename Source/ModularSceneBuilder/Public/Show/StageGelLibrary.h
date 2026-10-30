// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "StageGelLibrary.generated.h"

UENUM(BlueprintType)
enum class EStageGelBrand : uint8
{
	Lee,
	Rosco,
};

/**
 * One gel of the gel library (Docs/ADR/0020-professional-fixture-suite.md §3), a row of DT_StageGels. The table is built from
 * Content/Data/Source/StageGels.csv by Scripts/Content/create_fixture_library_content.py.
 *
 * Swatch is an sRGB approximation of the gel's look on a white source, for planning; it is not measured spectral data.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGelRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gel")
	EStageGelBrand Brand = EStageGelBrand::Lee;

	/** Catalogue number as printed on the swatch book, without the brand letter: "106", "27", "3202". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gel")
	FString Number;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gel")
	FText DisplayName;

	/** The gel's colour on a white source, sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gel")
	FColor Swatch = FColor::White;
};

/** A validated gel, ready to use. */
struct MODULARSCENEBUILDER_API FStageGel
{
	EStageGelBrand Brand = EStageGelBrand::Lee;
	FString Number;
	FText DisplayName;

	/** Linear transmission per channel, brightest channel 1 (a white source through the gel, normalised). */
	FLinearColor Filter = FLinearColor::White;

	/** "L106" / "R27": the short code consoles show. */
	FString GetCode() const;

	/** "Lee 106 Primary Red". */
	FText GetLabel() const;
};

/** Validated, ordered gel list (Lee, then Rosco, each by number) with lookup by the forms people type or say. */
class MODULARSCENEBUILDER_API FStageGelLibrary
{
public:
	/** Validates rows: an empty number, a duplicate brand + number or a black swatch is logged and skipped. */
	static FStageGelLibrary Build(TConstArrayView<FStageGelRow> Rows);

	/** The project's library (UStageLightingSettings::GelTable), loaded on first use on the game thread and kept. */
	static const FStageGelLibrary& Get();

	const TArray<FStageGel>& GetGels() const { return Gels; }

	/**
	 * Index of the gel Query names, or INDEX_NONE. Accepts "L106", "lee 106", "Lee106", "R27", "rosco 27", "roscolux 27",
	 * a bare number when only one brand has it, and the name ("primary red"; a unique part of a name, "congo").
	 */
	int32 Find(const FString& Query) const;

	/** Index of the gel with this code ("L106"), or INDEX_NONE. */
	int32 FindByCode(const FString& Code) const;

private:
	TArray<FStageGel> Gels;
};
