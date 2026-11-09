// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfParser.h"

const FStageGdtfWheel* FStageGdtfFixtureType::FindWheel(FStringView WheelName) const
{
	return Wheels.FindByPredicate([WheelName](const FStageGdtfWheel& Wheel) { return FStringView(Wheel.Name).Equals(WheelName, ESearchCase::CaseSensitive); });
}

const FStageGdtfEmitter* FStageGdtfFixtureType::FindEmitter(FStringView EmitterName) const
{
	return Emitters.FindByPredicate([EmitterName](const FStageGdtfEmitter& Emitter) { return FStringView(Emitter.Name).Equals(EmitterName, ESearchCase::CaseSensitive); });
}

const FStageGdtfModel* FStageGdtfFixtureType::FindModel(FStringView ModelName) const
{
	return Models.FindByPredicate([ModelName](const FStageGdtfModel& Model) { return FStringView(Model.Name).Equals(ModelName, ESearchCase::CaseSensitive); });
}

int32 FStageGdtfFixtureType::FindGeometry(FStringView GeometryName) const
{
	return Geometries.IndexOfByPredicate([GeometryName](const FStageGdtfGeometry& Geometry) { return FStringView(Geometry.Name).Equals(GeometryName, ESearchCase::CaseSensitive); });
}

bool FStageGdtfFixtureType::IsInSubtree(int32 Geometry, int32 Ancestor) const
{
	for (int32 Current = Geometry, Steps = 0; Geometries.IsValidIndex(Current) && Steps <= Geometries.Num(); Current = Geometries[Current].Parent, ++Steps)
	{
		if (Current == Ancestor)
		{
			return true;
		}
	}
	return false;
}

namespace StageGdtf
{
	namespace
	{
		bool ParseInt(FStringView Text, int64& Out)
		{
			Text = Text.TrimStartAndEnd();
			if (Text.IsEmpty() || Text.Len() > 19)
			{
				return false;
			}
			int64 Value = 0;
			bool bNegative = false;
			int32 Index = 0;
			if (Text[0] == TEXT('-') || Text[0] == TEXT('+'))
			{
				bNegative = Text[0] == TEXT('-');
				Index = 1;
			}
			if (Index >= Text.Len())
			{
				return false;
			}
			for (; Index < Text.Len(); ++Index)
			{
				if (!FChar::IsDigit(Text[Index]) || Value > (MAX_int64 - 9) / 10)
				{
					return false;
				}
				Value = Value * 10 + (Text[Index] - TEXT('0'));
			}
			Out = bNegative ? -Value : Value;
			return true;
		}

		bool ParseDouble(FStringView Text, double& Out)
		{
			Text = Text.TrimStartAndEnd();
			if (Text.IsEmpty() || Text.Len() > 64)
			{
				return false;
			}
			// Digits, one sign, one point and an exponent only: the engine's Atod would read "12abc" as 12.
			for (const TCHAR Char : Text)
			{
				if (!FChar::IsDigit(Char) && Char != TEXT('-') && Char != TEXT('+') && Char != TEXT('.') && Char != TEXT('e') && Char != TEXT('E'))
				{
					return false;
				}
			}
			const FString Copy(Text);
			Out = FCString::Atod(*Copy);
			return FMath::IsFinite(Out) && (FChar::IsDigit(Text[0]) || Text.Len() > 1);
		}

		/** "a,b,c" into exactly Count numbers. */
		bool ParseNumberList(FStringView Text, int32 Count, double* Out)
		{
			int32 Found = 0;
			int32 Start = 0;
			for (int32 Index = 0; Index <= Text.Len(); ++Index)
			{
				if (Index == Text.Len() || Text[Index] == TEXT(','))
				{
					if (Found >= Count || !ParseDouble(Text.Mid(Start, Index - Start), Out[Found]))
					{
						return false;
					}
					++Found;
					Start = Index + 1;
				}
			}
			return Found == Count;
		}

		/** The structural reader of one description.xml. */
		class FReader
		{
		public:
			FReader(const StageXml::FDocument& InDocument, const FLimits& InLimits, FStageGdtfParseResult& InResult)
				: Doc(InDocument), Limits(InLimits), Result(InResult), Type(InResult.Type)
			{
			}

