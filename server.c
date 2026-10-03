/* Theatre Seat Allocation - DSA: Linked List (C backend + web frontend)
 *
 *   hall     = linked list of Rows,    each Row     = linked list of Seats
 *   bookings = linked list of Bookings, each Booking = linked list of Tickets
 *
 * Features: tiered pricing, booking several people in ONE atomic request,
 * cancelling a single ticket or a whole booking (with a cancellation fee).
 * Minimal HTTP server using POSIX sockets, no external libraries. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define ROWS      6
#define COLS      10
#define MAX_GROUP 10      /* most tickets in one booking */
#define FEE_PCT   10      /* cancellation fee, percent of ticket price */
#define NAME_LEN  32
#define MSG_LEN   640
#define SHOW_BOOKINGS 30  /* newest bookings sent to the client */
#define RUPEE "\xe2\x82\xb9"

static const char *TIER_NAME[]  = { "Premium", "Standard", "Economy" };
static const int   TIER_PRICE[] = { 500, 350, 200 };
static int tier_of_row(int i) { return i < 2 ? 0 : i < 4 ? 1 : 2; }   /* A-B, C-D, E-F */

typedef struct Seat    { int num; int booked; int booking; char name[NAME_LEN]; struct Seat *next; } Seat;
typedef struct Row     { char label; int tier; Seat *head; struct Row *next; } Row;
typedef struct Ticket  { char row; int num; int price; int cancelled; int refund; char name[NAME_LEN]; struct Ticket *next; } Ticket;
typedef struct Booking { int id; Ticket *head; struct Booking *next; } Booking;
typedef struct { char row; int num; char name[NAME_LEN]; } Req;

static Row *hall = NULL;
static Booking *bk_head = NULL, *bk_tail = NULL;
static int next_id = 1;

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) { perror("calloc"); exit(1); }
    return p;
}

static void free_hall(void) {
    while (hall) {
        Row *r = hall; hall = hall->next;
        while (r->head) { Seat *s = r->head; r->head = s->next; free(s); }
        free(r);
    }
}

static void free_bookings(void) {
    while (bk_head) {
        Booking *b = bk_head; bk_head = b->next;
        while (b->head) { Ticket *t = b->head; b->head = t->next; free(t); }
        free(b);
    }
    bk_tail = NULL; next_id = 1;
}

/* Build the hall: insert at tail, O(ROWS*COLS) */
static void build_hall(void) {
    free_hall(); free_bookings();
    Row *rt = NULL;
    for (int i = 0; i < ROWS; i++) {
        Row *row = xcalloc(1, sizeof(Row));
        row->label = (char)('A' + i);
        row->tier = tier_of_row(i);
        Seat *st = NULL;
        for (int c = 1; c <= COLS; c++) {
            Seat *s = xcalloc(1, sizeof(Seat));
            s->num = c;
            if (!row->head) row->head = s; else st->next = s;
            st = s;
        }
        if (!hall) hall = row; else rt->next = row;
        rt = row;
    }
}

static Row *find_row(char label) {                      /* O(rows) */
    for (Row *r = hall; r; r = r->next) if (r->label == label) return r;
    return NULL;
}

static Seat *find_seat(char label, int num) {           /* O(rows + seats) */
    Row *r = find_row(label);
    if (!r) return NULL;
    for (Seat *s = r->head; s; s = s->next) if (s->num == num) return s;
    return NULL;
}

static Booking *find_booking(int id) {                  /* O(bookings) */
    for (Booking *b = bk_head; b; b = b->next) if (b->id == id) return b;
    return NULL;
}

/* ---------- booking ---------- */

/* Check every requested seat BEFORE changing anything, so a group booking is
 * all-or-nothing. */
