// See https://i3wm.org/docs/ipc.html for protocol information

#define _XOPEN_SOURCE 700
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <list.h>
#include <libinput.h>
#ifdef __linux__
struct ucred {
	pid_t pid;
	uid_t uid;
	gid_t gid;
};
#endif
#include "sway/ipc-json.h"
#include "sway/ipc-server.h"
#include "sway/security.h"
#include "sway/config.h"
#include "sway/commands.h"
#include "sway/input.h"
#include "sway/server.h"
#include "stringop.h"
#include "log.h"
#include "list.h"
#include "util.h"

static int ipc_socket = -1;
static struct wl_event_source *ipc_event_source =  NULL;
static struct sockaddr_un *ipc_sockaddr = NULL;
static list_t *ipc_client_list = NULL;

static const char ipc_magic[] = {'i', '3', '-', 'i', 'p', 'c'};

struct ipc_client {
	struct wl_event_source *event_source;
	struct wl_event_source *writable_event_source;
	int fd;
	uint32_t payload_length;
	uint32_t security_policy;
	enum ipc_command_type current_command;
	enum ipc_command_type subscribed_events;
	size_t write_buffer_len;
	size_t write_buffer_size;
	char *write_buffer;
};

static list_t *ipc_get_pixel_requests = NULL;

struct sockaddr_un *ipc_user_sockaddr(void);
int ipc_handle_connection(int fd, uint32_t mask, void *data);
int ipc_client_handle_readable(int client_fd, uint32_t mask, void *data);
int ipc_client_handle_writable(int client_fd, uint32_t mask, void *data);
void ipc_client_disconnect(struct ipc_client *client);
void ipc_client_handle_command(struct ipc_client *client);
bool ipc_send_reply(struct ipc_client *client, const char *payload, uint32_t payload_length);
void ipc_get_workspaces_callback(swayc_t *workspace, void *data);
void ipc_get_outputs_callback(swayc_t *container, void *data);
static void ipc_get_marks_callback(swayc_t *container, void *data);

void ipc_init(void) {
	ipc_socket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (ipc_socket == -1) {
		sway_abort("Unable to create IPC socket");
	}

	ipc_sockaddr = ipc_user_sockaddr();

	// We want to use socket name set by user, not existing socket from another sway instance.
	if (getenv("SWAYSOCK") != NULL && access(getenv("SWAYSOCK"), F_OK) == -1) {
		strncpy(ipc_sockaddr->sun_path, getenv("SWAYSOCK"), sizeof(ipc_sockaddr->sun_path));
		ipc_sockaddr->sun_path[sizeof(ipc_sockaddr->sun_path) - 1] = 0;
	}

	unlink(ipc_sockaddr->sun_path);
	if (bind(ipc_socket, (struct sockaddr *)ipc_sockaddr, sizeof(*ipc_sockaddr)) == -1) {
		sway_abort("Unable to bind IPC socket");
	}

	if (listen(ipc_socket, 3) == -1) {
		sway_abort("Unable to listen on IPC socket");
	}

	// Set i3 IPC socket path so that i3-msg works out of the box
	setenv("I3SOCK", ipc_sockaddr->sun_path, 1);
	setenv("SWAYSOCK", ipc_sockaddr->sun_path, 1);

	ipc_client_list = create_list();
	ipc_get_pixel_requests = create_list();

	ipc_event_source = wl_event_loop_add_fd(server.wl_event_loop, ipc_socket,
			WL_EVENT_READABLE, ipc_handle_connection, NULL);
}

void ipc_terminate(void) {
	if (ipc_event_source) {
		wl_event_source_remove(ipc_event_source);
	}
	close(ipc_socket);
	unlink(ipc_sockaddr->sun_path);

	list_free(ipc_client_list);

	if (ipc_sockaddr) {
		free(ipc_sockaddr);
	}
}

struct sockaddr_un *ipc_user_sockaddr(void) {
	struct sockaddr_un *ipc_sockaddr = malloc(sizeof(struct sockaddr_un));
	if (ipc_sockaddr == NULL) {
		sway_abort("Can't allocate ipc_sockaddr");
	}

