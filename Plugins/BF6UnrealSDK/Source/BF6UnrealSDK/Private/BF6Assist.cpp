#include "BF6Assist.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"

DEFINE_LOG_CATEGORY(LogBF6Assist);

namespace BF6Assist
{
namespace
{
	static const TCHAR* ENV_ENDPOINT = TEXT("BF6_AI_ENDPOINT");
	static const TCHAR* ENV_MODEL    = TEXT("BF6_AI_MODEL");
	static const TCHAR* ENV_KEY      = TEXT("BF6_AI_KEY");

	TArray<IConsoleObject*> GCommands;

	FString Env(const TCHAR* Name)
	{
		return FPlatformMisc::GetEnvironmentVariable(Name);
	}

	// A LOOPBACK ENDPOINT IS A DIFFERENT PROPOSITION.
	//
	// Ollama, LM Studio and the rest listen on 127.0.0.1 and need no key, and
	// nothing sent to them leaves the machine. That is worth telling the user
	// plainly, because "send my whole project to a model" and "send it to a
	// program already running on this computer" are not the same decision.
	// This has to be decided on the URL's actual host, not on whether the text
	// happens to contain "//localhost". A substring test called
	// https://localhost.example.invalid/ local and told the user their project
	// stayed on the machine while it was posted to someone else's server. The
	// label is a promise, so anything it cannot positively prove is remote.
	bool LooksLocal(const FString& Url)
	{
		FString Rest;
		if (Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))       { Rest = Url.Mid(7); }
		else if (Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)) { Rest = Url.Mid(8); }
		else { return false; }   // an unsupported scheme is not a local promise we can keep

		// The authority ends at the first path, query or fragment character.
		int32 Cut = Rest.Len();
		for (const TCHAR C : { TEXT('/'), TEXT('?'), TEXT('#') })
		{
			int32 At = INDEX_NONE;
			if (Rest.FindChar(C, At) && At < Cut) { Cut = At; }
		}
		FString Authority = Rest.Left(Cut);

		// Userinfo before an '@' is the classic disguise: "127.0.0.1@evil.test"
		// connects to evil.test. Refuse rather than try to be clever about it.
		if (Authority.Contains(TEXT("@"))) { return false; }

		FString Host = Authority;
		if (Host.StartsWith(TEXT("[")))
		{
			// IPv6 literal: the brackets delimit the host, a port may follow.
			int32 Close = INDEX_NONE;
			if (!Host.FindChar(TEXT(']'), Close)) { return false; }
			Host = Host.Mid(1, Close - 1);
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Host.FindChar(TEXT(':'), Colon)) { Host = Host.Left(Colon); }
		}
		Host.TrimStartAndEndInline();
		if (Host.IsEmpty()) { return false; }

		if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)) { return true; }
		if (Host.Equals(TEXT("::1")))                                { return true; }

		// The whole 127.0.0.0/8 range is loopback, not just 127.0.0.1.
		TArray<FString> Octets;
		Host.ParseIntoArray(Octets, TEXT("."), false);
		if (Octets.Num() == 4)
		{
			bool bNumeric = true;
			for (const FString& O : Octets)
			{
				if (O.IsEmpty() || !O.IsNumeric()) { bNumeric = false; break; }
			}
			if (bNumeric && FCString::Atoi(*Octets[0]) == 127) { return true; }
		}
		return false;
	}

	FString Redact(const FString& Url)
	{
		// Never print a query string: some providers put the key there.
		int32 At = INDEX_NONE;
		return Url.FindChar(TEXT('?'), At) ? Url.Left(At) + TEXT("?(redacted)") : Url;
	}
}

