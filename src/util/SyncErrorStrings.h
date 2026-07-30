#pragma once

#include <I18n.h>
#include <KOReaderSyncClient.h>

// Maps a sync client error to a translated, user-facing string. The client's
// own errorString() stays English for logs; everything shown on the panel goes
// through here.
inline const char* syncErrorText(const KOReaderSyncClient::Error error) {
  switch (error) {
    case KOReaderSyncClient::OK:
      return tr(STR_DONE);
    case KOReaderSyncClient::NO_CREDENTIALS:
      return tr(STR_NO_CREDENTIALS_MSG);
    case KOReaderSyncClient::NETWORK_ERROR:
      return tr(STR_SYNC_ERR_NETWORK);
    case KOReaderSyncClient::AUTH_FAILED:
      return tr(STR_SYNC_ERR_AUTH);
    case KOReaderSyncClient::SERVER_ERROR:
      return tr(STR_SYNC_ERR_SERVER);
    case KOReaderSyncClient::NOT_FOUND:
      return tr(STR_NO_REMOTE_MSG);
    case KOReaderSyncClient::LOW_MEMORY:
      return tr(STR_MEMORY_ERROR);
    case KOReaderSyncClient::JSON_ERROR:
    default:
      return tr(STR_ERROR_GENERAL_FAILURE);
  }
}
