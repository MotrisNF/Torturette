#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <execinfo.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>

#define MAX_TRACK 4096
#define MAX_FRAMES 32
#define FD_EVENTOS 3

static void *asignaciones[MAX_TRACK];
static size_t tamaños[MAX_TRACK];
static void *liberados[MAX_TRACK];
static int idx_liberados = 0, idx_malloc = 0, idx_free = 0, global_counter = 0, fail_after = -2;

/* Etiqueta que identifica de quien son las metricas/backtraces que se
 * vuelcan por el canal de eventos (fd 3): "BASE" para la ejecucion en
 * vivo real, o el numero de la llamada de malloc/calloc/realloc concreta
 * cuando este proceso es un "hijo de prueba" (ver mas abajo). */
static char g_etiqueta_proceso[32] = "BASE";
/* 1 si este proceso es un hijo de prueba (nacio de un fork() dentro de
 * malloc/calloc/realloc para comprobar que pasa si ESA llamada falla).
 * Los hijos de prueba nunca vuelven a hacer fork: solo se prueba UNA
 * llamada por hijo, todas las demas se sirven con normalidad. */
static int g_es_prueba = 0;

/* NOTA: se usa pthread_key_t (datos especificos de hilo, TSD) en vez de una
   variable "__thread" (TLS de ELF) porque el TLS de ELF en una biblioteca
   cargada por LD_PRELOAD puede fallar (SIGSEGV) al acceder desde hilos
   creados dinamicamente por el programa objetivo, dependiendo de la
   version de glibc. TSD via pthread_key_t no tiene ese problema. */
static pthread_key_t clave_diagnostico;
static pthread_once_t once_diagnostico = PTHREAD_ONCE_INIT;
static void crear_clave_diagnostico(void) { pthread_key_create(&clave_diagnostico, NULL); }
static int obtener_en_diagnostico(void) {
    pthread_once(&once_diagnostico, crear_clave_diagnostico);
    return (int)(intptr_t)pthread_getspecific(clave_diagnostico);
}
static void fijar_en_diagnostico(int valor) {
    pthread_once(&once_diagnostico, crear_clave_diagnostico);
    pthread_setspecific(clave_diagnostico, (void *)(intptr_t)valor);
}
/* La glibc reserva estructuras internas propias (p.ej. el DTV/TLS de cada
   hilo nuevo, via calloc, tipicamente ~272 bytes en x86-64) cuyo ciclo de
   vida no siempre pasa por un free() visible dentro de la ventana de la
   prueba. Se excluyen del tracking por tamaño, igual que ya se hacia con
   1024/4096. Es una heuristica aproximada (puede variar entre versiones
   de glibc/arquitecturas), no una solucion exacta. */
static int es_tamano_interno_libc(size_t total) {
    return total == 1024 || total == 4096 || total == 272;
}
static pthread_mutex_t mtx_tracking = PTHREAD_MUTEX_INITIALIZER;
/* Mutex APARTE (no mtx_tracking) para serializar el fork-y-espera de las
 * pruebas: si el programa objetivo tiene varios hilos llamando a malloc a
 * la vez, solo se prueba un fallo cada vez, nunca en paralelo -- mas
 * simple y predecible, aunque un pelin mas lento. No se reusa
 * mtx_tracking porque este mutex se mantiene cogido durante todo el
 * fork()+waitpid() (que puede tardar), y registrar_ptr() necesita poder
 * coger mtx_tracking mientras tanto sin bloquearse con esto. */
static pthread_mutex_t mtx_prueba = PTHREAD_MUTEX_INITIALIZER;

/* Proteccion fork-safety: si otro hilo tuviera mtx_tracking cogido justo
 * en el instante del fork(), el hijo heredaria ese mutex bloqueado para
 * siempre (el hilo que lo soltaria no existe en el hijo). Se registran
 * manejadores atfork para que, alrededor de cualquier fork(), mtx_tracking
 * este siempre libre. */
static void atfork_preparar(void) { pthread_mutex_lock(&mtx_tracking); }
static void atfork_padre(void) { pthread_mutex_unlock(&mtx_tracking); }
static void atfork_hijo(void) { pthread_mutex_unlock(&mtx_tracking); }

static void inicializar_entorno(void) {
    if (fail_after == -2) {
        char *env = getenv("FAIL_AFTER");
        fail_after = env ? atoi(env) : -1;
    }
}

/* Vuelca un backtrace legible por el canal de eventos (fd 3), delimitado
   por marcadores que torturete pueda parsear, etiquetado con de quien es
   (BASE o el numero de prueba). Usamos fd 3 en vez de stderr para no
   ensuciar la salida real del programa objetivo. */
static void volcar_backtrace(const char *etiqueta) {
    fijar_en_diagnostico(1);
    void *frames[MAX_FRAMES];
    int n = backtrace(frames, MAX_FRAMES);
    dprintf(FD_EVENTOS, "\n[BACKTRACE-INICIO]|%s|%s\n", g_etiqueta_proceso, etiqueta);
    backtrace_symbols_fd(frames, n, FD_EVENTOS);
    dprintf(FD_EVENTOS, "[BACKTRACE-FIN]\n");
    fijar_en_diagnostico(0);
}