			void Run()
			{
				const StageXml::FNode& Root = Doc.Nodes[0];
				if (!Root.Name.Equals(TEXT("GDTF"), ESearchCase::CaseSensitive))
				{
					Error(0, TEXT("the root element is not <GDTF>"));
					return;
				}
				Type.DataVersion = GetString(0, TEXT("DataVersion"));
				if (Type.DataVersion.IsEmpty())
				{
					Warning(0, TEXT("DataVersion is missing"));
				}
				const int32 FixtureType = Doc.FindChild(0, TEXT("FixtureType"));
				if (FixtureType == INDEX_NONE)
				{
					Error(0, TEXT("there is no <FixtureType>"));
					return;
				}
				ReadFixtureType(FixtureType);
				ReadAttributeDefinitions(Doc.FindChild(FixtureType, TEXT("AttributeDefinitions")));
				ReadWheels(Doc.FindChild(FixtureType, TEXT("Wheels")));
				ReadPhysicalDescriptions(Doc.FindChild(FixtureType, TEXT("PhysicalDescriptions")));
				ReadModels(Doc.FindChild(FixtureType, TEXT("Models")));
				ReadGeometries(Doc.FindChild(FixtureType, TEXT("Geometries")));
				ReadModes(Doc.FindChild(FixtureType, TEXT("DMXModes")));
			}

		private:
			void Error(int32 Node, const FString& Reason)
			{
				Result.Errors.Add(Describe(Node, Reason));
			}

			void Warning(int32 Node, const FString& Reason)
			{
				Result.Warnings.Add(Describe(Node, Reason));
			}

			FString Describe(int32 Node, const FString& Reason) const
			{
				return Doc.Nodes.IsValidIndex(Node)
					? FString::Printf(TEXT("%s (line %d): %s"), *Doc.GetPath(Node), Doc.Nodes[Node].Line, *Reason)
					: Reason;
			}

			/** An attribute's text, checked against MaxStringLength (an error, and empty, when longer). */
			FString GetString(int32 Node, const TCHAR* Name)
			{
				const FString* Value = Doc.Nodes[Node].FindAttribute(Name);
				if (!Value)
				{
					return FString();
				}
				if (Value->Len() > Limits.MaxStringLength && FCString::Strcmp(Name, TEXT("Description")) != 0)
				{
					Error(Node, FString::Printf(TEXT("%s is longer than %d characters"), Name, Limits.MaxStringLength));
					return FString();
				}
				return *Value;
			}

			/** A number attribute: Default when absent, an error when present but not a number. */
			double GetNumber(int32 Node, const TCHAR* Name, double Default)
			{
				const FString* Value = Doc.Nodes[Node].FindAttribute(Name);
				if (!Value || Value->IsEmpty())
				{
					return Default;
				}
				double Parsed = 0.0;
				if (!ParseDouble(*Value, Parsed))
				{
					Error(Node, FString::Printf(TEXT("%s \"%s\" is not a number"), Name, **Value));
					return Default;
				}
				return Parsed;
			}

			TOptional<float> GetOptionalNumber(int32 Node, const TCHAR* Name)
			{
				const FString* Value = Doc.Nodes[Node].FindAttribute(Name);
				if (!Value || Value->IsEmpty())
				{
					return TOptional<float>();
				}
				return float(GetNumber(Node, Name, 0.0));
			}

			int32 GetInt(int32 Node, const TCHAR* Name, int32 Default, int32 Min, int32 Max)
			{
				const FString* Value = Doc.Nodes[Node].FindAttribute(Name);
				if (!Value || Value->IsEmpty())
				{
					return Default;
				}
				int64 Parsed = 0;
				if (!ParseInt(*Value, Parsed) || Parsed < Min || Parsed > Max)
				{
					Error(Node, FString::Printf(TEXT("%s \"%s\" is not a whole number from %d to %d"), Name, **Value, Min, Max));
					return Default;
				}
				return int32(Parsed);
			}

			FStageGdtfColorCIE GetColor(int32 Node, const TCHAR* Name)
			{
				FStageGdtfColorCIE Color;
				const FString* Value = Doc.Nodes[Node].FindAttribute(Name);
				if (!Value || Value->IsEmpty())
				{
					return Color;
				}
				double Parts[3] = {};
				if (!ParseNumberList(*Value, 3, Parts) || Parts[0] < 0.0 || Parts[0] > 1.0 || Parts[1] <= 0.0 || Parts[1] > 1.0 || Parts[2] < 0.0)
				{
					Error(Node, FString::Printf(TEXT("%s \"%s\" is not a CIE x,y,Y colour"), Name, **Value));
					return Color;
				}
				Color.X = Parts[0];
				Color.Y = Parts[1];
				Color.Luminance = Parts[2];
				Color.bSet = true;
				return Color;
			}

