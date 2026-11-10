// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrParser.h"

#include "Misc/Paths.h"

namespace StageMvr
{
	namespace
	{
		/** Warnings kept for the report; a large rig with the same gap on every fixture must not flood it. */
		constexpr int32 MaxWarnings = 50;

		struct FKindName
		{
			const TCHAR* Element;
			EStageMvrObjectKind Kind;
			const TCHAR* Plural;
		};
		const FKindName KindNames[] =
		{
			{ TEXT("SceneObject"), EStageMvrObjectKind::SceneObject, TEXT("scene objects") },
			{ TEXT("GroupObject"), EStageMvrObjectKind::GroupObject, TEXT("groups") },
			{ TEXT("FocusPoint"), EStageMvrObjectKind::FocusPoint, TEXT("focus points") },
			{ TEXT("Fixture"), EStageMvrObjectKind::Fixture, TEXT("fixtures") },
			{ TEXT("Support"), EStageMvrObjectKind::Support, TEXT("supports") },
			{ TEXT("Truss"), EStageMvrObjectKind::Truss, TEXT("trusses") },
			{ TEXT("VideoScreen"), EStageMvrObjectKind::VideoScreen, TEXT("video screens") },
			{ TEXT("Projector"), EStageMvrObjectKind::Projector, TEXT("projectors") },
		};
		static_assert(UE_ARRAY_COUNT(KindNames) == static_cast<int32>(EStageMvrObjectKind::Count), "One entry per object kind.");

		const FKindName* FindKind(FStringView Element)
		{
			for (const FKindName& Entry : KindNames)
			{
				if (Element.Equals(Entry.Element, ESearchCase::CaseSensitive))
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		/** A whole string as a finite number (exponents allowed), nothing before or after it. */
		bool ParseNumber(FStringView Text, double& OutValue)
		{
			const FString Trimmed = FString(Text).TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Trimmed.Len() > 64)
			{
				return false;
			}
			// The whole text must be [+-]digits[.digits][e[+-]digits] with at least one digit: Atod alone would read "1x" as 1.
			int32 Index = Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('+') ? 1 : 0;
			int32 Digits = 0;
			const auto SkipDigits = [&Trimmed, &Index]()
			{
				int32 Count = 0;
				for (; Index < Trimmed.Len() && FChar::IsDigit(Trimmed[Index]); ++Index, ++Count)
				{
				}
				return Count;
			};
			Digits += SkipDigits();
			if (Index < Trimmed.Len() && Trimmed[Index] == TEXT('.'))
			{
				++Index;
				Digits += SkipDigits();
			}
			if (Digits == 0)
			{
				return false;
			}
			if (Index < Trimmed.Len() && (Trimmed[Index] == TEXT('e') || Trimmed[Index] == TEXT('E')))
			{
				++Index;
				Index += Index < Trimmed.Len() && (Trimmed[Index] == TEXT('-') || Trimmed[Index] == TEXT('+')) ? 1 : 0;
				if (SkipDigits() == 0)
				{
					return false;
				}
			}
			if (Index != Trimmed.Len())
			{
				return false;
			}
			const double Value = FCString::Atod(*Trimmed);
			if (!FMath::IsFinite(Value))
			{
				return false;
			}
			OutValue = Value;
			return true;
		}

		/** A whole string as an integer in [Min, Max]. */
		bool ParseInteger(FStringView Text, int64 Min, int64 Max, int64& OutValue)
		{
			const FString Trimmed = FString(Text).TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Trimmed.Len() > 18)
			{
				return false;
			}
			int32 Index = Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('+') ? 1 : 0;
			if (Index >= Trimmed.Len())
			{
				return false;
			}
			for (int32 Char = Index; Char < Trimmed.Len(); ++Char)
			{
				if (!FChar::IsDigit(Trimmed[Char]))
				{
					return false;
				}
			}
			const int64 Value = FCString::Atoi64(*Trimmed);
			if (Value < Min || Value > Max)
			{
				return false;
			}
			OutValue = Value;
			return true;
		}

		FString Trim(const FString& Text)
		{
			return Text.TrimStartAndEnd();
		}

