#pragma once

#ifdef __MINGW32__
  #define sprintf_s(buf, ...) snprintf(buf, sizeof(buf), __VA_ARGS__)
  #define strcat_s(dst, src) strncat(dst, src, MAX_PATH - strlen(dst) - 1)
#endif
