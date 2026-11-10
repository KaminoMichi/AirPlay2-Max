#pragma once
#include <plist/plist.h>

static inline plist_err_t ya_plist_from_memory_3(
    const char *data, uint32_t length, plist_t *output) {
  return plist_from_memory(data, length, output, NULL);
}

#define plist_from_memory(data, length, output) \
  ya_plist_from_memory_3((data), (length), (output))
