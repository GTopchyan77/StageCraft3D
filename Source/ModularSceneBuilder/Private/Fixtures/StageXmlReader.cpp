// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageXmlReader.h"

namespace StageXml
{
	namespace
	{
		bool IsNameStart(TCHAR Char)
		{
			return (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('a') && Char <= TEXT('z')) || Char == TEXT('_') || Char == TEXT(':') || Char > 0x7F;
		}

		bool IsNameChar(TCHAR Char)
		{
			return IsNameStart(Char) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('-') || Char == TEXT('.');
		}

		bool IsSpace(TCHAR Char)
		{
			return Char == TEXT(' ') || Char == TEXT('\t') || Char == TEXT('\r') || Char == TEXT('\n');
		}

		/** Appends a Unicode code point to Out as UTF-16 when TCHAR is 16 bits wide. False for code points XML does not allow. */
		bool AppendCodePoint(uint32 CodePoint, FString& Out)
		{
			const bool bAllowed = CodePoint == 0x9 || CodePoint == 0xA || CodePoint == 0xD
				|| (CodePoint >= 0x20 && CodePoint <= 0xD7FF) || (CodePoint >= 0xE000 && CodePoint <= 0xFFFD) || (CodePoint >= 0x10000 && CodePoint <= 0x10FFFF);
			if (!bAllowed)
			{
				return false;
			}
			if constexpr (sizeof(TCHAR) == 2)
			{
				if (CodePoint >= 0x10000)
				{
					const uint32 Value = CodePoint - 0x10000;
					Out.AppendChar(TCHAR(0xD800 + (Value >> 10)));
					Out.AppendChar(TCHAR(0xDC00 + (Value & 0x3FF)));
					return true;
				}
			}
			Out.AppendChar(TCHAR(CodePoint));
			return true;
		}

		/** The parser's cursor over the text, counting lines as it goes. */
		class FParser
		{
		public:
			FParser(FStringView InText, const FLimits& InLimits, FDocument& InDocument, FString& InError)
				: Text(InText), Limits(InLimits), Document(InDocument), Error(InError)
			{
			}

			bool Run()
			{
				Document.Nodes.Reset();
				if (!Text.IsEmpty() && Text[0] == TCHAR(0xFEFF))
				{
					Advance(1); // Byte-order mark.
				}
				TArray<int32> Open;
				bool bRootClosed = false;
				while (Position < Text.Len())
				{
					const TCHAR Char = Text[Position];
					if (Char != TEXT('<'))
					{
						if (Char == TEXT('>') && Open.IsEmpty())
						{
							return Fail(TEXT("unexpected '>'"));
						}
						if (!Open.IsEmpty() || IsSpace(Char))
						{
							Advance(1); // Text content is not used.
							continue;
						}
						return Fail(TEXT("text outside the root element"));
					}
					if (StartsWith(TEXT("<?")))
					{
						if (!SkipPast(TEXT("?>"), TEXT("unterminated processing instruction")))
						{
							return false;
						}
					}
					else if (StartsWith(TEXT("<!--")))
					{
						if (!SkipPast(TEXT("-->"), TEXT("unterminated comment")))
						{
							return false;
						}
					}
					else if (StartsWith(TEXT("<![CDATA[")))
					{
						if (Open.IsEmpty())
						{
							return Fail(TEXT("CDATA outside the root element"));
						}
						if (!SkipPast(TEXT("]]>"), TEXT("unterminated CDATA section")))
						{
							return false;
						}
					}
					else if (StartsWith(TEXT("<!")))
					{
						return Fail(TEXT("document type declarations are not allowed"));
					}
					else if (StartsWith(TEXT("</")))
					{
						if (!ParseEndTag(Open))
						{
							return false;
						}
						bRootClosed = Open.IsEmpty();
					}
					else
					{
						if (bRootClosed || (Open.IsEmpty() && !Document.Nodes.IsEmpty()))
						{
							return Fail(TEXT("a second root element"));
						}
						if (!ParseStartTag(Open))
						{
							return false;
						}
						bRootClosed = Open.IsEmpty();
					}
				}
				if (!Open.IsEmpty())
				{
					return Fail(FString::Printf(TEXT("element <%s> is not closed"), *Document.Nodes[Open.Last()].Name));
				}
				if (Document.Nodes.IsEmpty())
				{
					return Fail(TEXT("no root element"));
				}
				return true;
			}

		private:
			bool Fail(const FString& Reason)
			{
				Error = FString::Printf(TEXT("line %d: %s"), Line, *Reason);
				return false;
			}

			bool StartsWith(const TCHAR* Prefix) const
			{
				return Text.RightChop(Position).StartsWith(Prefix, ESearchCase::CaseSensitive);
			}

			void Advance(int32 Count)
			{
				for (int32 Index = 0; Index < Count && Position < Text.Len(); ++Index)
				{
					if (Text[Position] == TEXT('\n'))
					{
						++Line;
					}
					++Position;
				}
			}

			void SkipSpace()
			{
				while (Position < Text.Len() && IsSpace(Text[Position]))
				{
					Advance(1);
				}
			}

