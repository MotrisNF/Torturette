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
#include <link.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define CAPACIDAD_INICIAL_TRACK 256
#define MAX_FRAMES 32
#define FD_EVENTOS 3

/* Las tablas de tracking (asignaciones vivas y punteros ya liberados) eran
 * antes arrays de tamaño fijo (4096 entradas). Con programas que llegan a
 * tener mas de 4096 punteros vivos a la vez -- nada raro en proyectos
 * reales: listas enlazadas grandes, matrices, lectura de ficheros grandes
 * linea a linea -- las asignaciones que no cabian ni se registraban NI se
 * comprobaban, y sus leaks/double-free quedaban invisibles para torturete
 * sin ningun aviso. Ahora las tablas crecen dinamicamente (duplicando su
 * capacidad) segun hace falta, asi que no hay limite artificial. Se usa
 * SIEMPRE el realloc/malloc REAL (obtenido con dlsym, nunca el que este
 * fichero mismo intercepta) para su propio crecimiento interno, para no
 * registrar ni probar como fallo esta memoria de contabilidad interna. */
static void *(*g_real_realloc_interno)(void *, size_t) = NULL;
static void asegurar_real_realloc_interno(void) {
    if (!g_real_realloc_interno) g_real_realloc_interno = dlsym(RTLD_NEXT, "realloc");
}

static void **asignaciones = NULL;
static size_t *tamaños = NULL;
/* direccion_asignaciones[i] = direccion de retorno de quien hizo la
 * asignacion i (el mismo valor que ya se calcula para
 * direccion_pertenece_al_binario, reaprovechado). NO es una pila completa:
 * un backtrace() por asignacion se probo primero y se descarto -- and es
 * demasiado caro en programas con miles de asignaciones vivas, porque el
 * fork() de CADA prueba tiene que copiar una huella de memoria que crece
 * con cada asignacion trackeada (peor cuanto mas se guarda por cada una).
 * Con una sola direccion por asignacion (8 bytes) el coste extra es
 * minimo, y sigue siendo suficiente para que, si esta asignacion acaba
 * siendo un leak, se pueda resolver A POSTERIORI (con
 * backtrace_symbols_fd sobre este unico puntero, solo si hace falta) el
 * sitio EXACTO donde se reservo -- en vez de, como antes, capturar el
 * backtrace en el destructor, cuando main() ya ha retornado y no queda
 * pila real que capturar (siempre acababa apuntando a _start). */
static void **direccion_asignaciones = NULL;
static int capacidad_asignaciones = 0;
/* Primer indice que POSIBLEMENTE este libre. Nunca hace que se salte un
 * hueco real (si no se encuentra nada desde aqui en adelante, se rebusca
 * desde el principio antes de rendirse), pero evita reescanear desde cero
 * en el caso mas comun -- asignar sin apenas liberar de por medio -- que
 * de otra forma seria O(n) por asignacion, es decir O(n^2) en total. */
static int siguiente_hueco_probable = 0;

static void **liberados = NULL;
static int capacidad_liberados = 0;
static int idx_liberados = 0, idx_malloc = 0, idx_free = 0, global_counter = 0, fail_after = -2;
/* Suma de tamaños de todo lo reservado/liberado por el binario objetivo
 * (ver registrar_ptr/free). No es lo mismo que bytes_colgados (lo que
 * queda SIN liberar al salir): esto es el TOTAL acumulado a lo largo de
 * toda la ejecucion, se haya liberado luego o no. */
static size_t bytes_reservados_total = 0;
static size_t bytes_liberados_total = 0;

/* Debe llamarse con mtx_tracking ya cogido. Devuelve el indice del primer
 * hueco de la zona recien creada, o -1 si ni siquiera se pudo crecer
 * (memoria realmente agotada: esa asignacion en concreto queda sin
 * trackear, igual que ya pasaba antes al agotar la capacidad fija). */
static int crecer_asignaciones(void) {
    asegurar_real_realloc_interno();
    if (!g_real_realloc_interno) return -1;
    int cap_anterior = capacidad_asignaciones;
    int nueva_cap = cap_anterior ? cap_anterior * 2 : CAPACIDAD_INICIAL_TRACK;
    void **nuevo_asig = (void **)g_real_realloc_interno(asignaciones, (size_t)nueva_cap * sizeof(void *));
    if (!nuevo_asig) return -1;
    asignaciones = nuevo_asig;
    size_t *nuevo_tam = (size_t *)g_real_realloc_interno(tamaños, (size_t)nueva_cap * sizeof(size_t));
    if (!nuevo_tam) return -1; /* asignaciones ya crecio; no se pierde nada de lo ya registrado */
    tamaños = nuevo_tam;
    void **nueva_direccion = (void **)g_real_realloc_interno(direccion_asignaciones, (size_t)nueva_cap * sizeof(void *));
    if (!nueva_direccion) return -1;
    direccion_asignaciones = nueva_direccion;
    for (int i = cap_anterior; i < nueva_cap; i++) { asignaciones[i] = NULL; tamaños[i] = 0; direccion_asignaciones[i] = NULL; }
    capacidad_asignaciones = nueva_cap;
    return cap_anterior;
}