			FStageGdtfDMXValue GetDMXValue(int32 Node, const TCHAR* Name)
			{
				FStageGdtfDMXValue Value;
				const FString* Text = Doc.Nodes[Node].FindAttribute(Name);
				if (!Text || Text->IsEmpty() || Text->Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					return Value;
				}
				if (!ParseDMXValue(*Text, Limits.MaxValueBytes, Value))
				{
					Error(Node, FString::Printf(TEXT("%s \"%s\" is not a DMX value (such as 128/1)"), Name, **Text));
				}
				return Value;
			}

			/** Children counted against a cap; an error (and only the first Max) when over. */
			TArray<int32> GetCapped(int32 Node, const TCHAR* Name, int32 Max, const TCHAR* What)
			{
				TArray<int32> Children = Doc.GetChildren(Node, Name);
				if (Children.Num() > Max)
				{
					Error(Node, FString::Printf(TEXT("%d %s; the limit is %d"), Children.Num(), What, Max));
					Children.SetNum(Max);
				}
				return Children;
			}

			void ReadFixtureType(int32 Node)
			{
				Type.Name = GetString(Node, TEXT("Name"));
				Type.ShortName = GetString(Node, TEXT("ShortName"));
				Type.LongName = GetString(Node, TEXT("LongName"));
				Type.Manufacturer = GetString(Node, TEXT("Manufacturer"));
				Type.Description = GetString(Node, TEXT("Description")).Left(2000);
				if (Type.Name.IsEmpty())
				{
					Error(Node, TEXT("Name is missing"));
				}
				const FString Id = GetString(Node, TEXT("FixtureTypeID"));
				if (!FGuid::Parse(Id, Type.FixtureTypeId) || !Type.FixtureTypeId.IsValid())
				{
					Type.FixtureTypeId.Invalidate();
					Error(Node, FString::Printf(TEXT("FixtureTypeID \"%s\" is not a valid GUID"), *Id));
				}
			}

			void ReadAttributeDefinitions(int32 Node)
			{
				const int32 Attributes = Doc.FindChild(Node, TEXT("Attributes"));
				for (const int32 Attribute : GetCapped(Attributes, TEXT("Attribute"), 2048, TEXT("attributes")))
				{
					const FString Name = GetString(Attribute, TEXT("Name"));
					if (!Name.IsEmpty())
					{
						Type.DefinedAttributes.Add(Name);
					}
				}
			}

			void ReadWheels(int32 Node)
			{
				for (const int32 WheelNode : GetCapped(Node, TEXT("Wheel"), Limits.MaxWheels, TEXT("wheels")))
				{
					FStageGdtfWheel& Wheel = Type.Wheels.AddDefaulted_GetRef();
					Wheel.Name = GetString(WheelNode, TEXT("Name"));
					for (const int32 SlotNode : GetCapped(WheelNode, TEXT("Slot"), Limits.MaxSlotsPerWheel, TEXT("slots")))
					{
						FStageGdtfWheelSlot& Slot = Wheel.Slots.AddDefaulted_GetRef();
						Slot.Name = GetString(SlotNode, TEXT("Name"));
						Slot.Color = GetColor(SlotNode, TEXT("Color"));
						Slot.MediaFileName = GetString(SlotNode, TEXT("MediaFileName"));
						Slot.PrismFacets = Doc.GetChildren(SlotNode, TEXT("PrismFacet")).Num();
					}
				}
			}

			void ReadPhysicalDescriptions(int32 Node)
			{
				const int32 Emitters = Doc.FindChild(Node, TEXT("Emitters"));
				for (const int32 EmitterNode : GetCapped(Emitters, TEXT("Emitter"), Limits.MaxEmitters, TEXT("emitters")))
				{
					FStageGdtfEmitter& Emitter = Type.Emitters.AddDefaulted_GetRef();
					Emitter.Name = GetString(EmitterNode, TEXT("Name"));
					Emitter.Color = GetColor(EmitterNode, TEXT("Color"));
					Emitter.DominantWaveLengthNm = float(GetNumber(EmitterNode, TEXT("DominantWaveLength"), 0.0));
				}
				const int32 Properties = Doc.FindChild(Node, TEXT("Properties"));
				if (Properties == INDEX_NONE)
				{
					return;
				}
				if (const int32 Weight = Doc.FindChild(Properties, TEXT("Weight")); Weight != INDEX_NONE)
				{
					Type.Properties.WeightKg = float(FMath::Clamp(GetNumber(Weight, TEXT("Value"), 0.0), 0.0, 10000.0));
				}
				if (const int32 LegHeight = Doc.FindChild(Properties, TEXT("LegHeight")); LegHeight != INDEX_NONE)
				{
					Type.Properties.LegHeightM = float(FMath::Clamp(GetNumber(LegHeight, TEXT("Value"), 0.0), 0.0, 100.0));
				}
				for (const int32 Power : GetCapped(Properties, TEXT("PowerConsumption"), 64, TEXT("power connectors")))
				{
					Type.Properties.PowerW += float(FMath::Clamp(GetNumber(Power, TEXT("Value"), 0.0), 0.0, 1000000.0));
				}
			}

