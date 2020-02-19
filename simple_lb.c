#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>

#include <sys/event.h>

#include <regex.h>

#define MAXIMUM_CONNECTIONS 4096
#define DATA_BUFFER 1024 * 1024
#define MAXIMUM_NUMBER_OF_BACKENDS 16

struct socket_pair {
	int client, server;
	bool client_connected, server_connected;
	int refs;
};

struct backend_server {
	struct in_addr addr;
	in_port_t port;
	uint32_t connections;
};

struct backends {
	int number_of_backends;
	struct backend_server servers[MAXIMUM_NUMBER_OF_BACKENDS];
};

struct listener {
	in_port_t port;
	struct in_addr addr;
};

struct backends backends_list;
struct listener listener;

bool has_listener_port_opt = false;
bool has_listener_addr_opt = false;

/* Chooses a backend server based on the load balancing policy (just round-robin
 * for now) connects to it and return a socket or an error.
 */
int
connect_to_a_backend()
{

	struct sockaddr_in addr;
	int backend_sock = 0;
	int ret = 0;

	static int backend_index = 0;

	backend_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (backend_sock == -1)
		return -1;

	addr.sin_family = AF_INET;
	addr.sin_port = backends_list.servers[backend_index].port;
	addr.sin_addr.s_addr = backends_list.servers[backend_index].addr.s_addr;
	addr.sin_len = sizeof(addr);
	memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

	ret = connect(backend_sock, (struct sockaddr *)&addr, sizeof(addr));

	if (ret == -1)
		return -1;

	backend_index = (backend_index + 1) % backends_list.number_of_backends;

	return backend_sock;
}

/* Allocates a socket_pair structure, adds client_sock and backend_sock to it
 * and adds both sockets and a pointer to the structure to the kqueue.
 */
int
add_socket_pair_to_kqueue(int kq, int client_sock, int backend_sock)
{

	struct socket_pair *pair = NULL;
	struct kevent event[2];
	int ret = 0;

	pair = malloc(sizeof(struct socket_pair));

	if (pair == NULL) {
		printf("Failed to allocate memory for the socket pair\n");
		return -1;
	}

	pair->client = client_sock;
	pair->server = backend_sock;
	pair->client_connected = pair->server_connected = true;
	pair->refs = 2;

	EV_SET(&event[0], client_sock, EVFILT_READ, EV_ADD, 0, 0, (void *)pair);
	EV_SET(
	    &event[1], backend_sock, EVFILT_READ, EV_ADD, 0, 0, (void *)pair);

	ret = kevent(kq, event, 2, NULL, 0, NULL);

	if (ret == -1) {
		printf("Failed to register client_sock/backend_sock event\n");
		free(pair);
		return -1;
	}

	return 0;
}

int
del_socket_from_kqueue(int kq, int sock, struct socket_pair *pair)
{
	struct kevent event;
	int ret = 0;

	EV_SET(&event, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);

	ret = kevent(kq, &event, 1, NULL, 0, NULL);

	if (ret == -1) {
		printf("Failed to deregister client_sock/backend_sock event\n");
	}

	if (sock == pair->client)
		pair->client_connected = false;
	else
		pair->server_connected = false;

	close(sock);

	pair->refs--;

	if (pair->refs <= 0) {
		free(pair);
	}

	return 0;
}

int
add_new_backend(char *backend)
{
	char *backend_re_pattern =
	    "^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}:[0-9]{1,5}$";
	regex_t reg;
	char err_buf[100];
	int ret;
	char *split;
	int port;

	if (backends_list.number_of_backends >= 15) {
		printf("Maximum number of backends (%d) exeeded\n",
		    MAXIMUM_NUMBER_OF_BACKENDS);
		return 1;
	}

	ret = regcomp(&reg, backend_re_pattern, REG_EXTENDED);

	if (ret != 0) {
		regerror(ret, &reg, err_buf, 100);
		printf("Regex compilation failed: %s\n", err_buf);
		exit(1);
	}

	ret = regexec(&reg, backend, 0, NULL, 0);

	regfree(&reg);

	if (ret != 0) {
		printf("%s doesn\'t match the format xxx.xxx.xxx.xxx:port\n",
		    backend);
		exit(1);
	}

	split = strsep(&backend, ":");

	port = atoi(backend);

	if (port <= 0 || port >= 65536) {
		printf("Port number must be 1-65535\n");
		exit(1);
	}

	backends_list.servers[backends_list.number_of_backends].addr.s_addr =
	    inet_addr(split);
	backends_list.servers[backends_list.number_of_backends].port =
	    htons(port);
	backends_list.number_of_backends++;

	return 0;
}

void
configure_listener_addr(const char *addr)
{
	listener.addr.s_addr = inet_addr(addr);

	if (listener.addr.s_addr == INADDR_NONE) {
		printf("Listener address is not valid\n");
		exit(1);
	}
}

void
configure_listener_port(const char *port)
{
	int p = atoi(port);

	if (p <= 0 || p > 65535) {
		printf("Listener port is invalid\n");
		exit(1);
	}

	listener.port = htons(p);
}

void
print_help(char **argv)
{
	printf("Usage: %s [option] -b backend:port\n", argv[0]);
	printf("Options\n");
	printf("-h This message\n");
	printf("-l Listen address (default 127.0.0.1)\n");
	printf("-p Listen port (default 8000)\n");
	printf(
	    "-b Backend address. Format IP:PORT. Example -b 127.0.0.1:80. Can be repeated\n");
}

