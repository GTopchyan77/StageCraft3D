// Copyright DamenTools internal. Precise Transform plugin.

#include "PreciseTransformLibrary.h"
#include "PreciseTransform.h"
#include "UObject/UnrealType.h"
#include "Misc/DefaultValueHelper.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"
#include "EngineUtils.h"
#include "Engine/DataTable.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "DynamicDatatable/Public/DynamicDatatableBPLibrary.h"

namespace
{
	/** Finds a UFUNCTION by name on Obj's class, logging (via CallerName) if missing. */
	UFunction* FindBPFunctionChecked(UObject* Obj, const TCHAR* FunctionName, const TCHAR* CallerName)
	{
		if (!IsValid(Obj))
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("%s: target object is null/invalid."), CallerName);
			return nullptr;
		}
		UFunction* Func = Obj->GetClass()->FindFunctionByName(FunctionName);
		if (!Func)
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("%s: function '%s' not found on class '%s'."),
				CallerName, FunctionName, *Obj->GetClass()->GetName());
		}
		return Func;
	}

	/**
	 * Calls a Blueprint UFUNCTION via reflection, matching in/out params by
	 * name (not positional layout) so this stays correct even if the
	 * function's parameter order changes. Params are read/written as raw
	 * FProperty pointers rather than a hand-rolled matching struct - safer
	 * against layout drift between this plugin and the Blueprint.
	 */
	struct FScopedBPCall
	{
		UFunction* Func = nullptr;
		void* Params = nullptr;

		// Initialize/destroy only the CPF_Parm properties, never the whole UFunction via
		// InitializeStruct/DestroyStruct: a Blueprint function's property chain also holds
		// its local variables, laid out past ParmsSize, so the whole-struct calls write
		// outside this buffer (heap corruption, confirmed under -stompmalloc). Same pattern
		// as the engine's own UObject::CallFunctionByNameWithArguments.
		explicit FScopedBPCall(UFunction* InFunc) : Func(InFunc)
		{
			if (Func && Func->ParmsSize > 0)
			{
				Params = FMemory::Malloc(Func->ParmsSize, Func->GetMinAlignment());
				FMemory::Memzero(Params, Func->ParmsSize);
				for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->InitializeValue_InContainer(Params);
				}
			}
		}

		~FScopedBPCall()
		{
			if (Params)
			{
				for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->DestroyValue_InContainer(Params);
				}
				FMemory::Free(Params);
			}
		}

		template <typename T>
		bool SetParam(const TCHAR* Name, const T& Value)
		{
			if (!Func || !Params) return false;
			FProperty* Prop = Func->FindPropertyByName(Name);
			if (!Prop) return false;
			*Prop->ContainerPtrToValuePtr<T>(Params) = Value;
			return true;
		}

		template <typename T>
		bool GetParam(const TCHAR* Name, T& OutValue) const
		{
			if (!Func || !Params) return false;
			FProperty* Prop = Func->FindPropertyByName(Name);
			if (!Prop) return false;
			OutValue = *Prop->ContainerPtrToValuePtr<T>(Params);
			return true;
		}

		void Call(UObject* Obj)
		{
			if (Func)
			{
				Obj->ProcessEvent(Func, Params);
			}
		}
	};

	/**
	 * BP_BuildManager's cells are signed world-space cells (GetCellFromWorldLocation is
	 * round(World / GridCellSizeXY), no origin offset), so "0..GridSize" is not a validity
	 * range. Validity is whatever the Build Tool itself uses: CheckCellInBuilableBoundary.
	 */
	bool IsCellBuildable(UObject* BuildManagerRef, const FIntPoint& Cell, const TCHAR* CallerName)
	{
		UFunction* CheckFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("CheckCellInBuilableBoundary"), CallerName);
		if (!CheckFunc)
		{
			return false;
		}

		FScopedBPCall Call(CheckFunc);
		Call.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		Call.Call(BuildManagerRef);

		const FBoolProperty* SuccessProp = CastField<FBoolProperty>(CheckFunc->FindPropertyByName(TEXT("Success")));
		if (!SuccessProp || !Call.Params)
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("%s: CheckCellInBuilableBoundary has no bool 'Success' output - BP_BuildManager may have been refactored."), CallerName);
			return false;
		}
		return SuccessProp->GetPropertyValue_InContainer(Call.Params);
	}
}

