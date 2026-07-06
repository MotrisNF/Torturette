#ifndef MOTOR_H
#define MOTOR_H

#include <pthread.h>
#include <semaphore.h>
#include "comun.h"

/* Una prueba diferida: reproduce que pasaria si la llamada de malloc/
 * calloc/realloc numero 'indice' hubiese fallado. Se lanza en tiempo real
 * en cuanto esa llamada ocurre durante la ejecucion en vivo, y se queda
 * dormida hasta que dicha ejecucion en vivo termina. */
typedef struct {
    int indice;
    pthread_t hilo;
    int hilo_ok;   /* 0 si pthread_create fallo (recursos agotados): no se debe hacer join */
    ReporteMetricas resultado;
} PruebaDiferida;

/* Estado global de las pruebas diferidas ya lanzadas/en curso. Se exponen
 * para que el llamador (main) pueda recorrerlas una vez termine la
 * ejecucion en vivo. */
extern PruebaDiferida **pruebas;
extern int total_pruebas;
extern pthread_mutex_t mtx_pruebas;

/* Ruta absoluta de injector.so, calculada a partir de la ubicacion del
 * propio ejecutable de torturete (no depende del directorio de trabajo). */
void motor_configurar_ruta_injector(const char *ruta);

/* Numero maximo de pruebas diferidas que se ejecutan en paralelo (procesos
 * hijo simultaneos). Por defecto, uno por CPU; ajustable con la variable
 * de entorno TORTURETE_JOBS. */
int obtener_max_paralelo(void);

/* Ejecuta el programa objetivo EN VIVO: stdin/stdout/stderr reales,
 * lanzando en tiempo real un hilo dormido por cada malloc/calloc/realloc,
 * y despertandolos a todos en cuanto el programa objetivo termina (de
 * forma normal, por Ctrl+C, por Ctrl+D interpretado por el propio
 * programa, o por un crash real). Al volver, todas las pruebas diferidas
 * ya han terminado y sus resultados estan disponibles en 'pruebas'.
 * Devuelve el resultado de la propia ejecucion en vivo (comportamiento
 * base: crash/leak/double-free sin ningun fallo de malloc forzado). */
ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo, sem_t *sem_concurrencia);

#endif