/*
 *  Copyright (C) 2021 mod.io Pty Ltd. <https://mod.io>
 *
 *  This file is part of the mod.io SDK.
 *
 *  Distributed under the MIT License. (See accompanying file LICENSE or
 *   view online at <https://github.com/modio/modio-sdk/blob/main/LICENSE>)
 *
 */

#ifdef MODIO_SEPARATE_COMPILATION
	#include "modio/core/ModioModCollectionEntry.h"
#endif

#include "modio/core/ModioErrorCode.h"
#include "modio/core/ModioLogger.h"
#include "modio/detail/ModioConstants.h"
#include "modio/detail/ModioJsonHelpers.h"
#include <algorithm>

namespace Modio
{
	ModCollectionEntry::ModCollectionEntry(ModInfo ProfileData, std::string CalculatedModPath)
		: ID(ProfileData.ModId),
		  CurrentState(Modio::ModState::InstallationPending),
		  ModProfile(ProfileData),
		  LocalUserSubscriptions(),
		  PathOnDisk(CalculatedModPath),
		  RetriesRemainingThisSession(Modio::Detail::Constants::Configuration::DefaultNumberOfRetries) {};

	ModCollectionEntry::ModCollectionEntry(const ModCollectionEntry& Other)
		: ID(Other.ID),
		  CurrentState(Other.CurrentState.load()),
		  ModProfile(Other.ModProfile),
		  LocalUserSubscriptionCount(Other.LocalUserSubscriptionCount.load()),
		  LocalUserSubscriptions(Other.LocalUserSubscriptions),
		  PathOnDisk(Other.PathOnDisk),
		  SizeOnDisk(Other.SizeOnDisk),
		  RetriesRemainingThisSession(Modio::Detail::Constants::Configuration::DefaultNumberOfRetries) {};

	uint8_t ModCollectionEntry::GetRetriesRemaining()
	{
		return RetriesRemainingThisSession;
	}

	void ModCollectionEntry::UpdateModProfile(ModInfo ProfileData)
	{
		// check version in metadata and set pending install if need be
		if (ModProfile.FileInfo.has_value() && ProfileData.FileInfo.has_value())
		{
			if (ModProfile.FileInfo.value().MetadataId != ProfileData.FileInfo.value().MetadataId)
			{
				SetModState(ModState::UpdatePending);
			}
		}
		ModProfile = ProfileData;
	}

	uint8_t ModCollectionEntry::AddLocalUserSubscription(Modio::Optional<User> User)
	{
		if (User.has_value())
		{
			// if the mod's CurrentState is UninstallPending, it would be preferable to switch state back to Installed.
			// However, we don't yet have a way to verify the modfiles pending uninstall to check that they are intact
			// and up to date.
			//
			// For now, until we have a mod manifest available, we will allow the mod to uninstall so that we can be
			// confident the files are valid on reinstall.

			LocalUserSubscriptions.insert(User->UserId);
		}
		return (std::uint8_t) LocalUserSubscriptions.size();
	}

	uint8_t ModCollectionEntry::RemoveLocalUserSubscription(Modio::Optional<Modio::User> User)
	{
		if (User.has_value())
		{
			LocalUserSubscriptions.erase(User->UserId);
			if (LocalUserSubscriptions.size() == 0)
			{
				CurrentState.store(ModState::UninstallPending);
				Modio::Detail::Logger().Log(Modio::LogLevel::Info, Modio::LogCategory::ModManagement,
											"Reference count for mod {} now 0, marking for uninstallation", ID);
			}
		}
		return (std::uint8_t) LocalUserSubscriptions.size();
	}

	void ModCollectionEntry::SetModState(Modio::ModState NewState)
	{
		CurrentState = NewState;
		if (CurrentState == Modio::ModState::Installed)
		{
			RetriesRemainingThisSession = Modio::Detail::Constants::Configuration::DefaultNumberOfRetries;
		}
	}

	void ModCollectionEntry::MarkModNoRetryThisSession()
	{
		ShouldNotRetry.store(true);
	}

