// Copyright DamenTools internal. Space Calculator plugin.

#include "SpaceCalculatorLibrary.h"
#include "SpaceCalculator.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"

bool USpaceCalculatorLibrary::GetSpaceUsage(UObject* BuildManagerRef, float& UsedAreaM2, float& AvailableAreaM2, float& PercentUsed)
{
	UsedAreaM2 = 0.f;
	AvailableAreaM2 = 0.f;
	PercentUsed = 0.f;

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogSpaceCalculator, Warning, TEXT("GetSpaceUsage: BuildManagerRef is null/invalid."));
		return false;
	}

	UClass* Class = BuildManagerRef->GetClass();

	// See header comment: these three names/types must match BP_BuildManager exactly.
	// GridCellSizeXY is read as EITHER FFloatProperty or FDoubleProperty: Blueprint
	// "float" variables default to double-precision storage since UE5, so which one
	// a given BP variable actually is depends on its "Real Number" numeric type
	// setting, not just its Details-panel display as "Float".
	const FFloatProperty* CellSizeFloatProp = FindFProperty<FFloatProperty>(Class, TEXT("GridCellSizeXY"));
	const FDoubleProperty* CellSizeDoubleProp = CellSizeFloatProp ? nullptr : FindFProperty<FDoubleProperty>(Class, TEXT("GridCellSizeXY"));
	const FIntProperty* CellsCountProp = FindFProperty<FIntProperty>(Class, TEXT("CellsCount"));
	const FMapProperty* OccupancyProp = FindFProperty<FMapProperty>(Class, TEXT("OccupancyData"));

	if ((!CellSizeFloatProp && !CellSizeDoubleProp) || !CellsCountProp || !OccupancyProp)
	{
		UE_LOG(LogSpaceCalculator, Error,
			TEXT("GetSpaceUsage: BuildManagerRef (class %s) is missing an expected property - GridCellSizeXY(float/double): %s, CellsCount(int32): %s, OccupancyData(map): %s. BP_BuildManager may have been renamed/refactored - see SpaceCalculatorLibrary.h."),
			*Class->GetName(),
			(CellSizeFloatProp || CellSizeDoubleProp) ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			CellsCountProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			OccupancyProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"));
		return false;
	}

	const float CellSizeCm = CellSizeFloatProp
		? CellSizeFloatProp->GetPropertyValue_InContainer(BuildManagerRef)
		: static_cast<float>(CellSizeDoubleProp->GetPropertyValue_InContainer(BuildManagerRef));
	const int32 CellsCount = CellsCountProp->GetPropertyValue_InContainer(BuildManagerRef);

	FScriptMapHelper MapHelper(OccupancyProp, OccupancyProp->ContainerPtrToValuePtr<void>(BuildManagerRef));
	const int32 OccupiedCellsCount = MapHelper.Num();

	const float CellAreaM2 = FMath::Square(CellSizeCm / 100.f);

	AvailableAreaM2 = CellsCount * CellAreaM2;
	UsedAreaM2 = OccupiedCellsCount * CellAreaM2;
	PercentUsed = (AvailableAreaM2 > KINDA_SMALL_NUMBER) ? (UsedAreaM2 / AvailableAreaM2) * 100.f : 0.f;

	return true;
}

FText USpaceCalculatorLibrary::GetSpaceUsageText(UObject* BuildManagerRef)
{
	float UsedAreaM2 = 0.f;
	float AvailableAreaM2 = 0.f;
	float PercentUsed = 0.f;

	if (!GetSpaceUsage(BuildManagerRef, UsedAreaM2, AvailableAreaM2, PercentUsed))
	{
		return FText::FromString(TEXT("Space: n/a"));
	}

	return FText::Format(
		NSLOCTEXT("SpaceCalculator", "SpaceUsageFormat", "Used: {0} m² / Available: {1} m² ({2}%)"),
		FText::AsNumber(FMath::RoundToInt(UsedAreaM2)),
		FText::AsNumber(FMath::RoundToInt(AvailableAreaM2)),
		FText::AsNumber(FMath::RoundToInt(PercentUsed)));
}

bool USpaceCalculatorLibrary::GetPlacedObjectCounts(UObject* BuildManagerRef, TMap<FString, int32>& OutCounts)
{
	OutCounts.Reset();

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogSpaceCalculator, Warning, TEXT("GetPlacedObjectCounts: BuildManagerRef is null/invalid."));
		return false;
	}

	UClass* Class = BuildManagerRef->GetClass();

	// See header comment: same reflection pattern as
	// WarehousePlannerSaveLibrary::GetUniquePlacedObjects, reimplemented here
	// rather than depending on that plugin, to keep SpaceCalculator isolated.
	const FMapProperty* ObjectsDataProp = FindFProperty<FMapProperty>(Class, TEXT("ObjectsData"));
	if (!ObjectsDataProp)
	{
		UE_LOG(LogSpaceCalculator, Error,
			TEXT("GetPlacedObjectCounts: BuildManagerRef (class %s) is missing ObjectsData(map) - BP_BuildManager may have been renamed/refactored."),
			*Class->GetName());
		return false;
	}

	const FObjectProperty* ValueProp = CastField<FObjectProperty>(ObjectsDataProp->ValueProp);
	if (!ValueProp)
	{
		UE_LOG(LogSpaceCalculator, Error,
			TEXT("GetPlacedObjectCounts: ObjectsData's value type is not an object reference - BP_BuildManager may have been refactored."));
		return false;
	}

	FScriptMapHelper MapHelper(ObjectsDataProp, ObjectsDataProp->ContainerPtrToValuePtr<void>(BuildManagerRef));
	TSet<AActor*> UniqueActors;
	for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
	{
		if (!MapHelper.IsValidIndex(Index))
		{
			continue;
		}
		UObject* Obj = ValueProp->GetObjectPropertyValue(MapHelper.GetValuePtr(Index));
		if (AActor* Actor = Cast<AActor>(Obj))
		{
			if (IsValid(Actor))
			{
				UniqueActors.Add(Actor);
			}
		}
	}

	for (AActor* Actor : UniqueActors)
	{
		FString ClassName = Actor->GetClass()->GetName();
		ClassName.RemoveFromEnd(TEXT("_C"));
		ClassName.RemoveFromStart(TEXT("BP_"));
		OutCounts.FindOrAdd(ClassName)++;
	}

	return true;
}

FText USpaceCalculatorLibrary::GetPlacedObjectCountsText(UObject* BuildManagerRef)
{
	TMap<FString, int32> Counts;
	if (!GetPlacedObjectCounts(BuildManagerRef, Counts))
	{
		return FText::FromString(TEXT("Placed: n/a"));
	}

	if (Counts.Num() == 0)
	{
		return FText::FromString(TEXT("Placed: (none)"));
	}

	Counts.KeySort([](const FString& A, const FString& B) { return A < B; });

	TArray<FString> Parts;
	Parts.Reserve(Counts.Num());
	for (const TPair<FString, int32>& Pair : Counts)
	{
		Parts.Add(FString::Printf(TEXT("%s: %d"), *Pair.Key, Pair.Value));
	}

	return FText::FromString(FString::Join(Parts, TEXT(" | ")));
}
