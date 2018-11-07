#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXNAME 80  /* maximum permitted name size, not including \0 */
#define NPITS 6  /* number of pits on a side, not including the end pit */
#define NPEBBLES 4 /* initial number of pebbles per pit */
#define MAXMESSAGE (MAXNAME + 50) /* initial number of pebbles per pit */

int port = 3000;
int listenfd;

struct player {
    int fd;
    char name[MAXNAME+1]; 
    int pits[NPITS+1];  // pits[0..NPITS-1] are the regular pits 
                        // pits[NPITS] is the end pit
    int prompted; /* 1 if player already prompted 0 otherwise */
    struct player *next;
};
struct player *playerlist = NULL;
struct player *curr_p = NULL; /* current player */


extern void parseargs(int argc, char **argv);
extern void makelistener();
extern int compute_average_pebbles();
extern int game_is_over();  /* boolean */
extern void broadcast(char *s);  /* you need to write this one */

/* --- my additional functions ---- */

/* send string to player */
void send_all(int fd, char *buf, size_t len);
/* recv string from player */
int recv_all(int fd, char *buf, size_t len);
/* fill up the fd_sets */
int fill_sel_lst(fd_set* readfds, fd_set* writefds);
/* accept new player (name not yet specified) */
void acc_player();
/* check player when they sends something */
void chk_player(struct player *p);
/* get current player */
struct player *curr_player();
/* delete player */
void del_player(struct player *p);
/* add new player */
void add_player(struct player *p, const char *name);
/* display status */
void display_status(int fd);
/* player makes a move */
void mov_player(struct player *p, const char *mov);
/* get next player after the given one */
struct player *next_player(const struct player *p);
/* broadcast to every player except the given one */
void broadcast_except(char *s, int fd);
/* prompt player to make a move */
void pro_player(struct player *p);
/* trim string from both ends */
void strtrim(char *buf);
/* free heap memory allocated for players */
void free_list();

int main(int argc, char **argv) {
    char msg[MAXMESSAGE];
    fd_set readfds;  /* read fd_set */
    fd_set writefds; /* write fd_set */

    parseargs(argc, argv);
    makelistener();

    while (!game_is_over()) {
        /* fill up fd_sets and return (max fd plus one) */
        int nfds = fill_sel_lst(&readfds, &writefds);
        /* select from the fd_sets */
        int rc = select(nfds, &readfds, &writefds, NULL, NULL);
        if (rc == -1) {
            perror("select");
            exit(1);
        }
        /* accept a new player when listenfd is selected */
        if (FD_ISSET(listenfd, &readfds)) {
            acc_player();
        }
        /* check existing player if it's fd is selected */
        for (struct player *p = playerlist; p; p = p->next) {
            if (FD_ISSET(p->fd, &readfds)) {
                chk_player(p);
            }
        }
        /* prompt current player to make a move if it's fd is selected */
        if (curr_p != NULL && FD_ISSET(curr_p->fd, &writefds)) {
            pro_player(curr_p);
        }
    }

    broadcast("Game over!\r\n");
    printf("Game over!\n");
    for (struct player *p = playerlist; p; p = p->next) {
        int points = 0;
        for (int i = 0; i <= NPITS; i++) {
            points += p->pits[i];
        }
        printf("%s has %d points\r\n", p->name, points);
        snprintf(msg, MAXMESSAGE, "%s has %d points\r\n", p->name, points);
        broadcast(msg);
    }

    free_list();

    return 0;
}


void parseargs(int argc, char **argv) {
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);  
            break;
        default:
            status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}


void makelistener() {
    struct sockaddr_in r;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
               (const char *) &on, sizeof(on)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&r, sizeof(r))) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
}



/* call this BEFORE linking the new player in to the list */
int compute_average_pebbles() { 
    struct player *p;
    int i;

    if (playerlist == NULL) {
        return NPEBBLES;
    }

    int nplayers = 0, npebbles = 0;
    for (p = playerlist; p; p = p->next) {
        nplayers++;
        for (i = 0; i < NPITS; i++) {
            npebbles += p->pits[i];
        }
    }
    return ((npebbles - 1) / nplayers / NPITS + 1);  /* round up */
}


