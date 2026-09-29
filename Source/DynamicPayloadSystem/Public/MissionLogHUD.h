// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "FGameLogEntry.h"
#include "MissionLogReceiver.h"
#include "MissionLogHUD.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnMissionLogReceived,
	const FGameLogEntry&, Entry
);

/** HUD implementation of IMissionLogReceiver that forwards mission log entries
 *  to Blueprint. Set this class or a Blueprint child as the GameMode HUD class
 *  and bind OnMissionLog to receive entries. */
UCLASS(Blueprintable, ClassGroup = (Mission))
class DYNAMICPAYLOADSYSTEM_API AMissionLogHUD : public AHUD, public IMissionLogReceiver
{
	GENERATED_BODY()

public:
	/** Broadcast once per log entry, in order, including entries queued before the HUD was created. */
	UPROPERTY(BlueprintAssignable, Category = "Mission|Log")
	FOnMissionLogReceived OnMissionLog;

	/** Recent entries in chronological order, with the newest entry last. */
	UPROPERTY(BlueprintReadOnly, Category = "Mission|Log")
	TArray<FGameLogEntry> LogHistory;

	/** Maximum number of entries retained in LogHistory. Zero disables history. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Mission|Log",
		meta = (ClampMin = "0"))
	int32 MaxLogHistory = 64;

	/** Return the most recent message as a string. */
	UFUNCTION(BlueprintPure, Category = "Mission|Log")
	FString GetLastMessage() const;

	virtual void PushGameLog_Implementation(const FGameLogEntry& Entry) override;
};
