/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/file_download.h"
#include "storage/cache/storage_cache_types.h"

// Loads a secret-chat media file that lives ENCRYPTED at rest (written via
// Storage::File with the local key). It reads + decrypts the file into memory
// off the main thread and hands the plaintext to the standard FileLoader
// pipeline (in-memory bytes, or a user-chosen plaintext path on "Save as").
// There is no remote location for such a document, so this loader fully
// replaces the would-be mtpFileLoader for it.
class SecretFileLoader final : public FileLoader {
public:
	SecretFileLoader(
		not_null<Main::Session*> session,
		const QString &encryptedPath,
		const QString &toFile,
		int64 loadSize,
		int64 fullSize,
		LocationType locationType,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag,
		Storage::Cache::Key cacheKey);

private:
	Storage::Cache::Key cacheKey() const override;
	std::optional<MediaKey> fileLocationKey() const override;
	void cancelHook() override;
	void startLoading() override;

	QString _path;
	Storage::Cache::Key _cacheKey;

};
