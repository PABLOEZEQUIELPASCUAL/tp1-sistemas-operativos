/*
 * server.c
 *
 * Servidor de Ahorcado con Threads y Sockets TCP.
 *
 * - Acepta hasta MAX_CLIENTES concurrentes; el resto espera en la cola de listen().
 * - Imprime mensajes de información al arrancar (IP, puerto, max clientes, etc.).
 * - Refresca cada INTERVALO_REFRESCO segundos el estado de clientes activos y
 *   las estadísticas globales (partidas jugadas, ganadas, perdidas, % ganadas).
 * - Maneja SIGINT para cierre limpio: deja de aceptar, espera a que todos los clientes
 *   se desconecten y luego cierra.
 * - Usa SO_REUSEADDR para poder reiniciar inmediatamente en el mismo puerto.
 * - Permite "jugar otra partida" (PLAY) o "salir" (QUIT) tras finalizar una partida.
 * - Acumula y envía siempre la lista de letras usadas para que el cliente la muestre.
 * - Si el cliente se desconecta o envía QUIT durante la partida, la cuenta como perdida.
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
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <time.h>

// ======================= Configuración ========================
#define PUERTO 8080
#define MAX_CLIENTES 5
#define MAX_PALABRA 32      // Longitud máxima de cada palabra
#define MAX_INTENTOS 6      // Intentos máximos para cada partida

// Intervalo (en segundos) para refrescar la información en pantalla
#define INTERVALO_REFRESCO 10

// ==================== Variables globales ======================
int clientes_activos         = 0;
int total_partidas_jugadas   = 0;
int total_partidas_ganadas   = 0;
int total_partidas_perdidas  = 0;
pthread_mutex_t mutex_contador = PTHREAD_MUTEX_INITIALIZER;

// Para asignar un ID único a cada conexión
int siguiente_id = 0;

// Guardamos los pthread_t de hilos activos para monitoreo
pthread_t lista_hilos[MAX_CLIENTES];
pthread_t hilo_refresco;  // Guardamos el ID del hilo de refresco

// Array para guardar los sockets de clientes activos
int sockets_clientes[MAX_CLIENTES];
pthread_mutex_t mutex_sockets = PTHREAD_MUTEX_INITIALIZER;

// Socket del servidor (global para poder cerrarlo en el manejador de señales)
int server_socket_fd = -1;

// Flag para indicarle al servidor que debe cerrar
volatile sig_atomic_t shutdown_server = 0;

// ===================== Manejador SIGINT =======================
void handle_sigint(int sig) {
    printf("\nRecibido SIGINT (Ctrl+C). Iniciando cierre del servidor...\n");
    fflush(stdout);
    shutdown_server = 1;
    
    // Notificar a todos los clientes activos
    pthread_mutex_lock(&mutex_sockets);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (sockets_clientes[i] > 0) {
            send(sockets_clientes[i], "ERROR:Server shutting down\n", 26, 0);
            close(sockets_clientes[i]);  // Cerrar socket del cliente
            sockets_clientes[i] = 0;
        }
    }
    pthread_mutex_unlock(&mutex_sockets);

    // Cerrar el socket del servidor para interrumpir accept()
    if (server_socket_fd != -1) {
        close(server_socket_fd);
    }

    // Cancelar el hilo de refresco
    pthread_cancel(hilo_refresco);
}

// ======================= Palabras Ahorcado =====================
const char *lista_palabras[] = {
    "programacion",
    "linux",
    "sistemas",
    "socket",
    "concurrency",
    "memoria",
    "proceso",
    "thread"
};
const int num_palabras = sizeof(lista_palabras) / sizeof(lista_palabras[0]);

// Devuelve un índice aleatorio entre 0 y num_palabras-1
int palabra_aleatoria() {
    return rand() % num_palabras;
}

// ========== Función para enviar estado al cliente (UN SOLO send()) ==========
void enviar_estado(int client_fd,
                   const char *estado_palabra,
                   int intentos_restantes,
                   const char *letras_usadas,
                   const char *mensaje_extra)
{
    char buffer[512] = {0};

    // Construir todo en un solo buffer:
    // Primera línea: STATE:palabra|intentos|letras
    int n = snprintf(buffer, sizeof(buffer),
                     "STATE:%s|%d|%s\n",
                     estado_palabra, intentos_restantes, letras_usadas);

    // Segunda línea: mensaje extra (WIN/LOSE o "¡Acierto!" / "Letra incorrecta")
    if (mensaje_extra && strlen(mensaje_extra) > 0) {
        snprintf(buffer + n, sizeof(buffer) - n, "%s\n", mensaje_extra);
    }

    // Enviar TODO de golpe
    send(client_fd, buffer, strlen(buffer), 0);
}

// ========== Función de refresco periódico (thread aparte) ==========
void *refrescar_estado(void *arg) {
    // Configurar el hilo para que pueda ser cancelado inmediatamente
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (!shutdown_server) {
        sleep(INTERVALO_REFRESCO);
        
        if (shutdown_server) break;  // Verificar de nuevo después del sleep

        pthread_mutex_lock(&mutex_contador);
        int act = clientes_activos;
        int jugadas = total_partidas_jugadas;
        int ganadas = total_partidas_ganadas;
        int perdidas = total_partidas_perdidas;
        pthread_mutex_unlock(&mutex_contador);

        double porcentaje = 0.0;
        if (jugadas > 0) {
            porcentaje = (double)ganadas / (double)jugadas * 100.0;
        }

        printf("\n[REFRESCO] Clientes activos: %d\n", act);
        printf("[REFRESCO] Estadísticas globales:\n");
        printf("           Partidas jugadas:  %d\n", jugadas);
        printf("           Partidas ganadas:  %d\n", ganadas);
        printf("           Partidas perdidas: %d\n", perdidas);
        printf("           %% Ganadas:        %.2f%%\n", porcentaje);
        if (act > 0) {
            printf("[REFRESCO] Hilos de atención activos (pthread_t):\n");
            for (int i = 0; i < act; i++) {
                printf("             - %lu\n", (unsigned long)lista_hilos[i]);
            }
        }
        printf("[REFRESCO] =========================================\n");
    }
    
    printf("[Refresco] Hilo de refresco finalizado.\n");
    pthread_exit(NULL);
    return NULL;
}

// ================ Rutina de cada hilo de cliente =================
typedef struct {
    int socket_cliente;
    int id_cliente;
} thread_args_t;

void *atender_cliente(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    int client_fd = args->socket_cliente;
    int id = args->id_cliente;
    free(arg);

    // Registrar socket del cliente
    pthread_mutex_lock(&mutex_sockets);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (sockets_clientes[i] == 0) {
            sockets_clientes[i] = client_fd;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_sockets);

    printf("[Thread %d] Cliente conectado. Clientes activos: %d\n",
           id, clientes_activos);

    char buffer_recv[128];
    int bytes;

    while (!shutdown_server) {
        // Si el servidor está cerrando, salimos del bucle
        if (shutdown_server) {
            break;
        }

        // ================ Iniciar una partida =================
        int idx = palabra_aleatoria();
        char palabra_real[MAX_PALABRA];
        strncpy(palabra_real, lista_palabras[idx], MAX_PALABRA);
        palabra_real[MAX_PALABRA - 1] = '\0';
        int len = strlen(palabra_real);

        // Llenar estado con guiones bajos
        char estado[MAX_PALABRA];
        for (int i = 0; i < len; i++) {
            estado[i] = '_';
        }
        estado[len] = '\0';

        int intentos_restantes = MAX_INTENTOS;
        char letras_usadas[64] = "";  
        // letras_usadas irá acumulando cada letra que el cliente pruebe

        // Contador global de partidas
        pthread_mutex_lock(&mutex_contador);
        total_partidas_jugadas++;
        pthread_mutex_unlock(&mutex_contador);

        // Enviar estado inicial (un solo send)
        enviar_estado(client_fd, estado, intentos_restantes, letras_usadas, "");

        // ================ Bucle de la partida =================
        int fin_partida = 0;  // 0=sigue, 1=ganó, 2=perdió, 3=se desconectó/quit
        while (!fin_partida && !shutdown_server) {
            memset(buffer_recv, 0, sizeof(buffer_recv));
            bytes = recv(client_fd, buffer_recv, sizeof(buffer_recv) - 1, 0);
            if (bytes <= 0) {
                // Cliente se desconectó inesperadamente durante la partida
                printf("[Thread %d] Cliente se desconectó o error recv(). Cuenta como pérdida.\n", id);
                pthread_mutex_lock(&mutex_contador);
                total_partidas_perdidas++;
                pthread_mutex_unlock(&mutex_contador);
                fin_partida = 3;
                break;
            }
            buffer_recv[bytes] = '\0';

            // Eliminar '\n'
            size_t rlen = strlen(buffer_recv);
            if (rlen > 0 && buffer_recv[rlen - 1] == '\n') {
                buffer_recv[rlen - 1] = '\0';
            }

            if (strcmp(buffer_recv, "QUIT") == 0) {
                // Cliente envió QUIT durante la partida: cuenta como pérdida
                send(client_fd, "BYE\n", 4, 0);
                printf("[Thread %d] Cliente solicitó QUIT. Cuenta como pérdida y termina hilo.\n", id);
                pthread_mutex_lock(&mutex_contador);
                total_partidas_perdidas++;
                pthread_mutex_unlock(&mutex_contador);
                fin_partida = 3;
                break;
            }

            if (strncmp(buffer_recv, "TRY:", 4) == 0 && strlen(buffer_recv) == 5) {
                char letra = buffer_recv[4];
                int acierto = 0;

                // Verificar si la letra ya fue usada
                if (strchr(letras_usadas, letra) != NULL) {
                    send(client_fd, "ERROR:Letra ya usada\n", 20, 0);
                    continue;
                }

                // Verificar si quedan intentos
                if (intentos_restantes <= 0) {
                    send(client_fd, "ERROR:No quedan intentos\n", 24, 0);
                    continue;
                }

                // *** 1) Añadir la letra a 'letras_usadas' si no estaba ya ***
                int l = strlen(letras_usadas);
                if (l < (int)sizeof(letras_usadas) - 2) {
                    letras_usadas[l] = letra;
                    letras_usadas[l + 1] = '\0';
                }

                // 2) Actualizar todas las ocurrencias en 'estado'
                for (int i = 0; i < len; i++) {
                    if (palabra_real[i] == letra && estado[i] == '_') {
                        estado[i] = letra;
                        acierto = 1;
                    }
                }
                if (!acierto) {
                    intentos_restantes--;
                }

                // Verificar victoria
                if (strcmp(estado, palabra_real) == 0) {
                    pthread_mutex_lock(&mutex_contador);
                    total_partidas_ganadas++;
                    pthread_mutex_unlock(&mutex_contador);

                    // Enviar estado + WIN
                    enviar_estado(client_fd, estado, intentos_restantes, letras_usadas, "WIN");
                    fin_partida = 1;
                    break;
                }
                // Verificar derrota
                if (intentos_restantes <= 0) {
                    pthread_mutex_lock(&mutex_contador);
                    total_partidas_perdidas++;
                    pthread_mutex_unlock(&mutex_contador);

                    char msg_lose[64];
                    snprintf(msg_lose, sizeof(msg_lose), "LOSE|La palabra era:%s", palabra_real);
                    enviar_estado(client_fd, estado, intentos_restantes, letras_usadas, msg_lose);
                    fin_partida = 2;
                    break;
                }

                // Sigue jugando: enviar estado actualizado
                const char *msg_extra = acierto ? "¡Acierto!" : "Letra incorrecta";
                enviar_estado(client_fd, estado, intentos_restantes, letras_usadas, msg_extra);

            } else {
                send(client_fd, "ERROR: Comando inválido\n", 23, 0);
            }
        }

        if (fin_partida == 3 || shutdown_server) {
            // Cliente se desconectó o pidió QUIT, ya contabilizado como pérdida
            break;
        }

        // ================ Partida finalizada (ganó o perdió) ============
        // Enviar GAMEOVER
        if (fin_partida == 1) {
            send(client_fd, "GAMEOVER:WIN\n", 13, 0);
        } else if (fin_partida == 2) {
            char buffer_go[64];
            snprintf(buffer_go, sizeof(buffer_go), "GAMEOVER:LOSE:%s\n", palabra_real);
            send(client_fd, buffer_go, strlen(buffer_go), 0);
        }

        // Esperar respuesta del cliente: PLAY o QUIT
        memset(buffer_recv, 0, sizeof(buffer_recv));
        bytes = recv(client_fd, buffer_recv, sizeof(buffer_recv) - 1, 0);
        if (bytes <= 0) {
            // Cliente se desconectó justo tras GAMEOVER: cuenta como pérdida
            printf("[Thread %d] Cliente se desconectó tras GAMEOVER. Cuenta como pérdida.\n", id);
            pthread_mutex_lock(&mutex_contador);
            total_partidas_perdidas++;
            pthread_mutex_unlock(&mutex_contador);
            break;
        }
        buffer_recv[bytes] = '\0';

        // Eliminar '\n' y espacios
        size_t glen = strlen(buffer_recv);
        while (glen > 0 && (buffer_recv[glen - 1] == '\n' || buffer_recv[glen - 1] == '\r' || buffer_recv[glen - 1] == ' ')) {
            buffer_recv[glen - 1] = '\0';
            glen--;
        }

        if (strcmp(buffer_recv, "PLAY") == 0) {
            // Jugar otra: volvemos al principio del while principal
            printf("[Thread %d] Cliente eligió PLAY para nueva partida.\n", id);
            continue;
        } else if (strcmp(buffer_recv, "QUIT") == 0) {
            send(client_fd, "BYE\n", 4, 0);
            printf("[Thread %d] Cliente eligió QUIT tras GAMEOVER. Cuenta como pérdida y cierra hilo.\n", id);
            pthread_mutex_lock(&mutex_contador);
            total_partidas_perdidas++;
            pthread_mutex_unlock(&mutex_contador);
            break;
        } else {
            // Cualquier otra cosa, cerrar igual
            send(client_fd, "BYE\n", 4, 0);
            printf("[Thread %d] Respuesta inesperada tras GAMEOVER ('%s'). Cierra hilo.\n", id, buffer_recv);
            pthread_mutex_lock(&mutex_contador);
            total_partidas_perdidas++;
            pthread_mutex_unlock(&mutex_contador);
            break;
        }
    }

    // ================ Cerrar conexión y terminar hilo ================
    // Eliminar socket del cliente
    pthread_mutex_lock(&mutex_sockets);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (sockets_clientes[i] == client_fd) {
            sockets_clientes[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_sockets);

    close(client_fd);

    pthread_mutex_lock(&mutex_contador);
    clientes_activos--;
    int rem = clientes_activos;
    pthread_mutex_unlock(&mutex_contador);

    printf("[Thread %d] Thread finalizado. Quedan %d clientes activos.\n", id, rem);
    pthread_exit(NULL);
    return NULL;
}

// ========================== main() ============================
int main() {
    int new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    // Inicializar array de sockets
    memset(sockets_clientes, 0, sizeof(sockets_clientes));

    // Configurar manejadores de señales
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error configurando manejador SIGINT");
        exit(EXIT_FAILURE);
    }

    // También manejar SIGTERM
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Error configurando manejador SIGTERM");
        exit(EXIT_FAILURE);
    }

    srand(time(NULL));

    printf("===== INICIO DEL SERVIDOR DE AHORCADO =====\n");

    // ---------- 1) Crear socket ----------
    if ((server_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    printf("Socket creado.\n");

    // ---------- 2) Configurar SO_REUSEADDR ----------
    int opt = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }
    printf("SO_REUSEADDR configurado.\n");

    // ---------- 3) Configurar dirección ----------
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Aceptar conexiones en todas las interfaces
    address.sin_port = htons(PUERTO);

    // ---------- 4) bind() ----------
    if (bind(server_socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }
    printf("Bind exitoso en puerto %d.\n", PUERTO);

    // ---------- 5) listen() ----------
    if (listen(server_socket_fd, 10) < 0) {
        perror("listen");
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }
    printf("Servidor escuchando (listen) en puerto %d.\n", PUERTO);
    printf("Máximo de clientes concurrentes: %d\n\n", MAX_CLIENTES);

    // ---------- 6) Lanzar thread de refresco periódico ----------
    if (pthread_create(&hilo_refresco, NULL, refrescar_estado, NULL) != 0) {
        perror("pthread_create hilo_refresco");
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }

    // ---------- 7) Bucle principal de aceptación ----------
    while (!shutdown_server) {
        // Usar select() para esperar conexiones con timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket_fd, &readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 0.5 segundos

        int sel = select(server_socket_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue; // Interrumpido por señal
            perror("select");
            break;
        }

        // Antes de aceptar, revisar si hay que cerrar por falta de clientes
        pthread_mutex_lock(&mutex_contador);
        if (clientes_activos == 0 && siguiente_id > 0) {
            pthread_mutex_unlock(&mutex_contador);
            printf("\n[Main] No quedan clientes activos. Cerrando servidor automáticamente.\n");
            shutdown_server = 1;
            break;
        }
        pthread_mutex_unlock(&mutex_contador);

        if (sel == 0) {
            // Timeout, volver a chequear
            continue;
        }

        if (!FD_ISSET(server_socket_fd, &readfds)) {
            continue;
        }

        int new_socket = accept(server_socket_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) {
            if (shutdown_server) break;
            if (errno == EINTR) continue;  // Interrumpido por señal
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&mutex_contador);
        if (shutdown_server) {
            pthread_mutex_unlock(&mutex_contador);
            close(new_socket);
            break;
        }

        // Si llegamos al máximo, esperamos en bucle hasta que haya hueco
        while (clientes_activos >= MAX_CLIENTES && !shutdown_server) {
            pthread_mutex_unlock(&mutex_contador);
            sleep(1);
            pthread_mutex_lock(&mutex_contador);
        }

        if (shutdown_server) {
            pthread_mutex_unlock(&mutex_contador);
            close(new_socket);
            break;
        }

        // Aceptar a este cliente
        clientes_activos++;
        int id_actual = ++siguiente_id;
        printf("[Main] Aceptada conexión #%d. Clientes activos: %d\n", id_actual, clientes_activos);
        pthread_mutex_unlock(&mutex_contador);

        // Crear thread para atender a este cliente
        pthread_t tid;
        thread_args_t *args = malloc(sizeof(thread_args_t));
        args->socket_cliente = new_socket;
        args->id_cliente = id_actual;
        if (pthread_create(&tid, NULL, atender_cliente, args) != 0) {
            perror("pthread_create atender_cliente");
            close(new_socket);
            pthread_mutex_lock(&mutex_contador);
            clientes_activos--;
            pthread_mutex_unlock(&mutex_contador);
            free(args);
            continue;
        }
        // Guardar pthread_t en arreglo para refresco
        pthread_mutex_lock(&mutex_contador);
        int idx_hilo = clientes_activos - 1;
        lista_hilos[idx_hilo] = tid;
        pthread_mutex_unlock(&mutex_contador);

        pthread_detach(tid);
    }

    // ---------- 8) Cierre limpio ----------
    printf("\n[Main] Cierre limpio iniciado. Esperando que finalicen los clientes...\n");
    // Esperar a que todos los clientes (hilos) finalicen
    while (1) {
        pthread_mutex_lock(&mutex_contador);
        int rem = clientes_activos;
        pthread_mutex_unlock(&mutex_contador);
        if (rem == 0) break;
        printf("[Main] Esperando que finalicen %d clientes...\n", rem);
        sleep(1);
    }

    // Esperar a que el hilo de refresco termine
    pthread_join(hilo_refresco, NULL);

    printf("[Main] Todos los hilos han finalizado. Servidor cerrado.\n");
    return 0;
}
