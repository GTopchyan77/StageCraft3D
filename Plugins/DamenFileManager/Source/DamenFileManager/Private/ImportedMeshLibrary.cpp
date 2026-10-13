// Copyright DamenTools internal. Damen File Manager plugin.

#include "ImportedMeshLibrary.h"
#include "DamenFileManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

namespace
{
	/** Model-space matrix for every model, composed through its parent chain (child relative * parent world). */
	TArray<FMatrix> ComposeModelMatrices(const TArray<FRTIModelStruct>& Models)
	{
		TMap<int64, int32> IndexById;
		for (int32 Index = 0; Index < Models.Num(); ++Index)
		{
			IndexById.Add(Models[Index].modelID, Index);
		}

		TArray<FMatrix> World;
		World.SetNum(Models.Num());
		TArray<uint8> State; // 0 = pending, 1 = in progress (cycle guard), 2 = done
		State.SetNumZeroed(Models.Num());

		TFunction<const FMatrix&(int32)> Resolve = [&](int32 Index) -> const FMatrix&
		{
			if (State[Index] == 2)
			{
				return World[Index];
			}
			const FMatrix Local = Models[Index].relativeTransform.ToMatrixWithScale();
			World[Index] = Local;
			if (State[Index] == 0)
			{
				State[Index] = 1;
				const int32* ParentIndex = Models[Index].parent >= 0 ? IndexById.Find(Models[Index].parent) : nullptr;
				if (ParentIndex && State[*ParentIndex] != 1)
				{
					World[Index] = Local * Resolve(*ParentIndex);
				}
			}
			State[Index] = 2;
			return World[Index];
		};

		for (int32 Index = 0; Index < Models.Num(); ++Index)
		{
			Resolve(Index);
		}
		return World;
	}
}

