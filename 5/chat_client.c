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
#include <ctype.h>

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
	struct buffer *output_head;
	struct buffer *output_tail;
	char *input_buffer;
	ssize_t input_size;
	ssize_t input_capacity;

	struct chat_message *message_head;
	struct chat_message *message_tail;
};

static void trim(char *text, ssize_t *length);
static void append_message(struct chat_client *client, char *text, ssize_t length);
static int send_pending_output(struct chat_client *client);
static void process_incoming_data(struct chat_client *client, const char *data, ssize_t length);

struct chat_client *chat_client_new(const char *name)
{
	(void)name;

	struct chat_client *client = calloc(1, sizeof(*client));
	client->socket = -1;
	return client;
}

void chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);

	free(client->input_buffer);

	while (client->message_head)
	{
		struct chat_message *next = client->message_head->next;
		chat_message_delete(client->message_head);
		client->message_head = next;
	}

	while (client->output_head)
	{
		struct buffer *next = client->output_head->next;
		free(client->output_head);
		client->output_head = next;
	}

	free(client);
}

int chat_client_connect(struct chat_client *client, const char *address)
{
	if (client->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	const char *colon = strchr(address, ':');
	if (!colon)
		return CHAT_ERR_NO_ADDR;

	size_t host_length = colon - address;
	if (host_length >= 256)
		return CHAT_ERR_NO_ADDR;

	char host[256];
	memcpy(host, address, host_length);
	host[host_length] = '\0';
	const char *port = colon + 1;

	struct addrinfo hints = {0}, *result;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &result) != 0)
		return CHAT_ERR_NO_ADDR;

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		freeaddrinfo(result);
		return CHAT_ERR_SYS;
	}

	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

	if (connect(sock, result->ai_addr, result->ai_addrlen) != 0 && errno != EINPROGRESS)
	{
		close(sock);
		freeaddrinfo(result);
		return CHAT_ERR_SYS;
	}

	freeaddrinfo(result);
	client->socket = sock;
	return 0;
}

struct chat_message *chat_client_pop_next(struct chat_client *client)
{
	if (!client->message_head)
		return NULL;

	struct chat_message *message = client->message_head;
	client->message_head = message->next;

	if (!client->message_head)
		client->message_tail = NULL;

	message->next = NULL;
	return message;
}

int chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	short events = POLLIN;
	if (client->output_head)
		events |= POLLOUT;

	struct pollfd poll_fd = {
		.fd = client->socket,
		.events = events};

	int poll_result = poll(&poll_fd, 1, (int)(timeout * 1000));
	if (poll_result == 0)
		return CHAT_ERR_TIMEOUT;

	if (poll_result < 0)
		return CHAT_ERR_SYS;

	if (poll_fd.revents & POLLOUT)
		if (send_pending_output(client) != 0)
			return CHAT_ERR_SYS;

	if (poll_fd.revents & POLLIN)
	{
		char read_buffer[512];
		ssize_t bytes_read = read(client->socket, read_buffer, sizeof(read_buffer));

		if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;

		if (bytes_read <= 0)
			return CHAT_ERR_SYS;

		process_incoming_data(client, read_buffer, bytes_read);
	}

	return 0;
}

static void trim(char *text, ssize_t *length)
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

static void append_message(struct chat_client *client, char *text, ssize_t length)
{
	if (length <= 0)
	{
		free(text);
		return;
	}

	struct chat_message *message = malloc(sizeof(*message));
	message->data = text;
	message->next = NULL;

	if (client->message_tail)
		client->message_tail->next = message;
	else
		client->message_head = message;

	client->message_tail = message;
}

static int send_pending_output(struct chat_client *client)
{
	while (client->output_head)
	{
		struct buffer *buffer = client->output_head;
		ssize_t bytes_written = send(client->socket, buffer->data + buffer->offset, buffer->size - buffer->offset, MSG_NOSIGNAL);

		if (bytes_written < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return CHAT_ERR_SYS;
		}

		buffer->offset += bytes_written;

		if (buffer->offset == buffer->size)
		{
			client->output_head = buffer->next;
			if (client->output_tail == buffer)
				client->output_tail = NULL;
			free(buffer);
		}
		else
		{
			break;
		}
	}

	return 0;
}

static void process_incoming_data(struct chat_client *client, const char *data, ssize_t length)
{
	if (client->input_size + length >= client->input_capacity)
	{
		client->input_capacity = (client->input_size + length) * 2;
		client->input_buffer = realloc(client->input_buffer, client->input_capacity);
	}

	memcpy(client->input_buffer + client->input_size, data, length);
	client->input_size += length;

	ssize_t start = 0;
	for (ssize_t i = 0; i < client->input_size; i++)
	{
		if (client->input_buffer[i] == '\n')
		{
			ssize_t line_length = i - start;

			if (line_length <= 0)
			{
				start = i + 1;
				continue;
			}

			char *line = malloc(line_length + 1);
			memcpy(line, client->input_buffer + start, line_length);
			line[line_length] = '\0';

			trim(line, &line_length);
			if (line_length > 0)
				append_message(client, line, line_length);
			else
				free(line);

			start = i + 1;
		}
	}

	if (start > 0)
	{
		memmove(client->input_buffer, client->input_buffer + start, client->input_size - start);
		client->input_size -= start;
	}
}

int chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int chat_client_get_events(const struct chat_client *client)
{
	if (client->socket < 0)
		return 0;

	int mask = CHAT_EVENT_INPUT;
	if (client->output_head)
		mask |= CHAT_EVENT_OUTPUT;

	return mask;
}

int chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	struct buffer *buffer = malloc(sizeof(struct buffer) + msg_size);
	buffer->next = NULL;
	buffer->size = msg_size;
	buffer->offset = 0;
	memcpy(buffer->data, msg, msg_size);

	if (client->output_tail)
		client->output_tail->next = buffer;
	else
		client->output_head = buffer;

	client->output_tail = buffer;
	return 0;
}