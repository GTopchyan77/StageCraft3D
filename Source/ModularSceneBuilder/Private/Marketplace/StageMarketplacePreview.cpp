// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageMarketplacePreview.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "ModularSceneBuilder.h"
#include "PreviewScene.h"

namespace StageMarketplacePreviewImpl
{
	constexpr float FieldOfView = 50.f;
	constexpr float ExposureBias = 2.5f;

	/** Pitch stays above the floor and short of straight down, where yaw would spin the view. */
	constexpr double MinPitch = -85.0;
	constexpr double MaxPitch = -5.0;
	constexpr double DefaultYaw = -135.0;
	constexpr double DefaultPitch = -25.0;

	/** Each wheel step moves 12 % of the way; the camera stays between 0.3 and 6 stage radii. */
	constexpr double ZoomStep = 0.88;
	constexpr double MinDistanceRadii = 0.3;
	constexpr double MaxDistanceRadii = 6.0;

	/** The engine plane is 1 m; the floor is 300 x 300 m, larger than any stage the scene format accepts in practice. */
	constexpr double FloorScale = 300.0;
	const TCHAR* FloorMesh = TEXT("/Engine/BasicShapes/Plane.Plane");
	/** A plain grey surface: the engine default material is a fine checker that aliases into noise without temporal AA. */
	const TCHAR* FloorMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
}

FStageMarketplacePreviewStats StageMarketplacePreview::SpawnScene(UWorld& World, const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, UBaseItemData*>& Catalog,
	TArray<TObjectPtr<AActor>>& OutActors, FBox& OutBounds)
{
	FStageMarketplacePreviewStats Stats;
	for (const FStageItemSnapshot& Snapshot : Scene.Items)
	{
		UBaseItemData* Item = Catalog.FindRef(Snapshot.Item.ToSoftObjectPath());
		UClass* ActorClass = Item ? Item->ActorClass.LoadSynchronous() : nullptr;
		if (!ActorClass)
		{
			++Stats.NotInLibrary;
			continue;
		}

		// State first, so ApplyItemData builds the saved look (the order RestoreItem uses).
		AModularBaseActor* Actor = World.SpawnActorDeferred<AModularBaseActor>(ActorClass, Snapshot.Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Actor)
		{
			++Stats.NotInLibrary;
			continue;
		}
		Actor->RestoreSnapshotState(Snapshot);
		Actor->InitializeFromItemData(Item);
		Actor->FinishSpawning(Snapshot.Transform);
		OutActors.Add(Actor);
		OutBounds += Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		++Stats.Placed;
	}
	return Stats;
}

FStageMarketplacePreview::FStageMarketplacePreview() = default;

FStageMarketplacePreview::~FStageMarketplacePreview()
{
	Clear();
}

bool FStageMarketplacePreview::IsAvailable() const
{
	return FApp::CanEverRender();
}

FString FStageMarketplacePreview::GetReferencerName() const
{
	return TEXT("FStageMarketplacePreview");
}

void FStageMarketplacePreview::AddReferencedObjects(FReferenceCollector& Collector)
{
	// The preview scene keeps its world, and the world its actors and components; these are held here too so that the panel's
	// image and Clear never see a collected object.
	Collector.AddReferencedObject(Capture);
	Collector.AddReferencedObject(Floor);
	Collector.AddReferencedObject(Target);
	Collector.AddReferencedObjects(Actors);
}