static int validate(const Req *rq, int n, char *msg) {
    for (int i = 0; i < n; i++) {
        Seat *s = find_seat(rq[i].row, rq[i].num);
        if (!s) { snprintf(msg, MSG_LEN, "Seat %c%d does not exist.", rq[i].row, rq[i].num); return 0; }
        if (s->booked) { snprintf(msg, MSG_LEN, "Seat %c%d is already booked. Nothing was booked.", rq[i].row, rq[i].num); return 0; }
        for (int j = 0; j < i; j++)
            if (rq[j].row == rq[i].row && rq[j].num == rq[i].num) {
                snprintf(msg, MSG_LEN, "Seat %c%d was picked twice.", rq[i].row, rq[i].num); return 0;
            }
    }
    return 1;
}

/* Append a Booking node (with a list of Ticket nodes) to the bookings list.
 * Caller must have run validate(). */
static Booking *make_booking(const Req *rq, int n) {
    Booking *b = xcalloc(1, sizeof *b);
    b->id = next_id++;
    Ticket *tt = NULL;
    for (int i = 0; i < n; i++) {
        Row *r = find_row(rq[i].row);
        Seat *s = find_seat(rq[i].row, rq[i].num);
        Ticket *t = xcalloc(1, sizeof *t);
        t->row = rq[i].row; t->num = rq[i].num; t->price = TIER_PRICE[r->tier];
        snprintf(t->name, NAME_LEN, "%s", rq[i].name);
        s->booked = 1; s->booking = b->id;
        snprintf(s->name, NAME_LEN, "%s", rq[i].name);
        if (!b->head) b->head = t; else tt->next = t;
        tt = t;
    }
    if (!bk_head) bk_head = b; else bk_tail->next = b;
    bk_tail = b;
    return b;
}

static int booking_total(const Booking *b) {
    int sum = 0;
    for (const Ticket *t = b->head; t; t = t->next) sum += t->price;
    return sum;
}

static void trim(char *s) {
    char *p = s; while (*p == ' ') p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && s[l - 1] == ' ') s[--l] = 0;
}

/* "A1:Asha,B3:Ravi" -> Req[] ; returns count or -1 (msg filled) */
static int parse_tickets(const char *s, Req *out, char *msg) {
    int n = 0;
    while (*s) {
        const char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len > 0) {
            if (n == MAX_GROUP) { snprintf(msg, MSG_LEN, "You can book at most %d tickets at once.", MAX_GROUP); return -1; }
            char tok[96];
            if (len >= sizeof tok) len = sizeof tok - 1;
            memcpy(tok, s, len); tok[len] = 0;
            char *p = tok; while (*p == ' ') p++;
            if (!isalpha((unsigned char)*p)) { strcpy(msg, "Bad ticket format. Use seat:name, e.g. A1:Asha."); return -1; }
            out[n].row = (char)toupper((unsigned char)*p++);
            char *e; long num = strtol(p, &e, 10);
            if (e == p || *e != ':') { strcpy(msg, "Bad ticket format. Use seat:name, e.g. A1:Asha."); return -1; }
            char *nm = e + 1; trim(nm);
            if (!*nm) { snprintf(msg, MSG_LEN, "Enter a name for seat %c%ld.", out[n].row, num); return -1; }
            out[n].num = (int)num;
            snprintf(out[n].name, NAME_LEN, "%.*s", NAME_LEN - 1, nm);
            n++;
        }
        if (!end) break;
        s = end + 1;
    }
    return n;
}

/* "Asha, Ravi, Meena" -> names[] ; returns count or -1 */
static int parse_names(const char *s, char names[][NAME_LEN], char *msg) {
    int n = 0;
    while (*s) {
        const char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        char tok[96];
        if (len >= sizeof tok) len = sizeof tok - 1;
        memcpy(tok, s, len); tok[len] = 0; trim(tok);
        if (*tok) {
            if (n == MAX_GROUP) { snprintf(msg, MSG_LEN, "A group can have at most %d people.", MAX_GROUP); return -1; }
            snprintf(names[n++], NAME_LEN, "%.*s", NAME_LEN - 1, tok);
        }
        if (!end) break;
        s = end + 1;
    }
    return n;
}

/* Seat a group together: first row (optionally of one tier) with a run of n
 * adjacent free seats, one seat per person. */
