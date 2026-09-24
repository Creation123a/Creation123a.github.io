# ☕ Aurora Coffee Co.

> A futuristic coffee booking system with a **C++ backend** and a **vanilla HTML/CSS/JS frontend** — deployed live on Render via Docker.


**Live demo:** https://Creation123.onrender.com
**Repository:** https://github.com/Creation123a.github.io

---

## What is this?

A full-stack coffee ordering demo. The backend is a **real C++ HTTP server** — no frameworks, no Node, no Python. The frontend is served by that same C++ process and talks to it over REST.

Everything you would expect from a modern coffee app is here: browse a 49-item menu, search and filter, customise items, add to cart, checkout with online or cash-on-delivery payment, apply promo codes, and track your order live through a five-stage delivery pipeline.

The project exists to demonstrate **advanced OOP in C++** applied to a realistic product — not a console application.

---

## Features

### Customer experience
- 🌌 **4-second preloader** — a light beam travelling through a parallax starfield
- 🌓 **Dark / light mode** — persisted per user, respects system preference
- 🔍 **Live search** with skeleton loading shimmer
- 🏷️ **Category tabs** (Coffee · Tea · Pastry · Merch) and multi-mode sorting
- 🎨 **Item customisation modal** — size, milk, sugar, extras, notes
- 🛒 **Persistent cart** — per-session, survives page refresh
- 💳 **Two payment methods** — Online (simulated) and Cash on Delivery
- 🎟️ **Promo codes** — `BREW20` (20% off), `FIRSTCUP` (flat ₹50 off), `AURORA10` (10% off)
- 📦 **Live order tracking** — Placed → Confirmed → Brewing → Out for delivery → Delivered
- ⏱️ **Real-time ETA countdown** — updates every 2 seconds
- ❌ **Cancel window** — free cancellation within the first 15 seconds
- 🧑‍🍳 **Aria, the AI barista** — rule-based conversational assistant
- 📜 **Order history** — with tap-to-track
- 📱 **Fully responsive** — phone, tablet, desktop

### Engineering
- ✅ Thread-safe session store (Singleton)
- ✅ Disk persistence for orders
- ✅ Input validation on server **and** client
- ✅ HTTP request logging
- ✅ Health check endpoint
- ✅ Docker + Render deployment
- ✅ GitHub Actions CI

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Browser (any device)                                    │
│  ├─ index.html   ─ markup                                │
│  ├─ style.css    ─ dark/light theme, animations          │
│  └─ app.js       ─ fetch layer, view router              │
└─────────────────────────────────────────────────────────┘
                        │  HTTP + JSON
                        ▼
