/**
 * ocean eterna tool plugin for tinyclaw
 *
 * calls the OE REST API directly via fetch (bun-native, no deps).
 * register this file in your tinyclaw config to expose all 9 OE tools.
 *
 * usage:
 *   import { tools } from "./oe_tool.ts";
 *   // register with tinyclaw's tool system
 */

const OE_BASE = process.env.OE_BASE_URL ?? "http://localhost:9090";

interface ToolResult {
  content: string;
  error?: string;
}

async function post(path: string, body: Record<string, unknown>): Promise<Record<string, unknown>> {
  const res = await fetch(`${OE_BASE}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return res.json() as Promise<Record<string, unknown>>;
}

async function get(path: string, params?: Record<string, string | number>): Promise<Record<string, unknown>> {
  const url = new URL(`${OE_BASE}${path}`);
  if (params) {
    for (const [k, v] of Object.entries(params)) {
      url.searchParams.set(k, String(v));
    }
  }
  const res = await fetch(url.toString());
  return res.json() as Promise<Record<string, unknown>>;
}

// -- tool definitions --

export const tools = [
  {
    name: "oe_search",
    description: "Search the user's personal knowledge base using BM25. Returns answer + source chunks.",
    parameters: {
      type: "object",
      properties: {
        query: { type: "string", description: "Natural language search query" },
        conversation_id: { type: "string", description: "Optional conversation ID for multi-turn context" },
      },
      required: ["query"],
    },
    async execute(args: { query: string; conversation_id?: string }): Promise<ToolResult> {
      const payload: Record<string, string> = { question: args.query };
      if (args.conversation_id) payload.conversation_id = args.conversation_id;
      const data = await post("/chat", payload);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_get_chunk",
    description: "Retrieve a specific chunk by ID with optional adjacent chunks.",
    parameters: {
      type: "object",
      properties: {
        chunk_id: { type: "string", description: "The chunk identifier" },
        context_window: { type: "number", description: "Adjacent chunks to include (0-3)", default: 1 },
      },
      required: ["chunk_id"],
    },
    async execute(args: { chunk_id: string; context_window?: number }): Promise<ToolResult> {
      const data = await get(`/chunk/${args.chunk_id}`, { context_window: args.context_window ?? 1 });
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_add_file",
    description: "Ingest text content into the knowledge base.",
    parameters: {
      type: "object",
      properties: {
        filename: { type: "string", description: "Name for the document" },
        content: { type: "string", description: "Full text content to index" },
      },
      required: ["filename", "content"],
    },
    async execute(args: { filename: string; content: string }): Promise<ToolResult> {
      const data = await post("/add-file", args);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_add_file_path",
    description: "Ingest a file from the local filesystem into the knowledge base.",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute or relative path to the file" },
      },
      required: ["path"],
    },
    async execute(args: { path: string }): Promise<ToolResult> {
      const data = await post("/add-file-path", args);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_add_document",
    description: "Ingest a document in any supported format (PDF, DOCX, XLSX, CSV, image).",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute path to the document file" },
      },
      required: ["path"],
    },
    async execute(args: { path: string }): Promise<ToolResult> {
      // uses add-file-path endpoint; server handles format detection
      const data = await post("/add-file-path", args);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_stats",
    description: "Get Ocean Eterna server statistics — corpus size, chunk count, health.",
    parameters: { type: "object", properties: {} },
    async execute(): Promise<ToolResult> {
      const [health, stats] = await Promise.all([get("/health"), get("/stats")]);
      return { content: JSON.stringify({ health, stats }, null, 2) };
    },
  },

  {
    name: "oe_catalog",
    description: "Browse the indexed knowledge base catalog.",
    parameters: {
      type: "object",
      properties: {
        page: { type: "number", description: "Page number", default: 1 },
        page_size: { type: "number", description: "Results per page (max 100)", default: 20 },
        type_filter: { type: "string", description: "Filter by chunk type" },
        source: { type: "string", description: "Filter by source filename" },
      },
    },
    async execute(args: { page?: number; page_size?: number; type_filter?: string; source?: string }): Promise<ToolResult> {
      const params: Record<string, string | number> = {
        page: args.page ?? 1,
        page_size: Math.min(args.page_size ?? 20, 100),
      };
      if (args.type_filter) params.type = args.type_filter;
      if (args.source) params.source = args.source;
      const data = await get("/catalog", params);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_tell_me_more",
    description: "Expand context for a previous search result using its turn_id.",
    parameters: {
      type: "object",
      properties: {
        turn_id: { type: "string", description: "The turn_id from a previous oe_search result" },
      },
      required: ["turn_id"],
    },
    async execute(args: { turn_id: string }): Promise<ToolResult> {
      const data = await post("/tell-me-more", args);
      return { content: JSON.stringify(data, null, 2) };
    },
  },

  {
    name: "oe_reconstruct",
    description: "Reconstruct a document from a list of chunk IDs.",
    parameters: {
      type: "object",
      properties: {
        chunk_ids: {
          type: "array",
          items: { type: "string" },
          description: "List of chunk IDs to combine in order",
        },
      },
      required: ["chunk_ids"],
    },
    async execute(args: { chunk_ids: string[] }): Promise<ToolResult> {
      const data = await post("/reconstruct", args);
      return { content: JSON.stringify(data, null, 2) };
    },
  },
] as const;

// -- tinyclaw plugin export --
export default {
  name: "ocean-eterna",
  description: "Ocean Eterna BM25 search engine — 9ms search across 1B+ tokens",
  version: "1.0.0",
  tools,
};
