// Copyright Matin. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"

SANDDEFORMATION_API DECLARE_LOG_CATEGORY_EXTERN(LogSandDeformation, Log, All);

/**
 * Module entry point. All it does is map the plugin's Shaders/ folder onto
 * the virtual "/SandDeformationShaders" path so SandDeformation.usf can be
 * found by the shader compiler - see SandDeformationComputePass.cpp for
 * where that virtual path is actually used.
 */
class FSandDeformationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
