#include <stdint.h>

typedef void (*initializer_t)(void);

extern initializer_t __preinit_array_start[] __attribute__((weak));
extern initializer_t __preinit_array_end[] __attribute__((weak));
extern initializer_t __init_array_start[] __attribute__((weak));
extern initializer_t __init_array_end[] __attribute__((weak));

void _init_env(void *process_parameters);
int main(int argc, char **argv);

static void run(initializer_t *first, initializer_t *last)
{
    while (first && first != last)
        (*first++)();
}

__attribute__((visibility("default"), noreturn)) void _start(void *process_parameters,
                                                             void (*loader_teardown)(void))
{
    int argc = *(int *)process_parameters;
    char **argv = (char **)((uint8_t *)process_parameters + sizeof(uint64_t));

    (void)loader_teardown;
    _init_env(process_parameters);
    run(__preinit_array_start, __preinit_array_end);
    run(__init_array_start, __init_array_end);
    main(argc, argv);
    for (;;)
        ;
}
