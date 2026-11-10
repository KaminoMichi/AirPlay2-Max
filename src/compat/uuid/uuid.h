#pragma once
#include <stddef.h>
typedef unsigned char uuid_t[16];
#define UUID_STR_LEN 37
int uuid_parse(const char *, uuid_t);
void uuid_unparse_lower(const uuid_t, char *);
void uuid_generate_random(uuid_t);
void uuid_generate_sha1(uuid_t, const uuid_t, const char *, size_t);
