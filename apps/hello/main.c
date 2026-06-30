// Minimal Griffin application: exercises the syscall ABI through newlib stdio.
//
// printf() bottoms out in write() (SYS_WRITE) and reaches the firmware console
// (DUART + textport); no app-specific I/O code is needed.

#include <stdio.h>

int main(void)
{
    printf("Hello from a Griffin app loaded at 0x1000!\n");
    for (int i = 0; i < 3; i++)
    {
        printf("  count %d\n", i);
    }
    return 0;
}