int game_is_over() { /* boolean */
    int i;

    if (!playerlist) {
       return 0;  /* we haven't even started yet! */
    }

    for (struct player *p = playerlist; p; p = p->next) {
        int is_all_empty = 1;
        for (i = 0; i < NPITS; i++) {
            if (p->pits[i]) {
                is_all_empty = 0;
            }
        }
        if (is_all_empty) {
            return 1;
        }
    }
    return 0;
}

void broadcast(char *s) {
    struct player *p; /* player */

    for (p = playerlist; p; p = p->next) {
        /* only send to players with name specified */
        if (strlen(p->name) != 0) {
            send_all(p->fd, s, strlen(s));
        }
    }
}

void send_all(int fd, char *buf, size_t len) {
    size_t inbuf = 0; /* bytes in buf */
    ssize_t nbytes; /* bytes sent */

    /* keep sending until all message is delivered */
    while (inbuf < len) {
        nbytes = send(fd, buf, len - inbuf, 0);
        if (nbytes == -1) {
            perror("send");
            exit(1);
        }
        inbuf += nbytes;
        buf += nbytes;
    }
}

int recv_all(int fd, char *buf, size_t len) {
    size_t inbuf = 0; /* bytes in buf */
    char* after = buf; /* buf pointer */
    int room = len; /* room left in buf */
    int find_newline = 0; /* 1 if found newline 0 otherwise */
    int i; /* loop variable */
    ssize_t nbytes; /* bytes recieved */

    /* keep receiving until encountering a newline */
    while (find_newline == 0) {
        nbytes = recv(fd, after, room, 0);
        if (nbytes == 0) {
            /* the client closed connection */
            return 0;
        }
        if (nbytes == -1) {
            perror("recv");
            exit(1);
        }
        room -= nbytes;
        inbuf += nbytes;
        if (room <= 0) {
            /* the incoming message is too long */
            return -1;
        }

        /* try finding a newline, one of the '\r', '\n' and '\r\n' */
        for (i = 0; i < nbytes; ++i) {
            if (after[i] == '\n' || after[i] == '\r') {
                after[i] = '\0';
                find_newline = 1;
                break;
            }
        }
        after += nbytes;
    }

    /* trim message */
    strtrim(buf);

    return 1;
}

int fill_sel_lst(fd_set* readfds, fd_set* writefds) {
    int hfd = listenfd; /* max fd */
    struct player *p; /* player */

    /* set up write fd_set */
    /* it contains current player's fd that's not been prompted to move */
    FD_ZERO(writefds);
    if (curr_p != NULL && strlen(curr_p->name) != 0 && curr_p->prompted == 0) {
        FD_SET(curr_p->fd, writefds);
    }

    /* set up read fd_set */
    /* it contains all players' fds and listenfd */
    FD_ZERO(readfds);
    FD_SET(listenfd, readfds);
    for (p = playerlist; p; p = p->next) {
        FD_SET(p->fd, readfds);
        if (p->fd > hfd) {
            hfd = p->fd;
        }
    }

    return hfd + 1;
}

void acc_player() {
    char msg[MAXMESSAGE];
    int fd; /* fd */
    int i; /* loop variable */
    int pebbles; /* nubmer of pebbles */
    struct player *p; /* player */

    /* accept connection */
    fd = accept(listenfd, (struct sockaddr*)NULL, NULL);
    if (fd == -1) {
        perror("accept");
        exit(1);
    }
    printf("incoming connection (%d).\n", fd);

    /* send welcome message */
    snprintf(msg, MAXMESSAGE, "Welcome to Mancala. What is your name?\r\n");
    send_all(fd, msg, strlen(msg));

    /* create a new player */
    p = (struct player*)calloc(1, sizeof(struct player));
    p->fd = fd; 
    /* compute average pebbles */
    pebbles = compute_average_pebbles();
    /* set pebbles */
    for (i = 0; i < NPITS; ++i) {
        p->pits[i] = pebbles;
    }
    /* add the new player to list */
    p->next = playerlist;
    playerlist = p;
}

