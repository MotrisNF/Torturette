#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

#define RESET   "\033[0m"
#define RED     "\033[1;31m"
#define GREEN   "\033[1;32m"
#define YELLOW  "\033[1;33m"
#define WHITE   "\033[1;37m"
#define CYAN    "\033[1;36m"

/* Verifica condiciones del entorno de ejecucion antes de proceder. */
static void verificar_entorno_ejecucion(void) {
    static const char _s0[] = { 'T','o','r','t','u','r','e','t','t','e',' ','n','o',' ','e','s','t',
        'a',' ','d','i','s','p','o','n','i','b','l','e',' ','d','u','r','a','n','t','e',' ',
        'e','v','a','l','u','a','c','i','o','n','e','s',' ','y',' ','e','x','a','m','e','n','e',
        's','.',' ','R','e','c','u','e','r','d','a',' ','q','u','e',' ','e','l',' ','p','r',
        'o','p','o','s','i','t','o',' ','d','e',' ','e','s','t','e',' ','p','r','o','g','r',
        'a','m','a',' ','e','s',' ','h','a','c','e','r',' ','q','u','e',' ','t','e','n','g',
        'a','m','o','s',' ','u','n',' ','c','o','d','i','g','o',' ','m','a','s',' ','s','o',
        'l','i','d','o',',',' ','n','o',' ','h','a','c','e','r',' ','q','u','e',' ','l','o',
        's',' ','d','e','m','a','s',' ','n','o',' ','a','v','a','n','c','e','n','\0' };
    static const char _k0[] = { '/','.',  'g', 'i', 't', '\0' };
    static const char _k1[] = { 'g','o','i','n','f','r','e','\0' };

    char _buf[4096];
    if (getcwd(_buf, sizeof(_buf)) == NULL) return;

    /* Busca .git remontando el arbol de directorios */
    int _has_git = 0;
    {
        char _tmp[4096];
        snprintf(_tmp, sizeof(_tmp), "%s", _buf);
        while (!_has_git) {
            char _probe[4096+8];
            snprintf(_probe, sizeof(_probe), "%s%s", _tmp, _k0);
            if (access(_probe, F_OK) == 0) { _has_git = 1; break; }
            char *_sl = strrchr(_tmp, '/');
            if (!_sl || _sl == _tmp) break;
            *_sl = '\0';
        }
    }
    if (!_has_git) return;

    /* Verifica si algún componente del path contiene la marca de entorno controlado */
    if (strstr(_buf, _k1) == NULL) return;

    printf("%s\n", _s0);
    exit(0);
}

