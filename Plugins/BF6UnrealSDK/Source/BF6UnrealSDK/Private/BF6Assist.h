#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Dom/JsonObject.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBF6Assist, Log, All);

// ============================================================================
// BF6 Assist: the seam where the user's own AI attaches.
//
// The tool does not ship a model and does not choose one. It builds the
// briefing (Resources/assist/contextpack.js: the rules, every block signature,
// every event, the project state and the question) and, if the user has linked
// a provider, posts it and hands the answer back to whichever editor asked.
//
// WHAT IT NEVER DOES WITH THE KEY.
//
// The key is read from the environment at the moment of the request and is
// never stored by us, never written to a file under Saved, never logged, and
// never handed to a page. The editor pages are file:// documents; a key that
// reaches one has effectively been published. So the page sends a question and
// receives an answer, and the credential exists only inside this process for
// the length of one call.
//
// Nothing here applies an edit. An answer is a proposal: the caller compiles
// it, type-checks it, and shows a diff. A model that silently rewrites a rule
// in an editor where a disabled block ran for months is a bad trade.
// ============================================================================
namespace BF6Assist
{
	// Console commands. Registered from the module's startup.
	void Register();
	void Unregister();

	// Is a provider linked, and where does it point. Never includes the key.
	struct FProvider
	{
		FString Endpoint;   // OpenAI-compatible /chat/completions URL
		FString Model;
		FString KeyFile;    // the PATH the user pointed us at, never its contents
		bool bHasKey = false;
		bool bIsLocal = false;   // loopback: no key needed, nothing leaves the machine
	};
	FProvider CurrentProvider();

	// Setting it up without an environment variable and a restart. The endpoint
	// and model are ordinary saved settings; the key is not saved by us at all,
	// only the path to a file the user made and can move or delete themselves.
	void SetEndpoint(const FString& Url);
	void SetModel(const FString& Model);
	bool SetKeyFile(const FString& Path, FString& OutWhy);

	// The one wording of the setup steps, so the console, the panel and every
	// error message say the same thing.
	FString SetupHelp();

	// Ask, asynchronously. OnDone receives (bOk, TextOrError).
	void Ask(const FString& Briefing, const FString& Question,
	         TFunction<void(bool, const FString&)> OnDone);

	// A page asked. Ops: "assistStatus", "assistAsk".
	// Reply is delivered through the same channel the caller names.
	void HandleMessage(const TSharedPtr<FJsonObject>& Message,
	                   TFunction<void(const TSharedRef<FJsonObject>&)> Reply);
}
