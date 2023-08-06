/*
 *  Copyright (C) 2021 mod.io Pty Ltd. <https://mod.io>
 *
 *  This file is part of the mod.io SDK.
 *
 *  Distributed under the MIT License. (See accompanying file LICENSE or
 *   view online at <https://github.com/modio/modio-sdk/blob/main/LICENSE>)
 *
 */

#pragma comment(lib, "ws2_32.lib")

#include "common/HttpSharedState.h"
#include "modio/core/ModioLogger.h"
#include "modio/detail/AsioWrapper.h"
#include "modio/detail/ModioProfiling.h"
#include <winhttp.h>

static void __stdcall ModioWinhttpStatusCallback(HINTERNET InternetHandle, DWORD_PTR Context, DWORD InternetStatus,
	LPVOID StatusInformation, DWORD StatusInformationLength)
{
	MODIO_PROFILE_SCOPE(WinhttpCallback);

	// There's only ever a single SharedStateHolder for the entire life of the program
	std::shared_ptr<HttpSharedStateBase> SharedState = SharedStateHolder::Get().SharedStatePtr.lock();

	if (SharedState)
	{
		WinHTTPCallbackStatus StatusCode = static_cast<WinHTTPCallbackStatus>(InternetStatus);
		if (StatusCode == WinHTTPCallbackStatus::DataAvailable)
		{
			std::uint64_t Value = *(DWORD*)StatusInformation;
			SharedState->SetHandleStatus(InternetHandle, StatusCode, (void*)Value, StatusInformationLength);
		}
		else
		{
			if (StatusCode == WinHTTPCallbackStatus::RequestError)
			{
				WINHTTP_ASYNC_RESULT* Result = static_cast<WINHTTP_ASYNC_RESULT*>(StatusInformation);
				Modio::Detail::Logger().Log(Modio::LogLevel::Warning, Modio::LogCategory::Http,
					"Function {:x} returned error code {:x}\r\n",
					(unsigned long)Result->dwResult, (unsigned long)Result->dwError);
			}

			SharedState->SetHandleStatus(InternetHandle, StatusCode, StatusInformation, StatusInformationLength);
		}
	}

}