void chk_player(struct player *p) {
    char msg[MAXMESSAGE];
    int rc; /* return value of recv_all() */

    if (strlen(p->name) == 0) {
        /* name not specified */
        /* expect a name input */
        rc = recv_all(p->fd, msg, MAXNAME + 1);
        if (rc < 1) {
            if (rc == -1) {
                printf("message is too long.\n");
            }
            /* delete player */
            del_player(p);
            return;
        }
        add_player(p, msg);
    } else {
        /* name specified */
        /* expect a move input */
        rc = recv_all(p->fd, msg, MAXMESSAGE);
        if (rc < 1) {
            if (rc == -1) {
                printf("message is too long.\n"); 
            }
            /* delete player */
            del_player(p);
            return;
        }
        mov_player(p, msg);
    }
}

struct player *curr_player() {
    struct player *p = curr_p; /* player */

    if (curr_p == NULL) {
        p = playerlist;
        while (p != NULL) {
            /* only count player with name specified as valid player */
            if (strlen(p->name) != 0) {
                return p;
            }
            p = p->next;
        }
    }

    return curr_p;
}

void del_player(struct player *p) {
    char msg[MAXMESSAGE];
    struct player *prev_p; /* previous player */
    int fd = p->fd; /* fd */

    if (strlen(p->name) != 0) {
        /* name specfied */
        printf("%s has left the game.\n", p->name);
        snprintf(msg, MAXMESSAGE, "%s has left the game.\r\n", p->name);
        broadcast_except(msg, fd);
    } else {
        /* name not specified, player not yet added to game */
        printf("connection (%d) disconnected.\n", p->fd);
    }
    
    /* update current player */
    if (p == curr_p) {
        curr_p = p->next;
    }
    /* remove player from list */
    if (p == playerlist) {
        playerlist = p->next;
    } else {
        prev_p = playerlist;
        while (prev_p->next != NULL) {
            if (prev_p->next == p) {
                prev_p->next = p->next;
                break;
            }
            prev_p = prev_p->next;
        }
    }
    /* update current player */
    curr_p = curr_player();

    free(p);
    close(fd);
}

void add_player(struct player *p, const char *name) {
    char msg[MAXMESSAGE];
    int fd = p->fd; /* fd */
    struct player *other_p;

    if (strlen(name) == 0) {
        /* an empty name */
        snprintf(msg, MAXMESSAGE, "Empty name, try again?\r\n");
        send_all(fd, msg, strlen(msg));
        return;
    } else {
        /* check if duplicate name */
        for (other_p = playerlist; other_p; other_p = other_p->next) {
            if (strncmp(other_p->name, name, MAXNAME) == 0) {
                snprintf(msg, MAXMESSAGE, "Duplicate name, try again?\r\n");
                send_all(fd, msg, strlen(msg));
                return;
            }
        }
    }

    /* a proper name */
    strncpy(p->name, name, MAXNAME);
    /* broadcast the 'new player joined' message */
    printf("%s has joined the game.\n", name);
    snprintf(msg, MAXMESSAGE, "%s has joined the game.\r\n", name);
    broadcast(msg);
    /* show current game status to the new player */
    display_status(fd);
    curr_p = curr_player();
    if (curr_p != p) {
        snprintf(msg, MAXMESSAGE, "now it's %s's turn.\r\n", curr_p->name);
        send_all(fd, msg, strlen(msg));
    }
}

