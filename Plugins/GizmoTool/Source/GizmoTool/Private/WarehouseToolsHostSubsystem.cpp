// Copyright DamenTools internal. Gizmo Tool plugin.

#include "WarehouseToolsHostSubsystem.h"
#include "GizmoTool.h"
#include "InteractiveToolsContext.h"
#include "InteractiveToolManager.h"
#include "InteractiveGizmoManager.h"
#include "ToolContextInterfaces.h"
#include "InputRouter.h"
#include "InputState.h"
#include "ContextObjectStore.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "BaseGizmos/CombinedTransformGizmo.h"
#include "BaseGizmos/TransformProxy.h"
#include "BaseGizmos/GizmoViewContext.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "SceneView.h"
#include "UnrealClient.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Materials/Material.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "BaseGizmos/ViewAdjustedStaticMeshGizmoComponent.h"
#include "PreciseTransformLibrary.h"

namespace
{
	// Move arrows X/Y/Z + the ground (XY) plane handle + the yaw ring. Scale is deferred
	// (needs occupancy-footprint recalculation); pitch/roll rings are excluded by design.
	constexpr ETransformGizmoSubElements WarehouseGizmoElements =
		ETransformGizmoSubElements::TranslateAllAxes
		| ETransformGizmoSubElements::TranslatePlaneXY
		| ETransformGizmoSubElements::RotateAxisZ;
}

/** Scene/camera state the ITF asks for, answered from the game world and player 0's camera. */
class FWarehouseToolsQueriesAPI : public IToolsContextQueriesAPI
{
public:
	explicit FWarehouseToolsQueriesAPI(UWorld* InWorld) : World(InWorld) {}

	virtual UWorld* GetCurrentEditingWorld() const override { return World.Get(); }

	virtual void GetCurrentSelectionState(FToolBuilderState& StateOut) const override
	{
		// Selection is owned by BP_BuildManager, not the ITF; the gizmo target is set explicitly.
		StateOut.World = World.Get();
		StateOut.ToolManager = ToolsContext ? ToolsContext->ToolManager.Get() : nullptr;
		StateOut.GizmoManager = ToolsContext ? ToolsContext->GizmoManager.Get() : nullptr;
	}

	virtual void GetCurrentViewState(FViewCameraState& StateOut) const override
	{
		const APlayerController* PC = World.IsValid() ? World->GetFirstPlayerController() : nullptr;
		if (!PC || !PC->PlayerCameraManager)
		{
			return;
		}
		StateOut.Position = PC->PlayerCameraManager->GetCameraLocation();
		StateOut.Orientation = PC->PlayerCameraManager->GetCameraRotation().Quaternion();
		StateOut.HorizontalFOVDegrees = PC->PlayerCameraManager->GetFOVAngle();
		StateOut.bIsOrthographic = false;
		StateOut.bIsVR = false;
		int32 SizeX = 0, SizeY = 0;
		PC->GetViewportSize(SizeX, SizeY);
		StateOut.AspectRatio = (SizeY > 0) ? float(SizeX) / float(SizeY) : 1.f;
	}

	virtual EToolContextCoordinateSystem GetCurrentCoordinateSystem() const override { return EToolContextCoordinateSystem::World; }
	virtual EToolContextTransformGizmoMode GetCurrentTransformGizmoMode() const override { return GizmoMode; }

	virtual UMaterialInterface* GetStandardMaterial(EStandardToolContextMaterials MaterialType) const override
	{
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}

	virtual FViewport* GetHoveredViewport() const override { return GetGameViewport(); }
	virtual FViewport* GetFocusedViewport() const override { return GetGameViewport(); }

	UInteractiveToolsContext* ToolsContext = nullptr;
	EToolContextTransformGizmoMode GizmoMode = EToolContextTransformGizmoMode::Translation;

private:
	TWeakObjectPtr<UWorld> World;

	FViewport* GetGameViewport() const
	{
		return (World.IsValid() && World->GetGameViewport()) ? World->GetGameViewport()->Viewport : nullptr;
	}
};