bool UPreciseTransformLibrary::ParseCoordinateInput(const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutLocation)
{
	OutLocation = FVector::ZeroVector;

	float X = 0.f, Y = 0.f, Z = 0.f;
	const bool bXOk = FDefaultValueHelper::ParseFloat(XStr, X);
	const bool bYOk = FDefaultValueHelper::ParseFloat(YStr, Y);
	const bool bZOk = FDefaultValueHelper::ParseFloat(ZStr, Z);

	if (!bXOk || !bYOk || !bZOk)
	{
		UE_LOG(LogPreciseTransform, Warning,
			TEXT("ParseCoordinateInput: failed to parse - X('%s')=%s, Y('%s')=%s, Z('%s')=%s"),
			*XStr, bXOk ? TEXT("OK") : TEXT("INVALID"),
			*YStr, bYOk ? TEXT("OK") : TEXT("INVALID"),
			*ZStr, bZOk ? TEXT("OK") : TEXT("INVALID"));
		return false;
	}

	OutLocation = FVector(X, Y, Z);
	return true;
}

void UPreciseTransformLibrary::FormatLocationAsStrings(FVector Location, FString& OutXStr, FString& OutYStr, FString& OutZStr)
{
	OutXStr = FString::SanitizeFloat(Location.X);
	OutYStr = FString::SanitizeFloat(Location.Y);
	OutZStr = FString::SanitizeFloat(Location.Z);
}

bool UPreciseTransformLibrary::ValidateAndSnapToGrid(UObject* BuildManagerRef, FVector TypedLocation, FIntPoint& OutCell, FVector& OutSnappedWorldLocation)
{
	OutCell = FIntPoint::ZeroValue;
	OutSnappedWorldLocation = TypedLocation;

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ValidateAndSnapToGrid: BuildManagerRef is null/invalid."));
		return false;
	}

	UFunction* WorldToCellFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("GetCellFromWorldLocation"), TEXT("ValidateAndSnapToGrid"));
	UFunction* CellToCenterFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("GetCellCenter"), TEXT("ValidateAndSnapToGrid"));
	if (!WorldToCellFunc || !CellToCenterFunc)
	{
		return false;
	}

	FIntPoint Cell = FIntPoint::ZeroValue;
	{
		FScopedBPCall Call(WorldToCellFunc);
		Call.SetParam<FVector>(TEXT("Location"), TypedLocation);
		Call.Call(BuildManagerRef);
		if (!Call.GetParam<FIntPoint>(TEXT("Cell"), Cell))
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("ValidateAndSnapToGrid: GetCellFromWorldLocation has no 'Cell' output param - BP_BuildManager may have been refactored."));
			return false;
		}
	}

	if (!IsCellBuildable(BuildManagerRef, Cell, TEXT("ValidateAndSnapToGrid")))
	{
		UE_LOG(LogPreciseTransform, Warning,
			TEXT("ValidateAndSnapToGrid: typed location (%.1f, %.1f, %.1f) resolves to cell (%d, %d), outside the buildable boundary. Move rejected."),
			TypedLocation.X, TypedLocation.Y, TypedLocation.Z, Cell.X, Cell.Y);
		return false;
	}

	FVector2D SnappedXY = FVector2D::ZeroVector;
	{
		FScopedBPCall Call(CellToCenterFunc);
		Call.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		Call.Call(BuildManagerRef);
		if (!Call.GetParam<FVector2D>(TEXT("Location"), SnappedXY))
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("ValidateAndSnapToGrid: GetCellCenter has no 'Location' output param - BP_BuildManager may have been refactored."));
			return false;
		}
	}

	OutCell = Cell;
	OutSnappedWorldLocation = FVector(SnappedXY.X, SnappedXY.Y, TypedLocation.Z);
	return true;
}

