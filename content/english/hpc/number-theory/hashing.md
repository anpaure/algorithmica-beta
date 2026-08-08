---
title: Hashing
weight: 8
---

Hash functions take an object — a short message, document, image, or basically any binary sequence — and transform it into a fixed-length sequence in a deterministic way.

This definition is intentionally broad. A hash used to select a bucket, a CRC used to detect a damaged packet, a password hash, and SHA-256 all compress data, but they solve different problems and require almost opposite properties.

## Collisions

If a hash accepts arbitrarily long inputs and returns only $n$ bits, collisions are unavoidable: there are more possible inputs than outputs. A collision is a pair $x\ne y$ such that

$$
H(x)=H(y).
$$

For an idealized $n$-bit hash, two independent random inputs collide with probability $2^{-n}$. This does **not** mean that we can safely store $2^n$ hashes. Among $q$ inputs there are $q(q-1)/2$ pairs, and the [birthday approximation](/hpc/stats/#birthday-problem) gives

$$
\Pr[\text{at least one collision}]
\approx
1-\exp\left(-\frac{q(q-1)}{2^{n+1}}\right).
$$

The probability becomes substantial around $q=2^{n/2}$. A 64-bit fingerprint therefore does not provide 64 bits of collision resistance; its acceptability depends on how many values are compared and whether a collision silently changes the answer or merely causes one more equality check.

Cryptographic hashes separate three related goals:

- *preimage resistance*: given $h$, finding any $x$ with $H(x)=h$ should require about $2^n$ classical work;
- *second-preimage resistance*: given $x$, finding a different $y$ with the same hash should require about $2^n$ classical work;
- *collision resistance*: finding any colliding pair should require about $2^{n/2}$ classical work because of the birthday attack.

These are computational claims, not consequences of having a large output. Cryptographic algorithms are public; security must not rely on an attacker being unable to learn which hash function is used.

## Non-Cryptographic Hashing

A hash table wants a function that is cheap and distributes the keys evenly across buckets. It usually does not need preimage resistance: after two keys land in the same bucket, the table compares the actual keys and remains correct.

The relevant properties are instead:

- low latency for short keys and high throughput for long ones;
- good avalanche, so every input bit affects the bucket-selection bits;
- behavior on the key distributions the application actually has;
- and, for attacker-controlled keys, a secret random seed that prevents precomputed collision floods.

The [hash-table case study](/hpc/data-structures/hash-tables/) develops a small integer mixer and then optimizes the table around its memory accesses. A more elaborate general-purpose hash is not automatically better: on four-byte keys, initialization and finalization can cost more than reading the key itself.

### Polynomial Hashing

For strings, it is often useful to preserve algebraic structure. A polynomial hash treats the bytes as coefficients:

$$
H(s_0s_1\ldots s_{k-1})
=
s_0b^{k-1}+s_1b^{k-2}+\ldots+s_{k-1}
\pmod m.
$$

Hashes of concatenated strings and substrings can then be combined using precomputed powers of $b$. This is useful in Rabin–Karp search and randomized string algorithms, but it is not cryptographic. If $b$ and $m$ are fixed and public, an adversary can construct collisions. Randomizing the parameters turns equality testing into a probabilistic algorithm, while additional independent moduli can reduce its failure probability under the assumed input model.

Using `uint64_t` and allowing unsigned overflow gives the convenient modulus $2^{64}$ for free, but the choice has algebraic weaknesses and is not a substitute for a designed hash function on hostile input.

### Checksums and Similarity

A checksum or [CRC](../error-correction/#detecting-errors) is optimized for accidental corruption. A good CRC can guarantee detection of specific burst-error patterns, but an attacker can modify the data and compute a matching CRC just as easily as the sender.

Locality-sensitive hashing has a different goal again: nearby objects should collide *more often* than distant ones. This is useful for approximate nearest-neighbor search, while an ordinary hash table tries to make similar keys look unrelated.

Memoization, deduplication, chess-position tables, and probabilistic equality tests all use non-cryptographic fingerprints. The important distinction is whether a collision only makes the program slower or can make it return a wrong result.

## Cryptographic Hash Functions

A cryptographic hash makes the output appear unrelated to the input while preserving determinism. When one input bit changes, each output bit should flip with probability close to one half, and no shortcut should be known for the preimage, second-preimage, or collision problems above.

SHA-256 and SHA-512 belong to the SHA-2 family standardized in [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final). SHA-3 uses a different sponge construction and is standardized separately. MD5 and SHA-1 still appear in old formats, but practical collision attacks exist; they should not be selected for new security designs. NIST is [transitioning away from SHA-1](https://csrc.nist.gov/News/2022/nist-transitioning-away-from-sha-1-for-all-apps).

Hashing is necessarily $\Omega(n)$ in the message length because every input byte must influence the digest. Implementations process fixed-size blocks and maintain a small state, which makes them suitable for streaming data. Hardware SHA instructions accelerate some standardized functions, while tree hashes split a large input into independent chunks so several cores or SIMD lanes can work in parallel.

The fastest choice depends on message size and batching. A function with excellent bulk throughput may have high setup latency for 20-byte keys, and processing eight independent messages may expose more instruction-level parallelism than one dependent stream.

### Encoding and Domain Separation

A hash commits to bytes, not to the objects we intended those bytes to represent. Naive concatenation is ambiguous:

$$
\text{"ab"}\mathbin\|\text{"c"}
=
\text{"a"}\mathbin\|\text{"bc"}.
$$

Structured inputs need an unambiguous encoding, such as length prefixes or a canonical serialization. Different uses of the same hash should also have different domain tags; otherwise, a digest produced for one protocol may be accepted in another context.

Truncating a digest is safe only after accounting for the changed bounds. Keeping $t$ bits gives at most $t$-bit preimage resistance and roughly $t/2$-bit collision resistance.

## Message Authentication

Publishing $H(m)$ beside a message detects accidental changes only if the digest itself arrives through an authenticated channel. An active attacker can replace both.

A *message authentication code* (MAC) uses a shared secret key. Only parties with the key can create or verify a valid tag. HMAC combines a block-oriented hash $H$ with a normalized key $K^{\prime}$ and two fixed pads:

$$
\operatorname{HMAC}_K(m)
=
H\left((K^{\prime}\mathbin\oplus\mathrm{opad})
\mathbin\|
H((K^{\prime}\mathbin\oplus\mathrm{ipad})\mathbin\|m)\right).
$$

The nested construction is deliberate. Simply computing `hash(secret || message)` is not a generic MAC and, for Merkle–Damgård hashes, can be vulnerable to length-extension attacks. Use HMAC, KMAC, or the authentication operation supplied by an AEAD scheme rather than inventing a keyed-hash layout.

A MAC is symmetric: every verifier can also forge messages. A [digital signature](../cryptography/#digital-signatures) separates those roles because only the private-key holder can sign while the public key suffices to verify.

## Password Hashing

Passwords have low entropy. If a database stores `SHA256(password)`, an attacker who steals it can evaluate a large dictionary offline and compare the results. Preimage resistance against random 256-bit inputs says little about this guessing attack.

Password storage therefore uses a deliberately expensive, memory-hard password hashing function such as [Argon2id](https://www.rfc-editor.org/rfc/rfc9106.html). Each password receives a unique random *salt*, stored openly beside the result. The salt prevents one precomputed table from attacking every account and ensures equal passwords do not have equal stored values.

The time, memory, and parallelism parameters are part of the stored record and should be tuned for the deployment. An optional server-side *pepper* is a secret kept separately from the password database, but it does not replace salts, rate limiting, multi-factor authentication, or breach detection.

## Signatures, Tokens, and Content Addressing

Signature schemes normally hash a structured message and sign an encoded digest. Hashing keeps the public-key operation fixed-size, but the signature security still depends on collision resistance, canonical encoding, domain separation, and the signature scheme itself.

A common signed JSON Web Token has three base64url-encoded parts: header, claims, and signature or MAC. The claims are normally **not encrypted**; anyone holding the token can decode them. The authentication tag prevents undetected modification when the verifier pins an allowed algorithm and validates the key, issuer, audience, expiration, and application-specific claims.

JWT is a container format, not a new cryptographic primitive. Implementations should follow the [JWT best current practices](https://www.rfc-editor.org/rfc/rfc8725.html) and use a library. Appending a fixed secret to the claims and hashing the result is not an acceptable substitute for a specified MAC or signature.

Hashes can also name immutable content. If an object is addressed by a cryptographic digest of its bytes, changing the object changes its name. A *Merkle tree* extends this idea to collections: leaves hash data blocks, internal nodes hash their children, and the root commits to the whole structure while allowing short inclusion proofs.

## Proof of Work

In hash-based proof of work, a participant varies a nonce until the block hash is numerically below a target. Each trial is cheap and independent, the expected number of trials is controlled by the target, and verification requires only one candidate evaluation.

The work is intentionally difficult to reuse for another purpose; this is what makes the cost verifiable and hard to fake. It does not by itself provide confidentiality, signatures, or consensus. A blockchain protocol combines it with signatures, hash-linked history, networking rules, and an agreement mechanism — and other systems replace proof of work with different resource or trust assumptions.

## Choosing the Primitive

The word "hash" is not enough to choose an algorithm:

- For a trusted-input hash table, use a fast non-cryptographic hash matched to the key type.
- For attacker-controlled table keys, use a robust keyed hash or randomized implementation.
- For accidental transmission errors, use a checksum or CRC designed for the error model.
- For adversarial integrity with a shared key, use a MAC or authenticated encryption.
- For signatures and content identifiers, use a current cryptographic hash with sufficient output length.
- For passwords, use a salted memory-hard password hashing function.
- For similarity search, use a locality-sensitive construction.

Replacing one category with another can be wasteful, but replacing a security primitive with a merely fast checksum can be catastrophic. Start with the threat model and the failure mode; only then optimize the implementation.

### Acknowledgements

The HMAC construction follows [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104.html), and the password-hashing guidance follows [RFC 9106](https://www.rfc-editor.org/rfc/rfc9106.html). The standardized SHA-3-derived functions, including KMAC and ParallelHash, are described in [NIST SP 800-185](https://csrc.nist.gov/pubs/sp/800/185/final).
