#include "chat.h"
#include "chat_client.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <errno.h>

struct buffer
{
	struct buffer *next;
	ssize_t size;
	ssize_t offset;
	char data[];
};

struct chat_client
{
	int socket;
	struct buffer *out_head;
	struct buffer *out_tail;
};

struct chat_client *chat_client_new(const char *name)
{
	(void)name;

	struct chat_client *client = malloc(sizeof(*client));
	client->socket = -1;
	return client;
}

void chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);

	free(client);
}

int chat_client_connect(struct chat_client *client, const char *addr)
{
	if (client->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	const char *colon = strchr(addr, ':');
	if (!colon)
		return CHAT_ERR_NO_ADDR;

	size_t host_length = colon - addr;
	char host_buffer[256];
	if (host_length >= sizeof(host_buffer))
		return CHAT_ERR_NO_ADDR;

	memcpy(host_buffer, addr, host_length);
	host_buffer[host_length] = '\0';
	const char *port_string = colon + 1;

	struct addrinfo hints = {0};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *address_result;

	if (getaddrinfo(host_buffer, port_string, &hints, &address_result) != 0)
		return CHAT_ERR_NO_ADDR;

	int client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (client_socket < 0)
	{
		freeaddrinfo(address_result);
		return CHAT_ERR_SYS;
	}

	// Переводим сокет в неблокирующий режим ДО connect()
	int flags = fcntl(client_socket, F_GETFL, 0);
	fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);

	if (connect(client_socket, address_result->ai_addr, address_result->ai_addrlen) != 0)
	{
		if (errno != EINPROGRESS)
		{
			close(client_socket);
			freeaddrinfo(address_result);
			return CHAT_ERR_SYS;
		}
	}

	freeaddrinfo(address_result);

	client->socket = client_socket;
	return 0;
}

struct chat_message *chat_client_pop_next(struct chat_client *client)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)client;
	return NULL;
}

int chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	short events = POLLIN;
	if (client->out_head != NULL)
		events |= POLLOUT;

	struct pollfd poll_fd;
	poll_fd.fd = client->socket;
	poll_fd.events = events;

	int poll_result = poll(&poll_fd, 1, (int)(timeout * 1000));
	if (poll_result == 0)
		return CHAT_ERR_TIMEOUT;
	if (poll_result < 0)
		return CHAT_ERR_SYS;

	if (poll_fd.revents & POLLOUT)
	{
		while (client->out_head != NULL)
		{
			struct buffer *current = client->out_head;
			ssize_t written = write(client->socket, current->data + current->offset, current->size - current->offset);

			if (written < 0)
			{
				if (errno == EWOULDBLOCK || errno == EAGAIN)
					break;
				return CHAT_ERR_SYS;
			}

			current->offset += written;
			if (current->offset < current->size)
				break;

			client->out_head = current->next;
			if (client->out_tail == current)
				client->out_tail = NULL;

			free(current);
		}
	}

	return 0;
}

int chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int chat_client_get_events(const struct chat_client *client)
{
	if (client->socket < 0)
		return 0;

	return CHAT_EVENT_INPUT;
}

int chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	struct buffer *new_buffer = malloc(sizeof(struct buffer) + msg_size);
	new_buffer->next = NULL;
	new_buffer->size = msg_size;
	new_buffer->offset = 0;
	memcpy(new_buffer->data, msg, msg_size);

	if (client->out_tail != NULL)
		client->out_tail->next = new_buffer;
	else
		client->out_head = new_buffer;

	client->out_tail = new_buffer;
	return 0;
}