/** No undo in the planner: transactions are no-ops, messages go to the log. */
class FWarehouseToolsTransactionsAPI : public IToolsContextTransactionsAPI
{
public:
	virtual void DisplayMessage(const FText& Message, EToolMessageLevel Level) override
	{
		UE_LOG(LogGizmoTool, Log, TEXT("ToolsContext: %s"), *Message.ToString());
	}
	virtual void PostInvalidation() override {}
	virtual void BeginUndoTransaction(const FText& Description) override {}
	virtual void EndUndoTransaction() override {}
	virtual void AppendChange(UObject* TargetObject, TUniquePtr<FToolCommandChange> Change, const FText& Description) override {}
	virtual bool RequestSelectionChange(const FSelectedObjectsChangeList& SelectionChange) override { return false; }
};

bool UWarehouseToolsHostSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UWarehouseToolsHostSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	QueriesAPI = MakeShared<FWarehouseToolsQueriesAPI>(&InWorld);
	TransactionsAPI = MakeShared<FWarehouseToolsTransactionsAPI>();

	ToolsContext = NewObject<UInteractiveToolsContext>(this);
	ToolsContext->Initialize(QueriesAPI.Get(), TransactionsAPI.Get());
	QueriesAPI->ToolsContext = ToolsContext;

	const bool bGizmoTypesRegistered = UE::TransformGizmoUtil::RegisterTransformGizmoContextObject(ToolsContext);

	UE_LOG(LogGizmoTool, Log,
		TEXT("UWarehouseToolsHostSubsystem: tools context up in %s (InputRouter=%s ToolManager=%s GizmoManager=%s TransformGizmoContext=%s)"),
		*InWorld.GetName(),
		ToolsContext->InputRouter ? TEXT("OK") : TEXT("null"),
		ToolsContext->ToolManager ? TEXT("OK") : TEXT("null"),
		ToolsContext->GizmoManager ? TEXT("OK") : TEXT("null"),
		bGizmoTypesRegistered ? TEXT("OK") : TEXT("FAILED"));

	if (bGizmoTypesRegistered)
	{
		CreateTransformGizmo();
	}
}

void UWarehouseToolsHostSubsystem::CreateTransformGizmo()
{
	TransformGizmo = UE::TransformGizmoUtil::CreateCustomTransformGizmo(ToolsContext->GizmoManager, WarehouseGizmoElements, this);
	if (!TransformGizmo)
	{
		UE_LOG(LogGizmoTool, Error, TEXT("UWarehouseToolsHostSubsystem: CreateCustomTransformGizmo returned null - no gizmo this session."));
		return;
	}

	GizmoProxy = NewObject<UTransformProxy>(this);
	GizmoProxy->OnBeginTransformEdit.AddUObject(this, &UWarehouseToolsHostSubsystem::OnGizmoBeginEdit);
	GizmoProxy->OnTransformChanged.AddUObject(this, &UWarehouseToolsHostSubsystem::OnGizmoTransformChanged);
	GizmoProxy->OnEndTransformEdit.AddUObject(this, &UWarehouseToolsHostSubsystem::OnGizmoEndEdit);
	TransformGizmo->SetActiveTarget(GizmoProxy);
	TransformGizmo->SetVisibility(false);
	ApplyUndimmedGizmoMaterials();

	UE_LOG(LogGizmoTool, Log, TEXT("UWarehouseToolsHostSubsystem: engine transform gizmo created (translate XYZ + XY plane + yaw ring), hidden until something is selected."));
}

