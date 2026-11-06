// Copyright Epic Games, Inc. All Rights Reserved.

#include "People/StageFigureAssetSubsystem.h"

#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "ModularSceneBuilder.h"
#include "People/StageFigureRig.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFigureAssetSubsystem)

const FName UStageFigureAssetSubsystem::AnimTextureParameter(TEXT("AnimTex"));
const FName UStageFigureAssetSubsystem::FramesPerClipParameter(TEXT("FramesPerClip"));
const FName UStageFigureAssetSubsystem::TextureRowsParameter(TEXT("TextureRows"));
const FName UStageFigureAssetSubsystem::TextureColumnsParameter(TEXT("TextureColumns"));

namespace StageFigureAssets
{
	const FName MaterialSlot(TEXT("Figure"));

	/**
	 * Screen size (fraction of the screen height the figure's bounds cover) below which each LOD takes over. A person in the front
	 * rows of a camera on the stage is LOD0; most of an arena crowd is LOD1 and LOD2.
	 */
	constexpr float LodScreenSizes[StageFigure::LodCount] = { 1.f, 0.16f, 0.05f };

	/** Room for the poses around the rest-pose bounds (arms raised over the head, a step or a lean), so a pose is never culled. */
	const FVector PositiveBoundsExtension(70.0, 70.0, 45.0);
	const FVector NegativeBoundsExtension(70.0, 70.0, 10.0);

	FMeshDescription MakeDescription(const StageFigure::FMeshData& Data)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);

		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = MaterialSlot;

		const int32 Count = Data.Positions.Num();
		Description.ReserveNewVertices(Count);
		Description.ReserveNewVertexInstances(Count);
		Description.ReserveNewTriangles(Data.GetTriangleCount());

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = Data.Positions[Index];
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			const FVector3f Normal = Data.Normals[Index].GetSafeNormal();
			Normals[Instance] = Normal;
			// Any tangent perpendicular to the normal: the figure material has no normal map.
			FVector3f Tangent = Normal ^ FVector3f::UpVector;
			if (Tangent.SizeSquared() < 1e-4f)
			{
				Tangent = Normal ^ FVector3f::ForwardVector;
			}
			Tangents[Instance] = Tangent.GetSafeNormal();
			BinormalSigns[Instance] = 1.f;
			Colors[Instance] = FVector4f(1.f, 1.f, 1.f, 1.f);
			UVs.Set(Instance, 0, Data.UVs[Index]);
			// The material's contract (StageFigureRig.h): bone in U at the texel centre, region in V.
			UVs.Set(Instance, 1, FVector2f(float(Data.Bones[Index]) + 0.5f, Data.Regions[Index]));
			Instances.Add(Instance);
		}
		for (int32 Index = 0; Index + 2 < Data.Indices.Num(); Index += 3)
		{
			Description.CreateTriangle(Group, { Instances[Data.Indices[Index]], Instances[Data.Indices[Index + 1]], Instances[Data.Indices[Index + 2]] });
		}
		return Description;
	}
}

void UStageFigureAssetSubsystem::Deinitialize()
{
	Materials.Reset();
	AnimationTexture = nullptr;
	FigureMesh = nullptr;
	Super::Deinitialize();
}

UStaticMesh* UStageFigureAssetSubsystem::GetFigureMesh()
{
	if (!FigureMesh && !bMeshBuildFailed)
	{
		FigureMesh = BuildFigureMesh(this);
		bMeshBuildFailed = FigureMesh == nullptr;
	}
	return FigureMesh;
}

UTexture2D* UStageFigureAssetSubsystem::GetAnimationTexture()
{
	if (!AnimationTexture)
	{
		AnimationTexture = BuildAnimationTexture(this);
	}
	return AnimationTexture;
}

UMaterialInterface* UStageFigureAssetSubsystem::GetFigureMaterial(UMaterialInterface* Parent)
{
	if (!Parent)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInstanceDynamic>* Existing = Materials.Find(Parent))
	{
		return *Existing;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
	Instance->SetTextureParameterValue(AnimTextureParameter, GetAnimationTexture());
	Instance->SetScalarParameterValue(FramesPerClipParameter, float(StageFigure::FramesPerClip));
	Instance->SetScalarParameterValue(TextureRowsParameter, float(StageFigure::TextureHeight));
	Instance->SetScalarParameterValue(TextureColumnsParameter, float(StageFigure::TextureWidth));
	Materials.Add(Parent, Instance);
	return Instance;
}

UStaticMesh* UStageFigureAssetSubsystem::BuildFigureMesh(UObject* Outer)
{
	using namespace StageFigureAssets;

	TArray<FMeshDescription> Descriptions;
	for (int32 Lod = 0; Lod < StageFigure::LodCount; ++Lod)
	{
		const StageFigure::FMeshData Data = StageFigure::BuildMesh(Lod);
		if (!Data.IsValid())
		{
			UE_LOG(LogStageCraft, Error, TEXT("Figures: LOD %d of the figure mesh is invalid; people are not shown."), Lod);
			return nullptr;
		}
		Descriptions.Add(MakeDescription(Data));
	}
	TArray<const FMeshDescription*> DescriptionPointers;
	for (const FMeshDescription& Description : Descriptions)
	{
		DescriptionPointers.Add(&Description);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), MaterialSlot));

	// Clicks hit the simple box: the render data keeps no CPU copy to cook a triangle collision mesh from.
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseSimpleAsComplex;

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	// The only path in builds without the editor. A simple box lets a performer be clicked; crowds turn collision off.
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = true;
	Params.bAllowCpuAccess = false;
	if (!Mesh->BuildFromMeshDescriptions(DescriptionPointers, Params))
	{
		UE_LOG(LogStageCraft, Error, TEXT("Figures: the figure mesh could not be built; people are not shown."));
		return nullptr;
	}

	if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
	{
		for (int32 Lod = 0; Lod < StageFigure::LodCount && Lod < RenderData->LODResources.Num(); ++Lod)
		{
			RenderData->ScreenSize[Lod].Default = LodScreenSizes[Lod];
		}
	}
	Mesh->SetPositiveBoundsExtension(PositiveBoundsExtension);
	Mesh->SetNegativeBoundsExtension(NegativeBoundsExtension);
	Mesh->CalculateExtendedBounds();

	UE_LOG(LogStageCraft, Log, TEXT("Figures: mesh built (%d / %d / %d triangles), animation %d x %d texels."),
		StageFigure::BuildMesh(0).GetTriangleCount(), StageFigure::BuildMesh(1).GetTriangleCount(), StageFigure::BuildMesh(2).GetTriangleCount(),
		StageFigure::TextureWidth, StageFigure::TextureHeight);
	return Mesh;
}

UTexture2D* UStageFigureAssetSubsystem::BuildAnimationTexture(UObject* Outer)
{
	const TArray<FVector4f> Texels = StageFigure::BakeAnimationTexels();
	UTexture2D* Texture = UTexture2D::CreateTransient(StageFigure::TextureWidth, StageFigure::TextureHeight, PF_A32B32G32R32F);
	if (!Texture)
	{
		UE_LOG(LogStageCraft, Error, TEXT("Figures: the animation texture could not be created; people stand still."));
		return nullptr;
	}
	// Read texel by texel in the vertex shader: no filtering, no mips, no colour conversion.
	Texture->Filter = TF_Nearest;
	Texture->SRGB = false;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->NeverStream = true;
	Texture->CompressionSettings = TC_HDR;

	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Texels.GetData(), Texels.Num() * sizeof(FVector4f));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}
