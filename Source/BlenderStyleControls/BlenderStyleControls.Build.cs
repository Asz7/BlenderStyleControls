using UnrealBuildTool;

public class BlenderStyleControls : ModuleRules
{
    public BlenderStyleControls(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "InputCore", "UnrealEd", "EditorFramework",
            "LevelEditor", "Slate", "SlateCore", "ToolMenus", "ApplicationCore",
            "Projects", "Json", "RenderCore", "RHI",
            "ModelingComponents", "MeshConversion", "GeometryCore", "GeometryFramework",
            "MeshDescription", "StaticMeshDescription", "AssetTools", "AssetRegistry", "PhysicsCore"
        });
    }
}
