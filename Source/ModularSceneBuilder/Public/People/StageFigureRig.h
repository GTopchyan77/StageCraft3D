// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "StageFigureRig.generated.h"

/**
 * What a figure is doing: one looping animation clip each (Docs/ADR/0031 §3). Stored by name in item data and as an index in the
 * figure animation texture, so the order is part of the texture layout; append new clips at the end.
 */
UENUM(BlueprintType)
enum class EStageFigureClip : uint8
{
	/** Audience: standing, weight shifting from foot to foot. */
	CrowdIdle		UMETA(DisplayName = "Crowd: Idle"),
	/** Audience: bouncing on the beat, knees bending. */
	CrowdBounce		UMETA(DisplayName = "Crowd: Bounce"),
	/** Audience: both arms up, waving. */
	CrowdCheer		UMETA(DisplayName = "Crowd: Cheer"),
	/** Audience: clapping in front of the chest. */
	CrowdClap		UMETA(DisplayName = "Crowd: Clap"),
	/** Band: seated, alternating stick hits. */
	Drummer			UMETA(DisplayName = "Band: Drummer"),
	/** Band: strumming hand, fretting hand at the neck, nodding. */
	Guitarist		UMETA(DisplayName = "Band: Guitarist"),
	/** Band: one hand at the mouth, the other arm gesturing, swaying. */
	Vocalist		UMETA(DisplayName = "Band: Vocalist"),
	/** Band: both hands forward on the keys, alternating. */
	Keyboardist		UMETA(DisplayName = "Band: Keyboardist"),
	/** Crew: standing at a desk, hands on the surface, looking up at the stage. */
	DeskOperator	UMETA(DisplayName = "Crew: Desk Operator"),
	/** Crew: standing by, looking around. */
	Stagehand		UMETA(DisplayName = "Crew: Stagehand"),

	Count			UMETA(Hidden)
};

/**
 * Procedural stand-in people for the stage (Docs/ADR/0031): a rigid-segment humanoid mesh and its looping clips, baked into a bone
 * animation texture that a material plays on the GPU. Pure: no UObjects, no world; the asset subsystem turns the results into a
 * static mesh and a texture.
 *
 * Space: centimetres, +X forward (the way the figure faces), +Y right, +Z up, feet on Z = 0. The figure is 175 cm tall.
 *
 * Texture contract (shared with M_StageFigure and with any art baked later with the same layout):
 * - Columns: two texels per bone. Texel 2b holds bone b's rotation as a quaternion (x, y, z, w); texel 2b + 1 holds its
 *   translation (x, y, z, 0), such that a rest-pose vertex v of bone b is posed as Rotate(q, v) + t.
 * - Rows: FramesPerClip frames of clip 0, then of clip 1, and so on (EStageFigureClip order). Each clip loops: the frame after the
 *   last is the first. Consecutive quaternions of a bone are kept in the same hemisphere.
 * - Mesh: UV channel 1 holds the bone index in U (as b + 0.5) and the body region in V (Region* below; UVs, not vertex
 *   colours, because a mesh build stores colours as sRGB bytes).
 */
namespace StageFigure
{
	/** The rigid segments of the figure, parents before children. */
	enum class EBone : uint8
	{
		Pelvis,
		Chest,
		Head,
		UpperArmL,
		ForearmL,
		UpperArmR,
		ForearmR,
		ThighL,
		ShinL,
		ThighR,
		ShinR,
		Count
	};

	constexpr int32 BoneCount = static_cast<int32>(EBone::Count);
	constexpr int32 ClipCount = static_cast<int32>(EStageFigureClip::Count);
	constexpr int32 FramesPerClip = 32;
	constexpr int32 LodCount = 3;
	constexpr double Height = 175.0;

	/** UV channel 1 V of each body region: the material colours skin, top (shirt and sleeves), bottom (trousers) and dark (hair, shoes). */
	constexpr float RegionSkin = 0.f;
	constexpr float RegionTop = 1.f;
	constexpr float RegionBottom = 2.f;
	constexpr float RegionDark = 3.f;

	/** The parent of Bone, or Count for the root (Pelvis). */
	MODULARSCENEBUILDER_API EBone GetParent(EBone Bone);

	/** Where Bone turns in the rest pose (its joint), in figure space. */
	MODULARSCENEBUILDER_API FVector GetRestPivot(EBone Bone);

	/** One posed bone: Rotate(Rotation, v) + Translation places a rest-pose vertex v of the bone. */
	struct FBoneTransform
	{
		FQuat Rotation = FQuat::Identity;
		FVector Translation = FVector::ZeroVector;

		FVector Apply(const FVector& RestPosition) const { return Rotation.RotateVector(RestPosition) + Translation; }
	};

	/** A pose: every bone's transform. */
	using FPose = TStaticArray<FBoneTransform, BoneCount>;

	/** Local joint rotations (relative to the parent) and the root's offset from its rest pivot. */
	struct FLocalPose
	{
		TStaticArray<FQuat, BoneCount> Rotations;
		FVector RootOffset = FVector::ZeroVector;

		FLocalPose();
	};

	/** Forward kinematics: the pose of every bone from local rotations. */
	MODULARSCENEBUILDER_API FPose Solve(const FLocalPose& Local);

	/** The pose of Clip at Time in [0, 1) of its loop (wrapped). Pure and deterministic. */
	MODULARSCENEBUILDER_API FPose EvaluateClip(EStageFigureClip Clip, double Time);

	/** Loops per second the clip was designed for (a beat at 128 BPM for the bounce, slower for idling). Always > 0. */
	MODULARSCENEBUILDER_API float GetClipCyclesPerSecond(EStageFigureClip Clip);

	/** Short English name of a clip ("Crowd: Bounce"), for logs and tests. */
	MODULARSCENEBUILDER_API FString GetClipName(EStageFigureClip Clip);

	/** One level of detail of the figure mesh, ready to become a static mesh section. Indices are counter-clockwise seen from outside. */
	struct FMeshData
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> UVs;
		TArray<int32> Bones;
		TArray<float> Regions;
		TArray<int32> Indices;

		int32 GetTriangleCount() const { return Indices.Num() / 3; }
		bool IsValid() const;
	};

	/** The figure in its rest pose at level of detail Lod (0 most detailed, LodCount - 1 coarsest). Invalid for other Lod values. */
	MODULARSCENEBUILDER_API FMeshData BuildMesh(int32 Lod);

	/** Texture width: two texels per bone. */
	constexpr int32 TextureWidth = BoneCount * 2;

	/** Texture height: every frame of every clip. */
	constexpr int32 TextureHeight = ClipCount * FramesPerClip;

	/** The whole animation texture, row-major, TextureWidth x TextureHeight texels of four floats (the contract above). */
	MODULARSCENEBUILDER_API TArray<FVector4f> BakeAnimationTexels();
}
