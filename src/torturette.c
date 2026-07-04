#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#define RESET     "\033[0m"
#define RED       "\033[1;31m"
#define DARKRED   "\033[38;5;88m"
#define GREEN     "\033[1;32m"
#define YELLOW    "\033[1;33m"
#define WHITE     "\033[1;37m"
#define CYAN      "\033[1;36m"

#define FD_EVENTOS 3

/* Fichero donde se graba la entrada real (stdin) que recibio el programa
 * durante su ejecucion en vivo, para poder reproducirsela a cada prueba
 * de fallo de malloc en segundo plano y que asi lleguen a los mismos
 * puntos del programa que la ejecucion real. */
#define FICHERO_ENTRADA_GRABADA "./.torturete_stdin.tmp"
static int hay_entrada_grabada = 0;

const char *injector_source =
"#define _GNU_SOURCE\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <dlfcn.h>\n"
"#include <unistd.h>\n"
"#include <execinfo.h>\n"
"#include <signal.h>\n"
"#include <pthread.h>\n"
"#include <stdint.h>\n"
"\n"
"#define MAX_TRACK 4096\n"
"#define MAX_FRAMES 32\n"
"#define FD_EVENTOS 3\n"
"\n"
"static void *asignaciones[MAX_TRACK];\n"
"static size_t tamaños[MAX_TRACK];\n"
"static void *liberados[MAX_TRACK];\n"
"static int idx_liberados = 0, idx_malloc = 0, idx_free = 0, global_counter = 0, fail_after = -2, ya_fallado = 0;\n"
"/* NOTA: se usa pthread_key_t (datos especificos de hilo, TSD) en vez de una\n"
"   variable \"__thread\" (TLS de ELF) porque el TLS de ELF en una biblioteca\n"
"   cargada por LD_PRELOAD puede fallar (SIGSEGV) al acceder desde hilos\n"
"   creados dinamicamente por el programa objetivo, dependiendo de la\n"
"   version de glibc. TSD via pthread_key_t no tiene ese problema. */\n"
"static pthread_key_t clave_diagnostico;\n"
"static pthread_once_t once_diagnostico = PTHREAD_ONCE_INIT;\n"
"static void crear_clave_diagnostico(void) { pthread_key_create(&clave_diagnostico, NULL); }\n"
"static int obtener_en_diagnostico(void) {\n"
"    pthread_once(&once_diagnostico, crear_clave_diagnostico);\n"
"    return (int)(intptr_t)pthread_getspecific(clave_diagnostico);\n"
"}\n"
"static void fijar_en_diagnostico(int valor) {\n"
"    pthread_once(&once_diagnostico, crear_clave_diagnostico);\n"
"    pthread_setspecific(clave_diagnostico, (void *)(intptr_t)valor);\n"
"}\n"
"/* La glibc reserva estructuras internas propias (p.ej. el DTV/TLS de cada\n"
"   hilo nuevo, via calloc, tipicamente ~272 bytes en x86-64) cuyo ciclo de\n"
"   vida no siempre pasa por un free() visible dentro de la ventana de la\n"
"   prueba. Se excluyen del tracking por tamaño, igual que ya se hacia con\n"
"   1024/4096. Es una heuristica aproximada (puede variar entre versiones\n"
"   de glibc/arquitecturas), no una solucion exacta. */\n"
"static int es_tamano_interno_libc(size_t total) {\n"
"    return total == 1024 || total == 4096 || total == 272;\n"
"}\n"
"static pthread_mutex_t mtx_tracking = PTHREAD_MUTEX_INITIALIZER;\n"
"\n"
"static void inicializar_entorno(void) {\n"
"    if (fail_after == -2) {\n"
"        char *env = getenv(\"FAIL_AFTER\");\n"
"        fail_after = env ? atoi(env) : -1;\n"
"    }\n"
"}\n"
"\n"
"/* Vuelca un backtrace legible por el canal de eventos (fd 3), delimitado\n"
"   por marcadores que torturete pueda parsear. Usamos fd 3 en vez de\n"
"   stderr para no ensuciar la salida real del programa objetivo, que en\n"
"   modo EN VIVO se muestra tal cual al usuario. */\n"
"static void volcar_backtrace(const char *etiqueta) {\n"
"    fijar_en_diagnostico(1);\n"
"    void *frames[MAX_FRAMES];\n"
"    int n = backtrace(frames, MAX_FRAMES);\n"
"    dprintf(FD_EVENTOS, \"\\n[BACKTRACE-INICIO]|%s\\n\", etiqueta);\n"
"    backtrace_symbols_fd(frames, n, FD_EVENTOS);\n"
"    dprintf(FD_EVENTOS, \"[BACKTRACE-FIN]\\n\");\n"
"    fijar_en_diagnostico(0);\n"
"}\n"
"\n"
"static void manejador_señal(int sig) {\n"
"    const char *etiqueta = (sig == SIGSEGV) ? \"SIGSEGV\" : (sig == SIGABRT) ? \"SIGABRT\" : \"OTRA_SENAL\";\n"
"    volcar_backtrace(etiqueta);\n"
"    dprintf(FD_EVENTOS, \"\\n[METRICAS]|%d|%d|%d|0\\n\", idx_malloc, idx_free, fail_after);\n"
"    _exit(sig == SIGSEGV ? 11 : 6);\n"
"}\n"
"\n"
"__attribute__((constructor)) static void instalar_manejadores(void) {\n"
"    signal(SIGSEGV, manejador_señal);\n"
"    signal(SIGABRT, manejador_señal);\n"
"}\n"
"\n"
"/* Punto unico de decision, bajo mutex, para saber si la llamada actual de\n"
"   malloc/calloc/realloc debe fallar.\n"
"   - Modo EN VIVO (fail_after == -1): NUNCA falla nada. El programa\n"
"     objetivo se ejecuta con normalidad, con su entrada/salida reales.\n"
"     Cada llamada se anuncia por el canal de eventos (fd 3) como\n"
"     \"MALLOC|<numero>\", para que torturete pueda lanzar en el acto el\n"
"     hilo \"dormido\" que mas tarde reproducira ese fallo concreto.\n"
"   - Modo PRUEBA (fail_after == N): hace fallar exactamente la llamada\n"
"     numero N (y solo esa), igual que antes. */\n"
"static int intentar_fallar(void) {\n"
"    int debe_fallar = 0;\n"
"    if (fail_after != -2) {\n"
"        pthread_mutex_lock(&mtx_tracking);\n"
"        if (!ya_fallado) {\n"
"            int current_call = global_counter++;\n"
"            idx_malloc++;\n"
"            if (fail_after == -1) {\n"
"                dprintf(FD_EVENTOS, \"MALLOC|%d\\n\", current_call);\n"
"            } else if (current_call == fail_after) {\n"
"                ya_fallado = 1;\n"
"                idx_malloc--;\n"
"                debe_fallar = 1;\n"
"            }\n"
"        }\n"
"        pthread_mutex_unlock(&mtx_tracking);\n"
"    }\n"
"    return debe_fallar;\n"
"}\n"
"\n"
"static void registrar_ptr(void *ptr, size_t size) {\n"
"    pthread_mutex_lock(&mtx_tracking);\n"
"    for (int i = 0; i < MAX_TRACK; i++) {\n"
"        if (asignaciones[i] == NULL) { asignaciones[i] = ptr; tamaños[i] = size; break; }\n"
"    }\n"
"    for (int i = 0; i < idx_liberados; i++) {\n"
"        if (liberados[i] == ptr) { liberados[i] = NULL; break; }\n"
"    }\n"
"    pthread_mutex_unlock(&mtx_tracking);\n"
"}\n"
"\n"
"void *malloc(size_t size) {\n"
"    static void *(*real_malloc)(size_t) = NULL;\n"
"    static int initializing = 0;\n"
"    static char tmp_buffer[64];\n"
"    if (!real_malloc) {\n"
"        if (initializing) return (void *)tmp_buffer;\n"
"        initializing = 1; real_malloc = dlsym(RTLD_NEXT, \"malloc\"); initializing = 0;\n"
"    }\n"
"    if (obtener_en_diagnostico()) return real_malloc(size);\n"
"    inicializar_entorno();\n"
"    if (fail_after != -2 && !es_tamano_interno_libc(size)) {\n"
"        if (intentar_fallar()) { volcar_backtrace(\"MALLOC_FALLIDO\"); return NULL; }\n"
"    }\n"
"    void *ptr = real_malloc(size);\n"
"    if (ptr && fail_after != -2 && !es_tamano_interno_libc(size)) registrar_ptr(ptr, size);\n"
"    return ptr;\n"
"}\n"
"\n"
"void *calloc(size_t nmemb, size_t size) {\n"
"    static void *(*real_calloc)(size_t, size_t) = NULL;\n"
"    static int initializing = 0;\n"
"    /* dlsym puede necesitar llamar internamente a calloc (p.ej. para\n"
"       reservar su propio estado) antes de que real_calloc este resuelto.\n"
"       Sin este area de arranque estatica, eso provocaria recursion\n"
"       infinita. Se sirve desde aqui, ya puesto a cero, una unica vez. */\n"
"    static unsigned char area_arranque[4096];\n"
"    static size_t usado_arranque = 0;\n"
"    if (!real_calloc) {\n"
"        if (initializing) {\n"
"            size_t total = nmemb * size;\n"
"            if (total == 0) total = 1;\n"
"            if (usado_arranque + total > sizeof(area_arranque)) return NULL;\n"
"            void *p = area_arranque + usado_arranque;\n"
"            usado_arranque += total;\n"
"            return p;\n"
"        }\n"
"        initializing = 1; real_calloc = dlsym(RTLD_NEXT, \"calloc\"); initializing = 0;\n"
"    }\n"
"    if (obtener_en_diagnostico()) return real_calloc(nmemb, size);\n"
"    inicializar_entorno();\n"
"    size_t total = nmemb * size;\n"
"    if (fail_after != -2 && !es_tamano_interno_libc(total)) {\n"
"        if (intentar_fallar()) { volcar_backtrace(\"CALLOC_FALLIDO\"); return NULL; }\n"
"    }\n"
"    void *ptr = real_calloc(nmemb, size);\n"
"    if (ptr && fail_after != -2 && !es_tamano_interno_libc(total)) registrar_ptr(ptr, total);\n"
"    return ptr;\n"
"}\n"
"\n"
"void *realloc(void *ptr, size_t size) {\n"
"    static void *(*real_realloc)(void *, size_t) = NULL;\n"
"    if (!real_realloc) real_realloc = dlsym(RTLD_NEXT, \"realloc\");\n"
"    if (obtener_en_diagnostico()) return real_realloc(ptr, size);\n"
"    inicializar_entorno();\n"
"    if (fail_after != -2) {\n"
"        if (intentar_fallar()) { volcar_backtrace(\"REALLOC_FALLIDO\"); return NULL; }\n"
"    }\n"
"    if (ptr && fail_after != -2) {\n"
"        pthread_mutex_lock(&mtx_tracking);\n"
"        for (int i = 0; i < MAX_TRACK; i++) {\n"
"            if (asignaciones[i] == ptr) { asignaciones[i] = NULL; tamaños[i] = 0; break; }\n"
"        }\n"
"        pthread_mutex_unlock(&mtx_tracking);\n"
"    }\n"
"    void *new_ptr = real_realloc(ptr, size);\n"
"    if (new_ptr && fail_after != -2) registrar_ptr(new_ptr, size);\n"
"    return new_ptr;\n"
"}\n"
"\n"
"void free(void *ptr) {\n"
"    static void (*real_free)(void *) = NULL;\n"
"    if (!real_free) real_free = dlsym(RTLD_NEXT, \"free\");\n"
"    if (obtener_en_diagnostico()) { if (real_free) real_free(ptr); return; }\n"
"    inicializar_entorno();\n"
"    int es_doble_free = 0;\n"
"    if (ptr && fail_after != -2) {\n"
"        pthread_mutex_lock(&mtx_tracking);\n"
"        for (int i = 0; i < idx_liberados; i++) {\n"
"            if (liberados[i] == ptr) { es_doble_free = 1; break; }\n"
"        }\n"
"        if (!es_doble_free) {\n"
"            int encontrado = 0;\n"
"            for (int i = 0; i < MAX_TRACK; i++) {\n"
"                if (asignaciones[i] == ptr) { asignaciones[i] = NULL; tamaños[i] = 0; encontrado = 1; break; }\n"
"            }\n"
"            if (encontrado) { idx_free++; if (idx_liberados < MAX_TRACK) { liberados[idx_liberados] = ptr; idx_liberados++; } }\n"
"            else { idx_free++; }\n"
"        }\n"
"        pthread_mutex_unlock(&mtx_tracking);\n"
"    }\n"
"    if (es_doble_free) {\n"
"        volcar_backtrace(\"DOUBLE_FREE\");\n"
"        dprintf(FD_EVENTOS, \"\\n[METRICAS]|%d|%d|-666|0\\n\", idx_malloc, idx_free);\n"
"        _exit(6);\n"
"    }\n"
"    if (real_free) real_free(ptr);\n"
"}\n"
"\n"
"__attribute__((destructor)) void reportar_metricas(void) {\n"
"    inicializar_entorno();\n"
"    if (fail_after != -2) {\n"
"        size_t bytes_colgados = 0;\n"
"        int count_leaks = 0;\n"
"        pthread_mutex_lock(&mtx_tracking);\n"
"        for (int i = 0; i < MAX_TRACK; i++) {\n"
"            if (asignaciones[i] != NULL) { bytes_colgados += tamaños[i]; count_leaks++; }\n"
"        }\n"
"        pthread_mutex_unlock(&mtx_tracking);\n"
"        if (count_leaks > 0) volcar_backtrace(\"LEAK_DETECTADO_AL_SALIR\");\n"
"        dprintf(FD_EVENTOS, \"\\n[METRICAS]|%d|%d|%d|%zu\\n\", idx_malloc, idx_free, count_leaks, bytes_colgados);\n"
"    }\n"
"}\n"
"\n";


