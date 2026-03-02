#ifndef _STRING_H_
#define _STRING_H_

unsigned int strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, unsigned int count);

char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, unsigned int count);

int strcmp(const char* lhs, const char* rhs);
int strncmp(const char* lhs, const char* rhs, unsigned int count);

char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);

int memcmp(const void* ptr1, const void* ptr2, unsigned int num);
void* memset(void* dest, int val, unsigned int num);
void* memcpy(void* dest, const void* src, unsigned int count);
void* memmove(void* dest, const void* src, unsigned int count);

#endif // _STRING_H_
