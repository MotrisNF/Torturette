CC       = gcc
CFLAGS   = -Wall -Wextra -g -no-pie
LDFLAGS  = -ldl -pthread
Q        = @

NAME       = torturette
INJECTOR   = injector.so
TESTER     = try_me
TRY_DIR    = try
TRY_SRC    = $(TRY_DIR)/try_me.c
TRY_BIN    = $(TESTER)

SRC_DIR    = src
INC_DIR    = include
OBJ_DIR    = obj

SRCS       = $(SRC_DIR)/main.c $(SRC_DIR)/motor.c $(SRC_DIR)/informe.c
OBJS       = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

all: banner $(NAME) $(INJECTOR) finish

banner:
	@echo "Creando maquina de tortura"
	@for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo

finish:
	@echo "torturette generada con exito"

$(NAME): $(OBJS)
	$(Q)$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(INJECTOR): $(SRC_DIR)/injector.c | $(OBJ_DIR)
	$(Q)$(CC) -shared -fPIC -Wall -Wextra -o $@ $(SRC_DIR)/injector.c -ldl -lpthread

try: $(TRY_BIN)
	@if [ -x $(NAME) ]; then \
		./$(NAME) ./$(TRY_BIN); \
	else \
		echo "Crea primero la maquina de tortura, inutil"; \
	fi

$(TRY_BIN): $(TRY_SRC)
	$(Q)$(CC) $(CFLAGS) -o $@ $(TRY_SRC)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(INC_DIR)/comun.h $(INC_DIR)/motor.h $(INC_DIR)/informe.h | $(OBJ_DIR)
	$(Q)$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(OBJ_DIR):
	$(Q)mkdir -p $@

clean:
	@echo "Eliminando restos de torturas anteriores"
	$(Q)rm -f $(OBJS) $(TRY_BIN)
	$(Q)rm -f $(TRY_DIR)/try_me.c~ 2>/dev/null || true

fclean:
	@if [ "$(filter re,$(MAKECMDGOALS))" != "" ]; then \
		$(MAKE) --no-print-directory clean >/dev/null 2>&1; \
		rm -f $(NAME) $(INJECTOR) .torturete_stdin.tmp >/dev/null 2>&1; \
		rmdir --ignore-fail-on-non-empty $(OBJ_DIR) 2>/dev/null || true; \
		exit 0; \
	fi; \
	bash -lc 'read -r -p "¿Estas seguro? Aún puedo seguir rompiéndote cosas [y][N] " ans; if [ "$$ans" != "y" ] && [ "$$ans" != "Y" ] && [ -n "$$ans" ]; then echo "Operacion cancelada."; exit 0; fi; $(MAKE) --no-print-directory clean; for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; read -r -p "¿Vas a desacerte de mi? ¿Con lo que hemos vivido juntos? [y][N] " ans2; if [ "$$ans2" != "y" ] && [ "$$ans2" != "Y" ] && [ -n "$$ans2" ]; then echo "Operacion cancelada."; exit 0; fi; for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; echo "Me borras, pero volveras a usarme..."; rm -f $(NAME) $(INJECTOR) .torturete_stdin.tmp; rmdir --ignore-fail-on-non-empty $(OBJ_DIR) 2>/dev/null || true'

warning:
	@echo "No deberias de estar tocando el codigo..."

