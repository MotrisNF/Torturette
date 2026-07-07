#include <stdlib.h>
#include <unistd.h>

int ft_doble_free(char **matriz)
{
    free(matriz);
    return (0);
}

int main(void)
{
    char **matriz;
    int i = 0;
    matriz = malloc(sizeof(char *) * (10 + 1));
    int j = 0;
    while (i < 10)
    {
        j = 0;
        matriz[i] = malloc(sizeof(char) * 10);
        if (!matriz[i])
            return(free(matriz), ft_doble_free(matriz));
        while(j < 10)
            matriz[i][j++] = '\0';
        i ++;
    }
    return(0);
}