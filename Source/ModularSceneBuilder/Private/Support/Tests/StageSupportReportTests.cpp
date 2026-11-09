// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of support requests and bug reports (Docs/ADR/0032 §7, STATE.md #77): validation, redaction of secrets and personal data
// before anything leaves the machine, the log tail and the formats a report is sent or saved in.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Support; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Support/StageSupportReport.h"

namespace StageSupportReportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const TArray<uint8> TinyPng = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13 };

	FStageSupportReport MakeReport()
	{
		FStageSupportReport Report;
		Report.ReportId = FGuid(1, 2, 3, 4);
		Report.CreatedUtc = FDateTime(2026, 11, 7, 12, 0, 0);
		Report.Category = EStageSupportCategory::Purchase;
		Report.Title = TEXT("  Purchase stuck  ");
		Report.Description = TEXT("Paid but the stage did not download.");
		Report.Steps = TEXT("1. Buy\n2. Wait");
		Report.AppVersion = TEXT("1.0.0");
		Report.Platform = TEXT("Windows 11");
		return Report;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSupportValidateTest, "StageCraft.Support.Validate", StageSupportReportTests::Flags)

bool FStageSupportValidateTest::RunTest(const FString& Parameters)
{
	using namespace StageSupportReportTests;

	FStageSupportReport Report = MakeReport();
	TestTrue(TEXT("A complete report is valid"), StageSupportReport::Validate(Report).IsEmpty());

	FStageSupportReport Bad = Report;
	Bad.Title = TEXT(" a ");
	TestFalse(TEXT("Title too short"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad.Title = FString::ChrN(StageSupportReport::MaxTitleLength + 1, TEXT('t'));
	TestFalse(TEXT("Title too long"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad = Report;
	Bad.Description = TEXT("   ");
	TestFalse(TEXT("Description required"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad = Report;
	Bad.Actual = FString::ChrN(StageSupportReport::MaxTextLength + 1, TEXT('x'));
	TestFalse(TEXT("Answer too long"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad = Report;
	Bad.ContactEmail = TEXT("not an email");
	TestFalse(TEXT("Bad e-mail"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad.ContactEmail = TEXT("gevor@example.com");
	TestTrue(TEXT("Good e-mail"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad = Report;
	Bad.bIncludeScreenshot = true;
	TestFalse(TEXT("Screenshot asked for but missing"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad.ScreenshotPng = { 1, 2, 3 };
	TestFalse(TEXT("Screenshot that is not a PNG"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad.ScreenshotPng = TinyPng;
	TestTrue(TEXT("PNG screenshot"), StageSupportReport::Validate(Bad).IsEmpty());
	Bad.ScreenshotPng.SetNumZeroed(StageSupportReport::MaxScreenshotBytes + 1);
	FMemory::Memcpy(Bad.ScreenshotPng.GetData(), TinyPng.GetData(), TinyPng.Num());
	TestFalse(TEXT("Screenshot too large"), StageSupportReport::Validate(Bad).IsEmpty());

	for (const TCHAR* Good : { TEXT("a@b.co"), TEXT("first.last+tag@sub.example.am") })
	{
		TestTrue(FString::Printf(TEXT("Plausible: %s"), Good), StageSupportReport::IsPlausibleEmail(Good));
	}
	for (const TCHAR* Wrong : { TEXT(""), TEXT("@b.co"), TEXT("a@b"), TEXT("a@b."), TEXT("a@@b.co"), TEXT("a b@c.co"), TEXT("a@.co"), TEXT("a@b..co") })
	{
		TestFalse(FString::Printf(TEXT("Not plausible: '%s'"), Wrong), StageSupportReport::IsPlausibleEmail(Wrong));
	}
	for (int32 Index = 0; Index < int32(EStageSupportCategory::Count); ++Index)
	{
		TestFalse(TEXT("Category has a wire name"), FString(StageSupportReport::GetCategoryName(EStageSupportCategory(Index))).IsEmpty());
		TestFalse(TEXT("Category has a display name"), StageSupportReport::GetCategoryDisplayName(EStageSupportCategory(Index)).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSupportRedactTest, "StageCraft.Support.Redact", StageSupportReportTests::Flags)

bool FStageSupportRedactTest::RunTest(const FString& Parameters)
{
	const FString User = TEXT("Gevor");
	const auto Redact = [&User](const TCHAR* Text) { return StageSupportReport::Redact(Text, User); };

	TestEqual(TEXT("Anthropic key"), Redact(TEXT("key=sk-ant-api03-AbC_d-9 done")), FString(TEXT("key=sk-ant-[REDACTED] done")));
	TestEqual(TEXT("Stripe secret"), Redact(TEXT("sk_live_51HxYz and sk_test_abc")), FString(TEXT("sk_live_[REDACTED] and sk_test_[REDACTED]")));
	TestEqual(TEXT("Restricted key"), Redact(TEXT("rk_live_9z")), FString(TEXT("rk_live_[REDACTED]")));
	TestEqual(TEXT("Webhook secret"), Redact(TEXT("whsec_ABCdef123")), FString(TEXT("whsec_[REDACTED]")));
	TestEqual(TEXT("Bearer token"), Redact(TEXT("Authorization: Bearer eyJhbGciOi.J9x-_~+/= next")), FString(TEXT("Authorization: Bearer [REDACTED] next")));
	TestEqual(TEXT("bearer, any case"), Redact(TEXT("bearer abcdefgh1234")), FString(TEXT("bearer [REDACTED]")));
	TestEqual(TEXT("Short words after Bearer stay"), Redact(TEXT("Bearer of news")), FString(TEXT("Bearer of news")));
	TestEqual(TEXT("x-api-key header"), Redact(TEXT("x-api-key: abcdefgh12345678")), FString(TEXT("x-api-key: [REDACTED]")));
	TestEqual(TEXT("Publishable key is not secret"), Redact(TEXT("pk_live_123")), FString(TEXT("pk_live_123")));
	TestEqual(TEXT("E-mail"), Redact(TEXT("Contact gevor.h@example.com.")), FString(TEXT("Contact [email].")));
	TestEqual(TEXT("Two e-mails"), Redact(TEXT("a@b.co,c@d.am")), FString(TEXT("[email],[email]")));
	TestEqual(TEXT("@ alone stays"), Redact(TEXT("meet @ 5 and @handle and x@y")), FString(TEXT("meet @ 5 and @handle and x@y")));
	TestEqual(TEXT("Windows profile path"), Redact(TEXT("C:\\Users\\Gevor\\AppData\\Local")), FString(TEXT("C:\\Users\\<user>\\AppData\\Local")));
	TestEqual(TEXT("Any case"), Redact(TEXT("c:/users/GEVOR/Desktop")), FString(TEXT("c:/users/<user>/Desktop")));
	TestEqual(TEXT("Path at the end"), Redact(TEXT("in C:\\Users\\Gevor")), FString(TEXT("in C:\\Users\\<user>")));
	TestEqual(TEXT("Other users' folders stay"), Redact(TEXT("C:\\Users\\Public\\x")), FString(TEXT("C:\\Users\\Public\\x")));
	TestEqual(TEXT("Linux home"), Redact(TEXT("/home/Gevor/x")), FString(TEXT("/home/<user>/x")));
	TestEqual(TEXT("Longer name sharing a prefix stays"), Redact(TEXT("C:\\Users\\Gevorg\\x")), FString(TEXT("C:\\Users\\Gevorg\\x")));
	TestEqual(TEXT("Nothing to hide"), Redact(TEXT("Fixture 12 is dark.")), FString(TEXT("Fixture 12 is dark.")));
	TestEqual(TEXT("Empty"), Redact(TEXT("")), FString());

	const TArray<FString> Secrets = { TEXT("session-token-0123456789"), TEXT("short") };
	TestEqual(TEXT("Known secret anywhere"), StageSupportReport::Redact(TEXT("t=session-token-0123456789&x=short"), User, Secrets),
		FString(TEXT("t=[REDACTED]&x=short")));

	FStageSupportReport Report;
	Report.Title = TEXT("Key sk-ant-xyz leaked");
	Report.LogTail = TEXT("LogHttp: Bearer abcdefghijkl from C:\\Users\\Gevor\\x");
	Report.ContactEmail = TEXT("me@example.com");
	StageSupportReport::RedactReport(Report, User);
	TestEqual(TEXT("Title redacted"), Report.Title, FString(TEXT("Key sk-ant-[REDACTED] leaked")));
	TestEqual(TEXT("Log redacted"), Report.LogTail, FString(TEXT("LogHttp: Bearer [REDACTED] from C:\\Users\\<user>\\x")));
	TestEqual(TEXT("The user's own contact address is kept"), Report.ContactEmail, FString(TEXT("me@example.com")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSupportFormatTest, "StageCraft.Support.Format", StageSupportReportTests::Flags)

bool FStageSupportFormatTest::RunTest(const FString& Parameters)
{
	using namespace StageSupportReportTests;

	// The log tail: a byte cap in UTF-8, from a line boundary.
	TestEqual(TEXT("Short log is whole"), StageSupportReport::TailLog(TEXT("a\nb\n"), 100), FString(TEXT("a\nb\n")));
	TestEqual(TEXT("Cut at a line"), StageSupportReport::TailLog(TEXT("first line\nsecond\nthird\n"), 12), FString(TEXT("third\n")));
	const FString Armenian = FString(TEXT("\x0532\x0561\x0580\x0587\n")) + TEXT("ok\n");
	TestEqual(TEXT("Multi-byte characters count as their UTF-8 size"), StageSupportReport::TailLog(Armenian, 6), FString(TEXT("ok\n")));
	TestEqual(TEXT("No room"), StageSupportReport::TailLog(TEXT("abc"), 0), FString());
	TestEqual(TEXT("One long line keeps its end"), StageSupportReport::TailLog(TEXT("abcdef"), 3), FString(TEXT("def")));
	TestTrue(TEXT("Default cap"), StageSupportReport::TailLog(FString::ChrN(StageSupportReport::MaxLogBytes + 500, TEXT('x'))).Len() <= StageSupportReport::MaxLogBytes);

	// JSON: what the server reads.
	FStageSupportReport Report = MakeReport();
	TSharedPtr<FJsonObject> Json;
	TestTrue(TEXT("JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(StageSupportReport::ToJson(Report)), Json) && Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Id"), Json->GetStringField(TEXT("clientReportId")), FGuid(1, 2, 3, 4).ToString(EGuidFormats::DigitsWithHyphensLower));
	TestEqual(TEXT("Category"), Json->GetStringField(TEXT("category")), FString(TEXT("purchase")));
	TestEqual(TEXT("Title trimmed"), Json->GetStringField(TEXT("title")), FString(TEXT("Purchase stuck")));
	TestFalse(TEXT("No contact when none"), Json->HasField(TEXT("contactEmail")));
	TestFalse(TEXT("No log unless included"), Json->HasField(TEXT("logTail")));
	TestFalse(TEXT("No screenshot unless included"), Json->HasField(TEXT("screenshotPngBase64")));

	Report.LogTail = TEXT("line\n");
	Report.ScreenshotPng = TinyPng;
	TestFalse(TEXT("Still none while not ticked"), StageSupportReport::ToJson(Report).Contains(TEXT("logTail")));
	Report.bIncludeLog = true;
	Report.bIncludeScreenshot = true;
	Report.ContactEmail = TEXT(" me@example.com ");
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(StageSupportReport::ToJson(Report)), Json);
	TestEqual(TEXT("Log included"), Json->GetStringField(TEXT("logTail")), FString(TEXT("line\n")));
	TArray<uint8> Decoded;
	TestTrue(TEXT("Screenshot as base64"), FBase64::Decode(Json->GetStringField(TEXT("screenshotPngBase64")), Decoded) && Decoded == TinyPng);
	TestEqual(TEXT("Contact trimmed"), Json->GetStringField(TEXT("contactEmail")), FString(TEXT("me@example.com")));

	const FString Text = StageSupportReport::ToText(Report);
	TestTrue(TEXT("Text has the title"), Text.Contains(TEXT("Title:\r\nPurchase stuck")));
	TestTrue(TEXT("Text uses CRLF in answers"), Text.Contains(TEXT("1. Buy\r\n2. Wait")));
	TestTrue(TEXT("Text says what is attached"), Text.Contains(TEXT("Log attached: yes")));
	TestFalse(TEXT("Empty answers are left out"), Text.Contains(TEXT("Expected:")));
	return true;
}

#endif
