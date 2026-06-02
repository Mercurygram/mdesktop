/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_secret.h"

#include "main/main_session.h"
#include "storage/storage_account.h"
#include "mtproto/mtproto_auth_key.h"
#include "storage/storage_encrypted_file.h"
#include "storage/storage_encryption.h"
#include "base/bytes.h"

SecretFileLoader::SecretFileLoader(
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
	Storage::Cache::Key cacheKey
) : FileLoader(
	session,
	toFile,
	loadSize,
	fullSize,
	locationType,
	toCache,
	fromCloud,
	autoLoading,
	cacheTag,
	true) // allowLargeInMemory: whole-file in-memory decrypt, no remote fallback.
, _path(encryptedPath)
, _cacheKey(cacheKey) {
}

Storage::Cache::Key SecretFileLoader::cacheKey() const {
	// A valid, stable key lets the standard FileLoader pipeline cache the
	// decrypted bytes in the (local-key encrypted) cache and re-read them on
	// every later load, so bytes survive media-view churn. Still no plaintext
	// on disk: the cache database is encrypted at rest with the local key.
	return _cacheKey;
}

std::optional<MediaKey> SecretFileLoader::fileLocationKey() const {
	return std::nullopt;
}

void SecretFileLoader::cancelHook() {
}

void SecretFileLoader::startLoading() {
	const auto local = session().local().peekLegacyLocalKey();
	if (_path.isEmpty() || !local) {
		cancel(FailureReason::OtherFailure);
		return;
	}
	// The plaintext size is not stored by Storage::File (it pads the ciphertext
	// to a 16-byte block with random bytes), so truncate the decrypted bytes to
	// the known document size carried as fullSize.
	const auto path = _path;
	const auto plainSize = fullSize();
	auto key = Storage::EncryptionKey(bytes::make_vector(local->data()));
	const auto weak = base::make_weak(this);
	crl::async([=, key = std::move(key)]() mutable {
		auto plain = QByteArray();
		auto file = Storage::File();
		if (file.open(path, Storage::File::Mode::Read, key)
				== Storage::File::Result::Success) {
			const auto padded = file.size();
			if (padded > 0) {
				plain = QByteArray(int(padded), Qt::Uninitialized);
				const auto read = file.read(bytes::make_detached_span(plain));
				if (read > 0) {
					plain.resize(int(read));
					if (plainSize >= 0 && plainSize <= plain.size()) {
						plain.resize(int(plainSize));
					}
				} else {
					plain = QByteArray();
				}
			}
			file.close();
		}
		crl::on_main(weak, [=]() mutable {
			if (plain.isEmpty()) {
				cancel(FailureReason::OtherFailure);
			} else if (writeResultPart(0, bytes::make_span(plain))) {
				finalizeResult();
			}
		});
	});
}
