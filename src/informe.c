#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "comun.h"
#include "informe.h"

static const char *frases_ko_general[] = {
    "¿No sabes programar?",
    "¿En serio?",
    "Lo mismo esto no es lo tuyo...",
    "Vuelves a fallar en lo mismo",
    "¿Se puede ser mas malo?",
    "Deberías de dejarlo..",
    "En tu lugar, yo no lo intentaba más",
    "Eres malo en esto. Lo sabes",
    "Te tomaba por alguien más competente",
    "Yo en tu lugar, sentiría verguenza",
    "Eres la ausencia de talento personificada",
    "¿No te da vergüenza?",
    "Ya no solo decepcionas a tus padres",
    "Los problemas de memoria los tienes tu, no el programa ¿verdad?",
    "¿No te da vergüenza que tu programa sea peor que el mio?",
    "Venga, que a la próxima lo haces peor",
    "saperez- no es tu amigo, y tampoco lo soy yo",
    "En el fondo.. En el fondo me das pena",
    "Bueno, al menos sabemos que sigues siendo un inutil, como siempre",
    "La buena noticia es que tu programa al menos es mejor que tú",
    "Venga, que vas por mal camino",
    "A, pero que lo has vuelto a hacer mal. No me sorprende",
    "Si diesen premios por programas malos, el tuyo podría hasta retirarse con honores",
    "Otra vez... Otra vez lo mismo. ¿No te cansas?",
    "Esto no lo ve la Moulinette, pero yo si, y me das vergüenza ajena",
    "¿Solo lo veo yo o tu programa no funciona? No podemos decirle nada. Lo hiciste tu..."
};

void mostrar_banner(void) {
    printf("\n" RESET);
    printf(DARKRED "█████  ███  ████  █████ █   █ ████  █████ █████ █████ █████\n");
    printf("  █   █   █ █   █   █   █   █ █   █ █       █     █   █    \n");
    printf("  █   █   █ █   █   █   █   █ █   █ █       █     █   █    \n");
    printf("  █   █   █ ████    █   █   █ ████  ████    █     █   ████ \n");
    printf("  █   █   █ █ █     █   █   █ █ █   █       █     █   █    \n");
    printf("  █   █   █ █  █    █   █   █ █  █  █       █     █   █    \n");
    printf("  █    ███  █   █   █    ███  █   █ █████   █     █   █████\n" RESET);
    printf("\n" RED "¿Es el humano un ser perfecto? Tu programa, seguramente no...\n\n" RESET);
}

#define MAX_FRAMES_MOSTRADOS 2

void mostrar_backtrace(int fail_after, const char *motivo, const char *backtrace_crudo, const char *binario) {
    if (fail_after == -1) {
        printf(CYAN "\n>>> Ejecucion en vivo (base) -- %s\n" RESET, motivo);
    } else {
        printf(CYAN "\n>>> Caso Malloc %d (%s)\n" RESET, fail_after, motivo);
    }
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);

    if (!backtrace_crudo) {
        printf(YELLOW "[!] Ni backtrace ha dejado. Hasta para fallar hay que esforzarse un poco.\n\n" RESET);
        return;
    }

    char *copia = strdup(backtrace_crudo);
    if (!copia) { printf(RED "[!] Sin memoria para procesar el backtrace. Ironico, viniendo de esta herramienta.\n\n" RESET); return; }

    /* La primera linea (metida por el injector) trae la base de carga
     * real del ejecutable: "[BACKTRACE-INICIO]|<etiqueta>|<motivo>|0x...".
     * Los binarios de hoy en dia se compilan casi siempre PIE (posicion
     * independiente, con ASLR), asi que las direcciones que da
     * backtrace() son direcciones de memoria en tiempo de ejecucion, no
     * offsets de fichero -- y addr2line necesita offsets de fichero. Sin
     * restar esta base, addr2line falla SIEMPRE con binarios PIE (que
     * son la mayoria hoy en dia). Con binarios no-PIE la base es 0 y no
     * cambia nada. */
    unsigned long base_carga = 0;
    char *primera_linea_fin = strchr(copia, '\n');
    if (primera_linea_fin) {
        char guardado = *primera_linea_fin;
        *primera_linea_fin = '\0';
        char *ultima_barra = strrchr(copia, '|');
        if (ultima_barra) base_carga = strtoul(ultima_barra + 1, NULL, 16);
        *primera_linea_fin = guardado;
    }

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
                    unsigned long addr_runtime = strtoul(addr, NULL, 16);
                    unsigned long offset_fichero = (addr_runtime >= base_carga) ? (addr_runtime - base_carga) : addr_runtime;
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "addr2line -e '%s' -f -C -p 0x%lx 2>/dev/null", binario, offset_fichero);
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
        printf(YELLOW "[!] No se pudo resolver ninguna linea. Ni yo se donde la has liado.\n" RESET);
        printf(YELLOW "    Usa make torture, inutil\n" RESET);
    }

    free(copia);
    printf(CYAN "---------------------------------------------------------------------------------\n" RESET);
}

#define MAX_CASOS 65536
typedef struct {
    int fail_after;
    char motivo[64];
    char *backtrace;
} CasoSospechoso;

static CasoSospechoso casos[MAX_CASOS];
static int total_casos = 0;
static pthread_mutex_t mtx_casos = PTHREAD_MUTEX_INITIALIZER;

void registrar_caso(int fail_after, const char *motivo, const char *backtrace) {
    pthread_mutex_lock(&mtx_casos);
    if (total_casos < MAX_CASOS) {
        casos[total_casos].fail_after = fail_after;
        snprintf(casos[total_casos].motivo, sizeof(casos[total_casos].motivo), "%s", motivo);
        casos[total_casos].backtrace = backtrace ? strdup(backtrace) : NULL;
        total_casos++;
    }
    pthread_mutex_unlock(&mtx_casos);
}

void mostrar_casos_sospechosos(const char *binario) {
    if (total_casos > 0) {
        printf("\n" YELLOW "===================================================================================\n" RESET);
        printf(YELLOW "  ANALISIS: %d liadas sin resolver -- backtrace capturado en el momento del fallo\n" RESET, total_casos);
        printf(YELLOW "===================================================================================\n" RESET);
        for (int i = 0; i < total_casos; i++) {
            mostrar_backtrace(casos[i].fail_after, casos[i].motivo, casos[i].backtrace, binario);
            free(casos[i].backtrace);
        }
    } else {
        printf("\n" GREEN "No hay casos sospechosos que analizar. Por lo menos eso parece.\n" RESET);
    }
}

void mostrar_frases_ko_general(void) {
    size_t total = sizeof(frases_ko_general) / sizeof(frases_ko_general[0]);
    size_t indice = (size_t)rand() % total;
    printf("\n" DARKRED "%s\n" RESET, frases_ko_general[indice]);
}

static const char *frases_ok_general[] = {
    "¿Eres tan tonto que no sabes ni reservar memoria? Pues hoy pareceria que no.",
    "Ha sobrevivido. No te acostumbres.",
    "Por una vez no la has liado. Disfrutalo mientras dure.",
    "Sabes reservar memoria. Quien lo iba a decir."
};

void mostrar_frases_ok_general(void) {
    size_t total = sizeof(frases_ok_general) / sizeof(frases_ok_general[0]);
    size_t indice = (size_t)rand() % total;
    printf("\n" GREEN "%s\n" RESET, frases_ok_general[indice]);
}