		/** A file reference as the archive names it: ".3ds" or ".gdtf" added when the name has no extension (spec and real files). */
		FString WithExtension(const FString& Name, const TCHAR* DefaultExtension)
		{
			return FPaths::GetExtension(Name).IsEmpty() ? Name + DefaultExtension : Name;
		}

		/** Builds the scene from the parsed document; collects errors and warnings. */
		class FSceneReader
		{
		public:
			FSceneReader(const StageXml::FDocument& InDocument, const FLimits& InLimits, FStageMvrParseResult& InResult)
				: Document(InDocument), Limits(InLimits), Result(InResult), Scene(InResult.Scene)
			{
			}

			/** Warnings over MaxWarnings, counted but not kept. */
			int32 GetSuppressedWarnings() const { return SuppressedWarnings; }

			void Run()
			{
				const StageXml::FNode& Root = Document.Nodes[0];
				if (Root.Name != TEXT("GeneralSceneDescription"))
				{
					Error(0, FString::Printf(TEXT("the root element is <%s>, not <GeneralSceneDescription>; this is not an MVR scene"), *Root.Name));
					return;
				}
				int64 Major = 0;
				const FString* MajorText = Root.FindAttribute(TEXT("verMajor"));
				if (!MajorText || !ParseInteger(*MajorText, 0, 1000, Major))
				{
					Error(0, TEXT("verMajor is missing or not a number"));
					return;
				}
				Scene.VerMajor = int32(Major);
				int64 Minor = 0;
				const FString* MinorText = Root.FindAttribute(TEXT("verMinor"));
				if (MinorText && ParseInteger(*MinorText, 0, 1000, Minor))
				{
					Scene.VerMinor = int32(Minor);
				}
				else
				{
					Warn(TEXT("verMinor is missing or not a number; read as 0."));
				}
				if (Scene.VerMajor != 1)
				{
					Error(0, FString::Printf(TEXT("MVR version %d.%d is not supported (1.x is)"), Scene.VerMajor, Scene.VerMinor));
					return;
				}
				if (const FString* Provider = Root.FindAttribute(TEXT("provider")))
				{
					Scene.Provider = *Provider;
				}
				if (const FString* ProviderVersion = Root.FindAttribute(TEXT("providerVersion")))
				{
					Scene.ProviderVersion = *ProviderVersion;
				}

				const int32 SceneNode = Document.FindChild(0, TEXT("Scene"));
				if (SceneNode == INDEX_NONE)
				{
					Error(0, TEXT("there is no <Scene>"));
					return;
				}
				ReadSymdefs(Document.FindChild(SceneNode, TEXT("AUXData")));
				const int32 LayersNode = Document.FindChild(SceneNode, TEXT("Layers"));
				for (const int32 LayerNode : Document.GetChildren(LayersNode, TEXT("Layer")))
				{
					if (!Result.IsValid())
					{
						return;
					}
					ReadLayer(LayerNode);
				}
				if (Result.IsValid())
				{
					CheckReferences();
				}
				for (const TPair<FString, int32>& Unknown : UnknownElements)
				{
					Warn(FString::Printf(TEXT("%d <%s> %s not read."), Unknown.Value, *Unknown.Key, Unknown.Value == 1 ? TEXT("element was") : TEXT("elements were")));
				}
				if (Scene.Objects.IsEmpty() && Result.IsValid())
				{
					Warn(TEXT("The scene has no objects."));
				}
			}

		private:
			void Error(int32 Node, const FString& Reason)
			{
				const int32 Line = Document.Nodes.IsValidIndex(Node) ? Document.Nodes[Node].Line : 0;
				Result.Errors.Add(FString::Printf(TEXT("GeneralSceneDescription.xml line %d: %s."), Line, *Reason));
			}

			void Warn(const FString& Text)
			{
				if (Result.Warnings.Num() < MaxWarnings)
				{
					Result.Warnings.Add(Text);
				}
				else
				{
					++SuppressedWarnings;
				}
			}

			/** The text of Node's first child named Name, trimmed; empty when there is none. */
			FString ChildText(int32 Node, const TCHAR* Name) const
			{
				const int32 Child = Document.FindChild(Node, Name);
				return Child == INDEX_NONE ? FString() : Trim(Document.Nodes[Child].Text);
			}

