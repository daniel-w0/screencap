#ifndef SC_ASSERT_H
#define SC_ASSERT_H

#include "scTypes.h"

#if defined(SC_RELEASE)
  #define scAssert(cond, msg, ...) ((void)0)
#else
  void _scAssertFail(const char* sCondition, const char* sFile, s32 iLine, const char* sMsg, ...);

  #define scAssert(cond, msg, ...) \
  do { \
    if (!(cond)) { \
      _scAssertFail(#cond, __FILE__, __LINE__, msg, ##__VA_ARGS__); \
    } \
  } while(0)
#endif

#endif // SC_ASSERT_H