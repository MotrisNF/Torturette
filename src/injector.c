#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
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
static int idx_liberados = 0, idx_malloc = 0, idx_free = 0, global_counter = 0, fail_after = -2, ya_fallado = 0;
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

static void inicializar_entorno(void) {
    if (fail_after == -2) {
        char *env = getenv("FAIL_AFTER");
        fail_after = env ? atoi(env) : -1;
    }
}

/* Vuelca un backtrace legible por el canal de eventos (fd 3), delimitado
   por marcadores que torturete pueda parsear. Usamos fd 3 en vez de
   stderr para no ensuciar la salida real del programa objetivo, que en
   modo EN VIVO se muestra tal cual al usuario. */
static void volcar_backtrace(const char *etiqueta) {
    fijar_en_diagnostico(1);
    void *frames[MAX_FRAMES];
    int n = backtrace(frames, MAX_FRAMES);
    dprintf(FD_EVENTOS, "\n[BACKTRACE-INICIO]|%s\n", etiqueta);
    backtrace_symbols_fd(frames, n, FD_EVENTOS);
    dprintf(FD_EVENTOS, "[BACKTRACE-FIN]\n");
    fijar_en_diagnostico(0);
}

static void manejador_señal(int sig) {
    const char *etiqueta = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "OTRA_SENAL";
    volcar_backtrace(etiqueta);
    dprintf(FD_EVENTOS, "\n[METRICAS]|%d|%d|%d|0\n", idx_malloc, idx_free, fail_after);
    _exit(sig == SIGSEGV ? 11 : 6);
}

__attribute__((constructor)) static void instalar_manejadores(void) {
    signal(SIGSEGV, manejador_señal);
    signal(SIGABRT, manejador_señal);
    /* torturete sustituye el stdin real por un pipe (para poder grabarlo y
       repetirlo en las pruebas diferidas). Eso rompe la deteccion de
       "entrada interactiva" que usa glibc para volcar automaticamente la
       salida en linea antes de bloquear en una lectura (p.ej. un prompt
       "printf" sin salto de linea seguido de "scanf"). Se fuerza stdout
       sin buffer para que cualquier salida se vea al instante, aunque
       stdin ya no parezca una terminal. */
    setvbuf(stdout, NULL, _IONBF, 0);
}

/* Punto unico de decision, bajo mutex, para saber si la llamada actual de
   malloc/calloc/realloc debe fallar.
   - Modo EN VIVO (fail_after == -1): NUNCA falla nada. El programa
     objetivo se ejecuta con normalidad, con su entrada/salida reales.
     Cada llamada se anuncia por el canal de eventos (fd 3) como
     "MALLOC|<numero>", para que torturete pueda lanzar en el acto el
     hilo "dormido" que mas tarde reproducira ese fallo concreto.
   - Modo PRUEBA (fail_after == N): hace fallar exactamente la llamada
     numero N (y solo esa), igual que antes. */
static int intentar_fallar(void) {
    int debe_fallar = 0;
    if (fail_after != -2) {
        pthread_mutex_lock(&mtx_tracking);
        if (!ya_fallado) {
            int current_call = global_counter++;
            idx_malloc++;
            if (fail_after == -1) {
                dprintf(FD_EVENTOS, "MALLOC|%d\n", current_call);
            } else if (current_call == fail_after) {
                ya_fallado = 1;
                idx_malloc--;
                debe_fallar = 1;
            }
        }
        pthread_mutex_unlock(&mtx_tracking);
    }
    return debe_fallar;
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
        if (intentar_fallar()) { volcar_backtrace("MALLOC_FALLIDO"); return NULL; }
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
        if (intentar_fallar()) { volcar_backtrace("CALLOC_FALLIDO"); return NULL; }
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
        if (intentar_fallar()) { volcar_backtrace("REALLOC_FALLIDO"); return NULL; }
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
        dprintf(FD_EVENTOS, "\n[METRICAS]|%d|%d|-666|0\n", idx_malloc, idx_free);
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
        dprintf(FD_EVENTOS, "\n[METRICAS]|%d|%d|%d|%zu\n", idx_malloc, idx_free, count_leaks, bytes_colgados);
    }
}