	void ModCollectionEntry::SetLastError(Modio::ErrorCode Reason)
	{
		// For uninstallations, defer immediately if the error indicates we should defer, else make a number of retry
		// attempts then stop
		if (GetModState() == ModState::UninstallPending)
		{
			if (Modio::ErrorCodeMatches(Reason, Modio::ErrorConditionTypes::ModDeleteDeferredError))
			{
				MarkModNoRetryThisSession();
				return;
			}
			else
			{
				if (RetriesRemainingThisSession > 0)
				{
					RetriesRemainingThisSession--;
				}
				if (RetriesRemainingThisSession == 0)
				{
					MarkModNoRetryThisSession();
				}
			}
		}
		// For installations, if we should retry, make a fixed number of attempts then stop.
		// If the error is unrecoverable, prevent future reattempts permanently.
		// Otherwise, defer to next startup
		else
		{
			if (Modio::ErrorCodeMatches(Reason, Modio::ErrorConditionTypes::ModInstallRetryableError))
			{
				if (RetriesRemainingThisSession > 0)
				{
					RetriesRemainingThisSession--;
				}
				if (RetriesRemainingThisSession == 0)
				{
					MarkModNoRetryThisSession();
				}
			}
			else if (Modio::ErrorCodeMatches(Reason, Modio::ErrorConditionTypes::ModInstallUnrecoverableError))
			{
				NeverRetryReason = Reason;
			}
			else
			{
				MarkModNoRetryThisSession();
			}
		}
	}

	void ModCollectionEntry::ClearModNoRetry()
	{
		ShouldNotRetry.store(false);
	}

	bool ModCollectionEntry::ShouldRetry()
	{
		// Should only retry if we have don't have a never retry reason AND ShouldNotRetry is not set
		return !NeverRetryReason && !ShouldNotRetry.load();
	}

	Modio::ModState ModCollectionEntry::GetModState() const
	{
		return CurrentState;
	}

	Modio::ModID ModCollectionEntry::GetID() const
	{
		return ID;
	}

	Modio::ModInfo ModCollectionEntry::GetModProfile() const
	{
		return ModProfile;
	}

	std::string ModCollectionEntry::GetPath() const
	{
		return PathOnDisk;
	}

	Modio::Optional<Modio::FileSize> ModCollectionEntry::GetSizeOnDisk() const
	{
		if (CurrentState == ModState::Installed)
		{
			return SizeOnDisk;
		}
		else
		{
			return {};
		}
	}

	void ModCollectionEntry::UpdateSizeOnDisk(Modio::FileSize NewSize)
	{
		SizeOnDisk = NewSize;
	}

	void RollbackTransactionImpl(ModCollectionEntry& Entry)
	{
		if (!Entry.RollbackState.has_value())
		{
			Modio::Detail::Logger().Log(Modio::LogLevel::Warning, Modio::LogCategory::ModManagement,
										"Tried to rollback a transaction on a mod in the system collection while no "
										"transaction was occurring!");
			return;
		}
		else
		{
			Entry.CurrentState.store(Entry.RollbackState.take().value());
		}
	}

	void BeginTransactionImpl(ModCollectionEntry& Entry)
	{
		if (Entry.CurrentState == Modio::ModState::Downloading || Entry.CurrentState == Modio::ModState::Extracting ||
			Entry.RollbackState.has_value())
		{
			Modio::Detail::Logger().Log(
				Modio::LogLevel::Warning, Modio::LogCategory::ModManagement,
				"Beginning transaction on a mod in the system collection while a transaction is "
				"already occurring!");
			return;
		}
		else
		{
			Entry.RollbackState = Entry.CurrentState.load();
		}
	}

	ModCollectionEntry& ModCollectionEntry::operator=(const ModCollectionEntry& Other)
	{
		ID = Other.ID;
		CurrentState.store(Other.CurrentState.load());
		ModProfile = Other.ModProfile;
		LocalUserSubscriptions = Other.LocalUserSubscriptions;
		LocalUserSubscriptionCount.store(Other.LocalUserSubscriptionCount.load());
		PathOnDisk = Other.PathOnDisk;
		SizeOnDisk = Other.SizeOnDisk;
		RetriesRemainingThisSession = Other.RetriesRemainingThisSession;
		return *this;
	};

