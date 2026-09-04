using System.IO;
using UnrealBuildTool;

public class BF6UnrealSDK : ModuleRules
{
	public BF6UnrealSDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",                 // IPluginManager, to find the plugin's folder
			"ProceduralMeshComponent",  // build the decoded geometry into a mesh
			"Json",                     // save/load session files
			"JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",   // the Window > Tools menu group
			"ImageWrapper",             // decode the map thumbnail images for Slate
			"HTTP"                      // update check + download from GitHub releases
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",               // GEditor world
				"MaterialEditor",         // the runtime-built volume colour material
				"AppFramework",           // SColorPicker: the hue wheel, hex field and alpha
				"ToolMenus",              // the actor context menu, in the tree and the viewport
				"AdvancedPreviewScene",   // lit preview scene for the 3D model viewport
				"MainFrame",              // auto-open the map picker once the editor is ready
				"DesktopPlatform",        // native open-file dialog for importing .spatial.json
				"LevelEditor",             // viewport overlay (Build Mode chrome) + active viewport
				"SceneOutliner"        // the SDK-styled scene tree
			});
		}

		// The engine-neutral decode core. We only need its C header to compile
		// against; the .dll itself is loaded at runtime, so no static linking and
		// no toolchain-version fight with whatever built the core.
		string ThirdParty = Path.Combine(ModuleDirectory, "..", "ThirdParty", "libbf6");
		PublicIncludePaths.Add(Path.Combine(ThirdParty, "include"));

		// Stage bf6_core.dll in the plugin's own binary directory. Runtime loads
		// this exact path first; Source/ThirdParty is only a development-package
		// input and is not present in an installed/packaged plugin.
		string DllName = "bf6_core.dll";
		string DllSrc = Path.Combine(ThirdParty, "bin", "Win64", DllName);
		RuntimeDependencies.Add(Path.Combine("$(PluginDir)", "Binaries", "Win64", DllName), DllSrc);

		// Portal-SDK catalogues and converted meshes are generated into the
		// project's Saved/BF6UnrealSDK/sdkdata directory at runtime. They are not
		// build inputs and must not be staged out of Source/ThirdParty. Static map
		// thumbnails and gameplay marker meshes live in the plugin's Resources/
		// directory, which the plugin packager already carries as plugin content.
	}
}
