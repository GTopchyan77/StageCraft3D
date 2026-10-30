// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageFixtureCells.h"

#include "NativeGameplayTags.h"

namespace StageFixtureCells
{
	namespace
	{
		// Every cell's controls as native tags. A macro per cell keeps the 32 x 7 definitions in one readable table; the
		// engine macro ends with its own semicolon.
#define STAGECRAFT_CELL_TAGS(N) \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##Dimmer,	"StageCraft.Attribute.Cell." #N ".Dimmer") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorR,	"StageCraft.Attribute.Cell." #N ".ColorR") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorG,	"StageCraft.Attribute.Cell." #N ".ColorG") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorB,	"StageCraft.Attribute.Cell." #N ".ColorB") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorW,	"StageCraft.Attribute.Cell." #N ".ColorW") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorA,	"StageCraft.Attribute.Cell." #N ".ColorA") \
		UE_DEFINE_GAMEPLAY_TAG_STATIC(Cell##N##ColorRGB,	"StageCraft.Attribute.Cell." #N ".ColorRGB")

		STAGECRAFT_CELL_TAGS(01)
		STAGECRAFT_CELL_TAGS(02)
		STAGECRAFT_CELL_TAGS(03)
		STAGECRAFT_CELL_TAGS(04)
		STAGECRAFT_CELL_TAGS(05)
		STAGECRAFT_CELL_TAGS(06)
		STAGECRAFT_CELL_TAGS(07)
		STAGECRAFT_CELL_TAGS(08)
		STAGECRAFT_CELL_TAGS(09)
		STAGECRAFT_CELL_TAGS(10)
		STAGECRAFT_CELL_TAGS(11)
		STAGECRAFT_CELL_TAGS(12)
		STAGECRAFT_CELL_TAGS(13)
		STAGECRAFT_CELL_TAGS(14)
		STAGECRAFT_CELL_TAGS(15)
		STAGECRAFT_CELL_TAGS(16)
		STAGECRAFT_CELL_TAGS(17)
		STAGECRAFT_CELL_TAGS(18)
		STAGECRAFT_CELL_TAGS(19)
		STAGECRAFT_CELL_TAGS(20)
		STAGECRAFT_CELL_TAGS(21)
		STAGECRAFT_CELL_TAGS(22)
		STAGECRAFT_CELL_TAGS(23)
		STAGECRAFT_CELL_TAGS(24)
		STAGECRAFT_CELL_TAGS(25)
		STAGECRAFT_CELL_TAGS(26)
		STAGECRAFT_CELL_TAGS(27)
		STAGECRAFT_CELL_TAGS(28)
		STAGECRAFT_CELL_TAGS(29)
		STAGECRAFT_CELL_TAGS(30)
		STAGECRAFT_CELL_TAGS(31)
		STAGECRAFT_CELL_TAGS(32)

#undef STAGECRAFT_CELL_TAGS

#define STAGECRAFT_CELL_ROW(N) { &Cell##N##Dimmer, &Cell##N##ColorR, &Cell##N##ColorG, &Cell##N##ColorB, &Cell##N##ColorW, &Cell##N##ColorA, &Cell##N##ColorRGB },

		const FNativeGameplayTag* const CellTags[MaxCells][static_cast<int32>(EStageCellChannel::Count)] = {
			STAGECRAFT_CELL_ROW(01)
			STAGECRAFT_CELL_ROW(02)
			STAGECRAFT_CELL_ROW(03)
			STAGECRAFT_CELL_ROW(04)
			STAGECRAFT_CELL_ROW(05)
			STAGECRAFT_CELL_ROW(06)
			STAGECRAFT_CELL_ROW(07)
			STAGECRAFT_CELL_ROW(08)
			STAGECRAFT_CELL_ROW(09)
			STAGECRAFT_CELL_ROW(10)
			STAGECRAFT_CELL_ROW(11)
			STAGECRAFT_CELL_ROW(12)
			STAGECRAFT_CELL_ROW(13)
			STAGECRAFT_CELL_ROW(14)
			STAGECRAFT_CELL_ROW(15)
			STAGECRAFT_CELL_ROW(16)
			STAGECRAFT_CELL_ROW(17)
			STAGECRAFT_CELL_ROW(18)
			STAGECRAFT_CELL_ROW(19)
			STAGECRAFT_CELL_ROW(20)
			STAGECRAFT_CELL_ROW(21)
			STAGECRAFT_CELL_ROW(22)
			STAGECRAFT_CELL_ROW(23)
			STAGECRAFT_CELL_ROW(24)
			STAGECRAFT_CELL_ROW(25)
			STAGECRAFT_CELL_ROW(26)
			STAGECRAFT_CELL_ROW(27)
			STAGECRAFT_CELL_ROW(28)
			STAGECRAFT_CELL_ROW(29)
			STAGECRAFT_CELL_ROW(30)
			STAGECRAFT_CELL_ROW(31)
			STAGECRAFT_CELL_ROW(32)
		};

#undef STAGECRAFT_CELL_ROW

		struct FCellAddress
		{
			int32 CellIndex = INDEX_NONE;
			EStageCellChannel Channel = EStageCellChannel::Count;
		};

		/** Built on first use, after the tag manager registered the native tags. Game-thread reads only. */
		const TMap<FGameplayTag, FCellAddress>& GetCellLookup()
		{
			static const TMap<FGameplayTag, FCellAddress> Lookup = []
			{
				TMap<FGameplayTag, FCellAddress> Result;
				Result.Reserve(MaxCells * static_cast<int32>(EStageCellChannel::Count));
				for (int32 Cell = 0; Cell < MaxCells; ++Cell)
				{
					for (int32 Channel = 0; Channel < static_cast<int32>(EStageCellChannel::Count); ++Channel)
					{
						Result.Add(CellTags[Cell][Channel]->GetTag(), { Cell, static_cast<EStageCellChannel>(Channel) });
					}
				}
				return Result;
			}();
			return Lookup;
		}
	}

	FGameplayTag GetCellAttribute(int32 CellIndex, EStageCellChannel Channel)
	{
		if (CellIndex < 0 || CellIndex >= MaxCells || Channel >= EStageCellChannel::Count)
		{
			return FGameplayTag();
		}
		return CellTags[CellIndex][static_cast<int32>(Channel)]->GetTag();
	}

	bool ParseCellAttribute(const FGameplayTag& Attribute, int32& OutCellIndex, EStageCellChannel& OutChannel)
	{
		const FCellAddress* Address = Attribute.IsValid() ? GetCellLookup().Find(Attribute) : nullptr;
		if (!Address)
		{
			return false;
		}
		OutCellIndex = Address->CellIndex;
		OutChannel = Address->Channel;
		return true;
	}

	bool IsCellAttribute(const FGameplayTag& Attribute)
	{
		int32 Cell = INDEX_NONE;
		EStageCellChannel Channel = EStageCellChannel::Count;
		return ParseCellAttribute(Attribute, Cell, Channel);
	}
}