			void ReadModels(int32 Node)
			{
				static const TCHAR* Primitives[] = {
					TEXT("Undefined"), TEXT("Cube"), TEXT("Cylinder"), TEXT("Sphere"), TEXT("Base"), TEXT("Yoke"), TEXT("Head"), TEXT("Scanner"),
					TEXT("Conventional"), TEXT("Pigtail"), TEXT("Base1_1"), TEXT("Scanner1_1"), TEXT("Conventional1_1") };
				for (const int32 ModelNode : GetCapped(Node, TEXT("Model"), Limits.MaxModels, TEXT("models")))
				{
					FStageGdtfModel& Model = Type.Models.AddDefaulted_GetRef();
					Model.Name = GetString(ModelNode, TEXT("Name"));
					Model.SizeM = FVector(GetNumber(ModelNode, TEXT("Length"), 0.0), GetNumber(ModelNode, TEXT("Width"), 0.0), GetNumber(ModelNode, TEXT("Height"), 0.0));
					if (Model.SizeM.GetMin() < 0.0 || Model.SizeM.GetMax() > 100.0)
					{
						Error(ModelNode, TEXT("model sizes must be 0 to 100 metres"));
						Model.SizeM = FVector::ZeroVector;
					}
					Model.File = GetString(ModelNode, TEXT("File"));
					const FString Primitive = GetString(ModelNode, TEXT("PrimitiveType"));
					for (int32 Index = 0; Index < UE_ARRAY_COUNT(Primitives); ++Index)
					{
						if (Primitive.Equals(Primitives[Index], ESearchCase::CaseSensitive))
						{
							Model.Primitive = EStageGdtfPrimitive(Index);
						}
					}
				}
			}

			bool ParseGeometryType(const FString& Name, EStageGdtfGeometryType& Out) const
			{
				static const TCHAR* Names[] = {
					TEXT("Geometry"), TEXT("Axis"), TEXT("FilterBeam"), TEXT("FilterColor"), TEXT("FilterGobo"), TEXT("FilterShaper"), TEXT("Beam"),
					TEXT("MediaServerLayer"), TEXT("MediaServerCamera"), TEXT("MediaServerMaster"), TEXT("Display"), TEXT("GeometryReference"),
					TEXT("Laser"), TEXT("WiringObject"), TEXT("Inventory"), TEXT("Structure"), TEXT("Support"), TEXT("Magnet") };
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
				{
					if (Name.Equals(Names[Index], ESearchCase::CaseSensitive))
					{
						Out = EStageGdtfGeometryType(Index);
						return true;
					}
				}
				return false;
			}

			void ReadPosition(int32 Node, FStageGdtfGeometry& Geometry)
			{
				const FString Text = GetString(Node, TEXT("Position"));
				if (Text.IsEmpty())
				{
					return;
				}
				// "{a,b,c,d}{e,f,g,h}{i,j,k,l}{m,n,o,p}": four rows.
				TArray<FString> Rows;
				Text.Replace(TEXT("}"), TEXT("")).ParseIntoArray(Rows, TEXT("{"), /*bCullEmpty*/ true);
				double Values[16] = {};
				bool bOk = Rows.Num() == 4;
				for (int32 Row = 0; bOk && Row < 4; ++Row)
				{
					bOk = ParseNumberList(Rows[Row].TrimStartAndEnd(), 4, &Values[Row * 4]);
				}
				for (int32 Index = 0; bOk && Index < 16; ++Index)
				{
					bOk = FMath::Abs(Values[Index]) <= 1000.0;
				}
				if (!bOk)
				{
					Error(Node, FString::Printf(TEXT("Position \"%s\" is not a 4x4 matrix"), *Text.Left(80)));
					return;
				}
				FMemory::Memcpy(Geometry.Position, Values, sizeof(Values));
			}

