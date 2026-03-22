"""
test-backend.py — Simple FastAPI app that returns JSON for testing
Pidgin compression across nginx, Apache, and Kong.

Endpoints:
  GET /api/users      — 100 users (consistent per restart)
  GET /api/orders     — 50 orders (consistent per restart)
  GET /api/products   — 30 products
  GET /api/users/:id  — single user
  GET /health         — health check
"""

import random
from datetime import datetime, timedelta
from typing import Any

from fastapi import FastAPI
from fastapi.responses import JSONResponse

app = FastAPI(title="Pidgin Test Backend")

# Seed for reproducible data
rng = random.Random(42)

# --------------------------------------------------------------------------- #
# Data generators                                                              #
# --------------------------------------------------------------------------- #

FIRST_NAMES = [
    "Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Hank",
    "Ivy", "Jack", "Karen", "Leo", "Mona", "Nick", "Olivia", "Paul",
    "Quinn", "Rita", "Sam", "Tina", "Uma", "Victor", "Wendy", "Xander",
    "Yara", "Zane",
]

LAST_NAMES = [
    "Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
    "Davis", "Rodriguez", "Martinez", "Hernandez", "Lopez", "Gonzalez",
    "Wilson", "Anderson", "Thomas", "Taylor", "Moore", "Jackson", "Martin",
]

DEPARTMENTS = [
    "Engineering", "Marketing", "Sales", "Support", "Finance", "HR",
    "Operations", "Legal", "Product", "Design",
]

STATUSES = ["active", "inactive", "pending", "suspended"]

PRODUCT_CATEGORIES = [
    "Electronics", "Clothing", "Books", "Home & Garden", "Sports",
    "Automotive", "Health", "Toys", "Food", "Office",
]

ORDER_STATUSES = ["pending", "confirmed", "shipped", "delivered", "cancelled"]

COUNTRIES = [
    "US", "UK", "DE", "FR", "JP", "CA", "AU", "BR", "IN", "ES",
    "IT", "NL", "SE", "NO", "DK", "FI", "PL", "CZ", "AT", "CH",
]


def gen_email(first: str, last: str, idx: int) -> str:
    domain = rng.choice(["example.com", "test.org", "demo.io", "mail.dev"])
    return f"{first.lower()}.{last.lower()}{idx}@{domain}"


def gen_phone() -> str:
    return f"+1-{rng.randint(200,999)}-{rng.randint(100,999)}-{rng.randint(1000,9999)}"


def gen_date(start_year: int = 2020, end_year: int = 2025) -> str:
    start = datetime(start_year, 1, 1)
    end = datetime(end_year, 12, 31)
    delta = (end - start).days
    d = start + timedelta(days=rng.randint(0, delta))
    return d.strftime("%Y-%m-%dT%H:%M:%SZ")


def gen_address() -> dict:
    return {
        "street": f"{rng.randint(1, 9999)} {rng.choice(['Main', 'Oak', 'Elm', 'Pine', 'Maple', 'Cedar'])} {rng.choice(['St', 'Ave', 'Blvd', 'Dr', 'Ln'])}",
        "city": rng.choice(["Springfield", "Portland", "Franklin", "Clinton", "Madison", "Georgetown", "Arlington", "Salem"]),
        "state": rng.choice(["CA", "NY", "TX", "FL", "IL", "PA", "OH", "GA", "NC", "MI"]),
        "zip": f"{rng.randint(10000, 99999)}",
        "country": rng.choice(COUNTRIES),
    }


# --------------------------------------------------------------------------- #
# Pre-generate data                                                            #
# --------------------------------------------------------------------------- #

USERS: list[dict[str, Any]] = []
for i in range(1, 101):
    first = rng.choice(FIRST_NAMES)
    last = rng.choice(LAST_NAMES)
    USERS.append({
        "id": i,
        "first_name": first,
        "last_name": last,
        "email": gen_email(first, last, i),
        "phone": gen_phone(),
        "department": rng.choice(DEPARTMENTS),
        "role": rng.choice(["admin", "user", "manager", "viewer"]),
        "status": rng.choice(STATUSES),
        "salary": round(rng.uniform(35000, 150000), 2),
        "hire_date": gen_date(2018, 2024),
        "address": gen_address(),
        "tags": rng.sample(["remote", "onsite", "hybrid", "senior", "junior", "lead", "intern", "contractor"], k=rng.randint(1, 4)),
        "metadata": {
            "last_login": gen_date(2024, 2025),
            "login_count": rng.randint(1, 500),
            "preferences": {
                "theme": rng.choice(["light", "dark", "system"]),
                "language": rng.choice(["en", "de", "fr", "es", "ja"]),
                "notifications": rng.choice([True, False]),
            },
        },
    })

