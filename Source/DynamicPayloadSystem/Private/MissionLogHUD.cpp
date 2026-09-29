// Copyright Tanushk Nirmal 2026 All Rights Reserved.

#include "MissionLogHUD.h"

void AMissionLogHUD::PushGameLog_Implementation(const FGameLogEntry& Entry)
{
	if (MaxLogHistory > 0)
	{
		LogHistory.Add(Entry);

		// Remove the oldest entries to keep history bounded.
		const int32 Excess = LogHistory.Num() - MaxLogHistory;
		if (Excess > 0)
		{
			LogHistory.RemoveAt(0, Excess);
		}
	}

	OnMissionLog.Broadcast(Entry);
}

FString AMissionLogHUD::GetLastMessage() const
{
	return LogHistory.Num() > 0 ? LogHistory.Last().Message.ToString() : FString();
}
