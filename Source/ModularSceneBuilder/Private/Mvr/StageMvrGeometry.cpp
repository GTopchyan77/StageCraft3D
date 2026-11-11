// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrGeometry.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace StageMvrGeometry
{
	namespace
	{
		uint32 ReadU32(TConstArrayView<uint8> Bytes, int64 Offset)
		{
			return uint32(Bytes[Offset]) | (uint32(Bytes[Offset + 1]) << 8) | (uint32(Bytes[Offset + 2]) << 16) | (uint32(Bytes[Offset + 3]) << 24);
		}

		uint16 ReadU16(TConstArrayView<uint8> Bytes, int64 Offset)
		{
			return uint16(Bytes[Offset]) | uint16(uint16(Bytes[Offset + 1]) << 8);
		}

		float ReadF32(TConstArrayView<uint8> Bytes, int64 Offset)
		{
			const uint32 Bits = ReadU32(Bytes, Offset);
			float Value;
			FMemory::Memcpy(&Value, &Bits, sizeof(Value));
			return Value;
		}

		/** Reads Count finite numbers from Field of Object. */
		bool ReadNumbers(const FJsonObject& Object, const TCHAR* Field, int32 Count, TArray<double>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.TryGetArrayField(Field, Values) || Values->Num() != Count)
			{
				return false;
			}
			Out.Reset(Count);
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
				{
					return false;
				}
				Out.Add(Number);
			}
			return true;
		}

		/** A glTF node's local transform (column-major matrix, or translation, rotation and scale), as an Unreal matrix (row vectors). */
		FMatrix NodeMatrix(const FJsonObject& Node)
		{
			TArray<double> Values;
			if (ReadNumbers(Node, TEXT("matrix"), 16, Values))
			{
				// glTF stores column-major for column vectors; read in order, that is the row-vector matrix Unreal uses.
				FMatrix Matrix;
				for (int32 Index = 0; Index < 16; ++Index)
				{
					Matrix.M[Index / 4][Index % 4] = Values[Index];
				}
				return Matrix;
			}
			FVector Translation = FVector::ZeroVector;
			FQuat Rotation = FQuat::Identity;
			FVector Scale = FVector::OneVector;
			if (ReadNumbers(Node, TEXT("translation"), 3, Values))
			{
				Translation = FVector(Values[0], Values[1], Values[2]);
			}
			if (ReadNumbers(Node, TEXT("rotation"), 4, Values))
			{
				Rotation = FQuat(Values[0], Values[1], Values[2], Values[3]);
				Rotation = Rotation.IsNormalized() ? Rotation : Rotation.GetNormalized();
			}
			if (ReadNumbers(Node, TEXT("scale"), 3, Values))
			{
				Scale = FVector(Values[0], Values[1], Values[2]);
			}
			// The same maths (T * R * S for column vectors) in Unreal's row-vector order: scale, then rotate, then translate.
			return FScaleMatrix(Scale) * FQuatRotationTranslationMatrix(Rotation, Translation);
		}

		struct FGltfReader
		{
			const FJsonObject& Root;
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Accessors = nullptr;
			FBox Bounds = FBox(ForceInit);
			int32 Visited = 0;
			FString Error;

			explicit FGltfReader(const FJsonObject& InRoot) : Root(InRoot)
			{
				Root.TryGetArrayField(TEXT("nodes"), Nodes);
				Root.TryGetArrayField(TEXT("meshes"), Meshes);
				Root.TryGetArrayField(TEXT("accessors"), Accessors);
			}

			static const FJsonObject* At(const TArray<TSharedPtr<FJsonValue>>* Array, int32 Index)
			{
				if (!Array || !Array->IsValidIndex(Index) || !(*Array)[Index].IsValid() || (*Array)[Index]->Type != EJson::Object)
				{
					return nullptr;
				}
				return (*Array)[Index]->AsObject().Get();
			}

			void AddMesh(int32 MeshIndex, const FMatrix& World)
			{
				const FJsonObject* Mesh = At(Meshes, MeshIndex);
				const TArray<TSharedPtr<FJsonValue>>* Primitives = nullptr;
				if (!Mesh || !Mesh->TryGetArrayField(TEXT("primitives"), Primitives))
				{
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : *Primitives)
				{
					const TSharedPtr<FJsonObject>* Primitive = nullptr;
					const TSharedPtr<FJsonObject>* Attributes = nullptr;
					int32 Position = INDEX_NONE;
					if (!Value.IsValid() || !Value->TryGetObject(Primitive) || !(*Primitive)->TryGetObjectField(TEXT("attributes"), Attributes)
						|| !(*Attributes)->TryGetNumberField(TEXT("POSITION"), Position))
					{
						continue;
					}
					const FJsonObject* Accessor = At(Accessors, Position);
					TArray<double> Min;
					TArray<double> Max;
					if (!Accessor || !ReadNumbers(*Accessor, TEXT("min"), 3, Min) || !ReadNumbers(*Accessor, TEXT("max"), 3, Max))
					{
						continue;
					}
					const FBox Local(FVector(Min[0], Min[1], Min[2]), FVector(Max[0], Max[1], Max[2]));
					Bounds += Local.TransformBy(World);
				}
			}

			bool AddNode(int32 NodeIndex, const FMatrix& Parent, int32 Depth)
			{
				const FJsonObject* Node = At(Nodes, NodeIndex);
				if (!Node)
				{
					return true;
				}
				if (Depth > MaxNodeDepth || ++Visited > MaxNodes)
				{
					Error = FString::Printf(TEXT("the node tree is deeper than %d or has more than %d nodes"), MaxNodeDepth, MaxNodes);
					return false;
				}
				const FMatrix World = NodeMatrix(*Node) * Parent;
				int32 Mesh = INDEX_NONE;
				if (Node->TryGetNumberField(TEXT("mesh"), Mesh))
				{
					AddMesh(Mesh, World);
				}
				const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
				if (Node->TryGetArrayField(TEXT("children"), Children))
				{
					for (const TSharedPtr<FJsonValue>& Child : *Children)
					{
						double Index = -1.0;
						if (Child.IsValid() && Child->TryGetNumber(Index) && !AddNode(int32(Index), World, Depth + 1))
						{
							return false;
						}
					}
				}
				return true;
			}

			bool Run()
			{
				// The default scene's roots; without scenes, every node that is no other node's child.
				TArray<int32> Roots;
				const TArray<TSharedPtr<FJsonValue>>* Scenes = nullptr;
				int32 SceneIndex = 0;
				Root.TryGetNumberField(TEXT("scene"), SceneIndex);
				const FJsonObject* Scene = Root.TryGetArrayField(TEXT("scenes"), Scenes) ? At(Scenes, SceneIndex) : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* SceneNodes = nullptr;
				if (Scene && Scene->TryGetArrayField(TEXT("nodes"), SceneNodes))
				{
					for (const TSharedPtr<FJsonValue>& Value : *SceneNodes)
					{
						double Index = -1.0;
						if (Value.IsValid() && Value->TryGetNumber(Index))
						{
							Roots.Add(int32(Index));
						}
					}
				}
				else if (Nodes)
				{
					TSet<int32> Children;
					for (int32 Index = 0; Index < Nodes->Num() && Index < MaxNodes; ++Index)
					{
						const TArray<TSharedPtr<FJsonValue>>* Kids = nullptr;
						const FJsonObject* Node = At(Nodes, Index);
						if (Node && Node->TryGetArrayField(TEXT("children"), Kids))
						{
							for (const TSharedPtr<FJsonValue>& Kid : *Kids)
							{
								double Child = -1.0;
								if (Kid.IsValid() && Kid->TryGetNumber(Child))
								{
									Children.Add(int32(Child));
								}
							}
						}
					}
					for (int32 Index = 0; Index < Nodes->Num() && Index < MaxNodes; ++Index)
					{
						if (!Children.Contains(Index))
						{
							Roots.Add(Index);
						}
					}
				}
				for (const int32 Node : Roots)
				{
					if (!AddNode(Node, FMatrix::Identity, 0))
					{
						return false;
					}
				}
				if (!Bounds.IsValid)
				{
					Error = TEXT("it has no mesh positions with bounds");
					return false;
				}
				return true;
			}
		};

		bool ReadGltfJson(FStringView Json, FBox& OutBounds, FString& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::CreateFromView(Json);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				OutError = TEXT("its glTF JSON cannot be read");
				return false;
			}
			FGltfReader Gltf(*Root);
			if (!Gltf.Run())
			{
				OutError = Gltf.Error;
				return false;
			}
			// glTF metres, Y up -> MVR millimetres, Z up: (x, y, z) -> (x, -z, y) * 1000.
			FBox Converted(ForceInit);
			const FVector Corners[2] = { Gltf.Bounds.Min, Gltf.Bounds.Max };
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Point(Corners[Corner & 1].X, Corners[(Corner >> 1) & 1].Y, Corners[(Corner >> 2) & 1].Z);
				Converted += FVector(Point.X, -Point.Z, Point.Y) * 1000.0;
			}
			OutBounds = Converted;
			return true;
		}

		FString Utf8ToString(TConstArrayView<uint8> Bytes)
		{
			const int32 Skip = (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF) ? 3 : 0;
			const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Skip), Bytes.Num() - Skip);
			return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
		}
	}

	bool ReadGltfBounds(TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError)
	{
		constexpr uint32 GlbMagic = 0x46546C67; // "glTF"
		constexpr uint32 JsonChunk = 0x4E4F534A; // "JSON"
		if (Bytes.Num() >= 12 && ReadU32(Bytes, 0) == GlbMagic)
		{
			const uint32 Version = ReadU32(Bytes, 4);
			const uint32 Length = ReadU32(Bytes, 8);
			if (Version != 2 || Length > uint32(Bytes.Num()) || Bytes.Num() < 20)
			{
				OutError = TEXT("it is not a glTF 2.0 binary file");
				return false;
			}
			const uint32 ChunkLength = ReadU32(Bytes, 12);
			if (ReadU32(Bytes, 16) != JsonChunk || ChunkLength > Length - 20 || ChunkLength > uint32(MaxJsonBytes))
			{
				OutError = TEXT("its JSON chunk is missing or too large");
				return false;
			}
			return ReadGltfJson(Utf8ToString(Bytes.Slice(20, int32(ChunkLength))), OutBounds, OutError);
		}
		if (Bytes.Num() > MaxJsonBytes)
		{
			OutError = TEXT("its glTF JSON is too large");
			return false;
		}
		return ReadGltfJson(Utf8ToString(Bytes), OutBounds, OutError);
	}

	bool Read3dsBounds(TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError)
	{
		// Chunks are id (u16) + length (u32, header included). Containers walked: main 4D4D, editor 3D3D, object 4000 (after its
		// name), triangle mesh 4100; vertex list 4110 is u16 count + count * 3 floats.
		constexpr uint16 Main = 0x4D4D;
		constexpr uint16 Editor = 0x3D3D;
		constexpr uint16 Object = 0x4000;
		constexpr uint16 Mesh = 0x4100;
		constexpr uint16 Vertices = 0x4110;

		if (Bytes.Num() < 6 || ReadU16(Bytes, 0) != Main)
		{
			OutError = TEXT("it is not a 3DS file");
			return false;
		}
		FBox Bounds(ForceInit);
		int32 Chunks = 0;
		struct FRange { int64 Begin; int64 End; };
		TArray<FRange, TInlineAllocator<8>> Stack;
		Stack.Add({ 0, int64(Bytes.Num()) });
		while (!Stack.IsEmpty())
		{
			const FRange Range = Stack.Pop(EAllowShrinking::No);
			int64 Offset = Range.Begin;
			while (Offset + 6 <= Range.End)
			{
				if (++Chunks > Max3dsChunks)
				{
					OutError = TEXT("it has too many chunks");
					return false;
				}
				const uint16 Id = ReadU16(Bytes, Offset);
				const int64 Length = int64(ReadU32(Bytes, Offset + 2));
				if (Length < 6 || Offset + Length > Range.End)
				{
					break; // A damaged chunk ends this level; what was read so far still counts.
				}
				const int64 Body = Offset + 6;
				const int64 End = Offset + Length;
				if (Id == Main || Id == Editor || Id == Mesh)
				{
					Stack.Add({ Body, End });
				}
				else if (Id == Object)
				{
					int64 NameEnd = Body;
					while (NameEnd < End && Bytes[NameEnd] != 0)
					{
						++NameEnd;
					}
					if (NameEnd < End)
					{
						Stack.Add({ NameEnd + 1, End });
					}
				}
				else if (Id == Vertices && Body + 2 <= End)
				{
					const int64 Count = ReadU16(Bytes, Body);
					if (Body + 2 + Count * 12 <= End)
					{
						for (int64 Index = 0; Index < Count; ++Index)
						{
							const int64 At = Body + 2 + Index * 12;
							const FVector Point(ReadF32(Bytes, At), ReadF32(Bytes, At + 4), ReadF32(Bytes, At + 8));
							if (!Point.ContainsNaN() && Point.GetAbsMax() < 1e9)
							{
								Bounds += Point;
							}
						}
					}
				}
				Offset = End;
			}
		}
		if (!Bounds.IsValid)
		{
			OutError = TEXT("it has no vertices");
			return false;
		}
		OutBounds = Bounds;
		return true;
	}

	bool ReadBounds(const FString& FileName, TConstArrayView<uint8> Bytes, FBox& OutBounds, FString& OutError)
	{
		const FString Extension = FPaths::GetExtension(FileName).ToLower();
		if (Extension == TEXT("glb") || Extension == TEXT("gltf"))
		{
			return ReadGltfBounds(Bytes, OutBounds, OutError);
		}
		if (Extension == TEXT("3ds"))
		{
			return Read3dsBounds(Bytes, OutBounds, OutError);
		}
		OutError = FString::Printf(TEXT("\"%s\" files are not read"), *Extension);
		return false;
	}

	TArray<uint8> WriteBoxGlb(const FBox& Bounds, const FString& Name)
	{
		const FVector3f Min(float(Bounds.Min.X / 1000.0), float(Bounds.Min.Z / 1000.0), float(-Bounds.Max.Y / 1000.0));
		const FVector3f Max(float(Bounds.Max.X / 1000.0), float(Bounds.Max.Z / 1000.0), float(-Bounds.Min.Y / 1000.0));
		constexpr uint16 Indices[36] = { 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6 };

		TArray<uint8> Binary;
		const auto Append = [&Binary](const void* Data, int32 Size) { Binary.Append(static_cast<const uint8*>(Data), Size); };
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const float Position[3] = { (Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z };
			Append(Position, sizeof(Position));
		}
		const int32 PositionBytes = Binary.Num();
		Append(Indices, sizeof(Indices));
		const int32 IndexBytes = Binary.Num() - PositionBytes;
		while (Binary.Num() % 4 != 0)
		{
			Binary.Add(0);
		}

		FString SafeName;
		for (const TCHAR Character : Name)
		{
			if (Character == TEXT('"') || Character == TEXT('\\'))
			{
				SafeName.AppendChar(TEXT('\\'));
				SafeName.AppendChar(Character);
			}
			else if (Character >= 0x20)
			{
				SafeName.AppendChar(Character);
			}
		}
		const auto Number = [](float Value) { return FString::Printf(TEXT("%.9g"), double(Value)); };
		const FString Json = FString::Printf(
			TEXT("{\"asset\":{\"version\":\"2.0\",\"generator\":\"StageCraft\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],")
			TEXT("\"nodes\":[{\"mesh\":0,\"name\":\"%s\"}],\"meshes\":[{\"name\":\"%s\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],")
			TEXT("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\",\"min\":[%s,%s,%s],\"max\":[%s,%s,%s]},")
			TEXT("{\"bufferView\":1,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],")
			TEXT("\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%d,\"target\":34962},{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d,\"target\":34963}],")
			TEXT("\"buffers\":[{\"byteLength\":%d}]}"),
			*SafeName, *SafeName, *Number(Min.X), *Number(Min.Y), *Number(Min.Z), *Number(Max.X), *Number(Max.Y), *Number(Max.Z),
			PositionBytes, PositionBytes, IndexBytes, Binary.Num());
		const FTCHARToUTF8 JsonUtf8(*Json);
		TArray<uint8> JsonChunk(reinterpret_cast<const uint8*>(JsonUtf8.Get()), JsonUtf8.Length());
		while (JsonChunk.Num() % 4 != 0)
		{
			JsonChunk.Add(uint8(' '));
		}

		TArray<uint8> Glb;
		const auto Write32 = [&Glb](uint32 Value)
		{
			for (int32 Shift = 0; Shift < 32; Shift += 8)
			{
				Glb.Add(uint8((Value >> Shift) & 0xFF));
			}
		};
		Write32(0x46546C67);
		Write32(2);
		Write32(uint32(12 + 8 + JsonChunk.Num() + 8 + Binary.Num()));
		Write32(uint32(JsonChunk.Num()));
		Write32(0x4E4F534A);
		Glb.Append(JsonChunk);
		Write32(uint32(Binary.Num()));
		Write32(0x004E4942);
		Glb.Append(Binary);
		return Glb;
	}
}
