/*
 *  Copyright (C) 2021-2023 mod.io Pty Ltd. <https://mod.io>
 *
 *  This file is part of the mod.io UE4 Plugin.
 *
 *  Distributed under the MIT License. (See accompanying file LICENSE or
 *   view online at <https://github.com/modio/modio-ue4/blob/main/LICENSE>)
 *
 */

#pragma once

#include "Internal/ModioConvert.h"
#include "ModioSDK.h"
#include "Types/ModioGameStats.h"

FORCEINLINE FModioGameStats ToUnreal(const Modio::GameStats& In)
{
	FModioGameStats Out;

	Out.GameID = ToUnreal(In.GameID);
	Out.ModCountTotal = ToUnreal(In.ModCountTotal);
	Out.ModDownloadsToday = ToUnreal(In.ModDownloadsToday);
	Out.ModDownloadsTotal = ToUnreal(In.ModDownloadsTotal);
	Out.ModDownloadsDailyAverage = ToUnreal(In.ModDownloadsDailyAverage);
	Out.ModSubscribersTotal = ToUnreal(In.ModSubscribersTotal);
	Out.DateExpires = ToUnreal(In.DateExpires);

	return Out;
}