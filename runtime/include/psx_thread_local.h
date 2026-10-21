#ifndef PSX_THREAD_LOCAL_H
#define PSX_THREAD_LOCAL_H
#if defined(_MSC_VER)
#define PSX_THREAD_LOCAL __declspec(thread)
#else
#define PSX_THREAD_LOCAL __thread
#endif
#endif