bool UPreciseTransformLibrary::ApplyPreciseLocation(UObject* BuildManagerRef, AActor* ActivePlacer, FVector TypedLocation, FVector& OutAppliedLocation)
{
	OutAppliedLocation = TypedLocation;

	if (!IsValid(ActivePlacer))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ApplyPreciseLocation: ActivePlacer is null/invalid - is an object currently queued for placement?"));
		return false;
	}

	FIntPoint Cell;
	FVector SnappedLocation;
	if (!ValidateAndSnapToGrid(BuildManagerRef, TypedLocation, Cell, SnappedLocation))
	{
		return false;
	}

	UClass* BMClass = BuildManagerRef->GetClass();
	const FStructProperty* CellUnderCursorProp = FindFProperty<FStructProperty>(BMClass, TEXT("CellUnderCursor"));
	if (!CellUnderCursorProp)
	{
		UE_LOG(LogPreciseTransform, Error,
			TEXT("ApplyPreciseLocation: BuildManagerRef (class %s) is missing CellUnderCursor(IntPoint) - BP_BuildManager may have been renamed/refactored."),
			*BMClass->GetName());
		return false;
	}
	*CellUnderCursorProp->ContainerPtrToValuePtr<FIntPoint>(BuildManagerRef) = Cell;

	UFunction* BuildObjectFunc = FindBPFunctionChecked(ActivePlacer, TEXT("BuildObject"), TEXT("ApplyPreciseLocation"));
	if (!BuildObjectFunc)
	{
		return false;
	}

	// Synchronous, same call: no Tick can run between the CellUnderCursor
	// write above and this call, so nothing overwrites it first. See header
	// comment for why that ordering matters.
	ActivePlacer->ProcessEvent(BuildObjectFunc, nullptr);

	OutAppliedLocation = SnappedLocation;
	return true;
}

bool UPreciseTransformLibrary::ApplyPreciseLocationFromStrings(UObject* BuildManagerRef, AActor* ActivePlacer, const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutAppliedLocation)
{
	OutAppliedLocation = FVector::ZeroVector;

	FVector TypedLocation;
	if (!ParseCoordinateInput(XStr, YStr, ZStr, TypedLocation))
	{
		return false;
	}

	return ApplyPreciseLocation(BuildManagerRef, ActivePlacer, TypedLocation, OutAppliedLocation);
}

