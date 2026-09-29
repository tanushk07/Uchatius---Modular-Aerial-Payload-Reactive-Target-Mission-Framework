// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

// Dedicated log category for plugin diagnostics.
DECLARE_LOG_CATEGORY_EXTERN(LogDynamicPayload, Log, All);

class FDynamicPayloadSystemModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
