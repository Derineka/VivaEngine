/*
 *  Copyright (C) 2021 mod.io Pty Ltd. <https://mod.io>
 *
 *  This file is part of the mod.io SDK.
 *
 *  Distributed under the MIT License. (See accompanying file LICENSE or
 *   view online at <https://github.com/modio/modio-sdk/blob/main/LICENSE>)
 *
 */

#pragma once
#include "common/HttpSharedState.h"
#include "common/UTF16Support.h"
#include "common/http/HttpRequestImplementation.h"
#include "modio/core/ModioLogger.h"
#include "modio/detail/AsioWrapper.h"
#include "modio/detail/FmtWrapper.h"
#include "modio/detail/ModioConstants.h"
#include "modio/detail/ModioProfiling.h"
#include "modio/timer/ModioTimer.h"
#include <memory>

#include <asio/yield.hpp>

class ReadHttpResponseHeadersOp
{
	std::weak_ptr<HttpSharedStateBase> SharedState;
	std::shared_ptr<HttpRequestImplementation> Request;
	asio::coroutine CoroutineState;
	Modio::Detail::Timer Timer;

public:
	ReadHttpResponseHeadersOp(std::shared_ptr<HttpRequestImplementation> Request,
							  std::weak_ptr<HttpSharedStateBase> SharedState)
		: SharedState(SharedState),
		  Request(Request)
	{}

	template<typename CoroType>
	void operator()(CoroType& Self, Modio::ErrorCode ec = {})
	{
		MODIO_PROFILE_SCOPE(ReadHttpResponseHeaders);

		std::shared_ptr<HttpSharedStateBase> PinnedState = SharedState.lock();
		if (PinnedState == nullptr || PinnedState->IsClosing())
		{
			Self.complete(Modio::make_error_code(Modio::GenericError::OperationCanceled));
			return;
		}

		reenter(CoroutineState)
		{
			if (!WinHttpReceiveResponse(Request->RequestHandle, 0))
			{
				Modio::Detail::Logger().Log(Modio::LogLevel::Error, Modio::LogCategory::Http,
											"ReceiveResponse returned system error code {}", GetLastError());

				Self.complete(Modio::make_error_code(Modio::HttpError::RequestError));
				return;
			}

			while (PinnedState->PeekHandleStatus(Request->RequestHandle) == WinHTTPCallbackStatus::Waiting)
			{
				Timer.ExpiresAfter(Modio::Detail::Constants::Configuration::PollInterval);
				yield Timer.WaitAsync(std::move(Self));
			}

			switch (PinnedState->FetchAndClearHandleStatus(Request->RequestHandle))
			{
				case WinHTTPCallbackStatus::RequestError:
					Self.complete(Modio::make_error_code(Modio::HttpError::RequestError));
					return;
				default:

				{
					DWORD StatusCode = 0;
					DWORD BufferSize = sizeof(StatusCode);
					if (WinHttpQueryHeaders(
							Request->RequestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
							WINHTTP_HEADER_NAME_BY_INDEX, &StatusCode, &BufferSize, WINHTTP_NO_HEADER_INDEX))
					{
						Request->ResponseCode = StatusCode;
					}
					else
					{
						Modio::Detail::Logger().Log(Modio::LogLevel::Error, Modio::LogCategory::Http,
													"Unable to retrieve response headers for HTTP request");
						Self.complete(Modio::make_error_code(Modio::HttpError::RequestError));
						return;
					}
				}
					Self.complete(Modio::ErrorCode {});
					return;
			}
		}
	}
};

#include <asio/unyield.hpp>