// Copyright Epic Games, Inc. All Rights Reserved.

#include "Support/StageSupportReport.h"

#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "StageSupportReport"

namespace StageSupportReport
{
	namespace
	{
		const TCHAR* const CategoryNames[] = { TEXT("bug"), TEXT("crash"), TEXT("performance"), TEXT("marketplace"), TEXT("purchase"), TEXT("account"),
			TEXT("feature"), TEXT("other") };
		static_assert(UE_ARRAY_COUNT(CategoryNames) == int32(EStageSupportCategory::Count), "Every category needs a wire name");

		const TCHAR* const Redacted = TEXT("[REDACTED]");

		bool IsKeyChar(TCHAR C)
		{
			return FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-');
		}

		bool IsTokenChar(TCHAR C)
		{
			return IsKeyChar(C) || C == TEXT('.') || C == TEXT('~') || C == TEXT('+') || C == TEXT('/') || C == TEXT('=');
		}

		bool IsEmailLocalChar(TCHAR C)
		{
			return FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('_') || C == TEXT('%') || C == TEXT('+') || C == TEXT('-');
		}

		bool IsEmailDomainChar(TCHAR C)
		{
			return FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('-');
		}

		/** Replaces the run of IsValueChar characters after every Prefix (matched as Case says) with [REDACTED]; the prefix stays. */
		FString RedactAfter(const FString& Text, const TCHAR* Prefix, ESearchCase::Type Case, bool (*IsValueChar)(TCHAR), int32 MinValueLength)
		{
			const int32 PrefixLength = FCString::Strlen(Prefix);
			FString Out;
			Out.Reserve(Text.Len());
			int32 From = 0;
			for (;;)
			{
				const int32 At = Text.Find(Prefix, Case, ESearchDir::FromStart, From);
				if (At == INDEX_NONE)
				{
					break;
				}
				int32 End = At + PrefixLength;
				while (End < Text.Len() && IsValueChar(Text[End]))
				{
					++End;
				}
				Out.Append(*Text + From, At + PrefixLength - From);
				if (End - (At + PrefixLength) >= MinValueLength)
				{
					Out.Append(Redacted);
				}
				else
				{
					Out.Append(*Text + At + PrefixLength, End - (At + PrefixLength));
				}
				From = End;
			}
			Out.Append(*Text + From, Text.Len() - From);
			return Out;
		}

		FString RedactEmails(const FString& Text)
		{
			FString Out;
			Out.Reserve(Text.Len());
			int32 From = 0;
			for (int32 At = Text.Find(TEXT("@")); At != INDEX_NONE; At = Text.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
			{
				if (At < From)
				{
					continue;
				}
				int32 Start = At;
				while (Start > From && IsEmailLocalChar(Text[Start - 1]))
				{
					--Start;
				}
				int32 End = At + 1;
				while (End < Text.Len() && IsEmailDomainChar(Text[End]))
				{
					++End;
				}
				// A trailing dot ends the sentence, not the domain.
				while (End > At + 1 && Text[End - 1] == TEXT('.'))
				{
					--End;
				}
				const FString Domain = Text.Mid(At + 1, End - At - 1);
				const int32 Dot = Domain.Find(TEXT("."));
				if (Start == At || Dot <= 0 || Dot == Domain.Len() - 1)
				{
					continue;
				}
				Out.Append(*Text + From, Start - From);
				Out.Append(TEXT("[email]"));
				From = End;
			}
			Out.Append(*Text + From, Text.Len() - From);
			return Out;
		}

		/** The user's name after a user-profile folder, in either slash style, case-insensitive (Windows paths are). */
		FString RedactUserPaths(const FString& Text, const FString& UserName)
		{
			if (UserName.Len() < 2)
			{
				return Text;
			}
			FString Out = Text;
			for (const TCHAR* Folder : { TEXT("\\Users\\"), TEXT("/Users/"), TEXT("/home/") })
			{
				const FString Needle = FString(Folder) + UserName;
				for (int32 At = Out.Find(Needle, ESearchCase::IgnoreCase); At != INDEX_NONE;
					At = Out.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, At + 1))
				{
					// Only the whole folder name: "Gevorg" is someone else.
					const int32 NameStart = At + FCString::Strlen(Folder);
					const int32 NameEnd = NameStart + UserName.Len();
					if (NameEnd < Out.Len() && (FChar::IsAlnum(Out[NameEnd]) || Out[NameEnd] == TEXT('_') || Out[NameEnd] == TEXT('-') || Out[NameEnd] == TEXT('.')))
					{
						continue;
					}
					// Only the name is replaced, so the rest keeps the case it was written in.
					Out = Out.Left(NameStart) + TEXT("<user>") + Out.Mid(NameEnd);
				}
			}
			return Out;
		}