bool FStageMarketplacePreview::EnsureScene()
{
	if (Scene.IsValid())
	{
		return true;
	}
	if (!IsAvailable())
	{
		return false;
	}

	// SetEditor(false): a GamePreview world, where no StageCraft world subsystem is created.
	Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
		.SetEditor(false)
		.SetCreateDefaultLighting(true)
		.SetLightRotation(FRotator(-50.f, 35.f, 0.f))
		.SetLightBrightness(10.f)
		.SetSkyBrightness(1.f)
		.SetCreatePhysicsScene(false)
		.AllowAudioPlayback(false));

	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, StageMarketplacePreviewImpl::FloorMesh))
	{
		Floor = NewObject<UStaticMeshComponent>(GetTransientPackage());
		Floor->SetStaticMesh(Plane);
		Floor->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, StageMarketplacePreviewImpl::FloorMaterial));
		Floor->SetMobility(EComponentMobility::Movable);
		Scene->AddComponent(Floor, FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(StageMarketplacePreviewImpl::FloorScale)));
	}

	Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->ClearColor = FLinearColor::Black;
	Target->InitAutoFormat(Width, Height);
	Target->UpdateResourceImmediate(/*bClearRenderTarget*/ true);

	Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->FOVAngle = StageMarketplacePreviewImpl::FieldOfView;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->TextureTarget = Target;
	// One still image per change: nothing that needs frame history (eye adaptation, temporal AA, motion blur, AO) or a scene
	// representation the preview world does not build (Lumen).
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetTemporalAA(false);
	Capture->ShowFlags.SetMotionBlur(false);
	// Screen-space AO is a noisy sampler meant to be smoothed over frames by temporal AA; a single still shows its grain.
	Capture->ShowFlags.SetAmbientOcclusion(false);
	Capture->ShowFlags.SetScreenSpaceReflections(false);
	Capture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
	Capture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
	Capture->PostProcessSettings.bOverride_ReflectionMethod = true;
	Capture->PostProcessSettings.ReflectionMethod = EReflectionMethod::None;
	Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
	Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
	Capture->PostProcessSettings.AutoExposureBias = StageMarketplacePreviewImpl::ExposureBias;
	Scene->AddComponent(Capture, FTransform::Identity);
	return true;
}

FStageMarketplacePreviewStats FStageMarketplacePreview::Show(const FStageSceneDocument& SceneDocument, const TMap<FSoftObjectPath, UBaseItemData*>& Catalog)
{
	FStageMarketplacePreviewStats Stats;
	Clear();
	if (!EnsureScene())
	{
		return Stats;
	}

	FBox Bounds(ForceInit);
	Stats = StageMarketplacePreview::SpawnScene(*Scene->GetWorld(), SceneDocument, Catalog, Actors, Bounds);

	if (Bounds.IsValid)
	{
		Focus = Bounds.GetCenter();
		Radius = FMath::Max(Bounds.GetExtent().Size(), 100.0);
		if (Floor)
		{
			// Just under the lowest item, so decks stand on it and nothing is cut by it.
			Floor->SetWorldLocation(FVector(Focus.X, Focus.Y, FMath::Min(Bounds.Min.Z, 0.0) - 1.0));
		}
	}
	else
	{
		Focus = FVector::ZeroVector;
		Radius = 1000.0;
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace preview: %d placed, %d not in the library, radius %.0f cm."), Stats.Placed, Stats.NotInLibrary, Radius);
	Frame();
	return Stats;
}

void FStageMarketplacePreview::Clear()
{
	for (AActor* Actor : Actors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	Actors.Reset();
}

void FStageMarketplacePreview::Frame()
{
	using namespace StageMarketplacePreviewImpl;
	Yaw = DefaultYaw;
	Pitch = DefaultPitch;
	Distance = Radius / FMath::Sin(FMath::DegreesToRadians(FieldOfView * 0.5)) * 1.1;
	Render();
}

void FStageMarketplacePreview::Orbit(float DeltaYaw, float DeltaPitch)
{
	using namespace StageMarketplacePreviewImpl;
	Yaw = FRotator::NormalizeAxis(Yaw + DeltaYaw);
	Pitch = FMath::Clamp(Pitch + DeltaPitch, MinPitch, MaxPitch);
	Render();
}

void FStageMarketplacePreview::Zoom(float Steps)
{
	using namespace StageMarketplacePreviewImpl;
	Distance = FMath::Clamp(Distance * FMath::Pow(ZoomStep, double(Steps)), Radius * MinDistanceRadii, Radius * MaxDistanceRadii);
	Render();
}

void FStageMarketplacePreview::Render()
{
	if (!Scene.IsValid() || !Capture || !Target)
	{
		return;
	}
	const FRotator View(Pitch, Yaw, 0.0);
	Capture->SetWorldLocationAndRotation(Focus - View.Vector() * Distance, View);
	// The preview world never ticks, so spawns and moves reach the renderer only through this.
	Scene->GetWorld()->SendAllEndOfFrameUpdates();
	Capture->CaptureScene();
}
