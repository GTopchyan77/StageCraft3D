// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GenericQuadTree.h"
#include "OverCrowdDataAsset.h"
#include "OverCrowdManager.h"

#include "Math/GenericOctreePublic.h"
#include "Math/GenericOctree.h"

#include "CrowdInstancer.generated.h"


USTRUCT(Blueprintable)
struct FActorArray
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TArray<AActor*> Actors;
};


USTRUCT(Blueprintable)
struct FDataHISMLookUp
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<UVatMeshPairing*, FHISMData> HISMLookup;

	
};


struct FCrowdMemberOctreeElement
{
	FCrowdMemberOctreeElement() = default;

	explicit FCrowdMemberOctreeElement(FGuid InGuid, const FBoxCenterAndExtent& InBounds)
		: Guid(InGuid)
		, Bounds(InBounds)
	{
	}

	FGuid Guid;
	FBoxCenterAndExtent Bounds = {};
};

struct FCrowdMemberOctreeSemantics
{
	enum { MaxElementsPerLeaf = 32 };
	enum { MinInclusiveElementsPerNode = 7 };
	enum { MaxNodeDepth = 12 };

	typedef TInlineAllocator<MaxElementsPerLeaf> ElementAllocator;

	static FBoxCenterAndExtent GetBoundingBox(const FCrowdMemberOctreeElement& Element)
	{
		return Element.Bounds;
	}

	static const bool AreElementsEqual(const FCrowdMemberOctreeElement& Lhs, const FCrowdMemberOctreeElement& Rhs)
	{
		return Lhs.Guid == Rhs.Guid;
	}

	static void ApplyOffset(const FCrowdMemberOctreeElement& Element, const FVector& Offset)
	{
	}

	static void SetElementId(const FCrowdMemberOctreeElement& Element, FOctreeElementId2 OctreeElementID)
	{
	}
};

typedef TOctree2<FCrowdMemberOctreeElement, FCrowdMemberOctreeSemantics> FCrowdMemberOctree;

UCLASS()
class OVERCROWD_API ACrowdInstancer : public AOverCrowdManager
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ACrowdInstancer();

	

	FCrowdMemberOctree CrowdOctree;

	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	TArray<FGuid> GetGuidsInRange(FBox Extent);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void GetGuidsInsideBoxes(TArray<FBox> Boxes, TSet<FGuid> &OutGuids);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void InitOctree(const TMap<FGuid, FVector>& InLocations, FBox Extent);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void ClearAllComponents();
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void AddHISMComponent(UVatMeshPairing* VatAsset, const FWardrobe &Wardrobe);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void SetPerInstanceAnimationFrames(UInstancedStaticMeshComponent* Hism, int32 Index, int32 AnimIndexFrom, int32 AnimIndexTo, int32 AdditiveAnimIndexFrom, int32 AdditiveAnimIndexTo);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void ConfigureBodyPartInstanceOverrides(UInstancedStaticMeshComponent* Hism, int32 Index, const TArray<FMaterialParamData> &Overrides);
		 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void SetPlayrateOfInstance(UInstancedStaticMeshComponent* Hism, int32 Index, double Playrate);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void ConfigureAnimationOffset(UInstancedStaticMeshComponent* Hism, int32 Index, double TimeOffset);
	
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void SwitchAnimation(UPARAM(ref) TArray<FGuid>& CrowdMemberGuids, FGameplayTag AnimationTag, FGameplayTag AdditveAnimationTag);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void BlendAnimation(UPARAM(ref) TArray<FGuid>& CrowdMemberGuids, FGameplayTag AnimationTag, FGameplayTag AdditveAnimationTag);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void ReturnToPreviousAnimation(UPARAM(ref) TArray<FGuid>& CrowdMemberGuids);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void BlendAnimationFromPreviousState(UPARAM(ref) TArray<FGuid>& CrowdMemberGuids);


	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void UpdateInstanceOverrides(FGuid Guid, const FCrowdMemberData& CrowdMember);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void UpdateInstanceTransform(FGuid Guid, const FTransform& Transform);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void ShowHideCrowdMemeber(const FGuid& NPCGuid, bool Hide);
	 
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void AddCrowdMemberInstance(FCrowdMemberData CrowdMember);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void GetAnimationIndices(FGameplayTag WardrobeID, FGameplayTagContainer AnimationTags, int32& AnimIndex, int32& AdditiveAnimIndex, float &IncurredOffset, float &FisrtAnimDuration);

	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	FGameplayTag GetAnimationPaths(FGameplayTag WardrobeID, FGameplayTag AnimationFrom, FGameplayTag AnimationTo);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void BuildAnimationIndexLookup();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void SetLODDistanceScaleFactor(float LODScale);

	virtual bool ShouldTickIfViewportsOnly() const override;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Default")
	FRandomStream RandomStream;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	double MaxAngle;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	TObjectPtr<UVatClothingMask> ClothingMask;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Default")
	TMap<FGameplayTag, FDataHISMLookUp> HISMLookupWithWardrobeID;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	float LODDistanceScale = 1;

	// If the vertex animation moves too far from the origin then extra bounds should be added so it isn't culled too early
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	float ExtraBoundsSize = 100.0f;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	int32 NumPerInstanceCustomData = 20;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	TMap<FGuid, FCrowdMemberData> CrowdMembersData;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Default")
	TArray<FGuid> NpcGuidsToReinstate;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = "Default")
	TMap<FGuid, AActor*> NewlyAddedBodySwaps;

	//key is WardrobeID
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Default")
	TMap<FGameplayTag, FCachedAnimationIndices> CachedAnimationIndices;

};
/*
*/