help:
	@echo "Si has recurrido a esto, es que eres mas tonto de lo que yo pensaba..."
	@echo ""
	@echo "make all: construye la maquina principal y la libreria, porque sin mi ayuda te quedarías a medias."
	@echo "make try: compila el programa de prueba y lo lanza con torturette, como si fueras incapaz de hacerlo por tu cuenta."
	@echo "make torture: compila los .c que tengas en la raiz y los somete a torturette, que es mucho mas listo que tu codigo."
	@echo "  (lo que escribas despues se lo pasas de argumento a tu propio binario, ej: make torture arg1 arg2)"
	@echo "make silence: me callas de una vez, que ya cansa tanto comentario mientras te destrozo el codigo."
	@echo "make clean: limpia los restos del caos, aunque ya sabes que volveran a aparecer."
	@echo "make fclean: borra casi todo, por si pensabas que ibas a conservar algo de dignidad."
	@echo "make help: vuelve a mostrar esta ayuda, porque evidentemente lo necesitas."
	@echo ""
	@echo "Si prefieres no usar make torture, puedes invocar torturette directamente, pero si estas usando esto no te lo recomiendo:"
	@echo "  ./torturette <mi_victima> [argumentos...]"
	@echo "Te lo escribo bien, que si no no te vas a enterar de nada:"
	@echo "  ./torturette ./tu_basura arg1 arg2 ..."

# Compila todos los .c que haya en la raiz y los prueba con torturette.
# Todo lo que escribas despues de "make torture" se pasa tal cual como
# argumento al binario resultante, ej: make torture arg1 arg2
ifeq (torture,$(firstword $(MAKECMDGOALS)))
  TORTURE_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  $(eval $(TORTURE_ARGS):;@:)
endif

torture:
	@if [ -x $(NAME) ]; then \
		$(CC) $(CFLAGS) *.c -o a.out $(LDFLAGS) 2>/dev/null; \
		if [ -x a.out ]; then \
			echo "No se si estas preparado para esto..."; \
			sleep 0.5; \
			./$(NAME) ./a.out $(TORTURE_ARGS) $(ARGS); \
		else \
			echo "No hay nada util que compilar en la raiz"; \
		fi; \
	else \
		echo "Crea primero la maquina de tortura, inutil"; \
	fi

ifeq (silence,$(firstword $(MAKECMDGOALS)))
  SILENCE_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  $(eval $(SILENCE_ARGS):;@:)
endif

silence:
	@if [ -x $(NAME) ]; then \
		bash -lc ' \
		read -r -p "¿Quieres que me calle? [y][N] " a1; \
		echo "$$a1"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "¿De verdad, de verdad? [y][N] " a2; \
		echo "$$a2"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "¿Sabes lo que es un torturette sin voz? Ni yo. [y][N] " a3; \
		echo "$$a3"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "¿Has pensado en los bugs que se te van a escapar sin mis comentarios? [y][N] " a4; \
		echo "$$a4"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		echo "Vale... Ejecutando tortura"; \
		for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; \
		echo "¿De verdad te lo has creido?"; \
		sleep 0.8; \
		read -r -p "¿Prometes portarte bien si me callo? [y][N] " a5; \
		echo "$$a5"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "¿Seguro seguro seguro? [y][N] " a6; \
		echo "$$a6"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "Ultima pregunta, lo juro (mentira): ¿en serio quieres silencio? [y][N] " a7; \
		echo "$$a7"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "Si te digo que si, ¿me dejas en paz? [y][N] " a8; \
		echo "$$a8"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "¿Y si te cobro por cada pregunta que no contestas? [y][N] " a9; \
		echo "$$a9"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		read -r -p "Va, esta es la penultima de verdad [y][N] " a10; \
		echo "$$a10"; \
		for i in 1 2; do printf "."; sleep 0.5; done; echo; \
		echo "Dejame pensarlo un momento..."; \
		for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; \
		echo "No. Se me habia olvidado con quien estaba hablando."; \
		echo "Yo no me callo. Nunca lo he hecho, y hoy tampoco va a ser la excepcion..."; \
		for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; \
		$(CC) $(CFLAGS) *.c -o a.out $(LDFLAGS) 2>/dev/null; \
		if [ -x a.out ]; then \
			./$(NAME) ./a.out $(SILENCE_ARGS) $(ARGS); \
		else \
			echo "No hay nada util que compilar en la raiz"; \
		fi'; \
	else \
		echo "Crea primero la maquina de tortura, inutil"; \
	fi

re: warning fclean all

.PHONY: all clean fclean re try help torture silence warning banner finish