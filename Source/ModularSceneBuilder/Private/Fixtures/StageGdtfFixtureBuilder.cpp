// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfFixtureBuilder.h"

#include "Engine/StaticMesh.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "Show/StageFixtureCells.h"
#include "StaticMeshAttributes.h"

namespace StageGdtfFixtureBuilder
{
	namespace
	{
		const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
		const TCHAR* const CylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
		const TCHAR* const BasicShapeMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
		const FName MaterialSlot(TEXT("Body"));
		constexpr double MinPartCm = 1.0;
		constexpr double MaxPartCm = 500.0;
		constexpr int32 MaxDescriptionLength = 1000;
		constexpr int32 MaxLabelLength = 64;

		bool IsPositive(const FVector& Size)
		{
			return Size.X > 0.0 && Size.Y > 0.0 && Size.Z > 0.0;
		}

		FVector ClampPart(const FVector& Size)
		{
			return FVector(FMath::Clamp(Size.X, MinPartCm, MaxPartCm), FMath::Clamp(Size.Y, MinPartCm, MaxPartCm), FMath::Clamp(Size.Z, MinPartCm, MaxPartCm));
		}

		bool HasBoxHead(const FStageGdtfCapabilities& Capabilities, EStageFixtureKind Kind)
		{
			return Capabilities.CellCount > 1 || Kind == EStageFixtureKind::PixelBar || Kind == EStageFixtureKind::Blinder || Kind == EStageFixtureKind::Strobe;
		}

		FVector DefaultHead(const FStageGdtfCapabilities& Capabilities, EStageFixtureKind Kind)
		{
			if (Capabilities.bHasPanTilt)
			{
				return FVector(26.0, 26.0, 36.0);
			}
			if (Capabilities.CellCount > 1 || Kind == EStageFixtureKind::PixelBar)
			{
				const double Length = Capabilities.DimensionsCm.X > 0.0 ? Capabilities.DimensionsCm.X : FMath::Max(30.0, Capabilities.CellCount * 6.0);
				return FVector(Length, 10.0, 10.0);
			}
			if (Kind == EStageFixtureKind::Strobe || Kind == EStageFixtureKind::Blinder)
			{
				return FVector(40.0, 15.0, 12.0);
			}
			return FVector(28.0, 28.0, 24.0);
		}

		FText MakeDisplayName(const FStageGdtfFixtureType& Type, const FString& Mode)
		{
			const FString Name = Type.Manufacturer.IsEmpty() ? Type.Name : Type.Manufacturer + TEXT(" ") + Type.Name;
			return FText::FromString(FString::Printf(TEXT("%s (%s)"), *Name.Left(StageFixtureLibrary::MaxRefTextLength), *Mode.Left(StageFixtureLibrary::MaxRefTextLength)));
		}

		TArray<FStageGoboSlot> MakeGoboSlots(TConstArrayView<FStageGdtfSlotOut> Slots)
		{
			TArray<FStageGoboSlot> Out;
			Out.Reserve(Slots.Num());
			for (const FStageGdtfSlotOut& Slot : Slots)
			{
				FStageGoboSlot& Gobo = Out.AddDefaulted_GetRef();
				Gobo.Name = FText::FromString(Slot.Name);
			}
			return Out;
		}

		FMeshDescription MakeBoxDescription(const FVector3f& Half)
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
			UVs.SetNumChannels(1);

			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = MaterialSlot;