			/** Node's Matrix child (identity when there is none). False, with an error, when it is malformed. */
			bool ReadMatrix(int32 Node, FMatrix& OutMatrix)
			{
				OutMatrix = FMatrix::Identity;
				const int32 MatrixNode = Document.FindChild(Node, TEXT("Matrix"));
				if (MatrixNode == INDEX_NONE)
				{
					return true;
				}
				if (!ParseMatrix(Document.Nodes[MatrixNode].Text, OutMatrix))
				{
					Error(MatrixNode, TEXT("<Matrix> is not four {x,y,z} groups of numbers"));
					return false;
				}
				return true;
			}

			/** The uuid attribute of Node, required and unique. */
			bool ReadId(int32 Node, FGuid& OutId)
			{
				const FString* Text = Document.Nodes[Node].FindAttribute(TEXT("uuid"));
				if (!Text || !ParseUuid(*Text, OutId))
				{
					Error(Node, FString::Printf(TEXT("<%s> has no valid uuid"), *Document.Nodes[Node].Name));
					return false;
				}
				bool bAlreadyUsed = false;
				UsedIds.Add(OutId, &bAlreadyUsed);
				if (bAlreadyUsed)
				{
					Error(Node, FString::Printf(TEXT("uuid %s is used twice"), *Text->TrimStartAndEnd()));
					return false;
				}
				return true;
			}

			/** Geometry3D file names and Symbol references under Node's Geometries (objects) or directly under Node (symdefs). */
			bool ReadGeometry(int32 Container, TArray<FString>& OutFiles, TArray<FGuid>* OutSymbols)
			{
				if (!Document.Nodes.IsValidIndex(Container))
				{
					return true;
				}
				for (const int32 Child : Document.Nodes[Container].Children)
				{
					const StageXml::FNode& Node = Document.Nodes[Child];
					if (Node.Name == TEXT("Geometry3D"))
					{
						const FString* File = Node.FindAttribute(TEXT("fileName"));
						if (!File || Trim(*File).IsEmpty())
						{
							Error(Child, TEXT("<Geometry3D> has no fileName"));
							return false;
						}
						OutFiles.AddUnique(WithExtension(Trim(*File), TEXT(".3ds")));
					}
					else if (Node.Name == TEXT("Symbol") && OutSymbols)
					{
						const FString* Symdef = Node.FindAttribute(TEXT("symdef"));
						FGuid Id;
						if (!Symdef || !ParseUuid(*Symdef, Id))
						{
							Error(Child, TEXT("<Symbol> has no valid symdef"));
							return false;
						}
						OutSymbols->AddUnique(Id);
					}
				}
				return true;
			}

			void ReadSymdefs(int32 AuxNode)
			{
				for (const int32 Node : Document.GetChildren(AuxNode, TEXT("Symdef")))
				{
					FStageMvrSymdef Symdef;
					if (!ReadId(Node, Symdef.Id))
					{
						return;
					}
					if (const FString* Name = Document.Nodes[Node].FindAttribute(TEXT("name")))
					{
						Symdef.Name = *Name;
					}
					if (!ReadGeometry(Document.FindChild(Node, TEXT("ChildList")), Symdef.GeometryFiles, nullptr))
					{
						return;
					}
					Scene.Symdefs.Add(MoveTemp(Symdef));
				}
			}

			void ReadLayer(int32 Node)
			{
				if (Scene.Layers.Num() >= Limits.MaxLayers)
				{
					Error(Node, FString::Printf(TEXT("more than %d layers"), Limits.MaxLayers));
					return;
				}
				FStageMvrLayer Layer;
				if (!ReadId(Node, Layer.Id) || !ReadMatrix(Node, Layer.Matrix))
				{
					return;
				}
				if (const FString* Name = Document.Nodes[Node].FindAttribute(TEXT("name")))
				{
					Layer.Name = *Name;
				}
				const int32 LayerIndex = Scene.Layers.Add(MoveTemp(Layer));
				ReadChildList(Document.FindChild(Node, TEXT("ChildList")), LayerIndex, INDEX_NONE, Scene.Layers[LayerIndex].Matrix, 1);
			}

