# torturette


**Esta herramienta NO est diseñaada para su uso durante evaluaciones y examenes. Su ejecucion durante esos procesos es indeterminada. Aprendamos a aprender.**


**torturette** es una herramienta de análisis de robustez de memoria para programas C en Linux.  
Fuerza, de forma sistemática, que cada `malloc`, `realloc` y `free` del programa objetivo falle por turno, y reporta exactamente qué ocurre: crash, leak, double free, o si el programa lo maneja bien.

> Creada por **saperez-** a raíz de la frustración de no poder encontrar los fallos reales de memoria en proyectos del campus 42 — especialmente en `get_next_line` — donde los errores de gestión de memoria solo aparecen cuando una asignación falla en producción, y las herramientas convencionales no simulan ese escenario de forma sencilla.

---

## Compilación rápida

```bash
# Compilar torturette
gcc -Wall -Wextra -o torturette torturette.c

# Compilar el programa a analizar (OBLIGATORIO: -g -no-pie)
gcc -g -no-pie -o mi_programa mi_programa.c
```

> **`-g`** genera símbolos de debug necesarios para que `addr2line` resuelva archivo:línea exactos.  
> **`-no-pie`** fija las direcciones del binario en disco. Sin él, ASLR desplaza las direcciones en cada ejecución y `addr2line` no puede mapearlas correctamente.

---

## Uso

```bash
./torturette ./mi_programa
./torturette ./mi_programa arg1 arg2    # con argumentos
```

torturette acepta cualquier binario como objetivo. Los argumentos adicionales se pasan directamente al programa.

### Ejemplo de salida

```
Calibrando binario objetivo...
[CALIBRACIÓN] 8 malloc detectados  8 frees detectados

Iniciando Tortura de Memoria
---------------------------------------------------------------------------------
PRUEBA     | ESCENARIO        | MALLOCS   | FREES   | MEMORIA       | ESTADO
---------------------------------------------------------------------------------
#01        | Falla Malloc 1   | 0         | 0       | 0          bytes | [PASADO] OK
#02        | Falla Malloc 2   | 1         | 0       | 0          bytes | [FALLO]  SIGSEGV (Null Pointer)
#03        | Falla Malloc 3   | 2         | 1       | 32         bytes | [FALLO]  LEAK (+32 bytes colgados)
#04        | Falla Malloc 4   | 3         | 2       | 0          bytes | [FALLO]  DOUBLE FREE (Abort)
...
---------------------------------------------------------------------------------

>>> Caso FAIL_AFTER=1 (SIGSEGV)
---------------------------------------------------------------------------------
    Ocurre en      vec_push at mi_programa.c:87
    Llamado desde  main at mi_programa.c:201
---------------------------------------------------------------------------------
```

---

## Cómo funciona

torturette opera en dos fases:

**1. Calibración**  
Ejecuta el programa objetivo una vez sin ningún fallo inyectado, con `LD_PRELOAD` apuntando a `injector.so`. Cuenta el número total de asignaciones de memoria para saber cuántas pruebas hay que realizar.

**2. Tortura**  
Lanza el programa N veces (una por cada malloc detectado). En la prueba número `i`, fuerza que la asignación número `i` devuelva `NULL`. Observa el comportamiento resultante y lo clasifica:

| Estado | Significado |
|--------|-------------|
| `OK` | El programa detectó el fallo y lo gestionó correctamente |
| `SIGSEGV` | Desreferenciación de NULL — el retorno de malloc no se comprobó |
| `SIGABRT` | Double free o corrupción de heap detectada por glibc |
| `DOUBLE FREE` | El injector detectó que se liberó el mismo puntero dos veces |
| `LEAK` | Malloc sin free correspondiente cuando se tomó la rama de error |
| `AVISO` | Más `free` que `malloc` — comportamiento anómalo, posible bug |

Cada ejecución corre en un proceso hijo aislado. Si el proceso revienta, torturette captura el backtrace en ese instante exacto mediante `backtrace()`/`backtrace_symbols_fd()` inyectados, y al final muestra el archivo y la línea donde ocurrió el problema.

---

## El injector

El núcleo de torturette es `injector.so`, una biblioteca compartida que se genera automáticamente en el directorio del propio ejecutable cada vez que se lanza torturette. Se carga en el proceso hijo mediante `LD_PRELOAD`, lo que le permite interceptar `malloc`, `realloc` y `free` antes de que lleguen a glibc.

