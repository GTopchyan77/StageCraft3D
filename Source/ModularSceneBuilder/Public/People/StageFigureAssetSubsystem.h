// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "StageFigureAssetSubsystem.generated.h"

/**
 * The procedural people's shared GPU assets (Docs/ADR/0031 §3): one static mesh of the figure (three LODs, built from
 * StageFigure::BuildMesh) and one animation texture (StageFigure::BakeAnimationTexels), made on first use. Every audience,
 * band and crew item draws the same mesh with a dynamic instance of its item's figure material that points at the texture, so any
 * number of items share one mesh and one texture.
 *
 * Built at runtime rather than cooked: the rig is code, so the mesh and the texture cannot drift from it, and art baked later with
 * the same contract (StageFigureRig.h) replaces them by data. Game thread only. Building takes a few milliseconds.
 *
 * An engine subsystem, not a game-instance one: the assets come from code and are the same in every world, including worlds
 * without a game instance (the glTF export world, editor worlds), and are built once per process.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageFigureAssetSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** The figure mesh, built on first use. Null only if the build failed (logged). */
	class UStaticMesh* GetFigureMesh();

	/** The animation texture, built on first use. */
	class UTexture2D* GetAnimationTexture();

	/**
	 * A dynamic instance of Parent with the animation texture and its layout set (AnimTex, FramesPerClip, TextureRows,
	 * TextureColumns), shared by every caller with the same parent. Null for a null parent.
	 */
	class UMaterialInterface* GetFigureMaterial(class UMaterialInterface* Parent);

	/** Names of the material parameters the figure material must declare. */
	static const FName AnimTextureParameter;
	static const FName FramesPerClipParameter;
	static const FName TextureRowsParameter;
	static const FName TextureColumnsParameter;

	/** How many custom data floats each figure instance carries, and what they hold (the material reads them in this order). */
	static constexpr int32 CustomDataCount = 8;
	enum ECustomData : int32
	{
		/** EStageFigureClip as a float. */
		Clip = 0,
		/** Where in its loop the figure starts, 0..1. */
		Phase = 1,
		/** Loops per second. */
		Rate = 2,
		/** Shirt colour, linear RGB. */
		TopR = 3,
		TopG = 4,
		TopB = 5,
		/** 0 light to 1 dark skin. */
		SkinTone = 6,
		/** 0 blue denim to 1 black trousers. */
		BottomShade = 7,
	};

	/** Builds a figure static mesh with every LOD, owned by Outer (tests use this without a game instance). Null on failure. */
	static class UStaticMesh* BuildFigureMesh(UObject* Outer);

	/** Builds the animation texture, owned by Outer. */
	static class UTexture2D* BuildAnimationTexture(UObject* Outer);

private:
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMesh> FigureMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> AnimationTexture = nullptr;

	UPROPERTY(Transient)
	TMap<TObjectPtr<class UMaterialInterface>, TObjectPtr<class UMaterialInstanceDynamic>> Materials;

	bool bMeshBuildFailed = false;
};
