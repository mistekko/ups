#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT_NBO(HOST_BYTE_ORDER)	  \
	(HOST_BYTE_ORDER >> 8 | HOST_BYTE_ORDER << 8) & 0xffff
#define ADDR_NBO(MOST_SIGNIFICANT, SECOND, THIRD, LEAST) \
	MOST_SIGNIFICANT | SECOND << 8 | THIRD << 16 | LEAST << 24

const char output_separator[91] = "****************************************"
                                  "****************************************";

void die (const char *message) {
	perror(message);
	exit(1);
}

bool fd_is_readable (int fd) {
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN
	};
	poll(&pfd, 1, POLLIN);
	return pfd.revents & POLLIN;
}

int main (void) {
	struct sockaddr_in s_addr = {
		.sin_family = AF_INET,
		.sin_port   = PORT_NBO(29952),
		.sin_addr   = { ADDR_NBO(127, 0, 0, 1) },
		.sin_zero   = { 0 },
	};

	int server_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
		die("Failed to allocate socket");

	if (connect(server_fd, (struct sockaddr *)&s_addr, sizeof(s_addr)) == -1)
		die("Failed to connect to server");

	{
		DIR *test_packet_dir;
		struct dirent *packet_file;
		struct stat packet_stats;
		char *packet, *fname;
		int packet_fd;

		puts("Trying packets");
		puts(output_separator);

		if (chdir("./test-packets") == -1) {
			char *cwd = NULL;
			for (size_t cwd_capacity = 256;  ; cwd_capacity *= 2) {
				cwd = realloc(cwd, cwd_capacity);
				if (cwd == NULL)
					die("Out of memory");
				if (getcwd(cwd, cwd_capacity) != NULL)
					break;
			}
			fprintf(stderr, "Can't enter test-packets dir; looked in %s: ",
			        cwd);
			die(NULL);
		}
		test_packet_dir = opendir(".");
		if (test_packet_dir == NULL)
			die("Entered but couldn't open test-packet directory");

		errno = 0;
		while ((packet_file = readdir(test_packet_dir))) {
			fname = packet_file->d_name;

			if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0)
				/* it's okay to call close(3) on an invalid fd */
				goto cleanup_and_continue;

			printf("%25s... ", fname);
			packet_fd = open(fname, O_RDONLY);
			if (packet_fd == -1) {
				fprintf(stderr, "Failed to open %s: ", fname);
				die(NULL);
			}
			stat(fname, &packet_stats);

			{
				ssize_t packetlen = packet_stats.st_size;
				packet = mmap(NULL, packetlen + 1, PROT_WRITE|PROT_READ,
				              MAP_PRIVATE, packet_fd, 0);
				packet[packetlen] = '\0';

				if (write(server_fd, packet, packetlen) != packetlen) {
					puts("failed to write full packet");
					goto cleanup_and_continue;
				}
			}

			{
				char response_buf[512], *response = response_buf;
				ssize_t response_size;

				response_size = read(server_fd, response, 511);
				if (response_size == -1) {
					close(packet_fd);
					shutdown(server_fd, SHUT_RDWR);
					die("Failed to receive response from server");
				} else if (response_size < 12) {
					puts("response too small to be valid");
					goto cleanup_and_continue;
				}
				response[response_size] = '\0';

				if (!isdigit(response[9])) {
					puts("invalid response");
					goto cleanup_and_continue;
				} else if (response[9] == '2') {
					puts("200");
				} else if (response[9] == '4') {
					printf("%.*s: ", 3, response + 9);
					if (response_size < 14) {
						puts("server supplied no reason");
						goto flush_packet;
					}

					response += 13;

					char *newline;
					while (1) {
						if (NULL != (newline = strchr(response, '\r'))) {
							*newline = '\0';
							puts(response);
							break;
						}
						printf("%s", response);

						if (fd_is_readable(server_fd)) {
							response_size = read(server_fd, response, 498);
							if (response_size == -1) {
								puts("lost contact with server");
								goto cleanup_and_continue;
							}
							response[response_size] = '\0';
						} else {
							puts("response packet is invalidly truncated");
							goto cleanup_and_continue;
						}
					}
				}
			flush_packet:
				while (fd_is_readable(server_fd))
					read(server_fd, response, 512);
			}
		cleanup_and_continue:
			close(packet_fd);
			errno = 0;
		}
		if (errno != 0)
			perror("Error while getting test packets");

		puts(output_separator);
	}


	/* cleanup */
	{
		close(server_fd);
	}

	return 0;
}