static int auto_allocate(char names[][NAME_LEN], int n, int tier, char *msg) {
    for (Row *r = hall; r; r = r->next) {
        if (tier >= 0 && r->tier != tier) continue;
        Seat *start = NULL; int run = 0;
        for (Seat *s = r->head; s; s = s->next) {
            if (s->booked) { run = 0; start = NULL; continue; }
            if (!run) start = s;
            if (++run == n) {
                Req rq[MAX_GROUP]; Seat *p = start;
                for (int i = 0; i < n; i++, p = p->next) {
                    rq[i].row = r->label; rq[i].num = p->num;
                    snprintf(rq[i].name, NAME_LEN, "%.*s", NAME_LEN - 1, names[i]);
                }
                Booking *b = make_booking(rq, n);
                snprintf(msg, MSG_LEN, "Booking #%d: %d seat%s together, %c%d to %c%d. Total " RUPEE "%d.",
                         b->id, n, n > 1 ? "s" : "", r->label, start->num, r->label, start->num + n - 1, booking_total(b));
                return 1;
            }
        }
    }
    if (tier >= 0) snprintf(msg, MSG_LEN, "No %s row has %d adjacent free seats.", TIER_NAME[tier], n);
    else snprintf(msg, MSG_LEN, "No row has %d adjacent free seats.", n);
    return 0;
}

/* ---------- cancellation ---------- */

/* Mark a ticket cancelled, free its seat, return the refund. */
static int cancel_ticket(Ticket *t) {
    t->cancelled = 1;
    t->refund = t->price * (100 - FEE_PCT) / 100;
    Seat *s = find_seat(t->row, t->num);
    if (s) { s->booked = 0; s->booking = 0; s->name[0] = 0; }
    return t->refund;
}

/* label/num > 0 -> cancel one ticket; otherwise cancel the whole booking. */
static int cancel_request(int bid, char label, int num, char *msg) {
    int has_seat = label && num > 0;
    if (!bid && has_seat) {
        Seat *s = find_seat(label, num);
        if (!s) { snprintf(msg, MSG_LEN, "Seat %c%d does not exist.", label, num); return 0; }
        if (!s->booked) { snprintf(msg, MSG_LEN, "Seat %c%d is not booked.", label, num); return 0; }
        bid = s->booking;
    }
    if (!bid) { strcpy(msg, "Say which booking to cancel."); return 0; }
    Booking *b = find_booking(bid);
    if (!b) { snprintf(msg, MSG_LEN, "Booking #%d not found.", bid); return 0; }

    if (has_seat) {
        for (Ticket *t = b->head; t; t = t->next)
            if (!t->cancelled && t->row == label && t->num == num) {
                char who[NAME_LEN]; snprintf(who, sizeof who, "%s", t->name);
                int refund = cancel_ticket(t);
                snprintf(msg, MSG_LEN, "Cancelled %c%d (%s) in booking #%d. Refund " RUPEE "%d after the %d%% fee.",
                         label, num, who, bid, refund, FEE_PCT);
                return 1;
            }
        snprintf(msg, MSG_LEN, "Seat %c%d is not an active ticket in booking #%d.", label, num, bid);
        return 0;
    }
    int count = 0, refund = 0, paid = 0;
    for (Ticket *t = b->head; t; t = t->next)
        if (!t->cancelled) { paid += t->price; refund += cancel_ticket(t); count++; }
    if (!count) { snprintf(msg, MSG_LEN, "Booking #%d is already cancelled.", bid); return 0; }
    snprintf(msg, MSG_LEN, "Booking #%d cancelled (%d ticket%s). Refund " RUPEE "%d after a " RUPEE "%d fee.",
             bid, count, count > 1 ? "s" : "", refund, paid - refund);
    return 1;
}

/* ---------- JSON ---------- */

typedef struct { char *p; size_t len, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int w = vsnprintf(b->p + b->len, room, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if ((size_t)w < room) { b->len += (size_t)w; return; }
        while (b->cap - b->len <= (size_t)w) b->cap *= 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { perror("realloc"); exit(1); }
    }
}

