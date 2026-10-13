// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "StageBillOfMaterialsPanel.generated.h"

/** Display formats of the production report, shared by the panel and its tests. */
namespace StageReportFormat
{
	/** "512.4 kg" below a tonne, "1.25 t" from there. */
	MODULARSCENEBUILDER_API FText FormatWeight(double Kilograms);

	/** "850 W" below a kilowatt, "8.40 kW" from there. */
	MODULARSCENEBUILDER_API FText FormatPower(double Watts);

	/** The detail under a line's name: the manufacturer and, when units were placed as runs, "in N runs". */
	MODULARSCENEBUILDER_API FText MakeLineDetail(const FStageBomLine& Line);
}

/**
 * The Bill of Materials dock panel (StageCraft.Panel.BillOfMaterials, Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §5):
 * totals (items, units, weight, power) as tiles, then every catalog item by discipline with quantity, weight and power and a
 * subtotal per discipline, and a CSV export for production planning (rental quotes, truck packing, power distribution).
 *
 * A view in the project's binding pattern:
 *  - It reads UStageBillOfMaterialsSubsystem::GetReport and never computes totals itself (one source of truth).
 *  - Push, never poll: it rebuilds on the next tick after OnReportChanged, so a burst of edits is one rebuild, and follows the
 *    export through OnExportFinished.
 *  - Exporting goes through the controller's RequestExportBillOfMaterials like every request; refusals are reported there.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageBillOfMaterialsPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Report")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** A discipline subtotal or one catalog item. */
	enum class ERowStyle : uint8
	{
		Group,
		Line,
	};

	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildSummary();
	class UWidget* BuildFooter();
	class UWidget* MakeTile(const FText& Title, TObjectPtr<class UTextBlock>& OutValue);
	class UWidget* MakeTableRow(const FText& Name, const FText& Detail, const FText& Quantity, const FText& Weight, const FText& Power, ERowStyle Style);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;

	void RequestRebuild();
	void Rebuild();
	void RefreshExport();

	UFUNCTION()
	void HandleReportChanged();

	UFUNCTION()
	void HandleExportFinished(const FStageReportExportResult& Result);

	UFUNCTION()
	void HandleExportClicked();

	UFUNCTION()
	void HandleOpenFolderClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> TableBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SubtitleText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ItemsValue = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> UnitsValue = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> WeightValue = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> PowerValue = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> ExportButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ExportButtonText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> OpenFolderButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	TWeakObjectPtr<class UStageBillOfMaterialsSubsystem> Report;
	TWeakObjectPtr<class UStageSessionSubsystem> Session;
	FTimerHandle RebuildTimer;
};
