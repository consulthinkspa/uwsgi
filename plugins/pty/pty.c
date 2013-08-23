#include <uwsgi.h>
#include <pty.h>
#include <utmp.h>

extern struct uwsgi_server uwsgi;

struct uwsgi_pty_client {
	int fd;
	struct uwsgi_pty_client *prev;
	struct uwsgi_pty_client *next;
};

static struct uwsgi_pty {
	char *addr;
	char *remote;
	int queue;
	int server_fd;
	int master_fd;
	int slave_fd;
	int log;
	int original_log;
	int input;
	int original_input;
	int no_isig;
	struct uwsgi_pty_client *head;
	struct uwsgi_pty_client *tail;
} upty;

static struct uwsgi_pty_client *uwsgi_pty_client_new(int fd) {
	struct uwsgi_pty_client *upc = uwsgi_calloc(sizeof(struct uwsgi_pty_client));
	upc->fd = fd;
	if (upty.tail) {
		upc->prev = upty.tail;
		upty.tail->next = upc;
	}
	upty.tail = upc;
	if (!upty.head) upty.head = upc;
	return upc;
}

static void uwsgi_pty_client_remove(struct uwsgi_pty_client *upc) {
	struct uwsgi_pty_client *prev = upc->prev;
	struct uwsgi_pty_client *next = upc->next;

	if (prev) {
		prev->next = next;
	}

	if (next) {
		next->prev = prev;
	}

	if (upc == upty.head) {
		upty.head = next;
	}

	if (upc == upty.tail) {
		upty.tail = prev;
	}

	close(upc->fd);
	free(upc);
}

static struct uwsgi_option uwsgi_pty_options[] = {
	{"pty-socket", required_argument, 0, "bind the pty server on the specified address", uwsgi_opt_set_str, &upty.addr, 0},
	{"pty-log", no_argument, 0, "send stdout/stderr to the log engine too", uwsgi_opt_true, &upty.log, 0},
	{"pty-input", no_argument, 0, "read from original stdin in addition to pty", uwsgi_opt_true, &upty.input, 0},
	{"pty-connect", required_argument, 0, "connect the current terminal to a pty server", uwsgi_opt_set_str, &upty.remote, 0},
	{"pty-no-isig", no_argument, 0, "disable ISIG terminal attribute in client mode", uwsgi_opt_true, &upty.no_isig, 0},
	{0, 0, 0, 0, 0, 0, 0},
};

