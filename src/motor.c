#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#include "comun.h"
#include "motor.h"

static char ruta_injector[4096] = "./injector.so";
static int hay_entrada_grabada = 0;

void motor_configurar_ruta_injector(const char *ruta) {
    snprintf(ruta_injector, sizeof(ruta_injector), "%s", ruta);
}

int obtener_max_paralelo(void) {
    const char *env = getenv("TORTURETE_JOBS");
    if (env) { int v = atoi(env); if (v > 0) return v; }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    return (int)n;
}

/* ---------- Ejecucion de una prueba de fallo de malloc en segundo plano ----------
 * Se usa unicamente para las pruebas diferidas (fail_after == N concreto),
 * despues de que el programa en vivo ya haya terminado. Se ejecuta en
 * completo silencio (stdin/stdout/stderr a /dev/null, o repitiendo la
 * entrada grabada si la hay): quien quiere ver la ejecucion real es la
 * ejecucion EN VIVO, no estas repeticiones de fondo. El protocolo de
 * eventos (backtrace, metricas) viaja siempre por el fd 3 dedicado.
 * --------------------------------------------------------------------------- */
static ReporteMetricas ejecutar_hijo_y_analizar(int fail_after, char **argv) {
    ReporteMetricas rep = {0, 0, 0, 0, 0, NULL};
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) return rep;
    fflush(stdout); fflush(stderr);

    pid_t pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd != -1) {
            if (hay_entrada_grabada) {
                int entrada_fd = open(FICHERO_ENTRADA_GRABADA, O_RDONLY);
                dup2(entrada_fd != -1 ? entrada_fd : null_fd, STDIN_FILENO);
                if (entrada_fd != -1) close(entrada_fd);
            } else {
                dup2(null_fd, STDIN_FILENO);
            }
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        close(pipe_fd[0]);
        if (pipe_fd[1] != FD_EVENTOS) {
            dup2(pipe_fd[1], FD_EVENTOS);
            close(pipe_fd[1]);
        }
        char fail_val[16];
        snprintf(fail_val, sizeof(fail_val), "%d", fail_after);
        setenv("FAIL_AFTER", fail_val, 1);
        setenv("LD_PRELOAD", ruta_injector, 1);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(*argv, argv);
        _exit(1);
    }
    close(pipe_fd[1]);

    size_t cap_acumulado = 65536;
    char *acumulado = (char *)calloc(cap_acumulado, sizeof(char));
    char *buffer = (char *)calloc(4096, sizeof(char));
    size_t usado = 0;
    ssize_t bytes_read;
    if (acumulado && buffer) {
        while ((bytes_read = read(pipe_fd[0], buffer, 4095)) > 0) {
            buffer[bytes_read] = '\0';
            if (usado + (size_t)bytes_read < cap_acumulado - 1) {
                memcpy(acumulado + usado, buffer, bytes_read);
                usado += bytes_read;
                acumulado[usado] = '\0';
            }
        }
    }
    close(pipe_fd[0]);
    int status; waitpid(pid, &status, 0);

    if (acumulado) {
        char *inicio = strstr(acumulado, "[BACKTRACE-INICIO]|");
        if (inicio) {
            char *fin = strstr(inicio, "[BACKTRACE-FIN]");
            if (fin) {
                size_t len = (size_t)(fin - inicio);
                char *bt = (char *)malloc(len + 1);
                if (bt) { memcpy(bt, inicio, len); bt[len] = '\0'; rep.backtrace = bt; }
            }
        }
    }

    if (WIFSIGNALED(status)) {
        rep.fue_crash = WTERMSIG(status);
    }
    if (acumulado) {
        char *token = strstr(acumulado, "[METRICAS]|");
        if (token) sscanf(token, "[METRICAS]|%d|%d|%d|%zu", &rep.total_mallocs, &rep.total_frees, &rep.total_leaks, &rep.bytes_leaked);
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 11 || code == 6) rep.fue_crash = code;
    }
    free(acumulado); free(buffer); return rep;
}

PruebaDiferida **pruebas = NULL;
int total_pruebas = 0;
static int capacidad_pruebas = 0;
pthread_mutex_t mtx_pruebas = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t mtx_vivo = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_vivo = PTHREAD_COND_INITIALIZER;
static int vivo_terminado = 0;

static char **g_argv_objetivo = NULL;
static sem_t *g_sem_concurrencia = NULL;

