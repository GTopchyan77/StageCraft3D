// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Remote/StageQrCode.h"

namespace StageQrCodeTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** The long reference texts: letter 'a' + (i * 7) % 26, so no text has to be pasted here. */
	FString MakeText(int32 Length)
	{
		FString Text;
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Text.AppendChar(TEXT('a') + (Index * 7) % 26);
		}
		return Text;
	}

	struct FReference
	{
		FString Text;
		int32 Mask;
		int32 Version;
		int32 Size;
		const TCHAR* Hex;
	};

	/**
	 * Reference symbols from node-qrcode 1.5.4 (byte mode, level M, the mask forced), every one read back by the jsQR 1.4.0 decoder
	 * before it was pasted here (STATE.md #95). Modules row by row, one bit each, packed into hex.
	 */
	TArray<FReference> References()
	{
		return {
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 0, 3, 29,
		TEXT("fe2edbfc151a906e81cabb748f35dbabb9aec11ee907faaaafe00dfa00aa127094c3901a7d8933efc163ceadee0735d0f621b6eac9a06217ce9adec24218cb133967f12b1698608eaae804f8006e045ff948eb704eef1abab24fddd290e6eeb3153b0493b8afe916e58") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 1, 3, 29,
		TEXT("fefb8bfc13b0106eb49ebb742595dba6ecaec1744107faaaafe0075000a347212e693ab0e8dc66bb6bc96408bb5260fa5c8b1fbf9cf528bd64308b97174a61b993f2a47e4232ca240fbd51fd0044ac77fa1daa30444510ba671f8dd03a4c6ea6406f0439120fec43b08") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 2, 3, 29,
		TEXT("fe4d53fc11f9106eb928bb75b7d5dbad812ec150d107faaaafe01e7400be71fbe2eb739473b1d0604b5b2d2e603fd6327819560947989ab440a2e621cc26e3f0b769c9c89912586d29663cfb80603c7ff8c6eb905d6112bad1cfe5d67368eeabf6b504ab5b2fef2e060") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 3, 3, 29,
		TEXT("fecd53fc1722506e8244bb75b7d5dba037aec1260907faaaafe0151900b71c4a5aeb73945e870b0df980409e603fd66815af8d6cf143fab440a252faa190289d01a9c9c8987beeb644f0e7fe00603c7ffbab6b5050d719ba51cfe5d4a8056eb09b0304ab5b2fea98dd0") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 4, 3, 29,
		TEXT("fe8a4bfc13c1d06e9710bb75c615dba9f0aec1735d07faaaafe01968008bb6e7c9654b77d0bfe883532aea397c4e110a9b976c39a416ab335cd397e6d0556dc854cac7f07a0a29aa3e7a4dfc8043b447fa256bb04a7d13ba96df95d04b8b6e85ce5704da9c3feb5fc10") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 5, 3, 29,
		TEXT("fe7b8bfc17f1106eb928bb7570c5dba5812ec1305107faaaafe016540082f1fe732c6fe5b3b1d0606bd92508bb5260ba589b5e0947989da83165e621cc2661f897f2a47e4332da6509663cfb8051fc63f8c6eb90454110ba671f8dd07b486e8bf6b5046c475fef2e060") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 6, 3, 29,
		TEXT("fefb8bfc17c1d06eb00cbb7470c5dbaca5aec1283107faaaafe00697009fd56cbb2c6fe5ba95422873b8a318bb52608a9b976f2dd5d1bda83165c2b38506007e8fb2a47e43f1d655cdf475ff0051fc63fa8feb10535911bae71f8dd44b8b6e82d227046c475fee0a940") },
	{ TEXT("http://192.168.1.20:8080/#pin=123456"), 7, 3, 29,
		TEXT("fe2edbfc103e106e8558bb748f35dba1f0aec157cd07faaaafe009680096803d04c3901a6fc0177d88475cedee0735f4646892788084e217ce9a97e6d051ef817067f12b160a29aa38a120fa006e045ff9daaa505ca71eba324fddd7b474ae9787730493b8afeb5fc10") },
	{ TEXT("A"), 3, 1, 21,
		TEXT("fe93fc17d06e8abb75b5dba72ec12d07faafe01b00b73a5acaf233b41b6827c9e492005927fa65105836ba6f8dd79faeaac1044a5fe8480") },
	{ TEXT("ahovcjqxelszgn"), 6, 1, 21,
		TEXT("febbfc16906eb2bb7405dba9aec11d07faafe008009fd4bbe50807ca9f488b6efadd8051ebfae31050ccbab195d6312e8bb70467ffeab80") },
	{ TEXT("ahovcjqxelszgnu"), 1, 2, 25,
		TEXT("fea9bfc12f506ea0ebb74845dba5f2ec158907faaafe009700a306929ad71b6ed0423019928167d3120f4fa7ea3ff27837dafae6fd8043447fac2a304cb1bba7dfc5d04ed2ebba37043568fea6d88") },
	{ TEXT("\u0533\u0587\u0578\u0580 phone"), 4, 1, 21,
		TEXT("fee3fc11506e9abb7585dbaeaec17507faafe01c008bdfcf2e3123b84a82b2466d568073b7fb01f045bdba90a5d13fee884904294fea568") },
	{ MakeText(110), 5, 7, 45,
		TEXT("fe0289234bfc1544bfee906eb8e007b4bb7571cbfbb5dba781f9e3aec13b64684107faaaaaaaafe0130119d80082eb4f8116746bed0fbff831e8f5283ac589d58bcbddefb552da0d69f8dd23a72ed74bfc7fb0d9521a67bccfefcae9fca72452e5ac323ee5f890c8b537131acda9dd89cefbeefa37f9dc520c6ffc43aa24aa61ead71eff17bd1f3fc1ff8affdf842ff236c947d62a1d091cc9f953dee3c426d219103d8cdfd527a58d7d582f552d343d37a15aedcbb0f861489b84968bf3ec02c0b14c3d5cf3ad1b31ece9b2c0fa8bf8804df4472c67f8b26b54ab504e1716e51cba54efa32fd5d120c2be846e9076c5433b048f58152bcfedef5c56210") },
	{ MakeText(200), 2, 10, 57,
		TEXT("fe0f8bd79c7b3fc13380101502906eabccaec12fcbb753a2080f0225dbaf637bf05692ec145cab135af107faaaaaaaaaaafe016241c723f300be7cb57f1e513e34f198d3ce3ce4efbdd2817dc7fac11d36e8d25d1be07e90ab55814008ea67ab342b4ee5fa16a0c3ca38fd7b1abedfcbfd1c6aff3a011e7164d85525f3ce7ce552ae05842882ee4badb04b9ef1e3eae9cdd6158142172775fce42b4ed5cb2dad4d9f5b9c325f3c904514bd5e89fe219e70611816fbc34e3ce5efec938fefe4fedd1a0274587951eda88e22b7a14ac7c77caf102b0c74beaa15fbe92ffd63d97b5b8f7a4dc294c5b91e50d854baa853ca3cd32fde54fd9f81f4f915ce531a95a7cbbdca7155814283a52eed402b030c7aad6c7cac4b3c1a9c215b0172edf754444b1e70bd824b95d2ca3c9042888fbffbd794c20b40ff023d2ce3ea24d1a5a1428b02ceac302f463e9d549d89dc3935f039a20dc9da5d02270c7f1e72f8807b5ef18e38467f97821a88a0ead05326047cb3d1ebae18eff5a14fd5d60279d02b0352ea17c19f8f7e9904c73a3d2512acfe958b453e72f10") },
		};
	}

	FString ToHex(const StageQrCode::FCode& Code)
	{
		FString Hex;
		int32 Nibble = 0;
		int32 Bits = 0;
		for (const uint8 Module : Code.Modules)
		{
			Nibble = (Nibble << 1) | Module;
			if (++Bits == 4)
			{
				Hex.AppendChar(TEXT("0123456789abcdef")[Nibble]);
				Nibble = 0;
				Bits = 0;
			}
		}
		if (Bits > 0)
		{
			Hex.AppendChar(TEXT("0123456789abcdef")[Nibble << (4 - Bits)]);
		}
		return Hex;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageQrCodeTest, "StageCraft.WebRemote.QrCode", StageQrCodeTests::Flags)