			void ReadChildList(int32 ListNode, int32 Layer, int32 Parent, const FMatrix& ParentWorld, int32 Depth)
			{
				if (ListNode == INDEX_NONE)
				{
					return;
				}
				if (Depth > Limits.MaxObjectDepth)
				{
					Error(ListNode, FString::Printf(TEXT("objects nested deeper than %d"), Limits.MaxObjectDepth));
					return;
				}
				for (const int32 Child : Document.Nodes[ListNode].Children)
				{
					if (!Result.IsValid())
					{
						return;
					}
					const FKindName* Kind = FindKind(Document.Nodes[Child].Name);
					if (!Kind)
					{
						++UnknownElements.FindOrAdd(Document.Nodes[Child].Name);
						continue;
					}
					ReadObject(Child, Kind->Kind, Layer, Parent, ParentWorld, Depth);
				}
			}

			void ReadObject(int32 Node, EStageMvrObjectKind Kind, int32 Layer, int32 Parent, const FMatrix& ParentWorld, int32 Depth)
			{
				if (Scene.Objects.Num() >= Limits.MaxObjects)
				{
					Error(Node, FString::Printf(TEXT("more than %d objects"), Limits.MaxObjects));
					return;
				}
				if (Kind == EStageMvrObjectKind::Fixture && ++FixtureCount > Limits.MaxFixtures)
				{
					Error(Node, FString::Printf(TEXT("more than %d fixtures"), Limits.MaxFixtures));
					return;
				}
				const StageXml::FNode& Element = Document.Nodes[Node];
				FStageMvrObject Object;
				Object.Kind = Kind;
				Object.Layer = Layer;
				Object.Parent = Parent;
				Object.Line = Element.Line;
				if (!ReadId(Node, Object.Id) || !ReadMatrix(Node, Object.Local))
				{
					return;
				}
				if (const FString* Name = Element.FindAttribute(TEXT("name")))
				{
					Object.Name = *Name;
				}
				Object.World = Object.Local * ParentWorld;
				if (!ReadGeometry(Document.FindChild(Node, TEXT("Geometries")), Object.GeometryFiles, &Object.Symbols))
				{
					return;
				}
				if (Kind == EStageMvrObjectKind::Fixture && !ReadFixture(Node, Object))
				{
					return;
				}
				const int32 Index = Scene.Objects.Add(MoveTemp(Object));
				ReadChildList(Document.FindChild(Node, TEXT("ChildList")), Layer, Index, Scene.Objects[Index].World, Depth + 1);
			}