void UWarehouseToolsHostSubsystem::ApplyUndimmedGizmoMaterials()
{
	// The engine handle material (DisableDepthTest=true) still hides handles two ways, via static
	// switches that can only be changed on a MaterialInstanceConstant asset:
	//  - DimRegularOcclusion: anything behind scene geometry becomes a sparse dither (invisible on
	//    thin handles);
	//  - OccludeByCustomDepth: anything behind a CustomDepth writer is hidden - and every placeable
	//    renders CustomDepth, so a gizmo behind e.g. the Bank vanished completely.
	// MI_GizmoAlwaysOnTop is the engine material with both switches off. (The engine's own
	// bDimOccluded=false path can't be used: it loads _NotDimmed as a UMaterial and gets null,
	// and _NotDimmed keeps OccludeByCustomDepth on anyway.)
	UMaterialInterface* AlwaysOnTop = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/GizmoTool/MI_GizmoAlwaysOnTop.MI_GizmoAlwaysOnTop"));
	if (!AlwaysOnTop)
	{
		UE_LOG(LogGizmoTool, Warning, TEXT("UWarehouseToolsHostSubsystem: /Game/GizmoTool/MI_GizmoAlwaysOnTop missing - falling back to _NotDimmed (gizmo will still hide behind placed objects)."));
		AlwaysOnTop = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Engine/InteractiveToolsFramework/Materials/GizmoComponentMaterial_NotDimmed.GizmoComponentMaterial_NotDimmed"));
	}
	ACombinedTransformGizmoActor* GizmoActor = TransformGizmo ? TransformGizmo->GetGizmoActor() : nullptr;
	if (!AlwaysOnTop || !GizmoActor)
	{
		UE_LOG(LogGizmoTool, Warning, TEXT("UWarehouseToolsHostSubsystem: no always-on-top gizmo material or gizmo actor - handles keep the default dimmed material."));
		return;
	}

	const FLinearColor HoverColor(FColor::Yellow); // same hover colour the engine uses
	int32 NumSwapped = 0;
	TInlineComponentArray<UViewAdjustedStaticMeshGizmoComponent*> Handles;
	GizmoActor->GetComponents(Handles);
	for (UViewAdjustedStaticMeshGizmoComponent* Handle : Handles)
	{
		FLinearColor AxisColor = FLinearColor::White;
		if (const UMaterialInstanceDynamic* Current = Cast<UMaterialInstanceDynamic>(Handle->GetMaterial(0)))
		{
			Current->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("GizmoColor")), AxisColor);
		}

		UMaterialInstanceDynamic* Normal = UMaterialInstanceDynamic::Create(AlwaysOnTop, Handle);
		Normal->SetVectorParameterValue(TEXT("GizmoColor"), AxisColor);
		Handle->SetAllMaterials(Normal);

		UMaterialInstanceDynamic* Hover = UMaterialInstanceDynamic::Create(AlwaysOnTop, Handle);
		Hover->SetVectorParameterValue(TEXT("GizmoColor"), HoverColor);
		Handle->SetHoverOverrideMaterial(Hover);
		++NumSwapped;
	}
	UE_LOG(LogGizmoTool, Log, TEXT("UWarehouseToolsHostSubsystem: %d gizmo handle(s) switched to the undimmed always-on-top material."), NumSwapped);
}

void UWarehouseToolsHostSubsystem::UpdateGizmoModeFromKeys(APlayerController* PC)
{
	// Space toggles Move <-> Rotate (yaw ring). Not W/E/R (WASD pans the camera) and not 1/2 (both
	// bound in PC_Demo_PlayerCntroller). Space is unbound in the planner: its only bindings are
	// IMC_Default/IA_Jump (first-person template PC only), the unread legacy MoveUp axis, an
	// unreferenced ship BP and plugin demo characters. Scale is deferred.
	if (!PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		return;
	}
	if (ToolsContext->InputRouter && ToolsContext->InputRouter->HasActiveMouseCapture())
	{
		return; // never swap handles out from under an active drag
	}

	QueriesAPI->GizmoMode = (QueriesAPI->GizmoMode == EToolContextTransformGizmoMode::Rotation)
		? EToolContextTransformGizmoMode::Translation
		: EToolContextTransformGizmoMode::Rotation;
	UE_LOG(LogGizmoTool, Log, TEXT("UWarehouseToolsHostSubsystem: gizmo mode -> %s (Space)"),
		QueriesAPI->GizmoMode == EToolContextTransformGizmoMode::Rotation ? TEXT("Rotate") : TEXT("Move"));
}

void UWarehouseToolsHostSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!ToolsContext || !ToolsContext->ToolManager || !ToolsContext->GizmoManager)
	{
		return;
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		UpdateGizmoViewContext(PC);
		UpdateGizmoModeFromKeys(PC);
		SyncGizmoToSelection();
		FeedMouseInput(PC);
	}

	ToolsContext->ToolManager->Tick(DeltaTime);
	ToolsContext->GizmoManager->Tick(DeltaTime);
}

void UWarehouseToolsHostSubsystem::UpdateGizmoViewContext(APlayerController* PC)
{
	// Hit-testing and view-dependent handle sizing read UGizmoViewContext; in the editor a
	// viewport client refreshes it, at runtime nothing does unless we do it from the player's view.
	UGizmoViewContext* ViewContext = ToolsContext->ToolManager->GetContextObjectStore()->FindContext<UGizmoViewContext>();
	ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
	if (!ViewContext || !LocalPlayer || !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->Viewport)
	{
		return;
	}

	FViewport* Viewport = LocalPlayer->ViewportClient->Viewport;
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Viewport, GetWorld()->Scene, LocalPlayer->ViewportClient->EngineShowFlags).SetRealtimeUpdate(true));

	FVector ViewLocation;
	FRotator ViewRotation;
	// The view is owned (and freed) by ViewFamily.
	if (FSceneView* SceneView = LocalPlayer->CalcSceneView(&ViewFamily, ViewLocation, ViewRotation, Viewport))
	{
		ViewContext->ResetFromSceneView(*SceneView);
	}
}

void UWarehouseToolsHostSubsystem::FeedMouseInput(APlayerController* PC)
{
	UInputRouter* InputRouter = ToolsContext->InputRouter;
	float MouseX = 0.f, MouseY = 0.f;
	if (!InputRouter || !PC->GetMousePosition(MouseX, MouseY))
	{
		return;
	}

	FVector RayOrigin, RayDirection;
	if (!PC->DeprojectScreenPositionToWorld(MouseX, MouseY, RayOrigin, RayDirection))
	{
		return;
	}

	const bool bLeftDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bPressed = bLeftDown && !bLeftMouseWasDown;
	const bool bReleased = !bLeftDown && bLeftMouseWasDown;
	bLeftMouseWasDown = bLeftDown;

	FInputDeviceState Input;
	Input.InputDevice = EInputDevices::Mouse;
	Input.SetModifierKeyStates(PC->IsInputKeyDown(EKeys::LeftShift), PC->IsInputKeyDown(EKeys::LeftAlt),
		PC->IsInputKeyDown(EKeys::LeftControl), false);
	Input.Mouse.Position2D = FVector2D(MouseX, MouseY);
	Input.Mouse.Delta2D = Input.Mouse.Position2D - LastMousePosition;
	Input.Mouse.WorldRay = FRay(RayOrigin, RayDirection);
	Input.Mouse.Left.SetStates(bPressed, bLeftDown, bReleased);
	LastMousePosition = Input.Mouse.Position2D;

	// Same split the editor viewport uses: button activity (incl. every frame of a drag) goes
	// through PostInputEvent, plain mouse movement through the hover path.
	if (bPressed || bLeftDown || bReleased)
	{
		InputRouter->PostInputEvent(Input);
	}
	else
	{
		InputRouter->PostHoverInputEvent(Input);
	}
}

