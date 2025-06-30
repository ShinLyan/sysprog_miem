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
#include <sys/epoll.h>

struct buffer
{
	struct buffer *next;
	ssize_t size;
	ssize_t offset;
	char data[];
};

struct chat_peer
{
	int socket;

	struct buffer *output_buffer_head;
	struct buffer *output_buffer_tail;

	char *input_buffer;
	ssize_t input_buffer_size;
	ssize_t input_buffer_capacity;

	struct chat_peer *next;
};

struct chat_server
{
	int socket;
	int epoll_fd;
	struct chat_peer *peers;
	struct chat_message *message_head;
	struct chat_message *message_tail;
};

static int accept_new_clients(struct chat_server *server);
static int read_from_peer(struct chat_server *server, struct chat_peer *peer);
static void flush_peer_output(struct chat_peer *peer);
static void trim_message(char *text, ssize_t *length);
static void free_peer(struct chat_peer *peer);

struct chat_server *chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));
	server->socket = -1;
	server->epoll_fd = epoll_create1(0);
	if (server->epoll_fd < 0)
		abort();

	return server;
}

void chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);

	if (server->epoll_fd >= 0)
		close(server->epoll_fd);

	while (server->peers)
	{
		struct chat_peer *next = server->peers->next;
		free_peer(server->peers);
		server->peers = next;
	}

	while (server->message_head)
	{
		struct chat_message *message = server->message_head;
		server->message_head = message->next;
		chat_message_delete(message);
	}

	free(server);
}

int chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0)
		return CHAT_ERR_SYS;

	int enable = 1;
	setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) != 0)
	{
		close(server_socket);
		return CHAT_ERR_PORT_BUSY;
	}

	if (listen(server_socket, 16) != 0)
	{
		close(server_socket);
		return CHAT_ERR_SYS;
	}

	fcntl(server_socket, F_SETFL, fcntl(server_socket, F_GETFL, 0) | O_NONBLOCK);
	server->socket = server_socket;

	struct epoll_event event = {
		.events = EPOLLIN | EPOLLET,
		.data.ptr = server};

	epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server_socket, &event);
	return 0;
}

struct chat_message *chat_server_pop_next(struct chat_server *server)
{
	struct chat_message *message = server->message_head;
	if (!message)
		return NULL;

	server->message_head = message->next;

	if (!server->message_head)
		server->message_tail = NULL;

	message->next = NULL;
	return message;
}

int chat_server_update(struct chat_server *server, double timeout)
{
	if (server->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	struct epoll_event events[16];
	int ready_count = epoll_wait(server->epoll_fd, events, 16, (int)(timeout * 1000));
	if (ready_count == 0)
		return CHAT_ERR_TIMEOUT;

	if (ready_count < 0)
		return CHAT_ERR_SYS;

	for (int i = 0; i < ready_count; i++)
	{
		void *context = events[i].data.ptr;
		if (context == server)
		{
			int accept_result = accept_new_clients(server);
			if (accept_result != 0)
				return accept_result;
		}
		else
		{
			struct chat_peer *peer = context;
			int read_result = read_from_peer(server, peer);
			if (read_result != 0)
				return read_result;
		}
	}

	for (struct chat_peer *peer = server->peers; peer; peer = peer->next)
		flush_peer_output(peer);

	return 0;
}

static int accept_new_clients(struct chat_server *server)
{
	while (1)
	{
		int client_socket = accept(server->socket, NULL, NULL);
		if (client_socket < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return CHAT_ERR_SYS;
		}

		fcntl(client_socket, F_SETFL, fcntl(client_socket, F_GETFL, 0) | O_NONBLOCK);

		struct chat_peer *new_peer = calloc(1, sizeof(*new_peer));
		new_peer->socket = client_socket;
		new_peer->next = server->peers;
		server->peers = new_peer;

		struct epoll_event peer_event = {
			.events = EPOLLIN | EPOLLOUT | EPOLLET,
			.data.ptr = new_peer};
		epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_socket, &peer_event);
	}

	return 0;
}