bool UPreciseTransformLibrary::RepositionPlacedObject(UObject* BuildManagerRef, AActor* PlacedObject, FVector TypedLocation, FVector& OutAppliedLocation)
{
	OutAppliedLocation = TypedLocation;

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("RepositionPlacedObject: BuildManagerRef is null/invalid."));
		return false;
	}
	if (!IsValid(PlacedObject))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("RepositionPlacedObject: PlacedObject is null/invalid."));
		return false;
	}

	// Resolves + bounds-checks the typed location's CENTER cell only. The full
	// multi-cell footprint (below) still needs its own bounds/occupancy pass -
	// an object's footprint can extend out of bounds even when its center cell
	// is valid. SnappedCenterLocation itself is discarded: GetCellCenter gives
	// a single cell's center, not a multi-cell footprint's true visual center,
	// which is instead recomputed correctly further down via GetCenterOfRectangularArea.
	FIntPoint NewCenterCell;
	FVector SnappedCenterLocation;
	if (!ValidateAndSnapToGrid(BuildManagerRef, TypedLocation, NewCenterCell, SnappedCenterLocation))
	{
		return false;
	}

	UClass* BMClass = BuildManagerRef->GetClass();
	UClass* ObjClass = PlacedObject->GetClass();

	// See header comment: these four names/types must match exactly.
	const FMapProperty* OccupancyProp = FindFProperty<FMapProperty>(BMClass, TEXT("OccupancyData"));
	const FStructProperty* ObjectSizeProp = FindFProperty<FStructProperty>(ObjClass, TEXT("ObjectSize"));
	const FArrayProperty* OccupiedCellsProp = FindFProperty<FArrayProperty>(ObjClass, TEXT("OccupiedCells"));
	const FStructProperty* OccupiedCenterCellProp = FindFProperty<FStructProperty>(ObjClass, TEXT("OccupiedCenterCell"));

	if (!OccupancyProp || !ObjectSizeProp || !OccupiedCellsProp || !OccupiedCenterCellProp)
	{
		UE_LOG(LogPreciseTransform, Error,
			TEXT("RepositionPlacedObject: missing an expected property - BuildManagerRef.OccupancyData(map): %s, PlacedObject.ObjectSize(IntPoint): %s, PlacedObject.OccupiedCells(array): %s, PlacedObject.OccupiedCenterCell(IntPoint): %s. A Blueprint may have been renamed/refactored."),
			OccupancyProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			ObjectSizeProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			OccupiedCellsProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			OccupiedCenterCellProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"));
		return false;
	}

	const FIntPoint ObjectSize = *ObjectSizeProp->ContainerPtrToValuePtr<FIntPoint>(PlacedObject);
	const TArray<FIntPoint> OldOccupiedCells = *OccupiedCellsProp->ContainerPtrToValuePtr<TArray<FIntPoint>>(PlacedObject);
	const TMap<FIntPoint, int32>& OccupancyMap = *OccupancyProp->ContainerPtrToValuePtr<TMap<FIntPoint, int32>>(BuildManagerRef);

	UFunction* GetCellsFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("GetCellsInRectangularArea"), TEXT("RepositionPlacedObject"));
	UFunction* GetCenterFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("GetCenterOfRectangularArea"), TEXT("RepositionPlacedObject"));
	UFunction* SetOccupancyFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("SetOccupancyData"), TEXT("RepositionPlacedObject"));
	UFunction* SetObjectDataFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("SetObjectData"), TEXT("RepositionPlacedObject"));
	UFunction* RemoveObjectDataFunc = FindBPFunctionChecked(BuildManagerRef, TEXT("RemoveObjectData"), TEXT("RepositionPlacedObject"));
	if (!GetCellsFunc || !GetCenterFunc || !SetOccupancyFunc || !SetObjectDataFunc || !RemoveObjectDataFunc)
	{
		return false;
	}

	TArray<FIntPoint> NewCells;
	{
		FScopedBPCall Call(GetCellsFunc);
		Call.SetParam<FIntPoint>(TEXT("CenterAreaCell"), NewCenterCell);
		Call.SetParam<FIntPoint>(TEXT("AreaSize"), ObjectSize);
		Call.Call(BuildManagerRef);
		if (!Call.GetParam<TArray<FIntPoint>>(TEXT("Cells"), NewCells))
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("RepositionPlacedObject: GetCellsInRectangularArea has no 'Cells' output param - BP_BuildManager may have been refactored."));
			return false;
		}
	}

	// Validate the ENTIRE new footprint before touching anything: buildable boundary,
	// then no conflicting occupancy from another object. A cell already in this
	// object's OWN OldOccupiedCells is fine - it's "occupied" by itself.
	for (const FIntPoint& Cell : NewCells)
	{
		if (!IsCellBuildable(BuildManagerRef, Cell, TEXT("RepositionPlacedObject")))
		{
			UE_LOG(LogPreciseTransform, Warning,
				TEXT("RepositionPlacedObject: target footprint cell (%d, %d) is outside the buildable boundary. Move rejected, object untouched."),
				Cell.X, Cell.Y);
			return false;
		}
		if (OccupancyMap.Contains(Cell) && !OldOccupiedCells.Contains(Cell))
		{
			UE_LOG(LogPreciseTransform, Warning,
				TEXT("RepositionPlacedObject: target cell (%d, %d) is already occupied by another object. Move rejected, object untouched."),
				Cell.X, Cell.Y);
			return false;
		}
	}

	// Release old cells - mirrors BP_PlaceableObject_Base_Class::DestroyPlaceableObject's
	// per-cell SetOccupancyData(false) + RemoveObjectData loop exactly.
	for (const FIntPoint& Cell : OldOccupiedCells)
	{
		FScopedBPCall FreeCall(SetOccupancyFunc);
		FreeCall.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		FreeCall.SetParam<bool>(TEXT("Occupied"), false);
		FreeCall.Call(BuildManagerRef);

		FScopedBPCall RemoveCall(RemoveObjectDataFunc);
		RemoveCall.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		RemoveCall.Call(BuildManagerRef);
	}

	// Register new cells - mirrors BP_PlaceableObject_Base_Class::SetupPlaceableObject's
	// per-cell SetOccupancyData(true) + SetObjectData loop exactly.
	for (const FIntPoint& Cell : NewCells)
	{
		FScopedBPCall OccupyCall(SetOccupancyFunc);
		OccupyCall.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		OccupyCall.SetParam<bool>(TEXT("Occupied"), true);
		OccupyCall.Call(BuildManagerRef);

		FScopedBPCall ObjDataCall(SetObjectDataFunc);
		ObjDataCall.SetParam<FIntPoint>(TEXT("Cell"), Cell);
		ObjDataCall.SetParam<UObject*>(TEXT("PlacealeObject"), PlacedObject);
		ObjDataCall.Call(BuildManagerRef);
	}

	*OccupiedCellsProp->ContainerPtrToValuePtr<TArray<FIntPoint>>(PlacedObject) = NewCells;
	*OccupiedCenterCellProp->ContainerPtrToValuePtr<FIntPoint>(PlacedObject) = NewCenterCell;

	FVector2D NewCenterXY = FVector2D::ZeroVector;
	{
		FScopedBPCall Call(GetCenterFunc);
		Call.SetParam<FIntPoint>(TEXT("AreaCenterCell"), NewCenterCell);
		Call.SetParam<FIntPoint>(TEXT("AreaSize"), ObjectSize);
		Call.Call(BuildManagerRef);
		if (!Call.GetParam<FVector2D>(TEXT("Center"), NewCenterXY))
		{
			UE_LOG(LogPreciseTransform, Error,
				TEXT("RepositionPlacedObject: GetCenterOfRectangularArea has no 'Center' output param - BP_BuildManager may have been refactored. Occupancy data is ALREADY committed at this point; actor location was NOT updated - grid state may be inconsistent with visuals, investigate immediately."));
			return false;
		}
	}

	const FVector NewWorldLocation(NewCenterXY.X, NewCenterXY.Y, TypedLocation.Z);

	// Placeable roots are Static by default (BP_PlaceableObject_Base_Class.DefaultSceneRoot),
	// and SetActorLocation refuses to move a Static root at runtime. Occupancy is already
	// committed above, so the move must not be allowed to silently no-op here. Left Movable
	// afterwards: every caller (XYZ fields, gizmo drag) needs it again on the next move.
	if (USceneComponent* Root = PlacedObject->GetRootComponent())
	{
		if (Root->Mobility != EComponentMobility::Movable)
		{
			Root->SetMobility(EComponentMobility::Movable);
		}
	}
	PlacedObject->SetActorLocation(NewWorldLocation);

	UE_LOG(LogPreciseTransform, Log,
		TEXT("RepositionPlacedObject: SUCCESS - moved '%s' to (%.1f, %.1f, %.1f), center cell (%d, %d)."),
		*PlacedObject->GetName(), NewWorldLocation.X, NewWorldLocation.Y, NewWorldLocation.Z, NewCenterCell.X, NewCenterCell.Y);

	OutAppliedLocation = NewWorldLocation;
	return true;
}

