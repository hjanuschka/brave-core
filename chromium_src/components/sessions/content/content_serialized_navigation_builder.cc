/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "components/sessions/content/content_serialized_navigation_builder.h"

#include <string>

#include "brave/components/containers/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_CONTAINERS)
#include "base/containers/map_util.h"
#include "brave/components/containers/content/browser/contained_tab_handler_registry.h"
#endif  // BUILDFLAG(ENABLE_CONTAINERS)

#define FromNavigationEntry FromNavigationEntry_ChromiumImpl

#define BRAVE_CONTENT_SERIALIZED_NAVIGATION_BUILDER_TO_NAVIGATION_ENTRY \
  if (navigation->storage_partition_key().has_value()) {                \
    entry->SetStoragePartitionKeyToRestore(                             \
        navigation->storage_partition_key().value());                   \
  }

#include <components/sessions/content/content_serialized_navigation_builder.cc>

#undef FromNavigationEntry
#undef BRAVE_CONTENT_SERIALIZED_NAVIGATION_BUILDER_TO_NAVIGATION_ENTRY

namespace sessions {

// static
SerializedNavigationEntry
ContentSerializedNavigationBuilder::FromNavigationEntry(
    int index,
    content::NavigationEntry* entry,
    SerializationOptions serialization_options) {
  SerializedNavigationEntry navigation =
      FromNavigationEntry_ChromiumImpl(index, entry, serialization_options);

#if BUILDFLAG(ENABLE_CONTAINERS)
  if (auto storage_partition_key_to_restore =
          entry->GetStoragePartitionKeyToRestore()) {
    std::string url_prefix;
    if (auto altered_url =
            containers::ContainedTabHandlerRegistry::GetInstance()
                .EmbedStoragePartitionKeyInUrl(
                    navigation.virtual_url_, *storage_partition_key_to_restore,
                    url_prefix)) {
      navigation.set_virtual_url(*altered_url);
      // Do not set storage_partition_key into SerializedNavigationEntry, as
      // it's already embedded in the url.
      if (!navigation.encoded_page_state().empty()) {
        blink::PageState page_state_obj =
            blink::PageState::CreateFromEncodedData(
                navigation.encoded_page_state());
        navigation.set_encoded_page_state(
            page_state_obj.PrefixTopURL(url_prefix).ToEncodedData());
      }
    }
  }
#endif  // BUILDFLAG(ENABLE_CONTAINERS)
  return navigation;
}

}  // namespace sessions