			struct FFace
			{
				FVector3f Normal;
				FVector3f U;
				FVector3f V;
			};
			const FFace Faces[] = {
				{ FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 1.f, 0.f), FVector3f(0.f, 0.f, 1.f) },
				{ FVector3f(-1.f, 0.f, 0.f), FVector3f(0.f, -1.f, 0.f), FVector3f(0.f, 0.f, 1.f) },
				{ FVector3f(0.f, 1.f, 0.f), FVector3f(-1.f, 0.f, 0.f), FVector3f(0.f, 0.f, 1.f) },
				{ FVector3f(0.f, -1.f, 0.f), FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 0.f, 1.f) },
				{ FVector3f(0.f, 0.f, 1.f), FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 1.f, 0.f) },
				{ FVector3f(0.f, 0.f, -1.f), FVector3f(0.f, 1.f, 0.f), FVector3f(1.f, 0.f, 0.f) },
			};
			const FVector2f Corners[] = { FVector2f(-1.f, -1.f), FVector2f(1.f, -1.f), FVector2f(1.f, 1.f), FVector2f(-1.f, 1.f) };

			Description.ReserveNewVertices(24);
			Description.ReserveNewVertexInstances(24);
			Description.ReserveNewTriangles(12);
			for (const FFace& Face : Faces)
			{
				FVertexInstanceID Instances[4];
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					const FVector3f Unit = Face.Normal + Face.U * Corners[Corner].X + Face.V * Corners[Corner].Y;
					const FVertexID Vertex = Description.CreateVertex();
					Positions[Vertex] = Unit * Half;
					Instances[Corner] = Description.CreateVertexInstance(Vertex);
					Normals[Instances[Corner]] = Face.Normal;
					Tangents[Instances[Corner]] = Face.U;
					BinormalSigns[Instances[Corner]] = 1.f;
					Colors[Instances[Corner]] = FVector4f(1.f, 1.f, 1.f, 1.f);
					UVs.Set(Instances[Corner], 0, (Corners[Corner] + FVector2f(1.f, 1.f)) * 0.5f);
				}
				Description.CreateTriangle(Group, { Instances[0], Instances[2], Instances[1] });
				Description.CreateTriangle(Group, { Instances[0], Instances[3], Instances[2] });
			}
			return Description;
		}
	}

	FBodyLayout MakeBodyLayout(const FStageGdtfCapabilities& Capabilities, EStageFixtureKind Kind)
	{
		FBodyLayout Body;
		const bool bMoving = Capabilities.bHasPanTilt;
		Body.bBoxHead = HasBoxHead(Capabilities, Kind);
		Body.HeadSizeCm = ClampPart(IsPositive(Capabilities.HeadSizeCm) ? Capabilities.HeadSizeCm : DefaultHead(Capabilities, Kind));

		if (bMoving)
		{
			Body.BaseSizeCm = ClampPart(IsPositive(Capabilities.BaseSizeCm) ? Capabilities.BaseSizeCm
				: FVector(FMath::Max(Body.HeadSizeCm.X * 1.5, 30.0), FMath::Max(Body.HeadSizeCm.Y, 24.0), 15.0));
			Body.YokeSizeCm = ClampPart(IsPositive(Capabilities.YokeSizeCm) ? Capabilities.YokeSizeCm : FVector(Body.BaseSizeCm.X * 0.9, 12.0, 6.0));
		}
		else
		{
			Body.BaseSizeCm = ClampPart(FVector(Body.HeadSizeCm.X * 0.9, FMath::Max(Body.HeadSizeCm.Y * 0.6, 6.0), 3.0));
		}

		const double HeadFace = FMath::Min(Body.HeadSizeCm.X, Body.HeadSizeCm.Y);
		if (Body.bBoxHead)
		{
			Body.CellStripSizeCm = FVector2D(FMath::Max(Body.HeadSizeCm.X - 2.0, MinPartCm), FMath::Max(Body.HeadSizeCm.Y * 0.4, 2.0));
		}
		else
		{
			Body.LensDiameterCm = static_cast<float>(Capabilities.LensDiameterCm > 0.f ? FMath::Min<double>(Capabilities.LensDiameterCm, HeadFace) : HeadFace * 0.6);
		}

		const double TiltHeight = Body.HeadSizeCm.Z * 0.5 + 2.0;
		Body.PanPivotOffset = FVector(0.0, 0.0, Body.BaseSizeCm.Z * 0.5);
		Body.TiltPivotOffset = FVector(0.0, 0.0, (bMoving ? Body.YokeSizeCm.Z * 0.5 : 0.0) + TiltHeight);
		Body.BeamOriginOffset = FVector(0.0, 0.0, Body.HeadSizeCm.Z * 0.5 + 0.6);
		return Body;
	}

	TArray<FStageFixtureDMXChannel> MakeDMXChannels(const FStageGdtfModeLayout& Layout)
	{
		TArray<FStageFixtureDMXChannel> Channels;
		Channels.Reserve(Layout.Channels.Num());
		for (const FStageGdtfLayoutChannel& Source : Layout.Channels)
		{
			if (Source.Offsets.IsEmpty())
			{
				continue;
			}
			const bool bDriven = (Source.Status == EStageGdtfChannelStatus::Mapped || Source.Status == EStageGdtfChannelStatus::Approximated) && Source.Attribute.IsValid();
			const FName Label(*Source.GdtfAttribute.Left(MaxLabelLength));
			const bool bFineFollows = Source.Offsets.Num() >= 2 && Source.Offsets[1] == Source.Offsets[0] + 1;

			FStageFixtureDMXChannel& First = Channels.AddDefaulted_GetRef();
			First.Attribute = bDriven ? Source.Attribute : FGameplayTag();
			First.Channel = Source.Offsets[0];
			First.b16Bit = bFineFollows;
			First.Label = Label;

			for (int32 Index = bFineFollows ? 2 : 1; Index < Source.Offsets.Num(); ++Index)
			{
				FStageFixtureDMXChannel& Extra = Channels.AddDefaulted_GetRef();
				Extra.Channel = Source.Offsets[Index];
				Extra.Label = Label;
			}
		}
		Channels.StableSort([](const FStageFixtureDMXChannel& A, const FStageFixtureDMXChannel& B) { return A.Channel < B.Channel; });
		return Channels;
	}

	void Fill(ULightingFixtureData& Data, const FStageGdtfFixtureType& Type, const FStageGdtfCapabilities& Capabilities, const FStageGdtfModeLayout& Layout,
		TOptional<EStageFixtureKind> Kind, UStaticMesh* BaseMesh)
	{
		const EStageFixtureKind FixtureKind = Kind.Get(Capabilities.Kind);

		Data.DisplayName = MakeDisplayName(Type, Layout.Name);
		Data.Description = FText::FromString(Type.Description.Left(MaxDescriptionLength));
		Data.CategoryTag = Kind.IsSet() ? StageGdtfMapping::CategoryForKind(FixtureKind) : Capabilities.Category;
		Data.Specs.Manufacturer = FText::FromString(Type.Manufacturer.Left(StageFixtureLibrary::MaxRefTextLength));
		Data.Specs.WeightKg = FMath::Max(Capabilities.WeightKg, 0.f);
		Data.Specs.PowerDrawWatts = FMath::Max(Capabilities.PowerW, 0.f);
		Data.Specs.DimensionsCm = Capabilities.DimensionsCm.ComponentMax(FVector::ZeroVector);

		Data.FixtureKind = FixtureKind;
		Data.ColorSystem = Capabilities.ColorSystem;
		Data.ColorTemperatureK = FMath::Clamp(Capabilities.ColorTemperatureK, 1700.f, 12000.f);
		Data.bHasColorTemperatureControl = Capabilities.bHasColorTemperatureControl;
		Data.ColorTemperatureRangeK = FVector2D(FMath::Clamp(FMath::Min(Capabilities.ColorTemperatureRangeK.X, Capabilities.ColorTemperatureRangeK.Y), 1700.0, 12000.0),
			FMath::Clamp(FMath::Max(Capabilities.ColorTemperatureRangeK.X, Capabilities.ColorTemperatureRangeK.Y), 1700.0, 12000.0));
		Data.bHasShutter = Capabilities.bHasShutter;
		const double StrobeLow = FMath::Clamp(Capabilities.StrobeRateHz.X, 0.1, 50.0);
		Data.StrobeRateHz = FVector2D(StrobeLow, FMath::Clamp(Capabilities.StrobeRateHz.Y, StrobeLow, 50.0));

		Data.Emitters.Reset();
		for (const FLinearColor& Color : Capabilities.Emitters)
		{
			FStageColorEmitter& Emitter = Data.Emitters.AddDefaulted_GetRef();
			Emitter.Color = Color;
		}
		Data.bHasCTO = Capabilities.bHasCTO;
		Data.ColorWheel.Reset();
		for (const FStageGdtfSlotOut& Slot : Capabilities.ColorWheel)
		{
			FStageColorWheelSlot& Filter = Data.ColorWheel.AddDefaulted_GetRef();
			Filter.Name = FText::FromString(Slot.Name);
			Filter.Filter = Slot.Filter;
		}
		Data.bAcceptsGel = Capabilities.bAcceptsGel;

		Data.LuminousFlux = FMath::Max(Capabilities.LuminousFlux, 0.f);
		Data.BeamAngleMin = FMath::Clamp(FMath::Min(Capabilities.BeamAngleMin, Capabilities.BeamAngleMax), 0.5f, 170.f);
		Data.BeamAngleMax = FMath::Clamp(FMath::Max(Capabilities.BeamAngleMin, Capabilities.BeamAngleMax), 0.5f, 170.f);
		Data.FieldAngleRatio = FMath::Clamp(Capabilities.FieldAngleRatio, 1.f, 3.f);
		Data.GoboWheel = MakeGoboSlots(Capabilities.GoboWheel);
		Data.bGoboSpin = Capabilities.bGoboSpin;
		Data.StaticGoboWheel = MakeGoboSlots(Capabilities.StaticGoboWheel);
		Data.Prisms.Reset();
		for (const FStageGdtfSlotOut& Slot : Capabilities.Prisms)
		{
			FStagePrism& Prism = Data.Prisms.AddDefaulted_GetRef();
			Prism.Name = FText::FromString(Slot.Name);
			Prism.Facets = FMath::Clamp(Slot.Facets, 2, 16);
		}
		Data.MaxRotationRpm = FMath::Clamp(Capabilities.MaxRotationRpm, 1.f, 300.f);
		Data.bHasFrost = Capabilities.bHasFrost;
		Data.bHasIris = Capabilities.bHasIris;
		Data.BeamShaper = Capabilities.BeamShaper;

		Data.CellCount = FMath::Clamp(Capabilities.CellCount, 1, StageFixtureCells::MaxCells);
		Data.CellControl = Capabilities.CellControl;

		Data.bHasPanTilt = Capabilities.bHasPanTilt;
		Data.PanLimits = FVector2D(FMath::Min(Capabilities.PanLimits.X, Capabilities.PanLimits.Y), FMath::Max(Capabilities.PanLimits.X, Capabilities.PanLimits.Y));
		Data.TiltLimits = FVector2D(FMath::Min(Capabilities.TiltLimits.X, Capabilities.TiltLimits.Y), FMath::Max(Capabilities.TiltLimits.X, Capabilities.TiltLimits.Y));

		const FBodyLayout Body = MakeBodyLayout(Capabilities, FixtureKind);
		Data.Mesh = BaseMesh;
		Data.YokeMesh = Data.bHasPanTilt ? TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(CubePath)) : TSoftObjectPtr<UStaticMesh>();
		Data.HeadMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(Body.bBoxHead ? CubePath : CylinderPath));
		Data.PanPivotOffset = Body.PanPivotOffset;
		Data.TiltPivotOffset = Body.TiltPivotOffset;
		Data.BeamOriginOffset = Body.BeamOriginOffset;
		Data.Body.BaseSizeCm = Body.BaseSizeCm;
		Data.Body.YokeSizeCm = Body.YokeSizeCm;
		Data.Body.HeadSizeCm = Body.HeadSizeCm;
		Data.Body.LensDiameterCm = Body.LensDiameterCm;
		Data.Body.CellStripSizeCm = Body.CellStripSizeCm;

		Data.DMXModeName = FName(*Layout.Name.Left(StageFixtureLibrary::MaxRefTextLength));
		Data.DMXChannels = MakeDMXChannels(Layout);

		Data.FixtureTypeId = Type.FixtureTypeId;
		Data.SourceFormat = StageFixtureLibrary::GdtfFormat;
		Data.SourceTypeName = Type.Name.Left(StageFixtureLibrary::MaxRefTextLength);
	}

	UStaticMesh* BuildBaseMesh(UObject& Outer, FName Name, const FVector& SizeCm)
	{
		if (!IsPositive(SizeCm))
		{
			return nullptr;
		}
		const FMeshDescription Description = MakeBoxDescription(FVector3f(ClampPart(SizeCm) * 0.5));

		UStaticMesh* Mesh = NewObject<UStaticMesh>(&Outer, Name, RF_Transient);
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, BasicShapeMaterialPath);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface), MaterialSlot));
		Mesh->CreateBodySetup();
		Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseSimpleAsComplex;

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = true;
		Params.bAllowCpuAccess = false;
		if (!Mesh->BuildFromMeshDescriptions({ &Description }, Params))
		{
			return nullptr;
		}
		return Mesh;
	}
}
