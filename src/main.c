volatile unsigned int* const UART0_DR = (unsigned int*)0xFE201000;

void print_char(char c)
{
    *UART0_DR = c;
}

void print_string(const char* str)
{
    while (*str)
        print_char(*str++);
}

int main()
{
    print_string("Hello World!\n");

    while (1)
        ;
    return 0;
}