			bool ReadFixture(int32 Node, FStageMvrObject& Fixture)
			{
				const FString Label = FString::Printf(TEXT("Fixture \"%s\" (line %d)"), *Fixture.Name, Fixture.Line);
				const FString Spec = ChildText(Node, TEXT("GDTFSpec"));
				Fixture.GdtfSpec = Spec.IsEmpty() ? FString() : WithExtension(Spec, TEXT(".gdtf"));
				Fixture.GdtfMode = ChildText(Node, TEXT("GDTFMode"));
				if (Fixture.GdtfSpec.IsEmpty())
				{
					Warn(FString::Printf(TEXT("%s has no GDTF file; it will be placed as a generic fixture."), *Label));
				}
				else if (Fixture.GdtfMode.IsEmpty())
				{
					Warn(FString::Printf(TEXT("%s names no GDTF mode; its type's first mode will be used."), *Label));
				}
				if (const FString* Multipatch = Document.Nodes[Node].FindAttribute(TEXT("multipatch")); Multipatch && !Trim(*Multipatch).IsEmpty())
				{
					if (!ParseUuid(*Multipatch, Fixture.MultipatchOf))
					{
						Error(Node, TEXT("multipatch is not a valid uuid"));
						return false;
					}
				}
				const FString Focus = ChildText(Node, TEXT("Focus"));
				if (!Focus.IsEmpty() && !ParseUuid(Focus, Fixture.Focus))
				{
					Error(Document.FindChild(Node, TEXT("Focus")), TEXT("<Focus> is not a valid uuid"));
					return false;
				}
				Fixture.FixtureId = ChildText(Node, TEXT("FixtureID")).Left(64);
				const auto ReadCount = [this, Node](const TCHAR* Name, int32& Out)
				{
					const FString Text = ChildText(Node, Name);
					int64 Value = 0;
					if (Text.IsEmpty())
					{
						return true;
					}
					if (!ParseInteger(Text, 0, MAX_int32, Value))
					{
						Error(Document.FindChild(Node, Name), FString::Printf(TEXT("<%s> is not a whole number"), Name));
						return false;
					}
					Out = int32(Value);
					return true;
				};
				if (!ReadCount(TEXT("FixtureIDNumeric"), Fixture.FixtureIdNumeric) || !ReadCount(TEXT("UnitNumber"), Fixture.UnitNumber)
					|| !ReadCount(TEXT("CustomId"), Fixture.CustomId))
				{
					return false;
				}
				for (const int32 AddressNode : Document.GetChildren(Document.FindChild(Node, TEXT("Addresses")), TEXT("Address")))
				{
					if (Fixture.Addresses.Num() >= Limits.MaxAddressesPerFixture)
					{
						Error(AddressNode, FString::Printf(TEXT("more than %d addresses on one fixture"), Limits.MaxAddressesPerFixture));
						return false;
					}
					const StageXml::FNode& Address = Document.Nodes[AddressNode];
					int64 Break = 0;
					const FString* BreakText = Address.FindAttribute(TEXT("break"));
					FStageMvrAddress Parsed;
					if ((BreakText && !ParseInteger(*BreakText, 0, 1024, Break)) || !ParseAddress(Address.Text, int32(Break), Parsed))
					{
						Error(AddressNode, FString::Printf(TEXT("<Address> \"%s\" is not Universe.Address or an absolute DMX address"), *Trim(Address.Text).Left(32)));
						return false;
					}
					if (Parsed.Break != 0)
					{
						Warn(FString::Printf(TEXT("%s: DMX break %d is not patched (only break 0 is)."), *Label, Parsed.Break));
					}
					Fixture.Addresses.Add(Parsed);
				}
				return true;
			}

			/** Focus, multipatch and symbol references must name objects of the right kind; GDTF types are capped. */
			void CheckReferences()
			{
				TMap<FGuid, EStageMvrObjectKind> Kinds;
				for (const FStageMvrObject& Object : Scene.Objects)
				{
					Kinds.Add(Object.Id, Object.Kind);
				}
				TSet<FGuid> SymdefIds;
				for (const FStageMvrSymdef& Symdef : Scene.Symdefs)
				{
					SymdefIds.Add(Symdef.Id);
				}
				for (FStageMvrObject& Object : Scene.Objects)
				{
					const EStageMvrObjectKind* FocusKind = Object.Focus.IsValid() ? Kinds.Find(Object.Focus) : nullptr;
					if (Object.Focus.IsValid() && (!FocusKind || *FocusKind != EStageMvrObjectKind::FocusPoint))
					{
						Warn(FString::Printf(TEXT("Fixture \"%s\" (line %d) aims at a focus point that is not in the file; it keeps its own aim."), *Object.Name, Object.Line));
						Object.Focus.Invalidate();
					}
					const EStageMvrObjectKind* PatchKind = Object.MultipatchOf.IsValid() ? Kinds.Find(Object.MultipatchOf) : nullptr;
					if (Object.MultipatchOf.IsValid() && (!PatchKind || *PatchKind != EStageMvrObjectKind::Fixture))
					{
						Warn(FString::Printf(TEXT("Fixture \"%s\" (line %d) is a multipatch of a fixture that is not in the file."), *Object.Name, Object.Line));
						Object.MultipatchOf.Invalidate();
					}
					for (int32 Index = Object.Symbols.Num() - 1; Index >= 0; --Index)
					{
						if (!SymdefIds.Contains(Object.Symbols[Index]))
						{
							Warn(FString::Printf(TEXT("\"%s\" (line %d) uses a symbol that is not defined; it is left out."), *Object.Name, Object.Line));
							Object.Symbols.RemoveAt(Index);
						}
					}
				}
				const int32 GdtfFiles = Scene.GetGdtfSpecs().Num();
				if (GdtfFiles > Limits.MaxGdtfFiles)
				{
					Error(0, FString::Printf(TEXT("%d different GDTF files; the limit is %d"), GdtfFiles, Limits.MaxGdtfFiles));
				}
			}

