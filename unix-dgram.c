#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>
#include <jansson.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "janus/transport.h"
#include "janus/config.h"
#include "janus/debug.h"

/**
 * FIXME
 *
 * Currently using poll() with a single(?) thread to recv, and sending from any thread.
 * Should probably transition this to using libuv or something to make things a bit
 * simpler/safer...
 *
 **/


/* Constraints */
#define BMAX 8192	// Max buffer
#define PMAX 255	// Max socket path

/* Transport plugin information */
#define JANUS_UD_VERSION			1
#define JANUS_UD_VERSION_STRING	"0.0.1"
#define JANUS_UD_DESCRIPTION		"This transport plugin adds UNIX Datagram support to the Janus Gateway"
#define JANUS_UD_NAME				"JANUS Unix Datagram transport"
#define JANUS_UD_AUTHOR				""
#define JANUS_UD_PACKAGE			"janus.transport.ud"

/* Transport methods */
janus_transport *create(void);
int janus_ud_init(janus_transport_callbacks *callback, const char *config_path);
void janus_ud_destroy(void);
int janus_ud_get_api_compatibility(void);
int janus_ud_get_version(void);
const char *janus_ud_get_version_string(void);
const char *janus_ud_get_description(void);
const char *janus_ud_get_name(void);
const char *janus_ud_get_author(void);
const char *janus_ud_get_package(void);
gboolean janus_ud_is_janus_api_enabled(void);
gboolean janus_ud_is_admin_api_enabled(void);
int janus_ud_send_message(void *transport, void *request_id, gboolean admin, json_t *message);
void janus_ud_session_created(void *transport, guint64 session_id);
void janus_ud_session_over(void *transport, guint64 session_id, gboolean timeout);


/* Transport setup */
static janus_transport ud_plugin = JANUS_TRANSPORT_INIT (
	.init = janus_ud_init,
	.destroy = janus_ud_destroy,

	.get_api_compatibility = janus_ud_get_api_compatibility,
	.get_version = janus_ud_get_version,
	.get_version_string = janus_ud_get_version_string,
	.get_description = janus_ud_get_description,
	.get_name = janus_ud_get_name,
	.get_author = janus_ud_get_author,
	.get_package = janus_ud_get_package,

	.is_janus_api_enabled = janus_ud_is_janus_api_enabled,
	.is_admin_api_enabled = janus_ud_is_admin_api_enabled,

	.send_message = janus_ud_send_message,
	.session_created = janus_ud_session_created,
	.session_over = janus_ud_session_over,
);

/* Transport creator */
janus_transport *create(void) {
	JANUS_LOG(LOG_VERB, "[%s] created!\n", JANUS_UD_PACKAGE);
	return &ud_plugin;
}

static janus_transport_callbacks *gateway = NULL;

static GHashTable *addr_cache = NULL;
static pthread_mutex_t addr_mutex = PTHREAD_MUTEX_INITIALIZER;

static int adminfd = 0;
static int janusfd = 0;

static int thread_stop = 0;
static unsigned thread_count = 0;
static pthread_cond_t thread_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

typedef void *(*thread_func)(void *);
int new_thread(thread_func func, void *data) {
	int err;
	pthread_t thread;
	pthread_attr_t attr;
	
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if ((err = pthread_create(&thread, &attr, func, data))) return err;
	
	pthread_mutex_lock(&thread_lock);
	thread_count++;
	pthread_mutex_unlock(&thread_lock);
	
	return 0;
}

void stop_recv_threads() {
	pthread_mutex_lock(&thread_lock);
	thread_stop = 1;
	while (thread_count > 0) pthread_cond_wait(&thread_cv, &thread_lock);
	pthread_mutex_unlock(&thread_lock);
}

// Macro to calculate sockaddr_un length
#define UN_LEN(UN) \
	(sizeof(*(UN)) - sizeof((UN)->sun_path) + strlen((UN)->sun_path))

int new_unix_socket(const char *path, unsigned *err) {
	int r, fd;
	struct sockaddr_un addr;
	*err = 0;
	
	// Create UNIX socket
	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd == -1) goto Error;

	// Initialise our address structure
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	
	// Unlink the provided path and bind
	unlink(addr.sun_path);
	r = bind(fd, (struct sockaddr *)&addr, UN_LEN(&addr));
	if (r == -1) goto Error;
	
	return fd;
Error:
	close(fd);
	*err = errno;
	return -1;
}

struct buffer {
	size_t length;
	uint8_t data[BMAX];
};

