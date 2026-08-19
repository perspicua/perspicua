#include "stdio.h"
#include "stdlib.h"

int main(void)
{
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            printf("%s\n", environ[i]);
        }
    }
    return 0;
}