			void ReadBeam(int32 Node, FStageGdtfBeam& Beam)
			{
				static const TCHAR* BeamTypes[] = { TEXT("Wash"), TEXT("Spot"), TEXT("None"), TEXT("Rectangle"), TEXT("PC"), TEXT("Fresnel"), TEXT("Glow") };
				const FString LampType = GetString(Node, TEXT("LampType"));
				Beam.LampType = LampType.IsEmpty() ? Beam.LampType : LampType;
				Beam.PowerConsumptionW = float(GetNumber(Node, TEXT("PowerConsumption"), Beam.PowerConsumptionW));
				Beam.LuminousFlux = float(GetNumber(Node, TEXT("LuminousFlux"), Beam.LuminousFlux));
				Beam.ColorTemperatureK = float(GetNumber(Node, TEXT("ColorTemperature"), Beam.ColorTemperatureK));
				Beam.BeamAngleDeg = float(GetNumber(Node, TEXT("BeamAngle"), Beam.BeamAngleDeg));
				Beam.FieldAngleDeg = float(GetNumber(Node, TEXT("FieldAngle"), Beam.FieldAngleDeg));
				Beam.BeamRadiusM = float(GetNumber(Node, TEXT("BeamRadius"), Beam.BeamRadiusM));
				const FString BeamType = GetString(Node, TEXT("BeamType"));
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(BeamTypes); ++Index)
				{
					if (BeamType.Equals(BeamTypes[Index], ESearchCase::CaseSensitive))
					{
						Beam.BeamType = EStageGdtfBeamType(Index);
					}
				}
				const bool bValid = Beam.PowerConsumptionW >= 0.f && Beam.LuminousFlux >= 0.f && Beam.ColorTemperatureK >= 0.f && Beam.ColorTemperatureK <= 100000.f
					&& Beam.BeamAngleDeg >= 0.f && Beam.BeamAngleDeg <= 360.f && Beam.FieldAngleDeg >= 0.f && Beam.FieldAngleDeg <= 360.f
					&& Beam.BeamRadiusM >= 0.f && Beam.BeamRadiusM <= 10.f;
				if (!bValid)
				{
					Error(Node, TEXT("a beam value is out of its range"));
					Beam = FStageGdtfBeam();
				}
			}

			void ReadGeometries(int32 Node)
			{
				if (Node == INDEX_NONE)
				{
					Error(INDEX_NONE, TEXT("FixtureType has no <Geometries>"));
					return;
				}
				struct FPending { int32 XmlNode; int32 Parent; int32 Depth; };
				TArray<FPending> Stack;
				const TArray<int32>& TopLevel = Doc.Nodes[Node].Children;
				for (int32 Index = TopLevel.Num() - 1; Index >= 0; --Index)
				{
					Stack.Add({ TopLevel[Index], INDEX_NONE, 1 });
				}
				TSet<FString> Names;
				while (!Stack.IsEmpty())
				{
					const FPending Pending = Stack.Pop(EAllowShrinking::No);
					EStageGdtfGeometryType GeometryType;
					if (!ParseGeometryType(Doc.Nodes[Pending.XmlNode].Name, GeometryType))
					{
						continue; // Not a geometry element (for example a Break): read by its parent.
					}
					if (Type.Geometries.Num() >= Limits.MaxGeometries || Pending.Depth > Limits.MaxGeometryDepth)
					{
						Error(Pending.XmlNode, FString::Printf(TEXT("more than %d geometries, or deeper than %d"), Limits.MaxGeometries, Limits.MaxGeometryDepth));
						return;
					}
					const int32 Index = Type.Geometries.Num();
					FStageGdtfGeometry& Geometry = Type.Geometries.AddDefaulted_GetRef();
					Geometry.Name = GetString(Pending.XmlNode, TEXT("Name"));
					Geometry.Type = GeometryType;
					Geometry.Model = GetString(Pending.XmlNode, TEXT("Model"));
					Geometry.Parent = Pending.Parent;
					ReadPosition(Pending.XmlNode, Type.Geometries[Index]);
					if (Geometry.Name.IsEmpty())
					{
						Error(Pending.XmlNode, TEXT("a geometry has no Name"));
					}
					else if (Names.Contains(Geometry.Name))
					{
						Warning(Pending.XmlNode, FString::Printf(TEXT("geometry name \"%s\" is used twice; the first is used"), *Geometry.Name));
					}
					Names.Add(Geometry.Name);
					if (GeometryType == EStageGdtfGeometryType::Beam)
					{
						ReadBeam(Pending.XmlNode, Type.Geometries[Index].Beam);
					}
					if (GeometryType == EStageGdtfGeometryType::GeometryReference)
					{
						Type.Geometries[Index].ReferencedGeometry = GetString(Pending.XmlNode, TEXT("Geometry"));
						for (const int32 Break : Doc.GetChildren(Pending.XmlNode, TEXT("Break")))
						{
							Type.Geometries[Index].Breaks.Add({ GetInt(Break, TEXT("DMXOffset"), 1, 1, 512), GetInt(Break, TEXT("DMXBreak"), 1, 1, 64) });
						}
					}
					if (Pending.Parent != INDEX_NONE)
					{
						Type.Geometries[Pending.Parent].Children.Add(Index);
					}
					const TArray<int32>& Children = Doc.Nodes[Pending.XmlNode].Children;
					for (int32 Child = Children.Num() - 1; Child >= 0; --Child)
					{
						Stack.Add({ Children[Child], Index, Pending.Depth + 1 });
					}
				}
				for (const FStageGdtfGeometry& Geometry : Type.Geometries)
				{
					if (Geometry.Type == EStageGdtfGeometryType::GeometryReference && Type.FindGeometry(Geometry.ReferencedGeometry) == INDEX_NONE)
					{
						Result.Errors.Add(FString::Printf(TEXT("GeometryReference \"%s\" points at missing geometry \"%s\""), *Geometry.Name, *Geometry.ReferencedGeometry));
					}
				}
			}

