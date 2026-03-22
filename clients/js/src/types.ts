/**
 * TypeScript interfaces for Pidgin SchemaCodec profiles.
 *
 * A profile is a JSON descriptor shared between encoder (server) and decoder
 * (client).  Both sides know the schema so keys are never transmitted on the
 * wire — only values in deterministic field order.
 */

/** Supported data types in the Pidgin wire format. */
export type FieldDtype =
  | "bool"
  | "int"
  | "float"
  | "str"
  | "enum"
  | "timestamp"
  | "object"
  | "array";

/** Schema for a single field inside a profile. */
export interface FieldSchema {
  /** Data type tag. */
  dtype: FieldDtype;
  /** Whether the field can be null or absent. */
  nullable: boolean;
  /** Relative frequency of the field across sampled responses (0.0 - 1.0). */
  freq: number;
  /** For dtype "enum" — ordered list of known enum string values. */
  enum?: string[];
  /** For dtype "object" — nested field schemas (keyed by field name). */
  nested?: Record<string, FieldSchema>;
  /** For dtype "array" of objects — schema of array item fields. */
  items?: Record<string, FieldSchema>;
}

/** Top-level profile shared between server encoder and client decoder. */
export interface SchemaProfile {
  /** Profile version (bumped on schema evolution). */
  version: number;
  /** Deterministic ordered list of field names (most frequent first). */
  field_order: string[];
  /** Field schemas keyed by field name. */
  fields: Record<string, FieldSchema>;
}

/**
 * Configuration options for `PidginClient`.
 */
export interface PidginClientOptions {
  /** Base URL of the API server (e.g. "https://api.example.com"). */
  baseUrl: string;
  /**
   * Pre-loaded profiles keyed by an arbitrary alias.
   * The alias is passed to `client.get(path, alias)` to select the decoder.
   */
  profiles: Record<string, SchemaProfile>;
  /**
   * Optional custom `fetch` implementation (defaults to globalThis.fetch).
   * Useful for testing or Node.js versions without global fetch.
   */
  fetch?: typeof globalThis.fetch;
}
