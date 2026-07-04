# Torturette

> No viene a comprobar tu programa. Viene a confirmar lo que ya sospechabas.

Torturette no es una herramienta más. Es la forma elegante de decirle a un programa que, por muy orgulloso que se sienta, sigue siendo vulnerable a los errores más básicos. Mientras otros programas se pasean por el mundo con su `malloc` y su `free` como si fueran dioses, Torturette entra, les quita la máscara y les demuestra, con una mezcla de frialdad técnica y mala leche bien organizada, que un pequeño fallo de memoria puede ser suficiente para convertir una ejecución brillante en un desastre perfectamente documentado.

Porque claro, hay gente que escribe software como si todo fuera un juego de niños. Uno compila, uno corre, uno mira que no haya warnings, y ya se cree el dueño del universo. Uno incluso llega a pensar que "compila a la primera" es sinónimo de "está bien escrito", que son cosas completamente distintas y solo una de ellas es cierta en tu caso. Entonces aparece Torturette, con el entusiasmo de quien sabe que el programa va a caer, y le muestra al usuario final que su binario no era tan invencible como pretendía.

No es que Torturette tenga algo personal contra ti. Es que tu código tampoco le ha dado motivos para confiar. Fugas, doble free, aborts, accesos inválidos, señales de muerte y otras pequeñas muestras de incompetencia de bajo nivel: todo eso termina siendo expuesto con una claridad que hace bastante incómoda la experiencia para quien pensaba que su código era "casi estable". Casi estable es, para quien esto escribe, la forma más elegante que existe de decir "inestable", solo que con menos vergüenza.

Vas a intentar defenderte. Vas a decir que "eso nunca pasa en producción", que "esa rama del código no se ejecuta nunca", que "eso ya lo tenía controlado". Torturette ha oído esa misma frase de cientos de binarios distintos, todos ellos ahora reposando en un `core dump`. No discute contigo. Simplemente ejecuta, presiona, y deja que los hechos hablen, que para eso son mucho menos susceptibles que tú.

En resumen, Torturette no busca ser bonito. Busca ser implacable. Es la clase de herramienta que le hace la pregunta incómoda a un programa que pensaba que estaba bien: "¿Y esto qué pasa cuando algo se rompe de verdad?" Y la respuesta, casi siempre, es un montón de inutilidad evidente muy difícil de ignorar.

## Índice

