// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGMeshGeometryScript.cpp
//
// Implementation of the /mesh/* HTTP endpoints that expose UE5 Geometry Script
// mesh composition to MCP clients. Follows the same threading pattern as
// NGGHttpServer.cpp: handlers dispatch to the Game Thread via AsyncTask before
// touching any UObject / asset system.
//
// Handlers operate on transient UDynamicMesh working objects kept alive in
// FNGGHttpServer::MeshHandles (TMap<FString, TStrongObjectPtr<UDynamicMesh>>).
// Callers allocate a handle with POST /mesh/create, compose primitives into it
// via POST /mesh/append_primitive, combine handles with POST /mesh/boolean,
// then bake to a UStaticMesh asset via POST /mesh/bake_static.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- UE5 HTTP Server -------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
// ---- Dynamic Mesh ----------------------------------------------------------
#include "UDynamicMesh.h"
// ---- Geometry Script libraries --------------------------------------------
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
// ---- Editor-only: static mesh asset bake ----------------------------------
#if WITH_EDITOR
#include "GeometryScript/CreateNewAssetUtilityFunctions.h"
#endif
// ---- Socket assets ---------------------------------------------------------
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/Skeleton.h"
#include "Misc/PackageName.h"

// ============================================================================
// Local JSON helpers
// ============================================================================

namespace
{
	/** Serialise a FJsonObject to a string via FJsonSerializer. */
	static FString MeshSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** Build an {"ok":true,...} response body with optional extra fields. */
	static FString OkBody(TFunctionRef<void(TSharedRef<FJsonObject>&)> Filler)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), true);
		Filler(Root);
		return MeshSerializeJson(Root);
	}

	/** Read a JSON array of exactly 3 numbers into a FVector3-ish pack. */
	static bool ReadTriple(const TSharedPtr<FJsonObject>& Obj, const FString& Key,
		double& OutX, double& OutY, double& OutZ)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 3)
		{
			return false;
		}
		OutX = (*Arr)[0]->AsNumber();
		OutY = (*Arr)[1]->AsNumber();
		OutZ = (*Arr)[2]->AsNumber();
		return true;
	}

	/**
	 * Parse an FTransform from a JSON sub-object:
	 *   { "location":[x,y,z], "rotation":[pitch,yaw,roll], "scale":[x,y,z] }
	 * Missing fields default to identity / unit-scale.
	 */
	static FTransform ParseTransform(const TSharedPtr<FJsonObject>& Parent, const FString& Key)
	{
		FTransform Result = FTransform::Identity;
		if (!Parent.IsValid()) return Result;

		const TSharedPtr<FJsonObject>* SubObjPtr = nullptr;
		if (!Parent->TryGetObjectField(Key, SubObjPtr) || !SubObjPtr || !SubObjPtr->IsValid())
		{
			return Result;
		}
		const TSharedPtr<FJsonObject>& Sub = *SubObjPtr;

		double X, Y, Z;
		if (ReadTriple(Sub, TEXT("location"), X, Y, Z))
		{
			Result.SetLocation(FVector(X, Y, Z));
		}
		if (ReadTriple(Sub, TEXT("rotation"), X, Y, Z))
		{
			Result.SetRotation(FRotator(X, Y, Z).Quaternion());
		}
		if (ReadTriple(Sub, TEXT("scale"), X, Y, Z))
		{
			Result.SetScale3D(FVector(X, Y, Z));
		}
		return Result;
	}

	/** Build an FTransform whose rotation aligns the X axis with the requested axis. */
	static FTransform AxisAlignedTransform(const FString& Axis)
	{
		// Geometry Script warp functions operate along the frame's Z axis.
		// Build a rotator that sends Z to the requested world axis.
		if (Axis.Equals(TEXT("X"), ESearchCase::IgnoreCase))
		{
			return FTransform(FRotator(0.f, 0.f, 90.f));  // Z -> X
		}
		if (Axis.Equals(TEXT("Y"), ESearchCase::IgnoreCase))
		{
			return FTransform(FRotator(90.f, 0.f, 0.f));  // Z -> Y
		}
		// Default: Z
		return FTransform::Identity;
	}

	/** Get default primitive options struct. */
	static FGeometryScriptPrimitiveOptions DefaultPrimitiveOptions()
	{
		FGeometryScriptPrimitiveOptions Opts;
		return Opts;
	}
}

// ============================================================================
// Handle helpers
// ============================================================================