	ipc_sockaddr->sun_family = AF_UNIX;
	int path_size = sizeof(ipc_sockaddr->sun_path);

	// Env var typically set by logind, e.g. "/run/user/<user-id>"
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) {
		dir = "/tmp";
	}
	if (path_size <= snprintf(ipc_sockaddr->sun_path, path_size,
			"%s/sway-ipc.%i.%i.sock", dir, getuid(), getpid())) {
		sway_abort("Socket path won't fit into ipc_sockaddr->sun_path");
	}

	return ipc_sockaddr;
}

static pid_t get_client_pid(int client_fd) {
// FreeBSD supports getting uid/gid, but not pid
#ifdef __linux__
	struct ucred ucred;
	socklen_t len = sizeof(struct ucred);

	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
		return -1;
	}

	return ucred.pid;
#else
	return -1;
#endif
}

int ipc_handle_connection(int fd, uint32_t mask, void *data) {
	(void) fd; (void) data;
	sway_log(L_DEBUG, "Event on IPC listening socket");
	assert(mask == WL_EVENT_READABLE);

	int client_fd = accept(ipc_socket, NULL, NULL);
	if (client_fd == -1) {
		sway_log_errno(L_ERROR, "Unable to accept IPC client connection");
		return 0;
	}

	int flags;
	if ((flags = fcntl(client_fd, F_GETFD)) == -1
			|| fcntl(client_fd, F_SETFD, flags|FD_CLOEXEC) == -1) {
		sway_log_errno(L_ERROR, "Unable to set CLOEXEC on IPC client socket");
		close(client_fd);
		return 0;
	}
	if ((flags = fcntl(client_fd, F_GETFL)) == -1
			|| fcntl(client_fd, F_SETFL, flags|O_NONBLOCK) == -1) {
		sway_log_errno(L_ERROR, "Unable to set NONBLOCK on IPC client socket");
		close(client_fd);
		return 0;
	}

	struct ipc_client* client = malloc(sizeof(struct ipc_client));
	if (!client) {
		sway_log(L_ERROR, "Unable to allocate ipc client");
		close(client_fd);
		return 0;
	}
	client->payload_length = 0;
	client->fd = client_fd;
	client->subscribed_events = 0;
	client->event_source = wl_event_loop_add_fd(server.wl_event_loop, client_fd,
			WL_EVENT_READABLE, ipc_client_handle_readable, client);
	client->writable_event_source = NULL;

	client->write_buffer_size = 128;
	client->write_buffer_len = 0;
	client->write_buffer = malloc(client->write_buffer_size);
	if (!client->write_buffer) {
		sway_log(L_ERROR, "Unable to allocate ipc client write buffer");
		close(client_fd);
		return 0;
	}

	pid_t pid = get_client_pid(client->fd);
	client->security_policy = get_ipc_policy_mask(pid);

	sway_log(L_DEBUG, "New client: fd %d, pid %d", client_fd, pid);

	list_add(ipc_client_list, client);

	return 0;
}

static const int ipc_header_size = sizeof(ipc_magic)+8;

