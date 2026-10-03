# Curtain Call: Theatre Seat Allocation

A full stack project that shows a real use of the **linked list** data structure, written in **C**.

- **Backend:** C (`server.c`), POSIX sockets, no libraries. Exposes a small JSON API.
- **Frontend:** one HTML/CSS/JS page (`public/index.html`) served by the C server.
- **DSA concept:** singly linked lists.

## Features

- **Tiered prices:** Premium rows A-B `₹500`, Standard rows C-D `₹350`, Economy rows E-F `₹200`.
- **Book for several people at once:** pick up to 10 seats, give each seat its own guest name, and book them in a single request. The booking is all-or-nothing: if any seat is taken, nothing is booked.
- **Cancellation:** cancel one ticket (tap a booked seat) or a whole booking. A **10% cancellation fee** is kept; the rest is refunded. The seat is freed straight away.
- **Group seating:** enter names, choose a section, and the server finds a row with enough adjacent free seats.
- **Booking history:** every booking is listed with its tickets, totals, and refunds.

## How the linked lists are used

```
hall -> Row A -> Row B -> ... -> Row F -> NULL
          |
          v
        Seat 1 -> Seat 2 -> ... -> Seat 10 -> NULL

bookings -> Booking #1 -> Booking #2 -> ... -> NULL
               |
               v
            Ticket A1 -> Ticket A2 -> ... -> NULL
```

| Feature | Linked list operation | Time |
|---|---|---|
| Build hall | Insert at tail | O(rows x seats) |
| Find a seat | Traverse rows, then seats | O(rows + seats) |
| Book n people at once | Validate n seats, then append a Booking node with n Ticket nodes | O(n x (rows + seats)) |
| Cancel a ticket / booking | Find Booking node, walk its Ticket list, free the Seat node | O(bookings + tickets) |
| Seat a group together | Walk a row, track a run of free nodes | O(rows x seats) |
| Clear everything | Free every node, rebuild | O(rows x seats + tickets) |

Nothing is stored in an array: seats, rows, bookings and tickets are heap-allocated nodes joined by `next` pointers.

## API

Read-only calls use `GET`; everything that changes data must be `POST` (otherwise `405`). Every response is the full JSON state plus `ok` and `message`.

| Method | Path | Params |
|---|---|---|
| GET | `/api/seats` | none |
| POST | `/api/book` | `tickets` = `A1:Asha,A2:Ravi,C5:Meena` (seat:name pairs, 1 to 10) |
| POST | `/api/cancel` | `booking` (whole booking), plus optional `seat` (e.g. `A1`) for one ticket. `row` + `num` alone also works. |
| POST | `/api/auto` | `names` = `Asha,Ravi,Meena`, optional `tier` = `premium` / `standard` / `economy` / `any` |
| POST | `/api/reset` | none |

Example:

```bash
curl -X POST "http://localhost:8080/api/book?tickets=A1:Asha,A2:Ravi"
# {"ok":1,"message":"Booking #1 confirmed for 2 people. Total ₹1000.", ...}
curl -X POST "http://localhost:8080/api/cancel?booking=1&seat=A1"
# {"ok":1,"message":"Cancelled A1 (Asha) in booking #1. Refund ₹450 after the 10% fee.", ...}
```

## Run locally

```bash
make run          # or: gcc -O2 -o server server.c && ./server
# open http://localhost:8080
```

## Test

```bash
make test         # builds, starts the server on a free port, runs 17 API tests (needs python3)
```

## Deploy (Render, free tier)

1. Push this folder to a public GitHub repo.
2. On render.com choose **New > Web Service**, connect the repo, set **Runtime: Docker**.
3. Deploy. Render sets `PORT` automatically; copy the public URL into this README.

Live demo: https://theatre-seat-booking.onrender.com

Note: bookings live in memory, so they reset when the service restarts. The server handles one request at a time, which is also what makes each group booking atomic.