/* Debe llamarse con mtx_tracking ya cogido. */
static int crecer_liberados(void) {
    asegurar_real_realloc_interno();
    if (!g_real_realloc_interno) return 0;
    int nueva_cap = capacidad_liberados ? capacidad_liberados * 2 : CAPACIDAD_INICIAL_TRACK;
    void **nuevo = (void **)g_real_realloc_interno(liberados, (size_t)nueva_cap * sizeof(void *));
    if (!nuevo) return 0;
    liberados = nuevo;
    capacidad_liberados = nueva_cap;
    return 1;
}

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

/* 1 si EL HILO ACTUAL es quien tiene cogido mtx_prueba ahora mismo (ver mas
 * abajo). TSD, no un simple "int" global: lo consulta atfork_preparar(),
 * que se ejecuta en el hilo que esta llamando a fork() -- y ese fork()
 * puede ser precisamente el de intentar_fallar_y_probar() (que ya tiene
 * mtx_prueba cogido EL MISMO), o uno completamente ajeno hecho por el
 * propio binario objetivo desde otro hilo (que no lo tiene cogido). */
static pthread_key_t clave_prueba_propia;
static pthread_once_t once_prueba_propia = PTHREAD_ONCE_INIT;
static void crear_clave_prueba_propia(void) { pthread_key_create(&clave_prueba_propia, NULL); }
static int tengo_mtx_prueba_cogido(void) {
    pthread_once(&once_prueba_propia, crear_clave_prueba_propia);
    return (int)(intptr_t)pthread_getspecific(clave_prueba_propia);
}
static void fijar_tengo_mtx_prueba_cogido(int valor) {
    pthread_once(&once_prueba_propia, crear_clave_prueba_propia);
    pthread_setspecific(clave_prueba_propia, (void *)(intptr_t)valor);
}

static pthread_mutex_t mtx_tracking = PTHREAD_MUTEX_INITIALIZER;
/* Mutex APARTE (no mtx_tracking) para serializar el fork-y-espera de las
 * pruebas: si el programa objetivo tiene varios hilos llamando a malloc a
 * la vez, solo se prueba un fallo cada vez, nunca en paralelo -- mas
 * simple y predecible, aunque un pelin mas lento. No se reusa
 * mtx_tracking porque este mutex se mantiene cogido durante todo el
 * fork()+waitpid() (que puede tardar), y registrar_ptr() necesita poder
 * coger mtx_tracking mientras tanto sin bloquearse con esto.
 *
 * IMPORTANTE: este mutex NO puede ser recursivo. Un mutex recursivo (o de
 * tipo "error-checking") identifica a su dueño por el TID del hilo que lo
 * cogio -- y ese TID deja de ser valido justo despues de un fork(), porque
 * el (unico) hilo superviviente en el hijo tiene un TID NUEVO aunque toda
 * la memoria (incluido el propio mutex, con el TID del padre todavia
 * grabado dentro) se haya copiado tal cual. El resultado es un unlock()
 * que el hijo cree hacer sobre "su" mutex pero que en realidad no
 * coincide con el dueño grabado, dejando el mutex en un estado
 * imposible de recuperar -- un cuelgue real y reproducible, no teorico
 * (se detecto asi durante las pruebas de este mismo fix). Por eso se usa
 * un mutex NORMAL (PTHREAD_MUTEX_INITIALIZER, sin dueño ni contador de
 * anidamiento) y se evita la anidacion a mano con el TSD de mas arriba. */
static pthread_mutex_t mtx_prueba = PTHREAD_MUTEX_INITIALIZER;

/* Proteccion fork-safety: si otro hilo tuviera mtx_tracking o mtx_prueba
 * cogido justo en el instante de CUALQUIER fork() (el nuestro, dentro de
 * intentar_fallar_y_probar, o uno propio del binario objetivo -- p.ej. un
 * minishell o un pipex haciendo su propio pipeline de procesos), el hijo
 * heredaria ese mutex bloqueado para siempre (el hilo que lo soltaria no
 * existe en el hijo), y cualquier fallo de malloc posterior en ese hijo se
 * quedaria colgado para siempre intentando cogerlo. Se registran
 * manejadores atfork para que, alrededor de CUALQUIER fork(), ambos mutex
 * esten siempre libres tanto en el padre como en el hijo resultante.
 *
 * mtx_prueba se salta a proposito cuando el propio hilo que llama a fork()
 * ya lo tiene cogido (tengo_mtx_prueba_cogido()): eso solo pasa cuando el
 * fork() en curso es el de intentar_fallar_y_probar(), que ya se encarga
 * el mismo de soltarlo en su momento -- intentar cogerlo aqui otra vez
 * seria anidarlo a mano con un mutex normal (no recursivo), y eso si que
 * se autobloquearia. */
static void atfork_preparar(void) {
    if (!tengo_mtx_prueba_cogido()) pthread_mutex_lock(&mtx_prueba);
    pthread_mutex_lock(&mtx_tracking);
}
static void atfork_padre(void) {
    pthread_mutex_unlock(&mtx_tracking);
    if (!tengo_mtx_prueba_cogido()) pthread_mutex_unlock(&mtx_prueba);
}
static void atfork_hijo(void) {
    pthread_mutex_unlock(&mtx_tracking);
    if (!tengo_mtx_prueba_cogido()) pthread_mutex_unlock(&mtx_prueba);
}

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

