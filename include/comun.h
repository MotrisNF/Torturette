#ifndef COMUN_H
#define COMUN_H

#include <stddef.h>

#define RESET     "\033[0m"
#define RED       "\033[1;31m"
#define DARKRED   "\033[38;5;52m"
#define GREEN     "\033[1;32m"
#define YELLOW    "\033[1;33m"
#define WHITE     "\033[1;37m"
#define CYAN      "\033[1;36m"

/* Canal de protocolo dedicado (backtraces, metricas, eventos de malloc en
 * vivo) entre el injector.so cargado en el proceso objetivo y torturete.
 * Se usa un fd fijo en vez de stderr para no mezclarse nunca con la
 * salida real del programa objetivo. */
#define FD_EVENTOS 3

typedef struct {
    int total_mallocs;
    int total_frees;
    int total_leaks;   /* -666 = double free detectado */
    size_t bytes_leaked;
    int fue_crash;      /* numero de señal (11=SIGSEGV, 6=SIGABRT, 2=SIGINT...), o 0 */
    char *backtrace;    /* backtrace crudo capturado por el injector, o NULL */
} ReporteMetricas;

#endif