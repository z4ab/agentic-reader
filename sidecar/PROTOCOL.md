# Sidecar protocol

The app talks to the sidecar over the sidecar's stdin and stdout. `reader_sidecar/protocol.py` defines the same messages, and `tests/test_protocol_doc.py` checks that this file matches it.

## Framing

1. Each message is one JSON object on one line, ended by `\n`.
2. Every message has a string field `type` that names it. The tables below list the other fields.
3. The app writes UTF-8 to stdin. A `\r` before the `\n` is accepted.
4. The sidecar writes ASCII to stdout. Strings escape control characters and non-ASCII characters (`\n`, `\u00e9`), so a line never contains a raw line break.
5. The sidecar writes only protocol messages to stdout. Its logs go to stderr.
6. Key order has no meaning.

## Field rules

1. App to sidecar: a field whose type includes `null` is optional. The app may omit it or send `null`. Every other field is required. The sidecar ignores unknown keys.
2. Sidecar to app: every listed field is always present. A field whose type includes `null` may be `null`.
3. Every app message may carry an integer `seq`. An `error` caused by that message echoes the same `seq`.
4. The sidecar answers an invalid line with an `error` of kind `protocol`. A line is invalid when it is not JSON, is not an object, has no `type`, has a `type` that is not an app message, lacks a required field, or has a `context` that is not an object. The error echoes the line's `seq` when the line is an object with an integer `seq`.

## Messages from the app

### `open_project`

Sets the project folder. Claude runs with this folder as its working directory.

| field | type | meaning |
|---|---|---|
| `path` | string | Absolute path of the project folder. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"open_project","path":"/home/u/MyProject","seq":1}
```

### `prompt`

Sends a user message to a chat. The reply streams back as `text_delta` and `tool_use` messages and ends with `result` or `error`.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat from `new_chat` or `resume_chat`. |
| `text` | string | The user's message. |
| `context` | object or null | What the user is looking at. See the context object below. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"prompt","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","text":"Explain this paragraph.","context":{"file":"papers/attention.pdf","page":12,"page_count":15,"selection":"Scaled dot-product attention"},"seq":4}
```

### Context object

The `context` field of `prompt`. Every field is optional.

| field | type | meaning |
|---|---|---|
| `file` | string or null | Path of the open PDF. |
| `page` | integer or null | Current page, 1-based. |
| `page_count` | integer or null | Number of pages in the file. |
| `selection` | string or null | Selected text. |

### `new_chat`

Starts a chat. The sidecar answers with `chat_started` whose `session_id` is `null`.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | UUID chosen by the app. Later messages for this chat use it. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"new_chat","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","seq":2}
```

### `resume_chat`

Reopens a saved chat. The sidecar answers with `chat_started` carrying the same `session_id`.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | UUID chosen by the app. Later messages for this chat use it. |
| `session_id` | string | `session_id` from an earlier `result` of this chat. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"resume_chat","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":"b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b","seq":3}
```

### `interrupt`

Stops the reply running in a chat.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat to stop. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"interrupt","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","seq":5}
```

### `permission_reply`

Answers a `permission_request`.

| field | type | meaning |
|---|---|---|
| `req_id` | string | `req_id` from the request. |
| `allow` | boolean | `true` runs the tool. `false` denies it. |
| `seq` | integer or null | Echoed in errors. |

```json
{"type":"permission_reply","req_id":"9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4","allow":true,"seq":6}
```

## Messages from the sidecar

### `ready`

The first message the sidecar writes after it starts.

| field | type | meaning |
|---|---|---|
| `version` | string | Sidecar version. |

```json
{"type":"ready","version":"0.1.0"}
```

### `chat_started`

Answers `new_chat` and `resume_chat`.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat from the request. |
| `session_id` | string or null | The resumed session, or `null` for a new chat. A new chat's session ID arrives in its first `result`. |

```json
{"type":"chat_started","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":null}
```

### `text_delta`

A piece of streamed reply text. Append the pieces in order.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat the reply belongs to. |
| `text` | string | Next piece of the reply. |

```json
{"type":"text_delta","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","text":"Scaled dot-product attention divides by \u221ad_k (p. 4).\n"}
```

### `tool_use`

Claude called a tool.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat the reply belongs to. |
| `name` | string | Tool name, such as `Read`. |
| `input` | object | Tool arguments. |

```json
{"type":"tool_use","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","name":"Read","input":{"file_path":"papers/attention.pdf","pages":"12"}}
```

### `permission_request`

Claude wants to run a tool that needs approval. The app answers with `permission_reply` carrying the same `req_id`.

| field | type | meaning |
|---|---|---|
| `req_id` | string | Request ID. |
| `chat_id` | string | Chat the request belongs to. |
| `tool` | string | Tool name. |
| `input` | object | Tool arguments. |

```json
{"type":"permission_request","req_id":"9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","tool":"Bash","input":{"command":"ls"}}
```

### `result`

Ends a reply.

| field | type | meaning |
|---|---|---|
| `chat_id` | string | Chat the reply belongs to. |
| `session_id` | string | Session ID. Pass it to `resume_chat` to reopen the chat later. |
| `usage` | object | Token counts reported by the Agent SDK. |
| `cost_usd` | number | Cost of the reply in US dollars. |

```json
{"type":"result","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":"b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b","usage":{"input_tokens":1200,"output_tokens":340},"cost_usd":0.0123}
```

### `error`

Reports a failure.

| field | type | meaning |
|---|---|---|
| `chat_id` | string or null | Chat the error belongs to, or `null`. |
| `kind` | string | Machine-readable cause, such as `protocol` or `busy`. Treat an unknown kind as a generic failure. |
| `message` | string | Human-readable detail. |
| `seq` | integer or null | `seq` of the app message that caused the error, or `null`. |

```json
{"type":"error","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","kind":"busy","message":"chat is busy","seq":4}
```