/* Los binarios de hoy en dia se compilan casi siempre como PIE (posicion
 * independiente, con ASLR) -- es el default de gcc en sistemas
 * modernos. Eso significa que las direcciones que da backtrace() son
 * direcciones de memoria en tiempo de ejecucion, desplazadas por una
 * base de carga aleatoria, NO offsets dentro del fichero -- y addr2line
 * necesita offsets de fichero para poder resolver nada. Sin restar esta
 * base, addr2line falla siempre (con binarios PIE, siempre; con
 * binarios no-PIE, la base es 0 y no afecta). Se calcula con
 * dl_iterate_phdr: el primer objeto que reporta es siempre el propio
 * ejecutable (los .so cargados van despues). */
typedef struct {
    uintptr_t base;
    uintptr_t fin;
    int encontrado;
} RangoBinario;

static RangoBinario g_rango_binario;
static pthread_once_t once_rango_binario = PTHREAD_ONCE_INIT;

static int callback_rango_binario(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    RangoBinario *r = (RangoBinario *)data;
    uintptr_t max_fin = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        uintptr_t fin_segmento = (uintptr_t)info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz;
        if (fin_segmento > max_fin) max_fin = fin_segmento;
    }
    r->base = (uintptr_t)info->dlpi_addr;
    r->fin = (uintptr_t)info->dlpi_addr + max_fin;
    r->encontrado = 1;
    return 1; /* el primero que reporta dl_iterate_phdr es siempre el ejecutable
                 principal (los .so cargados van despues); con ese ya basta. */
}

/* Calcula, UNA sola vez por proceso, el rango de direcciones [base, fin) en
 * el que esta mapeado el binario objetivo. Se usa para dos cosas que antes
 * se resolvian con heuristicas fragiles dependientes de la version de
 * glibc/arquitectura:
 *
 *   1. Decidir si una llamada a malloc/calloc/realloc viene del propio
 *      binario objetivo (se trackea/prueba) o de una libreria del sistema
 *      -- libc, ld.so, libpthread, etc. (no es responsabilidad del binario
 *      objetivo, no se trackea ni se prueba). Antes se adivinaba por el
 *      TAMAÑO exacto de la reserva (1024/4096/272), que varia entre
 *      versiones de glibc y produce falsos positivos de "leak" cuando no
 *      coincide. Ahora se decide por ORIGEN real de la llamada.
 *
 *   2. Filtrar, en el backtrace crudo, que lineas pertenecen al binario
 *      objetivo. Antes se buscaba el texto de la ruta del binario dentro de
 *      cada linea, lo cual depende de como esa version concreta de
 *      glibc/ld.so registre el nombre del ejecutable principal (puede venir
 *      vacio, relativo o absoluto segun el caso) y puede no encontrar nunca
 *      ninguna coincidencia. Ahora se decide por RANGO DE DIRECCIONES, que
 *      no depende de texto ni de versiones. */
static void calcular_rango_binario(void) {
    dl_iterate_phdr(callback_rango_binario, &g_rango_binario);
}

static void obtener_rango_binario(uintptr_t *base, uintptr_t *fin) {
    pthread_once(&once_rango_binario, calcular_rango_binario);
    *base = g_rango_binario.base;
    *fin = g_rango_binario.fin;
}

/* Algunas funciones de la libc reservan memoria POR CUENTA del que las
 * llama y le devuelven un puntero del que este pasa a ser responsable
 * (strdup, getline, asprintf...). Su malloc() interno se ve, por
 * direccion, exactamente igual que el de una reserva realmente interna de
 * la libc (TLS de un hilo nuevo, buffer de stdio...): la direccion de
 * retorno cae dentro de libc.so, no del binario objetivo. Pero para el
 * programa que las usa, un strdup() que falla es un fallo de memoria tan
 * suyo como un malloc() directo -- son extremadamente comunes en proyectos
 * de 42 (minishell, parsers...) y si se excluyeran de raiz, ni se
 * probarian ni se trackearian: menos cobertura que la propia heuristica
 * de tamaños que se elimino, no mas. Se mantiene por eso esta lista
 * reducida de "envoltorios conocidos": si la funcion que contiene la
 * direccion de retorno es una de estas, se trata como si viniera del
 * binario objetivo pese a estar fisicamente dentro de libc.so. */
static int es_envoltorio_asignador_conocido(void *direccion) {
    static const char *conocidos[] = {
        "strdup", "__strdup", "strndup", "__strndup",
        "getline", "__getline", "getdelim", "__getdelim",
        "asprintf", "__asprintf", "vasprintf", "__vasprintf",
        "realpath", "canonicalize_file_name",
        "scandir", "get_current_dir_name", NULL
    };
    Dl_info info;
    if (!dladdr(direccion, &info) || !info.dli_sname) return 0;
    for (int i = 0; conocidos[i]; i++) {
        if (strcmp(info.dli_sname, conocidos[i]) == 0) return 1;
    }
    return 0;
}