/* receiving */
#define POLL_TIMEOUT 1000
void *recv_thread(void *data) {
   gboolean admin = FALSE;
   int stop = 0;
	struct pollfd fds[1];
	struct buffer b;
	struct sockaddr_storage addr;
	struct sockaddr_un *uaddr;
	socklen_t addrlen;

	if (data) admin = TRUE;
	if (admin) fds[0].fd = adminfd;
	else fds[0].fd = janusfd;
	fds[0].events = POLLIN;
	
	
	JANUS_LOG(LOG_INFO, "[%s] %s unix thread started\n", JANUS_UD_PACKAGE, (admin ? "Admin" : "Janus"));
	while (!stop) {
		pthread_mutex_lock(&thread_lock);
		stop = thread_stop;
		pthread_mutex_unlock(&thread_lock);
		if (stop) continue;
		
		// poll the socket
		int left = poll(fds, 1, POLL_TIMEOUT);
		if (left == -1) { // an error occured
			JANUS_LOG(LOG_ERR, "[%s] poll: %s\n", JANUS_UD_PACKAGE, strerror(errno));
			break;
		} else if (left == 0) { // timed out
			continue;
		}
		
		// error detected on socket?
		if (fds[0].revents & POLLERR) break;
		
		// recv()
		addrlen = sizeof(struct sockaddr_storage);
		ssize_t n = recvfrom(fds[0].fd, b.data, BMAX, 0, (struct sockaddr *)&addr, &addrlen);
		if (n == -1) { // an error occured
			JANUS_LOG(LOG_ERR, "[%s] recvfrom: %s\n", JANUS_UD_PACKAGE, strerror(errno));
			break;
		}
		b.data[n] = '\0';
		b.length = n;
		
		JANUS_LOG(LOG_HUGE, "[%s] received: %s\n", JANUS_UD_PACKAGE, b.data);
		
		// get a pointer to the cached version of this address
		struct sockaddr_un *uaddr, *caddr;
		uaddr = (struct sockaddr_un *)&addr;
		if (uaddr->sun_path[0] == '\0') {
			JANUS_LOG(LOG_WARN, "[%s] dropping request from anonymous socket\n", JANUS_UD_PACKAGE);
			continue;
		}

		caddr = g_hash_table_lookup(addr_cache, uaddr->sun_path);
		if (!caddr) { // if this address isn't cached, cache it
			caddr = malloc(sizeof(struct sockaddr_un));
			memcpy(caddr, uaddr, sizeof(struct sockaddr_un));
			g_hash_table_insert(addr_cache, uaddr->sun_path, caddr);
		}
		
		JANUS_LOG(LOG_HUGE, "[%s] remote address: %s\n", JANUS_UD_PACKAGE, uaddr->sun_path);
		
		// decode the message
		json_error_t err;
		json_t *msg = json_loadb(b.data, b.length, JSON_DISABLE_EOF_CHECK, &err);
		if (msg == NULL) { // an error occured
			JANUS_LOG(LOG_ERR, "[%s] json_loadb: %s\n", JANUS_UD_PACKAGE, err.text);
			continue;
		}

		// what kind of request is this?
		const gchar *method = json_string_value(json_object_get(msg, "janus"));
		JANUS_LOG(LOG_VERB, "[%s] Received '%s' request\n", JANUS_UD_PACKAGE, method);
		
		if (!strcasecmp(method, "info")) {
			json_t *reply = gateway->janus_info(json_string_value(json_object_get(msg, "transaction")));
			janus_ud_send_message(caddr, NULL, admin, reply);
			json_decref(reply);
			json_decref(msg);
			continue;
		}

		if (!strcasecmp(method, "ping")) {
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("pong"));
			json_object_set(reply, "transaction", json_object_get(msg, "transaction"));
			janus_ud_send_message(caddr, NULL, admin, reply);
			json_decref(reply);
			json_decref(msg);
			continue;
		}
		
		// pass *msg to the gateway
		gateway->incoming_request(&ud_plugin, caddr, NULL, admin, msg, NULL);
	}
	
	pthread_mutex_lock(&thread_lock);
	thread_count--;
	if (!thread_count) pthread_cond_signal(&thread_cv);
	pthread_mutex_unlock(&thread_lock);
	JANUS_LOG(LOG_INFO, "[%s] %s thread stopped\n", JANUS_UD_PACKAGE, (admin ? "admin" : "janus"));
	
	return NULL;
}

/* sending */
int dump_callback(uint8_t *data, size_t length, struct buffer *b) {
	if (b->length + length > BMAX) return -1;
	memcpy(&b->data[b->length], data, length);
	b->length += length;
	return 0;
}

int send_message(struct sockaddr_un *addr, gboolean admin, json_t *msg) {
	int fd;
	struct buffer b;
	
	if (admin) fd = adminfd;
	else fd = janusfd;
	
	b.length = 0;
	if (json_dump_callback(msg, (json_dump_callback_t)dump_callback, &b, JSON_COMPACT)) {
		JANUS_LOG(LOG_ERR, "[%s] json_dump_callback: encoding too long for buffer\n", JANUS_UD_PACKAGE);
		return -1;
	}
	b.data[b.length] = '\0';

	JANUS_LOG(LOG_HUGE, "[%s] sending: %s\n", JANUS_UD_PACKAGE, b.data);
	JANUS_LOG(LOG_HUGE, "[%s] remote address: %s\n", JANUS_UD_PACKAGE, addr->sun_path);

	socklen_t addrlen = UN_LEN(addr);
	ssize_t n = sendto(fd, b.data, b.length, 0, (struct sockaddr *)addr, addrlen);
	if (n == -1) {
		JANUS_LOG(LOG_ERR, "[%s] sendto: %s\n", JANUS_UD_PACKAGE, strerror(errno));
		return -1;
	}
	
	return 0;
}