┌─────────────────────────────────────────────────────────┐
│  C++ HTTP Server (main.cpp + httplib.h)                  │
│  ├─ MenuRepository   ─ 49 seeded products                │
│  ├─ Cart / SessionStore (Singleton) ─ per-user carts     │
│  ├─ Discount  (Strategy pattern)                         │
│  ├─ PaymentMethod (Online / COD, polymorphism)           │
│  ├─ Order + state machine (deriveStatus)                 │
│  ├─ OrderRepository  ─ persistence to data/orders.dat    │
│  └─ CoffeeBot        ─ rule-based intent matcher         │
└─────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  data/orders.dat  ─ pipe-delimited persistence           │
└─────────────────────────────────────────────────────────┘
```

### Design principles

> **C++ decides. JavaScript displays.**

Every price, tax, discount, ETA, and status transition originates in the C++ server. The frontend has **zero business logic** — it only moves data and renders what it's told.

---

## OOP concepts demonstrated

| Concept | Where |
|---|---|
| **Abstraction** | `Product` is a pure abstract base class |
| **Inheritance** | `Coffee`, `Tea`, `Pastry`, `Merch` all extend `Product` |
| **Polymorphism** | `virtual PaymentMethod::process()` — Online vs COD |
| **Encapsulation** | `Cart` exposes only `add` / `remove` / `subtotal`; internals private |
| **Strategy pattern** | `PercentOff` vs `FlatOff`, chosen at runtime |
| **Singleton** | `SessionStore::instance()`, `OrderRepository::instance()` |
| **State machine** | `deriveStatus()` walks order through 5 legal states |
| **Template / container polymorphism** | `vector<shared_ptr<Product>>` holding mixed subclasses |

---

## Tech stack

| Layer | Technology |
|---|---|
| Backend | C++17, [cpp-httplib](https://github.com/yhirose/cpp-httplib) |
| Frontend | Vanilla HTML5, CSS3, JavaScript (ES2020) |
| Fonts | Inter, JetBrains Mono (Google Fonts) |
| Build | `g++` with `-std=c++17 -O2` |
| Container | Docker (multi-stage: `gcc:12` → `debian:bookworm-slim`) |
| Deploy | Render (free tier, Docker runtime) |
| CI | GitHub Actions |

---

## Running locally

### Requirements
- A C++17 compiler (`g++` 7+, `clang++` 5+)
- `httplib.h` in the project root (already committed)
- Linux/macOS, or Windows with MinGW / WSL

### Build & run

**Linux / macOS**
```bash
g++ -std=c++17 -O2 -o server main.cpp -lpthread
./server
```

**Windows (MinGW)**
```bash
g++ -std=c++17 -O2 -o server.exe main.cpp -lws2_32 -lwsock32
server.exe
```

**Or use the provided build scripts**
```bash
./build.sh          # macOS / Linux / Git Bash
build.bat           # Windows CMD
```

Then open **http://localhost:8080**.

### Changing the port

The server reads the `PORT` environment variable, falling back to `8080`:

```bash
PORT=3000 ./server
```

---

## API reference

All POST bodies are `application/x-www-form-urlencoded`.
All responses are `application/json` unless noted.
Session is tracked via the `X-Session` header.

| Method | Path | Body / Query | Purpose |
|---|---|---|---|
| `GET`  | `/api/health` | — | Liveness probe |
| `GET`  | `/api/menu` | `?q=&cat=&sort=` | Search / filter / sort menu |
| `GET`  | `/api/promos` | — | Active promo codes |
| `GET`  | `/api/cart` | — | Current cart |
| `POST` | `/api/cart/add` | `id, qty, notes` | Add item |
| `POST` | `/api/cart/update` | `id, qty` | Change quantity |
| `POST` | `/api/cart/remove` | `id` | Remove item |
| `POST` | `/api/cart/clear` | — | Empty cart |
| `POST` | `/api/checkout` | `name, phone, email, address, payment, promo` | Place order |
| `GET`  | `/api/order/{id}` | — | Live order status + ETA |
| `POST` | `/api/order/{id}/cancel` | — | Cancel within 15 s |
| `GET`  | `/api/orders` | — | Recent orders (last 20) |
| `POST` | `/api/chat` | raw text body | Aria's reply |

### Cart response shape

```json
{
  "items": [
    { "id": 43, "name": "Brownie", "price": 160, "qty": 1, "notes": "", "emoji": "🍫" }
  ],
  "count": 1,
  "subtotal": 160,
  "discount": 0,
  "discountLabel": "",
  "tax": 8,
  "delivery": 40,
  "total": 208
}
```

### Order status response shape

```json
{
  "id": "CFE-10001",
  "status": "Out for delivery",
  "eta": 18,
  "placedAt": 1748000000,
  "customer": "Ada Lovelace",
  "address": "12 Analytical Engine Way",
  "total": 208,
  "payment": "Online",
  "canCancel": false,
  "steps": [
    { "name": "Placed",           "done": true,  "active": false },
    { "name": "Confirmed",        "done": true,  "active": false },
    { "name": "Brewing",          "done": true,  "active": false },
    { "name": "Out for delivery", "done": false, "active": true  },
    { "name": "Delivered",        "done": false, "active": false }
  ],
  "items": [
    { "id": 43, "name": "Brownie", "price": 160, "qty": 1, "emoji": "🍫" }
  ]
}
```

---

## Order lifecycle

| Elapsed time | Status |
|---|---|
| 0 – 5 s | Placed |
| 5 – 15 s | Confirmed |
| 15 – 30 s | Brewing |
| 30 – 50 s | Out for delivery |
| 50 s+ | Delivered |

Free cancellation is allowed until the **15 s** mark. After brewing begins, the order is locked.

---

## Deployment

The project deploys to **Render** via Docker. The container:

1. Compiles `main.cpp` inside `gcc:12` with `-O2`
2. Copies the binary + static files to a lightweight `debian:bookworm-slim`
3. Runs `./server` on the port provided by the `PORT` env var

Push to `main` → Render auto-redeploys in ~90 seconds.

**Note on Render's free tier:** the service sleeps after 15 minutes of inactivity. The first request afterwards triggers a 30–60 second cold start. To keep it warm, point a free [UptimeRobot](https://uptimerobot.com) monitor at `/api/health` every 5 minutes.

---

## Project structure

```
coffee-booking/
├── .github/
│   └── workflows/
│       └── build.yml       # CI: compile on every push
├── data/
│   └── orders.dat          # auto-created; persists orders
├── Dockerfile              # multi-stage build
├── render.yaml             # Render service definition
├── .gitignore
├── build.sh                # local build script (Unix)
├── build.bat               # local build script (Windows)
├── httplib.h               # cpp-httplib (single-header, vendored)
├── main.cpp                # C++ backend
├── index.html              # frontend markup
├── style.css               # frontend styling
├── app.js                  # frontend controller
└── README.md
```

---

## Aria — the barista

Aria is a **rule-based intent matcher**, not a large language model. She recognises keywords like `recommend`, `delivery`, `vegan`, `decaf`, `cancel`, `promo`, `hours`, and returns one of ~18 scripted replies. This is intentional: for a microproject, a rule-based agent is transparent, deterministic, and honest — and it demonstrates that the "AI" layer is just another service class in the backend (`CoffeeBot`).

---


## What's next

- [ ] Real card payment via Razorpay / Stripe sandbox
- [ ] Postgres for durable order storage
- [ ] Email / SMS order receipts
- [ ] User accounts with saved addresses
- [ ] Redis-backed session store for multi-instance scaling

---


Sample data only. No real payments are processed and no real personal information is stored.