			const StageXml::FDocument& Document;
			const FLimits& Limits;
			FStageMvrParseResult& Result;
			FStageMvrScene& Scene;
			TSet<FGuid> UsedIds;
			TMap<FString, int32> UnknownElements;
			int32 FixtureCount = 0;
			int32 SuppressedWarnings = 0;
		};

		/** With the archive's file list known: every referenced GDTF and geometry file is named in the report when it is missing. */
		void CheckArchiveFiles(FStageMvrParseResult& Result)
		{
			const FStageMvrScene& Scene = Result.Scene;
			TSet<FString> Reported;
			const auto Check = [&Result, &Scene, &Reported](const FString& File, const TCHAR* What)
			{
				const FString Key = File.ToLower();
				if (!Scene.ArchiveFiles.Contains(Key) && !Reported.Contains(Key) && Result.Warnings.Num() < MaxWarnings)
				{
					Reported.Add(Key);
					Result.Warnings.Add(FString::Printf(TEXT("%s \"%s\" is not in the file."), What, *File));
				}
			};
			bool bHas3ds = false;
			for (const FStageMvrObject& Object : Scene.Objects)
			{
				if (!Object.GdtfSpec.IsEmpty())
				{
					Check(Object.GdtfSpec, TEXT("GDTF file"));
				}
				for (const FString& File : Object.GeometryFiles)
				{
					Check(File, TEXT("Geometry"));
					bHas3ds |= FPaths::GetExtension(File).Equals(TEXT("3ds"), ESearchCase::IgnoreCase);
				}
			}
			for (const FStageMvrSymdef& Symdef : Scene.Symdefs)
			{
				for (const FString& File : Symdef.GeometryFiles)
				{
					Check(File, TEXT("Geometry"));
					bHas3ds |= FPaths::GetExtension(File).Equals(TEXT("3ds"), ESearchCase::IgnoreCase);
				}
			}
			if (bHas3ds && Result.Warnings.Num() < MaxWarnings)
			{
				Result.Warnings.Add(TEXT("Some geometry is 3DS (deprecated in MVR 1.6); it will be imported as boxes or left out, not as meshes."));
			}
		}
	}

	FLimits::FLimits()
	{
		Zip.MaxArchiveBytes = 64ll * 1024 * 1024;
		Zip.MaxEntries = 512;
		Zip.MaxEntryBytes = 64ll * 1024 * 1024;
		Zip.MaxTotalBytes = 256ll * 1024 * 1024;
		Xml.bKeepText = true;
		// A Layer's ChildList is at depth 5; each nested object adds two (object, ChildList), its Addresses two more.
		Xml.MaxDepth = 8 + 2 * MaxObjectDepth;
		// About 25 elements for a fully described fixture.
		Xml.MaxNodes = 25 * MaxObjects;
	}