void UWarehouseToolsHostSubsystem::SyncGizmoToSelection()
{
	if (!TransformGizmo)
	{
		return;
	}

	// Never re-seat the gizmo in the middle of a drag.
	if (ToolsContext->InputRouter && ToolsContext->InputRouter->HasActiveMouseCapture())
	{
		return;
	}

	AActor* Selected = PollSelectedPlaceableObject();
	if (Selected != GizmoTarget.Get())
	{
		GizmoTarget = Selected;
		TransformGizmo->SetVisibility(Selected != nullptr);
		UE_LOG(LogGizmoTool, Log, TEXT("UWarehouseToolsHostSubsystem: gizmo target -> %s"), Selected ? *Selected->GetName() : TEXT("None"));
		LastSyncedTargetTransform = FTransform::Identity;
	}

	if (!Selected)
	{
		return;
	}

	// World-aligned frame at the object's location: arrows match the world X/Y/Z the XYZ panel
	// types in, and the yaw ring is applied as deltas (free rotation turns the mesh, not the
	// actor), so the gizmo's own rotation is reset to identity whenever it is re-seated. This
	// also snaps the gizmo back onto the (grid-snapped) object after every drag.
	const FTransform TargetTransform(FQuat::Identity, Selected->GetActorLocation());
	if (!TargetTransform.Equals(LastSyncedTargetTransform))
	{
		TransformGizmo->ReinitializeGizmoTransform(TargetTransform);
		LastSyncedTargetTransform = TargetTransform;
		LastProxyYaw = 0.0;
	}
}

void UWarehouseToolsHostSubsystem::OnGizmoBeginEdit(UTransformProxy* Proxy)
{
	AActor* Target = GizmoTarget.Get();
	if (!Target || !BuildManager.IsValid())
	{
		return;
	}

	bInGizmoEdit = true;
	LastProxyYaw = Proxy->GetTransform().Rotator().Yaw;
	DragLastZ = Target->GetActorLocation().Z;

	FVector Snapped;
	if (!UPreciseTransformLibrary::ValidateAndSnapToGrid(BuildManager.Get(), Target->GetActorLocation(), DragLastCell, Snapped))
	{
		// Object currently sits outside the buildable area; any valid cell the drag reaches counts as a change.
		DragLastCell = FIntPoint(MAX_int32, MAX_int32);
	}
}

void UWarehouseToolsHostSubsystem::OnGizmoTransformChanged(UTransformProxy* Proxy, FTransform NewTransform)
{
	AActor* Target = GizmoTarget.Get();
	if (!bInGizmoEdit || !Target || !BuildManager.IsValid())
	{
		return;
	}

	// Yaw ring -> the same free-rotation path as the C/Z keys (turns the placed mesh, footprint unchanged).
	const double Yaw = NewTransform.Rotator().Yaw;
	const double DeltaYaw = FRotator::NormalizeAxis(Yaw - LastProxyYaw);
	if (FMath::Abs(DeltaYaw) > 0.01)
	{
		UPreciseTransformLibrary::ApplyPlacementYaw(Target, DeltaYaw);
		LastProxyYaw = Yaw;
	}

	// Arrows / ground plane -> the same move path as the typed XYZ fields (grid snap, buildable
	// area, occupancy). Only re-applied when the snapped cell or the height actually changes.
	const FVector Proposed = NewTransform.GetLocation();
	FIntPoint Cell;
	FVector Snapped;
	if (!UPreciseTransformLibrary::ValidateAndSnapToGrid(BuildManager.Get(), Proposed, Cell, Snapped))
	{
		return;
	}
	if (Cell == DragLastCell && FMath::IsNearlyEqual(Proposed.Z, DragLastZ, 0.5))
	{
		return;
	}

	FVector Applied;
	if (UPreciseTransformLibrary::RepositionPlacedObject(BuildManager.Get(), Target, Proposed, Applied))
	{
		DragLastCell = Cell;
		DragLastZ = Proposed.Z;
	}
}

void UWarehouseToolsHostSubsystem::OnGizmoEndEdit(UTransformProxy* Proxy)
{
	bInGizmoEdit = false;
	// Force a re-seat next tick so the gizmo lands back on the snapped object.
	LastSyncedTargetTransform = FTransform(FQuat::Identity, FVector(TNumericLimits<float>::Max()));
}