// Returns the mesh stored under Handle, or nullptr and fills OutErr.
// Must be called on the Game Thread.
static UDynamicMesh* ResolveHandle(
	TMap<FString, TStrongObjectPtr<UDynamicMesh>>& MeshHandles,
	const FString& Handle, FString& OutErr)
{
	if (Handle.IsEmpty())
	{
		OutErr = TEXT("'handle' is required");
		return nullptr;
	}
	TStrongObjectPtr<UDynamicMesh>* Found = MeshHandles.Find(Handle);
	if (!Found || !Found->IsValid())
	{
		OutErr = FString::Printf(TEXT("No mesh registered under handle '%s'"), *Handle);
		return nullptr;
	}
	return Found->Get();
}

// Triangle count from a UDynamicMesh via ProcessMesh (read-only).
static int32 GetTriangleCount(UDynamicMesh* Mesh)
{
	if (!Mesh) return 0;
	int32 Count = 0;
	Mesh->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& Read)
	{
		Count = Read.TriangleCount();
	});
	return Count;
}

// ============================================================================
// POST /mesh/create
// Body: { "handle": "dragon" }
// ============================================================================

bool FNGGHttpServer::HandleMeshCreate(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/create"));

	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle;
	if (!Body->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' is required")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle;

	AsyncTask(ENamedThreads::GameThread, [Self, Callback, CapturedHandle]()
	{
		// Reject reuse of a live handle: overwriting would drop the existing strong ref
		// and leave that mesh eligible for GC, silently invalidating prior work.
		if (Self->MeshHandles.Contains(CapturedHandle))
		{
			Callback(JsonError(409, FString::Printf(
				TEXT("handle '%s' already exists — delete it first (/mesh/delete_handle) or use a different handle"),
				*CapturedHandle)));
			return;
		}

		UDynamicMesh* NewMesh = NewObject<UDynamicMesh>(
			GetTransientPackage(), UDynamicMesh::StaticClass(), NAME_None, RF_Transient);
		if (!NewMesh)
		{
			Callback(JsonError(500, TEXT("Failed to allocate UDynamicMesh")));
			return;
		}

		Self->MeshHandles.Add(CapturedHandle, TStrongObjectPtr<UDynamicMesh>(NewMesh));
		UE_LOG(LogNGGBridge, Log, TEXT("/mesh/create: registered handle '%s' (total=%d)"),
			*CapturedHandle, Self->MeshHandles.Num());

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetStringField(TEXT("handle"), CapturedHandle);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/append_primitive
// Body:
//   {
//     "handle": "dragon",
//     "shape":  "Box|Sphere|Cylinder|Cone|Torus|Capsule",
//     "transform": { ... optional ... },
//     "params": { shape-specific }
//   }
// ============================================================================

bool FNGGHttpServer::HandleMeshAppendPrimitive(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle, Shape;
	Body->TryGetStringField(TEXT("handle"), Handle);
	Body->TryGetStringField(TEXT("shape"), Shape);
	if (Handle.IsEmpty() || Shape.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' and 'shape' are required")));
		return true;
	}

	const FTransform Xform = ParseTransform(Body, TEXT("transform"));

	// Grab the params object (may be missing — use shape defaults).
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Body->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/append_primitive handle='%s' shape='%s'"),
		*Handle, *Shape);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle;
	FString CapturedShape  = Shape;
	TSharedPtr<FJsonObject> CapturedParams = Params;

	AsyncTask(ENamedThreads::GameThread,
		[Self, Callback, CapturedHandle, CapturedShape, CapturedParams, Xform]()
	{
		FString Err;
		UDynamicMesh* Mesh = ResolveHandle(Self->MeshHandles, CapturedHandle, Err);
		if (!Mesh)
		{
			Callback(JsonError(404, Err));
			return;
		}

		const FGeometryScriptPrimitiveOptions Opts = DefaultPrimitiveOptions();

		// Helper to fetch a number with a default.
		auto N = [&](const TCHAR* Key, double Default)
		{
			double Out = Default;
			CapturedParams->TryGetNumberField(FString(Key), Out);
			return Out;
		};

		if (CapturedShape.Equals(TEXT("Box"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, Xform,
				N(TEXT("DimensionsX"), 100.0),
				N(TEXT("DimensionsY"), 100.0),
				N(TEXT("DimensionsZ"), 100.0),
				FMath::Max(1, (int32)N(TEXT("StepsX"), 1)),
				FMath::Max(1, (int32)N(TEXT("StepsY"), 1)),
				FMath::Max(1, (int32)N(TEXT("StepsZ"), 1)),
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else if (CapturedShape.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
				Mesh, Opts, Xform,
				N(TEXT("Radius"), 50.0),
				FMath::Max(3, (int32)N(TEXT("StepsPhi"), 12)),
				FMath::Max(3, (int32)N(TEXT("StepsTheta"), 16)),
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else if (CapturedShape.Equals(TEXT("Cylinder"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
				Mesh, Opts, Xform,
				N(TEXT("Radius"), 50.0),
				N(TEXT("Height"), 100.0),
				FMath::Max(3, (int32)N(TEXT("RadialSteps"), 16)),
				FMath::Max(1, (int32)N(TEXT("HeightSteps"), 1)),
				/*bCapped=*/true,
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else if (CapturedShape.Equals(TEXT("Cone"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
				Mesh, Opts, Xform,
				N(TEXT("BaseRadius"), 50.0),
				N(TEXT("TopRadius"), 0.0),
				N(TEXT("Height"), 100.0),
				FMath::Max(3, (int32)N(TEXT("RadialSteps"), 16)),
				FMath::Max(1, (int32)N(TEXT("HeightSteps"), 1)),
				/*bCapped=*/true,
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else if (CapturedShape.Equals(TEXT("Torus"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
				Mesh, Opts, Xform,
				FGeometryScriptRevolveOptions(),
				N(TEXT("MajorRadius"), 80.0),
				N(TEXT("MinorRadius"), 20.0),
				FMath::Max(3, (int32)N(TEXT("MajorSteps"), 24)),
				FMath::Max(3, (int32)N(TEXT("MinorSteps"), 12)),
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else if (CapturedShape.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase))
		{
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCapsule(
				Mesh, Opts, Xform,
				N(TEXT("Radius"), 30.0),
				N(TEXT("LineLength"), 80.0),
				FMath::Max(2, (int32)N(TEXT("HemisphereSteps"), 6)),
				FMath::Max(3, (int32)N(TEXT("CircleSteps"), 12)),
				FMath::Max(0, (int32)N(TEXT("SegmentSteps"), 0)),
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		else
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Unknown shape '%s' — expected one of: Box, Sphere, Cylinder, Cone, Torus, Capsule"),
				*CapturedShape)));
			return;
		}

		const int32 TriCount = GetTriangleCount(Mesh);
		UE_LOG(LogNGGBridge, Log, TEXT("/mesh/append_primitive: handle='%s' shape='%s' triangles=%d"),
			*CapturedHandle, *CapturedShape, TriCount);

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("triangle_count"), TriCount);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/boolean
// Body:
//   {
//     "handle": "dragon",
//     "other_handle": "temp",
//     "op": "Union|Subtract|Intersect",
//     "transform": { ... optional transform applied to 'other' before boolean ... }
//   }
// ============================================================================

bool FNGGHttpServer::HandleMeshBoolean(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle, OtherHandle, Op;
	Body->TryGetStringField(TEXT("handle"), Handle);
	Body->TryGetStringField(TEXT("other_handle"), OtherHandle);
	Body->TryGetStringField(TEXT("op"), Op);

	if (Handle.IsEmpty() || OtherHandle.IsEmpty() || Op.IsEmpty())
	{
		OnComplete(JsonError(400,
			TEXT("'handle', 'other_handle', and 'op' are all required")));
		return true;
	}

	const FTransform OtherXform = ParseTransform(Body, TEXT("transform"));

	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/boolean target='%s' other='%s' op='%s'"),
		*Handle, *OtherHandle, *Op);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedA = Handle, CapturedB = OtherHandle, CapturedOp = Op;

	AsyncTask(ENamedThreads::GameThread,
		[Self, Callback, CapturedA, CapturedB, CapturedOp, OtherXform]()
	{
		FString Err;
		UDynamicMesh* A = ResolveHandle(Self->MeshHandles, CapturedA, Err);
		if (!A) { Callback(JsonError(404, Err)); return; }
		UDynamicMesh* B = ResolveHandle(Self->MeshHandles, CapturedB, Err);
		if (!B) { Callback(JsonError(404, Err)); return; }

		EGeometryScriptBooleanOperation EnumOp;
		if      (CapturedOp.Equals(TEXT("Union"),     ESearchCase::IgnoreCase)) EnumOp = EGeometryScriptBooleanOperation::Union;
		else if (CapturedOp.Equals(TEXT("Subtract"),  ESearchCase::IgnoreCase)) EnumOp = EGeometryScriptBooleanOperation::Subtract;
		else if (CapturedOp.Equals(TEXT("Intersect"), ESearchCase::IgnoreCase)) EnumOp = EGeometryScriptBooleanOperation::Intersection;
		else
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Unknown boolean op '%s' — expected Union, Subtract, Intersect"), *CapturedOp)));
			return;
		}

		// Capture input triangle counts before the op so an empty result can be
		// distinguished from "both inputs were already empty".
		const int32 TriA = GetTriangleCount(A);
		const int32 TriB = GetTriangleCount(B);

		FGeometryScriptMeshBooleanOptions Opts;

		// Pass a debug object so the op can report its own success/failure. Geometry
		// Script signals failure by appending Error messages here (e.g. null/invalid
		// mesh, internal CSG failure) rather than via a return code, so an empty error
		// list is the authoritative "the boolean actually ran" signal.
		UGeometryScriptDebug* Debug = NewObject<UGeometryScriptDebug>();
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
			A, FTransform::Identity, B, OtherXform, EnumOp, Opts, Debug);

		if (Debug)
		{
			FString FirstError;
			for (const FGeometryScriptDebugMessage& Msg : Debug->Messages)
			{
				if (Msg.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
				{
					FirstError = Msg.Message.ToString();
					break;
				}
			}
			if (!FirstError.IsEmpty())
			{
				Callback(JsonError(422, FString::Printf(
					TEXT("Boolean '%s' failed: %s"), *CapturedOp, *FirstError)));
				return;
			}
		}

		const int32 TriCount = GetTriangleCount(A);
		UE_LOG(LogNGGBridge, Log, TEXT("/mesh/boolean: result triangles=%d (inputs a=%d b=%d)"),
			TriCount, TriA, TriB);

		// A 0-triangle result from non-empty inputs means the boolean produced no valid
		// geometry (degenerate/non-manifold inputs, fully-cancelling subtract, etc.).
		// Conservative: only error when at least one input had geometry to work with.
		if (TriCount == 0 && (TriA > 0 || TriB > 0))
		{
			Callback(JsonError(422, FString::Printf(
				TEXT("Boolean '%s' produced no geometry (0 triangles) from inputs a=%d, b=%d — likely non-manifold/degenerate input or a fully-cancelling operation"),
				*CapturedOp, TriA, TriB)));
			return;
		}

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("triangle_count"), TriCount);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/transform
// Body: { "handle":"dragon", "location":[x,y,z], "rotation":[p,y,r], "scale":[x,y,z] }
// ============================================================================

bool FNGGHttpServer::HandleMeshTransform(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle;
	Body->TryGetStringField(TEXT("handle"), Handle);
	if (Handle.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' is required")));
		return true;
	}

	FTransform Xform = FTransform::Identity;
	double X, Y, Z;
	if (ReadTriple(Body, TEXT("location"), X, Y, Z)) Xform.SetLocation(FVector(X, Y, Z));
	if (ReadTriple(Body, TEXT("rotation"), X, Y, Z)) Xform.SetRotation(FRotator(X, Y, Z).Quaternion());
	if (ReadTriple(Body, TEXT("scale"),    X, Y, Z)) Xform.SetScale3D(FVector(X, Y, Z));

	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/transform handle='%s'"), *Handle);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle;

	AsyncTask(ENamedThreads::GameThread, [Self, Callback, CapturedHandle, Xform]()
	{
		FString Err;
		UDynamicMesh* Mesh = ResolveHandle(Self->MeshHandles, CapturedHandle, Err);
		if (!Mesh) { Callback(JsonError(404, Err)); return; }

		UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(Mesh, Xform);

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("triangle_count"), GetTriangleCount(Mesh));
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/deform
// Body: { "handle":"dragon", "op":"Bend|Twist|Taper|Flare", "axis":"X|Y|Z",
//          "amount":30, "upper":100, "lower":-100 }
// ============================================================================

bool FNGGHttpServer::HandleMeshDeform(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle, Op, Axis = TEXT("Z");
	Body->TryGetStringField(TEXT("handle"), Handle);
	Body->TryGetStringField(TEXT("op"), Op);
	Body->TryGetStringField(TEXT("axis"), Axis);
	if (Handle.IsEmpty() || Op.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' and 'op' are required")));
		return true;
	}

	double Amount = 30.0, Upper = 100.0, Lower = -100.0;
	Body->TryGetNumberField(TEXT("amount"), Amount);
	Body->TryGetNumberField(TEXT("upper"), Upper);
	Body->TryGetNumberField(TEXT("lower"), Lower);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/mesh/deform handle='%s' op='%s' axis='%s' amount=%.2f upper=%.2f lower=%.2f"),
		*Handle, *Op, *Axis, Amount, Upper, Lower);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle, CapturedOp = Op, CapturedAxis = Axis;

	AsyncTask(ENamedThreads::GameThread,
		[Self, Callback, CapturedHandle, CapturedOp, CapturedAxis, Amount, Upper, Lower]()
	{
		FString Err;
		UDynamicMesh* Mesh = ResolveHandle(Self->MeshHandles, CapturedHandle, Err);
		if (!Mesh) { Callback(JsonError(404, Err)); return; }

		const FTransform WarpFrame = AxisAlignedTransform(CapturedAxis);

		if (CapturedOp.Equals(TEXT("Bend"), ESearchCase::IgnoreCase))
		{
			// UE5.7: BendAngle and BendExtent are function parameters, not struct fields.
			// FGeometryScriptBendWarpOptions only controls bSymmetricExtents/LowerExtent/bBidirectional.
			FGeometryScriptBendWarpOptions BendOpts;
			BendOpts.bSymmetricExtents = true;
			UGeometryScriptLibrary_MeshDeformFunctions::ApplyBendWarpToMesh(
				Mesh, BendOpts, WarpFrame,
				(float)Amount,  // BendAngle
				(float)Upper);  // BendExtent
		}
		else if (CapturedOp.Equals(TEXT("Twist"), ESearchCase::IgnoreCase))
		{
			// UE5.7: TwistAngle and TwistExtent are function parameters, not struct fields.
			FGeometryScriptTwistWarpOptions TwistOpts;
			TwistOpts.bSymmetricExtents = true;
			UGeometryScriptLibrary_MeshDeformFunctions::ApplyTwistWarpToMesh(
				Mesh, TwistOpts, WarpFrame,
				(float)Amount,  // TwistAngle
				(float)Upper);  // TwistExtent
		}
		else if (CapturedOp.Equals(TEXT("Taper"), ESearchCase::IgnoreCase) ||
		         CapturedOp.Equals(TEXT("Flare"), ESearchCase::IgnoreCase))
		{
			// UE5.7: FlarePercentX, FlarePercentY, and FlareExtent are function parameters.
			FGeometryScriptFlareWarpOptions FlareOpts;
			FlareOpts.bSymmetricExtents = true;
			UGeometryScriptLibrary_MeshDeformFunctions::ApplyFlareWarpToMesh(
				Mesh, FlareOpts, WarpFrame,
				(float)Amount,  // FlarePercentX
				(float)Amount,  // FlarePercentY
				(float)Upper);  // FlareExtent
		}
		else
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Unknown deform op '%s' — expected Bend, Twist, Taper, or Flare"),
				*CapturedOp)));
			return;
		}

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("triangle_count"), GetTriangleCount(Mesh));
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/remesh
// Body: { "handle":"dragon", "target_edge_length":5.0, "iterations":10, "smoothing":0.25 }
// ============================================================================

bool FNGGHttpServer::HandleMeshRemesh(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle;
	Body->TryGetStringField(TEXT("handle"), Handle);
	if (Handle.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' is required")));
		return true;
	}

	double TargetEdgeLength = 5.0, Smoothing = 0.25, Iterations = 10.0;
	Body->TryGetNumberField(TEXT("target_edge_length"), TargetEdgeLength);
	Body->TryGetNumberField(TEXT("smoothing"), Smoothing);
	Body->TryGetNumberField(TEXT("iterations"), Iterations);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/mesh/remesh handle='%s' target_edge=%.3f iters=%d smoothing=%.3f"),
		*Handle, TargetEdgeLength, (int32)Iterations, Smoothing);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle;
	int32 CapturedIters = FMath::Clamp<int32>((int32)Iterations, 1, 200);
	float CapturedEdge  = (float)TargetEdgeLength;
	float CapturedSmooth = (float)Smoothing;

	AsyncTask(ENamedThreads::GameThread,
		[Self, Callback, CapturedHandle, CapturedIters, CapturedEdge, CapturedSmooth]()
	{
		FString Err;
		UDynamicMesh* Mesh = ResolveHandle(Self->MeshHandles, CapturedHandle, Err);
		if (!Mesh) { Callback(JsonError(404, Err)); return; }

		// UE5.7: FGeometryScriptIterativeRemeshOptions does not exist.
		// Use FGeometryScriptRemeshOptions (holds RemeshIterations/SmoothingRate) +
		// FGeometryScriptUniformRemeshOptions (holds TargetEdgeLength) with
		// UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh.
		FGeometryScriptRemeshOptions Opts;
		Opts.RemeshIterations = CapturedIters;
		Opts.SmoothingRate    = CapturedSmooth;
		FGeometryScriptUniformRemeshOptions UniformOpts;
		UniformOpts.TargetType       = EGeometryScriptUniformRemeshTargetType::TargetEdgeLength;
		UniformOpts.TargetEdgeLength = CapturedEdge;

		UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(
			Mesh, Opts, UniformOpts);

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("triangle_count"), GetTriangleCount(Mesh));
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/bake_static
// Body: { "handle":"dragon", "asset_path":"/Game/Generated/SM_Dragon" }
// ============================================================================

bool FNGGHttpServer::HandleMeshBakeStatic(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
#if !WITH_EDITOR
	OnComplete(JsonError(501, TEXT("/mesh/bake_static requires WITH_EDITOR")));
	return true;
#else
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle, AssetPath;
	Body->TryGetStringField(TEXT("handle"), Handle);
	Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (Handle.IsEmpty() || AssetPath.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' and 'asset_path' are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/bake_static handle='%s' asset_path='%s'"),
		*Handle, *AssetPath);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle, CapturedPath = AssetPath;

	AsyncTask(ENamedThreads::GameThread, [Self, Callback, CapturedHandle, CapturedPath]()
	{
		FString Err;
		UDynamicMesh* Mesh = ResolveHandle(Self->MeshHandles, CapturedHandle, Err);
		if (!Mesh) { Callback(JsonError(404, Err)); return; }

		FGeometryScriptCreateNewStaticMeshAssetOptions Opts;
		Opts.bEnableRecomputeNormals  = true;
		Opts.bEnableRecomputeTangents = true;

		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UStaticMesh* Created =
			UGeometryScriptLibrary_CreateNewAssetFunctions::CreateNewStaticMeshAssetFromMesh(
				Mesh, CapturedPath, Opts, Outcome);

		if (!Created || Outcome != EGeometryScriptOutcomePins::Success)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateNewStaticMeshAssetFromMesh failed for '%s'"), *CapturedPath)));
			return;
		}

		Created->MarkPackageDirty();
		UE_LOG(LogNGGBridge, Log, TEXT("/mesh/bake_static: created '%s'"),
			*Created->GetPathName());

		const int32 TriCount = GetTriangleCount(Mesh);
		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetStringField(TEXT("asset_path"), Created->GetPathName());
			Root->SetNumberField(TEXT("triangle_count"), TriCount);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
#endif
}

// ============================================================================
// POST /mesh/delete_handle
// Body: { "handle": "temp" }
// ============================================================================

bool FNGGHttpServer::HandleMeshDeleteHandle(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString Handle;
	Body->TryGetStringField(TEXT("handle"), Handle);
	if (Handle.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'handle' is required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("/mesh/delete_handle handle='%s'"), *Handle);

	FHttpResultCallback Callback = OnComplete;
	TSharedRef<FNGGHttpServer> Self = AsShared();
	FString CapturedHandle = Handle;

	AsyncTask(ENamedThreads::GameThread, [Self, Callback, CapturedHandle]()
	{
		const int32 Removed = Self->MeshHandles.Remove(CapturedHandle);
		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetBoolField(TEXT("removed"), Removed > 0);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// POST /mesh/add_socket
// Body:
//   {
//     "mesh_path":   "/Game/Meshes/SM_Box.SM_Box",     // required — UStaticMesh, USkeletalMesh, or USkeleton
//     "socket_name": "muzzle",                          // required
//     "bone_name":   "hand_r",                          // required for skeletal/skeleton; ignored for static
//     "location":    [x, y, z],                         // optional, default [0,0,0]
//     "rotation":    [pitch, yaw, roll],                // optional, default [0,0,0]
//     "scale":       [x, y, z],                         // optional, default [1,1,1]
//     "replace":     false,                             // optional — overwrite existing socket of same name
//     "target":      "auto"                             // optional: "auto"|"static_mesh"|"skeletal_mesh"|"skeleton"
//   }
// Response: { "ok": true, "asset_path", "asset_class", "target", "socket_name", "bone_name?", "replaced" }
// ============================================================================

bool FNGGHttpServer::HandleMeshAddSocket(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
#if !WITH_EDITOR
	OnComplete(JsonError(501, TEXT("/mesh/add_socket requires WITH_EDITOR")));
	return true;
#else
	FString ParseErr;
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonBody(Req, Body, ParseErr))
	{
		OnComplete(JsonError(400, ParseErr));
		return true;
	}

	FString MeshPath, SocketName, BoneName, Target = TEXT("auto");
	Body->TryGetStringField(TEXT("mesh_path"),   MeshPath);
	Body->TryGetStringField(TEXT("socket_name"), SocketName);
	Body->TryGetStringField(TEXT("bone_name"),   BoneName);
	Body->TryGetStringField(TEXT("target"),      Target);
	if (MeshPath.IsEmpty() || SocketName.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'mesh_path' and 'socket_name' are required")));
		return true;
	}

	bool bReplace = false;
	Body->TryGetBoolField(TEXT("replace"), bReplace);

	FVector  Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector  Scale    = FVector::OneVector;
	double X, Y, Z;
	if (ReadTriple(Body, TEXT("location"), X, Y, Z)) Location = FVector(X, Y, Z);
	if (ReadTriple(Body, TEXT("rotation"), X, Y, Z)) Rotation = FRotator(X, Y, Z);
	if (ReadTriple(Body, TEXT("scale"),    X, Y, Z)) Scale    = FVector(X, Y, Z);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/mesh/add_socket mesh='%s' socket='%s' bone='%s' target='%s' replace=%d"),
		*MeshPath, *SocketName, *BoneName, *Target, bReplace ? 1 : 0);

	FHttpResultCallback Callback = OnComplete;
	const FString CapturedMesh   = MeshPath;
	const FString CapturedSocket = SocketName;
	const FString CapturedBone   = BoneName;
	const FString CapturedTarget = Target;
	const bool    CapturedReplace = bReplace;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedMesh, CapturedSocket, CapturedBone, CapturedTarget,
		 Location, Rotation, Scale, CapturedReplace]()
	{
		// LoadObject accepts either "/Game/Path/Asset" or "/Game/Path/Asset.Asset".
		// Try the literal path first, then fall back to "<path>.<shortname>".
		UObject* Asset = LoadObject<UObject>(nullptr, *CapturedMesh);
		if (!Asset)
		{
			int32 DotIdx;
			if (!CapturedMesh.FindChar(TEXT('.'), DotIdx))
			{
				const FString ShortName = FPackageName::GetShortName(CapturedMesh);
				const FString Full = CapturedMesh + TEXT(".") + ShortName;
				Asset = LoadObject<UObject>(nullptr, *Full);
			}
		}
		if (!Asset)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Asset not found at '%s'"), *CapturedMesh)));
			return;
		}

		const FName SocketFName(*CapturedSocket);
		const FName BoneFName(*CapturedBone);

		UStaticMesh*   StaticMeshObj   = Cast<UStaticMesh>(Asset);
		USkeletalMesh* SkeletalMeshObj = Cast<USkeletalMesh>(Asset);
		USkeleton*     SkeletonObj     = Cast<USkeleton>(Asset);

		// Honour explicit target override (else auto-detect via the first non-null cast).
		if (CapturedTarget.Equals(TEXT("static_mesh"), ESearchCase::IgnoreCase))
		{
			if (!StaticMeshObj)
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("target='static_mesh' but asset is %s"), *Asset->GetClass()->GetName())));
				return;
			}
			SkeletalMeshObj = nullptr; SkeletonObj = nullptr;
		}
		else if (CapturedTarget.Equals(TEXT("skeletal_mesh"), ESearchCase::IgnoreCase))
		{
			if (!SkeletalMeshObj)
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("target='skeletal_mesh' but asset is %s"), *Asset->GetClass()->GetName())));
				return;
			}
			StaticMeshObj = nullptr; SkeletonObj = nullptr;
		}
		else if (CapturedTarget.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
		{
			if (!SkeletonObj)
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("target='skeleton' but asset is %s"), *Asset->GetClass()->GetName())));
				return;
			}
			StaticMeshObj = nullptr; SkeletalMeshObj = nullptr;
		}

		FString ResolvedTarget;
		bool    bDidReplace = false;

		if (StaticMeshObj)
		{
			if (UStaticMeshSocket* Existing = StaticMeshObj->FindSocket(SocketFName))
			{
				if (!CapturedReplace)
				{
					Callback(JsonError(409, FString::Printf(
						TEXT("Socket '%s' already exists on '%s' — pass replace=true to overwrite"),
						*CapturedSocket, *StaticMeshObj->GetName())));
					return;
				}
				StaticMeshObj->RemoveSocket(Existing);
				bDidReplace = true;
			}

			UStaticMeshSocket* NewSocket = NewObject<UStaticMeshSocket>(StaticMeshObj);
			NewSocket->SetFlags(RF_Transactional);
			NewSocket->SocketName       = SocketFName;
			NewSocket->RelativeLocation = Location;
			NewSocket->RelativeRotation = Rotation;
			NewSocket->RelativeScale    = Scale;
			StaticMeshObj->AddSocket(NewSocket);

			StaticMeshObj->MarkPackageDirty();
			StaticMeshObj->PostEditChange();
			ResolvedTarget = TEXT("static_mesh");
		}
		else if (SkeletalMeshObj)
		{
			// Validate bone name (required) against the owning skeleton.
			if (CapturedBone.IsEmpty())
			{
				Callback(JsonError(400, TEXT("'bone_name' is required for skeletal mesh sockets")));
				return;
			}
			if (USkeleton* Skel = SkeletalMeshObj->GetSkeleton())
			{
				if (Skel->GetReferenceSkeleton().FindBoneIndex(BoneFName) == INDEX_NONE)
				{
					Callback(JsonError(400, FString::Printf(
						TEXT("Bone '%s' not found in skeleton of '%s'"),
						*CapturedBone, *SkeletalMeshObj->GetName())));
					return;
				}
			}

			if (USkeletalMeshSocket* Existing = SkeletalMeshObj->FindSocket(SocketFName))
			{
				if (!CapturedReplace)
				{
					Callback(JsonError(409, FString::Printf(
						TEXT("Socket '%s' already exists on '%s' — pass replace=true to overwrite"),
						*CapturedSocket, *SkeletalMeshObj->GetName())));
					return;
				}
				// FindSocket searches both mesh-only and skeleton sockets; we only own
				// the mesh-only list, so remove from there. Skeleton-owned sockets stay.
				TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = SkeletalMeshObj->GetMeshOnlySocketList();
				MeshSockets.RemoveAll([SocketFName](const TObjectPtr<USkeletalMeshSocket>& S)
				{
					return S && S->SocketName == SocketFName;
				});
				bDidReplace = true;
			}

			USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(SkeletalMeshObj);
			NewSocket->SetFlags(RF_Transactional);
			NewSocket->SocketName       = SocketFName;
			NewSocket->BoneName         = BoneFName;
			NewSocket->RelativeLocation = Location;
			NewSocket->RelativeRotation = Rotation;
			NewSocket->RelativeScale    = Scale;
			SkeletalMeshObj->AddSocket(NewSocket, /*bAddToSkeleton=*/false);

			SkeletalMeshObj->MarkPackageDirty();
			SkeletalMeshObj->PostEditChange();
			ResolvedTarget = TEXT("skeletal_mesh");
		}
		else if (SkeletonObj)
		{
			if (CapturedBone.IsEmpty())
			{
				Callback(JsonError(400, TEXT("'bone_name' is required for skeleton sockets")));
				return;
			}
			if (SkeletonObj->GetReferenceSkeleton().FindBoneIndex(BoneFName) == INDEX_NONE)
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("Bone '%s' not found in skeleton '%s'"),
					*CapturedBone, *SkeletonObj->GetName())));
				return;
			}

			if (USkeletalMeshSocket* Existing = SkeletonObj->FindSocket(SocketFName))
			{
				if (!CapturedReplace)
				{
					Callback(JsonError(409, FString::Printf(
						TEXT("Socket '%s' already exists on skeleton '%s' — pass replace=true to overwrite"),
						*CapturedSocket, *SkeletonObj->GetName())));
					return;
				}
				SkeletonObj->Sockets.RemoveAll([SocketFName](const TObjectPtr<USkeletalMeshSocket>& S)
				{
					return S && S->SocketName == SocketFName;
				});
				bDidReplace = true;
			}

			USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(SkeletonObj);
			NewSocket->SetFlags(RF_Transactional);
			NewSocket->SocketName       = SocketFName;
			NewSocket->BoneName         = BoneFName;
			NewSocket->RelativeLocation = Location;
			NewSocket->RelativeRotation = Rotation;
			NewSocket->RelativeScale    = Scale;
			SkeletonObj->Sockets.Add(NewSocket);

			SkeletonObj->MarkPackageDirty();
			SkeletonObj->PostEditChange();
			ResolvedTarget = TEXT("skeleton");
		}
		else
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Asset '%s' is not a UStaticMesh, USkeletalMesh, or USkeleton (got %s)"),
				*CapturedMesh, *Asset->GetClass()->GetName())));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/mesh/add_socket: added '%s' to %s '%s' (replaced=%d)"),
			*CapturedSocket, *ResolvedTarget, *Asset->GetPathName(), bDidReplace ? 1 : 0);

		const FString AssetPathOut  = Asset->GetPathName();
		const FString AssetClassOut = Asset->GetClass()->GetName();

		const FString BodyStr = OkBody([&](TSharedRef<FJsonObject>& Root)
		{
			Root->SetStringField(TEXT("asset_path"),  AssetPathOut);
			Root->SetStringField(TEXT("asset_class"), AssetClassOut);
			Root->SetStringField(TEXT("target"),      ResolvedTarget);
			Root->SetStringField(TEXT("socket_name"), CapturedSocket);
			if (!CapturedBone.IsEmpty())
			{
				Root->SetStringField(TEXT("bone_name"), CapturedBone);
			}
			Root->SetBoolField(TEXT("replaced"), bDidReplace);
		});
		Callback(JsonOk(BodyStr));
	});

	return true;
#endif
}

