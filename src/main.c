#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <time.h>

#include "comun.h"
#include "motor.h"
#include "informe.h"

static int comparar_pruebas(const void *a, const void *b) {
    const PruebaDiferida *pa = *(PruebaDiferida * const *)a;
    const PruebaDiferida *pb = *(PruebaDiferida * const *)b;
    return pa->indice - pb->indice;
}

/* injector.so vive junto al ejecutable de torturete (lo construye el
 * Makefile), no en el directorio de trabajo actual. Se resuelve via
 * /proc/self/exe para que "torturete" funcione sin importar desde donde
 * se invoque. */
static void resolver_ruta_injector(char *destino, size_t tam) {
    char ruta_exe[4096];
    ssize_t n = readlink("/proc/self/exe", ruta_exe, sizeof(ruta_exe) - 1);
    if (n <= 0) { snprintf(destino, tam, "./injector.so"); return; }
    ruta_exe[n] = '\0';
    char *directorio = dirname(ruta_exe); /* puede modificar ruta_exe */
    snprintf(destino, tam, "%s/injector.so", directorio);
}

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    signal(SIGINT, SIG_IGN);  /* si el usuario pulsa Ctrl+C para el programa objetivo,
                                 que no mate tambien a torturete antes de analizar. */
    mostrar_banner();
    if (argc < 2) { printf("Uso: %s <programa_objetivo> [argumentos...]\n", *argv); printf(YELLOW "(dime a quien torturar y dejame el resto a mi)\n" RESET); return 1; }

    char ruta_injector[4096];
    resolver_ruta_injector(ruta_injector, sizeof(ruta_injector));
    if (access(ruta_injector, F_OK) != 0) {
        fprintf(stderr, "[-] injector.so ha desaparecido (%s). Como tu gestion de memoria, aparentemente.\n", ruta_injector);
        fprintf(stderr, "    Compila el proyecto con 'make' antes de usar torturete.\n");
        return 1;
    }
    motor_configurar_ruta_injector(ruta_injector);

    printf(YELLOW "Ejecutando el programa objetivo... agarrate.\n" RESET);
    printf(CYAN "(Entrada y salida reales. Cada malloc/calloc/realloc/pthread_create se\n" RESET);
    printf(CYAN " prueba en el momento, con un fork() que hereda el estado real -- una\n" RESET);
    printf(CYAN " prueba cada vez, asi que esto va para largo. Ve haciendote un cafe.)\n" RESET);
    printf("---------------------------------------------------------------------------------\n");

    ReporteMetricas base = ejecutar_vivo_y_lanzar_diferidas(argv + 1);

    printf("---------------------------------------------------------------------------------\n");

    if (base.total_leaks == -666) {
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- DOUBLE FREE durante la ejecucion normal.\n");
        mostrar_backtrace(-1, "DOUBLE FREE EN EJECUCION BASE", base.backtrace, NULL, argv[1]);
    } else if (base.fue_crash == 2) {
        printf("[" YELLOW "EJECUCION BASE" RESET "] Interrumpida por el usuario (Ctrl+C). Huyendo de la verdad, como siempre.\n");
    } else if (base.fue_crash == -1) {
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- no se pudo ni arrancar el programa objetivo (fork fallido, recursos agotados).\n");
    } else if (base.fue_crash) {
        const char *motivo = "CRASH";
        if (base.fue_crash == 11) motivo = "SIGSEGV (Null Pointer / Use-After-Free)";
        else if (base.fue_crash == 6) motivo = "SIGABRT (Violacion de memoria)";
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- %s (senal %d).\n", motivo, base.fue_crash);
        mostrar_backtrace(-1, motivo, base.backtrace, NULL, argv[1]);
    } else if (base.total_leaks > 0) {
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- %d malloc / %d free, " RED "%zu bytes colgados" RESET ".\n",
               base.total_mallocs, base.total_frees, base.bytes_leaked);
        printf("    " CYAN "Memoria total reservada:" RESET " %zu bytes -- " CYAN "memoria total liberada:" RESET " %zu bytes\n",
               base.bytes_reservados, base.bytes_liberados);
        mostrar_backtrace(-1, "LEAK EN EJECUCION BASE", base.backtrace, NULL, argv[1]);
    } else {
        printf("[" GREEN "EJECUCION BASE" RESET "] " GREEN "OK" RESET " -- %d malloc / %d free, sin fugas.\n",
               base.total_mallocs, base.total_frees);
        printf("    " CYAN "Memoria total reservada:" RESET " %zu bytes -- " CYAN "memoria total liberada:" RESET " %zu bytes\n",
               base.bytes_reservados, base.bytes_liberados);
    }
    free(base.backtrace);
    free(base.salida_colgada);

    int total_pruebas_final = total_pruebas;
    int problemas_base = (base.total_leaks == -666 || (base.fue_crash && base.fue_crash != 2)) ? 1 : (base.total_leaks > 0 ? 1 : 0);

    if (total_pruebas_final == 0) {
        printf(YELLOW "\nNi una sola llamada a malloc/calloc/realloc/pthread_create en toda la ejecucion.\n" RESET);
        printf(YELLOW "O tu programa no hace nada, o hace justo lo minimo indispensable.\n" RESET);
        if (problemas_base) {
            /* Ni falta hizo tocar memoria para que la ejecucion base ya
             * hubiese crasheado/hecho double-free/dejado un leak (ver
             * arriba, ya reportado con su propio backtrace) -- eso YA es
             * un problema critico, con independencia de que no hubiera
             * ninguna llamada que someter a fallo de memoria. */
            printf(" " RED "RESULTADO GLOBAL:" RESET " [" RED "KO" RESET "] Ni siquiera hizo falta tocar la memoria para que se rompiera.\n");
            mostrar_frases_ko_general();
        } else {
            printf(" " GREEN "RESULTADO GLOBAL:" RESET " [" GREEN "OK" RESET "] El binario no registro operaciones de memoria durante la ejecucion.\n");
            mostrar_frases_ok_general();
        }
        return 0;
    }

    qsort(pruebas, total_pruebas_final, sizeof(PruebaDiferida *), comparar_pruebas);

    printf(YELLOW "\nAqui tienes: %d pruebas de fallo de memoria/hilos, sin trampa ni carton\n" RESET, total_pruebas_final);
    printf("---------------------------------------------------------------------------------\n");
    printf("%-10s | %-16s | %-9s | %-7s | %-13s | %-10s\n", "PRUEBA", "ESCENARIO", "MALLOCS", "FREES", "MEMORIA", "ESTADO");
    printf("---------------------------------------------------------------------------------\n");

    int tests_fallados = 0;
    for (int i = 0; i < total_pruebas_final; i++) {
        PruebaDiferida *p = pruebas[i];
        ReporteMetricas iteracion = p->resultado;
        char num_prueba[16], escenario[32];
        snprintf(num_prueba, sizeof(num_prueba), "#%02d", p->indice + 1);
        if (p->es_pthread) snprintf(escenario, sizeof(escenario), "Falla pthread %d", p->indice + 1);
        else snprintf(escenario, sizeof(escenario), "Falla Malloc %d", p->indice + 1);

        size_t bytes_netos = 0;
        if (iteracion.bytes_leaked > base.bytes_leaked) bytes_netos = iteracion.bytes_leaked - base.bytes_leaked;

        printf("%-10s | %-16s | " WHITE "%-9d" RESET " | " WHITE "%-7d" RESET " | %-10zu bytes | ",
               num_prueba, escenario, iteracion.total_mallocs, iteracion.total_frees, bytes_netos);

        if (iteracion.total_leaks == -667) {
            tests_fallados++;
            printf("[" RED "KO" RESET "] " RED "CUELGUE (no responde)" RESET "\n");
            registrar_caso(p->indice, "CUELGUE", NULL, iteracion.salida_colgada);
        } else if (iteracion.total_leaks == -666) {
            tests_fallados++;
            printf("[" RED "KO" RESET "] " RED "DOUBLE FREE (Abort)" RESET "\n");
            registrar_caso(p->indice, "DOUBLE FREE", iteracion.backtrace, NULL);
        } else if (iteracion.fue_crash) {
            tests_fallados++;
            if (iteracion.fue_crash == 11) { printf("[" RED "KO" RESET "] " RED "SIGSEGV (Null Pointer)" RESET "\n"); registrar_caso(p->indice, "SIGSEGV", iteracion.backtrace, NULL); }
            else if (iteracion.fue_crash == 6) { printf("[" RED "KO" RESET "] " RED "SIGABRT (Violacion)" RESET "\n"); registrar_caso(p->indice, "SIGABRT", iteracion.backtrace, NULL); }
            else { printf("[" RED "KO" RESET "] " RED "CRASH (Senal %d)" RESET "\n", iteracion.fue_crash); registrar_caso(p->indice, "CRASH", iteracion.backtrace, NULL); }
        } else if (iteracion.bytes_leaked > base.bytes_leaked) {
            tests_fallados++;
            printf("[" RED "KO" RESET "] " YELLOW "LEAK (+%zu bytes colgados)" RESET "\n", bytes_netos);
            registrar_caso(p->indice, "LEAK", iteracion.backtrace, NULL);
        } else {
            printf("[" GREEN "OK" RESET "] " GREEN "OK" RESET "\n");
        }
        free(iteracion.backtrace);
        free(iteracion.salida_colgada);
    }

    printf("---------------------------------------------------------------------------------\n");
    if (tests_fallados == 0 && !problemas_base) {
        printf(" " GREEN "RESULTADO GLOBAL:" RESET " [" GREEN "OK" RESET "] El binario es tolerante a fallos de memoria y su ejecucion base es limpia.\n");
        mostrar_frases_ok_general();
    } else {
        printf(" " RED "RESULTADO GLOBAL:" RESET " [" RED "KO" RESET "] Se encontraron " RED "%d" RESET " puntos criticos de memoria.\n", tests_fallados + problemas_base);
    }
    printf("---------------------------------------------------------------------------------\n");

    mostrar_casos_sospechosos(argv[1]);
    if (tests_fallados > 0 || problemas_base) {
        mostrar_frases_ko_general();
    }

    for (int i = 0; i < total_pruebas_final; i++) free(pruebas[i]);
    free(pruebas);
    return 0;
}