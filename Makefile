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

# Captura argumentos pasados como objetivos extra: `make torture arg1 arg2`
ARGS := $(filter-out torture,$(MAKECMDGOALS))

.PHONY: $(ARGS)

$(ARGS):
	@:

all: banner $(NAME) $(INJECTOR) finish

banner:
	@echo "Creando maquina de tortura"
	@for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo

finish:
	@echo "Ya estas a mi servicio"
	@for i in 1 2 3; do printf "."; sleep 0.5; done; echo
	@echo "Si eres igual de tonto de lo que creo, make help te ayudara a serlo algo menos"

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
	$(Q)rm -f $(OBJS) $(TRY_BIN) a.out
	$(Q)rm -f $(TRY_DIR)/try_me.c~ 2>/dev/null || true

fclean:
	@bash -lc 'if [ "$(filter re,$(MAKECMDGOALS))" != "" ]; then $(MAKE) --no-print-directory clean >/dev/null 2>&1; rm -f $(NAME) $(INJECTOR) .torturete_stdin.tmp >/dev/null 2>&1; rmdir --ignore-fail-on-non-empty $(OBJ_DIR) 2>/dev/null || true; exit 0; fi; read -r -p "¿Estas seguro? Aún puedo seguir rompiéndote cosas [y][N] " ans; if [ "$$ans" != "y" ] && [ "$$ans" != "Y" ] && [ -n "$$ans" ]; then echo "Operacion cancelada."; exit 0; fi; $(MAKE) --no-print-directory clean; for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; read -r -p "¿Vas a desacerte de mi? ¿Con lo que hemos vivido juntos? [y][N] " ans2; if [ "$$ans2" != "y" ] && [ "$$ans2" != "Y" ] && [ -n "$$ans2" ]; then echo "Operacion cancelada."; exit 0; fi; for i in 1 2 3 4 5; do printf "."; sleep 0.5; done; echo; echo "Me borras, pero volveras a usarme..."; rm -f $(NAME) $(INJECTOR) .torturete_stdin.tmp; rmdir --ignore-fail-on-non-empty $(OBJ_DIR) 2>/dev/null || true'

warning:
	@echo "No deberias de estar tocando el codigo..."

help:
	@echo "Si has recurrido a esto, es que eres mas tonto de lo que yo pensaba..."
	@echo ""
	@echo "make all: construye la maquina principal y la libreria, porque sin mi ayuda te quedarías a medias."
	@echo "make try: compila el programa de prueba y lo lanza con torturette, como si fueras incapaz de hacerlo por tu cuenta."
	@echo "make torture: compila los .c que tengas en la raiz y los somete a torturette, que es mucho mas listo que tu codigo."
	@echo "make torture <arg1> <arg2> etc tambien funciona. He pensado que no sabrias hacerlo solo"
	@echo "make clean: limpia los restos del caos, aunque ya sabes que volveran a aparecer."
	@echo "make fclean: borra casi todo, por si pensabas que ibas a conservar algo de dignidad."
	@echo "make help: vuelve a mostrar esta ayuda, porque evidentemente la necesitas."
	@echo ""
	@echo "Si prefieres no usar make torture, puedes invocar torturette directamente, pero si estas usando esto no te lo recomiendo:"
	@echo "  ./torturette <mi_victima> [argumentos...]"
	@echo "Te lo escribo bien, que si no no te vas a enterar de nada:"
	@echo "  ./torturette ./tu_basura arg1 arg2 ..."
	@echo ""
	@echo "Solo te deseo suerte, que se que la necesitas"
	@echo "Y ahora, si algo sale mal, no me vengas llorando a mi, que yo no tengo la culpa de tu inutilidad,"
	@echo "Aun que saperez- pude echarte una mano, que ya es mas de lo que temereces."

# Compila todos los .c que haya en la raiz y los prueba con torturette.
torture:
	@if [ -x $(NAME) ]; then \
		$(CC) $(CFLAGS) *.c -o a.out $(LDFLAGS) 2>/dev/null; \
		if [ -x a.out ]; then \
			echo "No se si estas preparado para esto..."; \
			sleep 0.5; \
			./$(NAME) ./a.out $(ARGS); \
		else \
			echo "Se que tu programa es malo, pero al menos ponlo aqui para que puedo compilarlo"; \
		fi; \
	else \
		echo "Crea primero la maquina de tortura, inutil"; \
	fi

re: warning fclean all

.PHONY: all clean fclean re try help torture
