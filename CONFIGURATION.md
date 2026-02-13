# OceanEterna v4 Configuration Reference

Configuration priority: **Environment Variables > config.json > Compiled Defaults**

## config.json

```json
{
  "server": {
    "port": 8888,
    "host": "0.0.0.0"
  },
  "llm": {
    "use_external": true,
    "external_url": "https://routellm.abacus.ai/v1/chat/completions",
    "external_model": "gpt-5-mini",
    "local_url": "http://127.0.0.1:1234/v1/chat/completions",
    "local_model": "qwen/qwen3-32b",
    "timeout_sec": 30,
    "max_retries": 3,
    "retry_backoff_ms": 1000
  },
  "search": {
    "top_k": 8,
    "bm25_k1": 1.5,
    "bm25_b": 0.75
  },
  "corpus": {
    "manifest": "guten_9m_build/manifest_guten9m.jsonl",
    "storage": "guten_9m_build/storage/guten9m.bin",
    "chapter_guide": "guten_9m_build/chapter_guide.json"
  },
  "auth": {
    "enabled": false,
    "api_key": ""
  },
  "rate_limit": {
    "enabled": false,
    "requests_per_minute": 60
  }
}
```

## Environment Variables

| Variable | Overrides | Description |
|----------|-----------|-------------|
| `OCEAN_API_KEY` | `llm.api_key` | API key for external LLM service |
| `OCEAN_API_URL` | `llm.external_url` | External LLM API endpoint |
| `OCEAN_MODEL` | `llm.external_model` | External LLM model name |
| `OCEAN_SERVER_API_KEY` | `auth.api_key` | Server authentication key |

## Section Details

### server
| Key | Default | Description |
|-----|---------|-------------|
| `port` | 8888 | HTTP server port |
| `host` | "0.0.0.0" | Bind address |

### llm
| Key | Default | Description |
|-----|---------|-------------|
| `use_external` | true | Use external API (false = local LM Studio) |
| `external_url` | routellm.abacus.ai | External API endpoint |
| `external_model` | "gpt-5-mini" | External model name |
| `local_url` | localhost:1234 | Local LM Studio endpoint |
| `local_model` | "qwen/qwen3-32b" | Local model name |
| `timeout_sec` | 30 | HTTP timeout for LLM calls |
| `max_retries` | 3 | Retry count on transient failures |
| `retry_backoff_ms` | 1000 | Initial backoff (doubles each retry) |

### search
| Key | Default | Description |
|-----|---------|-------------|
| `top_k` | 8 | Number of chunks to retrieve per query |
| `bm25_k1` | 1.5 | BM25 term frequency saturation |
| `bm25_b` | 0.75 | BM25 document length normalization |

### corpus
| Key | Default | Description |
|-----|---------|-------------|
| `manifest` | guten_9m_build/manifest_guten9m.jsonl | JSONL manifest path |
| `storage` | guten_9m_build/storage/guten9m.bin | Binary storage path |
| `chapter_guide` | guten_9m_build/chapter_guide.json | Chapter guide path |

### auth
| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | false | Enable X-API-Key authentication |
| `api_key` | "" | Required key value (or set OCEAN_SERVER_API_KEY) |

### rate_limit
| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | false | Enable per-IP rate limiting |
| `requests_per_minute` | 60 | Max requests per IP per minute |

## Compile Command

```bash
g++ -O3 -std=c++17 -march=native -fopenmp \
    -o ocean_chat_server_v4 ocean_chat_server.cpp \
    -llz4 -lcurl -lpthread -lzstd
```

## Quick Start

```bash
export OCEAN_API_KEY="your-api-key-here"
cd chat/
./ocean_chat_server_v4
# Server starts on http://localhost:8888
```