static void manejador_señal(int sig) {
    const char *etiqueta = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "OTRA_SENAL";
    volcar_backtrace(etiqueta);
    int senal_reportada = (sig == SIGSEGV) ? 11 : (sig == SIGABRT) ? 6 : sig;
    /* Se incluye la señal explicitamente: torturete ya no hace waitpid()
     * directo de cada hijo de prueba (nacen dentro de este mismo proceso,
     * no como procesos separados lanzados por torturete), asi que la
     * unica forma de que torturete sepa que este proceso crasheo es que
     * el propio proceso lo diga por el canal de eventos. */
    dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|0|0|%d\n", g_etiqueta_proceso, idx_malloc, idx_free, senal_reportada);
    _exit(sig == SIGSEGV ? 11 : 6);
}

__attribute__((constructor)) static void instalar_manejadores(void) {
    signal(SIGSEGV, manejador_señal);
    signal(SIGABRT, manejador_señal);
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_atfork(atfork_preparar, atfork_padre, atfork_hijo);
}

/* ---------------------------------------------------------------------
 * NUCLEO DEL NUEVO MODELO: en vez de que torturete relance el programa
 * entero desde cero para cada malloc que se quiere probar (con todo lo
 * que eso implica: grabar/repetir stdin, coordinar hilos y procesos
 * externos...), se hace un fork() AQUI MISMO, en el instante exacto de
 * la llamada de malloc/calloc/realloc que toca probar:
 *
 *   - El HIJO hereda el estado EXACTO del programa hasta este punto
 *     (memoria, descriptores de fichero, todo) gracias al propio fork().
 *     Se le fuerza a que ESTA llamada devuelva NULL, se le aisla la
 *     entrada/salida (para no interferir con la terminal real), y sigue
 *     ejecutandose el programa objetivo con toda normalidad a partir de
 *     ahi -- exactamente como si esa unica reserva hubiese fallado.
 *     El hijo NUNCA vuelve a hacer fork (g_es_prueba lo impide): solo se
 *     prueba esa unica llamada; el resto de su ejecucion es real.
 *
 *   - El PADRE (la ejecucion en vivo real) espera a que el hijo termine
 *     (sin limite de tiempo: si el hijo se queda colgado de verdad, el
 *     padre tambien esperara -- se acepto este compromiso a cambio de
 *     simplicidad), y despues continua con la reserva real: para el
 *     padre, esta llamada JAMAS falla.
 *
 * Esto es mas lento que probar todo en paralelo, pero es mucho mas
 * simple y evita de raiz los problemas de recursos (no hay explosion de
 * hilos ni de procesos: como mucho hay un hijo de prueba vivo a la vez).
 * --------------------------------------------------------------------- */
static int intentar_fallar_y_probar(void) {
    if (fail_after != -1 || g_es_prueba) return 0;

    pthread_mutex_lock(&mtx_prueba);

    pthread_mutex_lock(&mtx_tracking);
    int mi_indice = global_counter++;
    idx_malloc++;
    pthread_mutex_unlock(&mtx_tracking);

    fflush(stdout);
    pid_t pid = fork();

    if (pid == 0) {
        /* HIJO DE PRUEBA: esta llamada de malloc/calloc/realloc falla. */
        g_es_prueba = 1;
        snprintf(g_etiqueta_proceso, sizeof(g_etiqueta_proceso), "%d", mi_indice);

        /* Aislar la entrada/salida: no debe interferir con la terminal
         * real de la ejecucion en vivo. Si el programa sigue leyendo
         * stdin despues de este punto, recibira EOF de inmediato. */
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd != -1) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        return 1; /* esta llamada falla, en el hijo */
    }

    if (pid > 0) {
        /* PADRE: esperamos (sin limite de tiempo) a que el hijo de
         * prueba termine -- crash, leak, o final limpio -- antes de
         * seguir con la ejecucion real. El hijo reporta sus propias
         * metricas/backtraces por el canal de eventos al terminar. */
        int status;
        waitpid(pid, &status, 0);
    }
    /* Si fork() fallo (pid < 0), simplemente no se prueba este indice y
     * se sigue con normalidad, como si no hubiera pasado nada. */

    pthread_mutex_unlock(&mtx_prueba);
    return 0; /* el padre nunca falla esta reserva */
}

static void registrar_ptr(void *ptr, size_t size) {
    pthread_mutex_lock(&mtx_tracking);
    for (int i = 0; i < MAX_TRACK; i++) {
        if (asignaciones[i] == NULL) { asignaciones[i] = ptr; tamaños[i] = size; break; }
    }
    for (int i = 0; i < idx_liberados; i++) {
        if (liberados[i] == ptr) { liberados[i] = NULL; break; }
    }
    pthread_mutex_unlock(&mtx_tracking);
}

