#ifndef SRC_COMMON_H_
#define SRC_COMMON_H_

#include <stdio.h>
#include <stdlib.h>

#define ASSERT(exp, msg)                                                      \
    if (!(exp)) {                                                             \
      fprintf(stderr,                                                         \
              "Assertion failed at %s:%d\n" msg "\n",                         \
              __FILE__,                                                       \
              __LINE__);                                                      \
      abort();                                                                \
    }

#define OSERR_CHECK(err, msg)                                                 \
    if ((err) != noErr) {                                                     \
      fprintf(stderr,                                                         \
              "OS returned error at %s:%d with %d:\"%s\"\n" msg "\n",         \
              __FILE__,                                                       \
              __LINE__,                                                       \
              (err),                                                          \
              GetMacOSStatusErrorString((err)));                              \
      abort();                                                                \
    }

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif  // SRC_COMMON_H_
