/*
 * radmin.c	RADIUS Administration tool.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2008   The FreeRADIUS server project
 * Copyright 2008   Alan DeKok <aland@deployingradius.com>
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/radpaths.h>

#ifdef HAVE_READLINE_READLINE_H
#include <readline/readline.h>
#include <readline/history.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

/*
 *	For configuration file stuff.
 */
const char *radius_dir = RADDBDIR;
const char *progname = "radmin";

/*
 *	The rest of this is because the conffile.c, etc. assume
 *	they're running inside of the server.  And we don't (yet)
 *	have a "libfreeradius-server", or "libfreeradius-util".
 */
int debug_flag = 0;
struct main_config_t mainconfig;
char *request_log_file = NULL;
char *debug_log_file = NULL;
int radius_xlat(UNUSED char *out, UNUSED int outlen, UNUSED const char *fmt,
		UNUSED REQUEST *request, UNUSED RADIUS_ESCAPE_STRING func)
{
	return -1;
}

static int fr_domain_socket(const char *path)
{
        int sockfd;
	size_t len;
	socklen_t socklen;
        struct sockaddr_un saremote;

	len = strlen(path);
	if (len >= sizeof(saremote.sun_path)) {
		fprintf(stderr, "%s: Path too long in filename\n", progname);
		return -1;
	}

        if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "%s: Failed creating socket: %s\n",
			progname, strerror(errno));
		return -1;
        }

        saremote.sun_family = AF_UNIX;
	memcpy(saremote.sun_path, path, len); /* not zero terminated */
	
	socklen = sizeof(saremote.sun_family) + len;

        if (connect(sockfd, (struct sockaddr *)&saremote, socklen) < 0) {
		fprintf(stderr, "%s: Failed connecting to %s: %s\n",
			progname, path, strerror(errno));
		close(sockfd);
		return -1;
        }

#ifdef O_NONBLOCK
	{
		int flags;
		
		if ((flags = fcntl(sockfd, F_GETFL, NULL)) < 0)  {
			fprintf(stderr, "%s: Failure getting socket flags: %s",
				progname, strerror(errno));
			close(sockfd);
			return -1;
		}
		
		flags |= O_NONBLOCK;
		if( fcntl(sockfd, F_SETFL, flags) < 0) {
			fprintf(stderr, "%s: Failure setting socket flags: %s",
				progname, strerror(errno));
			close(sockfd);
			return -1;
		}
	}
#endif

	return sockfd;
}

static int usage(void)
{
	printf("Usage: %s [ -d raddb_dir ] [ -f socket_file ] [ -n name ] [ -q ]\n", progname);
	printf("  -d raddb_dir    Configuration files are in \"raddbdir/*\".\n");
	printf("  -f socket_file  Open socket_file directly, without reading radius.conf\n");
	printf("  -n name         Read raddb/name.conf instead of raddb/radiusd.conf\n");
	printf("  -q              Quiet mode.\n");

	exit(1);
}

static ssize_t run_command(int sockfd, const char *command,
			   char *buffer, size_t bufsize)
{
	char *p;
	ssize_t size, len;
	int flag = 1;

	/*
	 *	Write the text to the socket.
	 */
	if (write(sockfd, command, strlen(command)) < 0) return -1;
	if (write(sockfd, "\r\n", 2) < 0) return -1;

	/*
	 *	Read the response
	 */
	size = 0;
	buffer[0] = '\0';

	memset(buffer, 0, bufsize);

	while (flag == 1) {
		int rcode;
		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(sockfd, &readfds);

		rcode = select(sockfd + 1, &readfds, NULL, NULL, NULL);
		if (rcode < 0) {
			if (errno == EINTR) continue;

			fprintf(stderr, "%s: Failed selecting: %s\n",
				progname, strerror(errno));
			exit(1);
		}

		len = recv(sockfd, buffer + size,
			   bufsize - size - 1, MSG_DONTWAIT);
		if (len < 0) {
			/*
			 *	No data: keep looping
			 */
			if ((errno == EAGAIN) || (errno == EINTR)) {
				continue;
			}

			fprintf(stderr, "%s: Error reading socket: %s\n",
				progname, strerror(errno));
			exit(1);
		}
		if (len == 0) return 0;	/* clean exit */

		size += len;
		buffer[size] = '\0';

		/*
		 *	There really is a better way of doing this.
		 */
		p = strstr(buffer, "radmin> ");
		if (p &&
		    ((p == buffer) || 
		     (p[-1] == '\n') ||
		     (p[-1] == '\r'))) {
			*p = '\0';

			if (p[-1] == '\n') p[-1] = '\0';

			flag = 0;
			break;
		}
	}

	/*
	 *	Blank prompt.  Go get another command.
	 */
	if (!buffer[0]) return 1;

	buffer[size] = '\0'; /* this is at least right */

	return 2;
}