bool FStageQrCodeTest::RunTest(const FString& Parameters)
{
	using namespace StageQrCodeTests;

	for (const FReference& Reference : References())
	{
		const FTCHARToUTF8 Utf8(*Reference.Text, Reference.Text.Len());
		const StageQrCode::FCode Code = StageQrCode::EncodeBytes(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Reference.Mask);
		const FString What = FString::Printf(TEXT("%d bytes, mask %d"), Utf8.Length(), Reference.Mask);
		TestEqual(What + TEXT(": version"), Code.Version, Reference.Version);
		TestEqual(What + TEXT(": size"), Code.Size, Reference.Size);
		TestEqual(What + TEXT(": every module matches the reference"), ToHex(Code), FString(Reference.Hex));
	}

	// The automatic mask is one of the eight valid symbols: the address's own symbol must equal the reference for that mask.
	const FString Address = TEXT("http://192.168.1.20:8080/#pin=123456");
	const StageQrCode::FCode Chosen = StageQrCode::Encode(Address);
	TestTrue(TEXT("a mask was chosen"), Chosen.IsValid() && Chosen.Mask >= 0 && Chosen.Mask < 8);
	for (const FReference& Reference : References())
	{
		if (Reference.Text == Address && Reference.Mask == Chosen.Mask)
		{
			TestEqual(TEXT("the chosen symbol is the reference for its mask"), ToHex(Chosen), FString(Reference.Hex));
		}
	}
	TestEqual(TEXT("encoding is deterministic"), ToHex(StageQrCode::Encode(Address)), ToHex(Chosen));

	TestEqual(TEXT("version 1 holds 14 bytes"), StageQrCode::GetByteCapacity(1), 14);
	TestEqual(TEXT("version 10 holds 213 bytes"), StageQrCode::GetByteCapacity(10), 213);
	TestEqual(TEXT("213 bytes fit"), StageQrCode::Encode(MakeText(213)).Version, 10);
	TestFalse(TEXT("214 bytes do not fit"), StageQrCode::Encode(MakeText(214)).IsValid());
	TestFalse(TEXT("empty text is no symbol"), StageQrCode::Encode(FString()).IsValid());
	TestFalse(TEXT("outside the symbol is light (quiet zone)"), Chosen.IsDark(-1, 0) || Chosen.IsDark(Chosen.Size, 0));
	return true;
}

#endif