void
parse_args(int argc, char **argv)
{

	int ret;
	int ch;
	bool has_at_least_one_backend = false;

	while ((ch = getopt(argc, argv, "hl:p:b:")) != -1) {
		switch (ch) {
		case 'h':
			print_help(argv);
			break;
		case 'l':
			has_listener_addr_opt = true;
			configure_listener_addr(optarg);
			break;
		case 'p':
			has_listener_port_opt = true;
			configure_listener_port(optarg);
			break;
		case 'b':
			ret = add_new_backend(optarg);
			if (ret == 0)
				has_at_least_one_backend = true;
			break;
		}
	}

	if (has_at_least_one_backend == false) {
		printf("You need to defined at least one valid backend\n");
		exit(1);
	}
}

int
main(int argc, char **argv)
{

	struct sockaddr_in server_sockaddr, client_sockaddr;
	struct kevent server_kevent, *clients_kevent;
	char *data_buffer = NULL;
	int kq = 0;
	int server_sock = 0;
	int ret = 0;
	int enabled = 1;

	backends_list.number_of_backends = 0;

	parse_args(argc, argv);

	clients_kevent = malloc(sizeof(struct kevent) * MAXIMUM_CONNECTIONS);

	signal(SIGPIPE, SIG_IGN);

	if (clients_kevent == NULL) {
		printf("Failed to allocate clients_kevents\n");
		exit(1);
	}

	memset(clients_kevent, 0, sizeof(struct kevent) * MAXIMUM_CONNECTIONS);

	server_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (server_sock == -1) {
		printf("Failed to create the server socket\n");
		exit(1);
	}

	if (has_listener_port_opt)
		server_sockaddr.sin_port = listener.port;
	else
		server_sockaddr.sin_port = htons(8000);

	if (has_listener_addr_opt)
		server_sockaddr.sin_addr = listener.addr;
	else
		server_sockaddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	server_sockaddr.sin_family = AF_INET;
	server_sockaddr.sin_len = sizeof(server_sockaddr);
	memset(server_sockaddr.sin_zero, 0, sizeof(server_sockaddr.sin_zero));

	ret = setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT,
	    (void *)&enabled, sizeof(enabled));

	if (ret == -1) {
		printf("setsockopt has failed: %s\n", strerror(errno));
	}

	ret = bind(server_sock, (struct sockaddr *)&server_sockaddr,
	    sizeof(server_sockaddr));

	if (ret == -1) {
		printf("Server socket bind failed: %s\n", strerror(errno));
		close(server_sock);
		exit(1);
	}

	ret = listen(server_sock, MAXIMUM_CONNECTIONS);

	if (ret == -1) {
		printf("Server socket listen failed\n");
		close(server_sock);
		exit(1);
	}

	kq = kqueue();
	EV_SET(&server_kevent, server_sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	ret = kevent(kq, &server_kevent, 1, NULL, 0, NULL);

	if (ret == -1) {
		printf("Server event failed to be regitered\n");
		close(server_sock);
		exit(1);
	}

	if (server_kevent.flags & EV_ERROR) {
		printf(
		    "Server kevent error: %s\n", strerror(server_kevent.data));
		close(server_sock);
		exit(1);
	}

	socklen_t client_sockaddr_len = sizeof(client_sockaddr);
	data_buffer = malloc(DATA_BUFFER);

	while (1) {

		int i;
		int kevent_ret;

		kevent_ret = kevent(
		    kq, NULL, 0, clients_kevent, MAXIMUM_CONNECTIONS, NULL);

		if (kevent_ret == -1) {
			printf("Client kevent failed\n");
		}

		for (i = 0; i < kevent_ret; i++) {

			if ((int)clients_kevent[i].ident == server_sock) {

				/* The event is a new connection */

				memset(&client_sockaddr, 0,
				    sizeof(client_sockaddr));
				int client_sock =
				    accept(clients_kevent[i].ident,
					(struct sockaddr *)&client_sockaddr,
					&client_sockaddr_len);

				if (client_sock == -1) {
					printf("Accept failed\n");
				} else {

					int backend_sock =
					    connect_to_a_backend();

					if (backend_sock == -1) {
						printf(
						    "Failed to connect to the backend\n");
						shutdown(
						    client_sock, SHUT_RDWR);
					} else {

						ret = add_socket_pair_to_kqueue(
						    kq, client_sock,
						    backend_sock);

						if (ret == -1) {
							printf(
							    "Failed to add pair to kqueue\n");
						}
					}
				}
			} else {
				/* The event is not a new connection */

				struct socket_pair *pair =
				    (struct socket_pair *)clients_kevent[i]
					.udata;
				int event_sock = (int)clients_kevent[i].ident;
				int client = pair->client;
				int server = pair->server;
				int client_connected = pair->client_connected;
				int server_connected = pair->server_connected;

				/* There are data to be read from the socket */
				if (clients_kevent[i].filter & EVFILT_READ) {
					int data_size = clients_kevent[i].data;

					if (data_size > 0) {

						ret = read(event_sock,
						    data_buffer,
						    MIN(data_size,
							DATA_BUFFER - 1));

						if (ret <= 0) {
							printf(
							    "Error while reading from event_sock\n");
						} else {

							if (event_sock ==
							    client)
								ret = write(
								    server,
								    data_buffer,
								    ret);
							else
								ret = write(
								    client,
								    data_buffer,
								    ret);

							if (ret <= 0)
								printf(
								    "Error while writing to the event_sock\n");
						}
					}
				}

				/* Handling a disconnection */
				if (clients_kevent[i].flags & EV_EOF) {
					del_socket_from_kqueue(
					    kq, event_sock, pair);
					if (event_sock == client &&
					    server_connected) {
						shutdown(server, SHUT_RDWR);
					} else if (event_sock == server &&
					    client_connected) {
						shutdown(client, SHUT_RDWR);
					}
				}
			}
		}
	}

	return 0;
}
