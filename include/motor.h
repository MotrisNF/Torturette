#ifndef MOTOR_H
#define MOTOR_H

#include "comun.h"

/* Resultado de probar que pasaria si la llamada de malloc/calloc/realloc
 * numero 'indice' hubiese fallado. Se obtiene haciendo un fork() dentro
 * del propio proceso objetivo, en el instante exacto de esa llamada (ver
 * injector.c): el hijo hereda el estado real hasta ese punto, se le
 * fuerza a fallar esa unica reserva, y se le deja seguir ejecutandose
 * para ver que pasa. El padre (la ejecucion en vivo real) espera a que
 * el hijo termine antes de continuar -- por eso las pruebas se procesan
 * de una en una, en el mismo orden en que ocurren durante la ejecucion. */
typedef struct {
    int indice;
    int es_pthread; /* 1 si esta prueba es un fallo forzado de
                        pthread_create() en vez de malloc/calloc/realloc */
    ReporteMetricas resultado;
} PruebaDiferida;

/* Lista de resultados recogidos durante la ejecucion en vivo. Se rellena
 * dentro de ejecutar_vivo_y_lanzar_diferidas() a medida que se leen los
 * eventos del canal fd 3; para cuando esa funcion retorna, ya esta
 * completa. */
extern PruebaDiferida **pruebas;
extern int total_pruebas;

/* Ruta absoluta de injector.so, calculada a partir de la ubicacion del
 * propio ejecutable de torturete (no depende del directorio de trabajo). */
void motor_configurar_ruta_injector(const char *ruta);

/* Ejecuta el programa objetivo con su entrada/salida reales heredadas
 * directamente (sin grabar ni repetir nada). Cada malloc/calloc/realloc
 * que hace el programa objetivo se convierte, dentro del injector, en un
 * fork() que prueba ese fallo concreto y reporta el resultado por el
 * canal de eventos antes de que la ejecucion en vivo continue. Al
 * volver, todos los resultados estan disponibles en 'pruebas', y se
 * devuelve el resultado de la propia ejecucion en vivo (comportamiento
 * base: crash/leak/double-free sin ningun fallo de malloc forzado). */
ReporteMetricas ejecutar_vivo_y_lanzar_diferidas(char **argv_objetivo);

#endif