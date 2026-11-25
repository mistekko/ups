#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

/* defines const char document_packet[] */
/*     and const char favicon_packet[]  */
#include "resources.h"

#define PORT_NBO(PORT_HBO) (PORT_HBO << 8 | PORT_HBO >> 8) & 0xffff
#define IP_NBO(MOST_SIGNIFICANT_BYTE, BYTE2, BYTE3, BYTE4)	  \
	MOST_SIGNIFICANT_BYTE | BYTE2 << 8 | BYTE3 << 16 | BYTE4 << 24

#define BUF_SIZE 512 /* size of all data buffers */

enum PacketError {
	ConnectionTerminated = 1,
	TooSmall,
	InvalidMethod,
	InvalidTarget,
	MissingContentLength,
	InvalidHeaderValue,
	IncorrectContentLength,
	EarlyEnding,
	InternalError
};

enum Target {
	Page,
	Favicon,
	Upload
};

enum Method {
	Get,
	Put
};

struct packet {
	enum Method method;
	enum Target target;
	char *body;
	ssize_t body_len;
};

char *target_packets[] = {
	[Page] = document_packet,
	[Favicon] = favicon_packet
};

/* I plan to switch to using select(3) sometime in the future... */
int connections[1024] = { 0 };

static void cleanup (int sig) {
	printf("Server termination requested. Cleaning up... ");

	/* ...but for now we'll only worry about the listening socket and
	 * the client socket */
	for (int i = 0; i < 2; ++ i) {
		if (connections[i]) close(connections[i]);
	}

	puts("done.");
	signal(sig, SIG_DFL);
	raise(sig);
}

static void die (const char *message) {
	perror(message);
	exit(1);
}

char *strcasestr (const char *haystack, const char *needle) {
	for (char *h = (char *)haystack; *h; ++h)
		for (int i = 0;  ; ++i) {
			if (needle[i] == '\0')
				return h;
			else if (toupper(h[i]) != toupper(needle[i])) {
				break;
			}
		}
	return NULL;
}

bool fd_is_readable (int fd) {
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN
	};
	poll(&pfd, 1, POLLIN);
	return pfd.revents & POLLIN;
}

void flush_input (int fd) {
	char buf[512];
	while (fd_is_readable(fd)) read(fd, buf, 512);
}

int get_packet (int socket, struct packet *p) {
	char buf[BUF_SIZE], *parse_head = buf;
	size_t body_len;
	ssize_t buf_fill;

	buf_fill = read(socket, buf, BUF_SIZE - 1);
	if (buf_fill == 0) return ConnectionTerminated;
	if (buf[buf_fill - 1] == '\0') {
		--buf_fill;
		puts("read(3) would even write '\0'");
	} else buf[buf_fill] = '\0';

	if (buf_fill < 18) {
		flush_input(socket);
		return TooSmall;
	}

	if (strncmp(buf, "GET", 3) == 0) {
		p->method = Get;
		parse_head += 4;
	} else if (strncmp(buf, "PUT", 3) == 0) {
		p->method = Put;
		parse_head += 4;
	} else {
		flush_input(socket);
		return InvalidMethod;
	}

	{
		char tmp;
		int target_len = 0;

		while (!isspace(parse_head[target_len])
		       && parse_head[target_len] != '\0')
			++target_len;

		tmp = parse_head[target_len];
		parse_head[target_len] = '\0';
		if (strstr(parse_head, "favicon.svg") != NULL)
			p->target = Favicon;
		else if (strstr(parse_head, "favicon.ico") != NULL) {
			flush_input(socket);
			return InvalidTarget;
		} else
			p->target = Page;
		parse_head[target_len] = tmp;

		if (p->method == Get) {
			flush_input(socket);
			return 0;
		}

		parse_head += target_len;
	}

	{
		char *tmp_parse_head;

		parse_head = strcasestr(parse_head, "Content-Length:");
		if (parse_head == NULL) {
			flush_input(socket);
			return MissingContentLength;
		}

		parse_head += sizeof("Content-Length:") - 1; /* 1: '\0' */

		body_len = strtol(parse_head, &tmp_parse_head, 10);
		if (tmp_parse_head == parse_head) {
			flush_input(socket);
			return InvalidHeaderValue;
		}
		if (body_len == 0) {
			flush_input(socket);
			return 0;
		}

		p->body_len = body_len;

		parse_head = strstr(parse_head, "\r\n\r\n");
		if (!parse_head) {
			flush_input(socket);
			return EarlyEnding;
		}

		parse_head += sizeof("\r\n\r\n") - 1; /* - 1: '\0' */
	}

	{
		char *body = malloc(body_len + 1);
		size_t body_in_buf = buf_fill - (parse_head - buf);
		ssize_t unread_bytes = body_len - body_in_buf;
		/* Content-Length could be incorrectly low... */
		if (unread_bytes < 0) {
			flush_input(socket);
			return IncorrectContentLength;
		}
		/* or incorrectly high */
		else if (!fd_is_readable(socket)) {
			flush_input(socket);
			return IncorrectContentLength;
		}

		strcpy(body, parse_head);
		if (unread_bytes != 0)
			if (read(socket, body+body_in_buf, unread_bytes) != unread_bytes) {
				puts("Failed to read packet");
				return InternalError;
			}

		body[body_len] = '\0';
		p->body = body;
	}

	return 0;
}