static int direccion_pertenece_al_binario(void *direccion) {
    pthread_once(&once_rango_binario, calcular_rango_binario);
    uintptr_t a = (uintptr_t)direccion;
    if (g_rango_binario.encontrado && a >= g_rango_binario.base && a < g_rango_binario.fin) return 1;
    return es_envoltorio_asignador_conocido(direccion);
}

static void volcar_backtrace(const char *etiqueta) {
    fijar_en_diagnostico(1);
    void *frames[MAX_FRAMES];
    int n = backtrace(frames, MAX_FRAMES);
    uintptr_t base = 0, fin = 0;
    obtener_rango_binario(&base, &fin);
    dprintf(FD_EVENTOS, "\n[BACKTRACE-INICIO]|%s|%s|0x%lx|0x%lx\n", g_etiqueta_proceso, etiqueta, (unsigned long)base, (unsigned long)fin);
    backtrace_symbols_fd(frames, n, FD_EVENTOS);
    dprintf(FD_EVENTOS, "[BACKTRACE-FIN]\n");
    fijar_en_diagnostico(0);
}

/* Analogo a volcar_backtrace(), pero para un LEAK: en vez de capturar la
 * pila actual (que en el destructor, con main() ya retornado, es siempre
 * inutil -- apunta a _start), resuelve la UNICA direccion que se guardo en
 * el instante en que esa asignacion en concreto se registro (ver
 * registrar_ptr) -- ahora si, con backtrace_symbols_fd, que es caro pero
 * esto solo se paga como mucho una vez por proceso (aqui, no en cada
 * asignacion). Mismo formato de protocolo que volcar_backtrace(), asi que
 * motor.c/informe.c no necesitan saber de donde viene la direccion. */
static void volcar_backtrace_de_asignacion(const char *etiqueta, int indice) {
    fijar_en_diagnostico(1);
    uintptr_t base = 0, fin = 0;
    obtener_rango_binario(&base, &fin);
    dprintf(FD_EVENTOS, "\n[BACKTRACE-INICIO]|%s|%s|0x%lx|0x%lx\n", g_etiqueta_proceso, etiqueta, (unsigned long)base, (unsigned long)fin);
    if (indice >= 0 && indice < capacidad_asignaciones && direccion_asignaciones[indice]) {
        backtrace_symbols_fd(&direccion_asignaciones[indice], 1, FD_EVENTOS);
    }
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
    dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|0|0|%d|%zu|%zu\n", g_etiqueta_proceso, idx_malloc, idx_free, senal_reportada, bytes_reservados_total, bytes_liberados_total);
    _exit(sig == SIGSEGV ? 11 : 6);
}

/* La primerisima vez que se llama a backtrace() en un proceso, la propia
 * glibc reserva memoria internamente (malloc) para inicializar sus tablas
 * de unwind -- documentado en 'man 3 backtrace'. Si esa primera llamada
 * ocurre de forma perezosa DENTRO del manejador de señal (al recibir un
 * SIGSEGV/SIGABRT) o del destructor, y el crash ocurrio precisamente por
 * corrupcion de heap mientras el propio hilo tenia cogido el lock interno
 * del malloc de glibc (un caso muy tipico en los programas que esta
 * herramienta esta pensada para poner a prueba), esa llamada a malloc()
 * dentro del manejador se autobloquea esperando un lock que nunca se va a
 * soltar: el proceso se queda colgado para siempre, sin backtrace, sin
 * metricas, y torturete se queda esperando indefinidamente ese caso (y
 * todos los que vendrian despues, porque las pruebas son secuenciales).
 * Se evita "precalentando" backtrace() aqui, en el constructor: se ejecuta
 * antes de main(), en un unico hilo, sin manejador de señal activo y sin
 * ningun lock de malloc cogido por nadie -- el sitio mas seguro posible
 * para pagar ese coste una sola vez. A partir de aqui, cualquier llamada
 * posterior a backtrace() (incluso dentro de un manejador de señal) ya no
 * necesita reservar memoria. */
static void precalentar_backtrace(void) {
    fijar_en_diagnostico(1);
    void *frames_calentamiento[4];
    backtrace(frames_calentamiento, 4);
    fijar_en_diagnostico(0);
}

