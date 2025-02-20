// Author: Lucas Vilas-Boas
// Year: 2022
// Repo: https://github.com/lucoiso/UEProject_Elementus

#pragma once

#include "CoreMinimal.h"
#include "ElementusInventoryPackage.h"
#include "Actors/Interfaces/PEInteractable.h"
#include "PEInventoryPackage.generated.h"

/**
 * 
 */
UCLASS()
class PROJECTELEMENTUS_API APEInventoryPackage final : public AElementusInventoryPackage, public IPEInteractable
{
	GENERATED_BODY()

public:
	explicit APEInventoryPackage(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditDefaultsOnly, Category = "Project Elementus | Properties")
	TObjectPtr<UStaticMeshComponent> PackageMesh;

	UPROPERTY(EditDefaultsOnly, Category = "Project Elementus | Properties")
	TSoftClassPtr<UUserWidget> TradeWidgetClass;

protected:
	virtual void Tick(float DeltaSeconds) override;

	virtual bool IsInteractEnabled_Implementation() const override;
	virtual void
	DoInteractionBehavior_Implementation(APECharacter* CharacterInteracting, const FHitResult& HitResult) override;
};
