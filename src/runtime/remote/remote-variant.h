#pragma once
/* Deliberately no implicit default: the release must identify its payload. */
#ifndef YR_COMMAND_UINT
#error YR_COMMAND_UINT must be 0 or 1
#endif
#ifndef YR_WITH_PARAMS
#error YR_WITH_PARAMS must be 0 or 1
#endif
#if (YR_COMMAND_UINT != 0 && YR_COMMAND_UINT != 1) || (YR_WITH_PARAMS != 0 && YR_WITH_PARAMS != 1)
#error Invalid payload variant
#endif
#ifndef YR_VARIANT_LABEL
#error Missing payload label
#endif
const char *ya_remote_payload_variant(void);