void *malloc(size_t size) {
    static void *(*real_malloc)(size_t) = NULL;
    static int initializing = 0;
    static char tmp_buffer[64];
    if (!real_malloc) {
        if (initializing) return (void *)tmp_buffer;
        initializing = 1; real_malloc = dlsym(RTLD_NEXT, "malloc"); initializing = 0;
    }
    if (obtener_en_diagnostico()) return real_malloc(size);
    inicializar_entorno();
    if (fail_after != -2 && !es_tamano_interno_libc(size)) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    void *ptr = real_malloc(size);
    if (ptr && fail_after != -2 && !es_tamano_interno_libc(size)) registrar_ptr(ptr, size);
    return ptr;
}

void *calloc(size_t nmemb, size_t size) {
    static void *(*real_calloc)(size_t, size_t) = NULL;
    static int initializing = 0;
    /* dlsym puede necesitar llamar internamente a calloc (p.ej. para
       reservar su propio estado) antes de que real_calloc este resuelto.
       Sin este area de arranque estatica, eso provocaria recursion
       infinita. Se sirve desde aqui, ya puesto a cero, una unica vez. */
    static unsigned char area_arranque[4096];
    static size_t usado_arranque = 0;
    if (!real_calloc) {
        if (initializing) {
            size_t total = nmemb * size;
            if (total == 0) total = 1;
            if (usado_arranque + total > sizeof(area_arranque)) return NULL;
            void *p = area_arranque + usado_arranque;
            usado_arranque += total;
            return p;
        }
        initializing = 1; real_calloc = dlsym(RTLD_NEXT, "calloc"); initializing = 0;
    }
    if (obtener_en_diagnostico()) return real_calloc(nmemb, size);
    inicializar_entorno();
    size_t total = nmemb * size;
    if (fail_after != -2 && !es_tamano_interno_libc(total)) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    void *ptr = real_calloc(nmemb, size);
    if (ptr && fail_after != -2 && !es_tamano_interno_libc(total)) registrar_ptr(ptr, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    static void *(*real_realloc)(void *, size_t) = NULL;
    if (!real_realloc) real_realloc = dlsym(RTLD_NEXT, "realloc");
    if (obtener_en_diagnostico()) return real_realloc(ptr, size);
    inicializar_entorno();
    if (fail_after != -2) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    if (ptr && fail_after != -2) {
        pthread_mutex_lock(&mtx_tracking);
        for (int i = 0; i < MAX_TRACK; i++) {
            if (asignaciones[i] == ptr) { asignaciones[i] = NULL; tamaños[i] = 0; break; }
        }
        pthread_mutex_unlock(&mtx_tracking);
    }
    void *new_ptr = real_realloc(ptr, size);
    if (new_ptr && fail_after != -2) registrar_ptr(new_ptr, size);
    return new_ptr;
}

void free(void *ptr) {
    static void (*real_free)(void *) = NULL;
    if (!real_free) real_free = dlsym(RTLD_NEXT, "free");
    if (obtener_en_diagnostico()) { if (real_free) real_free(ptr); return; }
    inicializar_entorno();
    int es_doble_free = 0;
    if (ptr && fail_after != -2) {
        pthread_mutex_lock(&mtx_tracking);
        for (int i = 0; i < idx_liberados; i++) {
            if (liberados[i] == ptr) { es_doble_free = 1; break; }
        }
        if (!es_doble_free) {
            int encontrado = 0;
            for (int i = 0; i < MAX_TRACK; i++) {
                if (asignaciones[i] == ptr) { asignaciones[i] = NULL; tamaños[i] = 0; encontrado = 1; break; }
            }
            if (encontrado) { idx_free++; if (idx_liberados < MAX_TRACK) { liberados[idx_liberados] = ptr; idx_liberados++; } }
            else { idx_free++; }
        }
        pthread_mutex_unlock(&mtx_tracking);
    }
    if (es_doble_free) {
        volcar_backtrace("DOUBLE_FREE");
        dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|-666|0|6\n", g_etiqueta_proceso, idx_malloc, idx_free);
        _exit(6);
    }
    if (real_free) real_free(ptr);
}

__attribute__((destructor)) void reportar_metricas(void) {
    inicializar_entorno();
    if (fail_after != -2) {
        size_t bytes_colgados = 0;
        int count_leaks = 0;
        pthread_mutex_lock(&mtx_tracking);
        for (int i = 0; i < MAX_TRACK; i++) {
            if (asignaciones[i] != NULL) { bytes_colgados += tamaños[i]; count_leaks++; }
        }
        pthread_mutex_unlock(&mtx_tracking);
        if (count_leaks > 0) volcar_backtrace("LEAK_DETECTADO_AL_SALIR");
        dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|%d|%zu|0\n", g_etiqueta_proceso, idx_malloc, idx_free, count_leaks, bytes_colgados);
    }
}