UStaticMesh* UImportedMeshLibrary::BuildStaticMeshFromModels(const TArray<FRTIModelStruct>& ModelStructs, const FString& MeshName)
{
	const TArray<FMatrix> ModelMatrices = ComposeModelMatrices(ModelStructs);

	// Pass 1: baked positions, for the bottom-centre pivot.
	FBox Bounds(ForceInit);
	for (int32 ModelIndex = 0; ModelIndex < ModelStructs.Num(); ++ModelIndex)
	{
		for (const FRTIMeshStruct& Section : ModelStructs[ModelIndex].meshStructs)
		{
			for (const FVector& Position : Section.vertices)
			{
				Bounds += FVector(ModelMatrices[ModelIndex].TransformPosition(Position));
			}
		}
	}
	if (!Bounds.IsValid)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("BuildStaticMeshFromModels('%s'): %d model(s) but no vertices - no mesh built."), *MeshName, ModelStructs.Num());
		return nullptr;
	}
	const FVector PivotOffset(-Bounds.GetCenter().X, -Bounds.GetCenter().Y, -Bounds.Min.Z);

	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	const FString CleanName = FPaths::GetBaseFilename(MeshName).Replace(TEXT(" "), TEXT("_"));
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), FName(*(TEXT("SM_Imported_") + CleanName))), RF_Transient);
	UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	int32 SectionCount = 0;
	int32 TriangleCount = 0;
	for (int32 ModelIndex = 0; ModelIndex < ModelStructs.Num(); ++ModelIndex)
	{
		const FMatrix& Matrix = ModelMatrices[ModelIndex];
		const FMatrix NormalMatrix = Matrix.Inverse().GetTransposed();
		const bool bMirrored = Matrix.Determinant() < 0.0;

		for (const FRTIMeshStruct& Section : ModelStructs[ModelIndex].meshStructs)
		{
			if (Section.vertices.Num() == 0 || Section.triangles.Num() < 3)
			{
				continue;
			}

			const FName SlotName(*FString::Printf(TEXT("Section%d"), SectionCount++));
			const FPolygonGroupID Group = MeshDescription.CreatePolygonGroup();
			SlotNames[Group] = SlotName;
			Mesh->GetStaticMaterials().Add(FStaticMaterial(DefaultMaterial, SlotName, SlotName));

			TArray<FVertexInstanceID> Instances;
			Instances.Reserve(Section.vertices.Num());
			for (int32 V = 0; V < Section.vertices.Num(); ++V)
			{
				const FVertexID Vertex = MeshDescription.CreateVertex();
				Positions[Vertex] = FVector3f(FVector(Matrix.TransformPosition(Section.vertices[V])) + PivotOffset);

				const FVertexInstanceID Instance = MeshDescription.CreateVertexInstance(Vertex);
				const FVector Normal = Section.normals.IsValidIndex(V) ? FVector(NormalMatrix.TransformVector(Section.normals[V])).GetSafeNormal() : FVector::UpVector;
				Normals[Instance] = FVector3f(Normal);
				if (Section.tangents.IsValidIndex(V))
				{
					Tangents[Instance] = FVector3f(FVector(Matrix.TransformVector(Section.tangents[V].TangentX)).GetSafeNormal());
					BinormalSigns[Instance] = (Section.tangents[V].bFlipTangentY ? -1.f : 1.f) * (bMirrored ? -1.f : 1.f);
				}
				UVs.Set(Instance, 0, Section.UV0.IsValidIndex(V) ? FVector2f(Section.UV0[V]) : FVector2f::ZeroVector);
				Colors[Instance] = Section.vertexColors.IsValidIndex(V) ? FVector4f(Section.vertexColors[V]) : FVector4f(1.f, 1.f, 1.f, 1.f);
				Instances.Add(Instance);
			}

			for (int32 T = 0; T + 2 < Section.triangles.Num(); T += 3)
			{
				const int32 A = Section.triangles[T], B = Section.triangles[T + 1], C = Section.triangles[T + 2];
				if (!Instances.IsValidIndex(A) || !Instances.IsValidIndex(B) || !Instances.IsValidIndex(C) || A == B || B == C || A == C)
				{
					continue;
				}
				// A mirroring transform turns the triangle inside out; swap two corners to keep it facing outward.
				const FVertexInstanceID Corners[3] = { Instances[A], bMirrored ? Instances[C] : Instances[B], bMirrored ? Instances[B] : Instances[C] };
				MeshDescription.CreateTriangle(Group, MakeArrayView(Corners, 3));
				++TriangleCount;
			}
		}
	}

	if (TriangleCount == 0)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("BuildStaticMeshFromModels('%s'): no valid triangles - no mesh built."), *MeshName);
		return nullptr;
	}

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	Params.bFastBuild = true;        // the only path available outside the editor
	Params.bBuildSimpleCollision = true; // box collision: placed imports stay clickable/selectable
	Params.bAllowCpuAccess = true;
	if (!Mesh->BuildFromMeshDescriptions({ &MeshDescription }, Params))
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("BuildStaticMeshFromModels('%s'): BuildFromMeshDescriptions failed."), *MeshName);
		return nullptr;
	}

	// Logged in cm so a wrong unit/transform shows up immediately (compare with the FBX's real size).
	const FVector Size = Bounds.GetSize();
	UE_LOG(LogDamenFileManager, Log, TEXT("BuildStaticMeshFromModels('%s'): %d model(s), %d section(s), %d triangle(s), size %.1f x %.1f x %.1f cm -> %s"),
		*MeshName, ModelStructs.Num(), SectionCount, TriangleCount, Size.X, Size.Y, Size.Z, *Mesh->GetName());
	return Mesh;
}

bool UImportedMeshLibrary::ShowImportedMesh(UStaticMeshComponent* Target, UStaticMesh* Mesh, float Yaw)
{
	if (!Target || !Mesh)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("ShowImportedMesh: %s is null - imported object shows nothing."), Target ? TEXT("Mesh") : TEXT("Target component"));
		return false;
	}
	Target->SetStaticMesh(Mesh);
	Target->SetVisibility(true);
	if (!FMath::IsNearlyZero(Yaw))
	{
		Target->AddWorldRotation(FRotator(0.f, Yaw, 0.f));
	}
	return true;
}
