/**
 * PidginClient — high-level fetch wrapper that negotiates Pidgin binary
 * encoding with the server and falls back to plain JSON transparently.
 *
 * Usage:
 *   const client = new PidginClient({
 *     baseUrl: "https://api.example.com",
 *     profiles: { users: profileJson },
 *   });
 *   const users = await client.get("/api/users", "users");
 *   // Sends `Accept: application/x-pidgin, application/json`
 *   // If server returns pidgin binary → decode automatically
 *   // If server returns JSON → parse as usual
 */

import type {
  SchemaProfile,
  PidginClientOptions,
} from "./types.js";
import { PidginDecoder, type BrotliDecompressFn } from "./decoder.js";

const PIDGIN_MIME = "application/x-pidgin";
const JSON_MIME = "application/json";
const ACCEPT_HEADER = `${PIDGIN_MIME}, ${JSON_MIME};q=0.9, */*;q=0.1`;

export class PidginClient {
  private readonly baseUrl: string;
  private readonly decoders: Map<string, PidginDecoder>;
  private readonly fetchFn: typeof globalThis.fetch;

  constructor(options: PidginClientOptions, brotli?: BrotliDecompressFn) {
    this.baseUrl = options.baseUrl.replace(/\/+$/, "");
    this.fetchFn = options.fetch ?? globalThis.fetch.bind(globalThis);

    this.decoders = new Map();
    for (const [alias, profile] of Object.entries(options.profiles)) {
      this.decoders.set(alias, new PidginDecoder(profile, brotli));
    }
  }

  /**
   * Register (or replace) a profile at runtime.
   * Useful when profiles are fetched lazily from the server.
   */
  setProfile(
    alias: string,
    profile: SchemaProfile,
    brotli?: BrotliDecompressFn,
  ): void {
    this.decoders.set(alias, new PidginDecoder(profile, brotli));
  }

  /**
   * GET a resource, automatically decoding Pidgin binary if the server
   * responds with the pidgin content-type.
   *
   * @param path         URL path (appended to baseUrl) or full URL.
   * @param profileAlias The alias of the pre-loaded profile to use for
   *                     decoding.  Ignored if the server responds with JSON.
   * @param init         Additional RequestInit options (headers, signal, etc.)
   * @returns Decoded payload — a single object or an array of objects.
   */
  async get<T = Record<string, unknown> | Record<string, unknown>[]>(
    path: string,
    profileAlias: string,
    init?: RequestInit,
  ): Promise<T> {
    const url = path.startsWith("http") ? path : `${this.baseUrl}${path}`;

    const headers = new Headers(init?.headers);
    if (!headers.has("Accept")) {
      headers.set("Accept", ACCEPT_HEADER);
    }

    const response = await this.fetchFn(url, {
      ...init,
      method: "GET",
      headers,
    });

    if (!response.ok) {
      throw new PidginFetchError(
        `HTTP ${response.status}: ${response.statusText}`,
        response,
      );
    }

    const contentType = response.headers.get("content-type") ?? "";

    if (contentType.includes(PIDGIN_MIME)) {
      const decoder = this.decoders.get(profileAlias);
      if (!decoder) {
        throw new Error(
          `PidginClient: no profile registered for alias "${profileAlias}"`,
        );
      }
      const buffer = await response.arrayBuffer();
      return (await decoder.decode(buffer)) as T;
    }

    // Fallback: parse as JSON
    return (await response.json()) as T;
  }

  /**
   * Generic request method for POST/PUT/PATCH etc.
   * Sends JSON body, decodes response as Pidgin or JSON.
   */
  async request<T = Record<string, unknown> | Record<string, unknown>[]>(
    method: string,
    path: string,
    profileAlias: string,
    body?: unknown,
    init?: RequestInit,
  ): Promise<T> {
    const url = path.startsWith("http") ? path : `${this.baseUrl}${path}`;

    const headers = new Headers(init?.headers);
    if (!headers.has("Accept")) {
      headers.set("Accept", ACCEPT_HEADER);
    }
    if (body !== undefined && !headers.has("Content-Type")) {
      headers.set("Content-Type", JSON_MIME);
    }

    const response = await this.fetchFn(url, {
      ...init,
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });

    if (!response.ok) {
      throw new PidginFetchError(
        `HTTP ${response.status}: ${response.statusText}`,
        response,
      );
    }

    const contentType = response.headers.get("content-type") ?? "";

    if (contentType.includes(PIDGIN_MIME)) {
      const decoder = this.decoders.get(profileAlias);
      if (!decoder) {
        throw new Error(
          `PidginClient: no profile registered for alias "${profileAlias}"`,
        );
      }
      const buffer = await response.arrayBuffer();
      return (await decoder.decode(buffer)) as T;
    }

    return (await response.json()) as T;
  }
}

/**
 * Error thrown when the server returns a non-2xx status.
 * Carries the original Response for inspection.
 */
export class PidginFetchError extends Error {
  readonly response: Response;

  constructor(message: string, response: Response) {
    super(message);
    this.name = "PidginFetchError";
    this.response = response;
  }
}