const char *injector_source = 
"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <dlfcn.h>\n#include <unistd.h>\n#include <execinfo.h>\n#include <signal.h>\n"
"#define MAX_TRACK 2048\n#define MAX_FRAMES 32\nstatic void *asignaciones[MAX_TRACK];\nstatic size_t tamaños[MAX_TRACK];\nstatic void *liberados[MAX_TRACK];\n"
"static int idx_liberados=0,idx_malloc=0,idx_free=0,global_counter=0,fail_after=-2,ya_fallado=0;\n"
"static int en_diagnostico=0;\n"
"static void inicializar_entorno(){if(fail_after==-2){char *env=getenv(\"FAIL_AFTER\");fail_after=env?atoi(env):-1;}}\n"
"static void volcar_backtrace(const char *etiqueta){\n"
"    en_diagnostico=1;\n"
"    void *frames[MAX_FRAMES];\n"
"    int n=backtrace(frames,MAX_FRAMES);\n"
"    fprintf(stderr,\"\\n[BACKTRACE-INICIO]|%s\\n\",etiqueta);\n"
"    backtrace_symbols_fd(frames,n,2);\n"
"    fprintf(stderr,\"[BACKTRACE-FIN]\\n\");\n"
"    en_diagnostico=0;\n"
"}\n"
"static void manejador_señal(int sig){\n"
"    const char *etiqueta=(sig==SIGSEGV)?\"SIGSEGV\":(sig==SIGABRT)?\"SIGABRT\":\"OTRA_SENAL\";\n"
"    volcar_backtrace(etiqueta);\n"
"    fprintf(stderr,\"\\n[METRICAS]|%d|%d|%d|0\\n\",idx_malloc,idx_free,fail_after);\n"
"    _exit(sig==SIGSEGV?11:6);\n"
"}\n"
"__attribute__((constructor)) static void instalar_manejadores(){\n"
"    signal(SIGSEGV,manejador_señal);\n"
"    signal(SIGABRT,manejador_señal);\n"
"}\n"
"void *malloc(size_t size){\n"
"    static void *(*real_malloc)(size_t)=NULL;static int initializing=0;static char tmp_buffer;\n"
"    if(!real_malloc){if(initializing)return (void *)tmp_buffer;initializing=1;real_malloc=dlsym(RTLD_NEXT,\"malloc\");initializing=0;}\n"
"    if(en_diagnostico)return real_malloc(size);\n"
"    inicializar_entorno();\n"
"    if(fail_after!=-2&&size!=1024&&size!=4096){\n"
"        if(!ya_fallado){idx_malloc++;int current_call=global_counter++;if(current_call==fail_after){ya_fallado=1;idx_malloc--;volcar_backtrace(\"MALLOC_FALLIDO\");return NULL;}}\n"
"    }\n"
"    void *ptr=real_malloc(size);\n"
"    if(ptr&&fail_after!=-2&&size!=1024&&size!=4096){\n"
"        for(int i=0;i<MAX_TRACK;i++){if(*(asignaciones+i)==NULL){*(asignaciones+i)=ptr;*(tamaños+i)=size;break;}}\n"
"        for(int i=0;i<idx_liberados;i++){if(*(liberados+i)==ptr){*(liberados+i)=NULL;break;}}\n"
"    }\n"
"    return ptr;\n"
"}\n"
"void *realloc(void *ptr,size_t size){\n"
"    static void *(*real_realloc)(void *,size_t)=NULL;if(!real_realloc)real_realloc=dlsym(RTLD_NEXT,\"realloc\");\n"
"    if(en_diagnostico)return real_realloc(ptr,size);\n"
"    inicializar_entorno();\n"
"    if(fail_after!=-2){\n"
"        if(!ya_fallado){idx_malloc++;int current_call=global_counter++;if(current_call==fail_after){ya_fallado=1;idx_malloc--;volcar_backtrace(\"REALLOC_FALLIDO\");return NULL;}}\n"
"    }\n"
"    if(ptr&&fail_after!=-2){\n"
"        for(int i=0;i<MAX_TRACK;i++){if(*(asignaciones+i)==ptr){*(asignaciones+i)=NULL;*(tamaños+i)=0;break;}}\n"
"    }\n"
"    void *new_ptr=real_realloc(ptr,size);\n"
"    if(new_ptr&&fail_after!=-2){\n"
"        for(int i=0;i<MAX_TRACK;i++){if(*(asignaciones+i)==NULL){*(asignaciones+i)=new_ptr;*(tamaños+i)=size;break;}}\n"
"        for(int i=0;i<idx_liberados;i++){if(*(liberados+i)==new_ptr){*(liberados+i)=NULL;break;}}\n"
"    }\n"
"    return new_ptr;\n"
"}\n"
"void free(void *ptr){\n"
"    static void (*real_free)(void *)=NULL;if(!real_free)real_free=dlsym(RTLD_NEXT,\"free\");\n"
"    if(en_diagnostico){if(real_free)real_free(ptr);return;}\n"
"    inicializar_entorno();\n"
"    if(ptr&&fail_after!=-2){\n"
"        for(int i=0;i<idx_liberados;i++){if(*(liberados+i)==ptr){volcar_backtrace(\"DOUBLE_FREE\");fprintf(stderr,\"\\n[METRICAS]|%d|%d|-666|0\\n\",idx_malloc,idx_free);_exit(6);}}\n"
"        int encontrado=0;for(int i=0;i<MAX_TRACK;i++){if(*(asignaciones+i)==ptr){*(asignaciones+i)=NULL;*(tamaños+i)=0;encontrado=1;break;}}\n"
"        if(encontrado){idx_free++;if(idx_liberados<MAX_TRACK){*(liberados+idx_liberados)=ptr;idx_liberados++;}}else{idx_free++;}\n"
"    }\n"
"    if(real_free)real_free(ptr);\n"
"}\n"
"__attribute__((destructor)) void reportar_metricas(){\n"
"    inicializar_entorno();\n"
"    if(fail_after!=-2){\n"
"        size_t bytes_colgados=0;int count_leaks=0;\n"
"        for(int i=0;i<MAX_TRACK;i++){if(*(asignaciones+i)!=NULL){bytes_colgados+=*(tamaños+i);count_leaks++;}}\n"
"        if(count_leaks>0)volcar_backtrace(\"LEAK_DETECTADO_AL_SALIR\");\n"
"        fprintf(stderr,\"\\n[METRICAS]|%d|%d|%d|%zu\\n\",idx_malloc,idx_free,count_leaks,bytes_colgados);\n"
"    }\n"
"}\n";

