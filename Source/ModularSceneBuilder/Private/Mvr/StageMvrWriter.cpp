// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrWriter.h"

#include "Fixtures/StageZipWriter.h"
#include "Mvr/StageMvrGeometry.h"
#include "Misc/Paths.h"
#include "Mvr/StageMvrParser.h"

namespace StageMvrExport
{
	const TCHAR* const SceneFileName = TEXT("GeneralSceneDescription.xml");

	namespace
	{
		constexpr double MmPerCm = 10.0;
		constexpr int32 AddressesPerUniverse = 512;

		FString FormatNumber(double Value)
		{
			FString Text = FString::Printf(TEXT("%.6f"), FMath::IsNearlyZero(Value, 5e-7) ? 0.0 : Value);
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			if (Text.EndsWith(TEXT(".")))
			{
				Text.LeftChopInline(1);
			}
			return Text;
		}

		FString FormatUuid(const FGuid& Id)
		{
			return Id.ToString(EGuidFormats::DigitsWithHyphens);
		}

		FBox ToMvrBox(const FBox& LocalBoundsCm, const FVector& Scale)
		{
			const FVector A = LocalBoundsCm.Min * Scale * MmPerCm;
			const FVector B = LocalBoundsCm.Max * Scale * MmPerCm;
			const FVector Min = A.ComponentMin(B);
			const FVector Max = A.ComponentMax(B);
			return FBox(FVector(Min.X, -Max.Y, Min.Z), FVector(Max.X, -Min.Y, Max.Z));
		}

		FString BoxKey(const FBox& Box)
		{
			const auto Round = [](double Value) { return FMath::RoundToInt64(Value * 10.0); };
			return FString::Printf(TEXT("%lld_%lld_%lld_%lld_%lld_%lld"), Round(Box.Min.X), Round(Box.Min.Y), Round(Box.Min.Z), Round(Box.Max.X), Round(Box.Max.Y), Round(Box.Max.Z));
		}

		class FXml
		{
		public:
			void Open(const FString& Element, const FString& Attributes = FString())
			{
				Line(FString::Printf(TEXT("<%s%s>"), *Element, *Attributes));
				++Depth;
			}

			void Close(const FString& Element)
			{
				--Depth;
				Line(FString::Printf(TEXT("</%s>"), *Element));
			}

			void Leaf(const FString& Element, const FString& Text, const FString& Attributes = FString())
			{
				Line(FString::Printf(TEXT("<%s%s>%s</%s>"), *Element, *Attributes, *EscapeXml(Text), *Element));
			}

			void Empty(const FString& Element, const FString& Attributes)
			{
				Line(FString::Printf(TEXT("<%s%s/>"), *Element, *Attributes));
			}

			void Line(const FString& Text)
			{
				for (int32 Level = 0; Level < Depth; ++Level)
				{
					Out.Append(TEXT("  "));
				}
				Out.Append(Text);
				Out.AppendChar(TEXT('\n'));
			}

			FString Out;

		private:
			int32 Depth = 0;
		};

		FString Attribute(const TCHAR* Name, const FString& Value)
		{
			return FString::Printf(TEXT(" %s=\"%s\""), Name, *EscapeXml(Value));
		}

		FString ObjectAttributes(const FString& Name, const FGuid& Id)
		{
			return Attribute(TEXT("name"), Name) + Attribute(TEXT("uuid"), FormatUuid(Id));
		}

		bool CheckScene(const FScene& Scene, FString& OutError)
		{
			if (Scene.Fixtures.IsEmpty() && Scene.Objects.IsEmpty())
			{
				OutError = TEXT("There is nothing on the stage to export.");
				return false;
			}
			TSet<FGuid> Ids;
			const auto Claim = [&Ids, &OutError](const FGuid& Id, const FString& Name)
			{
				bool bRepeated = false;
				Ids.Add(Id, &bRepeated);
				if (!Id.IsValid() || bRepeated)
				{
					OutError = FString::Printf(TEXT("\"%s\" has no unique id."), *Name);
					return false;
				}
				return true;
			};
			for (const FLayer& Layer : Scene.Layers)
			{
				if (!Claim(Layer.Id, Layer.Name))
				{
					return false;
				}
			}
			for (const FFixture& Fixture : Scene.Fixtures)
			{
				if (!Claim(Fixture.Id, Fixture.Name) || !Scene.Layers.IsValidIndex(Fixture.Layer))
				{
					OutError = OutError.IsEmpty() ? FString::Printf(TEXT("\"%s\" is on no layer."), *Fixture.Name) : OutError;
					return false;
				}
			}
			for (const FBoxObject& Object : Scene.Objects)
			{
				if (!Claim(Object.Id, Object.Name) || !Scene.Layers.IsValidIndex(Object.Layer) || !Object.LocalBounds.IsValid)
				{
					OutError = OutError.IsEmpty() ? FString::Printf(TEXT("\"%s\" is on no layer or has no size."), *Object.Name) : OutError;
					return false;
				}
			}
			return true;
		}
	}