/* --- Grabacion de la entrada real, para poder reproducirla en las pruebas
 * diferidas: un programa interactivo necesita las mismas respuestas del
 * usuario para llegar a los mismos mallocs que llego durante la ejecucion
 * en vivo. Se graba en un fichero temporal (no en memoria) para no tener
 * limite de tamaño ni problemas de bloqueo entre varios hijos leyendo a
 * la vez. */
typedef struct {
    int fd_hacia_hijo;   /* extremo de escritura del pipe hacia el hijo en vivo */
    int fd_fichero;      /* fichero donde se va grabando una copia */
} EstadoTeeStdin;

static void *hilo_tee_stdin(void *arg) {
    EstadoTeeStdin *estado = (EstadoTeeStdin *)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        ssize_t escrito = write(estado->fd_hacia_hijo, buf, (size_t)n);
        if (estado->fd_fichero != -1) write(estado->fd_fichero, buf, (size_t)n);
        if (escrito < 0) break; /* el hijo ya no lee (pipe roto): dejamos de reenviar */
    }
    close(estado->fd_hacia_hijo);
    if (estado->fd_fichero != -1) close(estado->fd_fichero);
    free(estado);
    return NULL;
}

static void *hilo_prueba_diferida(void *arg) {
    PruebaDiferida *p = (PruebaDiferida *)arg;

    /* Dormir hasta que el programa en vivo haya terminado del todo. */
    pthread_mutex_lock(&mtx_vivo);
    while (!vivo_terminado) pthread_cond_wait(&cond_vivo, &mtx_vivo);
    pthread_mutex_unlock(&mtx_vivo);

    sem_wait(g_sem_concurrencia);
    p->resultado = ejecutar_hijo_y_analizar(p->indice, g_argv_objetivo);
    sem_post(g_sem_concurrencia);
    return NULL;
}

/* Se llama en tiempo real, segun van llegando los eventos "MALLOC|N" desde
 * el programa en vivo. Crea de inmediato el hilo "dormido" correspondiente. */