			bool SkipPast(const TCHAR* Terminator, const TCHAR* FailureReason)
			{
				const int32 Found = Text.RightChop(Position).Find(Terminator, 0, ESearchCase::CaseSensitive);
				if (Found == INDEX_NONE)
				{
					return Fail(FailureReason);
				}
				Advance(Found + FCString::Strlen(Terminator));
				return true;
			}

			bool ReadName(FString& OutName)
			{
				if (Position >= Text.Len() || !IsNameStart(Text[Position]))
				{
					return Fail(TEXT("a name was expected"));
				}
				const int32 Start = Position;
				while (Position < Text.Len() && IsNameChar(Text[Position]))
				{
					++Position;
					if (Position - Start > Limits.MaxNameLength)
					{
						return Fail(FString::Printf(TEXT("a name is longer than %d characters"), Limits.MaxNameLength));
					}
				}
				OutName = FString(Text.Mid(Start, Position - Start));
				return true;
			}

			/** Reads a quoted attribute value and decodes its references. The cursor is on the opening quote. */
			bool ReadValue(FString& OutValue)
			{
				const TCHAR Quote = Position < Text.Len() ? Text[Position] : TEXT('\0');
				if (Quote != TEXT('"') && Quote != TEXT('\''))
				{
					return Fail(TEXT("an attribute value must be quoted"));
				}
				Advance(1);
				OutValue.Reset();
				while (true)
				{
					if (Position >= Text.Len())
					{
						return Fail(TEXT("an attribute value is not closed"));
					}
					const TCHAR Char = Text[Position];
					if (Char == Quote)
					{
						Advance(1);
						return true;
					}
					if (Char == TEXT('<'))
					{
						return Fail(TEXT("'<' inside an attribute value"));
					}
					if (Char == TEXT('&'))
					{
						if (!ReadReference(OutValue))
						{
							return false;
						}
					}
					else
					{
						OutValue.AppendChar(Char);
						Advance(1);
					}
					if (OutValue.Len() > Limits.MaxValueLength)
					{
						return Fail(FString::Printf(TEXT("an attribute value is longer than %d characters"), Limits.MaxValueLength));
					}
				}
			}

			/** Decodes "&name;" or "&#N;" / "&#xH;" at the cursor into Out. */
			bool ReadReference(FString& Out)
			{
				const FStringView Rest = Text.RightChop(Position);
				const int32 End = Rest.Left(16).Find(TEXT(";"));
				if (End == INDEX_NONE)
				{
					return Fail(TEXT("an '&' that is not a reference"));
				}
				const FStringView Body = Rest.Mid(1, End - 1);
				if (Body.Equals(TEXT("amp"), ESearchCase::CaseSensitive)) { Out.AppendChar(TEXT('&')); }
				else if (Body.Equals(TEXT("lt"), ESearchCase::CaseSensitive)) { Out.AppendChar(TEXT('<')); }
				else if (Body.Equals(TEXT("gt"), ESearchCase::CaseSensitive)) { Out.AppendChar(TEXT('>')); }
				else if (Body.Equals(TEXT("quot"), ESearchCase::CaseSensitive)) { Out.AppendChar(TEXT('"')); }
				else if (Body.Equals(TEXT("apos"), ESearchCase::CaseSensitive)) { Out.AppendChar(TEXT('\'')); }
				else if (Body.StartsWith(TEXT("#"), ESearchCase::CaseSensitive))
				{
					const bool bHex = Body.StartsWith(TEXT("#x"), ESearchCase::CaseSensitive);
					const FStringView Digits = Body.RightChop(bHex ? 2 : 1);
					if (Digits.IsEmpty() || Digits.Len() > 8)
					{
						return Fail(TEXT("a bad character reference"));
					}
					uint32 CodePoint = 0;
					for (const TCHAR Digit : Digits)
					{
						int32 Value = -1;
						if (Digit >= TEXT('0') && Digit <= TEXT('9')) { Value = Digit - TEXT('0'); }
						else if (bHex && Digit >= TEXT('a') && Digit <= TEXT('f')) { Value = Digit - TEXT('a') + 10; }
						else if (bHex && Digit >= TEXT('A') && Digit <= TEXT('F')) { Value = Digit - TEXT('A') + 10; }
						if (Value < 0)
						{
							return Fail(TEXT("a bad character reference"));
						}
						CodePoint = CodePoint * (bHex ? 16 : 10) + Value;
						if (CodePoint > 0x10FFFF)
						{
							return Fail(TEXT("a character reference outside Unicode"));
						}
					}
					if (!AppendCodePoint(CodePoint, Out))
					{
						return Fail(TEXT("a character reference to a character XML does not allow"));
					}
				}
				else
				{
					return Fail(FString::Printf(TEXT("unknown entity '&%s;'"), *FString(Body)));
				}
				Advance(End + 1);
				return true;
			}