__attribute__((constructor)) static void instalar_manejadores(void) {
    signal(SIGSEGV, manejador_señal);
    signal(SIGABRT, manejador_señal);
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_atfork(atfork_preparar, atfork_padre, atfork_hijo);
    precalentar_backtrace();
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

/* fork() hace que el hijo comparta con el padre, para cualquier fd que
 * tuviera abierto, la MISMA descripcion de fichero abierto del kernel --
 * no solo el mismo fichero, sino el mismo OFFSET de lectura/escritura. Si
 * el hijo de prueba sigue ejecutandose (que es la idea) y hace mas
 * read()/write() sobre un fichero regular que el binario objetivo abrio
 * por su cuenta (piensa en un get_next_line leyendo de un fd que no sea
 * stdin), esos read()/write() avanzan ese offset compartido, y el cambio
 * es visible para el proceso en vivo real en cuanto retome la ejecucion
 * tras el waitpid() -- corrompiendo silenciosamente su comportamiento real
 * sin que haya ningun crash que lo delate. stdin/stdout/stderr ya se
 * aislan aparte (redirigidos a /dev/null); esto cubre cualquier OTRO fd de
 * fichero regular abierto por el propio binario. No se tocan pipes,
 * sockets ni terminales: no tienen un "offset" que tenga sentido duplicar
 * asi, y aislarlos de mas podria cambiar su comportamiento real (p.ej.
 * cerrar el extremo de un pipe que el programa SI espera que siga vivo). */
static void aislar_ficheros_regulares_del_hijo(void) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return;
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        if (entrada->d_name[0] == '.') continue;
        int fd = atoi(entrada->d_name);
        if (fd < 0 || fd == FD_EVENTOS || fd == dirfd(dir)) continue;
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        off_t offset_actual = lseek(fd, 0, SEEK_CUR);
        if (offset_actual == (off_t)-1) continue;
        int flags = fcntl(fd, F_GETFL);
        if (flags == -1) continue;
        char ruta_proc[32];
        snprintf(ruta_proc, sizeof(ruta_proc), "/proc/self/fd/%d", fd);
        int fd_propio = open(ruta_proc, flags & (O_ACCMODE | O_APPEND));
        if (fd_propio < 0) continue;
        lseek(fd_propio, offset_actual, SEEK_SET);
        dup2(fd_propio, fd);
        close(fd_propio);
    }
    closedir(dir);
}

/* Prepara un hijo de prueba recien nacido de fork(): marca que es un hijo
 * de prueba (nunca volvera a intentar fallar nada el mismo), le asigna su
 * etiqueta, y aisla su entrada/salida. stdin va a /dev/null (si el
 * programa sigue leyendo, recibira EOF de inmediato). stdout/stderr van a
 * 'ruta_salida' EN VEZ de /dev/null: si este hijo acaba quedandose colgado
 * y hay que matarlo por timeout (ver esperar_hijo_de_prueba), asi se puede
 * mostrar en el informe lo que llego a imprimir antes de quedarse
 * dormido, en vez de perderlo sin mas. */
static void preparar_hijo_de_prueba(const char *etiqueta, const char *ruta_salida) {
    g_es_prueba = 1;
    snprintf(g_etiqueta_proceso, sizeof(g_etiqueta_proceso), "%s", etiqueta);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd != -1) dup2(null_fd, STDIN_FILENO);

    int fd_salida = open(ruta_salida, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_salida != -1) {
        dup2(fd_salida, STDOUT_FILENO);
        dup2(fd_salida, STDERR_FILENO);
        if (fd_salida > STDERR_FILENO) close(fd_salida);
    } else if (null_fd != -1) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
    }
    if (null_fd != -1 && null_fd > STDERR_FILENO) close(null_fd);

    /* Aislar tambien cualquier OTRO fichero regular que el binario
     * objetivo tuviera abierto por su cuenta (ver
     * aislar_ficheros_regulares_del_hijo): si el hijo de prueba sigue
     * ejecutandose tras el fallo simulado -- que es precisamente el
     * objetivo, ver que pasa -- y hace mas read()/write() sobre ese fd,
     * heredado tal cual de fork(), estaria avanzando el MISMO offset de
     * fichero que ve el proceso en vivo real, corrompiendo silenciosamente
     * su comportamiento en cuanto el hijo termine y el padre continue. */
    aislar_ficheros_regulares_del_hijo();
}

/* Tiempo maximo (en segundos) que el padre espera a un hijo de prueba antes
 * de asumir que se ha quedado dormido para siempre y matarlo. Configurable
 * via TORTURETTE_TIMEOUT_SEG por si el sistema va justo de RAM (swapping)
 * y una prueba legitimamente lenta necesita mas margen -- el valor por
 * defecto es deliberadamente generoso para no confundir eso con un cuelgue
 * real. */
static double obtener_timeout_segundos(void) {
    static double timeout = -1.0;
    if (timeout < 0.0) {
        char *env = getenv("TORTURETTE_TIMEOUT_SEG");
        double valor = env ? atof(env) : 0.0;
        timeout = (valor > 0.0) ? valor : 5.0;
    }
    return timeout;
}

/* Vuelca por el canal de eventos lo que el hijo de prueba llego a
 * imprimir (capturado en 'ruta_salida') antes de quedarse colgado, y sus
 * metricas con leaks=-667 (sentinela para "cuelgue", igual que -666 ya se
 * usa para double-free). */
static void reportar_cuelgue(const char *etiqueta, const char *ruta_salida) {
    char contenido[8192];
    ssize_t leido = 0;
    int fd = open(ruta_salida, O_RDONLY);
    if (fd != -1) {
        leido = read(fd, contenido, sizeof(contenido) - 1);
        close(fd);
    }

    dprintf(FD_EVENTOS, "\n[CUELGUE-INICIO]|%s\n", etiqueta);
    if (leido > 0) {
        contenido[leido] = '\0';
        write(FD_EVENTOS, contenido, (size_t)leido);
        if (contenido[leido - 1] != '\n') dprintf(FD_EVENTOS, "\n");
    }
    dprintf(FD_EVENTOS, "[CUELGUE-FIN]\n");
    dprintf(FD_EVENTOS, "\n[METRICAS]|%s|0|0|-667|0|0|0|0\n", etiqueta);
}