			void ReadOffsets(int32 Node, FStageGdtfDMXChannel& Channel)
			{
				const FString Text = GetString(Node, TEXT("Offset"));
				if (Text.IsEmpty() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					return; // Virtual channel.
				}
				TArray<FString> Parts;
				Text.ParseIntoArray(Parts, TEXT(","), /*bCullEmpty*/ false);
				if (Parts.Num() > Limits.MaxValueBytes)
				{
					Error(Node, FString::Printf(TEXT("Offset \"%s\" has more than %d bytes"), *Text, Limits.MaxValueBytes));
					return;
				}
				for (const FString& Part : Parts)
				{
					int64 Offset = 0;
					if (!ParseInt(Part, Offset) || Offset < 1 || Offset > 512)
					{
						Error(Node, FString::Printf(TEXT("Offset \"%s\" is not a list of channels 1 to 512"), *Text));
						Channel.Offsets.Reset();
						return;
					}
					Channel.Offsets.Add(int32(Offset));
				}
			}

			void ReadChannelFunction(int32 Node, FStageGdtfChannelFunction& Function)
			{
				Function.Name = GetString(Node, TEXT("Name"));
				Function.Attribute = GetString(Node, TEXT("Attribute"));
				Function.OriginalAttribute = GetString(Node, TEXT("OriginalAttribute"));
				Function.DMXFrom = GetDMXValue(Node, TEXT("DMXFrom"));
				Function.Default = GetDMXValue(Node, TEXT("Default"));
				Function.PhysicalFrom = float(GetNumber(Node, TEXT("PhysicalFrom"), 0.0));
				Function.PhysicalTo = float(GetNumber(Node, TEXT("PhysicalTo"), 1.0));
				Function.Wheel = GetString(Node, TEXT("Wheel"));
				Function.Emitter = GetString(Node, TEXT("Emitter"));
				Function.ModeMaster = GetString(Node, TEXT("ModeMaster"));
				if (Function.Attribute.IsEmpty())
				{
					Function.Attribute = TEXT("NoFeature");
				}
				if (!Function.Wheel.IsEmpty() && !Type.FindWheel(Function.Wheel))
				{
					Error(Node, FString::Printf(TEXT("Wheel \"%s\" does not exist"), *Function.Wheel));
				}
				for (const int32 SetNode : GetCapped(Node, TEXT("ChannelSet"), Limits.MaxSetsPerFunction, TEXT("channel sets")))
				{
					FStageGdtfChannelSet& Set = Function.Sets.AddDefaulted_GetRef();
					Set.Name = GetString(SetNode, TEXT("Name"));
					Set.DMXFrom = GetDMXValue(SetNode, TEXT("DMXFrom"));
					Set.PhysicalFrom = GetOptionalNumber(SetNode, TEXT("PhysicalFrom"));
					Set.PhysicalTo = GetOptionalNumber(SetNode, TEXT("PhysicalTo"));
					Set.WheelSlotIndex = GetInt(SetNode, TEXT("WheelSlotIndex"), 0, 0, Limits.MaxSlotsPerWheel);
				}
			}