	void to_json(nlohmann::json& j, const ModCollectionEntry& Entry)
	{
		Modio::ModState EntryState = Entry.CurrentState.load();
		// If the current state is in progress, then we store the previous state. As when loading, the user might
		// not want to start the download/installation during SDK initialization. Instead the progress will be
		// resumed when the mod management loop is enabled
		if (Entry.CurrentState == Modio::ModState::Downloading || Entry.CurrentState == Modio::ModState::Extracting)
		{
			if (Entry.RollbackState.has_value())
			{
				EntryState = *Entry.RollbackState;
			}
			else
			{
				Modio::Detail::Logger().Log(Modio::LogLevel::Warning, Modio::LogCategory::ModManagement,
											"Mod {0} is in state Downloading or Extracting without a transaction in "
											"progress. Saving state as InstallationPending",
											Entry.ID);
				EntryState = Modio::ModState::InstallationPending;
			}
		}

		j = nlohmann::json::object(
			{{Modio::Detail::Constants::JSONKeys::ModEntryID, Entry.ID},
			 {Modio::Detail::Constants::JSONKeys::ModEntryProfile, Entry.ModProfile},
			 {Modio::Detail::Constants::JSONKeys::ModEntrySubCount, Entry.LocalUserSubscriptions},
			 {Modio::Detail::Constants::JSONKeys::ModEntryState, EntryState},
			 {Modio::Detail::Constants::JSONKeys::ModSizeOnDisk, Entry.SizeOnDisk},
			 {Modio::Detail::Constants::JSONKeys::ModPathOnDisk, Entry.PathOnDisk},
			 {Modio::Detail::Constants::JSONKeys::ModNeverRetryCode, Entry.NeverRetryReason.value()},
			 {Modio::Detail::Constants::JSONKeys::ModNeverRetryCategory,
			  Modio::Detail::ModioErrorCategoryID(Entry.NeverRetryReason.category())}});
	}

	void from_json(const nlohmann::json& j, ModCollectionEntry& Entry)
	{
		Modio::Detail::ParseSafe(j, Entry.ID, Modio::Detail::Constants::JSONKeys::ModEntryID);
		Modio::Detail::ParseSafe(j, Entry.ModProfile, Modio::Detail::Constants::JSONKeys::ModEntryProfile);
		Modio::Detail::ParseSafe(j, Entry.LocalUserSubscriptions, Modio::Detail::Constants::JSONKeys::ModEntrySubCount);
		Modio::Detail::ParseSafe(j, Entry.SizeOnDisk, Modio::Detail::Constants::JSONKeys::ModSizeOnDisk);
		Modio::ModState StateTmp = ModState::InstallationPending;
		Modio::Detail::ParseSafe(j, StateTmp, Modio::Detail::Constants::JSONKeys::ModEntryState);
		Entry.CurrentState.store(StateTmp);
		Modio::Detail::ParseSafe(j, Entry.PathOnDisk, Modio::Detail::Constants::JSONKeys::ModPathOnDisk);
		if (j.contains(Modio::Detail::Constants::JSONKeys::ModNeverRetryCode) &&
			j.contains(Modio::Detail::Constants::JSONKeys::ModNeverRetryCategory))
		{
			Entry.NeverRetryReason =
				std::error_code(j.at(Modio::Detail::Constants::JSONKeys::ModNeverRetryCode).get<uint32_t>(),
								Modio::Detail::GetModioErrorCategoryByID(
									j.at(Modio::Detail::Constants::JSONKeys::ModNeverRetryCategory).get<uint64_t>()));
		}
	}

	void SetState(Modio::ModProgressInfo& Info, Modio::ModProgressInfo::EModProgressState State)
	{
		Info.CurrentState = State;
	}

	void SetCurrentProgress(Modio::ModProgressInfo& Info, Modio::FileSize NewValue)
	{
		switch (Info.CurrentState)
		{
			case ModProgressInfo::EModProgressState::Downloading:
				Info.DownloadCurrent = NewValue;
				return;
			case ModProgressInfo::EModProgressState::Extracting:
				Info.ExtractCurrent = NewValue;
				return;
			case ModProgressInfo::EModProgressState::Uploading:
				Info.UploadCurrent = NewValue;
				return;
			case ModProgressInfo::EModProgressState::Compressing:
				Info.CompressCurrent = NewValue;
				MODIO_FALL_THROUGH;
			default:
				return;
		}
	}

	void IncrementCurrentProgress(Modio::ModProgressInfo& Info, Modio::FileSize NewValue)
	{
		switch (Info.CurrentState)
		{
			case ModProgressInfo::EModProgressState::Downloading:
				Info.DownloadCurrent += NewValue;
				return;
			case ModProgressInfo::EModProgressState::Extracting:
				Info.ExtractCurrent += NewValue;
				return;
			case ModProgressInfo::EModProgressState::Uploading:
				Info.UploadCurrent += NewValue;
				return;
			case ModProgressInfo::EModProgressState::Compressing:
				Info.CompressCurrent += NewValue;
				MODIO_FALL_THROUGH;
			default:
				return;
		}
	}

