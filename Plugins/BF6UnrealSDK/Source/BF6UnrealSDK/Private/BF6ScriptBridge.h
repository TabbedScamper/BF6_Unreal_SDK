#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BF6ScriptBridge.generated.h"

// ============================================================================
// The page -> tool bridge for the script editor.
//
// Bound as window.ue.bf6script on BOTH browser windows the feature touches:
// the tool's own editor page (Resources/script/editor.html) and the Portal
// site panel (Resources/script/site_script_sync.js). One object, because the
// two halves are two ends of the same conversation: the editor asks for a
// push, the site half does it and answers, and the answer has to come back to
// the editor.
//
// EVERY FUNCTION TAKES ONE JSON STRING AND NOTHING ELSE. The JS binding
// lowercases the name, so the page calls window.ue.bf6script.call('{...}').
// Every message carries "src", which is "editor" or "site", and the tool uses
// it to decide which window an answer goes to.
//
// NOTHING SECRET CROSSES THIS SEAM. No cookie, no token, no session id, no
// password. The site half reads and writes the text in the page's editor and
// presses the page's own Save button, and that is the whole of its reach.
// ============================================================================
UCLASS()
class UBF6ScriptBridge : public UObject
{
	GENERATED_BODY()

public:
	// A page finished wiring itself up: {src, v}. The tool answers by pushing
	// whatever that page needs, which for the editor is the worker source and
	// the current status.
	UFUNCTION()
	void ready(FString Json);

	// The one request channel: {id, src, op, ...}. The reply comes back as
	// BF6ScriptEditor.reply({id, ok, ...}) on the editor page. A message from
	// the site half carries op "sitestate" or "siteresult" and needs no reply.
	UFUNCTION()
	void call(FString Json);

	// Page console output the tool should keep: {level, msg}. Kept separate
	// from call() so it can never be mistaken for a request and never blocks.
	UFUNCTION()
	void log(FString Json);
};