			void ReadModes(int32 Node)
			{
				const TArray<int32> ModeNodes = GetCapped(Node, TEXT("DMXMode"), Limits.MaxModes, TEXT("DMX modes"));
				if (ModeNodes.IsEmpty())
				{
					Error(Node, TEXT("the fixture has no DMX modes"));
					return;
				}
				TSet<FString> ModeNames;
				for (const int32 ModeNode : ModeNodes)
				{
					FStageGdtfMode& Mode = Type.Modes.AddDefaulted_GetRef();
					Mode.Name = GetString(ModeNode, TEXT("Name"));
					Mode.Description = GetString(ModeNode, TEXT("Description")).Left(Limits.MaxStringLength);
					Mode.Geometry = GetString(ModeNode, TEXT("Geometry"));
					if (Mode.Name.IsEmpty() || ModeNames.Contains(Mode.Name))
					{
						Error(ModeNode, FString::Printf(TEXT("mode name \"%s\" is empty or used twice"), *Mode.Name));
					}
					ModeNames.Add(Mode.Name);
					const int32 TopGeometry = Type.FindGeometry(Mode.Geometry);
					if (TopGeometry == INDEX_NONE || Type.Geometries[TopGeometry].Parent != INDEX_NONE)
					{
						Error(ModeNode, FString::Printf(TEXT("Geometry \"%s\" is not a top-level geometry"), *Mode.Geometry));
					}
					const int32 Channels = Doc.FindChild(ModeNode, TEXT("DMXChannels"));
					for (const int32 ChannelNode : GetCapped(Channels, TEXT("DMXChannel"), Limits.MaxChannelsPerMode, TEXT("channels")))
					{
						FStageGdtfDMXChannel& Channel = Mode.Channels.AddDefaulted_GetRef();
						Channel.Line = Doc.Nodes[ChannelNode].Line;
						const FString Break = GetString(ChannelNode, TEXT("DMXBreak"));
						Channel.bBreakOverwrite = Break.Equals(TEXT("Overwrite"), ESearchCase::IgnoreCase);
						Channel.DMXBreak = Channel.bBreakOverwrite ? 1 : GetInt(ChannelNode, TEXT("DMXBreak"), 1, 1, 64);
						ReadOffsets(ChannelNode, Channel);
						Channel.Geometry = GetString(ChannelNode, TEXT("Geometry"));
						Channel.InitialFunction = GetString(ChannelNode, TEXT("InitialFunction"));
						if (Type.FindGeometry(Channel.Geometry) == INDEX_NONE)
						{
							Error(ChannelNode, FString::Printf(TEXT("Geometry \"%s\" does not exist"), *Channel.Geometry));
						}
						int32 FunctionCount = 0;
						for (const int32 LogicalNode : GetCapped(ChannelNode, TEXT("LogicalChannel"), Limits.MaxLogicalChannelsPerChannel, TEXT("logical channels")))
						{
							FStageGdtfLogicalChannel& Logical = Channel.LogicalChannels.AddDefaulted_GetRef();
							Logical.Attribute = GetString(LogicalNode, TEXT("Attribute"));
							for (const int32 FunctionNode : GetCapped(LogicalNode, TEXT("ChannelFunction"), Limits.MaxFunctionsPerChannel, TEXT("channel functions")))
							{
								ReadChannelFunction(FunctionNode, Logical.Functions.AddDefaulted_GetRef());
								++FunctionCount;
							}
						}
						if (FunctionCount == 0)
						{
							Error(ChannelNode, TEXT("the channel has no channel functions"));
						}
						else if (FunctionCount > Limits.MaxFunctionsPerChannel)
						{
							Error(ChannelNode, FString::Printf(TEXT("%d channel functions; the limit is %d"), FunctionCount, Limits.MaxFunctionsPerChannel));
						}
					}
					if (Mode.Channels.IsEmpty())
					{
						Warning(ModeNode, TEXT("the mode has no channels"));
					}
				}
			}

