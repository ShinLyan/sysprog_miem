#include "chat.h"

#include <poll.h>
#include <stdlib.h>

void chat_message_delete(struct chat_message *message)
{
	free(message->data);
	free(message);
}

int chat_events_to_poll_events(int mask)
{
	int poll_event_mask = 0;
	if ((mask & CHAT_EVENT_INPUT) != 0)
		poll_event_mask |= POLLIN;

	if ((mask & CHAT_EVENT_OUTPUT) != 0)
		poll_event_mask |= POLLOUT;

	return poll_event_mask;
}