// ---- WHERE THE SETTINGS LIVE ------------------------------------------------
//
// Environment variables and a restart is not a setup flow. It is the way a
// programmer configures something for themselves, and the people this tool is
// for do not have a systems-properties dialog open.
//
// So the endpoint and the model are ordinary saved settings, kept in the
// editor's per-project ini like every other preference, and changeable from
// the console or the panel with no restart.
//
// THE KEY IS DELIBERATELY NOT ONE OF THEM. It is never stored by this tool,
// never written to a file we own, never logged, and never handed to a page.
// There are two ways to supply it, and neither ends with a credential sitting
// in our settings:
//
//   BF6_AI_KEY in the environment, as before; or
//   a file the user made themselves, whose PATH we remember - not its contents.
//
// A path is not a secret. The file stays theirs, they can put it wherever their
// own security tools expect, and the tool reads it at the moment of a request
// and drops it again.
static const TCHAR* kIni        = TEXT("BF6UnrealSDK.Assist");
static const TCHAR* kIniEndpoint = TEXT("Endpoint");
static const TCHAR* kIniModel    = TEXT("Model");
static const TCHAR* kIniKeyFile  = TEXT("KeyFile");

static FString Setting(const TCHAR* Name)
{
	FString V;
	if (GConfig) { GConfig->GetString(kIni, Name, V, GEditorPerProjectIni); }
	return V.TrimStartAndEnd();
}

static void SetSetting(const TCHAR* Name, const FString& Value)
{
	if (!GConfig) { return; }
	GConfig->SetString(kIni, Name, *Value, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// The key, at the moment it is needed and not before. Never returned to a
// caller that could store it: Ask uses it inside one request and lets it go.
static FString ReadKey()
{
	const FString FromEnv = Env(ENV_KEY).TrimStartAndEnd();
	if (!FromEnv.IsEmpty()) { return FromEnv; }
	const FString Path = Setting(kIniKeyFile);
	if (Path.IsEmpty()) { return FString(); }
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) { return FString(); }
	// A file somebody typed a key into ends with a newline they did not mean.
	return Text.TrimStartAndEnd();
}

FProvider CurrentProvider()
{
	FProvider P;
	// The environment still wins, so a machine already set up that way keeps
	// working and a CI box can override without touching anybody's settings.
	P.Endpoint = Env(ENV_ENDPOINT).TrimStartAndEnd();
	if (P.Endpoint.IsEmpty()) { P.Endpoint = Setting(kIniEndpoint); }
	P.Model = Env(ENV_MODEL).TrimStartAndEnd();
	if (P.Model.IsEmpty()) { P.Model = Setting(kIniModel); }
	P.KeyFile = Setting(kIniKeyFile);
	P.bHasKey = !ReadKey().IsEmpty();
	P.bIsLocal = !P.Endpoint.IsEmpty() && LooksLocal(P.Endpoint);
	return P;
}

void SetEndpoint(const FString& Url) { SetSetting(kIniEndpoint, Url.TrimStartAndEnd()); }
void SetModel(const FString& Model)  { SetSetting(kIniModel, Model.TrimStartAndEnd()); }

bool SetKeyFile(const FString& Path, FString& OutWhy)
{
	const FString P = Path.TrimStartAndEnd();
	if (P.IsEmpty())
	{
		SetSetting(kIniKeyFile, FString());
		return true;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *P))
	{
		OutWhy = FString::Printf(TEXT("that file could not be read: %s"), *P);
		return false;
	}
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		OutWhy = TEXT("that file is empty. Put the key in it, on one line, and nothing else.");
		return false;
	}
	// The path is remembered. The contents are not, and are not logged.
	SetSetting(kIniKeyFile, P);
	return true;
}

