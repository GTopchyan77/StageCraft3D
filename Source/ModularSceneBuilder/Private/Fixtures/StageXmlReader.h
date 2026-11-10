// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A small, strict XML reader for untrusted interchange files (GDTF description.xml, later MVR; Docs/ADR/0033 §8, §14).
 *
 * Why not the engine's parsers: FXmlFile is line-based (one element per line, double quotes only), and FFastXml asserts (check)
 * on some malformed input, such as a broken declaration or very deep nesting, which a hostile file could use to crash a
 * Development build. This reader returns an error with a line number instead, and enforces hard caps.
 *
 * What it reads: elements and their attributes, with both quote styles, the five predefined entities and numeric character
 * references. Declarations, processing instructions and comments are skipped. Text content is ignored unless FLimits::bKeepText
 * (GDTF carries its data in attributes, MVR in the text of leaf elements). A DOCTYPE (and with it any entity definition) is
 * refused, so nothing is ever expanded.
 *
 * Pure: no files, no UObjects. Safe on any thread.
 */
namespace StageXml
{
	struct FLimits
	{
		int32 MaxDepth = 32;
		int32 MaxNodes = 100000;
		int32 MaxAttributesPerNode = 64;
		int32 MaxNameLength = 128;
		/** Longest attribute value, and longest element text when bKeepText. */
		int32 MaxValueLength = 4096;
		/**
		 * Keep the text of elements without child elements in FNode::Text (MVR carries its data there: Matrix, GDTFSpec, Address;
		 * STATE.md #87). Off for GDTF, which uses attributes only. Text around child elements (mixed content) is never kept.
		 */
		bool bKeepText = false;
	};

	/** One element. Its children are indices into FDocument::Nodes. */
	struct FNode
	{
		FString Name;
		TArray<TPair<FString, FString>> Attributes;
		TArray<int32> Children;
		int32 Parent = INDEX_NONE;
		/** 1-based line of the start tag, for error messages. */
		int32 Line = 0;
		/** With FLimits::bKeepText, the element's text (references decoded, CDATA included, not trimmed) when it has no child elements. */
		FString Text;

		/** The value of AttributeName (case-sensitive, as in XML), or null. */
		const FString* FindAttribute(FStringView AttributeName) const;
	};

	/** A parsed document. Nodes[0] is the root element. */
	struct FDocument
	{
		TArray<FNode> Nodes;

		/** The first child of Node named Name, or INDEX_NONE. */
		int32 FindChild(int32 Node, FStringView Name) const;

		/** Every child of Node named Name, in document order. */
		TArray<int32> GetChildren(int32 Node, FStringView Name) const;

		/** "GDTF/FixtureType/DMXModes/DMXMode" for error messages. */
		FString GetPath(int32 Node) const;
	};

	/** Parses Text. False, with "line N: reason" in OutError, when it is not well-formed or breaks a limit. */
	bool Parse(FStringView Text, const FLimits& Limits, FDocument& OutDocument, FString& OutError);
}
