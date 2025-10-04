// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
//#include "ProceduralMeshComponent.h"
//#include "obj/NiNode.h"
#include "NifTestGameMode.generated.h"

UCLASS()
class NIFTEST_API ANifTestGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;

private:
	// virtual void TraverseNifNodes(Niflib::NiObjectRef node, int depth = 0);
	// virtual void ShowProceduralMesh(const TArray<FVector>& Positions, const TArray<int32>& Triangles, const TArray<FVector2D>& UVs);
	// bool bSpawnedMesh = false;
};
