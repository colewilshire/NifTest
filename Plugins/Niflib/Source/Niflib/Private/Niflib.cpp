#include "Niflib.h"
#include "Modules/ModuleManager.h"

class FNiflib : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("Niflib module has started!"));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("Niflib module is shutting down."));
    }
};

IMPLEMENT_MODULE(FNiflib, Niflib)