bool UPreciseTransformLibrary::RepositionPlacedObjectFromStrings(UObject* BuildManagerRef, AActor* PlacedObject, const FString& XStr, const FString& YStr, const FString& ZStr, FVector& OutAppliedLocation)
{
	OutAppliedLocation = FVector::ZeroVector;

	FVector TypedLocation;
	if (!ParseCoordinateInput(XStr, YStr, ZStr, TypedLocation))
	{
		return false;
	}

	return RepositionPlacedObject(BuildManagerRef, PlacedObject, TypedLocation, OutAppliedLocation);
}

bool UPreciseTransformLibrary::ApplyBuildableBoundaryFromFloorTags(UObject* BuildManagerRef, FVector2D& OutMin, FVector2D& OutMax, FName FloorTag)
{
	OutMin = FVector2D::ZeroVector;
	OutMax = FVector2D::ZeroVector;

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ApplyBuildableBoundaryFromFloorTags: BuildManagerRef is null/invalid."));
		return false;
	}

	UWorld* World = BuildManagerRef->GetWorld();
	if (!World)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("ApplyBuildableBoundaryFromFloorTags: BuildManagerRef has no valid World."));
		return false;
	}

	FBox CombinedBounds(ForceInit);
	int32 FloorActorCount = 0;
	int32 FloorComponentCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		if (Actor->ActorHasTag(FloorTag))
		{
			CombinedBounds += Actor->GetComponentsBoundingBox(/*bNonColliding=*/false);
			++FloorActorCount;
		}

		// Independent of the actor-tag check above: a component can mark its own
		// footprint as buildable floor even when the owning actor isn't tagged.
		// Same IsCollisionEnabled() requirement as GetComponentsBoundingBox(false)
		// uses internally, so this doesn't loosen what counts as "real floor".
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
		Actor->GetComponents(PrimitiveComponents);
		for (UPrimitiveComponent* Component : PrimitiveComponents)
		{
			if (Component && Component->ComponentHasTag(FloorTag) && Component->IsCollisionEnabled())
			{
				CombinedBounds += Component->Bounds.GetBox();
				++FloorComponentCount;
			}
		}
	}

	if ((FloorActorCount + FloorComponentCount) == 0 || !CombinedBounds.IsValid)
	{
		UE_LOG(LogPreciseTransform, Error,
			TEXT("ApplyBuildableBoundaryFromFloorTags: no actor or component tagged '%s' with valid bounds found in world '%s' - boundary left untouched."),
			*FloorTag.ToString(), *World->GetName());
		return false;
	}

	UClass* BMClass = BuildManagerRef->GetClass();
	const FStructProperty* MinProp = FindFProperty<FStructProperty>(BMClass, TEXT("BuilableBoundaryMin"));
	const FStructProperty* MaxProp = FindFProperty<FStructProperty>(BMClass, TEXT("BuilableBoundaryMax"));
	const FBoolProperty* EnabledProp = FindFProperty<FBoolProperty>(BMClass, TEXT("BuildableBoundaryEnabled"));

	if (!MinProp || !MaxProp || !EnabledProp)
	{
		UE_LOG(LogPreciseTransform, Error,
			TEXT("ApplyBuildableBoundaryFromFloorTags: BuildManagerRef (class %s) is missing an expected property - BuilableBoundaryMin(FVector2D): %s, BuilableBoundaryMax(FVector2D): %s, BuildableBoundaryEnabled(bool): %s. BP_BuildManager may have been renamed/refactored."),
			*BMClass->GetName(),
			MinProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			MaxProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			EnabledProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"));
		return false;
	}

	OutMin = FVector2D(CombinedBounds.Min.X, CombinedBounds.Min.Y);
	OutMax = FVector2D(CombinedBounds.Max.X, CombinedBounds.Max.Y);

	*MinProp->ContainerPtrToValuePtr<FVector2D>(BuildManagerRef) = OutMin;
	*MaxProp->ContainerPtrToValuePtr<FVector2D>(BuildManagerRef) = OutMax;
	EnabledProp->SetPropertyValue_InContainer(BuildManagerRef, true);

	UE_LOG(LogPreciseTransform, Log,
		TEXT("ApplyBuildableBoundaryFromFloorTags: combined bounds of %d actor(s) + %d component(s) tagged '%s' -> Min=(%.1f, %.1f) Max=(%.1f, %.1f). BuildableBoundaryEnabled set to true."),
		FloorActorCount, FloorComponentCount, *FloorTag.ToString(), OutMin.X, OutMin.Y, OutMax.X, OutMax.Y);

	return true;
}

