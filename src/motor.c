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

static void anadir_prueba(int indice, ReporteMetricas resultado) {
    if (total_pruebas >= capacidad_pruebas) {
        int nueva_cap = capacidad_pruebas ? capacidad_pruebas * 2 : 64;
        PruebaDiferida **nuevo = (PruebaDiferida **)realloc(pruebas, nueva_cap * sizeof(PruebaDiferida *));
        if (!nuevo) return;
        pruebas = nuevo;
        capacidad_pruebas = nueva_cap;
    }
    PruebaDiferida *p = (PruebaDiferida *)malloc(sizeof(PruebaDiferida));
    if (!p) return;
    p->indice = indice;
    p->resultado = resultado;
    pruebas[total_pruebas++] = p;
}

/* Recorre el buffer acumulado del canal de eventos (fd 3) y extrae todos
 * los bloques "[BACKTRACE-INICIO]|<etiqueta>|...[BACKTRACE-FIN]" y
 * "[METRICAS]|<etiqueta>|mallocs|frees|leaks|bytes" que haya. La etiqueta
 * es "BASE" para la ejecucion en vivo real, o el numero de la prueba de
 * fallo de malloc concreta. Como el injector solo prueba un fallo cada
 * vez (y espera a que termine antes de seguir -- ver injector.c), como
 * mucho hay un backtrace "pendiente" de emparejar con sus metricas en
 * cualquier momento: no hace falta almacenarlos todos por indice. */
static void procesar_eventos(char *acumulado, ReporteMetricas *rep_vivo) {
    char *cursor = acumulado;
    char *backtrace_pendiente = NULL;
    char etiqueta_pendiente[32] = "";

    for (;;) {
        char *sig_bt = strstr(cursor, "[BACKTRACE-INICIO]|");
        char *sig_met = strstr(cursor, "[METRICAS]|");
        if (!sig_bt && !sig_met) break;

        if (sig_bt && (!sig_met || sig_bt < sig_met)) {
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
            snprintf(etiqueta_pendiente, sizeof(etiqueta_pendiente), "%s", etiqueta);
            cursor = fin_bloque;
            continue;
        }

        /* sig_met: "[METRICAS]|<etiqueta>|<mallocs>|<frees>|<leaks>|<bytes>|<senal>"
         * El campo <senal> es imprescindible para las pruebas (no para
         * BASE): torturete ya no hace waitpid() directo de cada hijo de
         * prueba, asi que la unica forma de saber que uno crasheo es que
         * el propio proceso lo reporte por el canal de eventos. */
        char etiqueta[32] = {0};
        int m = 0, f = 0, l = 0, senal = 0;
        size_t b = 0;
        sscanf(sig_met, "[METRICAS]|%31[^|]|%d|%d|%d|%zu|%d", etiqueta, &m, &f, &l, &b, &senal);
        char *fin_linea = strchr(sig_met, '\n');
        cursor = fin_linea ? fin_linea + 1 : sig_met + strlen(sig_met);

        char *bt_a_usar = NULL;
        if (strcmp(etiqueta_pendiente, etiqueta) == 0) {
            bt_a_usar = backtrace_pendiente;
            backtrace_pendiente = NULL;
            etiqueta_pendiente[0] = '\0';
        }

        if (strcmp(etiqueta, "BASE") == 0) {
            rep_vivo->total_mallocs = m;
            rep_vivo->total_frees = f;
            rep_vivo->total_leaks = l;
            rep_vivo->bytes_leaked = b;
            free(rep_vivo->backtrace);
            rep_vivo->backtrace = bt_a_usar;
        } else {
            ReporteMetricas r = { m, f, l, b, senal, bt_a_usar };
            anadir_prueba(atoi(etiqueta), r);
        }
    }
    free(backtrace_pendiente);
}

ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo) {
    ReporteMetricas rep_vivo = {0, 0, 0, 0, 0, NULL};

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
    }
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