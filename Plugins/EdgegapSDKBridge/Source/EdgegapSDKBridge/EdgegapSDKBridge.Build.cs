// Copyright Edgegap. All Rights Reserved.

using UnrealBuildTool;

public class EdgegapSDKBridge : ModuleRules
{
	public EdgegapSDKBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EdgegapIntegration",
				"EdgegapSDK",
				"EdgegapClient",
			});
	}
}