	void CompleteProgressState(Modio::ModProgressInfo& Info, Modio::ModProgressInfo::EModProgressState State)
	{
		switch (State)
		{
			case ModProgressInfo::EModProgressState::Downloading:
				Info.DownloadCurrent = Info.DownloadTotal;
				return;
			case ModProgressInfo::EModProgressState::Extracting:
				Info.ExtractCurrent = Info.ExtractTotal;
				return;
			case ModProgressInfo::EModProgressState::Compressing:
				Info.CompressCurrent = Info.CompressTotal;
				return;
			case ModProgressInfo::EModProgressState::Uploading:
				Info.UploadCurrent = Info.UploadTotal;
				return;
			default:
				return;
		}
	}

	void SetTotalProgress(Modio::ModProgressInfo& Info, Modio::ModProgressInfo::EModProgressState State,
						  Modio::FileSize NewTotal)
	{
		switch (State)
		{
			case ModProgressInfo::EModProgressState::Downloading:
				Info.DownloadTotal = NewTotal;
				return;
			case ModProgressInfo::EModProgressState::Extracting:
				Info.ExtractTotal = NewTotal;
				return;
			case ModProgressInfo::EModProgressState::Uploading:
				Info.UploadTotal = NewTotal;
				return;
			case ModProgressInfo::EModProgressState::Compressing:
				Info.CompressTotal = NewTotal;
				MODIO_FALL_THROUGH;
			default:
				return;
		}
	}

	UserSubscriptionList::UserSubscriptionList(std::vector<Modio::ModID>&& NewIDs)
		: InternalList(std::make_move_iterator(NewIDs.begin()), std::make_move_iterator(NewIDs.end()))
	{}

	UserSubscriptionList::UserSubscriptionList(std::vector<Modio::ModID> NewIDs)
		: InternalList(NewIDs.begin(), NewIDs.end())
	{}

	UserSubscriptionList::UserSubscriptionList() {}

	bool UserSubscriptionList::AddMod(Modio::ModInfo Mod)
	{
		return InternalList.insert(Mod.ModId).second;
	}

	void UserSubscriptionList::RemoveMod(Modio::ModID Mod)
	{
		InternalList.erase(Mod);
	}

	const std::set<Modio::ModID>& UserSubscriptionList::Get() const
	{
		return InternalList;
	}

	std::map<Modio::ModID, Modio::UserSubscriptionList::ChangeType> UserSubscriptionList::CalculateChanges(
		const UserSubscriptionList& Original, const UserSubscriptionList& Updated)
	{
		std::map<Modio ::ModID, ChangeType> Diff;

		std::vector<Modio::ModID> ChangedMods;
		// Get a vector of all ModIDs that only occur in either Original or Updated but not both
		std::set_symmetric_difference(Original.InternalList.begin(), Original.InternalList.end(),
									  Updated.InternalList.begin(), Updated.InternalList.end(),
									  std::back_inserter(ChangedMods));
		// For each Mod ID that was present only in one set...
		for (Modio::ModID ChangedModID : ChangedMods)
		{
			// If the Mod ID not present in the original set, it must be an addition
			if (Original.InternalList.find(ChangedModID) == Original.InternalList.end())
			{
				Diff[ChangedModID] = ChangeType::Added;
			}
			else
			{
				// if the mod ID was present in the original set, it must NOT be in the new set. Therefore it is a
				// removal.
				Diff[ChangedModID] = ChangeType::Removed;
			}
		}

		return Diff;
	}

	void to_json(nlohmann::json& j, const UserSubscriptionList& List)
	{
		j = nlohmann::json {Modio::Detail::Constants::JSONKeys::UserSubscriptionList, List.InternalList};
	}
	void from_json(const nlohmann::json& j, UserSubscriptionList& List)
	{
		if (j.is_array())
		{
			using nlohmann::from_json;
			from_json(j, List.InternalList);
		}
	}
	ModCollection::ModCollection(std::map<Modio::ModID, std::shared_ptr<Modio::ModCollectionEntry>> Entries)
	{
		for (auto& ModEntry : Entries)
		{
			ModEntries.emplace(std::make_pair(ModEntry.first, std::make_shared<Modio::ModCollectionEntry>(*ModEntry.second)));
		}
	}
	const Modio::ModCollection ModCollection::FilterByUserSubscriptions(
		const UserSubscriptionList& UserSubscriptions) const
	{
		ModCollection FilteredCollection;
		for (Modio::ModID UserModID : UserSubscriptions.Get())
		{
			if (ModEntries.count(UserModID))
			{
				FilteredCollection.ModEntries[UserModID] = ModEntries.at(UserModID);
			}
			else
			{
				// Silently fail for now, don't spam the log - empty list should only occur before first
				// FetchExternalUpdatesAsync
				/*Modio::Detail::Logger().Log(Modio::LogLevel::Warning, Modio::LogCategory::ModManagement,
									"Trying to look up mod ID {} but it is not present in local metadata",
									UserModID);*/
			}
		}
		return FilteredCollection;
	}