			const StageXml::FDocument& Doc;
			const FLimits& Limits;
			FStageGdtfParseResult& Result;
			FStageGdtfFixtureType& Type;
		};
	}

	bool ParseDMXValue(FStringView Text, int32 MaxBytes, FStageGdtfDMXValue& OutValue)
	{
		Text = Text.TrimStartAndEnd();
		int32 Slash = INDEX_NONE;
		Text.FindChar(TEXT('/'), Slash);
		int64 Value = 0;
		int64 Bytes = 1;
		bool bShift = false;
		if (Slash == INDEX_NONE)
		{
			if (!ParseInt(Text, Value)) // A bare number: the GDTF 1.0 form, one byte.
			{
				return false;
			}
		}
		else
		{
			FStringView BytesText = Text.RightChop(Slash + 1);
			if (BytesText.EndsWith(TEXT("s"), ESearchCase::CaseSensitive))
			{
				bShift = true;
				BytesText.LeftChopInline(1);
			}
			if (!ParseInt(Text.Left(Slash), Value) || !ParseInt(BytesText, Bytes))
			{
				return false;
			}
		}
		if (Bytes < 1 || Bytes > MaxBytes || Value < 0 || Value >= (int64(1) << (8 * Bytes)))
		{
			return false;
		}
		OutValue.Value = Value;
		OutValue.Bytes = int32(Bytes);
		OutValue.bShift = bShift;
		OutValue.bSet = true;
		return true;
	}

	int64 ToChannelValue(const FStageGdtfDMXValue& Value, int32 ChannelBytes)
	{
		ChannelBytes = FMath::Clamp(ChannelBytes, 1, 4);
		if (Value.Bytes == ChannelBytes)
		{
			return Value.Value;
		}
		if (Value.Bytes > ChannelBytes)
		{
			return Value.Value >> (8 * (Value.Bytes - ChannelBytes));
		}
		if (Value.bShift)
		{
			return Value.Value << (8 * (ChannelBytes - Value.Bytes));
		}
		// Byte mirroring: repeat the value's bytes, so its maximum stays the channel's maximum (255/1 on 16 bits is 65535).
		int64 Result = 0;
		int32 Filled = 0;
		while (Filled < ChannelBytes)
		{
			const int32 Take = FMath::Min(Value.Bytes, ChannelBytes - Filled);
			Result = (Result << (8 * Take)) | (Value.Value >> (8 * (Value.Bytes - Take)));
			Filled += Take;
		}
		return Result;
	}

	FStageGdtfParseResult ParseDescription(FStringView Xml, const FLimits& Limits)
	{
		FStageGdtfParseResult Result;
		StageXml::FDocument Document;
		FString XmlError;
		if (!StageXml::Parse(Xml, Limits.Xml, Document, XmlError))
		{
			Result.Errors.Add(FString::Printf(TEXT("description.xml %s"), *XmlError));
			return Result;
		}
		FReader Reader(Document, Limits, Result);
		Reader.Run();
		return Result;
	}

	FStageGdtfParseResult Parse(TConstArrayView<uint8> FileBytes, const FLimits& Limits)
	{
		FStageGdtfParseResult Failed;
		StageZip::FDirectory Directory;
		FString ZipError;
		if (!StageZip::ReadDirectory(FileBytes, Limits.Zip, Directory, ZipError))
		{
			Failed.Errors.Add(ZipError);
			return Failed;
		}
		const StageZip::FEntry* Description = Directory.Find(TEXT("description.xml"));
		if (!Description)
		{
			Failed.Errors.Add(TEXT("The file has no description.xml; it is not a GDTF file."));
			return Failed;
		}
		if (Description->UncompressedSize > Limits.MaxDescriptionBytes)
		{
			Failed.Errors.Add(FString::Printf(TEXT("description.xml is %lld bytes; the limit is %lld."), Description->UncompressedSize, Limits.MaxDescriptionBytes));
			return Failed;
		}
		TArray<uint8> Bytes;
		if (!StageZip::Extract(FileBytes, *Description, Limits.Zip, Bytes, ZipError))
		{
			Failed.Errors.Add(ZipError);
			return Failed;
		}
		const int32 Skip = (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF) ? 3 : 0;
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Skip), Bytes.Num() - Skip);

		FStageGdtfParseResult Result = ParseDescription(FStringView(Text.Get(), Text.Length()), Limits);
		for (const StageZip::FEntry& Entry : Directory.Entries)
		{
			Result.Type.ArchiveFiles.Add(Entry.Name.ToLower());
		}
		for (const FStageGdtfWheel& Wheel : Result.Type.Wheels)
		{
			for (const FStageGdtfWheelSlot& Slot : Wheel.Slots)
			{
				if (!Slot.MediaFileName.IsEmpty() && !Result.Type.ArchiveFiles.Contains(FString::Printf(TEXT("wheels/%s.png"), *Slot.MediaFileName).ToLower()))
				{
					Result.Warnings.Add(FString::Printf(TEXT("Wheel \"%s\" slot \"%s\": image wheels/%s.png is missing."), *Wheel.Name, *Slot.Name, *Slot.MediaFileName));
				}
			}
		}
		return Result;
	}
}