	FString EscapeXml(FStringView Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		for (const TCHAR Character : Text)
		{
			switch (Character)
			{
			case TEXT('&'): Out.Append(TEXT("&amp;")); break;
			case TEXT('<'): Out.Append(TEXT("&lt;")); break;
			case TEXT('>'): Out.Append(TEXT("&gt;")); break;
			case TEXT('"'): Out.Append(TEXT("&quot;")); break;
			case TEXT('\''): Out.Append(TEXT("&apos;")); break;
			default:
				if (Character >= 0x20 ? (Character != 0xFFFE && Character != 0xFFFF) : (Character == TEXT('\t') || Character == TEXT('\n') || Character == TEXT('\r')))
				{
					Out.AppendChar(Character);
				}
				break;
			}
		}
		return Out;
	}

	FString FormatMatrix(const FMatrix& Matrix)
	{
		FString Out;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			Out += FString::Printf(TEXT("{%s,%s,%s}"), *FormatNumber(Matrix.M[Row][0]), *FormatNumber(Matrix.M[Row][1]), *FormatNumber(Matrix.M[Row][2]));
		}
		return Out;
	}

	FMatrix FixtureMatrix(const FTransform& ActorTransform)
	{
		const FTransform Unscaled(ActorTransform.GetRotation().GetNormalized(), ActorTransform.GetLocation());
		return StageMvrUnits::FromUnreal(FTransform(FRotator(0.0, 0.0, 180.0)) * Unscaled);
	}

	FString MakeGdtfFileName(const FString& Preferred, const FString& Fallback)
	{
		FString Base = FPaths::GetBaseFilename(Preferred);
		FString Safe;
		for (const TCHAR Character : Base)
		{
			const bool bAllowed = FChar::IsAlnum(Character) || Character == TEXT('@') || Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(' ') || Character == TEXT('(') || Character == TEXT(')');
			if (bAllowed && Character < 0x80)
			{
				Safe.AppendChar(Character);
			}
		}
		Safe.TrimStartAndEndInline();
		while (Safe.StartsWith(TEXT(".")))
		{
			Safe.RightChopInline(1);
		}
		Safe.LeftInline(96);
		return (Safe.IsEmpty() ? Fallback : Safe) + TEXT(".gdtf");
	}

	FWriteResult Write(const FScene& Scene, const TMap<FString, TArray<uint8>>& GdtfFiles)
	{
		FWriteResult Result;
		if (!CheckScene(Scene, Result.Error))
		{
			return Result;
		}

		TMap<FString, const TArray<uint8>*> Gdtf;
		for (const TPair<FString, TArray<uint8>>& File : GdtfFiles)
		{
			Gdtf.Add(File.Key.ToLower(), &File.Value);
		}

		TArray<FString> GeometryNames;
		TArray<FBox> GeometryBoxes;
		TMap<FString, int32> GeometryByKey;
		TArray<int32> ObjectGeometry;
		ObjectGeometry.Reserve(Scene.Objects.Num());
		for (const FBoxObject& Object : Scene.Objects)
		{
			const FBox Box = ToMvrBox(Object.LocalBounds, Object.Transform.GetScale3D());
			const FString Key = BoxKey(Box);
			int32* Known = GeometryByKey.Find(Key);
			if (!Known)
			{
				const int32 Index = GeometryBoxes.Add(Box);
				GeometryNames.Add(FString::Printf(TEXT("%s_%03d.glb"), Object.Kind == EBoxKind::Truss ? TEXT("truss") : TEXT("object"), Index + 1));
				Known = &GeometryByKey.Add(Key, Index);
			}
			ObjectGeometry.Add(*Known);
		}

		TArray<FString> WrittenGdtf;
		FXml Xml;
		Xml.Line(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>"));
		Xml.Open(TEXT("GeneralSceneDescription"), FString::Printf(TEXT(" verMajor=\"%d\" verMinor=\"%d\""), VerMajor, VerMinor)
			+ Attribute(TEXT("provider"), Scene.Provider) + Attribute(TEXT("providerVersion"), Scene.ProviderVersion));
		Xml.Open(TEXT("Scene"));
		Xml.Open(TEXT("Layers"));
		for (int32 LayerIndex = 0; LayerIndex < Scene.Layers.Num(); ++LayerIndex)
		{
			const FLayer& Layer = Scene.Layers[LayerIndex];
			Xml.Open(TEXT("Layer"), ObjectAttributes(Layer.Name, Layer.Id));
			Xml.Open(TEXT("ChildList"));
			for (int32 Index = 0; Index < Scene.Objects.Num(); ++Index)
			{
				const FBoxObject& Object = Scene.Objects[Index];
				if (Object.Layer != LayerIndex)
				{
					continue;
				}
				const TCHAR* Element = Object.Kind == EBoxKind::Truss ? TEXT("Truss") : TEXT("SceneObject");
				const FTransform Unscaled(Object.Transform.GetRotation().GetNormalized(), Object.Transform.GetLocation());
				Xml.Open(Element, ObjectAttributes(Object.Name, Object.Id));
				Xml.Leaf(TEXT("Matrix"), FormatMatrix(StageMvrUnits::FromUnreal(Unscaled)));
				Xml.Open(TEXT("Geometries"));
				Xml.Empty(TEXT("Geometry3D"), Attribute(TEXT("fileName"), GeometryNames[ObjectGeometry[Index]]));
				Xml.Close(TEXT("Geometries"));
				Xml.Close(Element);
			}
			for (const FFixture& Fixture : Scene.Fixtures)
			{
				if (Fixture.Layer != LayerIndex)
				{
					continue;
				}
				Xml.Open(TEXT("Fixture"), ObjectAttributes(Fixture.Name, Fixture.Id));
				Xml.Leaf(TEXT("Matrix"), FormatMatrix(FixtureMatrix(Fixture.Transform)));
				if (!Fixture.GdtfSpec.IsEmpty())
				{
					if (Gdtf.Contains(Fixture.GdtfSpec.ToLower()))
					{
						Xml.Leaf(TEXT("GDTFSpec"), Fixture.GdtfSpec);
						Xml.Leaf(TEXT("GDTFMode"), Fixture.GdtfMode);
						WrittenGdtf.AddUnique(Fixture.GdtfSpec.ToLower());
					}
					else
					{
						Result.Notes.Add(FString::Printf(TEXT("%s: its GDTF file %s is not in the fixture library; written without a fixture type."), *Fixture.Name, *Fixture.GdtfSpec));
					}
				}
				if (Fixture.Universe > 0 && Fixture.Address > 0)
				{
					Xml.Open(TEXT("Addresses"));
					Xml.Leaf(TEXT("Address"), FString::FromInt((Fixture.Universe - 1) * AddressesPerUniverse + Fixture.Address), TEXT(" break=\"0\""));
					Xml.Close(TEXT("Addresses"));
				}
				Xml.Leaf(TEXT("FixtureID"), Fixture.FixtureId > 0 ? FString::FromInt(Fixture.FixtureId) : FString());
				if (Fixture.FixtureId > 0)
				{
					Xml.Leaf(TEXT("FixtureIDNumeric"), FString::FromInt(Fixture.FixtureId));
				}
				Xml.Leaf(TEXT("UnitNumber"), TEXT("0"));
				Xml.Close(TEXT("Fixture"));
			}
			Xml.Close(TEXT("ChildList"));
			Xml.Close(TEXT("Layer"));
		}
		Xml.Close(TEXT("Layers"));
		Xml.Close(TEXT("Scene"));
		Xml.Close(TEXT("GeneralSceneDescription"));

		StageZip::FWriter Zip;
		const FTCHARToUTF8 SceneUtf8(*Xml.Out);
		if (!Zip.AddFile(SceneFileName, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(SceneUtf8.Get()), SceneUtf8.Length()), StageZip::EStore::Deflate, Result.Error))
		{
			return Result;
		}
		for (const FString& Spec : WrittenGdtf)
		{
			const FString* Name = nullptr;
			for (const FFixture& Fixture : Scene.Fixtures)
			{
				if (Fixture.GdtfSpec.ToLower() == Spec)
				{
					Name = &Fixture.GdtfSpec;
					break;
				}
			}
			if (!Zip.AddFile(*Name, *Gdtf[Spec], StageZip::EStore::Store, Result.Error))
			{
				return Result;
			}
			++Result.GdtfFiles;
		}
		for (int32 Index = 0; Index < GeometryBoxes.Num(); ++Index)
		{
			if (!Zip.AddFile(GeometryNames[Index], StageMvrGeometry::WriteBoxGlb(GeometryBoxes[Index], FPaths::GetBaseFilename(GeometryNames[Index])), StageZip::EStore::Deflate, Result.Error))
			{
				return Result;
			}
			++Result.GeometryFiles;
		}
		Result.Bytes = Zip.Finish();
		return Result;
	}
}