- [Qué hace](#qué-hace)
- [Cómo usarlo](#cómo-usarlo)
- [Qué ofrece](#qué-ofrece)
- [Aspecto técnico](#aspecto-técnico)
- [Limitaciones](#limitaciones)
- [Prueba rápida](#prueba-rápida)

## Qué hace

El flujo de trabajo de Torturette es relativamente simple en la superficie, pero bastante agresivo en la práctica. El programa:

1. muestra un banner inicial y prepara el entorno de ejecución;
2. ejecuta el binario objetivo en un modo normal para observar su comportamiento base;
3. lanza una serie de ejecuciones adicionales en segundo plano, forzando situaciones de fallo de asignación o de manejo de memoria;
4. analiza los resultados y genera un informe con los casos sospechosos, los eventos más relevantes y los puntos donde el fallo parece haber ocurrido.

No se limita a "reintentar" de forma inocente. Lo que hace es someter al programa a una presión deliberada para comprobar si resiste o si, por el contrario, deja pruebas de su fragilidad en forma de errores, señales, aborts o comportamientos indefinidos. Torturette no tiene curiosidad, tiene método.

## Cómo usarlo

Compila el proyecto con:

```bash
make
```

Si algo no compila, no es culpa del Makefile. Revisa tu código primero, luego vuelve a echarle la culpa al Makefile si te quedas más tranquilo.

### `make torture`, la forma en la que esto está pensado para usarse

Olvídate de preparar nada a mano. La idea de Torturette no es que le indiques un binario ya compilado y cruces los dedos: es que dejes tu propio código, el que estás escribiendo ahora mismo, tirado en la raíz del proyecto, y le des directamente:

```bash
make torture
```

Con eso, Torturette compila **todos** los `.c` que haya en la raíz, junta el resultado en un único binario y lo somete inmediatamente a la tanda completa de fallos de memoria. Sin pasos intermedios, sin que tengas que acordarte de compilar tú antes, sin excusas de por medio. Un comando, y tu código ya está siendo puesto en su sitio.

Y si tu programa necesita argumentos, se los pasas tal cual, sin variables raras ni sintaxis de por medio:

```bash
make torture arg1 arg2
```

Eso es todo. Lo que escribas después de `torture` llega intacto a tu binario, como si lo hubieras ejecutado tú mismo, solo que esta vez alguien más está mirando por encima de tu hombro para ver si aguanta. Si no dejas ningún `.c` en la raíz, Torturette no se va a inventar un culpable: simplemente te va a decir que no hay nada que torturar, porque hasta para eso hace falta que aportes algo.

Este es el flujo pensado para el uso diario: escribes, `make torture`, lees el informe, lloras un poco, corriges, repites. El resto de este apartado es para quien quiera hacerlo de forma más manual, o para quien todavía no ha entendido que `make torture` ya se lo resuelve todo.

### Uso manual

Si prefieres compilar tú mismo el binario objetivo y pasárselo directamente a Torturette, también puedes:

```bash
./torturette <programa_objetivo> [argumentos...]
```

Por ejemplo:

```bash
./torturette ./try_me
```

### Resto de targets del Makefile

```bash
make          # construye torturette y el injector
make try      # compila try_me y lo somete a torturette
make torture  # compila TODOS los .c de la raiz y los somete a torturette
make silence  # como torture, pero con torturette calladito.
make clean    # limpia objetos y binarios de prueba
make fclean   # limpia todo. Tu codigo no. A veces hay que ser buena gente
make re       # fclean + all, por si te piensas que creandolo de nuevo tu codigo se va a arreglar.
make help     # por si con todo esto no te ha quedado claro
```

## Qué ofrece

- Ejecución normal del programa objetivo.
- Pruebas automáticas de fallo de memoria.
- Detección de problemas como:
  - fugas,
  - doble free,
  - crashes por señal,
  - aborts,
  - accesos inválidos,
  - estados de ejecución anómalos.
- Informe final con información útil para localizar el punto problemático.
- Integración con un programa de prueba sencillo para simular fallos manualmente.

## Aspecto técnico

Torturette está pensado como una herramienta de análisis orientada a la ejecución y a la observación de comportamientos anómalos. Su diseño se basa en una separación clara entre el orquestador principal, los módulos de ejecución y los módulos de reporte.

### Estructura general

El proyecto está compuesto por varios módulos:

- `main.c`: coordina la ejecución general del flujo.
- `motor.c` / `motor.h`: encapsulan la lógica de ejecución del programa objetivo y la gestión de los casos de prueba.
- `informe.c` / `informe.h`: se encargan de mostrar el banner, resumir los resultados y presentar los informes de fallo.
- `injector.c`: implementa la lógica relacionada con la inyección de condiciones de prueba y el comportamiento de los casos de fallo.
- `comun.h`: contiene definiciones y estructuras compartidas usadas por todo el sistema.
- `try/try_me.c`: es un programa de ejemplo pensado para provocar errores de forma controlada y comprobar cómo reacciona la herramienta.

### Modelo de ejecución

El flujo se puede entender como una combinación entre un análisis base y una fase de estrés. En la primera, Torturette ejecuta el programa objetivo de forma habitual para obtener una referencia de comportamiento. En la segunda, habilita condiciones de fallo artificiales para verificar si el binario se comporta de forma robusta o si deja señales de problemas ocultos.

Esta estrategia permite distinguir entre:

- fallos que aparecen de inmediato,
- fallos que se manifiestan solo bajo presión,
- comportamientos que parecen correctos pero dejan recursos mal gestionados,
- ejecuciones que terminan abruptamente y deben ser reportadas como fallos de señal o abort.

### Manejo de resultados

Cuando una prueba detecta un caso sospechoso, la herramienta intenta clasificarlo y resumirlo de forma legible. El reporte incluye información útil para el diagnóstico: tipo de problema, contexto de la ejecución y, cuando es posible, detalles de backtrace u otros datos derivados de la ejecución del binario objetivo.

Este enfoque es útil porque convierte un conjunto de fallos crudos en una secuencia de eventos que puede interpretarse de forma mucho más rápida. En otras palabras, no solo se "ve" que algo explotó, sino que también se intenta explicar qué tipo de error fue, dónde puede haber ocurrido y qué clase de comportamiento produjo.

### Relación con depuración

Aunque Torturette puede funcionar con binarios sin características de depuración, el valor del análisis aumenta considerablemente si el programa objetivo se compila con información de depuración. En ese caso, los mensajes y los informes pueden aportar mucho más contexto sobre el punto exacto en que el fallo se produjo.

Por eso, esta herramienta es más útil en entornos de desarrollo o análisis que en un contexto de ejecución final sin supervisión. No reemplaza un depurador ni una auditoría profunda, pero sí ofrece una forma rápida de forzar situaciones de error y observar cómo reacciona el software.

### Programa de prueba

El directorio [try](try) contiene un ejemplo mínimo pensado para demostrar el comportamiento de la herramienta. Ese programa acepta los siguientes comandos:

| Comando      | Qué provoca                                  |
|--------------|-----------------------------------------------|
| `malloc`     | reserva de memoria simple                     |
| `calloc`     | reserva de memoria inicializada a cero        |
| `mix`        | combinación de varias operaciones de memoria  |
| `leak`       | fuga de memoria, a propósito                  |
| `doublefree` | liberar lo mismo dos veces, por si acaso      |
| `segfault`   | acceso inválido directo, sin anestesia        |
| `abort`      | terminación abrupta controlada                |
| `exit`       | salida normal, para que veas que también sabe hacer eso |

Es especialmente útil para ver cómo Torturette reporta distintos tipos de problema sin necesidad de preparar un programa complejo desde cero.

## Limitaciones

- No es una herramienta de producción ni un sustituto de un depurador completo.
- El análisis depende de que el programa objetivo sea compilado con información de depuración si se quiere ver el origen exacto de los fallos.
- Algunos casos pueden terminar de forma abrupta, lo que es esperable en un entorno de prueba de fallos.
- El comportamiento puede variar según el sistema, la libc y las opciones de compilación del binario objetivo.

## Prueba rápida

El directorio [try](try) contiene un pequeño programa de ejemplo pensado para que el usuario pruebe el flujo. Se construye con:

```bash
make try
```

Ese programa permite probar comandos simples para ver cómo reacciona la herramienta y cómo se materializan los fallos en los informes. Si después de esto tu programa sigue "funcionando perfectamente", no es que sea perfecto: es que aún no lo hemos probado lo suficiente.