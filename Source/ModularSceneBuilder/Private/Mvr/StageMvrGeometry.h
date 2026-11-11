// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Bounds of the geometry files an MVR archive carries (Docs/ADR/0034 §6, MV2): enough to size a StageCraft truss run from an MVR
 * Truss without importing its mesh. Only the numbers a file states about itself are read:
 *  - glTF 2.0 (.glb, .gltf): the min / max that glTF requires on every POSITION accessor, moved by the node transforms of the
 *    default scene. No vertex buffer is read. glTF is metres and Y up; the result is converted to MVR space (millimetres, Z up,
 *    right-handed): (x, y, z) -> (x, -z, y) * 1000, the usual glTF-to-Z-up mapping (MVR 1.6 states no axis for glTF geometry;
 *    to be checked on the MV0 corpus).
 *  - 3DS (deprecated in MVR 1.6, 1 unit = 1 mm): every vertex list of every mesh, already in MVR space.
 *
 * The bytes are untrusted: every count, offset and nesting level is checked, and the JSON is capped. Pure; safe on any thread.
 */
namespace StageMvrGeometry
{
	/** Largest glTF JSON (the .gltf file or the GLB JSON chunk) read. */
	constexpr int32 MaxJsonBytes = 8 * 1024 * 1024;
	/** Most nodes, meshes and accessors looked at, and the deepest node nesting followed. */
	constexpr int32 MaxNodes = 20000;
	constexpr int32 MaxNodeDepth = 32;
	/** Most 3DS chunks walked. */
	constexpr int32 Max3dsChunks = 200000;

	/** Bounds of a .glb or .gltf file in MVR space (mm, Z up). False, with OutError, when the file cannot be read or has no positions. */
	bool ReadGltfBounds(TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError);

	/** Bounds of a .3ds file (mm). False, with OutError, when the file cannot be read or has no vertices. */
	bool Read3dsBounds(TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError);

	/** By extension (.glb, .gltf, .3ds; case-insensitive). False for any other file, with OutError. */
	bool ReadBounds(const FString& FileName, TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError);

	/**
	 * Docs/ADR/0034 §7 (MV4): a binary glTF 2.0 file holding one box mesh, Bounds (MVR mm, Z up) converted with the inverse of the
	 * mapping above, so ReadGltfBounds gives Bounds back. 8 positions and 12 outward-facing triangles; Name names its node and mesh.
	 * Bounds must be valid.
	 */
	TArray<uint8> WriteBoxGlb(const FBox& Bounds, const FString& Name);
}