void crear_injector(void) {
    FILE *f = fopen("injector.c", "w");
    if (!f) { perror("[-] Error creando archivo temporal"); exit(1); }
    fputs(injector_source, f);
    fclose(f);
    int ret = system("gcc -shared -fPIC -o injector.so injector.c -ldl -lpthread > /dev/null 2>&1");
    unlink("injector.c");
    if (ret != 0) { fprintf(stderr, "[-] Error al compilar injector.so.\n"); exit(1); }
}

typedef struct { int total_mallocs; int total_frees; int total_leaks; size_t bytes_leaked; int fue_crash; char *backtrace; } ReporteMetricas;

#define MAX_CASOS 65536
typedef struct {
    int fail_after;
    char motivo[64];
    char *backtrace;
} CasoSospechoso;

static CasoSospechoso casos[MAX_CASOS];
static int total_casos = 0;
static pthread_mutex_t mtx_casos = PTHREAD_MUTEX_INITIALIZER;

static void registrar_caso(int fail_after, const char *motivo, const char *backtrace) {
    pthread_mutex_lock(&mtx_casos);
    if (total_casos < MAX_CASOS) {
        casos[total_casos].fail_after = fail_after;
        snprintf(casos[total_casos].motivo, sizeof(casos[total_casos].motivo), "%s", motivo);
        casos[total_casos].backtrace = backtrace ? strdup(backtrace) : NULL;
        total_casos++;
    }
    pthread_mutex_unlock(&mtx_casos);
}

