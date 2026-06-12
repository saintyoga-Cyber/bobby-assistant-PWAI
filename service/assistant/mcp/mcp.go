// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package mcp exposes the user's long-term memory (the same Redis store the
// watch assistant reads and writes) as a remote MCP server over Streamable
// HTTP. Add it to claude.ai (or any MCP client) as a custom connector to read
// and write the same memories from your phone or computer.
//
// This is a deliberately small, tools-only implementation of the MCP
// Streamable HTTP transport: a single POST endpoint speaking JSON-RPC 2.0,
// replying with a single JSON object per request. It is gated behind a bearer
// token (MCP_AUTH_TOKEN) and operates on one configured Rebble user ID
// (MCP_MEMORY_USER_ID).
package mcp

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/pebble-dev/bobby-assistant/service/assistant/config"
	"github.com/pebble-dev/bobby-assistant/service/assistant/memory"
)

const protocolVersion = "2025-06-18"

type Handler struct {
	redis *redis.Client
}

func NewHandler(r *redis.Client) *Handler {
	return &Handler{redis: r}
}

// Enabled reports whether the endpoint is configured. When false, the service
// should not register the route at all.
func Enabled() bool {
	return config.GetConfig().MCPAuthToken != ""
}

// --- JSON-RPC 2.0 envelopes ------------------------------------------------

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type rpcResponse struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Result  any             `json:"result,omitempty"`
	Error   *rpcError       `json:"error,omitempty"`
}

func (h *Handler) ServeHTTP(rw http.ResponseWriter, r *http.Request) {
	if !Enabled() {
		http.NotFound(rw, r)
		return
	}
	// Bearer-token auth.
	auth := r.Header.Get("Authorization")
	expected := "Bearer " + config.GetConfig().MCPAuthToken
	if auth != expected {
		rw.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(rw, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodPost {
		// GET would open a server->client SSE stream; this tools-only server
		// has nothing to push, so we don't support it.
		http.Error(rw, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req rpcRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeRPC(rw, rpcResponse{JSONRPC: "2.0", Error: &rpcError{Code: -32700, Message: "parse error"}})
		return
	}

	// Notifications (no id) get no response body.
	isNotification := len(req.ID) == 0

	resp := rpcResponse{JSONRPC: "2.0", ID: req.ID}
	switch req.Method {
	case "initialize":
		resp.Result = h.initialize(req.Params)
	case "notifications/initialized", "notifications/cancelled":
		rw.WriteHeader(http.StatusAccepted)
		return
	case "ping":
		resp.Result = map[string]any{}
	case "tools/list":
		resp.Result = h.toolsList()
	case "tools/call":
		result, rerr := h.toolsCall(r, req.Params)
		if rerr != nil {
			resp.Error = rerr
		} else {
			resp.Result = result
		}
	default:
		resp.Error = &rpcError{Code: -32601, Message: "method not found: " + req.Method}
	}

	if isNotification {
		rw.WriteHeader(http.StatusAccepted)
		return
	}
	writeRPC(rw, resp)
}

func writeRPC(rw http.ResponseWriter, resp rpcResponse) {
	rw.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(rw).Encode(resp); err != nil {
		log.Printf("mcp: failed to write response: %v", err)
	}
}

func (h *Handler) initialize(params json.RawMessage) map[string]any {
	version := protocolVersion
	var p struct {
		ProtocolVersion string `json:"protocolVersion"`
	}
	if err := json.Unmarshal(params, &p); err == nil && p.ProtocolVersion != "" {
		version = p.ProtocolVersion
	}
	return map[string]any{
		"protocolVersion": version,
		"capabilities": map[string]any{
			"tools": map[string]any{},
		},
		"serverInfo": map[string]any{
			"name":    "bobby-memory",
			"version": "1.0.0",
		},
		"instructions": "Tools to read and write the user's long-term memory, shared with their Pebble assistant. " +
			"Use list_memories to see what is already known, remember to store a durable fact, and forget to delete one by key.",
	}
}

func (h *Handler) toolsList() map[string]any {
	return map[string]any{
		"tools": []map[string]any{
			{
				"name":        "list_memories",
				"description": "List everything stored in the user's long-term memory (shared with their Pebble watch assistant). Returns key: text pairs.",
				"inputSchema": map[string]any{
					"type":       "object",
					"properties": map[string]any{},
				},
			},
			{
				"name":        "remember",
				"description": "Store a durable fact about the user in their long-term memory (shared with their Pebble watch assistant). Reusing a key overwrites that memory.",
				"inputSchema": map[string]any{
					"type": "object",
					"properties": map[string]any{
						"text": map[string]any{
							"type":        "string",
							"description": "The fact to remember, as a short standalone statement.",
						},
						"key": map[string]any{
							"type":        "string",
							"description": "A short kebab-case identifier, e.g. 'wife-name'. Omit to auto-generate.",
						},
					},
					"required": []string{"text"},
				},
			},
			{
				"name":        "forget",
				"description": "Delete a stored memory by its key.",
				"inputSchema": map[string]any{
					"type": "object",
					"properties": map[string]any{
						"key": map[string]any{
							"type":        "string",
							"description": "The key of the memory to delete.",
						},
					},
					"required": []string{"key"},
				},
			},
		},
	}
}

func (h *Handler) toolsCall(r *http.Request, params json.RawMessage) (map[string]any, *rpcError) {
	var call struct {
		Name      string          `json:"name"`
		Arguments json.RawMessage `json:"arguments"`
	}
	if err := json.Unmarshal(params, &call); err != nil {
		return nil, &rpcError{Code: -32602, Message: "invalid params"}
	}
	userId := config.GetConfig().MCPMemoryUserId
	ctx := r.Context()

	switch call.Name {
	case "list_memories":
		entries, err := memory.GetAll(ctx, h.redis, userId)
		if err != nil {
			return toolError("Failed to read memory: " + err.Error()), nil
		}
		if len(entries) == 0 {
			return toolText("No memories stored yet."), nil
		}
		var b strings.Builder
		for _, e := range entries {
			fmt.Fprintf(&b, "%s: %s\n", e.Key, e.Text)
		}
		return toolText(strings.TrimRight(b.String(), "\n")), nil

	case "remember":
		var args struct {
			Text string `json:"text"`
			Key  string `json:"key"`
		}
		if err := json.Unmarshal(call.Arguments, &args); err != nil {
			return toolError("Invalid arguments."), nil
		}
		if strings.TrimSpace(args.Text) == "" {
			return toolError("Nothing to remember: 'text' is empty."), nil
		}
		key := strings.TrimSpace(args.Key)
		if key == "" {
			key = fmt.Sprintf("note-%d", time.Now().Unix())
		}
		if err := memory.Remember(ctx, h.redis, userId, key, args.Text); err != nil {
			return toolError(err.Error()), nil
		}
		return toolText("Remembered under key '" + key + "'."), nil

	case "forget":
		var args struct {
			Key string `json:"key"`
		}
		if err := json.Unmarshal(call.Arguments, &args); err != nil {
			return toolError("Invalid arguments."), nil
		}
		existed, err := memory.Forget(ctx, h.redis, userId, strings.TrimSpace(args.Key))
		if err != nil {
			return toolError(err.Error()), nil
		}
		if !existed {
			return toolError("No memory with key '" + args.Key + "' exists."), nil
		}
		return toolText("Forgotten."), nil
	}
	return nil, &rpcError{Code: -32602, Message: "unknown tool: " + call.Name}
}

func toolText(text string) map[string]any {
	return map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
	}
}

func toolError(text string) map[string]any {
	return map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
		"isError": true,
	}
}