int main(int argc, char **argv)
{
	int argval, quiet = 0;
	int done_license = 0;
	int sockfd;
	uint32_t magic;
	char *line = NULL;
	ssize_t len, size;
	const char *file = NULL;
	const char *name = "radiusd";
	char *p, buffer[2048];

	if ((progname = strrchr(argv[0], FR_DIR_SEP)) == NULL)
		progname = argv[0];
	else
		progname++;

	while ((argval = getopt(argc, argv, "d:he:f:n:q")) != EOF) {
		switch(argval) {
		case 'd':
			if (file) {
				fprintf(stderr, "%s: -d and -f cannot be used together.\n", progname);
				exit(1);
			}
			radius_dir = optarg;
			break;

		case 'e':
			line = optarg;
			break;

		case 'f':
			radius_dir = NULL;
			file = optarg;
			break;

		default:
		case 'h':
			usage();
			break;

		case 'n':
			name = optarg;
			break;

		case 'q':
			quiet = 1;
			break;
		}
	}

	if (radius_dir) {
		int rcode;
		CONF_SECTION *cs, *subcs;

		file = NULL;	/* MUST read it from the conffile now */

		snprintf(buffer, sizeof(buffer), "%s/%s.conf",
			 radius_dir, name);

		cs = cf_file_read(buffer);
		if (!cs) {
			fprintf(stderr, "%s: Errors reading %s\n",
				progname, buffer);
			exit(1);
		}

		subcs = NULL;
		while ((subcs = cf_subsection_find_next(cs, subcs, "listen")) != NULL) {
			const char *value;
			CONF_PAIR *cp = cf_pair_find(subcs, "type");
			
			if (!cp) continue;

			value = cf_pair_value(cp);
			if (!value) continue;

			if (strcmp(value, "control") != 0) continue;

			/*
			 *	Now find the socket name (sigh)
			 */
			rcode = cf_item_parse(subcs, "socket",
					      PW_TYPE_STRING_PTR,
					      &file, NULL);
			if (rcode < 0) {
				fprintf(stderr, "%s: Failed parsing listen section\n", progname);
				exit(1);
			}

			if (!file) {
				fprintf(stderr, "%s: No path given for socket\n",
					progname);
				exit(1);
			}
			break;
		}

		if (!file) {
			fprintf(stderr, "%s: Could not find control socket in %s\n",
				progname, buffer);
			exit(1);
		}
	}

	if (!isatty(STDIN_FILENO)) quiet = 1;

#ifdef HAVE_READLINE_READLINE_H
	if (!quiet) {
		using_history();
		rl_bind_key('\t', rl_insert);
	}
#endif

 reconnect:
	/*
	 *	FIXME: Get destination from command line, if possible?
	 */
	sockfd = fr_domain_socket(file);
	if (sockfd < 0) {
		exit(1);
	}

	/*
	 *	Read initial magic && version information.
	 */
	for (size = 0; size < 8; size += len) {
		len = read(sockfd, buffer + size, 8 - size);
		if (len < 0) {
			fprintf(stderr, "%s: Error reading initial data from socket: %s\n",
				progname, strerror(errno));
			exit(1);
		}
	}

	memcpy(&magic, buffer, 4);
	magic = ntohl(magic);
	if (magic != 0xf7eead15) {
		fprintf(stderr, "%s: Socket %s is not FreeRADIUS administration socket\n", progname, file);
		exit(1);
	}
	
	memcpy(&magic, buffer + 4, 4);
	magic = ntohl(magic);
	if (magic != 1) {
		fprintf(stderr, "%s: Socket version mismatch: Need 1, got %d\n",
			progname, magic);
		exit(1);
	}	

	/*
	 *	Run one command.
	 */
	if (line) {
		size = run_command(sockfd, line, buffer, sizeof(buffer));
		if (size < 0) exit(1);
		if ((size == 0) || (size == 1)) exit(0);

		puts(buffer);
		fflush(stdout);
		exit(0);
	}

	if (!done_license && !quiet) {
		printf("radmin " RADIUSD_VERSION " - FreeRADIUS Server administration tool.\n");
		printf("Copyright (C) 2008 The FreeRADIUS server project and contributors.\n");
		printf("There is NO warranty; not even for MERCHANTABILITY or FITNESS FOR A\n");
		printf("PARTICULAR PURPOSE.\n");
		printf("You may redistribute copies of FreeRADIUS under the terms of the\n");
		printf("GNU General Public License v2.\n");

		done_license = 1;
	}

	/*
	 *	FIXME: Do login?
	 */

	while (1) {
		if (!quiet) {
#ifndef HAVE_READLINE_READLINE_H
			printf("radmin> ");
			fflush(stdout);
#else
			line = readline("radmin> ");
			
			if (!line) break;
			
			if (!*line) {
				free(line);
				continue;
			}
			
			add_history(line);
#endif
		} else {	/* quiet, or no readline */
			
			line = fgets(buffer, sizeof(buffer), stdin);
			if (!line) break;
			
			p = strchr(buffer, '\n');
			if (!p) {
				fprintf(stderr, "%s: Input line too long\n",
					progname);
				exit(1);
			}
			
			*p = '\0';
		}

		if (strcmp(line, "reconnect") == 0) {
			close(sockfd);
			goto reconnect;
		}

		/*
		 *	Exit, done, etc.
		 */
		if ((strcmp(line, "exit") == 0) ||
		    (strcmp(line, "quit") == 0)) {
			break;
		}

		size = run_command(sockfd, line, buffer, sizeof(buffer));
		if (size <= 0) break; /* error, or clean exit */

		if (size == 1) continue; /* no output. */

		puts(buffer);
		fflush(stdout);
	}

	printf("\n");

	return 0;
}