// The one place that says how to set this up, so the console, the panel and any
// error message say the same words.
FString SetupHelp()
{
	return FString(
		TEXT("To attach an AI:\n")
		TEXT("  1. BF6.Assist.Endpoint <url>   the provider's chat completions URL.\n")
		TEXT("       OpenAI      https://api.openai.com/v1/chat/completions\n")
		TEXT("       Anthropic   https://api.anthropic.com/v1/messages\n")
		TEXT("       On this PC  http://127.0.0.1:11434/v1/chat/completions   (Ollama, LM Studio)\n")
		TEXT("  2. BF6.Assist.Model <name>     for example gpt-4o-mini, or claude-sonnet-4-5.\n")
		TEXT("  3. The key, if the provider needs one. The tool never stores it:\n")
		TEXT("       put it on one line in a file of your own, then\n")
		TEXT("       BF6.Assist.KeyFile <path to that file>\n")
		TEXT("       or set BF6_AI_KEY in your environment.\n")
		TEXT("  4. BF6.Assist.Test             asks it to say hello.\n")
		TEXT("A loopback endpoint keeps everything on this machine. Anything else sends your\n")
		TEXT("briefing - which includes your script - to that provider."));
}


void Ask(const FString& Briefing, const FString& Question,
         TFunction<void(bool, const FString&)> OnDone)
{
	const FProvider P = CurrentProvider();
	if (P.Endpoint.IsEmpty())
	{
		OnDone(false, FString(TEXT("No AI is linked yet.\n\n")) + SetupHelp());
		return;
	}

	// The briefing is the system message and the question is the user turn, so
	// a provider that caches system prompts can reuse the 43 KB of rules and
	// vocabulary across a conversation instead of re-reading it every time.
	TSharedRef<FJsonObject> Sys = MakeShared<FJsonObject>();
	Sys->SetStringField(TEXT("role"), TEXT("system"));
	Sys->SetStringField(TEXT("content"), Briefing);
	TSharedRef<FJsonObject> Usr = MakeShared<FJsonObject>();
	Usr->SetStringField(TEXT("role"), TEXT("user"));
	Usr->SetStringField(TEXT("content"), Question);

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShared<FJsonValueObject>(Sys));
	Messages.Add(MakeShared<FJsonValueObject>(Usr));

	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), P.Model.IsEmpty() ? TEXT("gpt-4o-mini") : P.Model);
	Body->SetArrayField(TEXT("messages"), Messages);
	Body->SetBoolField(TEXT("stream"), false);

	FString Payload;
	// Condensed: TJsonWriterFactory defaults to pretty, which would put a tab
	// in front of every line of a 43 KB briefing for no reason.
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Payload);
	FJsonSerializer::Serialize(Body, W);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(P.Endpoint);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	const FString Key = ReadKey();
	if (!Key.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Key);
		// Anthropic's own endpoint wants its key in a different header; sending
		// both is harmless and saves the user configuring which is which.
		Req->SetHeader(TEXT("x-api-key"), Key);
		Req->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
	}
	Req->SetContentAsString(Payload);

	UE_LOG(LogBF6Assist, Display, TEXT("asking %s (%s, %d chars of briefing, %d of question)"),
		*Redact(P.Endpoint), P.Model.IsEmpty() ? TEXT("default model") : *P.Model,
		Briefing.Len(), Question.Len());

	Req->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid())
			{
				OnDone(false, TEXT("could not reach the AI endpoint"));
				return;
			}
			const int32 Code = Resp->GetResponseCode();
			const FString Text = Resp->GetContentAsString();
			if (Code < 200 || Code >= 300)
			{
				// The body can carry the provider's own explanation, which is
				// usually the useful part. It cannot contain our key.
				OnDone(false, FString::Printf(TEXT("the AI endpoint answered %d: %s"),
					Code, *Text.Left(600)));
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
			{
				OnDone(false, TEXT("the AI endpoint answered with something that is not JSON"));
				return;
			}

			// OpenAI shape: choices[0].message.content. Anthropic: content[0].text.
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (Root->TryGetArrayField(TEXT("choices"), Choices) && Choices && Choices->Num())
			{
				const TSharedPtr<FJsonObject>* First = nullptr;
				if ((*Choices)[0]->TryGetObject(First) && First)
				{
					const TSharedPtr<FJsonObject>* Msg = nullptr;
					FString Content;
					if ((*First)->TryGetObjectField(TEXT("message"), Msg) && Msg &&
						(*Msg)->TryGetStringField(TEXT("content"), Content))
					{
						OnDone(true, Content);
						return;
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (Root->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num())
			{
				const TSharedPtr<FJsonObject>* First = nullptr;
				FString Piece;
				if ((*Content)[0]->TryGetObject(First) && First &&
					(*First)->TryGetStringField(TEXT("text"), Piece))
				{
					OnDone(true, Piece);
					return;
				}
			}
			OnDone(false, TEXT("the AI answered in a shape this tool does not recognise. ")
				TEXT("It expects an OpenAI or Anthropic chat response."));
		});

	Req->ProcessRequest();
}

void HandleMessage(const TSharedPtr<FJsonObject>& Message,
                   TFunction<void(const TSharedRef<FJsonObject>&)> Reply)
{
	if (!Message.IsValid()) { return; }
	FString Op;
	Message->TryGetStringField(TEXT("op"), Op);

	if (Op == TEXT("assistStatus"))
	{
		const FProvider P = CurrentProvider();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("op"), TEXT("assistStatus"));
		O->SetBoolField(TEXT("linked"), !P.Endpoint.IsEmpty());
		O->SetBoolField(TEXT("local"), P.bIsLocal);
		O->SetBoolField(TEXT("hasKey"), P.bHasKey);
		O->SetStringField(TEXT("endpoint"), Redact(P.Endpoint));
		O->SetStringField(TEXT("model"), P.Model);
		Reply(O);
		return;
	}

	if (Op == TEXT("assistAsk"))
	{
		FString Briefing, Question, Ticket;
		Message->TryGetStringField(TEXT("briefing"), Briefing);
		Message->TryGetStringField(TEXT("question"), Question);
		Message->TryGetStringField(TEXT("ticket"), Ticket);
		if (Question.IsEmpty())
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("op"), TEXT("assistAnswer"));
			O->SetStringField(TEXT("ticket"), Ticket);
			O->SetBoolField(TEXT("ok"), false);
			O->SetStringField(TEXT("text"), TEXT("there was no question to ask"));
			Reply(O);
			return;
		}
		Ask(Briefing, Question, [Reply, Ticket](bool bOk, const FString& Text)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("op"), TEXT("assistAnswer"));
			O->SetStringField(TEXT("ticket"), Ticket);
			O->SetBoolField(TEXT("ok"), bOk);
			O->SetStringField(TEXT("text"), Text);
			Reply(O);
		});
		return;
	}
}

