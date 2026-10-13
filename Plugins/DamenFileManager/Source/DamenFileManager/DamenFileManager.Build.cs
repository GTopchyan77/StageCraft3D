// Copyright DamenTools internal. Damen File Manager plugin.

using UnrealBuildTool;

public class DamenFileManager : ModuleRules
{
	public DamenFileManager(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore",
			// Public (not private) on purpose: DamenFileManagerWidget.h derives from
			// UDlFmMain_Logic and its member declarations name UFileDrop, so both
			// vendor modules' public headers are part of THIS module's public surface.
			"DlFileManager",
			"FileDropUMG"
		});

		// ImportedMeshLibrary: FRTIModelStruct/FRTIMeshStruct (RealTimeImport, which embeds
		// ProceduralMeshComponent's FProcMeshTangent) are in its public header.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"RealTimeImport",
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Runtime UStaticMesh building (BuildFromMeshDescriptions).
			"MeshDescription",
			"StaticMeshDescription"
		});
	}
}