static void registrar_malloc_evento(int indice) {
    PruebaDiferida *p = (PruebaDiferida *)malloc(sizeof(PruebaDiferida));
    if (!p) return;
    p->indice = indice;
    p->resultado = (ReporteMetricas){0, 0, 0, 0, 0, NULL};

    pthread_mutex_lock(&mtx_pruebas);
    if (total_pruebas >= capacidad_pruebas) {
        int nueva_cap = capacidad_pruebas ? capacidad_pruebas * 2 : 64;
        PruebaDiferida **nuevo = (PruebaDiferida **)realloc(pruebas, nueva_cap * sizeof(PruebaDiferida *));
        if (!nuevo) { pthread_mutex_unlock(&mtx_pruebas); free(p); return; }
        pruebas = nuevo;
        capacidad_pruebas = nueva_cap;
    }
    int mi_indice = total_pruebas++;
    pruebas[mi_indice] = p;
    pthread_mutex_unlock(&mtx_pruebas);

    pthread_create(&p->hilo, NULL, hilo_prueba_diferida, p);
}

ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo, sem_t *sem_concurrencia) {
    ReporteMetricas rep_vivo = {0, 0, 0, 0, 0, NULL};
    g_argv_objetivo = argv_objetivo;
    g_sem_concurrencia = sem_concurrencia;

    int pipe_eventos[2];
    if (pipe(pipe_eventos) == -1) return rep_vivo;

    int pipe_stdin[2];
    if (pipe(pipe_stdin) == -1) { close(pipe_eventos[0]); close(pipe_eventos[1]); return rep_vivo; }

    unlink(FICHERO_ENTRADA_GRABADA);
    int fd_fichero_entrada = open(FICHERO_ENTRADA_GRABADA, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    fflush(stdout); fflush(stderr);

    pid_t pid = fork();
    if (pid == 0) {
        /* stdout/stderr NO se tocan: heredan la terminal real. stdin, en
         * cambio, se sustituye por el extremo de lectura del pipe-proxy,
         * para poder grabar en paralelo lo que el usuario va escribiendo. */
        close(pipe_stdin[1]);
        if (pipe_stdin[0] != STDIN_FILENO) { dup2(pipe_stdin[0], STDIN_FILENO); close(pipe_stdin[0]); }

        close(pipe_eventos[0]);
        if (pipe_eventos[1] != FD_EVENTOS) {
            dup2(pipe_eventos[1], FD_EVENTOS);
            close(pipe_eventos[1]);
        }
        setenv("FAIL_AFTER", "-1", 1);
        setenv("LD_PRELOAD", ruta_injector, 1);
        /* torturete ignora SIGINT/SIGPIPE para sobrevivir a un ctrl+c del
         * programa objetivo, pero esa disposicion SOBREVIVE al execvp; hay
         * que devolverla a la normal para que el programa objetivo reciba
         * ctrl+c igual que si lo hubieramos lanzado directamente. */
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(*argv_objetivo, argv_objetivo);
        _exit(1);
    }
    close(pipe_eventos[1]);
    close(pipe_stdin[0]);

    /* Hilo que reenvia la entrada real (terminal) hacia el hijo, guardando
     * a la vez una copia en disco para poder repetirla luego. */
    EstadoTeeStdin *estado_tee = (EstadoTeeStdin *)malloc(sizeof(EstadoTeeStdin));
    estado_tee->fd_hacia_hijo = pipe_stdin[1];
    estado_tee->fd_fichero = fd_fichero_entrada;
    pthread_t hilo_tee;
    pthread_create(&hilo_tee, NULL, hilo_tee_stdin, estado_tee);
    pthread_detach(hilo_tee); /* no lo esperamos: si el usuario no ha pulsado
                                 ctrl+d todavia, seguiria bloqueado leyendo de
                                 la terminal mucho despues de que el hijo ya
                                 haya terminado, y no debe frenar el analisis. */

    size_t cap = 65536;
    char *acumulado = (char *)calloc(cap, 1);
    size_t usado = 0, procesado = 0;
    char buf[4096];
    ssize_t n;

    while (acumulado && (n = read(pipe_eventos[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
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

        /* Escaneo incremental: cada linea completa "MALLOC|N" se procesa
         * en el acto, lanzando ya mismo el hilo dormido correspondiente. */
        for (;;) {
            char *inicio_linea = acumulado + procesado;
            char *fin_linea = strchr(inicio_linea, '\n');
            if (!fin_linea) break;
            *fin_linea = '\0';
            if (strncmp(inicio_linea, "MALLOC|", 7) == 0) {
                int idx = atoi(inicio_linea + 7);
                registrar_malloc_evento(idx);
            }
            *fin_linea = '\n';
            procesado = (size_t)(fin_linea - acumulado) + 1;
        }
    }
    close(pipe_eventos[0]);

    int status;
    waitpid(pid, &status, 0);

    if (acumulado) {
        char *inicio = strstr(acumulado, "[BACKTRACE-INICIO]|");
        if (inicio) {
            char *fin = strstr(inicio, "[BACKTRACE-FIN]");
            if (fin) {
                size_t len = (size_t)(fin - inicio);
                char *bt = (char *)malloc(len + 1);
                if (bt) { memcpy(bt, inicio, len); bt[len] = '\0'; rep_vivo.backtrace = bt; }
            }
        }
        char *token = strstr(acumulado, "[METRICAS]|");
        if (token) sscanf(token, "[METRICAS]|%d|%d|%d|%zu", &rep_vivo.total_mallocs, &rep_vivo.total_frees, &rep_vivo.total_leaks, &rep_vivo.bytes_leaked);
    }

    if (WIFSIGNALED(status)) {
        rep_vivo.fue_crash = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 11 || code == 6) rep_vivo.fue_crash = code;
    }
    free(acumulado);

    /* Pequeño margen para que el hilo de grabacion de stdin (que va por su
     * cuenta, sin sincronizarse con el fin del hijo) tenga ocasion de volcar
     * a disco los ultimos bytes que el hijo llego a consumir. No es una
     * garantia perfecta, pero cubre el caso normal de entrada por lineas. */
    usleep(20000);
    struct stat st_entrada;
    if (stat(FICHERO_ENTRADA_GRABADA, &st_entrada) == 0 && st_entrada.st_size > 0) {
        hay_entrada_grabada = 1;
    }

    /* El programa en vivo ya ha terminado: es seguro despertar a todos los
     * hilos dormidos para que reproduzcan sus fallos de malloc. */
    pthread_mutex_lock(&mtx_vivo);
    vivo_terminado = 1;
    pthread_cond_broadcast(&cond_vivo);
    pthread_mutex_unlock(&mtx_vivo);

    pthread_mutex_lock(&mtx_pruebas);
    int total_actual = total_pruebas;
    pthread_mutex_unlock(&mtx_pruebas);
    for (int i = 0; i < total_actual; i++) pthread_join(pruebas[i]->hilo, NULL);

    return rep_vivo;
}