	bool ParseMatrix(FStringView Text, FMatrix& OutMatrix)
	{
		// Four groups "{a,b,c}"; white space anywhere between the tokens.
		FString Compact;
		Compact.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsWhitespace(Char))
			{
				Compact.AppendChar(Char);
			}
		}
		double Values[4][3];
		int32 Position = 0;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			if (Position >= Compact.Len() || Compact[Position] != TEXT('{'))
			{
				return false;
			}
			const int32 Close = Compact.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Position);
			if (Close == INDEX_NONE)
			{
				return false;
			}
			TArray<FString> Parts;
			Compact.Mid(Position + 1, Close - Position - 1).ParseIntoArray(Parts, TEXT(","), /*InCullEmpty*/ false);
			if (Parts.Num() != 3)
			{
				return false;
			}
			for (int32 Column = 0; Column < 3; ++Column)
			{
				if (!ParseNumber(Parts[Column], Values[Row][Column]))
				{
					return false;
				}
			}
			Position = Close + 1;
		}
		if (Position != Compact.Len())
		{
			return false;
		}
		OutMatrix = FMatrix(
			FPlane(Values[0][0], Values[0][1], Values[0][2], 0.0),
			FPlane(Values[1][0], Values[1][1], Values[1][2], 0.0),
			FPlane(Values[2][0], Values[2][1], Values[2][2], 0.0),
			FPlane(Values[3][0], Values[3][1], Values[3][2], 1.0));
		return true;
	}

	bool ParseAddress(FStringView Text, int32 Break, FStageMvrAddress& OutAddress)
	{
		// Art-Net's 15-bit port address is the largest universe space a console uses.
		constexpr int64 MaxUniverse = 32768;
		if (Break < 0)
		{
			return false;
		}
		const FString Trimmed = FString(Text).TrimStartAndEnd();
		FString UniverseText;
		FString AddressText;
		int64 Universe = 0;
		int64 Address = 0;
		if (Trimmed.Split(TEXT("."), &UniverseText, &AddressText))
		{
			if (!ParseInteger(UniverseText, 1, MaxUniverse, Universe) || !ParseInteger(AddressText, 1, 512, Address))
			{
				return false;
			}
		}
		else
		{
			int64 Absolute = 0;
			if (!ParseInteger(Trimmed, 1, MaxUniverse * 512, Absolute))
			{
				return false;
			}
			Universe = (Absolute - 1) / 512 + 1;
			Address = (Absolute - 1) % 512 + 1;
		}
		OutAddress.Break = Break;
		OutAddress.Universe = int32(Universe);
		OutAddress.Address = int32(Address);
		return true;
	}

	bool ParseUuid(FStringView Text, FGuid& OutId)
	{
		FString Digits;
		for (const TCHAR Char : FString(Text).TrimStartAndEnd())
		{
			if (Char == TEXT('-') || Char == TEXT('{') || Char == TEXT('}'))
			{
				continue;
			}
			if (!FChar::IsHexDigit(Char) || Digits.Len() >= 32)
			{
				return false;
			}
			Digits.AppendChar(Char);
		}
		FGuid Id;
		if (Digits.Len() != 32 || !FGuid::ParseExact(Digits, EGuidFormats::Digits, Id) || !Id.IsValid())
		{
			return false;
		}
		OutId = Id;
		return true;
	}

	FStageMvrParseResult ParseSceneDescription(FStringView Xml, const FLimits& Limits)
	{
		FStageMvrParseResult Result;
		StageXml::FDocument Document;
		FString XmlError;
		if (!StageXml::Parse(Xml, Limits.Xml, Document, XmlError))
		{
			Result.Errors.Add(FString::Printf(TEXT("GeneralSceneDescription.xml %s."), *XmlError));
			return Result;
		}
		FSceneReader Reader(Document, Limits, Result);
		Reader.Run();
		if (Reader.GetSuppressedWarnings() > 0)
		{
			Result.Warnings.Add(FString::Printf(TEXT("... and %d more warnings."), Reader.GetSuppressedWarnings()));
		}
		if (!Result.IsValid())
		{
			Result.Scene = FStageMvrScene();
		}
		return Result;
	}

	FStageMvrParseResult Parse(TConstArrayView<uint8> FileBytes, const FLimits& Limits)
	{
		FStageMvrParseResult Failed;
		StageZip::FDirectory Directory;
		FString ZipError;
		if (!StageZip::ReadDirectory(FileBytes, Limits.Zip, Directory, ZipError))
		{
			Failed.Errors.Add(ZipError);
			return Failed;
		}
		for (const StageZip::FEntry& Entry : Directory.Entries)
		{
			if (Entry.Name.Contains(TEXT("/")))
			{
				Failed.Errors.Add(FString::Printf(TEXT("\"%s\" is in a folder; MVR keeps every file at the archive root."), *Entry.Name.Left(128)));
				return Failed;
			}
		}
		const StageZip::FEntry* Description = Directory.Find(TEXT("GeneralSceneDescription.xml"));
		if (!Description)
		{
			Failed.Errors.Add(TEXT("The file has no GeneralSceneDescription.xml; it is not an MVR file."));
			return Failed;
		}
		if (Description->UncompressedSize > Limits.MaxSceneBytes)
		{
			Failed.Errors.Add(FString::Printf(TEXT("GeneralSceneDescription.xml is %lld bytes; the limit is %lld."), Description->UncompressedSize, Limits.MaxSceneBytes));
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
		// The converted text is a copy: free the UTF-8 bytes (up to MaxSceneBytes) before the XML tree is built.
		Bytes.Empty();

		FStageMvrParseResult Result = ParseSceneDescription(FStringView(Text.Get(), Text.Length()), Limits);
		if (!Result.IsValid())
		{
			return Result;
		}
		for (const StageZip::FEntry& Entry : Directory.Entries)
		{
			Result.Scene.ArchiveFiles.Add(Entry.Name.ToLower());
		}
		CheckArchiveFiles(Result);
		return Result;
	}

	FString Describe(const FStageMvrScene& Scene)
	{
		TArray<FString> Parts;
		for (const FKindName& Entry : KindNames)
		{
			if (const int32 Count = Scene.Count(Entry.Kind))
			{
				Parts.Add(FString::Printf(TEXT("%d %s"), Count, Entry.Plural));
			}
		}
		return FString::Printf(TEXT("MVR %d.%d from %s %s: %d %s; %s; %d GDTF %s"), Scene.VerMajor, Scene.VerMinor,
			Scene.Provider.IsEmpty() ? TEXT("an unnamed application") : *Scene.Provider, *Scene.ProviderVersion,
			Scene.Layers.Num(), Scene.Layers.Num() == 1 ? TEXT("layer") : TEXT("layers"),
			Parts.IsEmpty() ? TEXT("no objects") : *FString::Join(Parts, TEXT(", ")),
			Scene.GetGdtfSpecs().Num(), Scene.GetGdtfSpecs().Num() == 1 ? TEXT("type") : TEXT("types"));
	}
}