void Register()
{
	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.Status"),
		TEXT("Report whether an AI provider is linked, and where it points."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			const FProvider P = CurrentProvider();
			UE_LOG(LogBF6Assist, Display, TEXT("BF6 Assist"));
			UE_LOG(LogBF6Assist, Display, TEXT("  linked        : %s"),
				P.Endpoint.IsEmpty() ? TEXT("no") : TEXT("yes"));
			UE_LOG(LogBF6Assist, Display, TEXT("  endpoint      : %s"),
				P.Endpoint.IsEmpty() ? TEXT("(set BF6_AI_ENDPOINT)") : *Redact(P.Endpoint));
			UE_LOG(LogBF6Assist, Display, TEXT("  model         : %s"),
				P.Model.IsEmpty() ? TEXT("(set BF6_AI_MODEL)") : *P.Model);
			UE_LOG(LogBF6Assist, Display, TEXT("  key           : %s"),
				P.bHasKey ? TEXT("present (never stored, logged or sent to a page)")
				          : TEXT("none set"));
			UE_LOG(LogBF6Assist, Display, TEXT("  where it goes : %s"),
				P.Endpoint.IsEmpty() ? TEXT("nowhere")
				: (P.bIsLocal ? TEXT("this machine only (loopback)")
				              : TEXT("OFF THIS MACHINE - the briefing includes your project")));
			if (!P.KeyFile.IsEmpty())
			{
				UE_LOG(LogBF6Assist, Display, TEXT("  key file      : %s%s"), *P.KeyFile,
					P.bHasKey ? TEXT("") : TEXT("   (could not be read)"));
			}
			if (P.Endpoint.IsEmpty())
			{
				UE_LOG(LogBF6Assist, Display, TEXT(""));
				UE_LOG(LogBF6Assist, Display, TEXT("%s"), *SetupHelp());
			}
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.Setup"),
		TEXT("How to attach an AI to this editor."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6Assist, Display, TEXT("%s"), *SetupHelp());
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.Endpoint"),
		TEXT("BF6.Assist.Endpoint <url>   Where questions are sent. Saved with the project settings."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				const FProvider P = CurrentProvider();
				UE_LOG(LogBF6Assist, Display, TEXT("endpoint: %s"),
					P.Endpoint.IsEmpty() ? TEXT("(none)") : *Redact(P.Endpoint));
				return;
			}
			SetEndpoint(Args[0]);
			const FProvider P = CurrentProvider();
			UE_LOG(LogBF6Assist, Display, TEXT("endpoint set to %s (%s)"), *Redact(P.Endpoint),
				P.bIsLocal ? TEXT("this machine only")
				           : TEXT("OFF THIS MACHINE - your script goes with each question"));
			if (!P.bHasKey && !P.bIsLocal)
			{
				UE_LOG(LogBF6Assist, Display,
					TEXT("No key yet. Put it on one line in a file of your own, then: BF6.Assist.KeyFile <path>"));
			}
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.Model"),
		TEXT("BF6.Assist.Model <name>   Which model to ask. Saved with the project settings."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogBF6Assist, Display, TEXT("model: %s"),
					CurrentProvider().Model.IsEmpty() ? TEXT("(none set)") : *CurrentProvider().Model);
				return;
			}
			SetModel(Args[0]);
			UE_LOG(LogBF6Assist, Display, TEXT("model set to %s"), *Args[0]);
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.KeyFile"),
		TEXT("BF6.Assist.KeyFile <path>   A file of YOURS holding the key on one line. ")
		TEXT("The tool remembers the path, never the key: it is read for one request and dropped. ")
		TEXT("No argument clears it."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Why;
			if (Args.Num() < 1)
			{
				SetKeyFile(FString(), Why);
				UE_LOG(LogBF6Assist, Display, TEXT("key file cleared. The environment variable BF6_AI_KEY still applies if it is set."));
				return;
			}
			// Joined, because a path with spaces arrives as several arguments
			// and quoting it in the Unreal console is not obvious.
			const FString Path = FString::Join(Args, TEXT(" ")).TrimQuotes();
			if (!SetKeyFile(Path, Why))
			{
				UE_LOG(LogBF6Assist, Warning, TEXT("%s"), *Why);
				return;
			}
			UE_LOG(LogBF6Assist, Display,
				TEXT("key file set. Its contents are read at the moment of a request and never stored, ")
				TEXT("logged, or given to a page."));
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Assist.Test"),
		TEXT("Send a one line question to the linked AI and log the answer."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Q = Args.Num() ? FString::Join(Args, TEXT(" "))
				: TEXT("Reply with the single word: ready.");
			Ask(TEXT("You are attached to the BF6 Unreal SDK. Answer in one short line."), Q,
				[](bool bOk, const FString& Text)
				{
					if (bOk) { UE_LOG(LogBF6Assist, Display, TEXT("answer: %s"), *Text.Left(2000)); }
					else     { UE_LOG(LogBF6Assist, Warning, TEXT("%s"), *Text.Left(2000)); }
				});
		}),
		ECVF_Default));
}

void Unregister()
{
	for (IConsoleObject* C : GCommands)
	{
		if (C) { IConsoleManager::Get().UnregisterConsoleObject(C); }
	}
	GCommands.Empty();
}

}
