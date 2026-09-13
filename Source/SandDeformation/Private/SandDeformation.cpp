// Copyright Matin. All Rights Reserved.

#include "SandDeformation.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSandDeformationModule"

DEFINE_LOG_CATEGORY(LogSandDeformation);

void FSandDeformationModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SandDeformation"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));

	if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/SandDeformationShaders")))
	{
		AddShaderSourceDirectoryMapping(TEXT("/SandDeformationShaders"), PluginShaderDir);
	}
}

void FSandDeformationModule::ShutdownModule()
{
	// Nothing to clean up: shader directory mappings are process-global and
	// other modules may still rely on the mapping table during shutdown.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSandDeformationModule, SandDeformation)
