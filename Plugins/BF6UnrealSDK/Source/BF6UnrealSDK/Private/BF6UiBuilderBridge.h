#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BF6UiBuilderBridge.generated.h"

// ============================================================================
// The page -> tool bridge for the UI builder.
//
// Bound as window.ue.bf6uibuilder on the builder's own page
// (Resources/uibuilder/editor.html). The engine's CEF binding lowercases the
// object and the function, so the page calls
// window.ue.bf6uibuilder.call('{...}').
//
// One function taking one JSON string, for the same reason the blocks bridge
// works that way: reflection costs a UnrealHeaderTool pass, and a wide API
// would buy nothing a field in the JSON does not already give.
//
// Nothing secret crosses this seam. The builder never signs in, never talks to
// Portal and never touches a session. It writes files under the tool's own
// Saved folder and hands text to the two editors that already exist.
// ============================================================================
UCLASS()
class UBF6UiBuilderBridge : public UObject
{
	GENERATED_BODY()

public:
	// The one request channel: {op, ...}. Replies go back to the page as
	// BF6UiBuilder.recv({op, ...}).
	UFUNCTION()
	void call(FString Json);

	// Page console output the tool should keep: {level, msg}. Separate from
	// call() so it can never be mistaken for a request.
	UFUNCTION()
	void log(FString Json);
};