int ipc_client_handle_readable(int client_fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;

	if (mask & WL_EVENT_ERROR) {
		sway_log(L_ERROR, "IPC Client socket error, removing client");
		ipc_client_disconnect(client);
		return 0;
	}

	if (mask & WL_EVENT_HANGUP) {
		sway_log(L_DEBUG, "Client %d hung up", client->fd);
		ipc_client_disconnect(client);
		return 0;
	}

	sway_log(L_DEBUG, "Client %d readable", client->fd);

	int read_available;
	if (ioctl(client_fd, FIONREAD, &read_available) == -1) {
		sway_log_errno(L_INFO, "Unable to read IPC socket buffer size");
		ipc_client_disconnect(client);
		return 0;
	}

	// Wait for the rest of the command payload in case the header has already been read
	if (client->payload_length > 0) {
		if ((uint32_t)read_available >= client->payload_length) {
			ipc_client_handle_command(client);
		}
		return 0;
	}

	if (read_available < ipc_header_size) {
		return 0;
	}

	uint8_t buf[ipc_header_size];
	uint32_t *buf32 = (uint32_t*)(buf + sizeof(ipc_magic));
	// Should be fully available, because read_available >= ipc_header_size
	ssize_t received = recv(client_fd, buf, ipc_header_size, 0);
	if (received == -1) {
		sway_log_errno(L_INFO, "Unable to receive header from IPC client");
		ipc_client_disconnect(client);
		return 0;
	}

	if (memcmp(buf, ipc_magic, sizeof(ipc_magic)) != 0) {
		sway_log(L_DEBUG, "IPC header check failed");
		ipc_client_disconnect(client);
		return 0;
	}

	client->payload_length = buf32[0];
	client->current_command = (enum ipc_command_type)buf32[1];

	if (read_available - received >= (long)client->payload_length) {
		ipc_client_handle_command(client);
	}

	return 0;
}

int ipc_client_handle_writable(int client_fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;

	if (mask & WL_EVENT_ERROR) {
		sway_log(L_ERROR, "IPC Client socket error, removing client");
		ipc_client_disconnect(client);
		return 0;
	}

	if (mask & WL_EVENT_HANGUP) {
		sway_log(L_DEBUG, "Client %d hung up", client->fd);
		ipc_client_disconnect(client);
		return 0;
	}

	if (client->write_buffer_len <= 0) {
		return 0;
	}

	sway_log(L_DEBUG, "Client %d writable", client->fd);

	ssize_t written = write(client->fd, client->write_buffer, client->write_buffer_len);

	if (written == -1 && errno == EAGAIN) {
		return 0;
	} else if (written == -1) {
		sway_log_errno(L_INFO, "Unable to send data from queue to IPC client");
		ipc_client_disconnect(client);
		return 0;
	}

	memmove(client->write_buffer, client->write_buffer + written, client->write_buffer_len - written);
	client->write_buffer_len -= written;

	if (client->write_buffer_len == 0 && client->writable_event_source) {
		wl_event_source_remove(client->writable_event_source);
		client->writable_event_source = NULL;
	}

	return 0;
}

void ipc_client_disconnect(struct ipc_client *client) {
	if (!sway_assert(client != NULL, "client != NULL")) {
		return;
	}

	if (client->fd != -1) {
		shutdown(client->fd, SHUT_RDWR);
	}

	sway_log(L_INFO, "IPC Client %d disconnected", client->fd);
	wl_event_source_remove(client->event_source);
	if (client->writable_event_source) {
		wl_event_source_remove(client->writable_event_source);
	}
	int i = 0;
	while (i < ipc_client_list->length && ipc_client_list->items[i] != client) i++;
	list_del(ipc_client_list, i);
	free(client->write_buffer);
	close(client->fd);
	free(client);
}

bool output_by_name_test(swayc_t *view, void *data) {
	char *name = (char *)data;
	if (view->type != C_OUTPUT) {
		return false;
	}
	return !strcmp(name, view->name);
}

// greedy wildcard (only "*") matching
bool mime_type_matches(const char *mime_type, const char *pattern) {
	const char *wildcard = NULL;
	while (*mime_type && *pattern) {
		if (*pattern == '*' && !wildcard) {
			wildcard = pattern;
			++pattern;
		}

		if (*mime_type != *pattern) {
			if (!wildcard)
				return false;

			pattern = wildcard;
			++mime_type;
			continue;
		}

		++mime_type;
		++pattern;
	}

	while (*pattern == '*') {
		++pattern;
	}

	return (*mime_type == *pattern);
}