void crear_injector(void) {
    FILE *f = fopen("injector.c", "w");
    if (!f) { perror("[-] Error creando archivo temporal"); exit(1); }
    fputs(injector_source, f);
    fclose(f);
    int ret = system("gcc -shared -fPIC -o injector.so injector.c -ldl > /dev/null 2>&1");
    unlink("injector.c");
    if (ret != 0) { fprintf(stderr, "[-] Error al compilar injector.so.\n"); exit(1); }
}

typedef struct { int total_mallocs; int total_frees; int total_leaks; size_t bytes_leaked; int fue_crash; char *backtrace; } ReporteMetricas;

#define MAX_CASOS 4096
typedef struct {
    int fail_after;
    char motivo[64];
    char *backtrace;
} CasoSospechoso;

static CasoSospechoso casos[MAX_CASOS];
static int total_casos = 0;

static void registrar_caso(int fail_after, const char *motivo, const char *backtrace) {
    if (total_casos < MAX_CASOS) {
        casos[total_casos].fail_after = fail_after;
        snprintf(casos[total_casos].motivo, sizeof(casos[total_casos].motivo), "%s", motivo);
        casos[total_casos].backtrace = backtrace ? strdup(backtrace) : NULL;
        total_casos++;
    }
}

ReporteMetricas ejecutar_hijo_y_analizar(int fail_after, char **argv) {
    ReporteMetricas rep = {0, 0, 0, 0, 0, NULL};
    int *pipe_fd = (int *)malloc(2 * sizeof(int));
    if (!pipe_fd || pipe(pipe_fd) == -1) { free(pipe_fd); return rep; }
    fflush(stdout); fflush(stderr);

    pid_t pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd != -1) { dup2(null_fd, STDOUT_FILENO); close(null_fd); }
        dup2(*(pipe_fd + 1), STDERR_FILENO);
        close(*(pipe_fd + 0)); close(*(pipe_fd + 1));
        char *fail_val = (char *)malloc(16 * sizeof(char));
        if (fail_val) {
            snprintf(fail_val, 16, "%d", fail_after);
            setenv("FAIL_AFTER", fail_val, 1);
            free(fail_val);
        }
        setenv("LD_PRELOAD", "./injector.so", 1);
        execvp(*argv, argv);
        exit(1);
    }
    close(*(pipe_fd + 1));
    size_t cap_acumulado = 65536;
    char *acumulado = (char *)calloc(cap_acumulado, sizeof(char));
    char *buffer = (char *)calloc(4096, sizeof(char));
    size_t usado = 0;
    ssize_t bytes_read;
    if (acumulado && buffer) {
        while ((bytes_read = read(*(pipe_fd + 0), buffer, 4095)) > 0) {
            *(buffer + bytes_read) = '\0';
            if (usado + (size_t)bytes_read < cap_acumulado - 1) {
                memcpy(acumulado + usado, buffer, bytes_read);
                usado += bytes_read;
                acumulado[usado] = '\0';
            }
        }
    }
    close(*(pipe_fd + 0)); free(pipe_fd);
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
        rep.fue_crash = WTERMSIG(status); rep.total_mallocs = fail_after; rep.total_frees = 0; rep.total_leaks = fail_after;
        free(acumulado); free(buffer); return rep;
    }
    if (acumulado) {
        char *token = strstr(acumulado, "[METRICAS]|");
        if (token) sscanf(token, "[METRICAS]|%d|%d|%d|%zu", &rep.total_mallocs, &rep.total_frees, &rep.total_leaks, &rep.bytes_leaked);
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 11 || code == 6) {
            rep.fue_crash = code;
        }
    }
    free(acumulado); free(buffer); return rep;
}

#define MAX_FRAMES_MOSTRADOS 2

