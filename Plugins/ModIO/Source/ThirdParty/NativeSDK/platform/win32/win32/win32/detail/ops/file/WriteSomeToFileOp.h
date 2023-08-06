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
#include "file/FileObjectImplementation.h"
#include "fileapi.h"
#include "modio/core/ModioBuffer.h"
#include "modio/core/ModioErrorCode.h"
#include "modio/core/ModioLogger.h"
#include "modio/detail/ModioConstants.h"
#include "modio/detail/ModioObjectTrack.h"
#include "modio/detail/ModioProfiling.h"
#include "modio/timer/ModioTimer.h"

#include <asio/yield.hpp>

class WriteSomeToFileOp : public Modio::Detail::BaseOperation<WriteSomeToFile>
{
	/// <summary>
	/// Buffer to write to file
	/// </summary>
	Modio::Detail::Buffer Buffer;
	/// <summary>
	/// Reference to the platform-specific implementation of the file object
	/// </summary>
	std::shared_ptr<Modio::Detail::FileObjectImplementation> FileImpl;
	/// <summary>
	/// Offset within the file to write the data
	/// </summary>
	std::uintmax_t FileOffset;
	/// <summary>
	/// Win32 control structure for an async file IO operation
	/// </summary>
	Modio::StableStorage<OVERLAPPED> WriteOpParams;
	/// <summary>
	/// State container for the coroutine invoked in the function call operator overload
	/// </summary>
	asio::coroutine Coroutine;
	/// <summary>
	/// Timer to allow the polling interval to be set for the waiting state of the operation
	/// </summary>
	Modio::Detail::Timer StatusTimer;

public:
	WriteSomeToFile(std::shared_ptr<Modio::Detail::FileObjectImplementation> IOObject, std::uintmax_t Offset,
					Modio::Detail::Buffer Buffer)
		: Buffer(std::move(Buffer)),
		  FileImpl(IOObject),
		  FileOffset(Offset),
		  WriteOpParams {}
	{}

	WriteSomeToFile(WriteSomeToFile&& Other)
		: Buffer(std::move(Other.Buffer)),
		  FileImpl(Other.FileImpl),
		  FileOffset(Other.FileOffset),
		  WriteOpParams(std::move(Other.WriteOpParams)),
		  Coroutine(std::move(Other.Coroutine)),
		  StatusTimer(std::move(Other.StatusTimer)) {};

	~WriteSomeToFile()
	{
		if (WriteOpParams && WriteOpParams->hEvent != INVALID_HANDLE_VALUE)
		{
			CloseHandle(WriteOpParams->hEvent);
			WriteOpParams->hEvent = INVALID_HANDLE_VALUE;
		}
	}

	template<typename CoroType>
	void operator()(CoroType& Self, std::error_code ec = {})
	{
		if (FileImpl->ShouldCancel())
		{
			Self.complete(Modio::make_error_code(Modio::GenericError::OperationCanceled));
			return;
		}
		if (FileImpl->GetFileMode() == Modio::Detail::FileMode::ReadOnly)
		{
			self.complete(Modio::make_error_code(Modio::FilesystemError::NoPermission));
			return;
		}
		MODIO_PROFILE_SCOPE(WriteSomeToFileOp);
		reenter(Coroutine)
		{
			if (Buffer.GetSize() == 0)
			{
				Self.complete({}, Modio::Detail::Buffer(0));
				return;
			}

			yield asio::post(Modio::Detail::Services::GetGlobalContext().get_executor(), std::move(Self));
			Modio::Detail::Logger().Log(Modio::LogLevel::Trace, Modio::LogCategory::File,
										"Begin write of {} bytes to {} at {}", Buffer.GetSize(),
										FileImpl->GetPath().string(), FileOffset);
			WriteOpParams = std::make_shared<OVERLAPPED>();
			WriteOpParams->hEvent = CreateEvent(NULL, false, false, NULL);
			if (!WriteOpParams->hEvent)
			{
				WriteOpParams->hEvent = INVALID_HANDLE_VALUE;
				// Notify the caller that we could not create an event handle
				Modio::Detail::Logger().Log(Modio::LogLevel::Error, Modio::LogCategory::File,
											"Could not create event handle");
				Self.complete(Modio::make_error_code(Modio::GenericError::CouldNotCreateHandle));
				return;
			}

			WriteOpParams->OffsetHigh = FileOffset >> 32;
			WriteOpParams->Offset = (DWORD) FileOffset;

			if (!WriteFile(FileImpl->GetFileHandle(), Buffer.Data(), Buffer.GetSize(), nullptr, WriteOpParams.get()))
			{
				DWORD Error = GetLastError();
				// If Error is IO_PENDING, all is OK. Otherwise, bail
				if (Error != ERROR_IO_PENDING)
				{
					Modio::Detail::Logger().Log(Modio::LogLevel::Error, Modio::LogCategory::File,
												"WriteSomeToFile to file {} failed, error code = {}",
												FileImpl->GetPath().string(), Error);
					Self.complete(Modio::make_error_code(Modio::FilesystemError::WriteError));
					return;
				}
			}
			else
			{
				// File write completed synchronously so no need to wait, complete the operation
				Modio::Detail::Logger().Log(Modio::LogLevel::Trace, Modio::LogCategory::File, "Finish write to {}",
											FileImpl->GetPath().string());
				Buffer.~Buffer();
				Self.complete(std::error_code {});
				return;
			}

			// Poll the status of the write at the specified interval
			while (!HasOverlappedIoCompleted(WriteOpParams.get()))
			{
				StatusTimer.ExpiresAfter(Modio::Detail::Constants::Configuration::PollInterval);
				yield StatusTimer.WaitAsync(std::move(Self));
			}

			Modio::Detail::Logger().Log(Modio::LogLevel::Trace, Modio::LogCategory::File, "Finish write to {}",
										FileImpl->GetPath().string());
			Buffer.~Buffer();
			Self.complete(std::error_code {});
			return;
		}
	}
};

#include <asio/unyield.hpp>