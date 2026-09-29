// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "FGameLogEntry.h"
#include "MissionLogReceiver.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UMissionLogReceiver : public UInterface
{
	GENERATED_BODY()
};

/** Interface for objects that receive mission log entries. */
class DYNAMICPAYLOADSYSTEM_API IMissionLogReceiver
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Mission|Log")
	void PushGameLog(const FGameLogEntry& Entry);
};
