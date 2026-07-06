#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* --------------------------------------------------------------------
 * Simula una lista enlazada de tokens (el tipo de estructura que aparece
 * en minishell, un parser, o cualquier proyecto que trocea una entrada).
 * BUENA PRACTICA: si a mitad de construir la lista falla una reserva, se
 * libera TODO lo que ya se habia reservado antes de devolver NULL. Si
 * torturette hace fallar cualquiera de estos malloc, el resultado debe
 * ser siempre OK: ni fugas ni crashes, solo un NULL bien manejado.
 * -------------------------------------------------------------------- */
typedef struct s_token {
    char *texto;
    struct s_token *siguiente;
} t_token;

static void liberar_tokens(t_token *lista) {
    while (lista) {
        t_token *siguiente = lista->siguiente;
        free(lista->texto);
        free(lista);
        lista = siguiente;
    }
}

static t_token *construir_tokens(void) {
    const char *palabras[] = { "esto", "no", "lo", "vas", "a", "romper" };
    size_t n = sizeof(palabras) / sizeof(*palabras);
    t_token *cabeza = NULL, *cola = NULL;

    for (size_t i = 0; i < n; i++) {
        t_token *nodo = malloc(sizeof(t_token));
        if (!nodo) { liberar_tokens(cabeza); return NULL; }
        nodo->texto = strdup(palabras[i]);
        if (!nodo->texto) { free(nodo); liberar_tokens(cabeza); return NULL; }
        nodo->siguiente = NULL;
        if (!cabeza) cabeza = nodo; else cola->siguiente = nodo;
        cola = nodo;
    }
    return cabeza;
}

/* --------------------------------------------------------------------
 * Simula una matriz (filas de un stack tipo push_swap, un tablero,
 * lo que sea). MALA PRACTICA real y comun: si falla la reserva de una
 * fila que no sea la primera, las filas anteriores YA reservadas no se
 * liberan. Si torturette hace fallar el calloc de la fila 0, el
 * resultado es OK (nada que perder todavia); si falla en cualquier fila
 * posterior, el resultado debe ser KO (fuga de las filas previas).
 * -------------------------------------------------------------------- */
#define FILAS 4
#define COLUMNAS 4

static int **construir_matriz(void) {
    int **matriz = malloc(sizeof(int *) * FILAS);
    if (!matriz) return NULL;
    for (int i = 0; i < FILAS; i++) {
        matriz[i] = calloc(COLUMNAS, sizeof(int));
        if (!matriz[i]) return NULL; /* aqui esta el bug: no se liberan
                                         ni 'matriz' ni las filas 0..i-1 */
        for (int j = 0; j < COLUMNAS; j++) matriz[i][j] = (int)(i * COLUMNAS + j);
    }
    return matriz;
}

static void liberar_matriz(int **matriz) {
    if (!matriz) return;
    for (int i = 0; i < FILAS; i++) free(matriz[i]);
    free(matriz);
}

/* --------------------------------------------------------------------
 * Simula un buffer que va creciendo a base de leer trozos, al estilo
 * get_next_line. MALA PRACTICA muy real y muy comun: reasignar el
 * puntero original directamente con el resultado de realloc(). Si
 * realloc falla, se pierde para siempre el puntero al bloque original
 * -- fuga garantizada, y encima ya no hay forma de recuperarlo. Si
 * torturette hace fallar el malloc inicial, el resultado es OK; si hace
 * fallar cualquiera de los realloc posteriores, el resultado es KO.
 * -------------------------------------------------------------------- */
static char *buffer_creciente(void) {
    char *buf = malloc(4);
    if (!buf) return NULL;
    strcpy(buf, "GNL");
    for (int i = 0; i < 3; i++) {
        size_t nuevo_tam = strlen(buf) + 5;
        buf = realloc(buf, nuevo_tam); /* si esto falla, el buffer viejo
                                           se acaba de perder */
        if (!buf) return NULL;
        strcat(buf, "_x");
    }
    return buf;
}

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

int main(void) {
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
            t_token *tokens = construir_tokens();
            if (tokens) {
                printf("Tokens:");
                for (t_token *t = tokens; t; t = t->siguiente) printf(" %s", t->texto);
                printf("\n");
                liberar_tokens(tokens);
            } else {
                printf("No se pudieron construir los tokens (memoria agotada, gestionado sin fugas).\n");
            }
        } else if (strcmp(buffer, "calloc") == 0) {
            int **matriz = construir_matriz();
            if (matriz) {
                printf("Matriz %dx%d construida, ancho: %d\n",
                       FILAS, COLUMNAS, matriz[FILAS - 1][COLUMNAS - 1]);
                liberar_matriz(matriz);
            } else {
                printf("No se pudo construir la matriz completa.\n");
            }
        } else if (strcmp(buffer, "mix") == 0) {
            char *buf = buffer_creciente();
            if (buf) {
                printf("Buffer creciente: %s\n", buf);
                free(buf);
            } else {
                printf("El buffer no llego a completarse.\n");
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

    return 0;
}