/* El padre espera (con limite de tiempo) a que el hijo de prueba termine.
 * Sin este limite, un hijo que se quede dormido para siempre (el propio
 * bug que esta herramienta esta pensada para sacar a la luz cuando falla
 * algo tan basico como crear un hilo o reservar memoria) dejaria a
 * torturete entero esperando junto a el para siempre, indistinguible de
 * que torturete se hubiese colgado el mismo. Se sondea con WNOHANG en vez
 * de usar señales (SIGALRM) para no interferir con los manejadores de
 * SIGSEGV/SIGABRT ya instalados. */
static void esperar_hijo_de_prueba(pid_t pid, const char *etiqueta, const char *ruta_salida) {
    struct timespec inicio, ahora;
    clock_gettime(CLOCK_MONOTONIC, &inicio);
    int status;
    /* Sondeo con backoff exponencial: la inmensa mayoria de las pruebas
     * terminan en microsegundos (un malloc/pthread_create normal seguido
     * de que el programa continue), asi que empezar a dormir 20ms fijos
     * en cada vuelta (como en una primera version de este mismo codigo)
     * anadia esos 20ms de latencia a CADA prueba sin excepcion -- con
     * miles de asignaciones, esto disparaba el tiempo total de minutos.
     * Se empieza sondeando casi sin esperar, y solo se alarga la espera
     * (doblando cada vez, hasta un tope) si el hijo de verdad tarda. */
    long espera_ns = 50L * 1000; /* 50 microsegundos */
    const long espera_maxima_ns = 20L * 1000 * 1000; /* tope: 20 ms */

    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r == -1) { unlink(ruta_salida); return; }
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        double transcurrido = (double)(ahora.tv_sec - inicio.tv_sec) +
                               (double)(ahora.tv_nsec - inicio.tv_nsec) / 1e9;
        if (transcurrido >= obtener_timeout_segundos()) break;
        struct timespec espera = { 0, espera_ns };
        nanosleep(&espera, NULL);
        espera_ns *= 2;
        if (espera_ns > espera_maxima_ns) espera_ns = espera_maxima_ns;
    }

    /* TIMEOUT: el hijo no ha terminado a tiempo. Se asume colgado. */
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    reportar_cuelgue(etiqueta, ruta_salida);
    unlink(ruta_salida);
}

/* Nucleo compartido entre las pruebas de malloc/calloc/realloc y la de
 * pthread_create: calcula el indice de esta prueba, hace el fork(),
 * prepara al hijo (aislamiento de E/S, ver preparar_hijo_de_prueba) y en
 * el padre espera con timeout (ver esperar_hijo_de_prueba). Devuelve 1 si
 * ESTE proceso es el hijo de prueba (debe fallar esta llamada en
 * concreto), 0 si es el padre (nunca falla esta llamada).
 *
 * 'sufijo_etiqueta' distingue el TIPO de prueba en el indice numerico que
 * viaja por el canal de eventos ("5" para malloc/calloc/realloc, "5T"
 * para pthread_create): comparten la misma secuencia global de indices
 * (refleja el orden real en que ocurren durante la ejecucion), pero el
 * informe final necesita saber cual de las dos funciones fallo para
 * etiquetar el caso correctamente. atoi() se detiene en el primer
 * caracter no numerico, asi que "5T" se sigue interpretando como indice 5
 * sin necesidad de tocar el resto del parseo existente. */
static int fork_y_probar_fallo(const char *sufijo_etiqueta) {
    pthread_mutex_lock(&mtx_prueba);
    fijar_tengo_mtx_prueba_cogido(1);

    pthread_mutex_lock(&mtx_tracking);
    int mi_indice = global_counter++;
    pthread_mutex_unlock(&mtx_tracking);

    char etiqueta[32];
    snprintf(etiqueta, sizeof(etiqueta), "%d%s", mi_indice, sufijo_etiqueta);
    char ruta_salida[80];
    snprintf(ruta_salida, sizeof(ruta_salida), "/tmp/.torturette-%d-%s.out", (int)getpid(), etiqueta);

    fflush(stdout);
    pid_t pid = fork();

    if (pid == 0) {
        /* HIJO DE PRUEBA: esta llamada en concreto falla. */
        preparar_hijo_de_prueba(etiqueta, ruta_salida);
        return 1;
    }

    if (pid > 0) {
        /* PADRE: esperamos (con limite de tiempo) a que el hijo de prueba
         * termine -- crash, leak, cuelgue, o final limpio -- antes de
         * seguir con la ejecucion real. El hijo reporta sus propias
         * metricas/backtraces por el canal de eventos al terminar (o, si
         * se cuelga, es el propio padre quien reporta el cuelgue tras
         * matarlo). */
        esperar_hijo_de_prueba(pid, etiqueta, ruta_salida);
    }
    /* Si fork() fallo (pid < 0), simplemente no se prueba este indice y
     * se sigue con normalidad, como si no hubiera pasado nada. */

    fijar_tengo_mtx_prueba_cogido(0);
    pthread_mutex_unlock(&mtx_prueba);
    return 0; /* el padre nunca falla esta llamada */
}