/* Names never contain quotes, backslashes or control chars (see qparam), and
 * messages are built from fixed text + those names, so no extra escaping. */
static char *state_json(int ok, const char *msg) {
    int total = 0, booked = 0, sales = 0, refunded = 0, nb = 0;
    for (Row *r = hall; r; r = r->next)
        for (Seat *s = r->head; s; s = s->next) { total++; booked += s->booked; }
    for (Booking *b = bk_head; b; b = b->next) {
        nb++;
        for (Ticket *t = b->head; t; t = t->next) { if (t->cancelled) refunded += t->refund; else sales += t->price; }
    }
    Buf b = { malloc(4096), 0, 4096 };
    if (!b.p) { perror("malloc"); exit(1); }
    bput(&b, "{\"ok\":%d,\"message\":\"%s\",\"total\":%d,\"booked\":%d,\"sales\":%d,\"refunded\":%d,"
             "\"feePct\":%d,\"maxGroup\":%d,\"tiers\":[",
         ok, msg, total, booked, sales, refunded, FEE_PCT, MAX_GROUP);
    for (int i = 0; i < 3; i++)
        bput(&b, "%s{\"name\":\"%s\",\"price\":%d}", i ? "," : "", TIER_NAME[i], TIER_PRICE[i]);
    bput(&b, "],\"rows\":[");
    for (Row *r = hall; r; r = r->next) {
        bput(&b, "%s{\"label\":\"%c\",\"tier\":%d,\"price\":%d,\"seats\":[",
             r == hall ? "" : ",", r->label, r->tier, TIER_PRICE[r->tier]);
        for (Seat *s = r->head; s; s = s->next)
            bput(&b, "%s{\"n\":%d,\"b\":%d,\"bk\":%d,\"name\":\"%s\"}",
                 s == r->head ? "" : ",", s->num, s->booked, s->booking, s->name);
        bput(&b, "]}");
    }
    bput(&b, "],\"bookings\":[");
    int idx = 0, skip = nb > SHOW_BOOKINGS ? nb - SHOW_BOOKINGS : 0, first = 1;
    for (Booking *bk = bk_head; bk; bk = bk->next, idx++) {
        if (idx < skip) continue;
        bput(&b, "%s{\"id\":%d,\"tickets\":[", first ? "" : ",", bk->id);
        first = 0;
        for (Ticket *t = bk->head; t; t = t->next)
            bput(&b, "%s{\"seat\":\"%c%d\",\"name\":\"%s\",\"price\":%d,\"cancelled\":%d,\"refund\":%d}",
                 t == bk->head ? "" : ",", t->row, t->num, t->name, t->price, t->cancelled, t->refund);
        bput(&b, "]}");
    }
    bput(&b, "]}");
    return b.p;
}

/* ---------- HTTP ---------- */

static int qparam(const char *q, const char *key, char *out, size_t n) {
    size_t kl = strlen(key); out[0] = 0;
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1; size_t i = 0;
            while (*v && *v != '&' && i < n - 1) {
                char c = *v;
                if (c == '+') c = ' ';
                else if (c == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) {
                    char h[3] = {v[1], v[2], 0}; c = (char)strtol(h, NULL, 16); v += 2;
                }
                v++;
                if (c == '"' || c == '\\' || (unsigned char)c < 32) continue;
                out[i++] = c;
            }
            out[i] = 0; return 1;
        }
        q = strchr(q, '&'); if (q) q++;
    }
    return 0;
}