bool UPreciseTransformLibrary::MergeDataTableIntoDynamicDataTable(UDynamicDataTable* Target, UDataTable* Source, bool Overwrite)
{
	if (!IsValid(Target))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("MergeDataTableIntoDynamicDataTable: Target is null/invalid."));
		return false;
	}
	if (!IsValid(Source))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("MergeDataTableIntoDynamicDataTable: Source is null/invalid."));
		return false;
	}

	UDynamicDatatableBPLibrary::AddRowsFromDataTable(Target->DynamicTable, Source, Overwrite);
	Target->MarkPackageDirty();

	UE_LOG(LogPreciseTransform, Log,
		TEXT("MergeDataTableIntoDynamicDataTable: merged '%s' into '%s' (Overwrite=%s) - table now has %d row(s)."),
		*Source->GetName(), *Target->GetName(), Overwrite ? TEXT("true") : TEXT("false"), Target->DynamicTable.RowNames.Num());

	for (const FName& RowName : Target->DynamicTable.RowNames)
	{
		const FString ClassStr = UDynamicDatatableBPLibrary::GetPropertyOfTableRowAsString(Target->DynamicTable, RowName, TEXT("PlaceableObjectClass"));
		const FString IconStr = UDynamicDatatableBPLibrary::GetPropertyOfTableRowAsString(Target->DynamicTable, RowName, TEXT("Icon"));
		UE_LOG(LogPreciseTransform, Log, TEXT("MergeDataTableIntoDynamicDataTable: verify row '%s' -> PlaceableObjectClass='%s' Icon='%s'"),
			*RowName.ToString(), *ClassStr, *IconStr);
	}

	return true;
}

bool UPreciseTransformLibrary::DumpDynamicDataTableRow(UDynamicDataTable* Target, FName RowName)
{
	if (!IsValid(Target))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("DumpDynamicDataTableRow: Target is null/invalid."));
		return false;
	}
	if (Target->DynamicTable.Type == nullptr)
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("DumpDynamicDataTableRow: Target '%s' has no row Type set."), *Target->GetName());
		return false;
	}
	if (!Target->DynamicTable.Map.Contains(RowName))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("DumpDynamicDataTableRow: row '%s' not found in '%s'."), *RowName.ToString(), *Target->GetName());
		return false;
	}

	UE_LOG(LogPreciseTransform, Log, TEXT("DumpDynamicDataTableRow: === '%s' row '%s' ==="), *Target->GetName(), *RowName.ToString());
	for (TFieldIterator<FProperty> It(Target->DynamicTable.Type); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property)
		{
			continue;
		}
		FString ValueStr;
		Property->ExportTextItem_Direct(ValueStr, Property->ContainerPtrToValuePtr<void>(Target->DynamicTable.Map[RowName].GetMutableMemory()), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
		UE_LOG(LogPreciseTransform, Log, TEXT("DumpDynamicDataTableRow:   %s = %s"), *Property->GetAuthoredName(), *ValueStr);
	}

	return true;
}

