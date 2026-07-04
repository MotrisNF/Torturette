# Torturette

Torturette no es una herramienta más. Es la forma elegante de decirle a un programa que, por muy orgulloso que se sienta, sigue siendo vulnerable a los errores más básicos. Mientras otros programas se pasean por el mundo con su `malloc` y su `free` como si fueran dioses, Torturette entra, les quita la máscara y les demuestra, con una mezcla de frialdad técnica y mala leche bien organizada, que un pequeño fallo de memoria puede ser suficiente para convertir una ejecución brillante en un desastre perfectamente documentado.

Porque claro, hay gente que escribe software como si todo fuera un juego de niños. Uno compila, uno corre, uno mira que no haya warnings, y ya se cree el dueño del universo. Entonces aparece Torturette, con el entusiasmo de quien sabe que el programa va a caer, y le muestra al usuario final que su binario no era tan invencible como pretendía. Fugas, doble free, aborts, accesos inválidos, señales de muerte y otras pequeñas muestras de incompetencia de bajo nivel: todo eso termina siendo expuesto con una claridad que hace bastante incómoda la experiencia para quien pensaba que su código era “casi estable”.

En resumen, Torturette no busca ser bonito. Busca ser implacable. Es la clase de herramienta que le hace la pregunta incómoda a un programa que pensaba que estaba bien: “¿Y esto qué pasa cuando algo se rompe de verdad?” Y la respuesta, casi siempre, es un montón de inutilidad evidente muy difícil de ignorar.

## Qué hace

El flujo de trabajo de Torturette es relativamente simple en la superficie, pero bastante agresivo en la práctica. El programa:

1. muestra un banner inicial y prepara el entorno de ejecución;
2. ejecuta el binario objetivo en un modo normal para observar su comportamiento base;
3. lanza una serie de ejecuciones adicionales en segundo plano, forzando situaciones de fallo de asignación o de manejo de memoria;
4. analiza los resultados y genera un informe con los casos sospechosos, los eventos más relevantes y los puntos donde el fallo parece haber ocurrido.

No se limita a “reintentar” de forma inocente. Lo que hace es someter al programa a una presión deliberada para comprobar si resiste o si, por el contrario, deja pruebas de su fragilidad en forma de errores, señales, aborts o comportamientos indefinidos.

## Cómo usarlo

Compila el proyecto con:

```bash
make
```

Luego ejecuta el binario principal así:

```bash
make torture <arg1> <arg2> <arg3>
```

Puedes hacerlo asi:

```bash
./torturette <programa_objetivo> [argumentos...]
```
*Pero como se que seguramente te cueste mucho esfuerzo hacer mas cosas, no te lo recomiendo.*

También puedes usar los targets auxiliares del Makefile:

```bash
make try
make help
make clean
make fclean
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

Este enfoque es útil porque convierte un conjunto de fallos crudos en una secuencia de eventos que puede interpretarse de forma mucho más rápida. En otras palabras, no solo se “ve” que algo explotó, sino que también se intenta explicar qué tipo de error fue, dónde puede haber ocurrido y qué clase de comportamiento produjo.

### Relación con depuración

Aunque Torturette puede funcionar con binarios sin características de depuración, el valor del análisis aumenta considerablemente si el programa objetivo se compila con información de depuración. En ese caso, los mensajes y los informes pueden aportar mucho más contexto sobre el punto exacto en que el fallo se produjo.

Por eso, esta herramienta es más útil en entornos de desarrollo o análisis que en un contexto de ejecución final sin supervisión. No reemplaza un depurador ni una auditoría profunda, pero sí ofrece una forma rápida de forzar situaciones de error y observar cómo reacciona el software.

### Programa de prueba

El directorio [try](try) contiene un ejemplo mínimo pensado para demostrar el comportamiento de la herramienta. Ese programa acepta comandos como `malloc`, `calloc`, `mix`, `leak`, `doublefree`, `segfault`, `abort` y `exit`, y permite reproducir varias clases de fallo de forma controlada. Es especialmente útil para ver cómo Torturette reporta distintos tipos de problema sin necesidad de preparar un programa complejo desde cero.

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

Ese programa permite probar comandos simples para ver cómo reacciona la herramienta y cómo se materializan los fallos en los informes.
