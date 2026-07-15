"""
5bit OAuth — Google + GitHub (stdlib only)
============================================
No OAuth SDK. Pure stdlib. Google JWKS verification. GitHub token exchange.

POST /api/auth/oauth
  { provider: "google", idToken: "eyJ..." }
  { provider: "github", code: "abc123" }
  → { session: { token, userId, expiresAt } }

Flow:
  Google: verify idToken signature against JWKS → extract sub/email → lookup or create user
  GitHub: exchange code for access_token → GET /user → extract id/email → lookup or create user
"""
import json, time, base64, hashlib, hmac, urllib.request, urllib.parse
from typing import Optional, Dict

GOOGLE_JWKS_URL = 'https://www.googleapis.com/oauth2/v3/certs'
GITHUB_TOKEN_URL = 'https://github.com/login/oauth/access_token'
GITHUB_USER_URL = 'https://api.github.com/user'

# ── Google OAuth ────────────────────────────────────────────────────

def _b64url_decode(data: str) -> bytes:
    data = data + '=' * (4 - len(data) % 4)
    return base64.urlsafe_b64decode(data)

def verify_google_token(id_token: str, client_id: str = '') -> Optional[Dict]:
    """Verify a Google id_token. ALWAYS fails closed — unknown kid, missing crypto, bad sig all REJECT."""
    # Try google-auth first (correct, fail-closed, maintained by Google)
    try:
        from google.oauth2 import id_token as gid
        from google.auth.transport import requests as greq
        info = gid.verify_oauth2_token(id_token, greq.Request(), client_id or None)
        return {
            'sub': info.get('sub', ''),
            'email': info.get('email', ''),
            'name': info.get('name', ''),
            'picture': info.get('picture', ''),
        }
    except ImportError:
        pass  # Fall through to stdlib verification
    except Exception:
        return None  # google-auth rejected the token — fail closed

    # ── Stdlib fallback: strict, fail-closed ──────────────────────────
    try:
        parts = id_token.split('.')
        if len(parts) != 3:
            return None

        header = json.loads(_b64url_decode(parts[0]))
        payload = json.loads(_b64url_decode(parts[1]))
        kid = header.get('kid', '')
        alg = header.get('alg', '')

        # 1. Pin algorithm — never trust the token's choice
        if alg != 'RS256':
            return None

        # 2. Verify expiration
        if payload.get('exp', 0) < time.time():
            return None

        # 3. Verify issuer
        if payload.get('iss') not in ('https://accounts.google.com', 'accounts.google.com'):
            return None

        # 4. Verify audience (if configured)
        if client_id and payload.get('aud') != client_id:
            return None

        # 5. Fetch JWKS — find matching key. Missing kid = REJECT.
        jwks = json.loads(urllib.request.urlopen(GOOGLE_JWKS_URL, timeout=10).read())
        key = None
        for k in jwks.get('keys', []):
            if k.get('kid') == kid:
                key = k
                break
        if not key:
            return None  # FAIL CLOSED — unknown kid

        # 6. RSA verify signature. No crypto lib = REJECT.
        try:
            from cryptography.hazmat.primitives.asymmetric import rsa, padding
            from cryptography.hazmat.primitives import hashes
            from cryptography.hazmat.backends import default_backend

            n = int.from_bytes(_b64url_decode(key['n']), 'big')
            e = int.from_bytes(_b64url_decode(key['e']), 'big')
            pubkey = rsa.RSAPublicNumbers(e, n).public_key(default_backend())

            signed = f"{parts[0]}.{parts[1]}".encode()
            sig = _b64url_decode(parts[2])
            pubkey.verify(sig, signed, padding.PKCS1v15(), hashes.SHA256())
        except ImportError:
            return None  # FAIL CLOSED — no crypto lib
        except Exception:
            return None  # FAIL CLOSED — bad signature

        return {
            'sub': payload.get('sub', ''),
            'email': payload.get('email', ''),
            'name': payload.get('name', ''),
            'picture': payload.get('picture', ''),
        }
    except Exception:
        return None
    except Exception:
        return None


# ── GitHub OAuth ──────────────────────────────────────────────────────

def exchange_github_code(code: str, client_id: str, client_secret: str) -> Optional[Dict]:
    """Exchange a GitHub OAuth code for user info. Returns { sub, email, name }."""
    try:
        # Exchange code for access token
        data = urllib.parse.urlencode({
            'client_id': client_id, 'client_secret': client_secret, 'code': code,
        }).encode()
        req = urllib.request.Request(GITHUB_TOKEN_URL, data=data, headers={'Accept': 'application/json'})
        resp = json.loads(urllib.request.urlopen(req, timeout=10).read())

        access_token = resp.get('access_token', '')
        if not access_token:
            return None

        # Get user info
        user_req = urllib.request.Request(GITHUB_USER_URL, headers={
            'Authorization': f'Bearer {access_token}',
            'Accept': 'application/json',
        })
        user = json.loads(urllib.request.urlopen(user_req, timeout=10).read())

        # Get email if not in profile
        email = user.get('email', '')
        if not email:
            email_req = urllib.request.Request('https://api.github.com/user/emails', headers={
                'Authorization': f'Bearer {access_token}',
                'Accept': 'application/json',
            })
            emails = json.loads(urllib.request.urlopen(email_req, timeout=10).read())
            primary = [e for e in emails if e.get('primary')]
            email = primary[0]['email'] if primary else ''

        return {
            'sub': f"github:{user['id']}",
            'name': user.get('login', ''),
            'email': email,
        }
    except Exception:
        return None


# ── OAuth User Management ──────────────────────────────────────────────

OAUTH_BASE = 80_200_000  # offset for OAuth user mappings

def find_or_create_oauth_user(auth_grid, provider: str, oauth_sub: str,
                                email: str, name: str, mode: str = 'managed') -> int:
    """Find existing OAuth user or create new one. Returns userId."""
    from binary_grid_db import Encoder, Token

    # Search for existing mapping: oauth_sub → userId
    sub_hash = hashlib.sha256(f"{provider}:{oauth_sub}".encode()).digest()
    rid = OAUTH_BASE + (int.from_bytes(sub_hash[:4], 'big') & 0xFFFFF)

    existing = auth_grid.base.read(rid)
    if existing and not existing.is_tombstone:
        nums = [p.value for p in existing.parsed if hasattr(p, 'value')]
        if nums:
            return nums[0]

    # Create new user
    uid = rid % 900_000 + 1  # simple uid allocation
    auth_grid.signup(uid, oauth_sub + email, mode)  # OAuth "password" = sub + email

    # Store mapping
    auth_grid.base.write(rid, [
        *Encoder.encode_integer(uid),
        *Encoder.encode_word(f"{provider}:{oauth_sub}"),
        Token.RECORD,
    ])

    # Store email separately (same offset as email/password auth)
    auth_grid.base.write(80_100_000 + uid, [
        *Encoder.encode_integer(uid),
        *Encoder.encode_word(email or f"{provider}:{oauth_sub}"),
        Token.RECORD,
    ])

    return uid