void ipc_client_handle_command(struct ipc_client *client) {
	if (!sway_assert(client != NULL, "client != NULL")) {
		return;
	}

	char *buf = malloc(client->payload_length + 1);
	if (!buf) {
		sway_log_errno(L_INFO, "Unable to allocate IPC payload");
		ipc_client_disconnect(client);
		return;
	}
	if (client->payload_length > 0) {
		// Payload should be fully available
		ssize_t received = recv(client->fd, buf, client->payload_length, 0);
		if (received == -1)
		{
			sway_log_errno(L_INFO, "Unable to receive payload from IPC client");
			ipc_client_disconnect(client);
			free(buf);
			return;
		}
	}
	buf[client->payload_length] = '\0';

	const char *error_denied = "{ \"success\": false, \"error\": \"Permission denied\" }";

	switch (client->current_command) {
	case IPC_COMMAND:
	{
		if (!(client->security_policy & IPC_FEATURE_COMMAND)) {
			goto exit_denied;
		}
		struct cmd_results *results = handle_command(buf, CONTEXT_IPC);
		const char *json = cmd_results_to_json(results);
		char reply[256];
		int length = snprintf(reply, sizeof(reply), "%s", json);
		ipc_send_reply(client, reply, (uint32_t) length);
		free_cmd_results(results);
		goto exit_cleanup;
	}

	case IPC_SUBSCRIBE:
	{
		// TODO: Check if they're permitted to use these events
		struct json_object *request = json_tokener_parse(buf);
		if (request == NULL) {
			ipc_send_reply(client, "{\"success\": false}", 18);
			sway_log_errno(L_INFO, "Failed to read request");
			goto exit_cleanup;
		}

		// parse requested event types
		for (int i = 0; i < json_object_array_length(request); i++) {
			const char *event_type = json_object_get_string(json_object_array_get_idx(request, i));
			if (strcmp(event_type, "workspace") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_WORKSPACE);
			} else if (strcmp(event_type, "barconfig_update") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_BARCONFIG_UPDATE);
			} else if (strcmp(event_type, "mode") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_MODE);
			} else if (strcmp(event_type, "window") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_WINDOW);
			} else if (strcmp(event_type, "modifier") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_MODIFIER);
			} else if (strcmp(event_type, "binding") == 0) {
				client->subscribed_events |= event_mask(IPC_EVENT_BINDING);
			} else {
				ipc_send_reply(client, "{\"success\": false}", 18);
				json_object_put(request);
				sway_log_errno(L_INFO, "Failed to parse request");
				goto exit_cleanup;
			}
		}

		json_object_put(request);

		ipc_send_reply(client, "{\"success\": true}", 17);
		goto exit_cleanup;
	}

	case IPC_GET_WORKSPACES:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_WORKSPACES)) {
			goto exit_denied;
		}
		json_object *workspaces = json_object_new_array();
		container_map(&root_container, ipc_get_workspaces_callback, workspaces);
		const char *json_string = json_object_to_json_string(workspaces);
		ipc_send_reply(client, json_string, (uint32_t) strlen(json_string));
		json_object_put(workspaces); // free
		goto exit_cleanup;
	}

	case IPC_GET_INPUTS:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_INPUTS)) {
			goto exit_denied;
		}
		json_object *inputs = json_object_new_array();
		if (input_devices) {
			for(int i = 0; i<input_devices->length; i++) {
				struct libinput_device *device = input_devices->items[i];
				json_object_array_add(inputs, ipc_json_describe_input(device));
			}
		}
		const char *json_string = json_object_to_json_string(inputs);
		ipc_send_reply(client, json_string, (uint32_t) strlen(json_string));
		json_object_put(inputs);
		goto exit_cleanup;
	}

	case IPC_GET_OUTPUTS:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_OUTPUTS)) {
			goto exit_denied;
		}
		json_object *outputs = json_object_new_array();
		container_map(&root_container, ipc_get_outputs_callback, outputs);
		const char *json_string = json_object_to_json_string(outputs);
		ipc_send_reply(client, json_string, (uint32_t) strlen(json_string));
		json_object_put(outputs); // free
		goto exit_cleanup;
	}

	case IPC_GET_TREE:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_TREE)) {
			goto exit_denied;
		}
		json_object *tree = ipc_json_describe_container_recursive(&root_container);
		const char *json_string = json_object_to_json_string(tree);
		ipc_send_reply(client, json_string, (uint32_t) strlen(json_string));
		json_object_put(tree);
		goto exit_cleanup;
	}

	case IPC_GET_MARKS:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_MARKS)) {
			goto exit_denied;
		}
		json_object *marks = json_object_new_array();
		container_map(&root_container, ipc_get_marks_callback, marks);
		const char *json_string = json_object_to_json_string(marks);
		ipc_send_reply(client, json_string, (uint32_t) strlen(json_string));
		json_object_put(marks);
		goto exit_cleanup;
	}

	case IPC_GET_VERSION:
	{
		json_object *version = ipc_json_get_version();
		const char *json_string = json_object_to_json_string(version);
		ipc_send_reply(client, json_string, (uint32_t)strlen(json_string));
		json_object_put(version); // free
		goto exit_cleanup;
	}

	case IPC_GET_BAR_CONFIG:
	{
		if (!(client->security_policy & IPC_FEATURE_GET_BAR_CONFIG)) {
			goto exit_denied;
		}
		if (!buf[0]) {
			// Send list of configured bar IDs
			json_object *bars = json_object_new_array();
			int i;
			for (i = 0; i < config->bars->length; ++i) {
				struct bar_config *bar = config->bars->items[i];
				json_object_array_add(bars, json_object_new_string(bar->id));
			}
			const char *json_string = json_object_to_json_string(bars);
			ipc_send_reply(client, json_string, (uint32_t)strlen(json_string));
			json_object_put(bars); // free
		} else {
			// Send particular bar's details
			struct bar_config *bar = NULL;
			int i;
			for (i = 0; i < config->bars->length; ++i) {
				bar = config->bars->items[i];
				if (strcmp(buf, bar->id) == 0) {
					break;
				}
				bar = NULL;
			}
			if (!bar) {
				const char *error = "{ \"success\": false, \"error\": \"No bar with that ID\" }";
				ipc_send_reply(client, error, (uint32_t)strlen(error));
				goto exit_cleanup;
			}
			json_object *json = ipc_json_describe_bar_config(bar);
			const char *json_string = json_object_to_json_string(json);
			ipc_send_reply(client, json_string, (uint32_t)strlen(json_string));
			json_object_put(json); // free
		}
		goto exit_cleanup;
	}

	case IPC_GET_CLIPBOARD:
		// TODO WLR
		break;

	default:
		sway_log(L_INFO, "Unknown IPC command type %i", client->current_command);
		goto exit_cleanup;
	}