/* ---------- Ejecucion de una prueba de fallo de malloc en segundo plano ----------
 * Se usa unicamente para las pruebas diferidas (fail_after == N concreto),
 * despues de que el programa en vivo ya haya terminado. Se ejecuta en
 * completo silencio (stdin/stdout/stderr a /dev/null): quien quiere ver la
 * ejecucion real es la ejecucion EN VIVO de mas abajo, no estas repeticiones
 * de fondo. El protocolo de eventos (backtrace, metricas) viaja siempre por
 * el fd 3 dedicado, nunca por stderr, para no mezclarse con nada.
 * --------------------------------------------------------------------------- */
ReporteMetricas ejecutar_hijo_y_analizar(int fail_after, char **argv) {
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
        setenv("LD_PRELOAD", "./injector.so", 1);
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

#define MAX_FRAMES_MOSTRADOS 2

void mostrar_backtrace(int fail_after, const char *motivo, const char *backtrace_crudo, const char *binario) {
    if (fail_after == -1) {
        printf(CYAN "\n>>> Ejecucion en vivo (base) -- %s\n" RESET, motivo);
    } else {
        printf(CYAN "\n>>> Caso FAIL_AFTER=%d (%s)\n" RESET, fail_after, motivo);
    }
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);

    if (!backtrace_crudo) {
        printf(YELLOW "[!] El injector no capturo backtrace para este caso (¿senal no manejada o salida inesperada?).\n\n" RESET);
        return;
    }

    char *copia = strdup(backtrace_crudo);
    if (!copia) { printf(RED "[!] Sin memoria para procesar el backtrace.\n\n" RESET); return; }

    const char *etiquetas[MAX_FRAMES_MOSTRADOS] = { "Ocurre en", "Llamado desde" };
    int mostrados = 0;

    char *linea = strtok(copia, "\n");
    int primera = 1;
    while (linea != NULL && mostrados < MAX_FRAMES_MOSTRADOS) {
        if (primera) { primera = 0; linea = strtok(NULL, "\n"); continue; }
        if (strstr(linea, binario) == NULL) { linea = strtok(NULL, "\n"); continue; }

        char *ab = strrchr(linea, '[');
        char *cb = ab ? strchr(ab, ']') : NULL;
        char resuelto[512] = {0};
        int resuelto_ok = 0;

        if (ab && cb && cb > ab + 1) {
            char addr[64] = {0};
            size_t len = (size_t)(cb - ab - 1);
            if (len > 0 && len < sizeof(addr)) {
                memcpy(addr, ab + 1, len);
                addr[len] = '\0';
                if (addr[0] == '0' && (addr[1] == 'x' || addr[1] == 'X')) {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "addr2line -e '%s' -f -C -p %s 2>/dev/null", binario, addr);
                    FILE *p = popen(cmd, "r");
                    if (p) {
                        if (fgets(resuelto, sizeof(resuelto), p)) {
                            if (strstr(resuelto, "??:0") == NULL && strstr(resuelto, "?? ") == NULL) {
                                size_t rl = strlen(resuelto);
                                if (rl > 0 && resuelto[rl - 1] == '\n') resuelto[rl - 1] = '\0';
                                resuelto_ok = 1;
                            }
                        }
                        pclose(p);
                    }
                }
            }
        }

        if (resuelto_ok) {
            printf("    " WHITE "%-14s" RESET GREEN " %s" RESET "\n", etiquetas[mostrados], resuelto);
            mostrados++;
        }
        linea = strtok(NULL, "\n");
    }

    if (mostrados == 0) {
        printf(YELLOW "[!] No se pudo resolver ninguna linea.\n" RESET);
        printf(YELLOW "    Compila el binario objetivo con: gcc -g -no-pie -o <programa> <fuente>.c\n" RESET);
    }

    free(copia);
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);
}