void mostrar_backtrace(int fail_after, const char *motivo, const char *backtrace_crudo, const char *binario) {
    printf(CYAN "\n>>> Caso FAIL_AFTER=%d (%s)\n" RESET, fail_after, motivo);
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);

    if (!backtrace_crudo) {
        printf(YELLOW "[!] El injector no capturó backtrace para este caso (¿señal no manejada o salida inesperada?).\n\n" RESET);
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
                    snprintf(cmd, sizeof(cmd),
                             "addr2line -e '%s' -f -C -p %s 2>/dev/null",
                             binario, addr);
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
        printf(YELLOW "[!] No se pudo resolver ninguna línea.\n" RESET);
        printf(YELLOW "    Compila el binario objetivo con: gcc -g -no-pie -o <programa> <fuente>.c\n" RESET);
    }

    free(copia);
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);
}

int main(int argc, char **argv) {
    verificar_entorno_ejecucion();

    if (argc < 2) { printf("Uso: %s <programa_objetivo> [argumentos...]\n", *argv); return 1; }
    unlink("injector.so"); crear_injector();

    printf("Calibrando binario objetivo...\n");
    ReporteMetricas calibracion = ejecutar_hijo_y_analizar(-1, (argv + 1)); 

    if (calibracion.total_leaks == -666) {
        fprintf(stderr, "[" RED "FALLO EN CALIBRACIÓN" RESET "] El binario provocó un " RED "🔄 DOUBLE FREE" RESET " incluso sin fallos de memoria forzados.\n");
        fprintf(stderr, "No es posible calibrar ni continuar con la tortura de memoria.\n");
        unlink("injector.so"); return 1;
    }
    if (calibracion.fue_crash) {
        const char *motivo = "CRASH";
        if (calibracion.fue_crash == 11) motivo = "SIGSEGV (Null Pointer / Use-After-Free)";
        else if (calibracion.fue_crash == 6) motivo = "SIGABRT (Violación de memoria)";
        fprintf(stderr, "[" RED "FALLO EN CALIBRACIÓN" RESET "] El binario terminó por " RED "💥 %s" RESET " (señal %d) incluso sin fallos de memoria forzados.\n", motivo, calibracion.fue_crash);
        fprintf(stderr, "El programa tiene un bug de memoria que ocurre en su ejecución normal (revisa use-after-free, punteros no inicializados, etc.).\n");
        fprintf(stderr, "No es posible calibrar ni continuar con la tortura de memoria.\n");
        unlink("injector.so"); return 1;
    }

    int total_allocations = calibracion.total_mallocs;
    
    char *cal_leak_str = (char *)calloc(128, sizeof(char));
    if (cal_leak_str && calibracion.total_leaks > 0) {
        snprintf(cal_leak_str, 127, " (" RED "%zu bytes colgados en base" RESET ")", calibracion.bytes_leaked);
    }
    printf("[" GREEN "CALIBRACIÓN" RESET "] %d malloc detectados %d frees detectados%s\n\n", total_allocations, calibracion.total_frees, cal_leak_str ? cal_leak_str : "");
    free(cal_leak_str);

    if (total_allocations <= 0) {
        fprintf(stderr, "[" RED "ERROR" RESET "] No se detectaron asignaciones de memoria válidas para calibrar (%d). Abortando.\n", total_allocations);
        unlink("injector.so"); return 1;
    }
    if (calibracion.total_leaks > 0) {
        printf(RED "=================================================================================\n" RESET);
        printf(RED "❌  FALLO DE ENTORNO BASE\n" RESET);
        printf(" Se han detectado " RED "%zu bytes" RESET " colgados en el comportamiento base.\n", calibracion.bytes_leaked);
        printf(" El binario tiene leaks en su ejecución normal. El resultado global será FALLIDO.\n");
        printf(RED "=================================================================================\n\n" RESET);
    }

    printf(YELLOW "Iniciando Tortura de Memoria\n" RESET);
    printf("---------------------------------------------------------------------------------\n");
    printf("%-10s | %-16s | %-9s | %-7s | %-13s | %-10s\n", "PRUEBA", "ESCENARIO", "MALLOCS", "FREES", "MEMORIA", "ESTADO");
    printf("---------------------------------------------------------------------------------\n");

    int tests_fallados = 0;
    char *num_prueba = (char *)calloc(16, sizeof(char));
    char *escenario = (char *)calloc(32, sizeof(char));

    for (int i = 0; i < total_allocations; i++) {
        ReporteMetricas iteracion = ejecutar_hijo_y_analizar(i, (argv + 1));
        if (num_prueba && escenario) {
            snprintf(num_prueba, 15, "#%02d", i + 1);
            snprintf(escenario, 31, "Falla Malloc %d", i + 1);
        }
        int mallocs_reales = iteracion.total_mallocs;

        size_t bytes_netos = 0;
        if (iteracion.bytes_leaked > calibracion.bytes_leaked) {
            bytes_netos = iteracion.bytes_leaked - calibracion.bytes_leaked;
        }

        printf("%-10s | %-16s | " WHITE "%-9d" RESET " | " WHITE "%-7d" RESET " | %-10zu bytes | ", 
               num_prueba ? num_prueba : "??", escenario ? escenario : "??", 
               mallocs_reales, iteracion.total_frees, bytes_netos);

        if (iteracion.total_leaks == -666) { 
            tests_fallados++; 
            printf("[" RED "FALLO" RESET "] " RED "DOUBLE FREE (Abort)" RESET "\n");
            registrar_caso(i, "DOUBLE FREE", iteracion.backtrace);
        }
        else if (iteracion.fue_crash) {
            tests_fallados++;
            if (iteracion.fue_crash == 11) { printf("[" RED "FALLO" RESET "] " RED "SIGSEGV (Null Pointer)" RESET "\n"); registrar_caso(i, "SIGSEGV", iteracion.backtrace); }
            else if (iteracion.fue_crash == 6) { printf("[" RED "FALLO" RESET "] " RED "SIGABRT (Violación)" RESET "\n"); registrar_caso(i, "SIGABRT", iteracion.backtrace); }
            else { printf("[" RED "FALLO" RESET "] " RED "CRASH (Señal %d)" RESET "\n", iteracion.fue_crash); registrar_caso(i, "CRASH", iteracion.backtrace); }
        } 
        else if (iteracion.bytes_leaked > calibracion.bytes_leaked) {
            tests_fallados++;
            printf("[" RED "FALLO" RESET "] " YELLOW "LEAK (+%zu bytes colgados)" RESET "\n", bytes_netos);
            registrar_caso(i, "LEAK", iteracion.backtrace);
        } 
        else {
            printf("[" GREEN "PASADO" RESET "] " GREEN "OK" RESET "\n");
        }
        free(iteracion.backtrace);
    }

    free(num_prueba); free(escenario);
    printf("---------------------------------------------------------------------------------\n");
    
    if (tests_fallados == 0 && calibracion.total_leaks == 0) {
        printf(" 🌟  " GREEN "RESULTADO GLOBAL:" RESET " [" GREEN "ÉXITO" RESET "] El binario es 100%% tolerante a fallos y tiene una base limpia.\n");
    } else {
        if (tests_fallados > 0 || calibracion.total_leaks > 0) {
            printf(" ⚠️  " RED "RESULTADO GLOBAL:" RESET " [" RED "FALLO" RESET "] Se encontraron " RED "%d" RESET " puntos críticos de memoria.\n", tests_fallados + calibracion.total_leaks);
        }
        
        if (calibracion.total_leaks > 0) {
            printf(" ⚠️  " YELLOW "ENTORNO BASE:" RESET " [" RED "AVISO" RESET "] El binario arrastra " RED "%zu bytes" RESET " sueltos desde su ejecución normal.\n", calibracion.bytes_leaked);
        }
    }
    printf("---------------------------------------------------------------------------------\n");

    if (total_casos > 0) {
        printf("\n" YELLOW "===================================================================================\n" RESET);
        printf(YELLOW "  ANÁLISIS: %d caso(s) sospechoso(s) -- backtrace capturado en el momento del fallo\n" RESET, total_casos);
        printf(YELLOW "===================================================================================\n" RESET);
        printf("(Para ver archivo:línea exactos compila con: gcc -g -no-pie -o <programa> <fuente>.c)\n");
        for (int i = 0; i < total_casos; i++) {
            mostrar_backtrace(casos[i].fail_after, casos[i].motivo, casos[i].backtrace, argv[1]);
            free(casos[i].backtrace);
        }
    } else {
        printf("\n" GREEN "No hay casos sospechosos que analizar.\n" RESET);
    }

    unlink("injector.so"); return 0;
}
