#include "BF6BlocksExport.h"
#include "BF6Script.h"
#include "BF6SDKExtension.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace BF6BlocksExport
{
namespace
{
    FProcHandle Process;
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    FString JobDir, Tail;
    FTSTicker::FDelegateHandle TickHandle;
    TFunction<void(const FString&, bool)> Reporter;

    bool Tick(float)
    {
        const FString Fresh=FPlatformProcess::ReadPipe(ReadPipe);
        if (!Fresh.IsEmpty())
        {
            Tail=(Tail+Fresh).Right(16000);
            UE_LOG(LogTemp, Display, TEXT("Portal export: %s"), *Fresh);
            if (Reporter) Reporter(Fresh.Right(1500), false);
        }
        if (FPlatformProcess::IsProcRunning(Process)) return true;
        Tail=(Tail+FPlatformProcess::ReadPipe(ReadPipe)).Right(16000);
        int32 Code=-1; FPlatformProcess::GetProcReturnCode(Process,&Code);
        FPlatformProcess::CloseProc(Process); Process.Reset();
        FPlatformProcess::ClosePipe(ReadPipe,WritePipe); ReadPipe=WritePipe=nullptr;
        TickHandle.Reset();
        FString Text; TSharedPtr<FJsonObject> Result;
        bool Ok=false;
        if (FFileHelper::LoadFileToString(Text,*(JobDir/TEXT("result.json"))))
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Result);
        if (Result.IsValid()) Result->TryGetBoolField(TEXT("ok"),Ok);
        FString Message, Output;
        if (Code==0 && Ok && Result->TryGetStringField(TEXT("outputDir"),Output) &&
            FPaths::FileExists(Output/TEXT("bundle.ts")) && FPaths::FileExists(Output/TEXT("bundle.strings.json")))
        {
            Message=TEXT("Portal export ready. Upload bundle.ts and bundle.strings.json from ")+Output;
            FPlatformProcess::ExploreFolder(*Output);
        }
        else
        {
            Ok=false;
            FString Error; if (Result.IsValid()) Result->TryGetStringField(TEXT("error"),Error);
            Message=TEXT("Portal export failed. No new upload is ready. ")+(Error.IsEmpty()?Tail.Right(4000):Error);
        }
        if (Reporter) Reporter(Message,!Ok);
        BF6Ext::Notify(Message.Left(1400));
        Reporter=nullptr;
        return false;
    }
}

bool Start(const TSharedRef<FJsonObject>& Files,const FString& OutputDir,
    TFunction<void(const FString&,bool)> Report,FString& Why)
{
    if (Process.IsValid()) { Why=TEXT("A Portal export is already running. Wait for it to finish."); return false; }
    const FString Node=BF6Script::NodeExecutable();
    if (Node.IsEmpty()) { Why=TEXT("Export for Portal needs Node.js 24 or newer. Install it from nodejs.org, then retry. Your blocks are still in the editor."); return false; }
    const FString Script=FPaths::ConvertRelativePathToFull(BF6Ext::ToolPluginDir()/TEXT("Resources/script/portal-export.cjs"));
    if (!FPaths::FileExists(Script)) { Why=TEXT("The Portal export tool is missing. Reinstall the SDK update."); return false; }
    const FString Saved=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("BF6UnrealSDK"));
    JobDir=Saved/TEXT("portal-exports")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (!IFileManager::Get().MakeDirectory(*JobDir,true)) { Why=TEXT("Cannot create the export working folder: ")+JobDir; return false; }
    auto Job=MakeShared<FJsonObject>();
    Job->SetObjectField(TEXT("files"),Files);
    Job->SetStringField(TEXT("outputDir"),FPaths::ConvertRelativePathToFull(OutputDir));
    Job->SetStringField(TEXT("toolchainDir"),Saved/TEXT("portal-build-tools"));
    Job->SetStringField(TEXT("npmCli"),FPaths::GetPath(Node)/TEXT("node_modules/npm/bin/npm-cli.js"));
    FString Json; FJsonSerializer::Serialize(Job,TJsonWriterFactory<>::Create(&Json));
    const FString Request=JobDir/TEXT("request.json");
    if (!FFileHelper::SaveStringToFile(Json,*Request,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { Why=TEXT("Cannot write the export request."); return false; }
    if (!FPlatformProcess::CreatePipe(ReadPipe,WritePipe)) { Why=TEXT("Cannot capture the export log."); return false; }
    const FString Args=FString::Printf(TEXT("\"%s\" \"%s\""),*Script,*Request);
    Process=FPlatformProcess::CreateProc(*Node,*Args,false,true,true,nullptr,0,*JobDir,WritePipe,nullptr);
    if (!Process.IsValid())
    { FPlatformProcess::ClosePipe(ReadPipe,WritePipe); ReadPipe=WritePipe=nullptr; Why=TEXT("Cannot start Node.js for the export."); return false; }
    Tail.Reset(); Reporter=MoveTemp(Report);
    TickHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick),.1f);
    if (Reporter) Reporter(TEXT("Preparing your Portal export. First use installs the build tools; progress appears here."),false);
    return true;
}

void Stop()
{
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    if (Process.IsValid()) { FPlatformProcess::TerminateProc(Process,true); FPlatformProcess::CloseProc(Process); Process.Reset(); }
    if (ReadPipe || WritePipe) FPlatformProcess::ClosePipe(ReadPipe,WritePipe);
    ReadPipe=WritePipe=nullptr; Reporter=nullptr;
}
}
