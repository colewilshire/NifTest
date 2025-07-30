// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "obj/NiNode.h"
#include "NifTestGameMode.generated.h"

UCLASS()
class NIFTEST_API ANifTestGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;

private:
	//virtual void LoadFile();
	virtual void PrintVersion();
	virtual void PrintNifObjectType();
	virtual void TraverseNifNodes(Niflib::NiObjectRef node, int depth = 0);
};