		FString Serialize(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}
	}

	const TCHAR* GetCategoryName(EStageSupportCategory Category)
	{
		return Category < EStageSupportCategory::Count ? CategoryNames[int32(Category)] : TEXT("other");
	}

	FText GetCategoryDisplayName(EStageSupportCategory Category)
	{
		switch (Category)
		{
		case EStageSupportCategory::Bug: return LOCTEXT("Bug", "Something does not work");
		case EStageSupportCategory::Crash: return LOCTEXT("Crash", "Crash or freeze");
		case EStageSupportCategory::Performance: return LOCTEXT("Performance", "Slow or stuttering");
		case EStageSupportCategory::Marketplace: return LOCTEXT("Marketplace", "Marketplace");
		case EStageSupportCategory::Purchase: return LOCTEXT("Purchase", "Payment or refund");
		case EStageSupportCategory::Account: return LOCTEXT("Account", "Account and sign-in");
		case EStageSupportCategory::Feature: return LOCTEXT("Feature", "Idea or request");
		default: return LOCTEXT("Other", "Something else");
		}
	}

	bool IsPlausibleEmail(const FString& Email)
	{
		if (Email.IsEmpty() || Email.Len() > MaxEmailLength || Email.Contains(TEXT(" ")))
		{
			return false;
		}
		int32 At = INDEX_NONE;
		if (!Email.FindChar(TEXT('@'), At) || At == 0 || Email.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1) != INDEX_NONE)
		{
			return false;
		}
		const FString Domain = Email.Mid(At + 1);
		const int32 Dot = Domain.Find(TEXT("."));
		return Dot > 0 && !Domain.EndsWith(TEXT(".")) && !Domain.Contains(TEXT(".."));
	}

	bool IsPng(TConstArrayView<uint8> Bytes)
	{
		static const uint8 Signature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		return Bytes.Num() >= int32(sizeof(Signature)) && FMemory::Memcmp(Bytes.GetData(), Signature, sizeof(Signature)) == 0;
	}

	FText Validate(const FStageSupportReport& Report)
	{
		const FString Title = Report.Title.TrimStartAndEnd();
		if (Title.Len() < MinTitleLength)
		{
			return LOCTEXT("NoTitle", "Give the report a short title.");
		}
		if (Title.Len() > MaxTitleLength)
		{
			return FText::Format(LOCTEXT("LongTitle", "Keep the title under {0} characters."), FText::AsNumber(MaxTitleLength));
		}
		if (Report.Description.TrimStartAndEnd().IsEmpty())
		{
			return LOCTEXT("NoDescription", "Describe what happened.");
		}
		for (const FString* Text : { &Report.Description, &Report.Steps, &Report.Expected, &Report.Actual })
		{
			if (Text->Len() > MaxTextLength)
			{
				return FText::Format(LOCTEXT("LongText", "Keep each answer under {0} characters."), FText::AsNumber(MaxTextLength));
			}
		}
		if (!Report.ContactEmail.IsEmpty() && !IsPlausibleEmail(Report.ContactEmail.TrimStartAndEnd()))
		{
			return LOCTEXT("BadEmail", "The e-mail address does not look right.");
		}
		if (Report.bIncludeScreenshot && (!IsPng(Report.ScreenshotPng) || Report.ScreenshotPng.Num() > MaxScreenshotBytes))
		{
			return LOCTEXT("BadScreenshot", "The screenshot could not be attached. Take it again or leave it out.");
		}
		return FText::GetEmpty();
	}

	FString Redact(const FString& Text, const FString& UserName, TConstArrayView<FString> KnownSecrets)
	{
		FString Out = Text;
		for (const FString& Secret : KnownSecrets)
		{
			// Short strings are not secrets worth hiding and would shred the text.
			if (Secret.Len() >= 8)
			{
				Out = Out.Replace(*Secret, Redacted, ESearchCase::CaseSensitive);
			}
		}
		for (const TCHAR* Prefix : { TEXT("sk-ant-"), TEXT("sk_live_"), TEXT("sk_test_"), TEXT("rk_live_"), TEXT("rk_test_"), TEXT("whsec_") })
		{
			Out = RedactAfter(Out, Prefix, ESearchCase::CaseSensitive, &IsKeyChar, 1);
		}
		Out = RedactAfter(Out, TEXT("Bearer "), ESearchCase::IgnoreCase, &IsTokenChar, 8);
		Out = RedactAfter(Out, TEXT("x-api-key: "), ESearchCase::IgnoreCase, &IsTokenChar, 8);
		Out = RedactAfter(Out, TEXT("x-api-key="), ESearchCase::IgnoreCase, &IsTokenChar, 8);
		Out = RedactEmails(Out);
		return RedactUserPaths(Out, UserName);
	}

	void RedactReport(FStageSupportReport& Report, const FString& UserName, TConstArrayView<FString> KnownSecrets)
	{
		for (FString* Text : { &Report.Title, &Report.Description, &Report.Steps, &Report.Expected, &Report.Actual, &Report.SceneSummary, &Report.LogTail })
		{
			*Text = Redact(*Text, UserName, KnownSecrets);
		}
	}

	FString TailLog(const FString& LogText, int32 MaxBytes)
	{
		if (MaxBytes <= 0)
		{
			return FString();
		}
		// Walk back from the end counting UTF-8 bytes, so the cap holds for any script.
		int32 Bytes = 0;
		int32 Start = LogText.Len();
		while (Start > 0)
		{
			const TCHAR C = LogText[Start - 1];
			const int32 Size = C < 0x80 ? 1 : (C < 0x800 ? 2 : ((C >= 0xD800 && C <= 0xDFFF) ? 2 : 3));
			if (Bytes + Size > MaxBytes)
			{
				break;
			}
			Bytes += Size;
			--Start;
		}
		if (Start == 0)
		{
			return LogText;
		}
		// Start at the next whole line, unless that would leave nothing.
		const int32 LineBreak = LogText.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		return LineBreak != INDEX_NONE && LineBreak + 1 < LogText.Len() ? LogText.Mid(LineBreak + 1) : LogText.Mid(Start);
	}

	FString ToJson(const FStageSupportReport& Report)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("clientReportId"), Report.ReportId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Body->SetStringField(TEXT("category"), GetCategoryName(Report.Category));
		Body->SetStringField(TEXT("title"), Report.Title.TrimStartAndEnd());
		Body->SetStringField(TEXT("description"), Report.Description);
		Body->SetStringField(TEXT("steps"), Report.Steps);
		Body->SetStringField(TEXT("expected"), Report.Expected);
		Body->SetStringField(TEXT("actual"), Report.Actual);
		if (!Report.ContactEmail.TrimStartAndEnd().IsEmpty())
		{
			Body->SetStringField(TEXT("contactEmail"), Report.ContactEmail.TrimStartAndEnd());
		}
		Body->SetStringField(TEXT("appVersion"), Report.AppVersion);
		Body->SetStringField(TEXT("os"), Report.Platform);
		Body->SetStringField(TEXT("gpu"), Report.Gpu);
		Body->SetStringField(TEXT("sceneStats"), Report.SceneSummary);
		if (Report.bIncludeLog && !Report.LogTail.IsEmpty())
		{
			Body->SetStringField(TEXT("logTail"), TailLog(Report.LogTail));
		}
		if (Report.bIncludeScreenshot && IsPng(Report.ScreenshotPng))
		{
			Body->SetStringField(TEXT("screenshotPngBase64"), FBase64::Encode(Report.ScreenshotPng));
		}
		return Serialize(Body);
	}

	FString ToText(const FStageSupportReport& Report)
	{
		FString Text;
		const auto Add = [&Text](const TCHAR* Label, const FString& Value)
		{
			if (!Value.TrimStartAndEnd().IsEmpty())
			{
				Text += FString::Printf(TEXT("%s:\r\n%s\r\n\r\n"), Label, *Value.TrimStartAndEnd().Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\n"), TEXT("\r\n")));
			}
		};
		Text += FString::Printf(TEXT("StageCraft support report %s\r\nCreated: %s UTC\r\nCategory: %s\r\n\r\n"),
			*Report.ReportId.ToString(EGuidFormats::DigitsWithHyphensLower), *Report.CreatedUtc.ToIso8601(), GetCategoryName(Report.Category));
		Add(TEXT("Title"), Report.Title);
		Add(TEXT("What happened"), Report.Description);
		Add(TEXT("Steps to reproduce"), Report.Steps);
		Add(TEXT("Expected"), Report.Expected);
		Add(TEXT("Actual"), Report.Actual);
		Add(TEXT("Contact"), Report.ContactEmail);
		Add(TEXT("App version"), Report.AppVersion);
		Add(TEXT("Platform"), Report.Platform);
		Add(TEXT("GPU"), Report.Gpu);
		Add(TEXT("Scene"), Report.SceneSummary);
		Text += FString::Printf(TEXT("Log attached: %s\r\nScreenshot attached: %s\r\n"), Report.bIncludeLog ? TEXT("yes") : TEXT("no"),
			Report.bIncludeScreenshot ? TEXT("yes") : TEXT("no"));
		return Text;
	}
}

#undef LOCTEXT_NAMESPACE