static int read_from_peer(struct chat_server *server, struct chat_peer *peer)
{
	char temporary_read_buffer[512];
	while (1)
	{
		ssize_t bytes_read = read(peer->socket, temporary_read_buffer, sizeof(temporary_read_buffer));
		if (bytes_read <= 0)
			break;

		if (peer->input_buffer_size + bytes_read >= peer->input_buffer_capacity)
		{
			peer->input_buffer_capacity = (peer->input_buffer_size + bytes_read) * 2;
			peer->input_buffer = realloc(peer->input_buffer, peer->input_buffer_capacity);
		}

		memcpy(peer->input_buffer + peer->input_buffer_size, temporary_read_buffer, bytes_read);
		peer->input_buffer_size += bytes_read;
	}

	ssize_t message_start = 0;
	for (ssize_t i = 0; i < peer->input_buffer_size; i++)
	{
		if (peer->input_buffer[i] == '\n')
		{
			ssize_t message_length = i - message_start;
			if (message_length > 0)
			{
				char *raw_message = malloc(message_length + 1);
				memcpy(raw_message, peer->input_buffer + message_start, message_length);
				raw_message[message_length] = '\0';
				trim_message(raw_message, &message_length);

				if (message_length > 0)
				{
					struct chat_message *message = malloc(sizeof(*message));
					message->data = raw_message;
					message->next = NULL;
					if (server->message_tail)
						server->message_tail->next = message;
					else
						server->message_head = message;

					server->message_tail = message;

					size_t output_size = message_length + 1;
					char *broadcast_message = malloc(output_size);
					memcpy(broadcast_message, raw_message, message_length);
					broadcast_message[message_length] = '\n';

					for (struct chat_peer *other_peer = server->peers; other_peer; other_peer = other_peer->next)
					{
						if (other_peer == peer)
							continue;

						struct buffer *send_buffer = malloc(sizeof(*send_buffer) + output_size);
						memcpy(send_buffer->data, broadcast_message, output_size);
						send_buffer->offset = 0;
						send_buffer->size = output_size;
						send_buffer->next = NULL;

						if (other_peer->output_buffer_tail)
							other_peer->output_buffer_tail->next = send_buffer;
						else
							other_peer->output_buffer_head = send_buffer;

						other_peer->output_buffer_tail = send_buffer;
					}

					free(broadcast_message);
				}
				else
					free(raw_message);
			}

			message_start = i + 1;
		}
	}

	if (message_start > 0)
	{
		memmove(peer->input_buffer, peer->input_buffer + message_start, peer->input_buffer_size - message_start);
		peer->input_buffer_size -= message_start;
	}

	return 0;
}

static void flush_peer_output(struct chat_peer *peer)
{
	while (peer->output_buffer_head)
	{
		struct buffer *buffer = peer->output_buffer_head;
		ssize_t bytes_written = send(peer->socket, buffer->data + buffer->offset, buffer->size - buffer->offset, MSG_NOSIGNAL);

		if (bytes_written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;

		if (bytes_written < 0)
			return;

		buffer->offset += bytes_written;
		if (buffer->offset == buffer->size)
		{
			peer->output_buffer_head = buffer->next;
			if (peer->output_buffer_tail == buffer)
				peer->output_buffer_tail = NULL;
			free(buffer);
		}
		else
		{
			break;
		}
	}
}

static void trim_message(char *text, ssize_t *length)
{
	char *start = text;
	char *end = text + *length;
	while (start < end && isspace((unsigned char)*start))
		start++;
	while (end > start && isspace((unsigned char)*(end - 1)))
		end--;
	*length = end - start;
	memmove(text, start, *length);
	text[*length] = '\0';
}

static void free_peer(struct chat_peer *peer)
{
	close(peer->socket);
	while (peer->output_buffer_head)
	{
		struct buffer *next = peer->output_buffer_head->next;
		free(peer->output_buffer_head);
		peer->output_buffer_head = next;
	}
	free(peer->input_buffer);
	free(peer);
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
	return server->epoll_fd;
}

int chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int chat_server_get_events(const struct chat_server *server)
{
	if (server->socket < 0)
		return 0;

	for (struct chat_peer *peer = server->peers; peer; peer = peer->next)
		if (peer->output_buffer_head != NULL)
			return CHAT_EVENT_INPUT | CHAT_EVENT_OUTPUT;

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