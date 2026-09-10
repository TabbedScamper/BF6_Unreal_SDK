#pragma once
#include "CoreMinimal.h"

class FJsonObject;
namespace BF6BlocksExport
{
    bool Start(const TSharedRef<FJsonObject>& Files, const FString& OutputDir,
        TFunction<void(const FString&, bool)> Report, FString& Why);
    void Stop();
}