bool UPreciseTransformLibrary::DumpObjectProperty(UObject* Target, FName PropertyName)
{
	if (!IsValid(Target))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("DumpObjectProperty: Target is null/invalid."));
		return false;
	}
	FProperty* Property = Target->GetClass()->FindPropertyByName(PropertyName);
	if (!Property)
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("DumpObjectProperty: property '%s' not found on '%s' (class '%s')."),
			*PropertyName.ToString(), *Target->GetName(), *Target->GetClass()->GetName());
		return false;
	}
	FString ValueStr;
	Property->ExportTextItem_Direct(ValueStr, Property->ContainerPtrToValuePtr<void>(Target), nullptr, nullptr, EPropertyPortFlags::PPF_ExternalEditor);
	UE_LOG(LogPreciseTransform, Log, TEXT("DumpObjectProperty: %s (%s).%s = %s"), *Target->GetName(), *Target->GetClass()->GetName(), *PropertyName.ToString(), *ValueStr);
	return true;
}

UObject* UPreciseTransformLibrary::GetObjectProperty(UObject* Target, FName PropertyName)
{
	if (!IsValid(Target))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("GetObjectProperty: Target is null/invalid."));
		return nullptr;
	}
	FObjectProperty* Property = CastField<FObjectProperty>(Target->GetClass()->FindPropertyByName(PropertyName));
	if (!Property)
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("GetObjectProperty: object property '%s' not found on '%s' (class '%s')."),
			*PropertyName.ToString(), *Target->GetName(), *Target->GetClass()->GetName());
		return nullptr;
	}
	return Property->GetObjectPropertyValue_InContainer(Target);
}

bool UPreciseTransformLibrary::IsLocationOverWalkway(UObject* WorldContextObject, FVector Location, FName WalkwayTag)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("IsLocationOverWalkway: WorldContextObject is null/invalid."));
		return false;
	}

	UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("IsLocationOverWalkway: WorldContextObject has no valid World."));
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->ActorHasTag(WalkwayTag))
		{
			const FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
			if (Bounds.IsValid && Bounds.IsInsideXY(Location))
			{
				return true;
			}
		}
	}

	return false;
}

bool UPreciseTransformLibrary::IsLocationBlockingDoorClearance(UObject* WorldContextObject, FVector Location, FName DoorTag, float ClearanceRadius)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("IsLocationBlockingDoorClearance: WorldContextObject is null/invalid."));
		return false;
	}

	UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		UE_LOG(LogPreciseTransform, Error, TEXT("IsLocationBlockingDoorClearance: WorldContextObject has no valid World."));
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->ActorHasTag(DoorTag))
		{
			FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
			if (Bounds.IsValid)
			{
				Bounds = Bounds.ExpandBy(FVector(ClearanceRadius, ClearanceRadius, 0.0f));
				if (Bounds.IsInsideXY(Location))
				{
					return true;
				}
			}
		}
	}

	return false;
}

UStaticMesh* UPreciseTransformLibrary::GetClassDefaultStaticMesh(TSubclassOf<AActor> ActorClass)
{
	if (!ActorClass)
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("GetClassDefaultStaticMesh: ActorClass is null."));
		return nullptr;
	}

	auto MeshOf = [](const UObject* Template) -> UStaticMesh*
	{
		const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Template);
		return SMC ? SMC->GetStaticMesh() : nullptr;
	};

	for (UClass* Class = ActorClass; Class; Class = Class->GetSuperClass())
	{
		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
		if (!BPGC)
		{
			continue;
		}
		// A subclass that re-meshes a component it inherited stores that as an override record, not an SCS node.
		if (UInheritableComponentHandler* Handler = BPGC->GetInheritableComponentHandler())
		{
			for (auto It = Handler->CreateRecordIterator(); It; ++It)
			{
				if (UStaticMesh* Mesh = MeshOf(It->ComponentTemplate))
				{
					return Mesh;
				}
			}
		}
		if (BPGC->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
			{
				if (UStaticMesh* Mesh = Node ? MeshOf(Node->ComponentTemplate) : nullptr)
				{
					return Mesh;
				}
			}
		}
	}

	TInlineComponentArray<UStaticMeshComponent*> NativeComponents;
	ActorClass->GetDefaultObject<AActor>()->GetComponents(NativeComponents);
	for (const UStaticMeshComponent* SMC : NativeComponents)
	{
		if (UStaticMesh* Mesh = MeshOf(SMC))
		{
			return Mesh;
		}
	}

	UE_LOG(LogPreciseTransform, Warning, TEXT("GetClassDefaultStaticMesh: no StaticMeshComponent with a mesh set on '%s'."), *ActorClass->GetName());
	return nullptr;
}

