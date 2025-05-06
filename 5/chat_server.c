#include "chat.h"
#include "chat_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <netinet/in.h>

struct server_peer
{
	int socket;
	char *input_buf;
	ssize_t size;
	ssize_t capacity;
	struct server_peer *next;
};

struct chat_peer
{
	/** Client's socket. To read/write messages. */
	int socket;
	/** Output buffer. */
	/* ... */
	/* PUT HERE OTHER MEMBERS */
};

struct chat_server
{
	int socket;
	struct server_peer *peers;
	struct chat_message *messages;
	struct chat_message *last;
};

struct chat_server *chat_server_new(void)
{
	struct chat_server *server = malloc(sizeof(*server));
	server->socket = -1;
	server->peers = NULL;
	server->messages = NULL;
	server->last = NULL;
	return server;
}

void chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);

	struct server_peer *peer = server->peers;
	while (peer)
	{
		close(peer->socket);
		free(peer->input_buf);
		struct server_peer *next = peer->next;
		free(peer);
		peer = next;
	}

	while (server->messages)
	{
		struct chat_message *m = server->messages;
		server->messages = m->next;
		chat_message_delete(m);
	}

	free(server);
}

int chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return CHAT_ERR_SYS;

	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		close(sock);
		return CHAT_ERR_PORT_BUSY;
	}

	if (listen(sock, 16) != 0)
	{
		close(sock);
		return CHAT_ERR_SYS;
	}

	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	server->socket = sock;
	return 0;
}

struct chat_message *chat_server_pop_next(struct chat_server *server)
{
	if (!server->messages)
		return NULL;

	struct chat_message *msg = server->messages;
	server->messages = msg->next;
	if (!server->messages)
		server->last = NULL;
	msg->next = NULL;
	return msg;
}

static void trim(char *str, ssize_t *len)
{
	while (*len > 0 && isspace((unsigned char)str[0]))
	{
		str++;
		(*len)--;
	}
	while (*len > 0 && isspace((unsigned char)str[*len - 1]))
	{
		(*len)--;
	}
	memmove(str - (str - str), str, *len);
	str[*len] = '\0';
}

int chat_server_update(struct chat_server *server, double timeout)
{
	if (server->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	int count = 1;
	struct server_peer *p = server->peers;
	while (p)
	{
		count++;
		p = p->next;
	}

	struct pollfd *fds = malloc(sizeof(*fds) * count);
	fds[0].fd = server->socket;
	fds[0].events = POLLIN;

	int i = 1;
	p = server->peers;
	while (p)
	{
		fds[i].fd = p->socket;
		fds[i].events = POLLIN;
		i++;
		p = p->next;
	}

	int rc = poll(fds, count, (int)(timeout * 1000));
	if (rc == 0)
	{
		free(fds);
		return CHAT_ERR_TIMEOUT;
	}
	if (rc < 0)
	{
		free(fds);
		return CHAT_ERR_SYS;
	}

	if (fds[0].revents & POLLIN)
	{
		while (1)
		{
			int cfd = accept(server->socket, NULL, NULL);
			if (cfd < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				free(fds);
				return CHAT_ERR_SYS;
			}
			int flags = fcntl(cfd, F_GETFL, 0);
			fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

			struct server_peer *new_peer = malloc(sizeof(*new_peer));
			new_peer->socket = cfd;
			new_peer->input_buf = NULL;
			new_peer->size = 0;
			new_peer->capacity = 0;
			new_peer->next = server->peers;
			server->peers = new_peer;
		}
	}

	i = 1;
	struct server_peer **indirect = &server->peers;
	while (*indirect)
	{
		struct server_peer *peer = *indirect;
		if (fds[i].revents & POLLIN)
		{
			char buffer[512];
			ssize_t r = read(peer->socket, buffer, sizeof(buffer));
			if (r == 0)
			{
				close(peer->socket);
				free(peer->input_buf);
				*indirect = peer->next;
				free(peer);
				i++;
				continue;
			}
			if (r > 0)
			{
				if (peer->size + r >= peer->capacity)
				{
					peer->capacity = (peer->size + r) * 2;
					peer->input_buf = realloc(peer->input_buf, peer->capacity);
				}
				memcpy(peer->input_buf + peer->size, buffer, r);
				peer->size += r;

				ssize_t start = 0;
				for (ssize_t j = 0; j < peer->size; j++)
				{
					if (peer->input_buf[j] == '\n')
					{
						ssize_t len = j - start;
						if (len > 0)
						{
							char *msg_data = malloc(len + 1);
							memcpy(msg_data, peer->input_buf + start, len);
							msg_data[len] = '\0';
							ssize_t l = len;
							trim(msg_data, &l);
							if (l > 0)
							{
								struct chat_message *msg = malloc(sizeof(*msg));
								msg->data = msg_data;
								msg->next = NULL;
								if (server->last)
									server->last->next = msg;
								else
									server->messages = msg;
								server->last = msg;
								continue;
							}
							free(msg_data);
						}
						start = j + 1;
					}
				}

				if (start > 0)
				{
					memmove(peer->input_buf, peer->input_buf + start, peer->size - start);
					peer->size -= start;
				}
			}
		}
		i++;
		indirect = &(*indirect)->next;
	}

	free(fds);
	return 0;
}

int chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#endif
	(void)server;
	return -1;
}

int chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int chat_server_get_events(const struct chat_server *server)
{
	if (server->socket < 0)
		return 0;

	return CHAT_EVENT_INPUT;
}

int chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}