bool UWarehouseToolsHostSubsystem::IsTransformGizmoUnderCursor(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	const UWarehouseToolsHostSubsystem* Host = World ? World->GetSubsystem<UWarehouseToolsHostSubsystem>() : nullptr;
	return Host && Host->IsGizmoUnderCursorInternal();
}

bool UWarehouseToolsHostSubsystem::IsGizmoUnderCursorInternal() const
{
	if (!TransformGizmo || !GizmoTarget.IsValid() || !ToolsContext)
	{
		return false;
	}
	if (ToolsContext->InputRouter && ToolsContext->InputRouter->HasActiveMouseCapture())
	{
		return true;
	}

	const AActor* GizmoActor = TransformGizmo->GetGizmoActor();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	float MouseX = 0.f, MouseY = 0.f;
	FVector RayOrigin, RayDirection;
	if (!GizmoActor || GizmoActor->IsHidden() || !PC || !PC->GetMousePosition(MouseX, MouseY)
		|| !PC->DeprojectScreenPositionToWorld(MouseX, MouseY, RayOrigin, RayDirection))
	{
		return false;
	}

	// Same per-component test the gizmo's own hit targets use (view-adjusted handle sizes included).
	const FVector RayEnd = RayOrigin + RayDirection * HALF_WORLD_MAX;
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(WarehouseGizmoUnderCursor), true);
	TInlineComponentArray<UPrimitiveComponent*> Components;
	GizmoActor->GetComponents(Components);
	for (UPrimitiveComponent* Component : Components)
	{
		FHitResult Hit;
		if (Component && Component->IsVisible() && Component->LineTraceComponent(Hit, RayOrigin, RayEnd, Params))
		{
			return true;
		}
	}
	return false;
}

AActor* UWarehouseToolsHostSubsystem::PollSelectedPlaceableObject()
{
	if (!BuildManager.IsValid())
	{
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (It->GetClass()->GetName() == TEXT("BP_BuildManager_C"))
			{
				BuildManager = *It;
				break;
			}
		}
		if (!BuildManager.IsValid())
		{
			return nullptr;
		}
	}

	UFunction* Func = BuildManager->FindFunction(TEXT("GetSelectedPlaceableObject"));
	if (!Func || Func->ParmsSize <= 0)
	{
		return nullptr;
	}

	// Parameters only - never UFunction::InitializeStruct, which also initializes the Blueprint
	// function's locals past ParmsSize (the heap corruption fixed in PreciseTransform's FScopedBPCall).
	uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Func->ParmsSize, Func->GetMinAlignment()));
	FMemory::Memzero(Params, Func->ParmsSize);
	for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params);
	}

	BuildManager->ProcessEvent(Func, Params);

	AActor* Result = nullptr;
	for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It))
		{
			if (ObjProp->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
			{
				Result = Cast<AActor>(ObjProp->GetObjectPropertyValue_InContainer(Params));
			}
		}
		It->DestroyValue_InContainer(Params);
	}
	return Result;
}

TStatId UWarehouseToolsHostSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWarehouseToolsHostSubsystem, STATGROUP_Tickables);
}

void UWarehouseToolsHostSubsystem::Deinitialize()
{
	ShutdownToolsContext();
	Super::Deinitialize();
}

void UWarehouseToolsHostSubsystem::ShutdownToolsContext()
{
	if (ToolsContext)
	{
		if (ToolsContext->GizmoManager)
		{
			ToolsContext->GizmoManager->DestroyAllGizmosByOwner(this);
		}
		TransformGizmo = nullptr;
		GizmoProxy = nullptr;
		UE::TransformGizmoUtil::DeregisterTransformGizmoContextObject(ToolsContext);
		ToolsContext->Shutdown();
		ToolsContext = nullptr;
		UE_LOG(LogGizmoTool, Log, TEXT("UWarehouseToolsHostSubsystem: tools context shut down."));
	}
	QueriesAPI.Reset();
	TransactionsAPI.Reset();
}
