#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BF6BlocksBridge.generated.h"

// ============================================================================
// The one object both block pages talk to.
//
// The engine's CEF binding lowercases the object and the function name, so
// registering this under "bf6blocks" makes it window.ue.bf6blocks.msg('<json>')
// in the tool's editor page AND in the Portal site page. Every message names
// the page it came from, which is the whole routing table: an edit made in the
// tool goes out to the site, an edit made on the site comes back to the tool.
//
// Deliberately one function taking one string. Reflection here costs a full
// UnrealHeaderTool pass, and a wide API would buy nothing that a field in the
// JSON does not.
// ============================================================================
UCLASS()
class UBF6BlocksBridge : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void Msg(const FString& Json);
};