static int intentar_fallar_y_probar(void) {
    if (fail_after != -1 || g_es_prueba) return 0;
    pthread_mutex_lock(&mtx_tracking);
    idx_malloc++;
    pthread_mutex_unlock(&mtx_tracking);
    return fork_y_probar_fallo("");
}

/* Analogo a intentar_fallar_y_probar(), pero para pthread_create(): en vez
 * de simular un malloc/calloc/realloc devolviendo NULL, simula que el
 * sistema ha rechazado crear el hilo (tipicamente por haber agotado el
 * limite de hilos/procesos del usuario, ulimit -u -- mucho mas bajo en
 * las maquinas del campus 42 que en un equipo personal, que es
 * precisamente el problema real que motivo esta prueba). El binario
 * objetivo, ante una llamada de creacion de hilo que falla, deberia
 * manejarlo con normalidad; si en vez de eso alguno de sus hilos se queda
 * dormido para siempre esperando a uno que nunca llego a arrancar, el
 * hijo de prueba se colgara y sera detectado por el timeout de
 * esperar_hijo_de_prueba. */
static int intentar_fallar_pthread_create(void) {
    if (fail_after != -1 || g_es_prueba) return 0;
    return fork_y_probar_fallo("T");
}

/* Quita 'ptr' de la lista de punteros ya liberados, si estaba ahi: la
 * direccion se ha vuelto a asignar (el asignador la ha reutilizado), asi
 * que ya no cuenta como "liberada" de cara a detectar un futuro
 * double-free. Debe llamarse para CUALQUIER asignacion real que este
 * proceso observe, VENGA O NO del binario objetivo -- si solo se hiciera
 * para las que si vienen de el (dentro de registrar_ptr, como se hacia
 * antes), una direccion reutilizada por una asignacion que NO se trackea
 * (p.ej. el malloc interno de strdup/getline/asprintf, cuya llamada a
 * malloc() sale desde dentro de la libc, no del binario objetivo, ver
 * direccion_pertenece_al_binario) seguiria constando como "liberada" para
 * siempre. El siguiente free() real sobre ella -- esta vez si hecho por
 * codigo del binario objetivo -- se confundiria entonces con un
 * double-free que nunca ocurrio (visto en la practica: un get_next_line
 * de prueba con strdup() disparaba justo este falso positivo). */
static void desmarcar_liberado(void *ptr) {
    pthread_mutex_lock(&mtx_tracking);
    for (int i = 0; i < idx_liberados; i++) {
        if (liberados[i] == ptr) { liberados[i] = NULL; break; }
    }
    pthread_mutex_unlock(&mtx_tracking);
}