			bool ParseStartTag(TArray<int32>& Open)
			{
				const int32 TagLine = Line;
				Advance(1); // '<'
				FString Name;
				if (!ReadName(Name))
				{
					return false;
				}
				if (Document.Nodes.Num() >= Limits.MaxNodes)
				{
					return Fail(FString::Printf(TEXT("more than %d elements"), Limits.MaxNodes));
				}
				if (Open.Num() >= Limits.MaxDepth)
				{
					return Fail(FString::Printf(TEXT("elements nested deeper than %d"), Limits.MaxDepth));
				}

				const int32 Index = Document.Nodes.Num();
				FNode& Node = Document.Nodes.AddDefaulted_GetRef();
				Node.Name = MoveTemp(Name);
				Node.Line = TagLine;
				Node.Parent = Open.IsEmpty() ? INDEX_NONE : Open.Last();
				if (Node.Parent != INDEX_NONE)
				{
					Document.Nodes[Node.Parent].Children.Add(Index);
				}

				while (true)
				{
					const int32 Before = Position;
					SkipSpace();
					if (Position >= Text.Len())
					{
						return Fail(FString::Printf(TEXT("tag <%s> is not closed"), *Document.Nodes[Index].Name));
					}
					if (StartsWith(TEXT("/>")))
					{
						Advance(2);
						return true; // Empty element: never opened.
					}
					if (Text[Position] == TEXT('>'))
					{
						Advance(1);
						Open.Add(Index);
						return true;
					}
					if (Position == Before)
					{
						return Fail(TEXT("attributes must be separated by white space"));
					}
					FString AttributeName;
					FString Value;
					if (!ReadName(AttributeName))
					{
						return false;
					}
					SkipSpace();
					if (Position >= Text.Len() || Text[Position] != TEXT('='))
					{
						return Fail(FString::Printf(TEXT("attribute %s has no value"), *AttributeName));
					}
					Advance(1);
					SkipSpace();
					if (!ReadValue(Value))
					{
						return false;
					}
					FNode& Current = Document.Nodes[Index];
					if (Current.FindAttribute(AttributeName))
					{
						return Fail(FString::Printf(TEXT("attribute %s appears twice"), *AttributeName));
					}
					if (Current.Attributes.Num() >= Limits.MaxAttributesPerNode)
					{
						return Fail(FString::Printf(TEXT("more than %d attributes on <%s>"), Limits.MaxAttributesPerNode, *Current.Name));
					}
					Current.Attributes.Emplace(MoveTemp(AttributeName), MoveTemp(Value));
				}
			}

			bool ParseEndTag(TArray<int32>& Open)
			{
				Advance(2); // "</"
				FString Name;
				if (!ReadName(Name))
				{
					return false;
				}
				SkipSpace();
				if (Position >= Text.Len() || Text[Position] != TEXT('>'))
				{
					return Fail(FString::Printf(TEXT("end tag </%s> is not closed"), *Name));
				}
				Advance(1);
				if (Open.IsEmpty() || !Document.Nodes[Open.Last()].Name.Equals(Name, ESearchCase::CaseSensitive))
				{
					return Fail(FString::Printf(TEXT("end tag </%s> does not match the open element"), *Name));
				}
				Open.Pop();
				return true;
			}

			FStringView Text;
			const FLimits& Limits;
			FDocument& Document;
			FString& Error;
			int32 Position = 0;
			int32 Line = 1;
		};
	}

	const FString* FNode::FindAttribute(FStringView AttributeName) const
	{
		for (const TPair<FString, FString>& Attribute : Attributes)
		{
			if (FStringView(Attribute.Key).Equals(AttributeName, ESearchCase::CaseSensitive))
			{
				return &Attribute.Value;
			}
		}
		return nullptr;
	}

	int32 FDocument::FindChild(int32 Node, FStringView Name) const
	{
		if (!Nodes.IsValidIndex(Node))
		{
			return INDEX_NONE;
		}
		for (const int32 Child : Nodes[Node].Children)
		{
			if (FStringView(Nodes[Child].Name).Equals(Name, ESearchCase::CaseSensitive))
			{
				return Child;
			}
		}
		return INDEX_NONE;
	}

	TArray<int32> FDocument::GetChildren(int32 Node, FStringView Name) const
	{
		TArray<int32> Result;
		if (Nodes.IsValidIndex(Node))
		{
			for (const int32 Child : Nodes[Node].Children)
			{
				if (FStringView(Nodes[Child].Name).Equals(Name, ESearchCase::CaseSensitive))
				{
					Result.Add(Child);
				}
			}
		}
		return Result;
	}

	FString FDocument::GetPath(int32 Node) const
	{
		TArray<FString> Parts;
		for (int32 Current = Node; Nodes.IsValidIndex(Current) && Parts.Num() < 64; Current = Nodes[Current].Parent)
		{
			Parts.Insert(Nodes[Current].Name, 0);
		}
		return FString::Join(Parts, TEXT("/"));
	}

	bool Parse(FStringView Text, const FLimits& Limits, FDocument& OutDocument, FString& OutError)
	{
		FParser Parser(Text, Limits, OutDocument, OutError);
		if (!Parser.Run())
		{
			OutDocument.Nodes.Reset();
			return false;
		}
		return true;
	}
}
