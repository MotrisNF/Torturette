#ifndef INFORME_H
#define INFORME_H

void mostrar_banner(void);

/* Muestra el backtrace de un caso (fail_after == -1 para la ejecucion
 * base/en vivo, o el indice de la llamada de malloc/pthread_create que
 * fallo), resolviendo direcciones a archivo:linea via addr2line. Si
 * 'salida_colgada' no es NULL, el caso es un CUELGUE (timeout): se
 * muestra esa salida capturada en vez de intentar resolver un backtrace
 * (no hay ninguno: el proceso fue matado, no crasheo). */
void mostrar_backtrace(int fail_after, const char *motivo, const char *backtrace_crudo, const char *salida_colgada, const char *binario);

/* Registra un caso sospechoso (leak/crash/double-free/cuelgue) para
 * mostrarlo al final con su backtrace resuelto (o su salida capturada, si
 * es un cuelgue). Copia 'backtrace'/'salida_colgada' internamente. */
void registrar_caso(int fail_after, const char *motivo, const char *backtrace, const char *salida_colgada);

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