void uwsgi_pty_setterm(int fd) {
	struct termios tio;
        tcgetattr(fd, &tio);

        tio.c_iflag |= IGNPAR;
        tio.c_iflag &= ~(ISTRIP | IMAXBEL | BRKINT | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
#ifdef IUCLC
        tio.c_iflag &= ~IUCLC;
#endif
        tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	if (upty.no_isig) {
		tio.c_lflag &= ~(ISIG);
	}
#ifdef IEXTEN
        tio.c_lflag &= ~IEXTEN;
#endif
        tio.c_oflag &= ~OPOST;
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;

#ifdef B38400
	cfsetispeed(&tio, B38400);
	cfsetospeed(&tio, B38400);
#endif

        tcsetattr(fd, TCSANOW, &tio);
}

static void *uwsgi_pty_loop(void *arg) {

	/*
                if slave is ready there is something to send to the clients (and logs)

                if client is ready we have something to write to the master pty
        */

        for(;;) {
                char buf[8192];
                int interesting_fd = -1;
                int ret = event_queue_wait(upty.queue, -1, &interesting_fd);
                if (ret == 0) continue;
                if (ret < 0) continue;

		if (upty.input && interesting_fd == upty.original_input) {
			ssize_t rlen = read(upty.original_input, buf, 8192);
                        if (rlen <= 0) continue;
                        if (write(upty.master_fd, buf, rlen) != rlen) {
				// what to do ?
                        }
			continue;
		}

                if (interesting_fd == upty.master_fd) {
                        ssize_t rlen = read(upty.master_fd, buf, 8192);
                        if (rlen == 0) exit(1);
                        if (rlen < 0) {
                                uwsgi_error("uwsgi_pty_init()/read()");
                        }
			if (upty.log && upty.original_log >= 0) {
                        	if (write(upty.original_log, buf, rlen) != rlen) {
					// what to do ?
                        	}
			}
			struct uwsgi_pty_client *upc = upty.head;	
			while(upc) {
                        	if (write(upc->fd, buf, rlen) != rlen) {
					struct uwsgi_pty_client *tmp_upc = upc->next;
					uwsgi_pty_client_remove(upc);	
					upc = tmp_upc;
					continue;
				}
				upc = upc->next;
			}
                        continue;
                }

                if (interesting_fd == upty.server_fd) {
                        struct sockaddr_un client_src;
			memset(&client_src, 0, sizeof(struct sockaddr_un));
                        socklen_t client_src_len = 0;

                        int client_fd = accept(upty.server_fd, (struct sockaddr *) &client_src, &client_src_len);
                        if (client_fd < 0) {
                                uwsgi_error("accept()");
				continue;
                        }
			struct uwsgi_pty_client *upc = uwsgi_pty_client_new(client_fd);
                        event_queue_add_fd_read(upty.queue, upc->fd);
                        continue;
                }

		struct uwsgi_pty_client *upc = upty.head;
		while(upc) {
                	if (interesting_fd == upc->fd) {
                        	ssize_t rlen = read(upc->fd, buf, 8192);
                        	if (rlen <= 0) {
					uwsgi_pty_client_remove(upc);
					break;
                        	}
                        	if (write(upty.master_fd, buf, rlen) != rlen) {
				}
				break;
			}
			upc = upc->next;
                }

                continue;

        }

}

static void uwsgi_pty_init() {

	if (!upty.addr) return;

	char *tcp_port = strrchr(upty.addr, ':');
        if (tcp_port) {
        	// disable deferred accept for this socket
                int current_defer_accept = uwsgi.no_defer_accept;
                uwsgi.no_defer_accept = 1;
                upty.server_fd = bind_to_tcp(upty.addr, uwsgi.listen_queue, tcp_port);
                uwsgi.no_defer_accept = current_defer_accept;
	}
        else {
        	upty.server_fd = bind_to_unix(upty.addr, uwsgi.listen_queue, uwsgi.chmod_socket, uwsgi.abstract_socket);
        }

	if (upty.log) {
		upty.original_log = dup(1);
	}

	if (upty.input) {
		upty.original_input = dup(0);
	}

	if (openpty(&upty.master_fd, &upty.slave_fd, NULL, NULL, NULL)) {
		uwsgi_error("uwsgi_pty_init()/openpty()");
		exit(1);
	}


	uwsgi_log("pty server enabled on %s master: %d slave: %d\n", ttyname(upty.slave_fd), upty.master_fd, upty.slave_fd);

	upty.queue = event_queue_init();

	event_queue_add_fd_read(upty.queue, upty.master_fd);
	event_queue_add_fd_read(upty.queue, upty.server_fd);
	if (upty.input) {
		event_queue_add_fd_read(upty.queue, upty.original_input);
		uwsgi_pty_setterm(upty.original_input);
	}

	login_tty(upty.slave_fd);

	pthread_t t;
	pthread_create(&t, NULL, uwsgi_pty_loop, NULL);

}

static int uwsgi_pty_client() {
	if (!upty.remote) return 0;

	// save current terminal settings
	if (!tcgetattr(0, &uwsgi.termios)) {
        	uwsgi.restore_tc = 1;
        }

	upty.server_fd = uwsgi_connect(upty.remote, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], 0);
	if (upty.server_fd < 0) {
		uwsgi_error("uwsgi_pty_client()/connect()");
	}

	uwsgi_socket_nb(upty.server_fd);
	uwsgi_socket_nb(0);

	uwsgi_pty_setterm(0);

	upty.queue = event_queue_init();
	event_queue_add_fd_read(upty.queue, upty.server_fd);
	event_queue_add_fd_read(upty.queue, 0);

	for(;;) {
		char buf[8192];
		int interesting_fd = -1;
                int ret = event_queue_wait(upty.queue, -1, &interesting_fd);		
		if (ret <= 0) break;
		if (interesting_fd == 0) {
			ssize_t rlen = read(0, buf, 8192);
			if (rlen <= 0) break;
			if (write(upty.server_fd, buf, rlen) != rlen) break;
			continue;
		}	

		if (interesting_fd == upty.server_fd) {
			ssize_t rlen = read(upty.server_fd, buf, 8192);
                        if (rlen <= 0) break;
                        if (write(0, buf, rlen) != rlen) break;
                        continue;
		}
	}

	exit(0);
	// never here
	return 0;
}

struct uwsgi_plugin pty_plugin = {
	.name = "pty",
	.options = uwsgi_pty_options,
	.init = uwsgi_pty_client,
	.post_fork = uwsgi_pty_init,
};
