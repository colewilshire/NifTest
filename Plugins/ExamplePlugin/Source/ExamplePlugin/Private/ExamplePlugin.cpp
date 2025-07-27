#include "ExamplePlugin.h"
#include "Modules/ModuleManager.h"

class FExamplePluginModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("ExamplePlugin module has started!"));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogTemp, Warning, TEXT("ExamplePlugin module is shutting down."));
    }
};

IMPLEMENT_MODULE(FExamplePluginModule, ExamplePlugin)