exit_denied:
	ipc_send_reply(client, error_denied, (uint32_t)strlen(error_denied));
	sway_log(L_DEBUG, "Denied IPC client access to %i", client->current_command);

exit_cleanup:
	client->payload_length = 0;
	free(buf);
	return;
}

bool ipc_send_reply(struct ipc_client *client, const char *payload, uint32_t payload_length) {
	assert(payload);

	char data[ipc_header_size];
	uint32_t *data32 = (uint32_t*)(data + sizeof(ipc_magic));

	memcpy(data, ipc_magic, sizeof(ipc_magic));
	data32[0] = payload_length;
	data32[1] = client->current_command;

	while (client->write_buffer_len + ipc_header_size + payload_length >=
				 client->write_buffer_size) {
		client->write_buffer_size *= 2;
	}

	// TODO: reduce the limit back to 4 MB when screenshooter is implemented
	if (client->write_buffer_size > (1 << 28)) { // 256 MB
		sway_log(L_ERROR, "Client write buffer too big, disconnecting client");
		ipc_client_disconnect(client);
		return false;
	}

	char *new_buffer = realloc(client->write_buffer, client->write_buffer_size);
	if (!new_buffer) {
		sway_log(L_ERROR, "Unable to reallocate ipc client write buffer");
		ipc_client_disconnect(client);
		return false;
	}
	client->write_buffer = new_buffer;

	memcpy(client->write_buffer + client->write_buffer_len, data, ipc_header_size);
	client->write_buffer_len += ipc_header_size;
	memcpy(client->write_buffer + client->write_buffer_len, payload, payload_length);
	client->write_buffer_len += payload_length;

	if (!client->writable_event_source) {
		client->writable_event_source = wl_event_loop_add_fd(
				server.wl_event_loop, client->fd, WL_EVENT_WRITABLE,
				ipc_client_handle_writable, client);
	}

	sway_log(L_DEBUG, "Added IPC reply to client %d queue: %s", client->fd, payload);

	return true;
}