ORDERS: list[dict[str, Any]] = []
for i in range(1, 51):
    num_items = rng.randint(1, 5)
    items = []
    for j in range(num_items):
        qty = rng.randint(1, 10)
        price = round(rng.uniform(5.0, 500.0), 2)
        items.append({
            "product_id": rng.randint(1, 30),
            "product_name": f"Product {rng.randint(1, 30)}",
            "quantity": qty,
            "unit_price": price,
            "total": round(qty * price, 2),
            "category": rng.choice(PRODUCT_CATEGORIES),
        })

    subtotal = round(sum(it["total"] for it in items), 2)
    tax = round(subtotal * 0.08, 2)

    ORDERS.append({
        "id": i,
        "order_number": f"ORD-{2024000 + i}",
        "user_id": rng.randint(1, 100),
        "status": rng.choice(ORDER_STATUSES),
        "created_at": gen_date(2024, 2025),
        "updated_at": gen_date(2024, 2025),
        "items": items,
        "subtotal": subtotal,
        "tax": tax,
        "total": round(subtotal + tax, 2),
        "currency": "EUR",
        "shipping_address": gen_address(),
        "billing_address": gen_address(),
        "payment_method": rng.choice(["credit_card", "paypal", "bank_transfer", "crypto"]),
        "notes": rng.choice([None, "Rush delivery", "Gift wrap requested", "Leave at door", ""]),
    })

PRODUCTS: list[dict[str, Any]] = []
for i in range(1, 31):
    PRODUCTS.append({
        "id": i,
        "name": f"Product {i}",
        "sku": f"SKU-{rng.randint(10000, 99999)}",
        "description": f"High-quality product in the {rng.choice(PRODUCT_CATEGORIES).lower()} category. Features premium materials and excellent craftsmanship.",
        "category": rng.choice(PRODUCT_CATEGORIES),
        "price": round(rng.uniform(9.99, 999.99), 2),
        "currency": "EUR",
        "stock": rng.randint(0, 500),
        "weight_kg": round(rng.uniform(0.1, 25.0), 2),
        "dimensions": {
            "length_cm": round(rng.uniform(5, 100), 1),
            "width_cm": round(rng.uniform(5, 80), 1),
            "height_cm": round(rng.uniform(2, 50), 1),
        },
        "tags": rng.sample(["bestseller", "new", "sale", "premium", "eco", "limited"], k=rng.randint(1, 3)),
        "ratings": {
            "average": round(rng.uniform(3.0, 5.0), 1),
            "count": rng.randint(5, 2000),
        },
        "created_at": gen_date(2020, 2024),
        "available": rng.choice([True, True, True, False]),
    })


# --------------------------------------------------------------------------- #
# Routes                                                                       #
# --------------------------------------------------------------------------- #

@app.get("/health")
async def health():
    return {"status": "ok", "server": "test-backend", "endpoints": ["/api/users", "/api/orders", "/api/products"]}


@app.get("/api/users")
async def list_users():
    return JSONResponse(
        content={"data": USERS, "total": len(USERS), "page": 1, "per_page": 100},
        media_type="application/json",
    )


@app.get("/api/users/{user_id}")
async def get_user(user_id: int):
    for u in USERS:
        if u["id"] == user_id:
            return JSONResponse(content=u, media_type="application/json")
    return JSONResponse(content={"error": "not found"}, status_code=404)


@app.get("/api/orders")
async def list_orders():
    return JSONResponse(
        content={"data": ORDERS, "total": len(ORDERS), "page": 1, "per_page": 50},
        media_type="application/json",
    )


@app.get("/api/orders/{order_id}")
async def get_order(order_id: int):
    for o in ORDERS:
        if o["id"] == order_id:
            return JSONResponse(content=o, media_type="application/json")
    return JSONResponse(content={"error": "not found"}, status_code=404)


@app.get("/api/products")
async def list_products():
    return JSONResponse(
        content={"data": PRODUCTS, "total": len(PRODUCTS), "page": 1, "per_page": 30},
        media_type="application/json",
    )


@app.get("/api/products/{product_id}")
async def get_product(product_id: int):
    for p in PRODUCTS:
        if p["id"] == product_id:
            return JSONResponse(content=p, media_type="application/json")
    return JSONResponse(content={"error": "not found"}, status_code=404)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
