// Fill out your copyright notice in the Description page of Project Settings.


#include "NifTestActor.h"
//#include "NifFile.hpp"
//#include "Miniball.hpp"
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

// void ANifTestActor::LoadFile()
// {
// 	FString AbsoluteFilePath = TEXT("E:/Program Files (x86)/Steam/steamapps/common/The Guild 2 Renaissance/Objects/Animals/bull.nif");
// 	TArray<uint8> BinaryData;

// 	int64 UnixTime = FDateTime::UtcNow().ToUnixTimestamp();
// 	UE_LOG(LogTemp, Warning, TEXT("Current Unix time: %lld"), UnixTime);

// 	if (FFileHelper::LoadFileToArray(BinaryData, *AbsoluteFilePath))
// 	{
// 		UE_LOG(LogTemp, Warning, TEXT("Loaded file successfully. File size: %d bytes"), BinaryData.Num());
// 	}
// 	else
// 	{
// 		UE_LOG(LogTemp, Error, TEXT("Failed to load file: %s"), *AbsoluteFilePath);
// 	}

// 	float TestValue = 2.0f;
//     float Squared = Miniball::mb_sqr<float>(TestValue);
//     UE_LOG(LogTemp, Warning, TEXT("Nifly Test: The square of %f is %f"), TestValue, Squared);

// 	std::filesystem::path FilePathA("E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif");
// 	nifly::NifFile nifA;
// 	int a = nifA.Load(FilePathA);
// 	UE_LOG(LogTemp, Warning, TEXT("Output A is %d"), a);

// 	std::filesystem::path FilePathB("E:\\Test");
// 	nifly::NifFile nifB;
// 	int b = nifB.Load(FilePathB);
// 	UE_LOG(LogTemp, Warning, TEXT("Output B is %d"), b);
// }