static void registrar_ptr(void *ptr, size_t size, void *direccion_llamada) {
    pthread_mutex_lock(&mtx_tracking);
    int hueco = -1;
    for (int i = siguiente_hueco_probable; i < capacidad_asignaciones; i++) {
        if (asignaciones[i] == NULL) { hueco = i; break; }
    }
    if (hueco == -1) {
        for (int i = 0; i < siguiente_hueco_probable && i < capacidad_asignaciones; i++) {
            if (asignaciones[i] == NULL) { hueco = i; break; }
        }
    }
    if (hueco == -1) hueco = crecer_asignaciones();
    if (hueco != -1) {
        asignaciones[hueco] = ptr;
        tamaños[hueco] = size;
        siguiente_hueco_probable = hueco + 1;
        /* Se guarda la direccion de quien hizo esta llamada (ya calculada
         * por el que llama a registrar_ptr(), ver malloc/calloc/realloc):
         * si esta asignacion acaba siendo un leak, permite resolverla a
         * posteriori al sitio REAL donde se reservo. Antes el backtrace de
         * un leak se capturaba en el destructor, ya con el proceso
         * terminando: para ese momento main() ya ha retornado y la pila
         * real desaparecio, asi que siempre apuntaba a _start. Guardar
         * aqui una pila completa (probado y descartado) es demasiado caro
         * con miles de asignaciones vivas: el fork() de cada prueba tiene
         * que copiar una huella de memoria que crece con cada una. Con una
         * sola direccion el coste extra es minimo. */
        direccion_asignaciones[hueco] = direccion_llamada;
        bytes_reservados_total += size;
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
    void *direccion_llamada = __builtin_return_address(0);
    int es_del_objetivo = direccion_pertenece_al_binario(direccion_llamada);
    if (fail_after != -2 && es_del_objetivo) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    void *ptr = real_malloc(size);
    if (ptr && fail_after != -2) {
        desmarcar_liberado(ptr);
        if (es_del_objetivo) registrar_ptr(ptr, size, direccion_llamada);
    }
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
    void *direccion_llamada = __builtin_return_address(0);
    int es_del_objetivo = direccion_pertenece_al_binario(direccion_llamada);
    if (fail_after != -2 && es_del_objetivo) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    void *ptr = real_calloc(nmemb, size);
    if (ptr && fail_after != -2) {
        desmarcar_liberado(ptr);
        if (es_del_objetivo) registrar_ptr(ptr, total, direccion_llamada);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    static void *(*real_realloc)(void *, size_t) = NULL;
    if (!real_realloc) real_realloc = dlsym(RTLD_NEXT, "realloc");
    if (obtener_en_diagnostico()) return real_realloc(ptr, size);
    inicializar_entorno();
    void *direccion_llamada = __builtin_return_address(0);
    int es_del_objetivo = direccion_pertenece_al_binario(direccion_llamada);
    if (fail_after != -2 && es_del_objetivo) {
        if (intentar_fallar_y_probar()) return NULL;
    }
    if (ptr && fail_after != -2 && es_del_objetivo) {
        pthread_mutex_lock(&mtx_tracking);
        for (int i = 0; i < capacidad_asignaciones; i++) {
            if (asignaciones[i] == ptr) {
                asignaciones[i] = NULL; tamaños[i] = 0;
                if (i < siguiente_hueco_probable) siguiente_hueco_probable = i;
                break;
            }
        }
        pthread_mutex_unlock(&mtx_tracking);
    }
    void *new_ptr = real_realloc(ptr, size);
    if (new_ptr && fail_after != -2) {
        desmarcar_liberado(new_ptr);
        if (es_del_objetivo) registrar_ptr(new_ptr, size, direccion_llamada);
    }
    return new_ptr;
}

/* Crear un hilo tambien es una peticion de recursos que el sistema puede
 * rechazar (limite de hilos/procesos del usuario agotado -- EAGAIN), igual
 * que malloc/calloc/realloc pueden rechazar una reserva. Se prueba con el
 * mismo modelo: en el proceso en vivo, cada llamada real a pthread_create()
 * dispara un fork() que comprueba que pasaria si ESA llamada en concreto
 * fallase (ver intentar_fallar_pthread_create), mientras el proceso en
 * vivo crea el hilo de verdad y continua con toda normalidad. */
int pthread_create(pthread_t *hilo, const pthread_attr_t *attr,
                    void *(*rutina)(void *), void *arg) {
    static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
    if (!real_pthread_create) real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    if (obtener_en_diagnostico()) return real_pthread_create(hilo, attr, rutina, arg);
    inicializar_entorno();
    if (fail_after != -2 && direccion_pertenece_al_binario(__builtin_return_address(0))) {
        if (intentar_fallar_pthread_create()) return EAGAIN;
    }
    return real_pthread_create(hilo, attr, rutina, arg);
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
            for (int i = 0; i < capacidad_asignaciones; i++) {
                if (asignaciones[i] == ptr) {
                    bytes_liberados_total += tamaños[i];
                    asignaciones[i] = NULL; tamaños[i] = 0; encontrado = 1;
                    if (i < siguiente_hueco_probable) siguiente_hueco_probable = i;
                    break;
                }
            }
            if (encontrado) {
                idx_free++;
                if (idx_liberados >= capacidad_liberados) crecer_liberados();
                if (idx_liberados < capacidad_liberados) { liberados[idx_liberados] = ptr; idx_liberados++; }
            } else { idx_free++; }
        }
        pthread_mutex_unlock(&mtx_tracking);
    }
    if (es_doble_free) {
        volcar_backtrace("DOUBLE_FREE");
        dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|-666|0|6|%zu|%zu\n", g_etiqueta_proceso, idx_malloc, idx_free, bytes_reservados_total, bytes_liberados_total);
        _exit(6);
    }
    if (real_free) real_free(ptr);
}

__attribute__((destructor)) void reportar_metricas(void) {
    inicializar_entorno();
    if (fail_after != -2) {
        size_t bytes_colgados = 0;
        int count_leaks = 0;
        int indice_primer_leak = -1;
        pthread_mutex_lock(&mtx_tracking);
        for (int i = 0; i < capacidad_asignaciones; i++) {
            if (asignaciones[i] != NULL) {
                bytes_colgados += tamaños[i];
                count_leaks++;
                if (indice_primer_leak == -1) indice_primer_leak = i;
            }
        }
        pthread_mutex_unlock(&mtx_tracking);
        /* Se muestra la pila guardada de la PRIMERA asignacion que quedo
         * sin liberar (ver registrar_ptr): apunta al sitio real donde se
         * reservo, no al punto de salida del proceso. Si hay varios leaks
         * distintos en la misma ejecucion, solo se muestra este primero --
         * el resto se sigue contando en 'bytes_colgados'/'count_leaks',
         * pero mostrar todas las pilas por separado exigiria ampliar el
         * protocolo de eventos mas de lo que pide este arreglo. */
        if (count_leaks > 0) volcar_backtrace_de_asignacion("LEAK_DETECTADO_AL_SALIR", indice_primer_leak);
        dprintf(FD_EVENTOS, "\n[METRICAS]|%s|%d|%d|%d|%zu|0|%zu|%zu\n", g_etiqueta_proceso, idx_malloc, idx_free, count_leaks, bytes_colgados, bytes_reservados_total, bytes_liberados_total);
    }
}