void ipc_get_workspaces_callback(swayc_t *workspace, void *data) {
	if (workspace->type == C_WORKSPACE) {
		json_object *workspace_json = ipc_json_describe_container(workspace);
		// override the default focused indicator because
		// it's set differently for the get_workspaces reply
		bool focused = root_container.focused == workspace->parent && workspace->parent->focused == workspace;
		json_object_object_del(workspace_json, "focused");
		json_object_object_add(workspace_json, "focused", json_object_new_boolean(focused));
		json_object_array_add((json_object *)data, workspace_json);
	}
}

void ipc_get_outputs_callback(swayc_t *container, void *data) {
	if (container->type == C_OUTPUT) {
		json_object_array_add((json_object *)data, ipc_json_describe_container(container));
	}
}

static void ipc_get_marks_callback(swayc_t *container, void *data) {
	json_object *object = (json_object *)data;
	if (container->marks) {
		for (int i = 0; i < container->marks->length; ++i) {
			char *mark = (char *)container->marks->items[i];
			json_object_array_add(object, json_object_new_string(mark));
		}
	}
}

void ipc_send_event(const char *json_string, enum ipc_command_type event) {
	static struct {
		enum ipc_command_type event;
		enum ipc_feature feature;
	} security_mappings[] = {
		{ IPC_EVENT_WORKSPACE, IPC_FEATURE_EVENT_WORKSPACE },
		{ IPC_EVENT_OUTPUT, IPC_FEATURE_EVENT_OUTPUT },
		{ IPC_EVENT_MODE, IPC_FEATURE_EVENT_MODE },
		{ IPC_EVENT_WINDOW, IPC_FEATURE_EVENT_WINDOW },
		{ IPC_EVENT_BINDING, IPC_FEATURE_EVENT_BINDING },
		{ IPC_EVENT_INPUT, IPC_FEATURE_EVENT_INPUT }
	};

	uint32_t security_mask = 0;
	for (size_t i = 0; i < sizeof(security_mappings) / sizeof(security_mappings[0]); ++i) {
		if (security_mappings[i].event == event) {
			security_mask = security_mappings[i].feature;
			break;
		}
	}

	int i;
	struct ipc_client *client;
	for (i = 0; i < ipc_client_list->length; i++) {
		client = ipc_client_list->items[i];
		if (!(client->security_policy & security_mask)) {
			continue;
		}
		if ((client->subscribed_events & event_mask(event)) == 0) {
			continue;
		}
		client->current_command = event;
		if (!ipc_send_reply(client, json_string, (uint32_t) strlen(json_string))) {
			sway_log_errno(L_INFO, "Unable to send reply to IPC client");
			ipc_client_disconnect(client);
		}
	}
}

void ipc_event_workspace(swayc_t *old, swayc_t *new, const char *change) {
	sway_log(L_DEBUG, "Sending workspace::%s event", change);
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(change));
	if (strcmp("focus", change) == 0) {
		if (old) {
			json_object_object_add(obj, "old", ipc_json_describe_container_recursive(old));
		} else {
			json_object_object_add(obj, "old", NULL);
		}
	}

	if (new) {
		json_object_object_add(obj, "current", ipc_json_describe_container_recursive(new));
	} else {
		json_object_object_add(obj, "current", NULL);
	}

	const char *json_string = json_object_to_json_string(obj);
	ipc_send_event(json_string, IPC_EVENT_WORKSPACE);

	json_object_put(obj); // free
}

