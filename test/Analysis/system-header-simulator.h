#pragma clang system_header

typedef struct _FILE FILE;
extern FILE *stdin;
int fscanf(FILE *restrict stream, const char *restrict format, ...);

// Note, on some platforms errno macro gets replaced with a function call.
extern int errno;

unsigned long strlen(const char *);

char *strcpy(char *restrict s1, const char *restrict s2);

typedef unsigned long __darwin_pthread_key_t;
typedef __darwin_pthread_key_t pthread_key_t;
int pthread_setspecific(pthread_key_t ,
         const void *);
