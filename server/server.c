#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "my_signal.h"
#include "my_socket.h"
#include "get_num.h"
#include "bz_usleep.h"
#include "logUtil.h"
#include "set_cpu.h"

int debug = 0;
int enable_quick_ack = 0;
int set_so_sndbuf_size = 0;
volatile sig_atomic_t has_usr1 = 0;

int print_result(struct timeval start, struct timeval stop, int so_snd_buf, unsigned long long send_bytes)
{
    struct timeval diff;
    double elapse;

    timersub(&stop, &start, &diff);
    fprintfwt(stderr, "server: SO_SNDBUF: %d (final)\n", so_snd_buf);
    elapse = diff.tv_sec + 0.000001*diff.tv_usec;
    fprintfwt(stderr, "server: %.3f MB/s ( %lld bytes %ld.%06ld sec )\n",
        (double) send_bytes / elapse  / 1024.0 / 1024.0,
        send_bytes, diff.tv_sec, diff.tv_usec);

    return 0;
}

void sig_usr1()
{
    has_usr1 = 1;
    return;
}

int child_proc(int connfd, int bufsize, int sleep_usec, long rate, int sleep_to_resume_sec, int run_cpu, int use_no_delay)
{
    int n;
    unsigned char *buf;
    struct timeval start, stop;
    unsigned long long send_bytes = 0;

    buf = malloc(bufsize);
    if (buf == NULL) {
        err(1, "malloc sender buf in child_proc");
    }

    if (set_so_sndbuf_size > 0) {
        set_so_sndbuf(connfd, set_so_sndbuf_size);
    }

    if (use_no_delay) {
        fprintfwt(stderr, "use_no_delay\n");
        set_so_nodelay(connfd);
    }

    pid_t pid = getpid();
    fprintfwt(stderr, "server: pid: %d\n", pid);
    int so_snd_buf = get_so_sndbuf(connfd);
    fprintfwt(stderr, "server: SO_SNDBUF: %d (init)\n", so_snd_buf);

    if (run_cpu != -1) {
        if (set_cpu(run_cpu) < 0) {
            errx(1, "set_cpu");
        }
    }

    my_signal(SIGUSR1, sig_usr1);
    gettimeofday(&start, NULL);
    for ( ; ; ) {
        if (has_usr1) {
            has_usr1 = 0;
            sleep(sleep_to_resume_sec);
            if (rate > 0) {
                /*
                 * XXX 
                 * reset start time and send_bytes to send
                 * in rate after resume.
                 * if rate is specified and data sending suspend,
                 * the final result in bytes and transfer rate
                 * will be invalid values
                 */
                gettimeofday(&start, NULL);
                send_bytes = 0;
            }
        }
        if (enable_quick_ack) {
#ifdef __linux__
            int qack = 1;
            setsockopt(connfd, IPPROTO_TCP, TCP_QUICKACK, &qack, sizeof(qack));
#endif
        }
        n = write(connfd, buf, bufsize);
        if (n < 0) {
            gettimeofday(&stop, NULL);
            int so_snd_buf = get_so_sndbuf(connfd);
            print_result(start, stop, so_snd_buf, send_bytes);
            if (errno == ECONNRESET) {
                fprintfwt(stderr, "server: connection reset by client\n");
                break;
            }
            else if (errno == EPIPE) {
                fprintfwt(stderr, "server: connection reset by client\n");
                break;
            }
            else {
                err(1, "write");
            }
        }
        else if (n == 0) {
            warnx("write returns 0");
            exit(0);
        }
        else {
            send_bytes += n;
        }
        if (sleep_usec > 0) {
            bz_usleep(sleep_usec);
        }
        if (rate > 0) {
            struct timeval now, elapsed;
            // calculate sleep sec
            gettimeofday(&now, NULL);
            timersub(&now, &start, &elapsed);
            double delta_t_sec = (double) send_bytes / (double) rate - (double) elapsed.tv_sec - (double) 0.000001*elapsed.tv_usec ;
            if (delta_t_sec > 0.0) {
                int sleep_usec = (int) (delta_t_sec * 1000000.0);
                usleep(sleep_usec);
            }
        }
    }
    return 0;
}

void sig_chld(int signo)
{
    pid_t pid;
    int   stat;

    while ( (pid = waitpid(-1, &stat, WNOHANG)) > 0) {
        ;
    }
    return;
}

int usage(void)
{
    char *msg =
"Usage: server [-b bufsize (1460)] [-s sleep_usec (0)] [-q] [-r rate] [-S so_sndbuf]\n"
"-b bufsize:    one send size (may add k for kilo, m for mega)\n"
"-s sleep_usec: sleep useconds after write\n"
"-p port:       port number (1234)\n"
"-q:            enable quick ack\n"
"-r rate:       data send rate (bytes/sec).  k for kilo, m for mega\n"
"-B rate:       data send rate (bits/sec).  k for kilo, m for mega\n"
"-S so_sndbuf:  set socket send buffer size\n"
"-R sleep_sec:  sleep_sec after receive SIGUSR1\n"
"-c run_cpu:    specify server run cpu (in child proc)\n"
"-D:            use TCP_NODELAY socket option\n";

    fprintf(stderr, "%s", msg);

    return 0;
}

int main(int argc, char *argv[])
{
    int port = 1234;
    pid_t pid;
    struct sockaddr_in remote;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    int listenfd;
    int c;
    int bufsize = 1460;
    int sleep_usec = 0;
    long rate = 0;
    int sleep_to_resume_sec = 5;
    int run_cpu = -1;
    int use_no_delay = 0;

    while ( (c = getopt(argc, argv, "b:B:c:dDhp:qr:R:s:S:")) != -1) {
        switch (c) {
            case 'b':
                bufsize = get_num(optarg);
                break;
            case 'B':
                rate = get_num_10(optarg);
                rate = rate / 8;
                break;
            case 'c':
                run_cpu = strtol(optarg, NULL, 0);
                break;
            case 'd':
                debug += 1;
                break;
            case 'D':
                use_no_delay = 1;
                break;
            case 'h':
                usage();
                exit(0);
            case 'p':
                port = strtol(optarg, NULL, 0);
                break;
            case 'q':
                enable_quick_ack = 1;
                break;
            case 'r':
                rate = get_num(optarg);
                break;
            case 's':
                sleep_usec = get_num(optarg);
                break;
            case 'S':
                set_so_sndbuf_size = get_num(optarg);
                break;
            case 'R':
                sleep_to_resume_sec = strtol(optarg, NULL, 0);
                break;
            default:
                break;
        }
    }
    argc -= optind;
    argv += optind;

    my_signal(SIGCHLD, sig_chld);
    my_signal(SIGPIPE, SIG_IGN);

    listenfd = tcp_listen(port);
    if (listenfd < 0) {
        errx(1, "tcp_listen");
    }

    for ( ; ; ) {
        int connfd = accept(listenfd, (struct sockaddr *)&remote, &addr_len);
        if (connfd < 0) {
            err(1, "accept");
        }
        
        pid = fork();
        if (pid == 0) { //child
            if (close(listenfd) < 0) {
                err(1, "close listenfd");
            }
            if (child_proc(connfd, bufsize, sleep_usec, rate, sleep_to_resume_sec, run_cpu, use_no_delay) < 0) {
                errx(1, "child_proc");
            }
            exit(0);
        }
        else { // parent
            if (close(connfd) < 0) {
                err(1, "close for accept socket of parent");
            }
        }
    }
        
    return 0;
}