int respond (int client_fd, int code, const char *message) {
	if (code > 999 || code < 100) {
		fputs("Attempted to send response with invalid code", stderr);
		return -1;
	}

	/* 13: http/1.1 xxx; 4: \r\n\r\n; 1: '\0'; */
	size_t response_len = strlen(message) + 13 + 4 + 1;
	if (response_len > 256) { /* message can't be longer than 238 chars */
		fputs("Attempted to send obnoxiously long reason in response", stderr);
		return -1;
	}

	char response[256];
	snprintf(response, response_len, "HTTP/1.1 %d %s\r\n\r\n", code, message);
	write(client_fd, response, response_len - 1); /* don't write '\0' */
	printf("%d: %s\n", code, message);

	return 0;
}

void ok (struct packet *p) {
	char *method, *target;
	switch (p->method) {
	case Get:  method = "GET";  break;
	case Put:  method = "PUT";  break;
	} switch (p->target) {
	case Page:    target = "/up.html";     break;
	case Favicon: target = "/favicon.svg"; break;
	case Upload:  target = "/???";         break;
	}

	printf("200: %s %s\n", method, target);
}

int main (void) {
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = PORT_NBO(29952), /* "up" */
		.sin_addr = { IP_NBO(127, 0, 0, 1) },
		.sin_zero = { 0 },
	};

	int socket_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (socket_fd == -1)
		die("Failed to receive socket from OS");

	if (bind(socket_fd, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		die("Failed to bind socket");

	{
		int termination_signals[] = { SIGTERM, SIGINT, SIGQUIT, SIGHUP };
		for (int i = 0; i < 4; ++ i) {
			if (signal(termination_signals[i], cleanup) == SIG_IGN)
				signal(termination_signals[i], SIG_IGN);
		}
	}

	if (listen(socket_fd, 10) == -1)
		die("Failed to start listening");

	/* so the our signal handlers can close it and free up the port */
	connections[0] = socket_fd;

	int client_fd;
	while (1) {
		client_fd = accept(socket_fd, NULL, NULL);

		if (client_fd == -1) {
			perror("Failed to accept client");
			continue;
		}

		/* it's perfunctory but good enough etiquette that I care */
		connections[1] = client_fd;

		struct packet p;
		int error = 0;

		while (error != ConnectionTerminated) {
			memset(&p, 0, sizeof(p));
			error = get_packet(client_fd, &p);

			if (error) {
				switch(error) {
				case ConnectionTerminated:
					puts("*** Connection terminated ***");
					continue;
				case TooSmall:
					respond(client_fd, 400,
					        "Packet invalidly small");
					break;
				case InvalidMethod:
					respond(client_fd, 405,
					        "Unsupported HTTP method");
					break;
				case InvalidTarget:
					respond(client_fd, 404,
					        "Target unavailable");
					break;
				case MissingContentLength:
					respond(client_fd, 411,
					        "Content-Length header necessary but missing");
					break;
				case InvalidHeaderValue:
					respond(client_fd, 400,
					        "Content-Length not a valid number");
					break;
				case IncorrectContentLength:
					respond(client_fd, 400,
					        "Content-Length doesn't match body size");
					break;
				case EarlyEnding:
					respond(client_fd, 400,
					        "Packet invalidly truncated");
					break;
				}
			} else {
				char *target;

				switch (p.method) {
				case Get:
					target = target_packets[p.target];
					write(client_fd, target, strlen(target));
					ok(&p);
					break;
				case Put:
					break;
				}
			}
		}
		close(client_fd);
		connections[1] = 0;
	}
	close(socket_fd);
	connections[0] = 0;

	return 0;
}
