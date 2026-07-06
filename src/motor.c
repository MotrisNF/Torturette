#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#include "comun.h"
#include "motor.h"

static char ruta_injector[4096] = "./injector.so";

void motor_configurar_ruta_injector(const char *ruta) {
    snprintf(ruta_injector, sizeof(ruta_injector), "%s", ruta);
}

extern char **environ;

/* Construye, ANTES de hacer fork(), el array de variables de entorno que
 * necesitara el proceso objetivo (entorno actual + FAIL_AFTER=-1 +
 * LD_PRELOAD). Se hace en el padre para no tener que llamar a setenv()
 * (que usa malloc internamente) justo despues de un fork, evitando asi
 * cualquier riesgo de bloqueo por locks de malloc heredados. */
#define MAX_ENV_HIJO 512

static void construir_envp_hijo(char **envp_out, size_t max_entradas,
                                 char *buf_fail_after, size_t sz_fail_after,
                                 char *buf_ld_preload, size_t sz_ld_preload) {
    size_t n = 0;
    for (char **e = environ; *e && n < max_entradas - 3; e++) {
        if (strncmp(*e, "FAIL_AFTER=", 11) == 0) continue;
        if (strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
        envp_out[n++] = *e;
    }
    snprintf(buf_fail_after, sz_fail_after, "FAIL_AFTER=-1");
    snprintf(buf_ld_preload, sz_ld_preload, "LD_PRELOAD=%s", ruta_injector);
    envp_out[n++] = buf_fail_after;
    envp_out[n++] = buf_ld_preload;
    envp_out[n] = NULL;
}

PruebaDiferida **pruebas = NULL;
int total_pruebas = 0;
static int capacidad_pruebas = 0;

static void anadir_prueba(int indice, int es_pthread, ReporteMetricas resultado) {
    if (total_pruebas >= capacidad_pruebas) {
        int nueva_cap = capacidad_pruebas ? capacidad_pruebas * 2 : 64;
        PruebaDiferida **nuevo = (PruebaDiferida **)realloc(pruebas, nueva_cap * sizeof(PruebaDiferida *));
        if (!nuevo) {
            /* Memoria agotada para la propia contabilidad de torturete (no
             * del binario objetivo): 'resultado' ya trae backtrace/salida
             * colgada reservados en procesar_eventos() y con la propiedad
             * ya transferida (el llamador se quedo sin su propia
             * referencia) -- si no se guarda esta prueba, hay que
             * liberarlos aqui o se pierden para siempre. */
            free(resultado.backtrace);
            free(resultado.salida_colgada);
            return;
        }
        pruebas = nuevo;
        capacidad_pruebas = nueva_cap;
    }
    PruebaDiferida *p = (PruebaDiferida *)malloc(sizeof(PruebaDiferida));
    if (!p) {
        free(resultado.backtrace);
        free(resultado.salida_colgada);
        return;
    }
    p->indice = indice;
    p->es_pthread = es_pthread;
    p->resultado = resultado;
    pruebas[total_pruebas++] = p;
}

/* Recorre el buffer acumulado del canal de eventos (fd 3) y extrae todos
 * los bloques "[BACKTRACE-INICIO]|<etiqueta>|...[BACKTRACE-FIN]",
 * "[CUELGUE-INICIO]|<etiqueta>...[CUELGUE-FIN]" (lo que un hijo de prueba
 * llego a imprimir antes de ser matado por timeout) y
 * "[METRICAS]|<etiqueta>|mallocs|frees|leaks|bytes|senal" que haya. La
 * etiqueta es "BASE" para la ejecucion en vivo real, o el indice de la
 * prueba de fallo concreta -- con un sufijo "T" si es una prueba de
 * pthread_create() en vez de malloc/calloc/realloc (ver injector.c:
 * fork_y_probar_fallo). Como el injector solo prueba un fallo cada vez (y
 * espera a que termine antes de seguir), como mucho hay un backtrace y/o
 * una salida-colgada "pendiente" de emparejar con sus metricas en
 * cualquier momento: no hace falta almacenarlos todos por indice. */
static void procesar_eventos(char *acumulado, ReporteMetricas *rep_vivo) {
    char *cursor = acumulado;
    char *backtrace_pendiente = NULL;
    char etiqueta_pendiente_bt[32] = "";
    char *salida_pendiente = NULL;
    char etiqueta_pendiente_salida[32] = "";

    for (;;) {
        char *sig_bt = strstr(cursor, "[BACKTRACE-INICIO]|");
        char *sig_cg = strstr(cursor, "[CUELGUE-INICIO]|");
        char *sig_met = strstr(cursor, "[METRICAS]|");

        char *siguiente = sig_bt;
        if (sig_cg && (!siguiente || sig_cg < siguiente)) siguiente = sig_cg;
        if (sig_met && (!siguiente || sig_met < siguiente)) siguiente = sig_met;
        if (!siguiente) break;

        if (siguiente == sig_bt) {
            char *fin = strstr(sig_bt, "[BACKTRACE-FIN]");
            if (!fin) break; /* bloque incompleto: no deberia pasar, pero por seguridad */

            char etiqueta[32] = {0};
            char *p = sig_bt + strlen("[BACKTRACE-INICIO]|");
            char *barra = strchr(p, '|');
            if (barra) {
                size_t len = (size_t)(barra - p);
                if (len > 0 && len < sizeof(etiqueta)) { memcpy(etiqueta, p, len); etiqueta[len] = '\0'; }
            }

            char *fin_bloque = fin + strlen("[BACKTRACE-FIN]");
            if (*fin_bloque == '\n') fin_bloque++;
            size_t len_total = (size_t)(fin_bloque - sig_bt);
            char *bt = (char *)malloc(len_total + 1);
            if (bt) { memcpy(bt, sig_bt, len_total); bt[len_total] = '\0'; }

            free(backtrace_pendiente);
            backtrace_pendiente = bt;
            snprintf(etiqueta_pendiente_bt, sizeof(etiqueta_pendiente_bt), "%s", etiqueta);
            cursor = fin_bloque;
            continue;
        }

        if (siguiente == sig_cg) {
            char *fin = strstr(sig_cg, "[CUELGUE-FIN]");
            if (!fin) break;

            char etiqueta[32] = {0};
            char *p = sig_cg + strlen("[CUELGUE-INICIO]|");
            char *fin_linea = strchr(p, '\n');
            char *inicio_contenido = p;
            if (fin_linea && fin_linea < fin) {
                size_t len = (size_t)(fin_linea - p);
                if (len > 0 && len < sizeof(etiqueta)) { memcpy(etiqueta, p, len); etiqueta[len] = '\0'; }
                inicio_contenido = fin_linea + 1;
            }

            size_t len_contenido = (fin > inicio_contenido) ? (size_t)(fin - inicio_contenido) : 0;
            char *salida = (char *)malloc(len_contenido + 1);
            if (salida) { memcpy(salida, inicio_contenido, len_contenido); salida[len_contenido] = '\0'; }

            free(salida_pendiente);
            salida_pendiente = salida;
            snprintf(etiqueta_pendiente_salida, sizeof(etiqueta_pendiente_salida), "%s", etiqueta);

            char *fin_bloque = fin + strlen("[CUELGUE-FIN]");
            if (*fin_bloque == '\n') fin_bloque++;
            cursor = fin_bloque;
            continue;
        }

        /* siguiente == sig_met:
         * "[METRICAS]|<etiqueta>|<mallocs>|<frees>|<leaks>|<bytes>|<senal>"
         * El campo <senal> es imprescindible para las pruebas (no para
         * BASE): torturete ya no hace waitpid() directo de cada hijo de
         * prueba, asi que la unica forma de saber que uno crasheo es que
         * el propio proceso lo reporte por el canal de eventos. */
        char etiqueta[32] = {0};
        int m = 0, f = 0, l = 0, senal = 0;
        size_t b = 0, bytes_res = 0, bytes_lib = 0;
        sscanf(sig_met, "[METRICAS]|%31[^|]|%d|%d|%d|%zu|%d|%zu|%zu", etiqueta, &m, &f, &l, &b, &senal, &bytes_res, &bytes_lib);
        char *fin_linea = strchr(sig_met, '\n');
        cursor = fin_linea ? fin_linea + 1 : sig_met + strlen(sig_met);

        char *bt_a_usar = NULL, *salida_a_usar = NULL;
        if (strcmp(etiqueta_pendiente_bt, etiqueta) == 0) {
            bt_a_usar = backtrace_pendiente;
            backtrace_pendiente = NULL;
            etiqueta_pendiente_bt[0] = '\0';
        }
        if (strcmp(etiqueta_pendiente_salida, etiqueta) == 0) {
            salida_a_usar = salida_pendiente;
            salida_pendiente = NULL;
            etiqueta_pendiente_salida[0] = '\0';
        }

        if (strcmp(etiqueta, "BASE") == 0) {
            rep_vivo->total_mallocs = m;
            rep_vivo->total_frees = f;
            rep_vivo->total_leaks = l;
            rep_vivo->bytes_leaked = b;
            rep_vivo->bytes_reservados = bytes_res;
            rep_vivo->bytes_liberados = bytes_lib;
            free(rep_vivo->backtrace);
            rep_vivo->backtrace = bt_a_usar;
            free(rep_vivo->salida_colgada);
            rep_vivo->salida_colgada = salida_a_usar;
        } else {
            /* El sufijo "T" (pthread_create) va pegado al numero: atoi()
             * se detiene en el primer caracter no numerico, asi que sigue
             * devolviendo el indice correcto sin tocar nada mas. */
            size_t len_etq = strlen(etiqueta);
            int es_pthread = (len_etq > 0 && etiqueta[len_etq - 1] == 'T');
            ReporteMetricas r = { m, f, l, b, senal, bt_a_usar, salida_a_usar, bytes_res, bytes_lib };
            anadir_prueba(atoi(etiqueta), es_pthread, r);
        }
    }
    free(backtrace_pendiente);
    free(salida_pendiente);
}

ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo) {
    ReporteMetricas rep_vivo = {0, 0, 0, 0, 0, NULL, NULL, 0, 0};

    int pipe_eventos[2];
    if (pipe(pipe_eventos) == -1) return rep_vivo;

    fflush(stdout); fflush(stderr);

    char *envp_hijo[MAX_ENV_HIJO];
    char buf_fail_after[32];
    char buf_ld_preload[4096 + 16];
    construir_envp_hijo(envp_hijo, MAX_ENV_HIJO, buf_fail_after, sizeof(buf_fail_after),
                        buf_ld_preload, sizeof(buf_ld_preload));

    pid_t pid = fork();
    if (pid < 0) {
        /* Recursos agotados: no hay ni para lanzar el programa objetivo
         * una sola vez. No hay nada que probar. */
        close(pipe_eventos[0]);
        close(pipe_eventos[1]);
        fprintf(stderr, RED "[-] No se pudo lanzar el programa objetivo: recursos del sistema agotados (fork fallido).\n" RESET);
        rep_vivo.fue_crash = -1;
        return rep_vivo;
    }
    if (pid == 0) {
        /* stdin/stdout/stderr se heredan tal cual de torturete -- ya no
         * hace falta grabar ni repetir el stdin: cada prueba de fallo de
         * malloc se hace ahora con un fork() dentro del propio proceso
         * objetivo, en el instante exacto de la llamada (ver
         * injector.c), heredando el estado real en lugar de tener que
         * reproducirlo desde el principio. */
        close(pipe_eventos[0]);
        if (pipe_eventos[1] != FD_EVENTOS) {
            dup2(pipe_eventos[1], FD_EVENTOS);
            close(pipe_eventos[1]);
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvpe(*argv_objetivo, argv_objetivo, envp_hijo);
        _exit(1);
    }
    close(pipe_eventos[1]);

    /* Se lee todo el canal de eventos hasta EOF. El EOF llega solo cuando
     * TODOS los procesos que tenian el extremo de escritura abierto (el
     * proceso en vivo, y cualquier hijo de prueba que forkeara) hayan
     * terminado -- y como el injector espera (sin limite de tiempo) a
     * que cada hijo de prueba termine antes de continuar, para cuando el
     * proceso en vivo termine por su cuenta ya no queda nadie mas
     * sosteniendo el pipe abierto. */
    size_t cap = 65536;
    char *acumulado = (char *)calloc(cap, 1);
    size_t usado = 0;
    char buf[4096];
    ssize_t n;
    int pruebas_mostradas = 0;
    int stderr_es_terminal = isatty(STDERR_FILENO);
    while (acumulado && (n = read(pipe_eventos[0], buf, sizeof(buf))) > 0) {
        if (usado + (size_t)n + 1 > cap) {
            size_t nueva_cap = cap;
            while (usado + (size_t)n + 1 > nueva_cap) nueva_cap *= 2;
            char *nuevo = (char *)realloc(acumulado, nueva_cap);
            if (!nuevo) break;
            acumulado = nuevo;
            cap = nueva_cap;
        }
        memcpy(acumulado + usado, buf, n);
        usado += (size_t)n;
        acumulado[usado] = '\0';

        /* Contador de progreso: con el timeout de los hijos de prueba
         * (ver injector.c), una sola prueba puede tardar varios segundos
         * en confirmarse como cuelgue -- sin ningun indicio de que sigue
         * en marcha, es indistinguible de que torturete se ha quedado
         * pillado el mismo. Se cuentan las lineas "[METRICAS]|" ya
         * recibidas (cada una cierra una prueba).
         *
         * El "\r" que reescribe la misma linea SOLO tiene sentido si stderr
         * es una terminal de verdad: si se redirige a un fichero (log,
         * "2>&1 > salida.txt"...) el "\r" no hace nada especial, es un
         * caracter mas, y las actualizaciones se van APILANDO una detras de
         * otra -- con miles de pruebas, un muro gigante de la misma frase
         * repetida, que es justo el tipo de cosa "rara" que no se quiere
         * mostrar. Sin terminal, en vez de eso se avisa solo de vez en
         * cuando (cada 250 pruebas), con salto de linea normal. */
        int pruebas_ahora = 0;
        for (char *p = acumulado; (p = strstr(p, "[METRICAS]|")) != NULL; p += 11) pruebas_ahora++;
        if (pruebas_ahora != pruebas_mostradas) {
            pruebas_mostradas = pruebas_ahora;
            if (stderr_es_terminal) {
                fprintf(stderr, CYAN "\r[*] Sigo vivo, tranquilo -- %d pruebas ya hechas..." RESET, pruebas_mostradas);
                fflush(stderr);
            } else if (pruebas_mostradas % 250 == 0) {
                fprintf(stderr, CYAN "[*] Sigo vivo, tranquilo -- %d pruebas ya hechas...\n" RESET, pruebas_mostradas);
            }
        }
    }
    if (pruebas_mostradas > 0 && stderr_es_terminal) fprintf(stderr, "\n");
    close(pipe_eventos[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status)) {
        rep_vivo.fue_crash = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 11 || code == 6) rep_vivo.fue_crash = code;
    }

    if (acumulado) procesar_eventos(acumulado, &rep_vivo);
    free(acumulado);

    return rep_vivo;
}