	bool ModCollection::AddOrUpdateMod(Modio::ModInfo ModToAdd, std::string CalculatedModPath)
	{
		if (ModEntries.find(ModToAdd.ModId) == ModEntries.end())
		{
			ModEntries[ModToAdd.ModId] = std::make_shared<Modio::ModCollectionEntry>(ModToAdd, CalculatedModPath);
			return true;
		}
		else
		{
			ModEntries[ModToAdd.ModId]->UpdateModProfile(ModToAdd);
			return false;
		}
	}

	const std::map<Modio::ModID, std::shared_ptr<Modio::ModCollectionEntry>>& ModCollection::Entries() const
	{
		return ModEntries;
	}

	Modio::Optional<Modio::ModCollectionEntry&> ModCollection::GetByModID(Modio::ModID ModId) const
	{
		if (ModEntries.count(ModId))
		{
			return *(ModEntries.at(ModId));
		}
		else
		{
			return {};
		}
	}

	bool ModCollection::RemoveMod(Modio::ModID ModId, bool bForce)
	{
		if (ModEntries.count(ModId))
		{
			if ((ModEntries.at(ModId)->GetModState() == ModState::UninstallPending) || (bForce == true))
			{
				ModEntries.erase(ModId);
				return true;
			}
			else
			{
				Modio::Detail::Logger().Log(
					LogLevel::Warning, LogCategory::ModManagement,
					"Failed to remove Mod {} from Mod Collection as its state is not UninstallPending", ModId);
			}
		}
		return false;
	}

	std::vector<std::shared_ptr<Modio::ModCollectionEntry>> ModCollection::SortEntriesByRetryPriority() const
	{
		std::vector<std::shared_ptr<Modio::ModCollectionEntry>> SortedEntries;
		// Copy the entries to the vector
		for (const std::pair<const Modio::ModID, std::shared_ptr<Modio::ModCollectionEntry>>& Elem : ModEntries)
		{
			SortedEntries.push_back(Elem.second);
		}
		// Sort the entries by priority (that is, entries which can be retried should be first, and entries which
		// haven't been retried this session should be higher )
		auto FirstElemWithNoRetry =
			std::partition(SortedEntries.begin(), SortedEntries.end(),
						   [](std::shared_ptr<Modio::ModCollectionEntry> Elem) { return Elem->ShouldRetry(); });
		std::partition(
			SortedEntries.begin(), FirstElemWithNoRetry, [](std::shared_ptr<Modio::ModCollectionEntry> Elem) {
				return Elem->GetRetriesRemaining() == Modio::Detail::Constants::Configuration::DefaultNumberOfRetries;
			});
		return SortedEntries;
	}

	void to_json(nlohmann::json& Json, const Modio::ModCollection& Collection)
	{
		std::vector<Modio::ModCollectionEntry> ResolvedEntries;
		for (auto& Mod : Collection.ModEntries)
		{
			ResolvedEntries.push_back(*(Mod.second));
		}
		Json = {Modio::Detail::Constants::JSONKeys::ModCollection, ResolvedEntries};
	}

	void from_json(const nlohmann::json& Json, Modio::ModCollection& Collection)
	{
		std::vector<Modio::ModCollectionEntry> LoadedEntries;
		Modio::Detail::ParseSafe(Json, LoadedEntries, Modio::Detail::Constants::JSONKeys::ModCollection);
		for (Modio::ModCollectionEntry& Entry : LoadedEntries)
		{
			Collection.ModEntries[Entry.GetID()] = std::make_shared<Modio::ModCollectionEntry>(Entry);
		}
	}

	void ModEventLog::AddEntry(Modio::ModManagementEvent Entry)
	{
		Modio::Detail::Logger().Log(LogLevel::Info, LogCategory::ModManagement,
									"Adding ModManagementEvent {} with status {} to ModEventLog for ModID {}",
									static_cast<std::uint8_t>(Entry.Event), Entry.Status.value(), Entry.ID);
		InternalData.push_back(std::move(Entry));
	}

} // namespace Modio
