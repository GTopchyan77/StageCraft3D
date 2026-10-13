// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/StageModelThumbnailRenderer.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "Misc/App.h"
#include "PreviewScene.h"

namespace StageModelThumbnail
{
	constexpr float FieldOfView = 30.f;

	/** Manual exposure for the preview light; one fixed value keeps every thumbnail equally bright. */
	constexpr float ExposureBias = 1.5f;

	/** From the front-left and a little above, like a catalogue photo. Unreal: X forward, Y right, Z up. */
	const FVector ViewDirection = FVector(1.0, 0.75, -0.55).GetSafeNormal();
}

FStageModelThumbnailRenderer::FStageModelThumbnailRenderer() = default;
FStageModelThumbnailRenderer::~FStageModelThumbnailRenderer() = default;

bool FStageModelThumbnailRenderer::EnsureScene()
{
	if (Scene.IsValid())
	{
		return true;
	}
	if (!FApp::CanEverRender())
	{
		return false;
	}

	Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
		.SetEditor(false)
		.SetCreateDefaultLighting(true)
		// Key light from just above and right of the camera, so the faces the camera sees are lit.
		.SetLightRotation(FRotator(-45.f, 60.f, 0.f))
		.SetLightBrightness(10.f)
		.SetSkyBrightness(1.f)
		.SetCreatePhysicsScene(false)
		.AllowAudioPlayback(false));

	MeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
	MeshComponent->SetMobility(EComponentMobility::Movable);
	Scene->AddComponent(MeshComponent, FTransform::Identity);

	Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->FOVAngle = StageModelThumbnail::FieldOfView;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ShowOnlyComponents.Add(MeshComponent);
	// One still image of a private world: nothing that needs frame history (eye adaptation, temporal AA) or a scene
	// representation the preview world does not build (Lumen), and nothing but the model.
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetTemporalAA(false);
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->ShowFlags.SetAmbientOcclusion(false);
	Capture->ShowFlags.SetScreenSpaceReflections(false);
	Capture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
	Capture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
	Capture->PostProcessSettings.bOverride_ReflectionMethod = true;
	Capture->PostProcessSettings.ReflectionMethod = EReflectionMethod::None;
	Capture->ShowFlags.SetFog(false);
	Capture->ShowFlags.SetAtmosphere(false);
	Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
	Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
	Capture->PostProcessSettings.AutoExposureBias = StageModelThumbnail::ExposureBias;
	Scene->AddComponent(Capture, FTransform::Identity);
	return true;
}

UTextureRenderTarget2D* FStageModelThumbnailRenderer::Render(UStaticMesh& Mesh, UMaterialInterface* Material, UObject& Outer)
{
	const FBox Bounds = Mesh.GetBoundingBox();
	if (!Bounds.IsValid || !EnsureScene())
	{
		return nullptr;
	}

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(&Outer);
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->ClearColor = FLinearColor::Transparent;
	Target->InitAutoFormat(Size, Size);
	Target->UpdateResourceImmediate(/*bClearRenderTarget*/ true);

	MeshComponent->SetStaticMesh(&Mesh);
	UMaterialInterface* Clay = Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
	for (int32 SlotIndex = 0; SlotIndex < MeshComponent->GetNumMaterials(); ++SlotIndex)
	{
		MeshComponent->SetMaterial(SlotIndex, Clay);
	}

	// Frame the bounding sphere so any model fills the picture whatever its size.
	const double Radius = FMath::Max(Bounds.GetExtent().Size(), 1.0);
	const double Distance = Radius / FMath::Sin(FMath::DegreesToRadians(StageModelThumbnail::FieldOfView * 0.5)) * 1.05;
	const FVector Center = Bounds.GetCenter();
	Capture->SetWorldLocationAndRotation(Center - StageModelThumbnail::ViewDirection * Distance, StageModelThumbnail::ViewDirection.Rotation());
	// The preview world never ticks, so the mesh change has to reach the renderer before the capture reads the scene.
	Scene->GetWorld()->SendAllEndOfFrameUpdates();
	Capture->TextureTarget = Target;
	Capture->CaptureScene();
	Capture->TextureTarget = nullptr;
	return Target;
}
