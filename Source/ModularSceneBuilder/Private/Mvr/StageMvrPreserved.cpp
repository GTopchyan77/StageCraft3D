// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrPreserved.h"

#include "Mvr/StageMvrWriter.h"

namespace StageMvrPreserved
{
	const TCHAR* const RootName = TEXT("Keep");

	namespace
	{
		const TCHAR* const OwnedAttributes[] = { TEXT("uuid"), TEXT("name"), TEXT("multipatch") };
		const TCHAR* const OwnedElements[] = { TEXT("Matrix"), TEXT("Geometries"), TEXT("ChildList") };
		const TCHAR* const OwnedFixtureElements[] = { TEXT("GDTFSpec"), TEXT("GDTFMode"), TEXT("Addresses"), TEXT("FixtureID"), TEXT("FixtureIDNumeric") };
		const TCHAR* const DroppedElements[] = { TEXT("Focus"), TEXT("Classing"), TEXT("Position"), TEXT("Mappings"), TEXT("Gobo"), TEXT("Connections") };
		/** Only fixtures are exported with a GDTF type, so anything else's GDTF file would be missing from the archive. */
		const TCHAR* const DroppedTrussElements[] = { TEXT("GDTFSpec"), TEXT("GDTFMode") };

		/** Attribute names already written on one element: XML names are case-sensitive, so "a" and "A" are both kept. */
		bool AddName(TArray<FString>& Seen, const FString& Name)
		{
			if (Seen.ContainsByPredicate([&Name](const FString& Entry) { return Entry.Equals(Name, ESearchCase::CaseSensitive); }))
			{
				return false;
			}
			Seen.Add(Name);
			return true;
		}

		/** Nesting and node caps for reading a kept fragment back; a fragment is at most MaxChars, so these are generous. */
		constexpr int32 MaxReadDepth = 24;
		constexpr int32 MaxReadNodes = 2048;
		constexpr int32 MaxNameLength = 128;

		/** XML names are case-sensitive, so the lists are compared exactly. */
		bool IsIn(TConstArrayView<const TCHAR*> List, const FString& Name)
		{
			return List.ContainsByPredicate([&Name](const TCHAR* Entry) { return Name.Equals(Entry, ESearchCase::CaseSensitive); });
		}

		bool IsOwned(const FString& Element, EStageMvrObjectKind Kind)
		{
			return IsIn(OwnedElements, Element) || (Kind == EStageMvrObjectKind::Fixture && IsIn(OwnedFixtureElements, Element));
		}

		bool IsDropped(const FString& Element, EStageMvrObjectKind Kind)
		{
			return IsIn(DroppedElements, Element) || (Kind != EStageMvrObjectKind::Fixture && IsIn(DroppedTrussElements, Element));
		}