/* ================================================================================
 *  MOTOR EN VIVO: el programa objetivo se ejecuta UNA sola vez, de forma
 *  normal (con su stdin/stdout/stderr reales conectados a la terminal).
 *  Cada llamada a malloc/calloc/realloc que haga, se anuncia en tiempo real
 *  por el canal de eventos (fd 3). Por cada anuncio, se lanza AQUI MISMO un
 *  hilo que se queda dormido esperando a que el programa objetivo termine
 *  (de forma normal, por Ctrl+D interpretado por el propio programa, por
 *  Ctrl+C, o por un crash real). En cuanto termina, todos esos hilos
 *  despiertan a la vez y cada uno reproduce -en un proceso hijo aparte y en
 *  segundo plano- que pasaria si esa llamada concreta hubiese fallado.
 *  Un semaforo limita cuantas de esas reproducciones corren a la vez.
 * ================================================================================ */

typedef struct {
    int indice;
    pthread_t hilo;
    ReporteMetricas resultado;
} PruebaDiferida;

static PruebaDiferida **pruebas = NULL;
static int total_pruebas = 0;
static int capacidad_pruebas = 0;
static pthread_mutex_t mtx_pruebas = PTHREAD_MUTEX_INITIALIZER;

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

void *hilo_tee_stdin(void *arg) {
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

void *hilo_prueba_diferida(void *arg) {
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

int obtener_max_paralelo(void) {
    const char *env = getenv("TORTURETE_JOBS");
    if (env) { int v = atoi(env); if (v > 0) return v; }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    return (int)n;
}

/* Ejecuta el programa objetivo EN VIVO (stdin/stdout/stderr reales), lanza
 * en tiempo real los hilos diferidos por cada malloc/calloc/realloc, y
 * cuando el programa termina (por el motivo que sea) despierta a todos
 * esos hilos y espera a que acaben. Devuelve el resultado de la propia
 * ejecucion en vivo (crash/leak/double-free en el comportamiento base). */
ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo) {
    ReporteMetricas rep_vivo = {0, 0, 0, 0, 0, NULL};

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
        setenv("LD_PRELOAD", "./injector.so", 1);
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

void mostrar_banner(void) {
    printf(DARKRED);
    printf(" _______ ____  _____ _______ _    _ _____  ______ _______ ______ \n");
    printf("|__   __/ __ \\|  __ \\__   __| |  | |  __ \\|  ____|__   __|  ____|\n");
    printf("   | | | |  | | |__) | | |  | |  | | |__) | |__     | |  | |__   \n");
    printf(GREEN);
    printf("   | | | |  | |  _  /  | |  | |  | |  _  /|  __|    | |  |  __|  \n");
    printf("   | | | |__| | | \\ \\  | |  | |__| | | \\ \\| |____   | |  | |____ \n");
    printf("   |_|  \\____/|_|  \\_\\ |_|   \\____/|_|  \\_\\______|  |_|  |______|\n");
    printf(RESET "\n");
}

static int comparar_pruebas(const void *a, const void *b) {
    const PruebaDiferida *pa = *(PruebaDiferida * const *)a;
    const PruebaDiferida *pb = *(PruebaDiferida * const *)b;
    return pa->indice - pb->indice;
}

int main(int argc, char **argv) {
    signal(SIGINT, SIG_IGN); /* si el usuario pulsa Ctrl+C para el programa objetivo,
                                 que no mate tambien a torturete antes de analizar. */
    signal(SIGPIPE, SIG_IGN); /* el hilo que graba stdin puede escribir a un pipe ya
                                  cerrado si el hijo termina antes de consumir toda la
                                  entrada; sin esto, ese write() mataria a torturete. */
    mostrar_banner();
    if (argc < 2) { printf("Uso: %s <programa_objetivo> [argumentos...]\n", *argv); return 1; }
    unlink("injector.so"); crear_injector();

    int max_paralelo = obtener_max_paralelo();
    sem_t sem;
    sem_init(&sem, 0, max_paralelo);
    g_sem_concurrencia = &sem;
    g_argv_objetivo = (argv + 1);

    printf(YELLOW "Ejecutando el programa objetivo de forma normal...\n" RESET);
    printf(CYAN "(Entrada y salida reales; las pruebas de fallo de malloc se ejecutan en\n" RESET);
    printf(CYAN " segundo plano al terminar, hasta %d a la vez -- ajustable con TORTURETE_JOBS)\n" RESET, max_paralelo);
    printf("---------------------------------------------------------------------------------\n");

    ReporteMetricas base = ejecutar_vivo_y_lanzar_diferidas(argv + 1);

    printf("---------------------------------------------------------------------------------\n");

    if (base.total_leaks == -666) {
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- DOUBLE FREE durante la ejecucion normal.\n");
        mostrar_backtrace(-1, "DOUBLE FREE EN EJECUCION BASE", base.backtrace, argv[1]);
    } else if (base.fue_crash == 2) {
        printf("[" YELLOW "EJECUCION BASE" RESET "] Interrumpida por el usuario (Ctrl+C).\n");
    } else if (base.fue_crash) {
        const char *motivo = "CRASH";
        if (base.fue_crash == 11) motivo = "SIGSEGV (Null Pointer / Use-After-Free)";
        else if (base.fue_crash == 6) motivo = "SIGABRT (Violacion de memoria)";
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- %s (senal %d).\n", motivo, base.fue_crash);
        mostrar_backtrace(-1, motivo, base.backtrace, argv[1]);
    } else if (base.total_leaks > 0) {
        printf("[" RED "EJECUCION BASE" RESET "] " RED "KO" RESET " -- %d malloc / %d free, " RED "%zu bytes colgados" RESET ".\n",
               base.total_mallocs, base.total_frees, base.bytes_leaked);
        mostrar_backtrace(-1, "LEAK EN EJECUCION BASE", base.backtrace, argv[1]);
    } else {
        printf("[" GREEN "EJECUCION BASE" RESET "] " GREEN "OK" RESET " -- %d malloc / %d free, sin fugas.\n",
               base.total_mallocs, base.total_frees);
    }
    free(base.backtrace);

    pthread_mutex_lock(&mtx_pruebas);
    int total_pruebas_final = total_pruebas;
    pthread_mutex_unlock(&mtx_pruebas);

    if (total_pruebas_final == 0) {
        printf(YELLOW "\nNo se detecto ninguna llamada a malloc/calloc/realloc durante la ejecucion.\n" RESET);
        sem_destroy(&sem); unlink("injector.so"); unlink(FICHERO_ENTRADA_GRABADA); return 0;
    }

    qsort(pruebas, total_pruebas_final, sizeof(PruebaDiferida *), comparar_pruebas);

    printf(YELLOW "\nResultados de las %d pruebas de fallo de malloc (en paralelo, tras el fin del programa)\n" RESET, total_pruebas_final);
    printf("---------------------------------------------------------------------------------\n");
    printf("%-10s | %-16s | %-9s | %-7s | %-13s | %-10s\n", "PRUEBA", "ESCENARIO", "MALLOCS", "FREES", "MEMORIA", "ESTADO");
    printf("---------------------------------------------------------------------------------\n");

    int tests_fallados = 0;
    for (int i = 0; i < total_pruebas_final; i++) {
        PruebaDiferida *p = pruebas[i];
        ReporteMetricas iteracion = p->resultado;
        char num_prueba[16], escenario[32];
        snprintf(num_prueba, sizeof(num_prueba), "#%02d", p->indice + 1);
        snprintf(escenario, sizeof(escenario), "Falla Malloc %d", p->indice + 1);

        size_t bytes_netos = 0;
        if (iteracion.bytes_leaked > base.bytes_leaked) bytes_netos = iteracion.bytes_leaked - base.bytes_leaked;

        printf("%-10s | %-16s | " WHITE "%-9d" RESET " | " WHITE "%-7d" RESET " | %-10zu bytes | ",
               num_prueba, escenario, iteracion.total_mallocs, iteracion.total_frees, bytes_netos);

        if (iteracion.total_leaks == -666) {
            tests_fallados++;
            printf("[" RED "KO" RESET "] " RED "DOUBLE FREE (Abort)" RESET "\n");
            registrar_caso(p->indice, "DOUBLE FREE", iteracion.backtrace);
        } else if (iteracion.fue_crash) {
            tests_fallados++;
            if (iteracion.fue_crash == 11) { printf("[" RED "KO" RESET "] " RED "SIGSEGV (Null Pointer)" RESET "\n"); registrar_caso(p->indice, "SIGSEGV", iteracion.backtrace); }
            else if (iteracion.fue_crash == 6) { printf("[" RED "KO" RESET "] " RED "SIGABRT (Violacion)" RESET "\n"); registrar_caso(p->indice, "SIGABRT", iteracion.backtrace); }
            else { printf("[" RED "KO" RESET "] " RED "CRASH (Senal %d)" RESET "\n", iteracion.fue_crash); registrar_caso(p->indice, "CRASH", iteracion.backtrace); }
        } else if (iteracion.bytes_leaked > base.bytes_leaked) {
            tests_fallados++;
            printf("[" RED "KO" RESET "] " YELLOW "LEAK (+%zu bytes colgados)" RESET "\n", bytes_netos);
            registrar_caso(p->indice, "LEAK", iteracion.backtrace);
        } else {
            printf("[" GREEN "OK" RESET "] " GREEN "OK" RESET "\n");
        }
        free(iteracion.backtrace);
    }

    printf("---------------------------------------------------------------------------------\n");
    int problemas_base = (base.total_leaks == -666 || (base.fue_crash && base.fue_crash != 2)) ? 1 : (base.total_leaks > 0 ? 1 : 0);
    if (tests_fallados == 0 && !problemas_base) {
        printf(" " GREEN "RESULTADO GLOBAL:" RESET " [" GREEN "OK" RESET "] El binario es tolerante a fallos de memoria y su ejecucion base es limpia.\n");
    } else {
        printf(" " RED "RESULTADO GLOBAL:" RESET " [" RED "KO" RESET "] Se encontraron " RED "%d" RESET " puntos criticos de memoria.\n", tests_fallados + problemas_base);
    }
    printf("---------------------------------------------------------------------------------\n");

    if (total_casos > 0) {
        printf("\n" YELLOW "===================================================================================\n" RESET);
        printf(YELLOW "  ANALISIS: %d caso(s) sospechoso(s) -- backtrace capturado en el momento del fallo\n" RESET, total_casos);
        printf(YELLOW "===================================================================================\n" RESET);
        printf("(Para ver archivo:linea exactos compila con: gcc -g -no-pie -o <programa> <fuente>.c)\n");
        for (int i = 0; i < total_casos; i++) {
            mostrar_backtrace(casos[i].fail_after, casos[i].motivo, casos[i].backtrace, argv[1]);
            free(casos[i].backtrace);
        }
    } else {
        printf("\n" GREEN "No hay casos sospechosos que analizar (fuera de la ejecucion base).\n" RESET);
    }

    for (int i = 0; i < total_pruebas_final; i++) free(pruebas[i]);
    free(pruebas);
    sem_destroy(&sem);
    unlink("injector.so");
    unlink(FICHERO_ENTRADA_GRABADA);
    return 0;
}