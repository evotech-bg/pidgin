#!/usr/bin/env python3
"""
Minimal FastAPI app with Pidgin compression.

Run:  uvicorn examples.fastapi_app:app --reload
Test: curl -H "Accept-Encoding: pidgin" http://localhost:8000/api/users | xxd | head
"""

from fastapi import FastAPI
from pidgin.contrib.fastapi import PidginMiddleware

app = FastAPI()
app.add_middleware(PidginMiddleware)
# That's it. Pidgin auto-learns schemas and compresses JSON responses.


# --- Your normal API endpoints, unchanged ---

USERS = [
    {"id": i, "name": f"User {i}", "email": f"user{i}@example.com",
     "role": "admin" if i % 5 == 0 else "editor", "active": i % 3 != 0}
    for i in range(1, 101)
]


@app.get("/api/users")
async def get_users(page: int = 1, per_page: int = 20):
    start = (page - 1) * per_page
    return {
        "data": USERS[start:start + per_page],
        "total": len(USERS),
        "page": page,
        "per_page": per_page,
    }


@app.get("/api/users/{user_id}")
async def get_user(user_id: int):
    for u in USERS:
        if u["id"] == user_id:
            return u
    return {"error": "not found"}
