// Fill out your copyright notice in the Description page of Project Settings.

#include "NifTestActor.h"
#include "niflib.h"

// Sets default values
ANifTestActor::ANifTestActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void ANifTestActor::BeginPlay()
{
	Super::BeginPlay();

	//this->LoadFile();
	this->NiflibTest();
}

// Called every frame
void ANifTestActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ANifTestActor::NiflibTest()
{
	// Example absolute path - change to your test file
    FString NifPath = TEXT("E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif");
    std::string NifFilePath = TCHAR_TO_UTF8(*NifPath);

    unsigned int Version = Niflib::GetNifVersion(NifFilePath);
    std::string VerString = Niflib::FormatVersionString(Version);

    UE_LOG(LogTemp, Warning, TEXT("NIF Version: %s (raw: %u)"), *FString(VerString.c_str()), Version);

    if (Version == 0xFFFFFFFE) {
        UE_LOG(LogTemp, Error, TEXT("Failed to open NIF file: %s"), *NifPath);
    }
}
