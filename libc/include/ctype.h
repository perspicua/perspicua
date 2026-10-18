/*
 * ctype.h - ASCII character classes and case mapping.
 */

#ifndef PERSPICUA_LIBC_CTYPE_H
#define PERSPICUA_LIBC_CTYPE_H

// Each is non-zero for an ASCII c in its class, and 0 for EOF and anything above 127.
int isalnum(int c);

int isalpha(int c);

int iscntrl(int c);

int isdigit(int c);

int isgraph(int c);

int islower(int c);

int isprint(int c);

int ispunct(int c);

int isspace(int c);

int isupper(int c);

int isxdigit(int c);

int isblank(int c);

// Letters change case; anything else comes back unchanged.
int tolower(int c);

int toupper(int c);

#endif // PERSPICUA_LIBC_CTYPE_H
