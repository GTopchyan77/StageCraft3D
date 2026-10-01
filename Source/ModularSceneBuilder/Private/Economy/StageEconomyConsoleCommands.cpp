// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the economy, until a shop UI exists. Purchases go through the
// player controller's request bridge, exactly like a Buy button would. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Economy/StageProductData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"

namespace StageEconomyCommands
{
	UStageEconomySubsystem* GetEconomy(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageEconomySubsystem>() : nullptr;
	}

	UStageProfileSubsystem* GetProfile(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageProfileSubsystem>() : nullptr;
	}

	FString StateName(EStageOwnershipState State)
	{
		return UEnum::GetDisplayValueAsText(State).ToString();
	}

	FString PriceText(const UStageProductData& Product)
	{
		TArray<FString> Parts;
		for (const FStageCurrencyAmount& Cost : Product.Price)
		{
			Parts.Add(FString::Printf(TEXT("%lld %s"), Cost.Amount, *Cost.Currency.ToString()));
		}
		return Parts.IsEmpty() ? TEXT("free") : FString::Join(Parts, TEXT(" + "));
	}

	void ListProducts(const TArray<FString>& Args, UWorld* World)
	{
		UStageEconomySubsystem* Economy = GetEconomy(World);
		if (!Economy)
		{
			UE_LOG(LogStageCraft, Display, TEXT("No economy (start PIE first)."));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("%d products:"), Economy->GetProducts().Num());
		for (const UStageProductData* Product : Economy->GetProducts())
		{
			const FStageEconomyResultInfo Check = Economy->CanPurchase(Product);
			UE_LOG(LogStageCraft, Display, TEXT("  %s  \"%s\"  %s  grants [%s]  -> %s"), *Product->GetName(), *Product->DisplayName.ToString(),
				*PriceText(*Product), *Product->GrantedEntitlements.ToStringSimple(),
				Check.IsSuccess() ? TEXT("can buy") : *Check.Message.ToString());
		}
	}

	void Buy(const TArray<FString>& Args, UWorld* World)
	{
		UStageEconomySubsystem* Economy = GetEconomy(World);
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Economy || !Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage (in PIE): StageCraft.Shop.Buy <ProductAssetName>"));
			return;
		}
		UStageProductData* Product = Economy->FindProduct(FPrimaryAssetId(UStageProductData::ProductAssetType, FName(*Args[0])));
		const FStageEconomyResultInfo Result = Controller->RequestPurchase(Product);
		UE_LOG(LogStageCraft, Display, TEXT("Buy %s: %s %s"), *Args[0], *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageProfileSubsystem* Profile = GetProfile(World);
		if (!Profile)
		{
			UE_LOG(LogStageCraft, Display, TEXT("No profile (start PIE first)."));
			return;
		}
		const FStagePlayerProfile& Data = Profile->GetProfile();
		UE_LOG(LogStageCraft, Display, TEXT("Profile '%s' (slot %s): level %d, integrity %s, %d purchases"), *Data.DisplayName, *Profile->GetSaveSlotName(),
			Data.Level, Profile->IsIntegrityValid() ? TEXT("ok") : TEXT("FAILED"), Data.PurchaseHistory.Num());
		for (const TPair<FGameplayTag, int64>& Balance : Data.Balances)
		{
			UE_LOG(LogStageCraft, Display, TEXT("  %s: %lld"), *Balance.Key.ToString(), Balance.Value);
		}
		UE_LOG(LogStageCraft, Display, TEXT("  Entitlements: [%s]"), *Data.Entitlements.ToStringSimple());
	}

	void Reset(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageProfileSubsystem* Profile = GetProfile(World))
		{
			Profile->DevResetProfile();
			Status(Args, World);
		}
	}

	void Grant(const TArray<FString>& Args, UWorld* World)
	{
		UStageEconomySubsystem* Economy = GetEconomy(World);
		if (!Economy || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage (in PIE): StageCraft.Profile.Grant <Amount> [CurrencyTag]"));
			return;
		}
		const FGameplayTag Currency = Args.Num() > 1 ? FGameplayTag::RequestGameplayTag(FName(*Args[1]), false) : StageCraftTags::Currency_Credits;
		const bool bOk = Economy->DevGrantCurrency(Currency, FCString::Atoi64(*Args[0]));
		UE_LOG(LogStageCraft, Display, TEXT("Grant %s %s: %s"), *Args[0], *Currency.ToString(), bOk ? TEXT("ok") : TEXT("refused"));
	}

	FAutoConsoleCommandWithWorldAndArgs ListCommand(TEXT("StageCraft.Shop.List"), TEXT("Lists shop products and whether they can be bought."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ListProducts));
	FAutoConsoleCommandWithWorldAndArgs BuyCommand(TEXT("StageCraft.Shop.Buy"), TEXT("<ProductAssetName> Buys a product through the player controller."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Buy));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Profile.Status"), TEXT("Logs balances, entitlements and integrity."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ResetCommand(TEXT("StageCraft.Profile.Reset"), TEXT("Resets the profile to a new one (starting balances, nothing owned)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Reset));
	FAutoConsoleCommandWithWorldAndArgs GrantCommand(TEXT("StageCraft.Profile.Grant"), TEXT("<Amount> [CurrencyTag] Adds currency (dev only)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Grant));
}

#endif // !UE_BUILD_SHIPPING
