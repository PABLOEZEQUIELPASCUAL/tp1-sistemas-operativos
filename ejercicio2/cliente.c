/*
 * client.c
 *
 * Cliente para el Ahorcado:
 * - Se conecta al servidor, recibe el estado inicial y maneja TRY:<letra>, QUIT, HELP.
 * - Tras cada partida (GAMEOVER), pregunta "¿Querés jugar otra? (S/N)"
 *   Si responde "S", envía "PLAYSe perdió la conexión con el servidor. El juego se cerrará.\n" y empieza nueva partida sin reconectar.
 *   Si responde "N", envía "QUIT\n" y finaliza.
 * - Ignora mensajes diferentes a "STATE:..." o "GAMEOVER:...".
 * - Maneja desconexión inesperada del servidor (SIGPIPE ignorado).
 *
 * Autor: Tú mismo
 * Fecha: 2025
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_BUFFER 512
#define MAX_INPUT 32  // Tamaño máximo de entrada usuario

// Muestra ayuda y reglas del juego
void mostrar_ayuda() {
    printf("\n=== JUEGO DEL AHORCADO ===\n");
    printf("Comandos disponibles:\n");
    printf("  TRY:<letra>  - Intentar adivinar una letra (ej: TRY:a)\n");
    printf("  QUIT         - Salir del juego\n");
    printf("  HELP         - Mostrar esta ayuda\n");
    printf("\nReglas:\n");
    printf("- Tienes 6 intentos para adivinar la palabra\n");
    printf("- Las letras ya usadas se muestran en el estado\n");
    printf("- Puedes usar mayúsculas o minúsculas\n");
    printf("========================\n\n");
}

// Procesa línea "STATE:palabra|intentos|letras"
void procesar_estado(const char *estado) {
    char palabra[MAX_BUFFER] = {0};
    int intentos = 0;
    char letras_usadas[MAX_BUFFER] = {0};

    // Escanea hasta '|', luego el entero, luego hasta '\n'
    if (sscanf(estado, "STATE:%[^|]|%d|%[^\n]", palabra, &intentos, letras_usadas) >= 2) {
        printf("\nPalabra actual: %s\n", palabra);
        printf("Intentos restantes: %d\n", intentos);
        if (strlen(letras_usadas) > 0) {
            printf("Letras usadas: %s\n", letras_usadas);
        }
    }
}

// Pregunta al usuario si quiere jugar otra y devuelve 1=SI, 0=NO
int preguntar_replay() {
    char resp[4];
    while (1) {
        printf("¿Querés jugar otra? (S/N): ");
        fflush(stdout);
        if (!fgets(resp, sizeof(resp), stdin)) {
            return 0; // EOF, damos por NO
        }
        // Convertir a mayúscula y tomar primera letra
        char c = toupper(resp[0]);
        if (c == 'S') return 1;
        if (c == 'N') return 0;
        printf("Respuesta inválida. Escribí 'S' o 'N'.\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <IP_Servidor> <Puerto>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    const char *ip_servidor = argv[1];
    int puerto = atoi(argv[2]);

    int sockfd;
    struct sockaddr_in serv_addr;

    // Crear socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket cliente");
        exit(EXIT_FAILURE);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(puerto);
    if (inet_pton(AF_INET, ip_servidor, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Dirección inválida: %s\n", ip_servidor);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Conectando a %s:%d …\n", ip_servidor, puerto);
    // connect() bloqueará si el servidor ya tiene MAX_CLIENTES activos:
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Conectado al servidor %s:%d\n", ip_servidor, puerto);
    mostrar_ayuda();

    char buffer_recv[MAX_BUFFER];
    char buffer_send[MAX_INPUT];
    int bytes;

    // ========== Recibir estado inicial (espera bloqueo si hay 5 clientes) ==========
    printf("Esperando a que el servidor envíe el estado inicial…\n");
    memset(buffer_recv, 0, sizeof(buffer_recv));
    bytes = recv(sockfd, buffer_recv, sizeof(buffer_recv) - 1, 0);
    if (bytes <= 0) {
        printf("Error o desconexión antes de recibir estado inicial.\n");
        close(sockfd);
        return 1;
    }
    buffer_recv[bytes] = '\0';

    // Puede venir "ERROR:" si el servidor está cerrándose
    if (strncmp(buffer_recv, "ERROR:", 6) == 0) {
        printf("%s", buffer_recv);
        close(sockfd);
        return 1;
    }

    // Configurar timeout de recepción (5 segundos) DESPUÉS de recibir estado inicial
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        perror("setsockopt timeout");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Podría llegar "STATE:...|...|...\n¡Acierto!" o "STATE:…\n" si no hay extra
    // En cualquier caso, procesamos la línea STATE y descartamos el resto por ahora
    if (strncmp(buffer_recv, "STATE:", 6) == 0) {
        // Extraer parte hasta la primera línea nueva
        char *newline = strchr(buffer_recv, '\n');
        if (newline) {
            *newline = '\0';
        }
        procesar_estado(buffer_recv);
    }

    // ========== Bucle principal de juego ==========
    while (1) {
        // Leer comando del usuario
        printf("\nIngrese comando > ");
        fflush(stdout);
        memset(buffer_send, 0, sizeof(buffer_send));
        if (!fgets(buffer_send, sizeof(buffer_send), stdin)) {
            // EOF o error
            break;
        }
        // Quitar '\n'
        size_t len = strlen(buffer_send);
        if (len > 0 && buffer_send[len - 1] == '\n') {
            buffer_send[len - 1] = '\0';
            len--;
        }

        // HELP
        if (strcmp(buffer_send, "HELP") == 0) {
            mostrar_ayuda();
            continue;
        }

        // QUIT
        if (strcmp(buffer_send, "QUIT") == 0) {
            send(sockfd, "QUIT\n", 5, 0);
            memset(buffer_recv, 0, sizeof(buffer_recv));
            bytes = recv(sockfd, buffer_recv, sizeof(buffer_recv) - 1, 0);
            if (bytes > 0) {
                buffer_recv[bytes] = '\0';
                printf("%s", buffer_recv);  // "BYE\n"
            }
            break;
        }

        // Validar TRY:<letra>
        if (strncmp(buffer_send, "TRY:", 4) != 0 || strlen(buffer_send) != 5) {
            printf("Formato inválido. Use TRY:<letra> (ej: TRY:a) o escriba HELP.\n");
            continue;
        }
        buffer_send[4] = tolower(buffer_send[4]);
        if (!isalpha(buffer_send[4])) {
            printf("Error: Debe ingresar una letra válida (a-z).\n");
            continue;
        }

        // Enviar TRY con '\n'
        char cmd_with_nl[MAX_INPUT + 2];
        snprintf(cmd_with_nl, sizeof(cmd_with_nl), "%s\n", buffer_send);
        if (send(sockfd, cmd_with_nl, strlen(cmd_with_nl), 0) < 0) {
            if (errno == EPIPE) {
                printf("\nSe perdió la conexión con el servidor. El juego se cerrará.\n");
            } else {
                perror("send");
            }
            break;
        }

        // Recibir respuesta: puede incluir STATE + mensaje extra + GAMEOVER en un solo bloque
        memset(buffer_recv, 0, sizeof(buffer_recv));
        bytes = recv(sockfd, buffer_recv, sizeof(buffer_recv) - 1, 0);
        if (bytes <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("\nTimeout esperando respuesta del servidor. El servidor podría estar caído.\n");
                printf("Se perdió la conexión con el servidor. El juego se cerrará.\n");
            } else {
                printf("\nEl servidor se desconectó inesperadamente.\n");
                printf("Se perdió la conexión con el servidor. El juego se cerrará.\n");
            }
            break;
        }
        buffer_recv[bytes] = '\0';

        // Si viene "ERROR:" (raro en el juego normal), mostramos y salimos
        if (strncmp(buffer_recv, "ERROR:", 6) == 0) {
            if (strncmp(buffer_recv, "ERROR:Letra ya usada", 19) == 0) {
                printf("\nEsa letra ya fue usada. Intenta con otra.\n");
                continue;
            } else if (strncmp(buffer_recv, "ERROR:No quedan intentos", 23) == 0) {
                printf("\nNo te quedan más intentos. Espera a que termine el juego.\n");
                continue;
            } else {
                printf("\n%s ", buffer_recv);
                printf("Se perdió la conexión con el servidor. El juego se cerrará.\n");
                break;
            }
        }

        // 1) Separar en líneas: "STATE:..." posible "¡Acierto!"/¡Letra incorrecta" y luego "GAMEOVER:..."
        //    Cada línea termina en '\n'. Podemos tokenizar por "\n".
        char *line = NULL;
        char *rest = buffer_recv;
        int line_num = 0;
        char estado_line[MAX_BUFFER] = {0};
        char extra_line[MAX_BUFFER] = {0};
        char gameover_line[MAX_BUFFER] = {0};

        while ((line = strsep(&rest, "\n")) != NULL) {
            if (strlen(line) == 0) continue;
            if (strncmp(line, "STATE:", 6) == 0) {
                strncpy(estado_line, line, sizeof(estado_line) - 1);
                line_num++;
            } else if (strncmp(line, "WIN", 3) == 0 ||
                       strncmp(line, "LOSE|", 5) == 0 ||
                       strcmp(line, "¡Acierto!") == 0 ||
                       strcmp(line, "Letra incorrecta") == 0) {
                // Esta línea es mensaje extra o parte del STATE
                strncpy(extra_line, line, sizeof(extra_line) - 1);
                line_num++;
            } else if (strncmp(line, "GAMEOVER:", 9) == 0) {
                strncpy(gameover_line, line, sizeof(gameover_line) - 1);
                line_num++;
            }
        }

        // 2) Mostrar el estado y mensaje extra si existen
        if (strlen(estado_line) > 0) {
            procesar_estado(estado_line);
        }
        if (strlen(extra_line) > 0) {
            printf("%s\n", extra_line);
        }

        // 3) Si encontramos "GAMEOVER:", procesarlo y preguntar replay
        if (strlen(gameover_line) > 0) {
            if (strncmp(gameover_line, "GAMEOVER:WIN", 12) == 0) {
                printf("\n¡¡FELICITACIONES!! ¡Has ganado esta partida!\n");
            } else if (strncmp(gameover_line, "GAMEOVER:LOSE:", 14) == 0) {
                // Extraer palabra
                char *pal = strchr(gameover_line, ':') + 1;
                pal = strchr(pal, ':') + 1;
                printf("\n¡GAME OVER! La palabra era: %s\n", pal);
            }

            // ====== Preguntar si quiere jugar otra ======
            int jugar_otra = preguntar_replay();
            if (jugar_otra) {
                if (send(sockfd, "PLAY\n", 5, 0) < 0) {
                    if (errno == EPIPE) {
                        printf("\nSe perdió la conexión con el servidor. El juego se cerrará.\n");
                    } else {
                        perror("send");
                    }
                    break;
                }
                // Esperar nuevo STATE inicial
                memset(buffer_recv, 0, sizeof(buffer_recv));
                bytes = recv(sockfd, buffer_recv, sizeof(buffer_recv) - 1, 0);
                if (bytes <= 0) {
                    printf("\nEl servidor se desconectó al iniciar nueva partida.\n");
                    printf("Se perdió la conexión con el servidor. El juego se cerrará.\n");
                    break;
                }
                buffer_recv[bytes] = '\0';
                // El buffer debe contener "STATE:..."; lo procesamos tal cual
                if (strncmp(buffer_recv, "STATE:", 6) == 0) {
                    procesar_estado(buffer_recv);
                }
                continue;  // Volver a bucle de juego
            } else {
                send(sockfd, "QUIT\n", 5, 0);
                memset(buffer_recv, 0, sizeof(buffer_recv));
                bytes = recv(sockfd, buffer_recv, sizeof(buffer_recv) - 1, 0);
                if (bytes > 0) {
                    buffer_recv[bytes] = '\0';
                    printf("%s", buffer_recv);  // "BYE\n"
                }
                break;
            }
        }
        // Si no había "GAMEOVER:", el bucle continúa normalmente
    }

    close(sockfd);
    printf("\nConexión cerrada. ¡Hasta la próxima!\n");
    return 0;
}