/* Transport implementation */
int janus_ud_init(janus_transport_callbacks *callbacks, const char *cpath) {
	unsigned err;
	
	if (callbacks == NULL || cpath == NULL) return -1;
	gateway = callbacks;

	// Cache of remote addresses
	addr_cache = g_hash_table_new(g_str_hash, g_str_equal);

	char filename[255];
	snprintf(filename, 255, "%s/%s.cfg", cpath, JANUS_UD_PACKAGE);
	JANUS_LOG(LOG_VERB, "[%s] Configuration file: %s\n", JANUS_UD_PACKAGE, filename);

	janus_config *config = janus_config_parse(filename);
	if (config == NULL) {
		JANUS_LOG(LOG_FATAL, "[%s] Unable to parse configuration file\n", JANUS_UD_PACKAGE);
		return -1;
	}

	janus_config_item *item;

	// Setup admin endpoint
	item = janus_config_get_item_drilldown(config, "general", "admin");
	if (item && item->value) {
		char path[PMAX];

		memset(path, '\0', PMAX);
		strncpy(path, item->value, PMAX);
		if (path[PMAX - 1] != '\0') {
			JANUS_LOG(LOG_FATAL, "[%s] Admin socket path is too long\n", JANUS_UD_PACKAGE);
			return -1;
		}
		
		// Remove terminating '\n' if it exists
		if (path[strlen(path) - 1] == '\n') path[strlen(path) - 1] == '\0';
		
		// Create Admin socket
		adminfd = new_unix_socket(path, &err);
		if (adminfd == -1) {
			JANUS_LOG(LOG_ERR, "[%s] new_unix_socket: %s\n", JANUS_UD_PACKAGE, strerror(err));
		}
		
		// Start receiving on socket
		new_thread(recv_thread, (void *)1);
	}
	
	// Setup janus endpoint
	item = janus_config_get_item_drilldown(config, "general", "janus");
	if (item && item->value) {
		char path[PMAX];

		memset(path, '\0', PMAX);
		strncpy(path, item->value, PMAX);
		if (path[PMAX - 1] != '\0') {
			JANUS_LOG(LOG_FATAL, "[%s] Janus socket path is too long\n", JANUS_UD_PACKAGE);
			return -1;
		}
		
		// Remove terminating '\n' if it exists
		if (path[strlen(path) - 1] == '\n') path[strlen(path) - 1] == '\0';

		// Create Janus socket
		janusfd = new_unix_socket(path, &err);
		if (janusfd == -1) {
			JANUS_LOG(LOG_ERR, "[%s] new_unix_socket: %s\n", JANUS_UD_PACKAGE, strerror(err));
		}

		// Start receiving on socket
		new_thread(recv_thread, NULL);
	}
	
	JANUS_LOG(LOG_INFO, "[%s] started\n", JANUS_UD_PACKAGE);
	return 0;
}

void janus_ud_destroy(void) {
	stop_recv_threads();
	
	if (adminfd) close(adminfd);
	if (janusfd) close(janusfd);
	g_hash_table_destroy(addr_cache);
	JANUS_LOG(LOG_INFO, "[%s] stopped\n", JANUS_UD_PACKAGE);
}

int janus_ud_get_api_compatibility(void) {
	return JANUS_TRANSPORT_API_VERSION;
}

int janus_ud_get_version(void) {
	return JANUS_UD_VERSION;
}

const char *janus_ud_get_version_string(void) {
	return JANUS_UD_VERSION_STRING;
}

const char *janus_ud_get_description(void) {
	return JANUS_UD_DESCRIPTION;
}

const char *janus_ud_get_name(void) {
	return JANUS_UD_NAME;
}

const char *janus_ud_get_author(void) {
	return JANUS_UD_AUTHOR;
}

const char *janus_ud_get_package(void) {
	return JANUS_UD_PACKAGE;
}

gboolean janus_ud_is_janus_api_enabled(void) {
	if (janusfd) return TRUE;
	return FALSE;
}

gboolean janus_ud_is_admin_api_enabled(void) {
	if (adminfd) return TRUE;
	return FALSE;
}

int janus_ud_send_message(void *addr, void *rid, gboolean admin, json_t *msg) {
	send_message(addr, admin, msg);
	return 0;
}

void janus_ud_session_created(void *addr, guint64 session) {
}

void janus_ud_session_over(void *addr, guint64 session, gboolean timeout) {
}