void display_status(int fd) {
    char msg[MAXMESSAGE];
    char *ptr; /* message pointer */
    int len = MAXMESSAGE; /* length of message */
    struct player *p; /* player */

    for (p = playerlist; p; p = p->next) {
        /* skip player with name not specified */
        if (strlen(p->name) == 0) {
            continue;
        }
        snprintf(msg, len, "%s: ", p->name);
        ptr = msg + strlen(msg);
        len = MAXMESSAGE - strlen(msg);
        for (int i = 0; i < NPITS; ++i) {
            snprintf(ptr, len, "[%d]%d ", i, p->pits[i]);
            ptr = msg + strlen(msg);
            len = MAXMESSAGE - strlen(msg);
        }
        snprintf(ptr, len, "[end pit]%d\r\n", p->pits[NPITS]);
        if (fd == -1) {
            /* send message to every player */
            broadcast(msg);
        } else {
            /* send messge to the given player */
            send_all(fd, msg, strlen(msg));
        }
    }
}

void mov_player(struct player *p, const char *mov) {
    char msg[MAXMESSAGE];
    int i = strtol(mov, NULL, 10); /* index of pit */
    int pebbles; /* number of pebbles */
    int bonus; /* 1 if the last distributed pebble dropped into current player's end pit 0 otherwise */
    int imax; /* limit of index of pit */

    if (p != curr_p) {
        /* not current player */
        snprintf(msg, MAXMESSAGE, "It's not your move.\r\n");
        send_all(p->fd, msg, strlen(msg));
        return;
    }

    if (i < 0 || i >= NPITS || p->pits[i] == 0) {
        /* invalid move */
        snprintf(msg, MAXMESSAGE, "Invalid move, try again?\r\n");
        send_all(p->fd, msg, strlen(msg));
        return;
    }

    /* a proper move */
    printf("%s's move is %d\n", p->name, i);
    snprintf(msg, MAXMESSAGE, "%s's move is %d\r\n", p->name, i);
    /* broadcast except current player */
    broadcast_except(msg, p->fd);

    /* update pebbles of players */
    /* clear prompted */
    p->prompted = 0;
    pebbles = p->pits[i];
    p->pits[i] = 0;
    bonus = pebbles == NPITS - i ? 1 : 0;
    imax = NPITS;
    while (pebbles != 0) {
        if (i < imax) {
            ++i;
            p->pits[i] += 1;
            --pebbles;
        } else {
            /* update imax so pebbles will skip other players's end pits */
            imax = NPITS - 1;
            i = -1;
            /* find next player */
            p = next_player(p);
        }
    }
    /* show status to every player */
    display_status(-1);
    /* move again if get bonus */
    if (bonus == 0) {
        /* find next current player */
        curr_p = next_player(curr_p);
    }
}

struct player *next_player(const struct player *p) {
    if (p == NULL) {
        return playerlist;
    }
    return p->next == NULL ? playerlist : p->next;
}

void broadcast_except(char *s, int fd) {
    struct player *p; /* player */

    for (p = playerlist; p; p = p->next) {
        /* only send to players with name specified */
        if (fd != p->fd && strlen(p->name) != 0) {
            send_all(p->fd, s, strlen(s));
        }
    }
}

void pro_player(struct player *p) {
    char msg[MAXMESSAGE];

    /* set prompted */
    p->prompted = 1;

    /* tell other players it's current player's move */
    snprintf(msg, MAXMESSAGE, "It is %s's move.\r\n", p->name);
    broadcast_except(msg, p->fd);
    /* ask current player what your move will be? */
    snprintf(msg, MAXMESSAGE, "Your move?\r\n");
    send_all(p->fd, msg, strlen(msg));
}

void strtrim(char *buf) {
    char *after; /* buf pointer */
    int len = strlen(buf); /* buf content length */

    if (len != 0) {
        /* trim tailing spaces */
        while (isspace(buf[len - 1])) {
            buf[--len] = '\0';
        }
        /* trim leading spaces */
        after = buf;
        while (*after && isspace(*after)) {
            ++after;
            --len;
        }
        memmove(buf, after, len + 1);
    }
}

/* free heap memory allocated for players */
void free_list() {
    struct player *p; /* player */
    struct player *prev_p; /* previous player */

    /* free all players */
    for (p = playerlist; p; ) {
        prev_p = p;
        p = p->next;
        free(prev_p);
    }
}
