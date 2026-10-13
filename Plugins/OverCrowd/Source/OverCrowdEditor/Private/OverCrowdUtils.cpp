// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "OverCrowdUtils.h"

#include "OverCrowdEditor.h"

namespace OverCrowd_Private
{

void DecomposeTransformation(const FTransform& Transform, 
	FVector3f& OutTranslation, FVector4f& OutRotation)
{
	// Get Translation
	OutTranslation = (FVector3f)Transform.GetTranslation();

	// Get Rotation 
	const FQuat4f Quat = (FQuat4f)Transform.GetRotation();

	FVector3f Axis;
	float Angle;
	Quat.ToAxisAndAngle(Axis, Angle);

	OutRotation = FVector4f(Axis, Angle);
}

void DecomposeTransformations(const TArray<FTransform>& Transforms, 
	TArray<FVector3f>& OutTranslations, TArray<FVector4f>& OutRotations)
{
	const int32 NumTransforms = Transforms.Num();
	OutTranslations.SetNumUninitialized(NumTransforms);
	OutRotations.SetNumUninitialized(NumTransforms);

	for (int32 Index = 0; Index < NumTransforms; ++Index)
	{
		DecomposeTransformation(Transforms[Index], OutTranslations[Index], OutRotations[Index]);
	}
};
//
//
//void MapBoneIndicesToForgeinSkeleton(const USkeletalMesh* ForgeinSkeletalMesh, const USkeletalMesh* SkeletalMesh, TArray<int32>& OutBoneIndexMap)
//{
//	if (!ForgeinSkeletalMesh || !SkeletalMesh)
//	{
//		return;
//	}
//
//	const FReferenceSkeleton& ForgeinRefSkeleton = ForgeinSkeletalMesh->GetSkeleton()->GetReferenceSkeleton();
//	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetSkeleton()->GetReferenceSkeleton();
//
//	int32 NumBones = RefSkeleton.GetNum();
//	OutBoneIndexMap.Init(0, NumBones);
//
//	for (int32 i = 0; i < OutBoneIndexMap.Num(); i++)
//	{
//		const int32 ForgeinBoneID = ForgeinRefSkeleton.FindBoneIndex(RefSkeleton.GetBoneName(i));
//		if (ForgeinBoneID == INDEX_NONE)
//		{
//			UE_LOG(LogOverCrowdEditor, Warning, TEXT("MapBoneIndicesToForgeinSkeleton() -- BoneName: %s does not exist in ForgeinSkeletalMesh: %s"), *RefSkeleton.GetBoneName(i).ToString(), *ForgeinSkeletalMesh->GetName());
//		}
//		else
//		{
//			OutBoneIndexMap[i] = ForgeinBoneID;
//		}
//
//	}
//}


//FORCEINLINE_DEBUGGABLE 
void OverCrowd_Private::GetSkinWeightsToUV(const TArray<OverCrowd_Private::VertexSkinWeightFour>& SkinWeights, TArray<FVector4f>& Out_BoneIndices, TArray< FColor>& Out_BoneWeights)
{

	const int32 NumVertices = SkinWeights.Num();

	Out_BoneWeights.Init(FColor(0, 0, 0, 0), NumVertices);
	Out_BoneIndices.SetNum(NumVertices);


	for (int32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
	{
		if (VertexIndex >= SkinWeights.Num())
		{
			UE_LOG(LogOverCrowdEditor, Warning, TEXT("GetSkinWeightsToUV() - VertexIndex: %d exeeds SkinWeights %d"), VertexIndex, SkinWeights.Num());
			continue;
		}
		const VertexSkinWeightFour& VertexSkinWeight = SkinWeights[VertexIndex];

		// Normalize BoneIndices
		/*const FVector4f BoneIndices(
			(float)VertexSkinWeight.MeshBoneIndices[0] / float(NumBones),
			(float)VertexSkinWeight.MeshBoneIndices[1] / float(NumBones),
			(float)VertexSkinWeight.MeshBoneIndices[2] / float(NumBones),
			(float)VertexSkinWeight.MeshBoneIndices[3] / float(NumBones));*/
		const FVector4f BoneIndices(
			(float)VertexSkinWeight.MeshBoneIndices[0],
			(float)VertexSkinWeight.MeshBoneIndices[1],
			(float)VertexSkinWeight.MeshBoneIndices[2],
			(float)VertexSkinWeight.MeshBoneIndices[3]);


		const FColor BoneWeights(
			VertexSkinWeight.BoneWeights[0],
			VertexSkinWeight.BoneWeights[1],
			VertexSkinWeight.BoneWeights[2],
			VertexSkinWeight.BoneWeights[3]);

		//FColor& TmpBoneWeight = Out_BoneWeights[VertexIndex];
		//VectorToColor<FVector4f, FColor>(BoneWeights, TmpBoneWeight);

		Out_BoneWeights[VertexIndex] = BoneWeights;
		Out_BoneIndices[VertexIndex] = BoneIndices;
	};
}



} // end namespace OverCrowd_Private