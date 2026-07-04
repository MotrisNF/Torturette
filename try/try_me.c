#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static void provocar_leak(void) {
    char *p = malloc(64);
    if (!p) return;
    strcpy(p, "leak");
    printf("Leak provocado: %s\n", p);
    /* Intencionadamente se omite free. */
}

static void provocar_double_free(void) {
    int *p = malloc(sizeof(int));
    if (!p) return;
    *p = 42;
    free(p);
    free(p);
}

static void provocar_sigsegv(void) {
    int *p = NULL;
    *p = 7;
}

static void provocar_sigabrt(void) {
    abort();
}

static void *forzar_malloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "[malloc] fallo catastrfico: no se pudo reservar %zu bytes\n", size);
        provocar_leak();
        provocar_double_free();
        provocar_sigabrt();
    }
    return p;
}

static void *forzar_calloc(size_t count, size_t size) {
    void *p = calloc(count, size);
    if (!p) {
        fprintf(stderr, "[calloc] fallo catastrfico: no se pudo reservar %zu x %zu bytes\n", count, size);
        provocar_leak();
        provocar_sigsegv();
    }
    return p;
}

int main(void) {
    char *texto = NULL;
    int *nums = NULL;
    char *extra = NULL;

    printf("Escribe algo y pulsa Enter para continuar.\n");
    printf("Prueba comandos como: malloc, calloc, mix, leak, doublefree, segfault, abort o exit.\n");
    printf("Cuando quieras terminar, escribe: exit\n");

    while (1) {
        char buffer[128];
        printf("> ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            printf("\nFin de entrada. Saliendo...\n");
            break;
        }

        buffer[strcspn(buffer, "\r\n")] = '\0';

        if (strcmp(buffer, "exit") == 0) {
            printf("Saliendo de forma controlada.\n");
            break;
        }

        if (strcmp(buffer, "malloc") == 0) {
            texto = forzar_malloc(32);
            if (texto) {
                snprintf(texto, 32, "memoria malloc");
                printf("Allocado con malloc: %s\n", texto);
                free(texto);
                texto = NULL;
            }
        } else if (strcmp(buffer, "calloc") == 0) {
            nums = forzar_calloc(5, sizeof(int));
            if (nums) {
                printf("Allocado con calloc: %d %d %d %d %d\n", nums[0], nums[1], nums[2], nums[3], nums[4]);
                free(nums);
                nums = NULL;
            }
        } else if (strcmp(buffer, "mix") == 0) {
            texto = forzar_malloc(64);
            nums = forzar_calloc(3, sizeof(int));
            if (texto && nums) {
                snprintf(texto, 64, "mix ok");
                printf("Escenario mix: %s\n", texto);
                free(texto);
                free(nums);
                texto = NULL;
                nums = NULL;
            } else {
                free(texto);
                free(nums);
            }
        } else if (strcmp(buffer, "leak") == 0) {
            provocar_leak();
        } else if (strcmp(buffer, "doublefree") == 0) {
            provocar_double_free();
        } else if (strcmp(buffer, "segfault") == 0) {
            provocar_sigsegv();
        } else if (strcmp(buffer, "abort") == 0) {
            provocar_sigabrt();
        } else {
            printf("Has escrito: %s\n", buffer);
        }
    }

    free(texto);
    free(nums);
    free(extra);
    return 0;
}
