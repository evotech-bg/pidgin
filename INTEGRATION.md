# Pidgin Integration Guide

## Install

```bash
pip install pidgin                  # core (brotli included)
pip install pidgin[crypto]          # + RatchetCipher / SecureChannel
```

---

## 1. Schema-Aware API Compression (SchemaCodec)

Compresses JSON API responses by learning the schema once, then encoding only values.
Beats brotli and Protobuf on both compression ratio and speed. Fully lossless.

### How it works

```
Phase 1 (once):    Learn schema from sample responses → profile (~1KB)
Phase 2 (runtime): Compress responses using profile → binary (no keys, optimal types)
                   Decompress on client using same profile → original JSON
```

### Quick Start

```python
from pidgin import SchemaCodec

# Step 1: Learn from sample responses (do once, save profile)
samples = [
    {"id": 1, "name": "Alice", "email": "alice@example.com", "role": "admin", "active": True},
    {"id": 2, "name": "Bob",   "email": "bob@example.com",   "role": "editor", "active": False},
    # ... 10-50 samples is enough
]
codec = SchemaCodec.learn(samples)

# Step 2: Save profile (share with client)
profile_json = codec.profile.to_json()
# Store in file, serve at /api/schema-profile, embed in mobile app, etc.

# Step 3: Compress responses
compressed = codec.compress({"id": 3, "name": "Charlie", ...})  # bytes
original = codec.decompress(compressed)                          # dict

# Works with arrays too
compressed = codec.compress([user1, user2, ...])   # list[dict] → bytes
users = codec.decompress(compressed)                # bytes → list[dict]
```

### FastAPI Integration

```python
from fastapi import FastAPI, Header, Response
from pidgin import SchemaCodec
from pidgin.schema_codec import SchemaProfile

app = FastAPI()

# Load profile (created once during setup/deployment)
with open("profiles/users.json") as f:
    profile = SchemaProfile.from_json(f.read())
users_codec = SchemaCodec(profile)


@app.get("/api/schema-profile/users")
async def get_users_profile():
    """Client fetches this once to enable Pidgin decoding."""
    return Response(
        content=users_codec.profile.to_json(),
        media_type="application/json",
    )


@app.get("/api/users")
async def get_users(accept_encoding: str = Header("")):
    users = await db.get_users()  # list[dict]

    if "pidgin" in accept_encoding:
        return Response(
            content=users_codec.compress(users),
            media_type="application/octet-stream",
            headers={
                "Content-Encoding": "pidgin",
                "X-Pidgin-Profile": "users",
            },
        )

    # Fallback: regular JSON
    return users
```

### Django / DRF Integration

```python
# middleware.py
from pidgin import SchemaCodec
from pidgin.schema_codec import SchemaProfile
import json

class PidginMiddleware:
    PROFILES = {}  # endpoint → SchemaCodec

    def __init__(self, get_response):
        self.get_response = get_response
        self._load_profiles()

    def _load_profiles(self):
        """Load pre-generated profiles for known endpoints."""
        import glob
        for path in glob.glob("profiles/*.json"):
            name = path.split("/")[-1].replace(".json", "")
            with open(path) as f:
                profile = SchemaProfile.from_json(f.read())
            self.PROFILES[name] = SchemaCodec(profile)

    def __call__(self, request):
        response = self.get_response(request)

        if "pidgin" not in request.META.get("HTTP_ACCEPT_ENCODING", ""):
            return response

        profile_name = self._match_profile(request.path)
        if not profile_name or profile_name not in self.PROFILES:
            return response

        codec = self.PROFILES[profile_name]
        try:
            data = json.loads(response.content)
            compressed = codec.compress(data)
            response.content = compressed
            response["Content-Encoding"] = "pidgin"
            response["Content-Type"] = "application/octet-stream"
            response["X-Pidgin-Profile"] = profile_name
        except (json.JSONDecodeError, Exception):
            pass

        return response

    def _match_profile(self, path):
        if "/api/users" in path:
            return "users"
        if "/api/products" in path:
            return "products"
        return None
```

```python
# settings.py
MIDDLEWARE = [
    "myapp.middleware.PidginMiddleware",
    # ...
]
```

### JavaScript Client (Browser / Node)

```bash
npm install @pidgin/codec
```

```typescript
import { PidginClient } from '@pidgin/codec';

const client = new PidginClient({
  baseUrl: 'https://api.example.com',
  profiles: { users: usersProfileJson },
});

// Auto-negotiates: sends Accept-Encoding: pidgin, decodes response
const users = await client.get('/api/users', 'users');
```

---

## 2. Schema Evolution

When your API schema changes, evolve the profile instead of re-learning from scratch.

```python
from pidgin import SchemaCodec

# v1 schema
codec_v1 = SchemaCodec.learn(users_v1_samples)

# API evolves — new fields, removed fields, new enum values
v2_profile = codec_v1.profile.evolve(users_v2_samples)
codec_v2 = SchemaCodec(v2_profile)

# v2 handles both old and new data shapes
codec_v2.compress(old_shape_data)   # missing new fields → absent marker
codec_v2.compress(new_shape_data)   # new fields encoded normally

# See what changed
for line in codec_v1.profile.diff(v2_profile):
    print(line)
```

---

## 3. When to Use What

| Scenario | Use | Why |
|---|---|---|
| REST API responses | `SchemaCodec` | 81-89% savings, lossless, faster than brotli |
| Mobile API | `SchemaCodec` | Smaller payloads = faster loads on slow networks |
| WebSocket streams | `SchemaCodec + RatchetCipher` | Per-event savings + forward-secrecy encryption |
| Database sync / ETL | `SchemaCodec` | Massive savings on repetitive structured data |
| IoT / edge | `SchemaCodec + RatchetCipher` | Compact + encrypted on constrained links |
| Already using Protobuf | Consider switching | Pidgin compresses 1-5% better, no codegen needed |
| Free-form text / files | Not suitable | No repeating schema to exploit |

---

## 4. Profile Management

### Profile size

Typical profile sizes:
- Simple object (5-10 fields): 200-500 bytes
- Complex object (20+ fields, nested): 1-3 KB
- Profile is fetched once, cached indefinitely until schema changes

### HTTP caching

```
GET /api/schema-profile/users
Cache-Control: public, max-age=86400
ETag: "profile-v2-users"
```

Client fetches profile once, caches it. On schema change, bump version/ETag.