bool UPreciseTransformLibrary::ApplyPlacementYaw(AActor* Placeable, float Yaw)
{
	if (!IsValid(Placeable))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ApplyPlacementYaw: Placeable is null/invalid."));
		return false;
	}
	if (FMath::IsNearlyZero(Yaw))
	{
		return true;
	}

	const UStaticMesh* GhostMesh = GetClassDefaultStaticMesh(Placeable->GetClass());
	if (!GhostMesh)
	{
		return false;
	}

	bool bRotatedAny = false;
	TInlineComponentArray<UStaticMeshComponent*> Components;
	Placeable->GetComponents(Components);
	for (UStaticMeshComponent* SMC : Components)
	{
		if (SMC && SMC->GetStaticMesh() == GhostMesh)
		{
			SMC->AddWorldRotation(FRotator(0.0f, Yaw, 0.0f));
			bRotatedAny = true;
		}
	}

	if (!bRotatedAny)
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ApplyPlacementYaw: no component on '%s' shows its ghost mesh '%s'."),
			*Placeable->GetName(), *GhostMesh->GetName());
	}
	return bRotatedAny;
}

namespace
{
	const FName GhostMeshComponentName(TEXT("GhostMeshComponent"));
	const FName RotationPivotName(TEXT("RotationPivot"));

	UStaticMeshComponent* FindGhostMeshComponent(const AActor* Placer)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components;
		Placer->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			if (Component && Component->GetFName() == GhostMeshComponentName)
			{
				return Component;
			}
		}
		return nullptr;
	}
}

bool UPreciseTransformLibrary::ShowPlacerGhostMesh(AActor* Placer, UStaticMesh* Mesh)
{
	if (!IsValid(Placer))
	{
		UE_LOG(LogPreciseTransform, Warning, TEXT("ShowPlacerGhostMesh: Placer is null/invalid."));
		return false;
	}

	UStaticMeshComponent* Ghost = FindGhostMeshComponent(Placer);
	if (!Ghost)
	{
		USceneComponent* Pivot = nullptr;
		TInlineComponentArray<USceneComponent*> SceneComponents;
		Placer->GetComponents(SceneComponents);
		for (USceneComponent* Candidate : SceneComponents)
		{
			if (Candidate && Candidate->GetFName() == RotationPivotName)
			{
				Pivot = Candidate;
				break;
			}
		}
		if (!Pivot)
		{
			UE_LOG(LogPreciseTransform, Error, TEXT("ShowPlacerGhostMesh: '%s' has no scene component named '%s' - no ghost mesh shown."),
				*Placer->GetName(), *RotationPivotName.ToString());
			return false;
		}

		Ghost = NewObject<UStaticMeshComponent>(Placer, GhostMeshComponentName);
		Ghost->SetMobility(EComponentMobility::Movable);
		Ghost->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ghost->SetGenerateOverlapEvents(false);
		Ghost->SetCastShadow(false);
		Ghost->SetupAttachment(Pivot);
		Ghost->RegisterComponent();
		Placer->AddInstanceComponent(Ghost);
	}

	Ghost->SetStaticMesh(Mesh);
	Ghost->SetVisibility(Mesh != nullptr);
	return Mesh != nullptr;
}

float UPreciseTransformLibrary::GetPlacerGhostYaw(AActor* Placer)
{
	if (!IsValid(Placer))
	{
		return 0.f;
	}
	const UStaticMeshComponent* Ghost = FindGhostMeshComponent(Placer);
	return Ghost ? Ghost->GetComponentRotation().Yaw : Placer->GetActorRotation().Yaw;
}