El injector:
- Cuenta cada llamada a `malloc`/`realloc` y activa el fallo en la llamada número `FAIL_AFTER`
- Rastrea qué bloques están vivos para detectar leaks al salir
- Mantiene un registro de punteros liberados para detectar double free
- Instala manejadores para `SIGSEGV` y `SIGABRT` que vuelcan el backtrace justo antes de que el proceso muera
- Comunica los resultados al proceso padre mediante `stderr` con el formato `[METRICAS]|mallocs|frees|leaks|bytes`

### Por qué se ignoran los tamaños 1024 y 4096

El injector **no cuenta ni falla** los `malloc` de exactamente 1024 o 4096 bytes. Esta exclusión no es arbitraria: son los tamaños de buffer que glibc usa internamente para operaciones como `fprintf`, `fgets`, o el propio `backtrace_symbols_fd`. Si se contaran, el injector se contaminaría a sí mismo: sus propias llamadas de diagnóstico aparecerían en el conteo del programa objetivo, desplazando todos los índices y produciendo resultados incorrectos o falsos positivos. Al excluir esos dos tamaños, el injector permanece invisible para el sistema de conteo.

---

## Requisitos

- Linux (usa `LD_PRELOAD`, `backtrace()`, `/proc/self/exe`, `fork`/`exec`)
- GCC con soporte para `-shared`, `-fPIC` y `-ldl`
- `binutils` instalado (`addr2line` para resolver archivo:línea)
- El programa objetivo debe compilarse con `gcc -g -no-pie`

No se necesitan dependencias externas ni instalación. torturette se compila en un único archivo fuente y genera su `injector.so` de forma autónoma en tiempo de ejecución.

---

## Limitaciones

- **Solo Linux.** El mecanismo de `LD_PRELOAD` y `/proc/self/exe` son específicos de Linux. No funciona en macOS ni en sistemas sin soporte de biblioteca dinámica en tiempo de ejecución.

- **No intercepta `mmap` ni `calloc` directo del kernel.** Solo cubre `malloc`, `realloc` y `free`. Programas que reservan memoria directamente con `mmap` o que usan allocators personalizados no serán analizados completamente.

- **Programas con muchos malloc pueden ser lentos.** La tortura lanza `N` procesos hijos, uno por cada malloc detectado. Un programa con 500 allocations ejecutará 500 veces el binario. En la mayoría de proyectos de 42 esto es instantáneo, pero en programas grandes puede tardar.

- **`-no-pie` es obligatorio para ver archivo:línea.** Si el binario objetivo se compila sin `-no-pie`, torturette seguirá detectando los fallos correctamente, pero el análisis de backtrace mostrará solo `[!] No se pudo resolver ninguna línea` porque `addr2line` no puede mapear direcciones desplazadas por ASLR sin conocer la base de carga del proceso.

- **MAX_TRACK = 2048 bloques vivos simultáneos.** El injector rastrea hasta 2048 punteros activos a la vez. Programas que mantengan más bloques vivos simultáneamente pueden producir falsos negativos en la detección de leaks.

- **Incompatible con Valgrind.** Valgrind intercepta `malloc`/`free` a nivel de símbolo global de la misma forma que `LD_PRELOAD`, por lo que no pueden usarse juntos. Son herramientas complementarias, no combinables.

- **El double free solo se detecta si el puntero pasó antes por `malloc` rastreado.** Punteros en pila, globales, o asignados antes de que el injector se inicializara no aparecen en el registro y su double free podría no detectarse.

---

## Archivos de prueba incluidos

| Archivo | Descripción |
|---------|-------------|
| `test_victima.c` | Primera batería: SIGSEGV por NULL sin comprobar, double free en lista enlazada, leak en struct con campos múltiples |
| `test_victima2.c` | Segunda batería: SIGSEGV por `realloc` sin comprobar, double free por referencia externa guardada, leak en lista enlazada con fallo intermedio |

```bash
gcc -g -no-pie -o test_victima  test_victima.c
gcc -g -no-pie -o test_victima2 test_victima2.c

./torturette ./test_victima
./torturette ./test_victima2
```

---

*torturette — saperez- — Campus 42*