void ipc_event_window(swayc_t *window, const char *change) {
	sway_log(L_DEBUG, "Sending window::%s event", change);
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(change));
	json_object_object_add(obj, "container", ipc_json_describe_container_recursive(window));

	const char *json_string = json_object_to_json_string(obj);
	ipc_send_event(json_string, IPC_EVENT_WINDOW);

	json_object_put(obj); // free
}

void ipc_event_barconfig_update(struct bar_config *bar) {
	sway_log(L_DEBUG, "Sending barconfig_update event");
	json_object *json = ipc_json_describe_bar_config(bar);
	const char *json_string = json_object_to_json_string(json);
	ipc_send_event(json_string, IPC_EVENT_BARCONFIG_UPDATE);

	json_object_put(json); // free
}

void ipc_event_mode(const char *mode) {
	sway_log(L_DEBUG, "Sending mode::%s event", mode);
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(mode));

	const char *json_string = json_object_to_json_string(obj);
	ipc_send_event(json_string, IPC_EVENT_MODE);

	json_object_put(obj); // free
}

void ipc_event_modifier(uint32_t modifier, const char *state) {
	sway_log(L_DEBUG, "Sending modifier::%s event", state);
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(state));

	const char *modifier_name = get_modifier_name_by_mask(modifier);
	json_object_object_add(obj, "modifier", json_object_new_string(modifier_name));

	const char *json_string = json_object_to_json_string(obj);
	ipc_send_event(json_string, IPC_EVENT_MODIFIER);

	json_object_put(obj); // free
}

static void ipc_event_binding(json_object *sb_obj) {
	sway_log(L_DEBUG, "Sending binding::run event");
	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string("run"));
	json_object_object_add(obj, "binding", sb_obj);

	const char *json_string = json_object_to_json_string(obj);
	ipc_send_event(json_string, IPC_EVENT_BINDING);

	json_object_put(obj); // free
}

void ipc_event_binding_keyboard(struct sway_binding *sb) {
	json_object *sb_obj = json_object_new_object();
	json_object_object_add(sb_obj, "command", json_object_new_string(sb->command));

	const char *names[10];

	int len = get_modifier_names(names, sb->modifiers);
	int i;
	json_object *modifiers = json_object_new_array();
	for (i = 0; i < len; ++i) {
		json_object_array_add(modifiers, json_object_new_string(names[i]));
	}

	json_object_object_add(sb_obj, "event_state_mask", modifiers);

	json_object *input_codes = json_object_new_array();
	int input_code = 0;
	json_object *symbols = json_object_new_array();
	json_object *symbol = NULL;

	if (sb->bindcode) { // bindcode: populate input_codes
		uint32_t keycode;
		for (i = 0; i < sb->keys->length; ++i) {
			keycode = *(uint32_t *)sb->keys->items[i];
			json_object_array_add(input_codes, json_object_new_int(keycode));
			if (i == 0) {
				input_code = keycode;
			}
		}
	} else { // bindsym: populate symbols
		uint32_t keysym;
		char buffer[64];
		for (i = 0; i < sb->keys->length; ++i) {
			keysym = *(uint32_t *)sb->keys->items[i];
			if (xkb_keysym_get_name(keysym, buffer, 64) > 0) {
				json_object *str = json_object_new_string(buffer);
				json_object_array_add(symbols, str);
				if (i == 0) {
					symbol = str;
				}
			}
		}
	}

	json_object_object_add(sb_obj, "input_codes", input_codes);
	json_object_object_add(sb_obj, "input_code", json_object_new_int(input_code));
	json_object_object_add(sb_obj, "symbols", symbols);
	json_object_object_add(sb_obj, "symbol", symbol);
	json_object_object_add(sb_obj, "input_type", json_object_new_string("keyboard"));

	ipc_event_binding(sb_obj);
}
