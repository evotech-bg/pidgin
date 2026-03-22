/**
 * @pidgin/codec — JavaScript/TypeScript client decoder for the Pidgin
 * SchemaCodec binary wire format.
 *
 * Typical usage:
 *
 *   import { PidginDecoder, PidginClient } from "@pidgin/codec";
 *
 *   // Low-level: decode a raw binary payload
 *   const decoder = PidginDecoder.fromJSON(profileJsonString);
 *   const data = await decoder.decode(binaryPayload);
 *
 *   // High-level: fetch wrapper with automatic negotiation
 *   const client = new PidginClient({
 *     baseUrl: "https://api.example.com",
 *     profiles: { users: profileObject },
 *   });
 *   const users = await client.get("/api/users", "users");
 */

// Core decoder
export { PidginDecoder } from "./decoder.js";
export type { BrotliDecompressFn } from "./decoder.js";

// Fetch wrapper
export { PidginClient, PidginFetchError } from "./fetch.js";

// Varint utilities (exposed for advanced use / testing)
export { decodeZigZagVarint } from "./varint.js";
export type { Cursor } from "./varint.js";

// Types
export type {
  SchemaProfile,
  FieldSchema,
  FieldDtype,
  PidginClientOptions,
} from "./types.js";