		/**
		 * A conservative XML name (ASCII letters, digits, '_', '-', '.', ':'; not starting with a digit, '-', '.' or ':'), so whatever
		 * a file or a scene file holds can be written into an element or attribute name without escaping.
		 */
		bool IsSafeName(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > MaxNameLength)
			{
				return false;
			}
			for (int32 Index = 0; Index < Name.Len(); ++Index)
			{
				const TCHAR Character = Name[Index];
				const bool bLetter = (Character >= TEXT('A') && Character <= TEXT('Z')) || (Character >= TEXT('a') && Character <= TEXT('z')) || Character == TEXT('_');
				const bool bOther = (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('-') || Character == TEXT('.') || Character == TEXT(':');
				if (!bLetter && (Index == 0 || !bOther))
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * "<UnitNumber>0</UnitNumber>" is what the export writes for a fixture without one, so keeping it would only make every
		 * StageCraft export look as if it carried data of its own when it is imported again.
		 */
		bool IsDefaultUnitNumber(const StageXml::FNode& Element)
		{
			return Element.Name.Equals(TEXT("UnitNumber"), ESearchCase::CaseSensitive) && Element.Attributes.IsEmpty() && Element.Children.IsEmpty()
				&& Element.Text.TrimStartAndEnd() == TEXT("0");
		}

		FString AttributeText(const FString& Name, const FString& Value)
		{
			return FString::Printf(TEXT(" %s=\"%s\""), *Name, *StageMvrExport::EscapeXml(Value));
		}

		/** Appends Node and its subtree as canonical XML. False (and Out unchanged) when a name in it is not safe. */
		bool WriteElement(const StageXml::FDocument& Document, int32 Node, FString& Out)
		{
			const StageXml::FNode& Element = Document.Nodes[Node];
			if (!IsSafeName(Element.Name))
			{
				return false;
			}
			FString Text = TEXT("<") + Element.Name;
			TArray<FString> Seen;
			for (const TPair<FString, FString>& Attribute : Element.Attributes)
			{
				if (!IsSafeName(Attribute.Key) || !AddName(Seen, Attribute.Key))
				{
					return false;
				}
				Text += AttributeText(Attribute.Key, Attribute.Value);
			}
			if (Element.Children.IsEmpty() && Element.Text.IsEmpty())
			{
				Out += Text + TEXT("/>");
				return true;
			}
			Text += TEXT(">");
			if (Element.Children.IsEmpty())
			{
				Text += StageMvrExport::EscapeXml(Element.Text);
			}
			for (const int32 Child : Element.Children)
			{
				if (!WriteElement(Document, Child, Text))
				{
					return false;
				}
			}
			Out += Text + TEXT("</") + Element.Name + TEXT(">");
			return true;
		}
	}

	TConstArrayView<const TCHAR*> DroppedReferences()
	{
		return DroppedElements;
	}

	FCaptured Capture(const StageXml::FDocument& Document, int32 ObjectNode, EStageMvrObjectKind Kind)
	{
		FCaptured Out;
		if (!Document.Nodes.IsValidIndex(ObjectNode))
		{
			return Out;
		}
		const StageXml::FNode& Object = Document.Nodes[ObjectNode];
		FString Attributes;
		for (const TPair<FString, FString>& Attribute : Object.Attributes)
		{
			if (!IsIn(OwnedAttributes, Attribute.Key) && IsSafeName(Attribute.Key))
			{
				Attributes += AttributeText(Attribute.Key, Attribute.Value);
				++Out.Attributes;
			}
		}
		FString Children;
		for (const int32 Child : Object.Children)
		{
			const FString& Name = Document.Nodes[Child].Name;
			if (IsOwned(Name, Kind) || IsDefaultUnitNumber(Document.Nodes[Child]))
			{
				continue;
			}
			if (IsDropped(Name, Kind))
			{
				Out.Dropped.Add(Name);
				continue;
			}
			if (WriteElement(Document, Child, Children))
			{
				++Out.Elements;
			}
		}
		if (Out.Attributes == 0 && Out.Elements == 0)
		{
			return Out;
		}
		Out.Xml = FString::Printf(TEXT("<%s%s>%s</%s>"), RootName, *Attributes, *Children, RootName);
		if (Out.Xml.Len() > MaxChars)
		{
			Out.Xml.Reset();
			Out.bTooLarge = true;
		}
		return Out;
	}

	bool ReadForWriting(FStringView Xml, EStageMvrObjectKind Kind, FForWriting& Out)
	{
		Out = FForWriting();
		if (Xml.IsEmpty() || Xml.Len() > MaxChars)
		{
			return false;
		}
		StageXml::FLimits Limits;
		Limits.bKeepText = true;
		Limits.MaxDepth = MaxReadDepth;
		Limits.MaxNodes = MaxReadNodes;
		Limits.MaxNameLength = MaxNameLength;
		Limits.MaxValueLength = MaxChars;
		StageXml::FDocument Document;
		FString Error;
		if (!StageXml::Parse(Xml, Limits, Document, Error) || Document.Nodes.IsEmpty() || !Document.Nodes[0].Name.Equals(RootName, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const StageXml::FNode& Root = Document.Nodes[0];
		TArray<FString> Seen;
		for (const TPair<FString, FString>& Attribute : Root.Attributes)
		{
			if (IsIn(OwnedAttributes, Attribute.Key) || !IsSafeName(Attribute.Key) || !AddName(Seen, Attribute.Key))
			{
				return false;
			}
			Out.Attributes += AttributeText(Attribute.Key, Attribute.Value);
		}
		for (const int32 Child : Root.Children)
		{
			const FString& Name = Document.Nodes[Child].Name;
			FString Element;
			if (IsOwned(Name, Kind) || IsDropped(Name, Kind) || !WriteElement(Document, Child, Element))
			{
				return false;
			}
			Out.Children.Add(MoveTemp(Element));
			Out.ChildNames.Add(Name);
		}
		return !Out.Attributes.IsEmpty() || !Out.Children.IsEmpty();
	}
}
