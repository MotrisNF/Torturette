#ifndef INFORME_H
#define INFORME_H

void mostrar_banner(void);

/* Muestra el backtrace de un caso (fail_after == -1 para la ejecucion
 * base/en vivo, o el indice de la llamada de malloc que fallo), resolviendo
 * direcciones a archivo:linea via addr2line. */
void mostrar_backtrace(int fail_after, const char *motivo, const char *backtrace_crudo, const char *binario);

/* Registra un caso sospechoso (leak/crash/double-free) para mostrarlo al
 * final con su backtrace resuelto. Copia 'backtrace' internamente. */
void registrar_caso(int fail_after, const char *motivo, const char *backtrace);

/* Muestra todos los casos registrados con registrar_caso, con su backtrace
 * resuelto a archivo:linea. Libera la memoria interna al terminar. */
void mostrar_casos_sospechosos(const char *binario);

/* Muestra una lista de frases de ánimo en rojo oscuro cuando el resultado
 * global del test es KO. */
void mostrar_frases_ko_general(void);

/* Muestra una frase sarcastica en verde cuando el resultado global del
 * test es OK (aprobado con retintin, no vaya a ser que se lo crea). */
void mostrar_frases_ok_general(void);

#endif