int32 FStageMvrScene::Count(EStageMvrObjectKind Kind) const
{
	int32 Count = 0;
	for (const FStageMvrObject& Object : Objects)
	{
		Count += Object.Kind == Kind ? 1 : 0;
	}
	return Count;
}

const FStageMvrObject* FStageMvrScene::FindObject(const FGuid& Id) const
{
	return Objects.FindByPredicate([&Id](const FStageMvrObject& Object) { return Object.Id == Id; });
}

TArray<FString> FStageMvrScene::GetGdtfSpecs() const
{
	TArray<FString> Specs;
	for (const FStageMvrObject& Object : Objects)
	{
		if (Object.Kind == EStageMvrObjectKind::Fixture && !Object.GdtfSpec.IsEmpty()
			&& !Specs.ContainsByPredicate([&Object](const FString& Spec) { return Spec.Equals(Object.GdtfSpec, ESearchCase::IgnoreCase); }))
		{
			Specs.Add(Object.GdtfSpec);
		}
	}
	return Specs;
}

namespace StageMvrUnits
{
	namespace
	{
		/** Mirrors Y: negates every element in exactly one Y row or column (S * M * S with S = diag(1, -1, 1, 1)). */
		FMatrix MirrorY(const FMatrix& In)
		{
			FMatrix Out = In;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					if ((Row == 1) != (Column == 1))
					{
						Out.M[Row][Column] = -In.M[Row][Column];
					}
				}
			}
			return Out;
		}

		/** Millimetres per Unreal unit (centimetre). */
		constexpr double MmPerCm = 10.0;
	}

	FTransform ToUnreal(const FMatrix& MvrMatrix)
	{
		FMatrix Matrix = MirrorY(MvrMatrix);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Matrix.M[3][Axis] /= MmPerCm;
		}
		return FTransform(Matrix);
	}

	FMatrix FromUnreal(const FTransform& Transform)
	{
		FMatrix Matrix = Transform.ToMatrixWithScale();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Matrix.M[3][Axis] *= MmPerCm;
		}
		return MirrorY(Matrix);
	}
}
