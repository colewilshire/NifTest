#include "Nifly.h"
#include "Modules/ModuleManager.h"

class FNifly : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("Nifly module has started!"));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("Nifly module is shutting down."));
    }
};

IMPLEMENT_MODULE(FNifly, Nifly)