static void send_all(int fd, const char *p, size_t len) {
    while (len) {
        ssize_t w = send(fd, p, len, 0);
        if (w <= 0) return;
        p += w; len -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *type, const char *body, size_t len) {
    char h[256];
    int hl = snprintf(h, sizeof h,
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        code, type, len);
    send_all(fd, h, (size_t)hl); send_all(fd, body, len);
}

static void handle(int fd) {
    char req[8192], method[8], path[2048];
    ssize_t r = recv(fd, req, sizeof req - 1, 0);
    if (r <= 0) return;
    req[r] = 0;
    if (sscanf(req, "%7s %2047s", method, path) != 2) return;
    char *q = strchr(path, '?'); if (q) *q++ = 0;

    if (!strcmp(path, "/")) {
        FILE *f = fopen("public/index.html", "rb");
        if (!f) { respond(fd, 404, "text/plain", "index.html missing", 17); return; }
        fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
        char *data = malloc((size_t)len); size_t got = fread(data, 1, (size_t)len, f); fclose(f);
        respond(fd, 200, "text/html; charset=utf-8", data, got); free(data); return;
    }
    if (strncmp(path, "/api/", 5) != 0) { respond(fd, 404, "text/plain", "Not found", 9); return; }
    if (strcmp(path, "/api/seats") && strcmp(method, "POST")) {
        respond(fd, 405, "text/plain", "Use POST", 8); return;
    }

    char msg[MSG_LEN] = "", row[8], num[8], seat[16], booking[16], tier[16], tickets[1024], names[1024];
    int ok = 1;
    qparam(q, "row", row, sizeof row); qparam(q, "num", num, sizeof num);
    qparam(q, "seat", seat, sizeof seat); qparam(q, "booking", booking, sizeof booking);
    qparam(q, "tier", tier, sizeof tier);
    qparam(q, "tickets", tickets, sizeof tickets); qparam(q, "names", names, sizeof names);

    if (!strcmp(path, "/api/seats")) { /* just state */ }
    else if (!strcmp(path, "/api/book")) {
        Req rq[MAX_GROUP];
        int n = parse_tickets(tickets, rq, msg);
        if (n < 0) ok = 0;
        else if (n == 0) { ok = 0; strcpy(msg, "Pick at least one seat."); }
        else if (!validate(rq, n, msg)) ok = 0;
        else {
            Booking *b = make_booking(rq, n);
            snprintf(msg, MSG_LEN, "Booking #%d confirmed for %d %s. Total " RUPEE "%d.",
                     b->id, n, n == 1 ? "person" : "people", booking_total(b));
        }
    } else if (!strcmp(path, "/api/cancel")) {
        char label = 0; int snum = 0;
        if (seat[0]) { label = (char)toupper((unsigned char)seat[0]); snum = atoi(seat + 1); }
        else if (row[0]) { label = (char)toupper((unsigned char)row[0]); snum = atoi(num); }
        ok = cancel_request(atoi(booking), label, snum, msg);
    } else if (!strcmp(path, "/api/auto")) {
        char grp[MAX_GROUP][NAME_LEN];
        int n = parse_names(names, grp, msg), t = -1;
        if (n < 0) ok = 0;
        else if (n == 0) { ok = 0; strcpy(msg, "Enter at least one name."); }
        else {
            if (tier[0] && strcasecmp(tier, "any")) {
                for (int i = 0; i < 3; i++) if (!strcasecmp(tier, TIER_NAME[i])) t = i;
                if (t < 0) { ok = 0; strcpy(msg, "Unknown tier."); }
            }
            if (ok) ok = auto_allocate(grp, n, t, msg);
        }
    } else if (!strcmp(path, "/api/reset")) { build_hall(); strcpy(msg, "All bookings cleared."); }
    else { respond(fd, 404, "text/plain", "Not found", 9); return; }

    char *js = state_json(ok, msg);
    respond(fd, 200, "application/json; charset=utf-8", js, strlen(js));
    free(js);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *p = getenv("PORT"); int port = p ? atoi(p) : 8080;
    build_hall();
    int srv = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((unsigned short)port);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 32) < 0) { perror("bind/listen"); return 1; }
    printf("Theatre seat allocation running on port %d\n", port);
    fflush(stdout);
    for (;;) { int c = accept(srv, NULL, NULL); if (c < 0) continue; handle(c); close(c); }
}
